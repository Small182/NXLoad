#define _GNU_SOURCE
#include "fichier.h"
#include "range.h"
#include "storage.h"
#include "language.h"
#include "json.h"
#include "unpack.h"

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define HTML_CAPACITY (512 * 1024)
#define URL_CAPACITY 2048
#define FILE_WRITE_BUFFER (512 * 1024)
#define FILE_RECEIVE_BUFFER (256 * 1024)
#define DLARCHIVE_ORIGIN "https://drive.dlarchive.my.id"
#define DLARCHIVE_FILE_HOST "direct-stor.berkasdrive.com"
typedef enum { HOST_FICHIER, HOST_MEGAUP, HOST_DLARCHIVE, HOST_ROMSHQ, HOST_DATANODES } DownloadHost;

typedef struct {
    FichierJob *job;
    char *html;
    size_t length;
    char name[160];
    char fallback_name[160];
    long status;
    uint64_t expected;
    bool challenge;
    bool text;
    bool attachment;
    bool binary;
    bool allow_file;
    bool retry_parallel;
    FILE *file;
    char *write_buffer;
    char partial[256];
    const char *error;
} Transfer;

static bool in_domain(const char *host, const char *domain) {
    size_t n = strlen(host), m = strlen(domain);
    return !strcasecmp(host, domain) ||
        (n > m && host[n - m - 1] == '.' && !strcasecmp(host + n - m, domain));
}

static bool resolve_host_url(const char *base, const char *target,
                             char out[URL_CAPACITY], DownloadHost source) {
    if (!target || !*target) return false;
    CURLU *parsed = curl_url();
    if (!parsed) return false;
    char *scheme = NULL, *host = NULL, *url = NULL, *port = NULL, *query = NULL;
    bool ok = (!base || !curl_url_set(parsed, CURLUPART_URL, base, CURLU_DISALLOW_USER)) &&
        !curl_url_set(parsed, CURLUPART_URL, target, CURLU_DISALLOW_USER) &&
        !curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) && !strcmp(scheme, "https") &&
        !curl_url_get(parsed, CURLUPART_HOST, &host, 0) &&
        (source == HOST_MEGAUP ? !strcasecmp(host, MEGAUP_FILE_HOST) :
         source == HOST_DLARCHIVE ? !strcasecmp(host, DLARCHIVE_FILE_HOST) :
         source == HOST_ROMSHQ ? (!strcasecmp(host, "romshq.com") || !strcasecmp(host, "www.romshq.com")) :
         source == HOST_DATANODES ? in_domain(host, "datanodes.to") :
         in_domain(host, "1fichier.com") || in_domain(host, "desfichiers.com")) &&
        !curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) && !strcmp(port, "443") &&
        !curl_url_get(parsed, CURLUPART_URL, &url, 0) && strlen(url) < URL_CAPACITY;
    if (ok && source == HOST_MEGAUP) {
        ok = !curl_url_get(parsed, CURLUPART_QUERY, &query, 0) &&
            !strncmp(query, "download_token=", 15) && query[15] && query[15] != '&';
    }
    if (ok) strcpy(out, url);
    curl_free(query);
    curl_free(scheme);
    curl_free(host);
    curl_free(url);
    curl_free(port);
    curl_url_cleanup(parsed);
    return ok;
}

static bool resolve_romshq_url(const char *base, const char *target, const char *prefix,
                               char out[URL_CAPACITY]) {
    if (!resolve_host_url(base, target, out, HOST_ROMSHQ)) return false;
    const char *path = strchr(out + sizeof("https://") - 1, '/');
    size_t length = strlen(prefix);
    return path && !strncmp(path, prefix, length) && path[length] &&
           path[length] != '?' && path[length] != '#';
}

// Signed ROMsHQ /d/ URLs and DataNodes download pages can redirect to
// distinct HTTPS delivery hosts. Accept ordinary public DNS names only:
 // do not follow redirects to IP literals, local machines or custom ports.
static bool public_redirect_host(const char *host) {
    if (!host) return false;
    size_t n = strlen(host);
    if (n < 4 || n > 253 || !strchr(host, '.')) return false;
    if (in_domain(host, "localhost") || in_domain(host, "local") ||
        in_domain(host, "internal") || in_domain(host, "test") ||
        in_domain(host, "invalid")) return false;
    bool has_letter = false, at_label_start = true;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)host[i];
        if (c == '.') {
            if (at_label_start || host[i - 1] == '-') return false;
            at_label_start = true;
        } else if (c >= 'a' && c <= 'z') {
            has_letter = true;
            at_label_start = false;
        } else if (c >= 'A' && c <= 'Z') {
            has_letter = true;
            at_label_start = false;
        } else if (c >= '0' && c <= '9') {
            at_label_start = false;
        } else if (c == '-' && !at_label_start) {
            at_label_start = false;
        } else {
            return false;
        }
    }
    return has_letter && !at_label_start && host[n - 1] != '-';
}

