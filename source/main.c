#define _GNU_SOURCE
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define ORIGIN "https://buzzheavier.com"
#define FILE_ID "kku84raex2i9"
#define SHARE_URL ORIGIN "/" FILE_ID
#define DOWNLOAD_PATH "/" FILE_ID "/download"
#define FICHIER_SHARE_URL "https://1fichier.com/?u5n5c73gd1sgod2gegpx"
#define TITLE "My Cozy Room"
#define APP_HEADING "Switch Download Probe 0.2\n" TITLE
#define CONTROLS "A: Buzzheavier    X: 1Fichier    +: exit\n"
#define HTML_LIMIT (256 * 1024)
#define URL_LIMIT 2048

typedef struct {
    char *body;
    size_t size;
    char redirect[URL_LIMIT];
    int challenge;
    int attachment;
} Response;

typedef struct {
    int fichier;
    long web_status;
    long button_status;
    CURLcode curl_error;
    const char *summary;
    char host[128];
} Probe;

static size_t receive_body(char *data, size_t size, size_t count, void *arg) {
    Response *response = arg;
    size_t bytes = size * count;
    if (response->attachment || !response->body || bytes > HTML_LIMIT - response->size) return 0;
    memcpy(response->body + response->size, data, bytes);
    response->size += bytes;
    response->body[response->size] = 0;
    return bytes;
}

static size_t receive_header(char *line, size_t size, size_t count, void *arg) {
    Response *response = arg;
    size_t bytes = size * count;
    const char name[] = "HX-Redirect:";
    const char attachment[] = "Content-Disposition: attachment";
    if (bytes >= 5 && !memcmp(line, "HTTP/", 5)) {
        response->redirect[0] = 0;
        response->challenge = 0;
        response->attachment = 0;
    }
    if (bytes >= sizeof(attachment) - 1 && !strncasecmp(line, attachment, sizeof(attachment) - 1)) {
        response->attachment = 1;
    }
    if (bytes >= 23 && !strncasecmp(line, "cf-mitigated: challenge", 23)) {
        response->challenge = 1;
    }
    if (bytes >= sizeof(name) - 1 && !strncasecmp(line, name, sizeof(name) - 1)) {
        const char *start = line + sizeof(name) - 1;
        const char *end = line + bytes;
        while (start < end && isspace((unsigned char)*start)) ++start;
        while (end > start && isspace((unsigned char)end[-1])) --end;
        if ((size_t)(end - start) < sizeof(response->redirect)) {
            memcpy(response->redirect, start, end - start);
            response->redirect[end - start] = 0;
        }
    }
    return bytes;
}

static int button_url(const char *html, char out[URL_LIMIT]) {
    const char marker[] = DOWNLOAD_PATH "?t=";
    const char *path = strstr(html, marker);
    if (!path) return 0;
    const char *token = path + sizeof(marker) - 1;
    size_t n = strspn(token, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~-%=+/");
    if (!n || n + sizeof(ORIGIN) + sizeof(marker) >= URL_LIMIT) return 0;
    if (token[n] && !strchr("\"'<> \t\r\n),", token[n])) return 0;
    snprintf(out, URL_LIMIT, "%s%s%.*s", ORIGIN, marker, (int)n, token);
    return 1;
}

static int issued_host(const char *redirect, char host[128]) {
    CURLU *url = curl_url();
    if (!url) return 0;
    char *scheme = NULL, *domain = NULL, *path = NULL, *query = NULL;
    int ok = !curl_url_set(url, CURLUPART_URL, redirect, CURLU_DISALLOW_USER) &&
        !curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) &&
        !curl_url_get(url, CURLUPART_HOST, &domain, 0) &&
        !curl_url_get(url, CURLUPART_PATH, &path, 0) &&
        !curl_url_get(url, CURLUPART_QUERY, &query, 0);
    if (ok) {
        size_t length = strlen(domain);
        const char suffix[] = ".buzzheavier.com";
        ok = !strcmp(scheme, "https") && length < 128 &&
            (!strcasecmp(domain, "buzzheavier.com") ||
                (length >= sizeof(suffix) && !strcasecmp(domain + length - sizeof(suffix) + 1, suffix))) &&
            !strcmp(path, "/d/" FILE_ID) && !strncmp(query, "v=", 2) && query[2];
        if (ok) memcpy(host, domain, length + 1);
    }
    curl_free(scheme);
    curl_free(domain);
    curl_free(path);
    curl_free(query);
    curl_url_cleanup(url);
    return ok;
}

