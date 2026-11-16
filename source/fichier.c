#define _GNU_SOURCE
#include "fichier.h"

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define HTML_CAPACITY (512 * 1024)
#define URL_CAPACITY 2048
#ifdef __SWITCH__
#define OUTPUT_DIR "sdmc:/switch/SwitchDownloadProbe"
#define MEGAUP_DIR "sdmc:/switch/MegaUpDownloader"
#else
#define OUTPUT_DIR "./SwitchDownloadProbe"
#define MEGAUP_DIR "./MegaUpDownloader"
#endif

typedef struct {
    FichierJob *job;
    char *html;
    size_t length;
    char name[160];
    long status;
    bool challenge;
    bool text;
    bool attachment;
    bool binary;
    bool allow_file;
    bool html_type;
    FILE *file;
    char partial[256];
    const char *error;
} Transfer;

static bool in_domain(const char *host, const char *domain) {
    size_t n = strlen(host), m = strlen(domain);
    return !strcasecmp(host, domain) ||
        (n > m && host[n - m - 1] == '.' && !strcasecmp(host + n - m, domain));
}

typedef enum {
    MEGAUP_INVALID,
    MEGAUP_SHARE,
    MEGAUP_GATE,
    MEGAUP_CONTINUE,
    MEGAUP_FILE
} MegaUpStep;

static bool has_query_value(const char *query, const char *key) {
    size_t key_length = strlen(key);
    for (const char *field = query; field && *field;) {
        size_t length = strcspn(field, "&");
        if (length > key_length + 1 && field[key_length] == '=' &&
            !strncmp(field, key, key_length)) return true;
        field += length;
        if (*field == '&') ++field;
    }
    return false;
}

static MegaUpStep megaup_url(const char *base, const char *target, char out[URL_CAPACITY]) {
    if (!target || !*target || strpbrk(target, "\r\n\t ")) return MEGAUP_INVALID;

    CURLU *parsed = curl_url();
    if (!parsed) return MEGAUP_INVALID;
    char *scheme = NULL, *host = NULL, *port = NULL, *path = NULL;
    char *query = NULL, *full = NULL;
    MegaUpStep step = MEGAUP_INVALID;
    if ((base && curl_url_set(parsed, CURLUPART_URL, base, CURLU_DISALLOW_USER)) ||
        curl_url_set(parsed, CURLUPART_URL, target, CURLU_DISALLOW_USER) ||
        curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) || strcmp(scheme, "https") ||
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) ||
        curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) || strcmp(port, "443") ||
        curl_url_get(parsed, CURLUPART_PATH, &path, 0)) goto done;

    curl_url_get(parsed, CURLUPART_QUERY, &query, 0);
    if (!strcasecmp(host, "download.megaup.net") && !strcmp(path, "/") &&
        (has_query_value(query, "url") || has_query_value(query, "idurl"))) {
        step = MEGAUP_GATE;
    } else if ((!strcasecmp(host, "megaup.net") || !strcasecmp(host, "www.megaup.net")) &&
               path[0] == '/' && path[1]) {
        step = has_query_value(query, "pt") ? MEGAUP_CONTINUE : MEGAUP_SHARE;
    } else if (in_domain(host, "megaupdownup.org") && path[0] == '/' && path[1] &&
               has_query_value(query, "download_token")) {
        step = MEGAUP_FILE;
    }

    if (step != MEGAUP_INVALID) {
        curl_url_set(parsed, CURLUPART_FRAGMENT, NULL, 0);
        if (curl_url_get(parsed, CURLUPART_URL, &full, 0) || strlen(full) >= URL_CAPACITY)
            step = MEGAUP_INVALID;
        else
            strcpy(out, full);
    }

done:
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_free(path);
    curl_free(query);
    curl_free(full);
    curl_url_cleanup(parsed);
    return step;
}

