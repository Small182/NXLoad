#define _GNU_SOURCE
#include "catalog.h"

#include <ctype.h>
#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/uri.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SITE "https://nsroms.net"
#define LIST_URL SITE "/roms/"
#define DRIVE_PREFIX "https://drive.dlarchive.my.id/dl/?id="
#define PAGE_BYTES (4 * 1024 * 1024)
#define COVER_BYTES (2 * 1024 * 1024)

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} Buffer;

static size_t receive(void *data, size_t width, size_t count, void *ptr) {
    Buffer *buf = ptr;
    size_t bytes = width * count;
    if (bytes > buf->capacity - buf->size) return 0;
    memcpy(buf->data + buf->size, data, bytes);
    buf->size += bytes;
    return bytes;
}

static int should_cancel(void *ptr, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    return atomic_load((atomic_bool *)ptr);
}

static Buffer fetch(atomic_bool *cancel, const char *url, size_t limit, long *status) {
    Buffer buf = { .data = malloc(limit + 1), .capacity = limit };
    if (!buf.data) return (Buffer){0};
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(buf.data);
        return (Buffer){0};
    }
    *status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 16L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NXLoad/1.0.2");
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, should_cancel);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || *status != 200 || !buf.size) {
        free(buf.data);
        return (Buffer){0};
    }
    buf.data[buf.size] = 0;
    return buf;
}

unsigned char *catalog_fetch_cover(const char *url, atomic_bool *cancel, size_t *bytes) {
    long status = 0;
    Buffer image = fetch(cancel, url, COVER_BYTES, &status);
    *bytes = image.size;
    return image.data;
}

static htmlDocPtr fetch_page(CatalogJob *job, const char *url) {
    Buffer buf = fetch(&job->cancel, url, PAGE_BYTES, &job->http);
    if (!buf.data) return NULL;
    htmlDocPtr doc = htmlReadMemory((char *)buf.data, (int)buf.size, url, "UTF-8",
        HTML_PARSE_NONET | HTML_PARSE_NOWARNING | HTML_PARSE_NOERROR);
    free(buf.data);
    return doc;
}

static bool is_element(xmlNodePtr node, const char *name) {
    return node->type == XML_ELEMENT_NODE && !xmlStrcasecmp(node->name, BAD_CAST name);
}

static void copy_text(char *out, size_t capacity, const xmlChar *text) {
    size_t n = 0;
    bool space = false;
    if (!text) return;
    const unsigned char *p = text;
    while (*p) {
        if (*p < 0x80 && isspace(*p)) {
            space = n != 0;
            ++p;
            continue;
        }
        size_t width = *p < 0x80 ? 1 : *p < 0xe0 ? 2 : *p < 0xf0 ? 3 : 4;
        size_t i = 1;
        while (i < width && p[i] && (p[i] & 0xc0) == 0x80) ++i;
        if (i != width || n + width + space >= capacity) break;
        if (space) out[n++] = ' ';
        memcpy(out + n, p, width);
        n += width;
        p += width;
        space = false;
    }
    out[n] = 0;
}

static void node_text(xmlNodePtr node, char out[CATALOG_TEXT]) {
    xmlChar *content = xmlNodeGetContent(node);
    copy_text(out, CATALOG_TEXT, content);
    xmlFree(content);
}

static xmlNodePtr find_tag(xmlNodePtr node, const char *name) {
    for (; node; node = node->next) {
        if (is_element(node, name)) return node;
        xmlNodePtr nested = find_tag(node->children, name);
        if (nested) return nested;
    }
    return NULL;
}

static bool site_url(const xmlChar *href, char out[CATALOG_URL]) {
    if (!href) return false;
    xmlChar *url = xmlBuildURI(href, BAD_CAST SITE "/");
    bool valid = url && !strncmp((char *)url, SITE "/", sizeof(SITE)) &&
        strlen((char *)url) < CATALOG_URL;
    if (valid) strcpy(out, (char *)url);
    xmlFree(url);
    return valid;
}