static bool resolve_download_redirect(const char *base, const char *target,
                                      char out[URL_CAPACITY], char host_out[128]) {
    if (!target || !*target) return false;
    CURLU *parsed = curl_url();
    if (!parsed) return false;
    char *scheme = NULL, *host = NULL, *port = NULL, *address = NULL;
    bool ok = (!base || !curl_url_set(parsed, CURLUPART_URL, base, CURLU_DISALLOW_USER)) &&
        !curl_url_set(parsed, CURLUPART_URL, target, CURLU_DISALLOW_USER) &&
        !curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) &&
        !strcasecmp(scheme, "https") &&
        !curl_url_get(parsed, CURLUPART_HOST, &host, 0) &&
        public_redirect_host(host) &&
        !curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) &&
        !strcmp(port, "443") &&
        !curl_url_get(parsed, CURLUPART_URL, &address, 0) &&
        strlen(address) < URL_CAPACITY;
    if (ok) {
        strcpy(out, address);
        snprintf(host_out, 128, "%s", host);
    }
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_free(address);
    curl_url_cleanup(parsed);
    return ok;
}

static void redirect_referer(const char *from, char referer[URL_CAPACITY]) {
    CURLU *parsed = curl_url();
    if (!parsed) return;
    char *host = NULL;
    if (!curl_url_set(parsed, CURLUPART_URL, from, CURLU_DISALLOW_USER) &&
        !curl_url_get(parsed, CURLUPART_HOST, &host, 0) &&
        public_redirect_host(host))
        snprintf(referer, URL_CAPACITY, "https://%s/", host);
    curl_free(host);
    curl_url_cleanup(parsed);
}

static bool resolve_url(const char *base, const char *target, char out[URL_CAPACITY]) {
    return resolve_host_url(base, target, out, HOST_FICHIER);
}

static bool same_host(const char *a, const char *b) {
    const char *end_a = a + strcspn(a + 8, "/?") + 8;
    const char *end_b = b + strcspn(b + 8, "/?") + 8;
    return end_a - a == end_b - b && !strncasecmp(a, b, end_a - a);
}

static void copy_filename(char *out, size_t capacity, const char *name, size_t n) {
    if (n == 0 || n >= capacity || name[0] == '.') return;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 32 || ch == 127 || strchr("/\\:<>|?*\"", ch)) return;
    }
    memcpy(out, name, n);
    out[n] = 0;
}

static void read_filename(Transfer *t, const char *value) {
    const char *name = strcasestr(value, "filename=");
    if (!name) return;
    name += strlen("filename=");
    bool quoted = *name == '"';
    if (quoted) ++name;
    copy_filename(t->name, sizeof(t->name), name, strcspn(name, quoted ? "\"\r\n" : ";\r\n"));
}

static size_t on_header(char *data, size_t size, size_t count, void *arg) {
    Transfer *t = arg;
    size_t n = size * count;
    char line[1024];
    if (n >= sizeof(line)) return n;
    memcpy(line, data, n);
    line[n] = 0;
    if (!strncmp(line, "HTTP/", 5)) {
        const char *code = strchr(line, ' ');
        t->status = code ? strtol(code + 1, NULL, 10) : 0;
        t->length = 0;
        t->expected = 0;
        t->html[0] = 0;
        t->name[0] = 0;
        t->challenge = t->text = t->attachment = t->binary = false;
    } else if (!strncasecmp(line, "cf-mitigated:", 13)) {
        t->challenge = strcasestr(line, "challenge") != NULL;
    } else if (!strncasecmp(line, "content-disposition:", 20)) {
        t->attachment = strcasestr(line, "attachment") != NULL;
        read_filename(t, line);
    } else if (!strncasecmp(line, "content-length:", 15)) {
        t->expected = strtoull(line + 15, NULL, 10);
    } else if (!strncasecmp(line, "content-type:", 13)) {
        t->text = strcasestr(line, "text/") || strcasestr(line, "html") || strcasestr(line, "json");
        t->binary = strcasestr(line, "application/octet-stream") ||
            strcasestr(line, "application/x-rar") || strcasestr(line, "application/vnd.rar") ||
            strcasestr(line, "application/zip") || strcasestr(line, "application/x-zip-compressed") ||
            strcasestr(line, "application/x-7z") ||
            ((t->job->dlarchive || t->job->romshq || t->job->datanodes) &&
                                  (strcasestr(line, "application/x-nsp") ||
                                    strcasestr(line, "application/force-download")));
    }
    return n;
}


static void partial_path(char out[256], const char *url, const char *id) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const char *values[] = {url ? url : "", id ? id : ""};
    for (unsigned i = 0; i < 2; ++i) {
        for (const unsigned char *p = (const unsigned char *)values[i]; *p; ++p) {
            hash ^= *p;
            hash *= UINT64_C(1099511628211);
        }
        hash ^= 0xff;
        hash *= UINT64_C(1099511628211);
    }
    snprintf(out, 256, NXLOAD_DOWNLOAD_DIR "/.nxload-%016" PRIx64 ".part", hash);
}

void fichier_discard_partial(const char *url, const char *id) {
    char path[256], metadata[288];
    partial_path(path, url, id);
    unlink(path);
    snprintf(metadata, sizeof(metadata), "%s.range", path);
    unlink(metadata);
    snprintf(metadata, sizeof(metadata), "%s.progress", path);
    unlink(metadata);
    snprintf(metadata, sizeof(metadata), "%s.progress.prev", path);
    unlink(metadata);
    snprintf(metadata, sizeof(metadata), "%s.progress.tmp", path);
    unlink(metadata);
}