static bool resolve_url(const char *base, const char *target, char out[URL_CAPACITY]) {
    if (!target || !*target) return false;
    CURLU *parsed = curl_url();
    if (!parsed) return false;
    char *scheme = NULL, *host = NULL, *url = NULL, *port = NULL;
    bool ok = (!base || !curl_url_set(parsed, CURLUPART_URL, base, CURLU_DISALLOW_USER)) &&
        !curl_url_set(parsed, CURLUPART_URL, target, CURLU_DISALLOW_USER) &&
        !curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) && !strcmp(scheme, "https") &&
        !curl_url_get(parsed, CURLUPART_HOST, &host, 0) &&
        (in_domain(host, "1fichier.com") || in_domain(host, "desfichiers.com")) &&
        !curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) && !strcmp(port, "443") &&
        !curl_url_get(parsed, CURLUPART_URL, &url, 0) && strlen(url) < URL_CAPACITY;
    if (ok) strcpy(out, url);
    curl_free(scheme);
    curl_free(host);
    curl_free(url);
    curl_free(port);
    curl_url_cleanup(parsed);
    return ok;
}

static bool same_host(const char *a, const char *b) {
    const char *end_a = a + strcspn(a + 8, "/?") + 8;
    const char *end_b = b + strcspn(b + 8, "/?") + 8;
    return end_a - a == end_b - b && !strncasecmp(a, b, end_a - a);
}

static void read_filename(Transfer *t, const char *value) {
    const char *name = strcasestr(value, "filename=");
    if (!name) return;
    name += strlen("filename=");
    bool quoted = *name == '"';
    if (quoted) ++name;
    size_t n = strcspn(name, quoted ? "\"\r\n" : ";\r\n");
    if (n == 0 || n >= sizeof(t->name) || name[0] == '.') return;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 32 || ch >= 127 || strchr("/\\:<>|?*\"", ch)) return;
    }
    memcpy(t->name, name, n);
    t->name[n] = 0;
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
        t->html[0] = 0;
        t->name[0] = 0;
        t->challenge = t->text = t->html_type = t->attachment = t->binary = false;
    } else if (!strncasecmp(line, "cf-mitigated:", 13)) {
        t->challenge = strcasestr(line, "challenge") != NULL;
    } else if (!strncasecmp(line, "content-disposition:", 20)) {
        t->attachment = strcasestr(line, "attachment") != NULL;
        read_filename(t, line);
    } else if (!strncasecmp(line, "content-type:", 13)) {
        t->html_type = strcasestr(line, "text/html") || strcasestr(line, "application/xhtml");
        t->text = strcasestr(line, "text/") || strcasestr(line, "html") || strcasestr(line, "json");
        t->binary = strcasestr(line, "application/octet-stream") ||
            strcasestr(line, "application/x-rar") || strcasestr(line, "application/vnd.rar") ||
            strcasestr(line, "application/zip") || strcasestr(line, "application/x-7z");
    }
    return n;
}

static bool open_partial(Transfer *t) {
    const char *fallback = t->job->megaup ? "megaup-download.bin" : "1fichier-download.bin";
    const char *name = t->name[0] ? t->name : fallback;
    const char *dir = t->job->megaup_native ? MEGAUP_DIR : OUTPUT_DIR;
    snprintf(t->job->path, sizeof(t->job->path), "%s/%s", dir, name);
    snprintf(t->partial, sizeof(t->partial), "%s/%s.part", dir, name);
    struct stat info;
    if (!stat(t->job->path, &info)) {
        t->error = "Destination file already exists";
        return false;
    }
    int fd = open(t->partial, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        t->error = "Cannot open .part file on SD";
        return false;
    }
    t->file = fdopen(fd, "wb");
    if (!t->file) {
        close(fd);
        unlink(t->partial);
        t->error = "Cannot open output stream";
        return false;
    }
    return true;
}

