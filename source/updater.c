#define _GNU_SOURCE
#include "updater.h"
#include "storage.h"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>
#include <unistd.h>

#ifndef NXLOAD_BUILD_SHA
#define NXLOAD_BUILD_SHA "unknown"
#endif

#define ACTIONS "https://api.github.com/repos/Small182/NXLoad/actions/"
#define TOKEN_PATH NXLOAD_APP_DIR "/github-token.txt"
#define JSON_LIMIT (256 * 1024)
#define ZIP_LIMIT (96 * UINT64_C(1024) * 1024)

typedef struct {
    FichierJob *job;
    FILE *out;
    char *json;
    size_t size;
    Sha256Context sha;
} Response;

typedef struct {
    const char *begin, *end;
} Json;

static const char *whitespace(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) ++p;
    return p;
}

static const char *value_end(const char *p, const char *end, unsigned depth) {
    p = whitespace(p, end);
    if (p == end || depth > 50) return NULL;
    if (*p == '"') {
        while (++p < end) {
            if (*p == '"') return p + 1;
            if (*p == '\\' && ++p == end) break;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p++ == '{' ? '}' : ']';
        p = whitespace(p, end);
        while (p < end && *p != close) {
            p = value_end(p, end, depth + 1);
            if (!p) return NULL;
            p = whitespace(p, end);
            if (p < end && (*p == ':' || *p == ',')) {
                p = whitespace(p + 1, end);
                if (p == end || *p == close) return NULL;
            } else if (p == end || *p != close) return NULL;
        }
        return p < end ? p + 1 : NULL;
    }
    const char *start = p;
    while (p < end && !strchr(",]}:", *p) && !isspace((unsigned char)*p)) ++p;
    return p == start ? NULL : p;
}

static bool literal(Json part, const char *value) {
    return part.begin && (size_t)(part.end - part.begin) == strlen(value) &&
           !memcmp(part.begin, value, strlen(value));
}

static Json field(Json object, const char *key) {
    if (!object.begin || *object.begin != '{') return (Json){0};
    const char *p = whitespace(object.begin + 1, object.end);
    while (p < object.end && *p == '"') {
        const char *last = value_end(p, object.end, 0);
        if (!last) break;
        Json name = {p + 1, last - 1};
        p = whitespace(last, object.end);
        if (p == object.end || *p != ':') break;
        p = whitespace(p + 1, object.end);
        last = value_end(p, object.end, 0);
        if (!last) break;
        if (literal(name, key)) return (Json){p, last};
        p = whitespace(last, object.end);
        if (p == object.end || *p != ',') break;
        p = whitespace(p + 1, object.end);
    }
    return (Json){0};
}

static bool string_field(Json object, const char *key, char *out, size_t capacity) {
    Json value = field(object, key);
    if (!value.begin || *value.begin != '"' || (size_t)(value.end - value.begin) < 2)
        return false;
    size_t len = (size_t)(value.end - value.begin) - 2;
    if (len >= capacity || memchr(value.begin + 1, '\\', len)) return false;
    memcpy(out, value.begin + 1, len);
    out[len] = 0;
    return true;
}

static bool id_field(Json object, char out[24]) {
    Json id = field(object, "id");
    if (!id.begin) return false;
    size_t len = (size_t)(id.end - id.begin);
    if (!len || len >= 24 || strspn(id.begin, "0123456789") != len) return false;
    memcpy(out, id.begin, len);
    out[len] = 0;
    return true;
}

static Json next_object(Json array, const char **cursor) {
    if (!array.begin || *array.begin != '[') return (Json){0};
    const char *p = whitespace(*cursor ? *cursor : array.begin + 1, array.end);
    if (p < array.end && *p == ',') p = whitespace(p + 1, array.end);
    if (p == array.end || *p != '{') return (Json){0};
    const char *end = value_end(p, array.end, 0);
    if (!end) return (Json){0};
    *cursor = end;
    return (Json){p, end};
}

static size_t receive(char *data, size_t size, size_t count, void *arg) {
    Response *r = arg;
    size_t bytes = size * count;
    size_t limit = r->out ? ZIP_LIMIT : JSON_LIMIT;
    if (r->size > limit || bytes > limit - r->size) return 0;
    if (r->out) {
        if (fwrite(data, 1, bytes, r->out) != bytes) return 0;
        sha256ContextUpdate(&r->sha, data, bytes);
        atomic_fetch_add(&r->job->received, bytes);
    } else {
        memcpy(r->json + r->size, data, bytes);
        r->json[r->size + bytes] = 0;
    }
    r->size += bytes;
    return bytes;
}

static int progress(void *arg, curl_off_t total, curl_off_t current,
                    curl_off_t upload, curl_off_t uploaded) {
    (void)current;
    (void)upload;
    (void)uploaded;
    Response *r = arg;
    if (r->out && total > 0) atomic_store(&r->job->total, (uint64_t)total);
    return atomic_load(&r->job->cancelled);
}

static bool request(const char *url, const char *token, Response *r, long *status) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    char bearer[560];
    int n = snprintf(bearer, sizeof(bearer), "Authorization: Bearer %s", token);
    struct curl_slist *with_token = n > 0 && (size_t)n < sizeof(bearer) && headers ?
                                   curl_slist_append(headers, bearer) : NULL;
    if (!with_token) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return false;
    }
    r->size = 0;
    if (r->json) r->json[0] = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, with_token);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NXLoad");
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, r->out ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, r->out ? 240L : 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, r);
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(with_token);
    return result == CURLE_OK && *status == 200;
}

