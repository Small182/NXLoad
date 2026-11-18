#define _GNU_SOURCE
#include "catalog.h"
#include "game_info.h"
#include "storage.h"

#include <ctype.h>
#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/uri.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define SITE "https://nsroms.net"
#define LIST_URL SITE "/roms/"
#define ROMSHQ "https://romshq.com"
#define ROMSHQ_GAME ROMSHQ "/game/"
#define ROMSHQ_LIST ROMSHQ "/browse?scene=62"
#define DRIVE_PREFIX "https://drive.dlarchive.my.id/dl/?id="
#define PAGE_BYTES (4 * 1024 * 1024)
#define COVER_BYTES (2 * 1024 * 1024)
#define CACHE_MAGIC "NXLCAT02"
#ifndef ROMSHQ_SEED
#define ROMSHQ_SEED "romfs:/romshq.tsv.gz"
#endif

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

static Buffer fetch(CURL *curl, atomic_bool *cancel, const char *url, size_t limit,
                    CatalogFetchStatus *status, bool thumbnail) {
    *status = (CatalogFetchStatus){0};
    Buffer buf = { .data = malloc(limit + 1), .capacity = limit };
    if (!buf.data) {
        status->curl = CURLE_OUT_OF_MEMORY;
        return (Buffer){0};
    }
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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, thumbnail ? 3L : 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, thumbnail ? 7L : 16L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NXLoad/1.0.3");
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, should_cancel);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    status->curl = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status->http);
    if (status->curl == CURLE_PEER_FAILED_VERIFICATION)
        curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &status->verify);
    if (status->curl != CURLE_OK || status->http != 200 || !buf.size) {
        free(buf.data);
        return (Buffer){0};
    }
    buf.data[buf.size] = 0;
    return buf;
}

static unsigned char *fetch_resource(const char *url, atomic_bool *cancel, size_t limit,
                                     size_t *bytes, CatalogFetchStatus *status) {
    *bytes = 0;
    CURL *curl = curl_easy_init();
    if (!curl) {
        *status = (CatalogFetchStatus){ .curl = CURLE_FAILED_INIT };
        return NULL;
    }
    Buffer data = fetch(curl, cancel, url, limit, status, false);
    curl_easy_cleanup(curl);
    *bytes = data.size;
    return data.data;
}

unsigned char *catalog_fetch_cover(const char *url, atomic_bool *cancel, size_t *bytes) {
    CatalogFetchStatus status;
    return fetch_resource(url, cancel, COVER_BYTES, bytes, &status);
}

unsigned char *catalog_fetch_thumbnail(CURL *connection, const char *url, atomic_bool *cancel, size_t *bytes) {
    CatalogFetchStatus status;
    Buffer image = fetch(connection, cancel, url, COVER_BYTES, &status, true);
    *bytes = image.size;
    return image.data;
}

unsigned char *catalog_fetch_document(const char *url, atomic_bool *cancel, size_t *bytes,
                                      CatalogFetchStatus *status) {
    return fetch_resource(url, cancel, PAGE_BYTES, bytes, status);
}