static int challenged(const Response *response, long status) {
    return response->challenge ||
        (status == 403 && strcasestr(response->body, "Just a moment"));
}

static int fichier_free_form(const char *body) {
    return strcasestr(body, "<form") && strcasestr(body, "free download");
}

static int fichier_host(const char *location, char host[128]) {
    if (!location) return 0;
    CURLU *url = curl_url();
    if (!url) return 0;
    char *scheme = NULL, *domain = NULL;
    int ok = !curl_url_set(url, CURLUPART_URL, location, CURLU_DISALLOW_USER) &&
        !curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) &&
        !curl_url_get(url, CURLUPART_HOST, &domain, 0);
    if (ok) {
        size_t length = strlen(domain);
        const char *suffix = strrchr(domain, '.');
        while (suffix && suffix > domain && suffix[-1] != '.') --suffix;
        ok = !strcmp(scheme, "https") && length < 128 && suffix && suffix > domain &&
            (!strcasecmp(suffix, "1fichier.com") || !strcasecmp(suffix, "desfichiers.com"));
        if (ok) memcpy(host, domain, length + 1);
    }
    curl_free(scheme);
    curl_free(domain);
    curl_url_cleanup(url);
    return ok;
}

static void probe_fichier_form(CURL *curl, Response *response, Probe *result) {
    if (strcasestr(response->body, "Site Unavailable")) {
        result->summary = "1Fichier: Site Unavailable (HTML response)";
        return;
    }
    if (!fichier_free_form(response->body)) {
        result->summary = "1Fichier: free download form not found";
        return;
    }

    response->size = 0;
    response->body[0] = 0;
    curl_easy_setopt(curl, CURLOPT_REFERER, FICHIER_SHARE_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    sleep(2);
    result->curl_error = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->button_status);
    char *location = NULL;
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
    if (challenged(response, result->button_status)) {
        result->summary = "1Fichier free form: Cloudflare challenge";
    } else if (response->attachment) {
        result->curl_error = CURLE_OK;
        result->summary = "1Fichier free form: file response received";
    } else if (result->curl_error) {
        result->summary = "1Fichier free form: request failed";
    } else if (fichier_host(location, result->host)) {
        result->summary = "1Fichier free form: HTTPS redirect received";
    } else if (strcasestr(response->body, "show the download link")) {
        result->summary = "1Fichier: next download form received";
    } else if (strcasestr(response->body, "var count") ||
               strcasestr(response->body, "you must wait") ||
               strcasestr(response->body, "too fast")) {
        result->summary = "1Fichier: wait or rate limit reported";
    } else {
        result->summary = "1Fichier free form: no download step recognized";
    }
}