static bool read_token(char token[512]) {
    FILE *file = fopen(TOKEN_PATH, "rb");
    if (!file) return false;
    size_t n = fread(token, 1, 511, file);
    bool ok = !ferror(file) && feof(file);
    fclose(file);
    if (!ok) return false;
    while (n && isspace((unsigned char)token[n - 1])) --n;
    token[n] = 0;
    return n && strspn(token,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == n;
}

bool update_configured(void) {
    return !access(TOKEN_PATH, R_OK);
}

static bool zip_digest(Response *r, const char *expected) {
    const char hex[] = "0123456789abcdef";
    unsigned char bytes[SHA256_HASH_SIZE];
    char hash[SHA256_HASH_SIZE * 2 + 1];
    sha256ContextGetHash(&r->sha, bytes);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        hash[2 * i] = hex[bytes[i] >> 4];
        hash[2 * i + 1] = hex[bytes[i] & 15];
    }
    hash[sizeof(hash) - 1] = 0;
    return !strncmp(expected, "sha256:", 7) && !strcasecmp(hash, expected + 7);
}

static bool extract_nro(FichierJob *job, const char *zip, const char *stage) {
    struct archive *a = archive_read_new();
    if (!a) return false;
    struct archive_entry *e;
    bool ok = false;
    if (archive_read_support_format_zip(a) == ARCHIVE_OK &&
        archive_read_open_filename(a, zip, 32 * 1024) == ARCHIVE_OK &&
        archive_read_next_header(a, &e) == ARCHIVE_OK &&
        archive_entry_pathname(e) && !strcmp(archive_entry_pathname(e), "NXLoad.nro") &&
        archive_entry_filetype(e) == AE_IFREG && archive_entry_size(e) >= 128 &&
        (uint64_t)archive_entry_size(e) <= ZIP_LIMIT) {
        FILE *out = fopen(stage, "wbx");
        if (out) {
            unsigned char bytes[32 * 1024];
            la_ssize_t n = archive_read_data(a, bytes, sizeof(bytes));
            ok = n >= 128 && !memcmp(bytes + 16, "NRO0", 4);
            uint64_t written = 0;
            while (ok && n > 0 && !atomic_load(&job->cancelled)) {
                ok = (uint64_t)n <= ZIP_LIMIT - written &&
                     fwrite(bytes, 1, (size_t)n, out) == (size_t)n;
                written += (uint64_t)n;
                if (ok) n = archive_read_data(a, bytes, sizeof(bytes));
            }
            bool closed = fclose(out) == 0;
            ok = ok && n == 0 && closed && written == (uint64_t)archive_entry_size(e) &&
                 archive_read_next_header(a, &e) == ARCHIVE_EOF && !atomic_load(&job->cancelled);
            if (!ok) unlink(stage);
        }
    }
    archive_read_free(a);
    return ok;
}