static htmlDocPtr fetch_page(CURL *curl, atomic_bool *cancel, const char *url, CatalogFetchStatus *status) {
    Buffer buf = fetch(curl, cancel, url, PAGE_BYTES, status, false);
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

static bool site_url(const xmlChar *href, char out[CATALOG_URL], CatalogSource source) {
    if (!href) return false;
    const char *base = source == CATALOG_ROMSHQ ? ROMSHQ "/" : SITE "/";
    xmlChar *url = xmlBuildURI(href, BAD_CAST base);
    bool valid = url && !strncmp((char *)url, base, strlen(base)) &&
        strlen((char *)url) < CATALOG_URL;
    if (valid) strcpy(out, (char *)url);
    xmlFree(url);
    return valid;
}

static bool game_page(const xmlChar *href, char out[CATALOG_URL], CatalogSource source) {
    const char *prefix = source == CATALOG_ROMSHQ ? ROMSHQ_GAME : LIST_URL;
    if (!site_url(href, out, source) || strncmp(out, prefix, strlen(prefix))) return false;
    char *slug = out + strlen(prefix);
    size_t n = strspn(slug, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789%-_");
    if (!n || (slug[n] && strcmp(slug + n, "/"))) return false;
    if (source == CATALOG_ROMSHQ) {
        slug[n] = 0;
        return true;
    }
    if (!slug[n]) {
        if ((size_t)(slug - out) + n + 1 >= CATALOG_URL) return false;
        strcpy(slug + n, "/");
    }
    return true;
}

static void read_cover(xmlNodePtr img, char out[CATALOG_URL], CatalogSource source) {
    if (!img) return;
    const char *keys[] = {"data-src", "data-lazy-src", "data-srcset", "srcset", "src"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        xmlChar *value = xmlGetProp(img, BAD_CAST keys[i]);
        if (value && strstr(keys[i], "srcset")) {
            size_t first = strcspn((char *)value, " \t,\r\n");
            value[first] = 0;
        }
        bool ok = value && *value && site_url(value, out, source);
        xmlFree(value);
        if (ok) return;
    }
}

static void read_game(xmlNodePtr anchor, Game *game, CatalogSource source) {
    xmlNodePtr heading = find_tag(anchor->children, "h3");
    if (!heading) heading = find_tag(anchor->children, "h2");
    if (!heading) heading = find_tag(anchor->children, "h4");
    if (heading) node_text(heading, game->title);
    xmlNodePtr img = find_tag(anchor->children, "img");
    read_cover(img, game->cover, source);
    if (!game->title[0] && img) {
        xmlChar *alt = xmlGetProp(img, BAD_CAST "alt");
        copy_text(game->title, sizeof(game->title), alt);
        xmlFree(alt);
    }
    if (!game->title[0]) node_text(anchor, game->title);
    if (!game->title[0]) {
        const char *slug = game->page + strlen(source == CATALOG_ROMSHQ ? ROMSHQ_GAME : LIST_URL);
        size_t n = strcspn(slug, "/");
        if (n >= sizeof(game->title)) n = sizeof(game->title) - 1;
        for (size_t i = 0; i < n; ++i) game->title[i] = slug[i] == '-' ? ' ' : slug[i];
    }
    clean_catalog_title(game->title);
}

static bool contains(const Game *games, size_t count, const char *url) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(games[i].page, url)) return true;
    return false;
}

static void romshq_platform(xmlNodePtr node, bool *switch_game, bool *other) {
    for (; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (is_element(node, "span") || is_element(node, "small") || is_element(node, "strong") ||
            is_element(node, "div")) {
            char label[CATALOG_TEXT];
            node_text(node, label);
            if (!strcasecmp(label, "Switch") || !strcasecmp(label, "Nintendo Switch")) *switch_game = true;
            if (!strcasecmp(label, "Switch 2") || !strcasecmp(label, "Nintendo Switch 2") ||
                !strcasecmp(label, "Switch2") || !strcasecmp(label, "Nintendo Switch2") ||
                !strcasecmp(label, "PS5") || !strcasecmp(label, "PlayStation 5")) *other = true;
        }
        romshq_platform(node->children, switch_game, other);
    }
}

