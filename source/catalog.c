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
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define SITE "https://nsroms.net"
#define LIST_URL SITE "/roms/"
#define DRIVE_PREFIX "https://drive.dlarchive.my.id/dl/?id="
#define PAGE_BYTES (4 * 1024 * 1024)
#define COVER_BYTES (2 * 1024 * 1024)
#ifdef __SWITCH__
#define CACHE_DIR "sdmc:/switch/NXLoad"
#else
#define CACHE_DIR "./NXLoad-cache"
#endif
#define CACHE_FILE CACHE_DIR "/catalog.bin"
#define CACHE_TEMP CACHE_DIR "/catalog.tmp"
#define CACHE_BACKUP CACHE_DIR "/catalog.prev"
#define CACHE_MAGIC "NXLCAT02"

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

static Buffer fetch(CURL *curl, atomic_bool *cancel, const char *url, size_t limit, long *status) {
    *status = 0;
    Buffer buf = { .data = malloc(limit + 1), .capacity = limit };
    if (!buf.data) return (Buffer){0};
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NXLoad/1.0.3");
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, should_cancel);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    if (result != CURLE_OK || *status != 200 || !buf.size) {
        free(buf.data);
        return (Buffer){0};
    }
    buf.data[buf.size] = 0;
    return buf;
}

static unsigned char *fetch_resource(const char *url, atomic_bool *cancel, size_t limit, size_t *bytes) {
    *bytes = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    long status = 0;
    Buffer data = fetch(curl, cancel, url, limit, &status);
    curl_easy_cleanup(curl);
    *bytes = data.size;
    return data.data;
}

unsigned char *catalog_fetch_cover(const char *url, atomic_bool *cancel, size_t *bytes) {
    return fetch_resource(url, cancel, COVER_BYTES, bytes);
}

unsigned char *catalog_fetch_document(const char *url, atomic_bool *cancel, size_t *bytes) {
    return fetch_resource(url, cancel, PAGE_BYTES, bytes);
}

static htmlDocPtr fetch_page(CURL *curl, atomic_bool *cancel, const char *url, long *status) {
    Buffer buf = fetch(curl, cancel, url, PAGE_BYTES, status);
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

static bool load_page(CURL *curl, CatalogIndexJob *job, unsigned page,
                      Game *games, size_t *count, long *status) {
    char url[CATALOG_URL] = LIST_URL;
    if (page > 1) snprintf(url, sizeof(url), LIST_URL "page/%u/", page);
    htmlDocPtr doc = fetch_page(curl, &job->cancel, url, status);
    if (!doc) return false;
    *count = 0;
    read_catalog(xmlDocGetRootElement(doc), games, count);
    unsigned pages = declared_pages(doc);
    if (pages) atomic_store(&job->pages, pages);
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
    CURL *curl = curl_easy_init();
    htmlDocPtr doc = curl ? fetch_page(curl, &job->cancel, job->game.page, &job->http) : NULL;
    if (curl) curl_easy_cleanup(curl);
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

void catalog_request(CatalogJob *job) {
    load_details(job);
}

static bool valid_game(const Game *game) {
    if (!game->title[0] || !memchr(game->title, 0, sizeof(game->title)) ||
        !memchr(game->page, 0, sizeof(game->page)) ||
        !memchr(game->cover, 0, sizeof(game->cover))) return false;
    char normalized[CATALOG_URL];
    return game_page(BAD_CAST game->page, normalized) && !strcmp(normalized, game->page) &&
        (!game->cover[0] || (site_url(BAD_CAST game->cover, normalized) &&
                            !strcmp(normalized, game->cover)));
}

static bool read_cache_path(const char *path, CatalogIndex *index) {
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
    for (size_t i = 0; ok && i < count; ++i) ok = valid_game(&index->games[i]);
    if (ok) {
        index->count = count;
        index->saved_at = (time_t)saved_at;
    }
    return ok;
}

bool catalog_read_cache(CatalogIndex *index) {
    index->count = 0;
    index->saved_at = 0;
    return read_cache_path(CACHE_FILE, index) || read_cache_path(CACHE_BACKUP, index);
}

static bool save_cache(const CatalogIndex *index) {
    if (mkdir(CACHE_DIR, 0777) && errno != EEXIST) return false;
    FILE *file = fopen(CACHE_TEMP, "wb");
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
        if (!stat(CACHE_FILE, &st)) {
            unlink(CACHE_BACKUP);
            moved = rename(CACHE_FILE, CACHE_BACKUP) == 0;
            ok = moved;
        } else {
            ok = errno == ENOENT;
        }
        if (ok) ok = rename(CACHE_TEMP, CACHE_FILE) == 0;
        if (ok) unlink(CACHE_BACKUP);
        else if (moved) rename(CACHE_BACKUP, CACHE_FILE);
    }
    if (!ok) unlink(CACHE_TEMP);
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
        long status = 0;
        size_t count = 0;
        bool loaded = load_page(curl, job, page, games, &count, &status);
        if (!loaded && (status == 0 || status >= 500) && wait_until_idle(job)) {
            index_delay();
            loaded = load_page(curl, job, page, games, &count, &status);
        }
        if (atomic_load(&job->cancel)) break;
        unsigned pages = atomic_load(&job->pages);
        if (!loaded) {
            if (!pages && page > 1 && status == 404) job->complete = true;
            else snprintf(job->error, sizeof(job->error), "Каталог · сторінка %u · HTTP %ld", page, status);
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
        if (!save_cache(&job->index)) strcpy(job->error, "Каталог завантажився. Кеш на microSD недоступний.");
    } else if (!atomic_load(&job->cancel) && !job->error[0]) {
        strcpy(job->error, "Оновлення каталогу не завершилося");
    }
done:
    if (curl) curl_easy_cleanup(curl);
    free(games);
}
