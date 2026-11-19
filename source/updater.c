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

#include "language.h"

#ifndef NXLOAD_BUILD_SHA
#define NXLOAD_BUILD_SHA "unknown"
#endif

#ifndef NXLOAD_BUILD_RUN
#define NXLOAD_BUILD_RUN ""
#endif

#define UPDATE_BRANCH "native-1fichier"
#define BUILD_WORKFLOW ".github/workflows/build.yml"
#define ACTIONS "https://api.github.com/repos/Small182/NXLoad/actions/"
#define TOKEN_PATH NXLOAD_APP_DIR "/github-token.txt"
#define JSON_LIMIT (512 * 1024)
#define ZIP_LIMIT (96 * UINT64_C(1024) * 1024)

typedef struct {
    FichierJob *job;
    atomic_bool *cancelled;
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
    return atomic_load(r->cancelled);
}

static bool request(const char *url, const char *token, Response *r, long *status) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    if (headers && !r->out) {
        struct curl_slist *fresh = curl_slist_append(headers, "Cache-Control: no-cache");
        if (!fresh) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return false;
        }
        headers = fresh;
    }
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

static uint64_t build_number(Json run) {
    Json value = field(run, "run_number");
    if (!value.begin || value.end - value.begin > 18 ||
        strspn(value.begin, "0123456789") != (size_t)(value.end - value.begin)) return 0;
    return strtoull(value.begin, NULL, 10);
}

static bool read_run(Json run, UpdateBuild *build, char id[24]) {
    build->run = build_number(run);
    return build->run && literal(field(run, "event"), "\"push\"") &&
           literal(field(run, "head_branch"), "\"" UPDATE_BRANCH "\"") &&
           literal(field(run, "path"), "\"" BUILD_WORKFLOW "\"") &&
           literal(field(run, "conclusion"), "\"success\"") &&
           id_field(run, id) && string_field(run, "head_sha", build->sha, sizeof(build->sha)) &&
           strlen(build->sha) == 40 && strspn(build->sha, "0123456789abcdef") == 40;
}

static bool newest_run(Response *r, UpdateBuild *latest, char run_id[24]) {
    Json body = {r->json, r->json + r->size};
    Json runs = field(body, "workflow_runs");
    const char *cursor = NULL;
    for (Json run = next_object(runs, &cursor); run.begin; run = next_object(runs, &cursor)) {
        UpdateBuild candidate = {0};
        char id[24];
        if (read_run(run, &candidate, id) && candidate.run > latest->run) {
            *latest = candidate;
            strcpy(run_id, id);
        }
    }
    return latest->run != 0;
}

static bool read_artifact(Response *r, UpdateBuild *build) {
    Json artifacts = field((Json){r->json, r->json + r->size}, "artifacts");
    const char *cursor = NULL;
    for (Json item = next_object(artifacts, &cursor); item.begin; item = next_object(artifacts, &cursor)) {
        if (literal(field(item, "name"), "\"NXLoad\"") && literal(field(item, "expired"), "false") &&
            id_field(item, build->id) && string_field(item, "digest", build->digest, sizeof(build->digest)) &&
            strlen(build->digest) == 71 && !strncmp(build->digest, "sha256:", 7) &&
            strspn(build->digest + 7, "0123456789abcdef") == 64) return true;
    }
    return false;
}

void check_update(UpdateCheck *check) {
    static const char no_build[] = "GitHub ще не повернув файл останньої збірки NXLoad";
    char token[512], run_id[24], url[320];
    long status = 0;
    UpdateBuild candidate = {0};
    check->build = (UpdateBuild){0};
    check->error = NULL;
    if (!read_token(token)) {
        check->error = "Додай токен GitHub у switch/nxload/github-token.txt";
        return;
    }
    Response r = {.cancelled = &check->cancelled, .json = malloc(JSON_LIMIT + 1)};
    if (!r.json) {
        check->error = "Бракує пам’яті для перевірки оновлення";
        return;
    }
    const char *runs = ACTIONS "runs?branch=" UPDATE_BRANCH "&event=push&status=success&per_page=10";
    if (!request(runs, token, &r, &status)) {
        check->error = http_error(status);
        goto done;
    }
    if (!newest_run(&r, &candidate, run_id)) {
        check->error = no_build;
        goto done;
    }
    if (!strcmp(candidate.sha, NXLOAD_BUILD_SHA)) goto done;
    uint64_t installed = strtoull(NXLOAD_BUILD_RUN, NULL, 10);
    if (!installed) {
        // A build without the CI run number can still identify itself by its commit.
        if (strlen(NXLOAD_BUILD_SHA) == 40 && strspn(NXLOAD_BUILD_SHA, "0123456789abcdef") == 40) {
            snprintf(url, sizeof(url), "%s&head_sha=%s", runs, NXLOAD_BUILD_SHA);
            UpdateBuild current = {0};
            char current_id[24];
            if (request(url, token, &r, &status) && newest_run(&r, &current, current_id)
                && !strcmp(current.sha, NXLOAD_BUILD_SHA)) installed = current.run;
        }
        if (!installed) {
            check->error = "Номер цієї збірки невідомий. Встанови актуальний NXLoad.nro вручну.";
            goto done;
        }
    }
    if (candidate.run <= installed) goto done;
    snprintf(url, sizeof(url), ACTIONS "runs/%s/artifacts?per_page=30", run_id);
    if (!request(url, token, &r, &status)) check->error = http_error(status);
    else if (!read_artifact(&r, &candidate)) check->error = no_build;
    else check->build = candidate;

done:
    free(r.json);
}