static bool open_partial(Transfer *t, bool buffered) {
    const char *fallback = t->job->romshq ? "romshq-download.bin" :
        t->job->datanodes ? "datanodes-download.bin" :
        t->job->megaup ? "megaup-download.bin" : "1fichier-download.bin";
    char generic[DLARCHIVE_ID_CAPACITY + 8];
    if (t->job->romshq && t->job->romshq_id) {
        snprintf(generic, sizeof(generic), "romshq-%s.bin", t->job->romshq_id);
        fallback = generic;
    }
    if (t->job->dlarchive) {
        snprintf(generic, sizeof(generic), "%s.bin", t->job->dlarchive_id);
        fallback = t->fallback_name[0] ? t->fallback_name : generic;
    }
    const char *name = t->name[0] ? t->name : fallback;
    snprintf(t->job->path, sizeof(t->job->path), "%s/%s", NXLOAD_DOWNLOAD_DIR, name);
    partial_path(t->partial, t->job->url, t->job->dlarchive_id ? t->job->dlarchive_id : t->job->romshq_id);
    struct stat info;
    if (!stat(t->job->path, &info)) {
        t->error = "Destination file already exists";
        return false;
    }

    int fd = -1;
#ifdef __SWITCH__
    // Never recreate an existing partial file when resuming a large download.
    if (t->expected > UINT32_MAX && access(t->partial, F_OK) != 0) {
        Result rc = fsdevCreateFile(t->partial, 0, FsCreateOption_BigFile);
        if (R_FAILED(rc)) {
            t->error = "Cannot create a large split file on SD";
            return false;
        }
    }
#endif
    fd = open(t->partial, O_WRONLY | O_CREAT | (buffered ? O_TRUNC : 0), 0666);
    if (buffered) {
        char metadata[272];
        snprintf(metadata, sizeof(metadata), "%s.range", t->partial);
        unlink(metadata);
    }
    if (fd < 0) {
        t->error = nxload_storage_error(errno, "Не вдалося відкрити тимчасовий файл на microSD");
        return false;
    }
    t->file = fdopen(fd, "wb");
    if (!t->file) {
        close(fd);
        unlink(t->partial);
        t->error = "Cannot open output stream";
        return false;
    }
    t->write_buffer = buffered ? malloc(FILE_WRITE_BUFFER) : NULL;
    if (t->write_buffer && setvbuf(t->file, t->write_buffer, _IOFBF, FILE_WRITE_BUFFER)) {
        free(t->write_buffer);
        t->write_buffer = NULL;
    }
    return true;
}

static bool parallel_download(Transfer *t, const char *url, const char *referer) {
    FichierJob *job = t->job;
    const RangeSource source = { url, referer, "romfs:/cacert.pem" };
    RangeInfo info;
    char partial[256];
    partial_path(partial, job->url,
                 job->dlarchive_id ? job->dlarchive_id : job->romshq_id);
    struct stat pending;
    bool had_partial = !stat(partial, &pending) && pending.st_size > 0;
    if (!range_probe(&source, job, &info)) {
        if (had_partial) {
            job->resume_blocked = true;
            job->result = "Сервер не підтримує відновлення. Y — скасувати.";
            return true;
        }
        if (info.status != 429) return false;
        job->http = info.status;
        job->result = job->dlarchive ? "DLArchive обмежив кількість запитів" :
                                        "Файловий сервер обмежив кількість запитів";
        return true;
    }
    t->expected = info.length;
    read_filename(t, info.disposition);
    if (!open_partial(t, false)) {
        job->result = t->error;
        return true;
    }
    RangeResult result = range_download(&source, &info, job, fileno(t->file), t->partial);
    bool closed = fclose(t->file) == 0;
    t->file = NULL;
    if (result == RANGE_OK && closed && !atomic_load(&job->cancelled)) {
        if (!rename(t->partial, job->path)) {
            fichier_discard_partial(job->url,
                                    job->dlarchive_id ? job->dlarchive_id : job->romshq_id);
            job->result = "Download saved";
            job->tls_detail[0] = 0;
            return true;
        }
        job->result = "Could not finalize file on SD";
        return true;
    }
    if (atomic_load(&job->cancelled) && !atomic_load(&job->pause_requested))
        fichier_discard_partial(job->url, job->dlarchive_id ? job->dlarchive_id : job->romshq_id);
    if (atomic_load(&job->cancelled)) return true;
    if (result == RANGE_SD_ERROR || !closed) {
        job->result = "SD write failed (space or file size limit)";
        return true;
    }
    if (result == RANGE_RATE_LIMIT) {
        job->result = job->dlarchive ? "DLArchive обмежив кількість запитів" :
                                        "Файловий сервер обмежив кількість запитів";
        return true;
    }
    if (result == RANGE_INTERRUPTED || (result == RANGE_RETRY_SINGLE && had_partial)) {
        job->resume_blocked = true;
        job->result = "Не вдалося перевірити збережені блоки. Y — скасувати.";
        return true;
    }
    if (result == RANGE_RETRY_SINGLE) {
        // No previously saved data: fall back to the original single-GET path.
        fichier_discard_partial(job->url, job->dlarchive_id ?
                                 job->dlarchive_id : job->romshq_id);
    }
    atomic_store(&job->received, 0);
    atomic_store(&job->total, 0);
    atomic_store(&job->streams, 0);
    job->avg_speed = 0;
    job->http = 0;
    return false;
}