static bool game_page(const xmlChar *href, char out[CATALOG_URL]) {
    if (!site_url(href, out) || strncmp(out, LIST_URL, sizeof(LIST_URL) - 1)) return false;
    char *slug = out + sizeof(LIST_URL) - 1;
    size_t n = strspn(slug, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789%-_");
    if (!n || (slug[n] && strcmp(slug + n, "/"))) return false;
    if (!slug[n]) {
        if ((size_t)(slug - out) + n + 1 >= CATALOG_URL) return false;
        strcpy(slug + n, "/");
    }
    return true;
}

static void read_cover(xmlNodePtr img, char out[CATALOG_URL]) {
    if (!img) return;
    const char *keys[] = {"data-src", "data-lazy-src", "data-srcset", "srcset", "src"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        xmlChar *value = xmlGetProp(img, BAD_CAST keys[i]);
        if (value && strstr(keys[i], "srcset")) {
            size_t first = strcspn((char *)value, " \t,\r\n");
            value[first] = 0;
        }
        bool ok = value && *value && site_url(value, out);
        xmlFree(value);
        if (ok) return;
    }
}

static void read_game(xmlNodePtr anchor, Game *game) {
    xmlNodePtr heading = find_tag(anchor->children, "h3");
    if (!heading) heading = find_tag(anchor->children, "h2");
    if (!heading) heading = find_tag(anchor->children, "h4");
    if (heading) node_text(heading, game->title);
    xmlNodePtr img = find_tag(anchor->children, "img");
    read_cover(img, game->cover);
    if (!game->title[0] && img) {
        xmlChar *alt = xmlGetProp(img, BAD_CAST "alt");
        copy_text(game->title, sizeof(game->title), alt);
        xmlFree(alt);
    }
    if (!game->title[0]) node_text(anchor, game->title);
    if (!game->title[0]) {
        const char *slug = game->page + sizeof(LIST_URL) - 1;
        size_t n = strcspn(slug, "/");
        if (n >= sizeof(game->title)) n = sizeof(game->title) - 1;
        for (size_t i = 0; i < n; ++i) game->title[i] = slug[i] == '-' ? ' ' : slug[i];
    }
}

static bool contains(const Game *games, size_t count, const char *url) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(games[i].page, url)) return true;
    return false;
}