static size_t on_body(char *data, size_t size, size_t count, void *arg) {
    Transfer *t = arg;
    size_t n = size * count;
    if (atomic_load(&t->job->cancelled)) return 0;
    if (t->job->megaup && t->status >= 300 && t->status < 400) return n;
    bool file_body = t->job->megaup ? !t->html_type && (t->attachment || t->binary) :
        !t->text && (t->attachment || t->binary);
    if (t->allow_file && t->status == 200 && !t->challenge && file_body) {
        if (!t->file && !open_partial(t)) return 0;
        size_t written = fwrite(data, 1, n, t->file);
        atomic_fetch_add(&t->job->received, written);
        if (written != n) t->error = "SD write failed (space or file size limit)";
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

static bool megaup_action_allowed(MegaUpStep page, MegaUpStep action) {
    if (page == MEGAUP_SHARE) return action == MEGAUP_GATE;
    return action == MEGAUP_CONTINUE || action == MEGAUP_FILE;
}

static MegaUpStep megaup_anchor(xmlNodePtr node, const char *base, MegaUpStep page,
                                char next[URL_CAPACITY]) {
    for (; node; node = node->next) {
        if (node_is(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            MegaUpStep step = megaup_url(base, (const char *)href, next);
            xmlFree(href);
            if (megaup_action_allowed(page, step)) return step;
        }
        MegaUpStep child = megaup_anchor(node->children, base, page, next);
        if (child != MEGAUP_INVALID) return child;
    }
    return MEGAUP_INVALID;
}

static MegaUpStep megaup_action(const Transfer *t, const char *base, MegaUpStep page,
                                char next[URL_CAPACITY]) {
    if (!t->length) return MEGAUP_INVALID;
    htmlDocPtr doc = htmlReadMemory(t->html, (int)t->length, NULL, NULL,
        HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return MEGAUP_INVALID;
    MegaUpStep step = megaup_anchor(xmlDocGetRootElement(doc), base, page, next);
    xmlFreeDoc(doc);
    if (step != MEGAUP_INVALID || page != MEGAUP_SHARE) return step;

    // Some share pages place a literal download-button href in their timer script.
    // Parse just that attribute as HTML so any escaped query separators are decoded.
    for (const char *timer = strcasestr(t->html, ".download-timer"); timer;
         timer = strcasestr(timer + 15, ".download-timer")) {
        const char *script_end = strcasestr(timer, "</script");
        const char *href = strcasestr(timer, "href=");
        if (!href || !script_end || href >= script_end) continue;
        const char *value = href + 5;
        char quote = *value++;
        if (quote != '\'' && quote != '"') continue;
        const char *end = strchr(value, quote);
        if (!end || end >= script_end || end - value >= URL_CAPACITY) continue;
        char tag[URL_CAPACITY + 20];
        snprintf(tag, sizeof(tag), "<a %.*s></a>", (int)(end - href + 1), href);
        doc = htmlReadMemory(tag, (int)strlen(tag), NULL, NULL,
            HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
        if (!doc) continue;
        step = megaup_anchor(xmlDocGetRootElement(doc), base, page, next);
        xmlFreeDoc(doc);
        if (step != MEGAUP_INVALID) return step;
    }
    return MEGAUP_INVALID;
}

static const char *megaup_error(const Transfer *t, MegaUpStep step) {
    if (t->challenge || t->status == 401 || t->status == 403)
        return "MegaUp refused the request or requires browser verification";
    if (t->status == 429) return "MegaUp rate limit. Try again later.";
    if (t->status == 404 || t->status == 410 || strcasestr(t->html, "file not found") ||
        strcasestr(t->html, "file was deleted")) {
        return step == MEGAUP_FILE ? "MegaUp file link expired or storage returned not found" :
            "MegaUp reports that the shared file is unavailable";
    }
    if (t->status >= 400) return "MegaUp returned an HTTP error";
    return NULL;
}

static void megaup_wait(FichierJob *job) {
    // Allow the normal page countdown to elapse before requesting its link.
    for (unsigned seconds = 15; seconds && !atomic_load(&job->cancelled); --seconds) {
        atomic_store(&job->waiting, seconds);
        sleep(1);
    }
    atomic_store(&job->waiting, 0);
}

static const char *megaup_stage(MegaUpStep step) {
    switch (step) {
        case MEGAUP_SHARE: return "Share page";
        case MEGAUP_GATE: return "Download page";
        case MEGAUP_CONTINUE: return "Countdown link";
        case MEGAUP_FILE: return "File server";
        default: return "URL validation";
    }
}

void fichier_download(FichierJob *job) {
    job->result = "Download setup failed";
    const char *dir = job->megaup_native ? MEGAUP_DIR : OUTPUT_DIR;
    if (mkdir(dir, 0777) && errno != EEXIST) {
        job->result = "Cannot create download directory on SD";
        return;
    }
    CURL *curl = curl_easy_init();
    xmlBufferPtr data = xmlBufferCreate();
    Transfer t = { .job = job, .html = calloc(HTML_CAPACITY + 1, 1) };
    char url[URL_CAPACITY], next[URL_CAPACITY], referrer[URL_CAPACITY] = {0};
    bool posting = false;
    unsigned forms = 0;
    bool checked_early = false;
    MegaUpStep mega_step = MEGAUP_INVALID;
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Switch-Download-Probe/0.5");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    if (job->megaup_native) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    if (job->megaup) {
        job->stage = megaup_stage(MEGAUP_INVALID);
        mega_step = megaup_url(NULL, job->url, url);
        if (mega_step == MEGAUP_INVALID || (!job->megaup_native && mega_step != MEGAUP_FILE)) {
            job->result = "Enter a valid HTTPS MegaUp file link";
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
    char initial_url[URL_CAPACITY];
    strcpy(initial_url, url);

    for (unsigned hop = 0; hop < 10 && !atomic_load(&job->cancelled); ++hop) {
        if (job->megaup) {
            job->stage = megaup_stage(mega_step);
            t.allow_file = mega_step == MEGAUP_FILE;
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
        if (t.file) {
            bool closed = fclose(t.file) == 0;
            t.file = NULL;
            if (code == CURLE_OK && closed && !atomic_load(&job->cancelled)) {
                if (!rename(t.partial, job->path)) {
                    job->result = "Download saved";
                    goto done;
                }
                t.error = "Could not finalize file on SD";
            }
            unlink(t.partial);
            job->result = t.error ? t.error : closed ? curl_easy_strerror(code) : "SD flush failed";
            goto done;
        }
        if (code != CURLE_OK || t.error) {
            job->result = t.error ? t.error : curl_easy_strerror(code);
            goto done;
        }
        if (job->megaup) {
            const char *error = megaup_error(&t, mega_step);
            if (error) {
                job->result = error;
                goto done;
            }
            char *location = NULL;
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
            bool redirect = t.status >= 300 && t.status < 400 && location;
            MegaUpStep next_step = redirect ? megaup_url(url, location, next) :
                (mega_step == MEGAUP_FILE ? MEGAUP_INVALID :
                 megaup_action(&t, url, mega_step, next));
            if (!redirect && mega_step == MEGAUP_CONTINUE && t.status == 200 &&
                (next_step == MEGAUP_INVALID || !strcmp(url, next)) && !checked_early) {
                checked_early = true;
                megaup_wait(job);
                continue;
            }
            if (next_step == MEGAUP_INVALID || !strcmp(url, next)) {
                job->result = redirect ? "Unexpected MegaUp redirect" :
                    mega_step == MEGAUP_FILE ? "File server did not send a downloadable file" :
                    mega_step == MEGAUP_CONTINUE ? "MegaUp has not released the download link" :
                    "No download link found on this MegaUp page";
                goto done;
            }
            if (!redirect) megaup_wait(job);
            strcpy(referrer, url);
            strcpy(url, next);
            mega_step = next_step;
            job->result = "MegaUp reached the request limit";
            continue;
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
    if (atomic_load(&job->cancelled)) job->result = "Download cancelled";
    curl_easy_cleanup(curl);
    xmlBufferFree(data);
    free(t.html);
}