static size_t on_body(char *data, size_t size, size_t count, void *arg) {
    Transfer *t = arg;
    size_t n = size * count;
    if (atomic_load(&t->job->cancelled)) return 0;
    if (t->allow_file && t->status == 200 && !t->challenge && !t->text &&
        (t->attachment || t->binary)) {
        if (!t->file) {
            char existing[256];
            partial_path(existing, t->job->url,
                         t->job->dlarchive_id ? t->job->dlarchive_id : t->job->romshq_id);
            struct stat pending;
            if (!stat(existing, &pending) && pending.st_size > 0) {
                // Try the same provider as verified HTTP Range, never truncate.
                t->retry_parallel = true;
                return 0;
            }
            if (!open_partial(t, true)) return 0;
        }
        size_t written = fwrite(data, 1, n, t->file);
        atomic_fetch_add(&t->job->received, written);
        if (written != n) t->error = nxload_storage_error(errno, "Помилка запису на microSD");
        return written;
    }
    if (n > HTML_CAPACITY - t->length) {
        t->error = "Unexpected response size";
        return 0;
    }
    memcpy(t->html + t->length, data, n);
    t->length += n;
    t->html[t->length] = 0;
    return n;
}

static int on_progress(void *arg, curl_off_t total, curl_off_t now, curl_off_t up, curl_off_t sent) {
    (void)now;
    (void)up;
    (void)sent;
    Transfer *t = arg;
    if (t->file && total > 0) atomic_store(&t->job->total, (uint64_t)total);
    return atomic_load(&t->job->cancelled);
}

static bool node_is(xmlNodePtr node, const char *name) {
    return node->type == XML_ELEMENT_NODE && !xmlStrcasecmp(node->name, BAD_CAST name);
}

static bool label_matches(const char *s) {
    return s && (strcasestr(s, "free download") ||
        strcasestr(s, "show the download link") || strcasestr(s, "show download link"));
}

static bool has_download_control(xmlNodePtr node, bool second) {
    for (; node; node = node->next) {
        if (node_is(node, "input")) {
            xmlChar *value = xmlGetProp(node, BAD_CAST "value");
            xmlChar *name = xmlGetProp(node, BAD_CAST "name");
            xmlChar *kind = xmlGetProp(node, BAD_CAST "type");
            bool match = label_matches((const char *)value) ||
                (second && name && kind && !xmlStrcasecmp(name, BAD_CAST "t") &&
                 !xmlStrcasecmp(kind, BAD_CAST "hidden"));
            xmlFree(value);
            xmlFree(name);
            xmlFree(kind);
            if (match) return true;
        }
        if (has_download_control(node->children, second)) return true;
    }
    return false;
}

static xmlNodePtr find_form(xmlNodePtr node, bool second) {
    for (; node; node = node->next) {
        if (node_is(node, "form")) {
            xmlChar *method = xmlGetProp(node, BAD_CAST "method");
            xmlChar *content = xmlNodeGetContent(node);
            bool post = method && !xmlStrcasecmp(method, BAD_CAST "post");
            bool match = label_matches((const char *)content) || has_download_control(node->children, second);
            xmlFree(method);
            xmlFree(content);
            if (post && match) return node;
        }
        xmlNodePtr found = find_form(node->children, second);
        if (found) return found;
    }
    return NULL;
}

static bool append_fields(CURL *curl, xmlNodePtr node, xmlBufferPtr buffer) {
    for (; node; node = node->next) {
        if (node_is(node, "input")) {
            xmlChar *type = xmlGetProp(node, BAD_CAST "type");
            xmlChar *name = xmlGetProp(node, BAD_CAST "name");
            xmlChar *value = xmlGetProp(node, BAD_CAST "value");
            bool hidden = type && !xmlStrcasecmp(type, BAD_CAST "hidden") && name && *name;
            bool ok = true;
            if (hidden && !xmlHasProp(node, BAD_CAST "disabled")) {
                char *key = curl_easy_escape(curl, (char *)name, 0);
                char *escaped = curl_easy_escape(curl, value ? (char *)value : "", 0);
                ok = key && escaped && xmlBufferLength(buffer) < 16384 &&
                    (!xmlBufferLength(buffer) || !xmlBufferCat(buffer, BAD_CAST "&")) &&
                    !xmlBufferCat(buffer, BAD_CAST key) &&
                    !xmlBufferCat(buffer, BAD_CAST "=") &&
                    !xmlBufferCat(buffer, BAD_CAST escaped);
                curl_free(key);
                curl_free(escaped);
            }
            xmlFree(type);
            xmlFree(name);
            xmlFree(value);
            if (!ok) return false;
        }
        if (!append_fields(curl, node->children, buffer)) return false;
    }
    return true;
}

static bool posted_form_url(CURL *curl, const char *base, xmlNodePtr form,
                            char next[URL_CAPACITY], xmlBufferPtr data) {
    xmlChar *action = xmlGetProp(form, BAD_CAST "action");
    bool ok = resolve_url(base, action && *action ? (char *)action : base, next) &&
        same_host(base, next) && append_fields(curl, form->children, data);
    xmlFree(action);
    return ok;
}