static bool romshq_other_game(xmlNodePtr node, const char *page) {
    for (; node; node = node->next) {
        if (is_element(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            char target[CATALOG_URL];
            bool other = game_page(href, target, CATALOG_ROMSHQ) && strcmp(page, target);
            xmlFree(href);
            if (other) return true;
        }
        if (romshq_other_game(node->children, page)) return true;
    }
    return false;
}

static xmlNodePtr romshq_card(xmlNodePtr link, const char *page) {
    for (unsigned depth = 0; link && depth < 4; ++depth, link = link->parent) {
        if (romshq_other_game(link->children, page)) return NULL;
        bool switch_game = false, other = false;
        romshq_platform(link->children, &switch_game, &other);
        if (other) return NULL;
        if (switch_game && (find_tag(link->children, "h4") ||
                            find_tag(link->children, "h3") || find_tag(link->children, "h2"))) return link;
    }
    return NULL;
}

static void read_catalog(xmlNodePtr node, Game *games, size_t *count, CatalogSource source) {
    for (; node && *count < CATALOG_PAGE_GAMES; node = node->next) {
        if (is_element(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            Game game = {0};
            bool ok = game_page(href, game.page, source);
            xmlNodePtr card = ok && source == CATALOG_ROMSHQ ? romshq_card(node, game.page) : node;
            xmlFree(href);
            if (ok && card && !contains(games, *count, game.page)) {
                read_game(card, &game, source);
                if (game.title[0]) games[(*count)++] = game;
            }
        }
        read_catalog(node->children, games, count, source);
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

static unsigned romshq_pages(xmlNodePtr node) {
    unsigned max = 0;
    for (; node; node = node->next) {
        if (is_element(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            if (href) {
                const char *url = (const char *)href;
                if (!strncmp(url, ROMSHQ "/browse?", sizeof(ROMSHQ "/browse?") - 1) ||
                    !strncmp(url, "/browse?", sizeof("/browse?") - 1)) {
                    const char *field = strstr(url, "page=");
                    if (field && (field[-1] == '?' || field[-1] == '&')) {
                        char *end;
                        unsigned long page = strtoul(field + 5, &end, 10);
                        if ((!*end || *end == '&') && page <= CATALOG_MAX_PAGES && page > max)
                            max = (unsigned)page;
                    }
                }
                xmlFree(href);
            }
        }
        unsigned children = romshq_pages(node->children);
        if (children > max) max = children;
    }
    return max;
}

static bool load_page(CURL *curl, CatalogIndexJob *job, unsigned page,
                      Game *games, size_t *count, CatalogFetchStatus *status) {
    char url[CATALOG_URL];
    if (job->source == CATALOG_ROMSHQ) {
        if (page == 1) strcpy(url, ROMSHQ_LIST);
        else snprintf(url, sizeof(url), ROMSHQ_LIST "&page=%u", page);
    } else if (page > 1)
        snprintf(url, sizeof(url), LIST_URL "page/%u/", page);
    else
        strcpy(url, LIST_URL);
    htmlDocPtr doc = fetch_page(curl, &job->cancel, url, status);
    if (!doc) return false;
    *count = 0;
    read_catalog(xmlDocGetRootElement(doc), games, count, job->source);
    unsigned pages = job->source == CATALOG_NSROMS ? declared_pages(doc) :
                     romshq_pages(xmlDocGetRootElement(doc));
    if (pages > atomic_load(&job->pages)) atomic_store(&job->pages, pages);
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

static xmlNodePtr romshq_section(xmlNodePtr node, const char *heading, const char *id) {
    for (; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        xmlChar *value = xmlGetProp(node, BAD_CAST "id");
        bool identified = value && !xmlStrcasecmp(value, BAD_CAST id) && !is_element(node, "a");
        xmlFree(value);
        if (identified) return node;
        if (is_element(node, "h2") || is_element(node, "h3")) {
            char title[CATALOG_TEXT];
            node_text(node, title);
            if (!strcasecmp(title, heading)) return node->parent;
        }
        xmlNodePtr found = romshq_section(node->children, heading, id);
        if (found) return found;
    }
    return NULL;
}

static bool romshq_id(const char *value, char id[DLARCHIVE_ID_CAPACITY]) {
    if (!value) return false;
    size_t digits = strspn(value, "0123456789");
    if (!digits || digits > 20 || value[digits]) return false;
    strcpy(id, value);
    return true;
}

static bool romshq_button_id(xmlNodePtr node, char id[DLARCHIVE_ID_CAPACITY]) {
    if (!xmlHasProp(node, BAD_CAST "data-romshq-download")) return false;
    xmlChar *value = xmlGetProp(node, BAD_CAST "data-video-id");
    bool ok = romshq_id((char *)value, id);
    xmlFree(value);
    return ok;
}

static void romshq_file_title(xmlNodePtr node, char title[CATALOG_TEXT]) {
    for (unsigned depth = 0; node && depth < 4; ++depth, node = node->parent) {
        char label[CATALOG_TEXT];
        node_text(node, label);
        if (strcasestr(label, "Base Game") || strcasestr(label, "Download Here")) {
            strcpy(title, "Базова гра");
            return;
        }
        if (strcasestr(label, "Update")) {
            strcpy(title, "Оновлення");
            return;
        }
        if (strcasestr(label, "DLC")) {
            strcpy(title, "DLC");
            return;
        }
        if (strcasestr(label, "Mirror")) {
            snprintf(title, CATALOG_TEXT, "%s", label);
            return;
        }
    }
    strcpy(title, "Файл ROMsHQ");
}

static void read_romshq_files(xmlNodePtr node, GameDetails *details) {
    for (; node && details->count < CATALOG_FILE_COUNT; node = node->next) {
        if (is_element(node, "a") || is_element(node, "button")) {
            GameFile file = {0};
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            bool valid = romshq_button_id(node, file.id);
            if (valid) file.source = FILE_ROMSHQ_SIGNED;
            else if (href && strlen((char *)href) < sizeof(file.url)) {
                const char *link = (const char *)href;
                if (!strncmp(link, "https://datanodes.to/", sizeof("https://datanodes.to/") - 1)) {
                    file.source = FILE_DATANODES;
                    valid = true;
                } else if (!strncmp(link, "https://1fichier.com/", sizeof("https://1fichier.com/") - 1)) {
                    file.source = FILE_1FICHIER;
                    valid = true;
                }
                if (valid) strcpy(file.url, link);
            }
            xmlFree(href);
            for (size_t i = 0; valid && i < details->count; ++i) {
                const GameFile *previous = &details->files[i];
                if (previous->source == file.source && !strcmp(previous->id, file.id) &&
                    !strcmp(previous->url, file.url)) valid = false;
            }
            if (valid) {
                romshq_file_title(node, file.title);
                const char *label = file.title;
                bool excluded = !strncasecmp(label, "Mirror ", 7) &&
                    (label[7] == '2' || label[7] == '3') && !isdigit((unsigned char)label[8]);
                if (!excluded) details->files[details->count++] = file;
            }
        }
        read_romshq_files(node->children, details);
    }
}

static void read_romshq_screenshots(xmlNodePtr node, GameDetails *details) {
    for (; node && details->screenshot_count < CATALOG_SCREENSHOTS; node = node->next) {
        if (is_element(node, "a") || is_element(node, "img")) {
            xmlChar *value = xmlGetProp(node, BAD_CAST (is_element(node, "a") ? "href" : "data-src"));
            if (!value && is_element(node, "img")) value = xmlGetProp(node, BAD_CAST "src");
            char url[CATALOG_URL];
            bool valid = site_url(value, url, CATALOG_ROMSHQ);
            xmlFree(value);
            if (valid) {
                const char *extension = strrchr(url, '.');
                valid = extension && (!strcasecmp(extension, ".jpg") ||
                    !strcasecmp(extension, ".jpeg") || !strcasecmp(extension, ".png") ||
                    !strcasecmp(extension, ".webp"));
            }
            for (size_t i = 0; valid && i < details->screenshot_count; ++i)
                if (!strcmp(details->screenshots[i], url)) valid = false;
            if (valid) strcpy(details->screenshots[details->screenshot_count++], url);
        }
        read_romshq_screenshots(node->children, details);
    }
}

static void load_details(CatalogJob *job) {
    GameDetails *details = &job->details;
    strcpy(details->title, job->game.title);
    strcpy(details->cover_url, job->game.cover);
    CURL *curl = curl_easy_init();
    CatalogFetchStatus status = {.curl = CURLE_FAILED_INIT};
    htmlDocPtr doc = curl ? fetch_page(curl, &job->cancel, job->game.page, &status) : NULL;
    job->http = status.http;
    if (curl) curl_easy_cleanup(curl);
    if (doc) {
        xmlNodePtr root = xmlDocGetRootElement(doc);
        if (job->source == CATALOG_ROMSHQ) {
            xmlNodePtr files = romshq_section(root, "Download Links", "download");
            if (files) read_romshq_files(files->children, details);
            xmlNodePtr gallery = romshq_section(root, "Screenshots", "screenshots");
            if (gallery) read_romshq_screenshots(gallery->children, details);
        } else {
            read_files(root, details);
        }
        xmlNodePtr heading = find_tag(root, "h1");
        if (heading) node_text(heading, details->title);
        clean_catalog_title(details->title);
        xmlNodePtr head = find_tag(root, "head");
        for (xmlNodePtr node = head ? head->children : NULL; node; node = node->next) {
            if (!is_element(node, "meta")) continue;
            xmlChar *property = xmlGetProp(node, BAD_CAST "property");
            xmlChar *value = xmlGetProp(node, BAD_CAST "content");
            if (!xmlStrcmp(property, BAD_CAST "og:image")) site_url(value, details->cover_url, job->source);
            xmlFree(property);
            xmlFree(value);
        }
        xmlFreeDoc(doc);
    } else {
        if (status.curl)
            snprintf(job->error, sizeof(job->error), "Сторінка гри недоступна · cURL %d", status.curl);
        else
            snprintf(job->error, sizeof(job->error), "Сторінка гри недоступна · HTTP %ld", status.http);
    }
}

void catalog_request(CatalogJob *job) {
    load_details(job);
}

static bool valid_game(const Game *game, CatalogSource source) {
    if (!game->title[0] || !memchr(game->title, 0, sizeof(game->title)) ||
        !memchr(game->page, 0, sizeof(game->page)) ||
        !memchr(game->cover, 0, sizeof(game->cover))) return false;
    char normalized[CATALOG_URL];
    return game_page(BAD_CAST game->page, normalized, source) && !strcmp(normalized, game->page) &&
        (!game->cover[0] || (site_url(BAD_CAST game->cover, normalized, source) &&
                            !strcmp(normalized, game->cover)));
}

static bool read_cache_path(const char *path, CatalogIndex *index, CatalogSource source) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char magic[8];
    uint32_t count = 0;
    int64_t saved_at = 0;
    bool ok = fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
       !memcmp(magic, CACHE_MAGIC, sizeof(magic)) &&
       fread(&count, sizeof(count), 1, file) == 1 && count && count <= CATALOG_CAPACITY &&
       fread(&saved_at, sizeof(saved_at), 1, file) == 1 &&
       fread(index->games, sizeof(Game), count, file) == count &&
       fgetc(file) == EOF && !ferror(file);
    fclose(file);
    for (size_t i = 0; ok && i < count; ++i) {
        ok = valid_game(&index->games[i], source);
        if (ok) clean_catalog_title(index->games[i].title);
    }
    if (ok) {
        index->count = count;
        index->saved_at = (time_t)saved_at;
    }
    return ok;
}

static bool read_romshq_seed(CatalogIndex *index) {
    gzFile file = gzopen(ROMSHQ_SEED, "rb");
    if (!file) return false;
    char line[CATALOG_TEXT + 2 * CATALOG_URL + 4];
    unsigned long long timestamp = 0;
    bool ok = gzgets(file, line, sizeof(line)) && sscanf(line, "NXLSEED1 %llu", &timestamp) == 1 &&
              timestamp > 0 && timestamp <= INT64_MAX && (time_t)timestamp > index->saved_at;
    Game *games = ok ? calloc(CATALOG_CAPACITY, sizeof(*games)) : NULL;
    if (!games) ok = false;
    size_t count = 0;
    while (ok && gzgets(file, line, sizeof(line))) {
        size_t length = strlen(line);
        char *field = line;
        if (!length || line[length - 1] != '\n' || count == CATALOG_CAPACITY) {
            ok = false;
            break;
        }
        line[length - 1] = 0;
        const char *title = strsep(&field, "\t");
        const char *page = strsep(&field, "\t");
        const char *cover = field;
        if (!page || !cover || !title[0] || strlen(title) >= CATALOG_TEXT ||
            strlen(page) >= CATALOG_URL || strlen(cover) >= CATALOG_URL) {
            ok = false;
            break;
        }
        Game *game = &games[count];
        strcpy(game->title, title);
        strcpy(game->page, page);
        strcpy(game->cover, cover);
        ok = valid_game(game, CATALOG_ROMSHQ);
        if (ok) {
            clean_catalog_title(game->title);
            ++count;
        }
    }
    ok = ok && gzeof(file) && count > index->count;
    if (gzclose(file) != Z_OK) ok = false;
    if (ok) {
        memcpy(index->games, games, count * sizeof(*games));
        index->count = count;
        index->saved_at = (time_t)timestamp;
    }
    free(games);
    return ok;
}

bool catalog_read_cache(CatalogIndex *index, CatalogSource source) {
    index->count = 0;
    index->saved_at = 0;
    if (source == CATALOG_ROMSHQ) {
        bool cached = read_cache_path(NXLOAD_DOWNLOAD_DIR "/romshq.bin", index, source) ||
                      read_cache_path(NXLOAD_DOWNLOAD_DIR "/romshq.prev", index, source);
        return read_romshq_seed(index) || cached;
    }
    return read_cache_path(NXLOAD_DOWNLOAD_DIR "/catalog.bin", index, source) ||
           read_cache_path(NXLOAD_DOWNLOAD_DIR "/catalog.prev", index, source);
}

static bool save_cache(const CatalogIndex *index, CatalogSource source) {
    const char *dest = source == CATALOG_ROMSHQ ? NXLOAD_DOWNLOAD_DIR "/romshq.bin" : NXLOAD_DOWNLOAD_DIR "/catalog.bin";
    const char *temp = source == CATALOG_ROMSHQ ? NXLOAD_DOWNLOAD_DIR "/romshq.tmp" : NXLOAD_DOWNLOAD_DIR "/catalog.tmp";
    const char *backup = source == CATALOG_ROMSHQ ? NXLOAD_DOWNLOAD_DIR "/romshq.prev" : NXLOAD_DOWNLOAD_DIR "/catalog.prev";
    if (!nxload_ensure_storage()) return false;
    FILE *file = fopen(temp, "wb");
    if (!file) return false;
    const uint32_t count = (uint32_t)index->count;
    const int64_t saved_at = index->saved_at;
    bool ok = fwrite(CACHE_MAGIC, 1, 8, file) == 8 &&
        fwrite(&count, sizeof(count), 1, file) == 1 &&
        fwrite(&saved_at, sizeof(saved_at), 1, file) == 1 &&
        fwrite(index->games, sizeof(Game), count, file) == count &&
        fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file)) ok = false;

    bool moved = false;
    if (ok) {
        struct stat st;
        if (!stat(dest, &st)) {
            unlink(backup);
            moved = rename(dest, backup) == 0;
            ok = moved;
        } else {
            ok = errno == ENOENT;
        }
        if (ok) ok = rename(temp, dest) == 0;
        if (ok) unlink(backup);
        else if (moved) rename(backup, dest);
    }
    if (!ok) unlink(temp);
    return ok;
}

static void index_delay(void) {
#ifdef __SWITCH__
    svcSleepThread(200000000L);
#else
    usleep(200000);
#endif
}

static bool wait_until_idle(CatalogIndexJob *job) {
    while (atomic_load(&job->paused) && !atomic_load(&job->cancel)) index_delay();
    return !atomic_load(&job->cancel);
}

void catalog_build_index(CatalogIndexJob *job) {
    Game *games = calloc(CATALOG_PAGE_GAMES, sizeof(*games));
    CURL *curl = curl_easy_init();
    if (!games || !curl) {
        strcpy(job->error, "Не вдалося підготувати індекс каталогу");
        goto done;
    }
    for (unsigned page = 1; page <= CATALOG_MAX_PAGES && wait_until_idle(job); ++page) {
        atomic_store(&job->page, page);
        CatalogFetchStatus status;
        size_t count = 0;
        bool loaded = load_page(curl, job, page, games, &count, &status);
        if (!loaded && (status.curl || status.http >= 500) && wait_until_idle(job)) {
            index_delay();
            loaded = load_page(curl, job, page, games, &count, &status);
        }
        if (atomic_load(&job->cancel)) break;
        unsigned pages = atomic_load(&job->pages);
        if (!loaded) {
            if (!pages && page > 1 && status.http == 404) job->complete = true;
            else if (status.curl == CURLE_PEER_FAILED_VERIFICATION)
                snprintf(job->error, sizeof(job->error), "Стор. %u · TLS 60 · X.509 0x%lX", page, status.verify);
            else if (status.curl)
                snprintf(job->error, sizeof(job->error), "Сторінка %u · cURL %d", page, status.curl);
            else
                snprintf(job->error, sizeof(job->error), "Сторінка %u · HTTP %ld", page, status.http);
            break;
        }

        size_t previous = job->index.count;
        for (size_t i = 0; i < count; ++i) {
            if (contains(job->index.games, job->index.count, games[i].page)) continue;
            if (job->index.count == CATALOG_CAPACITY) {
                strcpy(job->error, "Каталог перевищує місткість індексу");
                goto done;
            }
            job->index.games[job->index.count++] = games[i];
        }
        atomic_store(&job->published, job->index.count);

        if (job->index.count == previous) {
            if (previous && !pages) job->complete = true;
            else snprintf(job->error, sizeof(job->error), "Каталог · немає ігор на сторінці %u", page);
            break;
        }
        if (pages && page >= pages) {
            job->complete = true;
            break;
        }
    }
    if (wait_until_idle(job) && job->complete) {
        job->index.saved_at = time(NULL);
        if (!save_cache(&job->index, job->source)) strcpy(job->error, "Каталог завантажився. Кеш на microSD недоступний.");
    } else if (!atomic_load(&job->cancel) && !job->error[0]) {
        strcpy(job->error, "Оновлення каталогу не завершилося");
    }
done:
    if (curl) curl_easy_cleanup(curl);
    free(games);
}
