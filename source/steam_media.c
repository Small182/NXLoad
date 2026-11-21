#include "steam_media.h"
#include "json.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Nintendo's DNS.mitm rules can block *.nintendo.net on an emuMMC. Steam's
 * storefront and media CDNs are independent of Nintendo's domains. */
#define STEAM_SEARCH "https://store.steampowered.com/api/storesearch/?term="
#define STEAM_DETAILS "https://store.steampowered.com/api/appdetails?appids="

static void name_key(const char *name, char out[CATALOG_TEXT]) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && n + 1 < CATALOG_TEXT; ++p) {
        if (*p < 128) {
            if (isalnum(*p)) out[n++] = (char)tolower(*p);
        } else {
            out[n++] = (char)*p;
        }
    }
    out[n] = 0;
}

static bool steam_screenshot(const char *url) {
    if (!url || strlen(url) >= CATALOG_URL) return false;
    static const char *hosts[] = {
        "https://shared.akamai.steamstatic.com/",
        "https://shared.fastly.steamstatic.com/",
        "https://shared.cloudflare.steamstatic.com/",
        "https://cdn.akamai.steamstatic.com/",
        "https://cdn.fastly.steamstatic.com/",
    };
    bool trusted = false;
    for (size_t i = 0; i < sizeof(hosts) / sizeof(*hosts); ++i)
        if (!strncmp(url, hosts[i], strlen(hosts[i]))) trusted = true;
    if (!trusted) return false;
    const char *filename = strrchr(url, '/');
    if (!filename) return false;
    const char *query = strchr(filename, '?');
    size_t length = query ? (size_t)(query - filename) : strlen(filename);
    return length >= 4 &&
        (!strncasecmp(filename + length - 4, ".jpg", 4) ||
         !strncasecmp(filename + length - 4, ".png", 4));
}

/* Steam API may supply http or https and add a volatile ?t= parameter.
 * Normalize to an HTTPS MP4 that the native range player can validate. */
static bool steam_movie(const char *url, char result[CATALOG_URL]) {
    static const char *hosts[] = {
        "video.akamai.steamstatic.com",
        "video.cloudflare.steamstatic.com",
        "video.fastly.steamstatic.com",
    };
    if (!url) return false;
    const char *p = url;
    if (!strncmp(p, "https://", 8)) p += 8;
    else if (!strncmp(p, "http://", 7)) p += 7;
    else return false;
    const char *selected = NULL;
    for (size_t i = 0; i < sizeof(hosts) / sizeof(*hosts); ++i) {
        size_t len = strlen(hosts[i]);
        if (!strncmp(p, hosts[i], len) && p[len] == '/') {
            selected = hosts[i];
            p += len;
            break;
        }
    }
    if (!selected) return false;
    static const char stem[] = "/store_trailers/";
    if (strncmp(p, stem, sizeof(stem) - 1)) return false;
    p += sizeof(stem) - 1;
    size_t digits = strspn(p, "0123456789");
    if (digits < 5 || digits > 16 ||
        strncmp(p + digits, "/movie480.mp4", sizeof("/movie480.mp4") - 1)) return false;
    const char *suffix = p + digits + sizeof("/movie480.mp4") - 1;
    if (*suffix && *suffix != '?') return false;
    int n = snprintf(result, CATALOG_URL, "https://%s/store_trailers/%.*s/movie480.mp4",
                     selected, (int)digits, p);
    return n > 0 && n < CATALOG_URL;
}

static bool app_id(Json item, char result[24]) {
    Json id = field(item, "id");
    if (!id.begin) return false;
    size_t length = (size_t)(id.end - id.begin);
    if (!length || length > 10 || strspn(id.begin, "0123456789") != length)
        return false;
    memcpy(result, id.begin, length);
    result[length] = 0;
    return true;
}

bool steam_media_lookup(const char *title, atomic_bool *cancel, SteamMedia *media) {
    if (!title || !title[0] || atomic_load(cancel)) return false;
    char wanted[CATALOG_TEXT];
    name_key(title, wanted);
    if (!wanted[0]) return false;

    CURL *curl = curl_easy_init();
    if (!curl) {
        media->curl_error = CURLE_FAILED_INIT;
        return false;
    }
    char *encoded = curl_easy_escape(curl, title, 0);
    if (!encoded) {
        curl_easy_cleanup(curl);
        return false;
    }
    char url[CATALOG_URL];
    int n = snprintf(url, sizeof(url), STEAM_SEARCH "%s&l=english&cc=US", encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    if (n <= 0 || n >= (int)sizeof(url)) return false;

    CatalogFetchStatus status = {0};
    size_t bytes = 0;
    unsigned char *body = catalog_fetch_document(url, cancel, &bytes, &status);
    media->curl_error = status.curl;
    media->http_error = status.http;
    if (!body || atomic_load(cancel)) {
        free(body);
        return false;
    }

    Json search = {(char *)body, (char *)body + bytes};
    Json items = field(search, "items");
    const char *cursor = NULL;
    char id[24] = {0}, name[CATALOG_TEXT], candidate[CATALOG_TEXT];
    for (;;) {
        Json item = next_object(items, &cursor);
        if (!item.begin) break;
        if (!string_field(item, "name", name, sizeof(name))) continue;
        name_key(name, candidate);
        if (strcmp(candidate, wanted)) continue;
        if (app_id(item, id)) break;
    }
    free(body);
    if (!id[0] || atomic_load(cancel)) return false;

    snprintf(url, sizeof(url), STEAM_DETAILS "%s&cc=US&l=en", id);
    bytes = 0;
    body = catalog_fetch_document(url, cancel, &bytes, &status);
    media->curl_error = status.curl;
    media->http_error = status.http;
    if (!body || atomic_load(cancel)) {
        free(body);
        return false;
    }
    Json root = {(char *)body, (char *)body + bytes};
    Json app = field(root, id);
    Json data = field(app, "data");
    char confirmed[CATALOG_TEXT], confirmed_key[CATALOG_TEXT];
    if (!literal(field(app, "success"), "true") ||
        !string_field(data, "name", confirmed, sizeof(confirmed))) {
        free(body);
        return false;
    }
    name_key(confirmed, confirmed_key);
    if (strcmp(confirmed_key, wanted)) {
        free(body);
        return false;
    }
    media->matched = true;

    Json screenshots = field(data, "screenshots");
    cursor = NULL;
    while (media->screenshot_count < CATALOG_SCREENSHOTS) {
        Json shot = next_object(screenshots, &cursor);
        if (!shot.begin) break;
        char image[CATALOG_URL];
        if ((!string_field(shot, "path_full", image, sizeof(image)) ||
             !steam_screenshot(image)) &&
            (!string_field(shot, "path_thumbnail", image, sizeof(image)) ||
             !steam_screenshot(image))) continue;
        strcpy(media->screenshots[media->screenshot_count++], image);
    }
    Json movies = field(data, "movies");
    cursor = NULL;
    while (media->trailer_count < TRAILER_LIMIT) {
        Json movie = next_object(movies, &cursor);
        if (!movie.begin) break;
        char source[CATALOG_URL], normalized[CATALOG_URL];
        if (!string_field(field(movie, "mp4"), "480", source, sizeof(source)) ||
            !steam_movie(source, normalized)) continue;
        bool duplicate = false;
        for (size_t i = 0; i < media->trailer_count; ++i)
            if (!strcmp(media->trailers[i], normalized)) duplicate = true;
        if (!duplicate) strcpy(media->trailers[media->trailer_count++], normalized);
    }
    free(body);
    return true;
}