static bool download_anchor(xmlNodePtr node, const char *base, char next[URL_CAPACITY]) {
    for (; node; node = node->next) {
        if (node_is(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            xmlChar *label = xmlNodeGetContent(node);
            bool ok = label && strcasestr((char *)label, "download") &&
                resolve_url(base, (char *)href, next) && !same_host(base, next);
            xmlFree(href);
            xmlFree(label);
            if (ok) return true;
        }
        if (download_anchor(node->children, base, next)) return true;
    }
    return false;
}

static bool in_download_spoiler(xmlNodePtr node) {
    for (xmlNodePtr parent = node->parent; parent; parent = parent->parent) {
        if (!node_is(parent, "div")) continue;
        xmlChar *classes = xmlGetProp(parent, BAD_CAST "class");
        bool match = classes && strstr((const char *)classes, "su-spoiler-content");
        xmlFree(classes);
        if (match) return true;
    }
    return false;
}

static bool base_game_row(xmlNodePtr node) {
    xmlChar *text = xmlNodeGetContent(node);
    if (!text) return false;
    const char *p = (const char *)text;
    while (isspace((unsigned char)*p)) ++p;
    bool match = !strncasecmp(p, "game:", 5) ||
        !strncasecmp(p, "base game", 9) ||
        !strncasecmp(p, "base:", 5);
    xmlFree(text);
    return match;
}

static bool fichier_link(xmlNodePtr node, char out[URL_CAPACITY]) {
    for (; node; node = node->next) {
        if (node_is(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            bool valid = href && resolve_url(NULL, (const char *)href, out);
            xmlFree(href);
            if (valid) return true;
        }
        if (fichier_link(node->children, out)) return true;
    }
    return false;
}

static bool find_base_mirror(xmlNodePtr node, char out[URL_CAPACITY]) {
    for (; node; node = node->next) {
        if (node_is(node, "p") && in_download_spoiler(node) &&
            base_game_row(node) && fichier_link(node->children, out)) return true;
        if (find_base_mirror(node->children, out)) return true;
    }
    return false;
}

static bool resolve_bunny(CURL *curl, Transfer *t, char out[URL_CAPACITY]) {
    FichierJob *job = t->job;
    curl_easy_setopt(curl, CURLOPT_URL, BUNNY_NSWGF_URL);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    CURLcode code = curl_easy_perform(curl);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &job->http);
    if (code != CURLE_OK || job->http != 200 || t->challenge) {
        job->result = "NSWGF unavailable. Use Y to enter a 1Fichier link.";
        return false;
    }
    htmlDocPtr doc = htmlReadMemory(t->html, (int)t->length, NULL, NULL,
        HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    bool found = doc && find_base_mirror(xmlDocGetRootElement(doc), out);
    xmlFreeDoc(doc);
    if (!found) job->result = "No base-game 1Fichier link found. Use Y.";
    return found;
}

// DLArchive returns a flat JSON object with string, boolean and numeric fields.
static void skip_json_space(const char **p) {
    while (isspace((unsigned char)**p)) ++*p;
}

static bool read_dlarchive_reply(Transfer *t, char out[URL_CAPACITY]) {
    const char *p = t->html;
    char target[URL_CAPACITY] = {0};
    bool ok = false;
    skip_json_space(&p);
    if (*p++ != '{') return false;
    while (true) {
        char key[64], value[URL_CAPACITY];
        skip_json_space(&p);
        if (!read_json_string(&p, key, sizeof(key))) return false;
        skip_json_space(&p);
        if (*p++ != ':') return false;
        skip_json_space(&p);
        if (*p == '"') {
            if (!read_json_string(&p, value, sizeof(value))) return false;
            if (!strcmp(key, "url")) strcpy(target, value);
            if (!strcmp(key, "filename")) {
                copy_filename(t->fallback_name, sizeof(t->fallback_name), value, strlen(value));
            }
        } else {
            size_t n = strcspn(p, ",} \t\r\n");
            if (!n) return false;
            if (!strcmp(key, "ok")) ok = n == 4 && !memcmp(p, "true", 4);
            p += n;
        }
        skip_json_space(&p);
        if (*p == '}') break;
        if (*p++ != ',') return false;
    }
    ++p;
    skip_json_space(&p);
    return !*p && ok && !strncmp(target, "https://", 8) &&
        resolve_host_url(NULL, target, out, HOST_DLARCHIVE);
}

static bool resolve_dlarchive(CURL *curl, Transfer *t, char out[URL_CAPACITY]) {
    FichierJob *job = t->job;
    const char *id = job->dlarchive_id;
    size_t n = id ? strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") : 0;
    if (!n || n >= DLARCHIVE_ID_CAPACITY || id[n]) {
        job->result = "Invalid DLArchive file ID";
        return false;
    }
    char page[160], api[180];
    snprintf(page, sizeof(page), DLARCHIVE_ORIGIN "/dl/?id=%s", id);
    snprintf(api, sizeof(api),
             "https://uptobox.dlarchive.my.id/files_api.php?action=file-url&id=%s", id);
    struct curl_slist *headers = curl_slist_append(NULL, "Origin: " DLARCHIVE_ORIGIN);
    if (!headers) return false;
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_REFERER, page);
    curl_easy_setopt(curl, CURLOPT_URL, api);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &job->api_http);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, NULL);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(headers);

    if (code != CURLE_OK) {
        job->result = curl_easy_strerror(code);
        return false;
    }
    if (job->api_http != 200 || t->challenge) {
        job->result = "DLArchive API did not return a file address";
        return false;
    }
    if (!read_dlarchive_reply(t, out)) {
        job->result = "DLArchive API returned no supported file address";
        return false;
    }
    return true;
}