static void run_probe(Probe *result) {
    CURL *curl = curl_easy_init();
    Response response = { .body = calloc(HTML_LIMIT + 1, 1) };
    struct curl_slist *headers = NULL;
    int fichier = result->fichier;
    *result = (Probe){ .fichier = fichier, .summary = "Unable to initialize HTTP client" };
    if (!curl || !response.body) goto cleanup;

    curl_easy_setopt(curl, CURLOPT_URL, fichier ? FICHIER_SHARE_URL : SHARE_URL);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Switch-Download-Probe/0.2");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 9000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);

    result->curl_error = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->web_status);
    if (challenged(&response, result->web_status)) {
        result->summary = "Share page: Cloudflare challenge";
        goto cleanup;
    }
    if (response.attachment) {
        result->curl_error = CURLE_OK;
        result->summary = "Share page: file response received";
        goto cleanup;
    }
    if (result->curl_error || result->web_status != 200) {
        result->summary = "Share page: request failed";
        goto cleanup;
    }

    if (fichier) {
        probe_fichier_form(curl, &response, result);
        goto cleanup;
    }

    char url[URL_LIMIT];
    if (!button_url(response.body, url)) {
        result->summary = "Share page: signed Download File action unavailable in HTML";
        goto cleanup;
    }

    response.size = 0;
    response.body[0] = 0;
    headers = curl_slist_append(NULL, "HX-Request: true");
    if (!headers) goto cleanup;
    struct curl_slist *next = curl_slist_append(headers, "HX-Current-URL: " SHARE_URL);
    if (!next) goto cleanup;
    headers = next;
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_REFERER, SHARE_URL);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    result->curl_error = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->button_status);
    if (challenged(&response, result->button_status)) {
        result->summary = "Download action: Cloudflare challenge";
    } else if (result->curl_error || result->button_status != 204) {
        result->summary = "Download action: expected HTTP 204";
    } else if (!issued_host(response.redirect, result->host)) {
        result->summary = response.redirect[0] ?
            "HTTP 204: unexpected HX-Redirect target" : "HTTP 204: HX-Redirect missing";
    } else {
        result->summary = "PASS: HTTP 204 + signed HX-Redirect";
    }

cleanup:
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(response.body);
}

static void print_result(const Probe *result) {
    printf("Share page HTTP:      %ld\n", result->web_status);
    printf("Download action HTTP: %ld\n", result->button_status);
    if (result->curl_error) printf("libcurl: %s\n", curl_easy_strerror(result->curl_error));
    printf("\n%s\n", result->summary);
    if (result->host[0]) printf("File host: %s\n", result->host);
    printf("\nArchive transfer skipped.\n");
}

#ifdef __SWITCH__
static void worker(void *arg) {
    run_probe(arg);
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Result sockets = socketInitializeDefault();
    CURLcode curl_init = R_SUCCEEDED(sockets) ?
        curl_global_init(CURL_GLOBAL_DEFAULT) : CURLE_FAILED_INIT;
    printf(APP_HEADING "\n\n");
    if (R_FAILED(sockets)) printf("Socket init failed: 0x%08x\n", (unsigned)sockets);
    else if (curl_init) printf("HTTP init failed: %s\n", curl_easy_strerror(curl_init));
    else printf(CONTROLS);

    Thread thread;
    int busy = 0;
    Probe result;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 buttons = padGetButtonsDown(&pad);
        if (!busy && (buttons & HidNpadButton_Plus)) break;
        if (!busy && !curl_init && (buttons & (HidNpadButton_A | HidNpadButton_X))) {
            consoleClear();
            result = (Probe){ .fichier = !(buttons & HidNpadButton_A) };
            printf(APP_HEADING " | %s\n\nChecking over HTTPS...\n",
                   result.fichier ? "1Fichier" : "Buzzheavier");
            Result status = threadCreate(&thread, worker, &result, NULL, 128 * 1024, 0x2c, -2);
            if (R_SUCCEEDED(status)) {
                status = threadStart(&thread);
                if (R_SUCCEEDED(status)) busy = 1;
                else threadClose(&thread);
            }
            if (!busy) printf("Thread error: 0x%08x\n" CONTROLS, (unsigned)status);
        }
        if (busy && R_SUCCEEDED(waitSingle(waiterForThread(&thread), 0))) {
            threadWaitForExit(&thread);
            threadClose(&thread);
            busy = 0;
            consoleClear();
            printf(APP_HEADING " | %s\n\n", result.fichier ? "1Fichier" : "Buzzheavier");
            print_result(&result);
            printf("\n" CONTROLS);
        }
        consoleUpdate(NULL);
    }
    if (busy) {
        threadWaitForExit(&thread);
        threadClose(&thread);
    }
    if (!curl_init) curl_global_cleanup();
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    return 0;
}
#else
int main(int argc, char **argv) {
    CURLcode init = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (init) return 1;
    Probe result = { .fichier = argc > 1 && !strcmp(argv[1], "1fichier") };
    run_probe(&result);
    print_result(&result);
    curl_global_cleanup();
    return result.host[0] ? 0 : 1;
}
#endif