static void read_catalog(xmlNodePtr node, Game *games, size_t *count) {
    for (; node && *count < CATALOG_PAGE_GAMES; node = node->next) {
        if (is_element(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            Game game = {0};
            bool ok = game_page(href, game.page);
            xmlFree(href);
            if (ok && !contains(games, *count, game.page)) {
                read_game(node, &game);
                if (game.title[0]) games[(*count)++] = game;
            }
        }
        read_catalog(node->children, games, count);
    }
}

static unsigned declared_pages(xmlDocPtr doc) {
    xmlNodePtr title = find_tag(xmlDocGetRootElement(doc), "title");
    if (!title) return 0;
    char text[CATALOG_TEXT] = {0};
    node_text(title, text);
    const char *page = strcasestr(text, " of ");
    unsigned count = page ? (unsigned)strtoul(page + 4, NULL, 10) : 0;
    return count <= CATALOG_MAX_PAGES ? count : 0;
}

static bool load_page(CatalogJob *job, unsigned page, Game *games, size_t *count,
                      unsigned *pages) {
    char url[CATALOG_URL] = LIST_URL;
    if (page > 1) snprintf(url, sizeof(url), LIST_URL "page/%u/", page);
    htmlDocPtr doc = fetch_page(job, url);
    if (!doc) return false;
    *count = 0;
    read_catalog(xmlDocGetRootElement(doc), games, count);
    if (!*pages) *pages = declared_pages(doc);
    xmlFreeDoc(doc);
    return true;
}

static void read_files(xmlNodePtr node, GameDetails *details) {
    for (; node; node = node->next) {
        if (is_element(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            if (href && !strncmp((char *)href, DRIVE_PREFIX, sizeof(DRIVE_PREFIX) - 1)) {
                const char *id = (char *)href + sizeof(DRIVE_PREFIX) - 1;
                const size_t n = strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
                bool unique = n && n < DLARCHIVE_ID_CAPACITY && (!id[n] || id[n] == '&');
                for (size_t i = 0; unique && i < details->count; ++i)
                    if (strlen(details->files[i].id) == n && !memcmp(details->files[i].id, id, n)) unique = false;
                if (unique && details->count < CATALOG_FILE_COUNT) {
                    GameFile *file = &details->files[details->count++];
                    memcpy(file->id, id, n);
                    file->id[n] = 0;
                    node_text(node, file->title);
                    if (!file->title[0]) strcpy(file->title, "Файл DLArchive");
                }
            }
            xmlFree(href);
        }
        read_files(node->children, details);
    }
}

static void load_details(CatalogJob *job) {
    GameDetails *details = &job->details;
    strcpy(details->title, job->game.title);
    char cover[CATALOG_URL];
    strcpy(cover, job->game.cover);
    htmlDocPtr doc = fetch_page(job, job->game.page);
    if (doc) {
        xmlNodePtr root = xmlDocGetRootElement(doc);
        read_files(root, details);
        xmlNodePtr heading = find_tag(root, "h1");
        if (heading) node_text(heading, details->title);
        xmlNodePtr head = find_tag(root, "head");
        for (xmlNodePtr node = head ? head->children : NULL; node; node = node->next) {
            if (!is_element(node, "meta")) continue;
            xmlChar *property = xmlGetProp(node, BAD_CAST "property");
            xmlChar *value = xmlGetProp(node, BAD_CAST "content");
            if (!xmlStrcmp(property, BAD_CAST "og:image")) site_url(value, cover);
            if (!xmlStrcmp(property, BAD_CAST "og:description"))
                copy_text(details->info, sizeof(details->info), value);
            xmlFree(property);
            xmlFree(value);
        }
        xmlFreeDoc(doc);
    } else {
        snprintf(job->error, sizeof(job->error), "Сторінка гри недоступна (HTTP %ld)", job->http);
    }
    if (cover[0] && !atomic_load(&job->cancel)) {
        details->cover = catalog_fetch_cover(cover, &job->cancel, &details->cover_bytes);
    }
}

static void search_pages(CatalogJob *job) {
    Game *games = calloc(CATALOG_PAGE_GAMES, sizeof(*games));
    if (!games) {
        strcpy(job->error, "Недостатньо пам’яті для пошуку");
        return;
    }
    char first[CATALOG_URL] = {0};
    unsigned last = 0;
    for (unsigned page = 1; page <= (last ? last : CATALOG_MAX_PAGES) &&
         job->count < CATALOG_SEARCH_LIMIT && !atomic_load(&job->cancel); ++page) {
        atomic_store(&job->searching_page, page);
        size_t count = 0;
        if (!load_page(job, page, games, &count, &last)) {
            if (job->http != 404 || page == 1)
                snprintf(job->error, sizeof(job->error), "Пошук зупинився на сторінці %u (HTTP %ld)", page, job->http);
            break;
        }
        if (!count || !strcmp(games[0].page, first)) break;
        if (page == 1) strcpy(first, games[0].page);
        for (size_t i = 0; i < count && job->count < CATALOG_SEARCH_LIMIT; ++i) {
            if ((strcasestr(games[i].title, job->query) ||
                 strcasestr(games[i].page + sizeof(LIST_URL) - 1, job->query)) &&
                !contains(job->games, job->count, games[i].page)) {
                job->games[job->count++] = games[i];
            }
        }
    }
    free(games);
}

void catalog_request(CatalogJob *job) {
    if (job->request == CATALOG_DETAILS) {
        load_details(job);
    } else if (job->request == CATALOG_SEARCH) {
        search_pages(job);
    } else {
        unsigned last = 0;
        if (!load_page(job, job->page, job->games, &job->count, &last))
            snprintf(job->error, sizeof(job->error), "Каталог недоступний (HTTP %ld)", job->http);
        job->more = job->count && job->page < CATALOG_MAX_PAGES && (!last || job->page < last);
    }
}
