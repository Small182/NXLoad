#define _GNU_SOURCE
#include "fichier.h"

#include <curl/curl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define ORIGIN "https://buzzheavier.com"
#define FILE_ID "kku84raex2i9"
#define SHARE_URL ORIGIN "/" FILE_ID
#define DOWNLOAD_PATH "/" FILE_ID "/download"
#define TITLE "My Cozy Room"
#define APP_HEADING "Switch Download Probe 0.4"
#define CONTROLS "X: A Pretty Odd Bunny    Y: enter 1Fichier link\nA: Buzzheavier check     +: exit\n"
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

static void run_probe(Probe *result) {
    CURL *curl = curl_easy_init();
    Response response = { .body = calloc(HTML_LIMIT + 1, 1) };
    struct curl_slist *headers = NULL;
    *result = (Probe){ .summary = "Unable to initialize HTTP client" };
    if (!curl || !response.body) goto cleanup;

    curl_easy_setopt(curl, CURLOPT_URL, SHARE_URL);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Switch-Download-Probe/0.4");
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
static void probe_worker(void *arg) {
    run_probe(arg);
}

static void download_worker(void *arg) {
    fichier_download(arg);
}

static bool enter_fichier_url(char url[FICHIER_INPUT_CAPACITY]) {
    SwkbdConfig keyboard;
    char input[FICHIER_INPUT_CAPACITY] = {0};
    Result rc = swkbdCreate(&keyboard, 0);
    if (R_FAILED(rc)) {
        printf("Keyboard unavailable: 0x%08x\n", (unsigned)rc);
        return false;
    }

    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, "1Fichier");
    swkbdConfigSetGuideText(&keyboard, "Enter a 1Fichier link or just the file ID");
    swkbdConfigSetOkButtonText(&keyboard, "Start");
    swkbdConfigSetStringLenMax(&keyboard, FICHIER_INPUT_CAPACITY - 1);
    swkbdConfigSetReturnButtonFlag(&keyboard, false);
    rc = swkbdShow(&keyboard, input, sizeof(input));
    swkbdClose(&keyboard);
    if (R_FAILED(rc) || !input[0]) return false;

    input[strcspn(input, "\r\n")] = 0;
    char *code = input;
    while (isspace((unsigned char)*code)) ++code;
    const char *prefixes[] = {
        "https://1fichier.com/?", "https://www.1fichier.com/?",
        "1fichier.com/?", "www.1fichier.com/?"
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t n = strlen(prefixes[i]);
        if (!strncasecmp(code, prefixes[i], n)) {
            code += n;
            break;
        }
    }
    size_t length = strspn(code, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    char *end = code + length;
    while (isspace((unsigned char)*end)) ++end;
    if (length < 6 || length > 64 || (*end && *end != '&')) {
        printf("Expected a 1Fichier URL or file ID.\n");
        return false;
    }
    snprintf(url, FICHIER_INPUT_CAPACITY, "https://1fichier.com/?%.*s", (int)length, code);
    return true;
}

static void show_download(const FichierJob *job) {
    uint_fast64_t received = atomic_load(&job->received);
    uint_fast64_t total = atomic_load(&job->total);
    unsigned waiting = atomic_load(&job->waiting);
    consoleClear();
    printf(APP_HEADING " | 1Fichier\n\n");
    if (waiting) printf("Host wait: %u s\n", waiting);
    else if (received) {
        printf("Received: %" PRIuFAST64 " bytes\n", received);
        if (total) printf("Total:    %" PRIuFAST64 " bytes\n", total);
    } else {
        printf("Looking up the download over HTTPS...\n");
    }
    printf("\nB: cancel\n");
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
    bool busy = false, downloading = false;
    unsigned frame = 0;
    Probe result;
    char entered_url[FICHIER_INPUT_CAPACITY] = {0};
    FichierJob job = { .url = BUNNY_NSWGF_URL };
    atomic_init(&job.cancelled, false);
    atomic_init(&job.received, 0);
    atomic_init(&job.total, 0);
    atomic_init(&job.waiting, 0);
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 buttons = padGetButtonsDown(&pad);
        if (!busy && (buttons & HidNpadButton_Plus)) break;
        if (busy && downloading && (buttons & HidNpadButton_B)) {
            atomic_store(&job.cancelled, true);
        }
        if (!busy && !curl_init && (buttons & (HidNpadButton_A | HidNpadButton_X | HidNpadButton_Y))) {
            downloading = !(buttons & HidNpadButton_A);
            consoleClear();
            if (downloading) {
                job.url = BUNNY_NSWGF_URL;
                if (buttons & HidNpadButton_Y) {
                    if (!enter_fichier_url(entered_url)) {
                        printf("\n" CONTROLS);
                        consoleUpdate(NULL);
                        continue;
                    }
                    job.url = entered_url;
                }
                atomic_store(&job.cancelled, false);
                atomic_store(&job.received, 0);
                atomic_store(&job.total, 0);
                atomic_store(&job.waiting, 0);
                job.path[0] = 0;
                job.http = 0;
                show_download(&job);
            } else {
                printf(APP_HEADING " | " TITLE " / Buzzheavier\n\nChecking over HTTPS...\n");
            }
            ThreadFunc task = downloading ? download_worker : probe_worker;
            void *arg = downloading ? (void *)&job : (void *)&result;
            Result status = threadCreate(&thread, task, arg, NULL, 128 * 1024, 0x2c, -2);
            if (R_SUCCEEDED(status)) {
                status = threadStart(&thread);
                if (R_SUCCEEDED(status)) busy = true;
                else threadClose(&thread);
            }
            if (!busy) printf("Thread error: 0x%08x\n" CONTROLS, (unsigned)status);
        }
        if (busy && R_SUCCEEDED(waitSingle(waiterForThread(&thread), 0))) {
            threadWaitForExit(&thread);
            threadClose(&thread);
            busy = false;
            consoleClear();
            if (downloading) {
                printf(APP_HEADING " | 1Fichier\n\nHTTP: %ld\n%s\n", job.http, job.result);
                printf("Received: %" PRIuFAST64 " bytes\n", atomic_load(&job.received));
                if (!strcmp(job.result, "Download saved")) printf("\n%s\n", job.path);
            } else {
                printf(APP_HEADING " | " TITLE " / Buzzheavier\n\n");
                print_result(&result);
            }
            printf("\n" CONTROLS);
        } else if (busy && downloading && ++frame % 30 == 0) {
            show_download(&job);
        }
        consoleUpdate(NULL);
    }
    if (busy) {
        if (downloading) atomic_store(&job.cancelled, true);
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
    if (argc > 1 && !strcmp(argv[1], "1fichier")) {
        FichierJob job = { .url = argc > 2 ? argv[2] : BUNNY_NSWGF_URL };
        atomic_init(&job.cancelled, false);
        atomic_init(&job.received, 0);
        atomic_init(&job.total, 0);
        atomic_init(&job.waiting, 0);
        fichier_download(&job);
        printf("HTTP: %ld\n%s\nReceived: %" PRIuFAST64 " bytes\n",
               job.http, job.result, atomic_load(&job.received));
        curl_global_cleanup();
        return strcmp(job.result, "Download saved") != 0;
    }
    Probe result;
    run_probe(&result);
    print_result(&result);
    curl_global_cleanup();
    return result.host[0] ? 0 : 1;
}
#endif