// Check the embedded build SHA in the executable image, excluding bundled ROMFS data.
static bool nro_has_build(const char *path, const char *sha) {
    if (strlen(sha) != 40 || strspn(sha, "0123456789abcdef") != 40) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char header[28], buffer[4096 + 40];
    bool found = false;
    if (fread(header, 1, sizeof(header), file) == sizeof(header) &&
        !memcmp(header + 16, "NRO0", 4)) {
        uint32_t limit = 0;
        for (unsigned i = 0; i < 4; ++i) limit |= (uint32_t)header[24 + i] << (8 * i);
        if (limit >= sizeof(header) && limit <= ZIP_LIMIT) {
            size_t remaining = limit - sizeof(header), carry = 0;
            while (remaining) {
                size_t size = remaining < 4096 ? remaining : 4096;
                if (fread(buffer + carry, 1, size, file) != size) break;
                size_t available = size + carry;
                if (memmem(buffer, available, sha, 41)) {
                    found = true;
                    break;
                }
                carry = available < 40 ? available : 40;
                memmove(buffer, buffer + available - carry, carry);
                remaining -= size;
            }
        }
    }
    fclose(file);
    return found;
}

const char *stage_update(FichierJob *job, const char *target, const UpdateBuild *build) {
    char token[512], stage[320], zip[] = NXLOAD_APP_DIR "/.update-XXXXXX";
    if (!read_token(token)) return "Додай токен GitHub у switch/nxload/github-token.txt";
    if (!build->id[0]) return "Спочатку перевір наявність оновлення NXLoad";
    if (snprintf(stage, sizeof(stage), "%s.new", target) >= (int)sizeof(stage))
        return "Шлях до NXLoad.nro задовгий";
    if (!nxload_ensure_storage()) return "Не вдалося підготувати каталог оновлення на microSD";

    Response r = {.job = job, .cancelled = &job->cancelled};
    const char *update_error;
    long status = 0;
    char url[256];
    bool zip_created = false;
    int fd = mkstemp(zip);
    if (fd < 0) { update_error = "Не вдалося створити файл оновлення на microSD"; goto done; }
    zip_created = true;
    r.out = fdopen(fd, "wb");
    if (!r.out) { close(fd); update_error = "Не вдалося записати файл оновлення"; goto done; }
    sha256ContextCreate(&r.sha);
    snprintf(url, sizeof(url), ACTIONS "artifacts/%s/zip", build->id);
    bool received = request(url, token, &r, &status);
    bool saved = fclose(r.out) == 0;
    if (!received || !saved) { update_error = http_error(status); goto done; }
    if (!zip_digest(&r, build->digest)) { update_error = "Контрольна сума збірки не збігається"; goto done; }
    if (!access(stage, F_OK) && unlink(stage)) {
        update_error = "Не вдалося прибрати попередній тимчасовий файл оновлення";
        goto done;
    }
    if (!extract_nro(job, zip, stage)) {
        update_error = "Не вдалося підготувати NXLoad.nro. Перевір місце та відсутність файлу .new.";
        goto done;
    }
    if (!nro_has_build(stage, build->sha)) {
        unlink(stage);
        update_error = tr("Файл NXLoad не відповідає вибраній збірці");
        goto done;
    }
    update_error = NULL;

done:
    if (zip_created) unlink(zip);
    if (atomic_load(&job->cancelled)) {
        if (!update_error) unlink(stage);
        return "Оновлення скасовано";
    }
    return update_error;
}

bool apply_update(const char *target, const char *sha) {
    char stage[320], backup[320];
    if (snprintf(stage, sizeof(stage), "%s.new", target) >= (int)sizeof(stage) ||
        snprintf(backup, sizeof(backup), "%s.bak", target) >= (int)sizeof(backup)) return false;
    if (access(target, R_OK) || !nro_has_build(stage, sha)) return false;
    if (!access(backup, F_OK) && unlink(backup)) return false;
    if (rename(target, backup)) return false;
    if (!rename(stage, target)) {
        if (nro_has_build(target, sha)) return true;
        if (rename(target, stage)) return false;
    }
    rename(backup, target);
    return false;
}