static bool read_romshq_reply(const char *p, const char *page, char target[URL_CAPACITY]) {
    char url[URL_CAPACITY] = {0}, mode[24] = {0};
    bool success = false;
    skip_json_space(&p);
    if (*p++ != '{') return false;
    while (true) {
        char key[64], value[URL_CAPACITY];
        skip_json_space(&p);
        if (!read_json_string(&p, key, sizeof(key))) return false;
        skip_json_space(&p);
        if (*p++ != ':') return false;
        skip_json_space(&p);
        if (*p == '"') {
            if (!read_json_string(&p, value, sizeof(value))) return false;
            if (!strcmp(key, "download_url")) strcpy(url, value);
            if (!strcmp(key, "mode") && strlen(value) < sizeof(mode)) strcpy(mode, value);
        } else {
            size_t n = strcspn(p, ",} \t\r\n");
            if (!n) return false;
            if (!strcmp(key, "success")) success = n == 4 && !memcmp(p, "true", 4);
            p += n;
        }
        skip_json_space(&p);
        if (*p == '}') break;
        if (*p++ != ',') return false;
    }
    ++p;
    skip_json_space(&p);
    if (*p || !success) return false;
    if (!strcmp(mode, "direct"))
        return resolve_romshq_url(page, url, "/d/", target);
    return resolve_host_url(NULL, url, target, HOST_DATANODES);
}

static bool romshq_headers(struct curl_slist **headers, const char *line) {
    struct curl_slist *next = curl_slist_append(*headers, line);
    if (!next) return false;
    *headers = next;
    return true;
}

static void csrf_token(xmlNodePtr node, char out[256]) {
    for (; node && !out[0]; node = node->next) {
        if (node_is(node, "meta")) {
            xmlChar *name = xmlGetProp(node, BAD_CAST "name");
            if (name && !xmlStrcasecmp(name, BAD_CAST "csrf-token")) {
                xmlChar *value = xmlGetProp(node, BAD_CAST "content");
                if (value && *value && xmlStrlen(value) < 256 &&
                    !strpbrk((char *)value, "\r\n")) strcpy(out, (char *)value);
                xmlFree(value);
            }
            xmlFree(name);
        }
        csrf_token(node->children, out);
    }
}

static bool fetch_romshq_card(CURL *curl, Transfer *t, char page[URL_CAPACITY]) {
    for (unsigned hop = 0; hop <= 5; ++hop) {
        curl_easy_setopt(curl, CURLOPT_URL, page);
        CURLcode code = curl_easy_perform(curl);
        t->job->api_http = t->status;
        if (code == CURLE_OK && t->status == 200 && !t->challenge) return true;
        if (t->challenge || t->status == 403) {
            t->job->result = "ROMsHQ вимагає перевірку в браузері";
            return false;
        }
        if (code != CURLE_OK) {
            t->job->result = curl_easy_strerror(code);
            return false;
        }
        if (t->status < 300 || t->status >= 400) {
            t->job->result = "Картка ROMsHQ недоступна";
            return false;
        }
        char *location = NULL, next[URL_CAPACITY];
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
        if (!resolve_romshq_url(page, location, "/game/", next)) {
            t->job->result = "ROMsHQ перенаправив картку на непідтримувану адресу";
            return false;
        }
        if (hop == 5 || !strcmp(next, page)) break;
        strcpy(page, next);
    }

    t->job->result = "Забагато перенаправлень картки ROMsHQ";
    return false;
}

static bool resolve_romshq(CURL *curl, Transfer *t, char out[URL_CAPACITY]) {
    FichierJob *job = t->job;
    const char *id = job->romshq_id;
    char page[URL_CAPACITY], api[160], origin[64];
    if (!id || !*id || strlen(id) > 20 || id[strspn(id, "0123456789")] ||
        !resolve_romshq_url(NULL, job->url, "/game/", page)) {
        job->result = "Некоректна картка або ID ROMsHQ";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (!fetch_romshq_card(curl, t, page)) return false;

    const char *path = strchr(page + sizeof("https://") - 1, '/');
    snprintf(origin, sizeof(origin), "%.*s", (int)(path - page), page);
    char token[256] = {0}, csrf[300], origin_header[80];
    snprintf(origin_header, sizeof(origin_header), "Origin: %s", origin);
    htmlDocPtr doc = htmlReadMemory(t->html, (int)t->length, page, "UTF-8",
        HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (doc) {
        csrf_token(xmlDocGetRootElement(doc), token);
        xmlFreeDoc(doc);
    }
    snprintf(csrf, sizeof(csrf), "X-CSRF-TOKEN: %s", token);
    struct curl_slist *headers = NULL;
    if (!romshq_headers(&headers, origin_header) ||
        !romshq_headers(&headers, "Accept: application/json") ||
        !romshq_headers(&headers, "X-Requested-With: XMLHttpRequest") ||
        (token[0] && !romshq_headers(&headers, csrf))) {
        curl_slist_free_all(headers);
        return false;
    }
    snprintf(api, sizeof(api), "%s/api/download/sign/%s", origin, id);
    curl_easy_setopt(curl, CURLOPT_URL, api);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_REFERER, page);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &job->api_http);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, NULL);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_slist_free_all(headers);
    if (code != CURLE_OK || job->api_http != 200 || t->challenge) {
        job->result = "ROMsHQ не видав підписане посилання";
        return false;
    }
    if (!read_romshq_reply(t->html, page, out)) {
        job->result = "ROMsHQ повернув непідтримуване посилання";
        return false;
    }
    return true;
}