static const char *http_error(long status) {
    if (status == 401 || status == 403 || status == 404)
        return "GitHub відхилив доступ. Перевір github-token.txt і дозвіл Actions: Read.";
    return "Не вдалося зв’язатися з GitHub Actions";
}

const char *stage_update(FichierJob *job, const char *target) {
    char token[512], stage[320], zip[] = NXLOAD_APP_DIR "/.update-XXXXXX";
    if (!read_token(token)) return "Додай токен GitHub у switch/nxload/github-token.txt";
    if (snprintf(stage, sizeof(stage), "%s.new", target) >= (int)sizeof(stage))
        return "Шлях до NXLoad.nro задовгий";

    Response r = {.job = job, .json = malloc(JSON_LIMIT + 1)};
    if (!r.json) return "Бракує пам’яті для оновлення";
    const char *update_error = "GitHub не повернув артефакт NXLoad з успішної збірки";
    long status = 0;
    char url[256], build[24], sha[41], id[24], digest[72];
    bool zip_created = false;
    if (!request(ACTIONS "workflows/build.yml/runs?branch=native-1fichier&event=push&status=success&per_page=1",
                 token, &r, &status)) {
        update_error = http_error(status);
        goto done;
    }
    const char *cursor = NULL;
    Json body = {r.json, r.json + r.size};
    Json run = next_object(field(body, "workflow_runs"), &cursor);
    if (!id_field(run, build) || !string_field(run, "head_sha", sha, sizeof(sha)) ||
        strlen(sha) != 40 || strspn(sha, "0123456789abcdef") != 40) goto done;
    if (!strcmp(sha, NXLOAD_BUILD_SHA)) {
        update_error = "Вже встановлено останню успішну збірку NXLoad";
        goto done;
    }

    snprintf(url, sizeof(url), ACTIONS "runs/%s/artifacts?per_page=30", build);
    if (!request(url, token, &r, &status)) { update_error = http_error(status); goto done; }
    cursor = NULL;
    body = (Json){r.json, r.json + r.size};
    Json artifacts = field(body, "artifacts");
    bool found = false;
    for (Json item = next_object(artifacts, &cursor); item.begin; item = next_object(artifacts, &cursor)) {
        if (literal(field(item, "name"), "\"NXLoad\"") && literal(field(item, "expired"), "false")) {
            found = id_field(item, id) && string_field(item, "digest", digest, sizeof(digest));
            break;
        }
    }
    if (!found) goto done;

    int fd = mkstemp(zip);
    if (fd < 0) { update_error = "Не вдалося створити файл оновлення на microSD"; goto done; }
    zip_created = true;
    r.out = fdopen(fd, "wb");
    if (!r.out) { close(fd); update_error = "Не вдалося записати файл оновлення"; goto done; }
    sha256ContextCreate(&r.sha);
    snprintf(url, sizeof(url), ACTIONS "artifacts/%s/zip", id);
    bool received = request(url, token, &r, &status);
    bool saved = fclose(r.out) == 0;
    if (!received || !saved) { update_error = http_error(status); goto done; }
    if (!zip_digest(&r, digest)) { update_error = "Контрольна сума збірки не збігається"; goto done; }
    if (!access(stage, F_OK) && unlink(stage)) {
        update_error = "Не вдалося прибрати попередній тимчасовий файл оновлення";
        goto done;
    }
    if (!extract_nro(job, zip, stage)) {
        update_error = "Не вдалося підготувати NXLoad.nro. Перевір місце та відсутність файлу .new.";
        goto done;
    }
    update_error = NULL;

done:
    if (zip_created) unlink(zip);
    free(r.json);
    if (atomic_load(&job->cancelled)) {
        if (!update_error) unlink(stage);
        return "Оновлення скасовано";
    }
    return update_error;
}

bool apply_update(const char *target) {
    char stage[320], backup[320];
    if (snprintf(stage, sizeof(stage), "%s.new", target) >= (int)sizeof(stage) ||
        snprintf(backup, sizeof(backup), "%s.bak", target) >= (int)sizeof(backup)) return false;
    if (access(target, R_OK) || access(stage, R_OK)) return false;
    if (!access(backup, F_OK) && unlink(backup)) return false;
    if (rename(target, backup)) return false;
    if (!rename(stage, target)) return true;
    rename(backup, target);
    return false;
}