static const char *page_error(const Transfer *t) {
    if (t->challenge || strcasestr(t->html, "just a moment") ||
        strcasestr(t->html, "cf-turnstile") || strcasestr(t->html, "h-captcha") ||
        strcasestr(t->html, "g-recaptcha")) return "Host asks for browser verification";
    if (strcasestr(t->html, "site unavailable")) return "1Fichier returned Site Unavailable";
    if (t->status == 404 || strcasestr(t->html, "file not found")) return "File unavailable on 1Fichier";
    if (t->status == 429 || strcasestr(t->html, "requests are too fast")) return "1Fichier rate limit";
    if (t->status >= 400) return "1Fichier refused the request";
    return NULL;
}

static unsigned wait_seconds(const char *html) {
    const char *counter = strcasestr(html, "var count");
    if (!counter) return 0;
    counter += strlen("var count");
    while (isspace((unsigned char)*counter)) ++counter;
    if (*counter != '=') return 0;
    unsigned long seconds = strtoul(counter + 1, NULL, 10);
    return seconds > 120 ? 121 : (unsigned)seconds;
}

void fichier_download(FichierJob *job) {
    job->result = "Download setup failed";
    if (!nxload_ensure_storage()) {
        job->result = "Cannot create download directory on SD";
        return;
    }
    CURL *curl = curl_easy_init();
    xmlBufferPtr data = xmlBufferCreate();
    Transfer t = { .job = job, .html = calloc(HTML_CAPACITY + 1, 1) };
    char url[URL_CAPACITY], next[URL_CAPACITY], referrer[URL_CAPACITY] = {0};
    bool posting = false;
    unsigned forms = 0;
    if (!curl || !data || !t.html) goto done;

#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 40L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NXLoad/1.0.5");
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    if (job->dlarchive) {
        if (!resolve_dlarchive(curl, &t, url)) goto done;
        strcpy(referrer, DLARCHIVE_ORIGIN "/");
        t.name[0] = 0;
        if (parallel_download(&t, url, referrer)) goto done;
    } else if (job->romshq) {
        if (job->romshq_id) {
            if (!resolve_romshq(curl, &t, url)) goto done;
            snprintf(referrer, sizeof(referrer), "%s", job->url);
        } else if (!resolve_romshq_url(NULL, job->url, "/d/", url)) {
            job->result = "Потрібне підписане посилання ROMsHQ /d/";
            goto done;
        }
    } else if (job->datanodes) {
        if (!resolve_host_url(NULL, job->url, url, HOST_DATANODES)) {
            job->result = "Некоректне посилання DataNodes";
            goto done;
        }
    } else if (job->megaup) {
        if (!resolve_host_url(NULL, job->url, url, HOST_MEGAUP)) {
            job->result = "Unexpected MegaUp file address";
            goto done;
        }
    } else if (!strcmp(job->url, BUNNY_NSWGF_URL)) {
        if (!resolve_bunny(curl, &t, url)) goto done;
        job->http = 0;
    } else if (!resolve_url(NULL, job->url, url)) {
        job->result = "Invalid 1Fichier link";
        goto done;
    }
    t.allow_file = true;
    if (job->dlarchive || job->romshq || job->datanodes)
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, (long)FILE_RECEIVE_BUFFER);
    char initial_url[URL_CAPACITY];
    strcpy(initial_url, url);

    bool probe_file_host = false;
    for (unsigned hop = 0; hop < 10 && !atomic_load(&job->cancelled); ++hop) {
        if (probe_file_host) {
            probe_file_host = false;
            t.name[0] = 0;
            if (parallel_download(&t, url, referrer) || atomic_load(&job->cancelled)) goto done;
        }
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_REFERER, referrer[0] ? referrer : NULL);
        if (posting) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)xmlBufferContent(data));
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)xmlBufferLength(data));
        } else {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        }
        CURLcode code = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &job->http);
        if (t.retry_parallel) {
            t.retry_parallel = false;
            if (parallel_download(&t, url, referrer) || atomic_load(&job->cancelled))
                goto done;
            job->resume_blocked = true;
            job->result = "Сервер не підтримує відновлення. Y — скасувати.";
            goto done;
        }
        if (t.file) {
            curl_off_t speed = 0;
            curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD_T, &speed);
            job->avg_speed = speed > 0 ? (uint64_t)speed : 0;
            bool closed = fclose(t.file) == 0;
            if (!closed && !t.error)
                t.error = nxload_storage_error(errno, "Не вдалося зберегти файл на microSD");
            t.file = NULL;
            free(t.write_buffer);
            t.write_buffer = NULL;
            if (code == CURLE_OK && closed && !atomic_load(&job->cancelled)) {
                if (!rename(t.partial, job->path)) {
                    job->result = "Download saved";
                    job->tls_detail[0] = 0;
                    goto done;
                }
                t.error = "Could not finalize file on SD";
            }
            if (atomic_load(&job->cancelled) && !atomic_load(&job->pause_requested))
                fichier_discard_partial(job->url, job->dlarchive_id ? job->dlarchive_id : job->romshq_id);
            job->result = t.error ? t.error : curl_easy_strerror(code);
            goto done;
        }
        if (code != CURLE_OK || t.error) {
            if (job->dlarchive && (code == CURLE_PEER_FAILED_VERIFICATION ||
                code == CURLE_SSL_CACERT_BADFILE || code == CURLE_SSL_CONNECT_ERROR)) {
                long verify = 0;
                curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify);
                char utc[32] = "—";
                time_t now = time(NULL);
                struct tm tm_now;
                if (gmtime_r(&now, &tm_now))
                    strftime(utc, sizeof(utc), "%Y-%m-%d %H:%M UTC", &tm_now);
                snprintf(job->tls_detail, sizeof(job->tls_detail),
                         "TLS %d · X.509 0x%lX · %s", (int)code, (unsigned long)verify, utc);
                if (code == CURLE_PEER_FAILED_VERIFICATION)
                    job->result = "Сертифікат файлового сервера не пройшов перевірку";
                else if (code == CURLE_SSL_CACERT_BADFILE)
                    job->result = "Не вдалося завантажити довірені сертифікати";
                else
                    job->result = "Не вдалося встановити захищене з’єднання";
            } else {
                job->result = t.error ? t.error : curl_easy_strerror(code);
            }
            goto done;
        }
        if (job->dlarchive) {
            char *redirect = NULL;
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);
            if (redirect && resolve_host_url(url, redirect, next, HOST_DLARCHIVE)) {
                strcpy(url, next);
                job->result = "DLArchive reached the request limit";
                continue;
            }
            job->result = t.challenge || t.status == 401 || t.status == 403 ?
                "DLArchive file host rejected the native GET" :
                "DLArchive file host did not return a file";
            goto done;
        }
        if (job->megaup) {
            job->result = t.status == 401 || t.status == 403 || t.challenge ?
                "MegaUp file host rejected the native GET" :
                "MegaUp file host did not return an archive";
            goto done;
        }
        if (job->romshq || job->datanodes) {
            char *location = NULL;
            char destination_host[128] = {0};
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
            if (t.status >= 300 && t.status < 400 && location &&
                resolve_download_redirect(url, location, next, destination_host) &&
                strcmp(next, url)) {
                // Do not leak an expiring signed URL in the Referer header.
                redirect_referer(url, referrer);
                const char *from_card = "https://romshq.com/";
                const char *www_card = "https://www.romshq.com/";
                probe_file_host = strncmp(next, from_card, strlen(from_card)) &&
                                  strncmp(next, www_card, strlen(www_card));
                strcpy(url, next);
                job->result = "Забагато перенаправлень до файлу";
                continue;
            }
            if (location) {
                char ignored[URL_CAPACITY], target_host[128] = {0};
                if (resolve_download_redirect(url, location, ignored, target_host))
                    snprintf(job->tls_detail, sizeof(job->tls_detail),
                             tr("Перенаправлення зупинено: %.78s"), target_host);
                else
                    strcpy(job->tls_detail, "Небезпечне або непідтримуване перенаправлення");
            }
            job->result = t.challenge || t.status == 403 ?
                "Хостинг вимагає перевірку в браузері" :
                t.status >= 300 && t.status < 400 ?
                "Хостинг перенаправив на непідтримувану адресу" :
                "Хостинг не повернув файл";
            goto done;
        }
        const char *error = page_error(&t);
        if (error) {
            job->result = error;
            goto done;
        }

        char *redirect = NULL;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);
        if (redirect) {
            if (!resolve_url(url, redirect, next)) {
                job->result = "Unexpected 1Fichier redirect";
                goto done;
            }
            if (t.status != 307 && t.status != 308) posting = false;
        } else {
            unsigned seconds = wait_seconds(t.html);
            if (seconds > 120) {
                job->result = "1Fichier wait exceeds 120 seconds";
                goto done;
            }
            if (seconds) {
                while (seconds && !atomic_load(&job->cancelled)) {
                    atomic_store(&job->waiting, seconds--);
                    sleep(1);
                }
                atomic_store(&job->waiting, 0);
                if (atomic_load(&job->cancelled)) goto done;
                strcpy(next, initial_url);
                forms = 0;
                posting = false;
            } else {
                htmlDocPtr doc = htmlReadMemory(t.html, (int)t.length, NULL, NULL,
                    HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
                xmlNodePtr root = doc ? xmlDocGetRootElement(doc) : NULL;
                xmlNodePtr form = forms < 2 ? find_form(root, forms > 0) : NULL;
                bool found = download_anchor(root, url, next);
                posting = false;
                if (!found && form) {
                    xmlBufferEmpty(data);
                    found = posted_form_url(curl, url, form, next, data);
                    posting = found;
                    if (found) {
                        ++forms;
                        sleep(2);
                    }
                }
                xmlFreeDoc(doc);
                if (!found) {
                    job->result = "1Fichier returned no recognized free download step";
                    goto done;
                }
            }
        }
        strcpy(referrer, url);
        strcpy(url, next);
        job->result = "1Fichier reached the request limit";
    }

done:
    if (atomic_load(&job->cancelled)) job->result =
        atomic_load(&job->pause_requested) ? "Download paused" : "Download cancelled";
    curl_easy_cleanup(curl);
    xmlBufferFree(data);
    free(t.html);
}
