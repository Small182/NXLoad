#define _GNU_SOURCE
#include "catalog.h"
#include "game_info.h"
#include "storage.h"
#include "language.h"
#include "json.h"
#include <inttypes.h>

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
#define ROMSNS "https://romsns.com"
#define ROMSNS_LIST ROMSNS "/"
#define DLARCHIVE_POST "https://dlarchive.my.id/?post_type=roms&p="
#define DRIVE_PATH "https://drive.dlarchive.my.id/dl"
#define DATANODES "https://datanodes.to/"
#define FICHIER "https://1fichier.com/"
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
    CURL *html_connection;
} Buffer;

static size_t receive(void *data, size_t width, size_t count, void *ptr) {
    Buffer *buf = ptr;
    size_t bytes = width * count;
    if (buf->html_connection) {
        char *type = NULL;
        curl_easy_getinfo(buf->html_connection, CURLINFO_CONTENT_TYPE, &type);
        if (!type || strncasecmp(type, "text/html", sizeof("text/html") - 1)) return 0;
    }
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
                    CatalogFetchStatus *status, bool thumbnail, bool redirects, bool html_only) {
    *status = (CatalogFetchStatus){0};
    Buffer buf = { .data = malloc(limit + 1), .capacity = limit, .html_connection = html_only ? curl : NULL };
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
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, thumbnail ? 3L : 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, thumbnail ? 7L : 16L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Preserve libcurl's normal IPv4/IPv6 fallback (as in v1.0.50).
    const bool nintendo_store = !strncmp(url, "https://www.nintendo.com/",
                                         sizeof("https://www.nintendo.com/") - 1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, nintendo_store ?
                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                     "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36" : "NXLoad/1.0.3");
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
    Buffer data = fetch(curl, cancel, url, limit, status, false, true, false);
    curl_easy_cleanup(curl);
    *bytes = data.size;
    return data.data;
}

unsigned char *catalog_fetch_cover_status(const char *url, atomic_bool *cancel, size_t *bytes,
                                          CatalogFetchStatus *status) {
    return fetch_resource(url, cancel, COVER_BYTES, bytes, status);
}

unsigned char *catalog_fetch_cover(const char *url, atomic_bool *cancel, size_t *bytes) {
    CatalogFetchStatus status;
    return catalog_fetch_cover_status(url, cancel, bytes, &status);
}

unsigned char *catalog_fetch_thumbnail(CURL *connection, const char *url, atomic_bool *cancel, size_t *bytes) {
    CatalogFetchStatus status;
    Buffer image = fetch(connection, cancel, url, COVER_BYTES, &status, true, true, false);
    *bytes = image.size;
    return image.data;
}

unsigned char *catalog_fetch_document(const char *url, atomic_bool *cancel, size_t *bytes,
                                      CatalogFetchStatus *status) {
    // Nintendo Store's server-rendered product metadata can exceed 4 MiB.
    const bool nintendo_store = !strncmp(url, "https://www.nintendo.com/",
                                         sizeof("https://www.nintendo.com/") - 1);
    return fetch_resource(url, cancel, nintendo_store ? 12 * 1024 * 1024 : PAGE_BYTES, bytes, status);
}

static htmlDocPtr fetch_page(CURL *curl, atomic_bool *cancel, const char *url, CatalogFetchStatus *status, bool redirects) {
    Buffer buf = fetch(curl, cancel, url, PAGE_BYTES, status, false, redirects, false);
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

static bool has_class(xmlNodePtr node, const char *name) {
    xmlChar *value = xmlGetProp(node, BAD_CAST "class");
    const char *p = (const char *)value;
    const char *space = " \t\r\n";
    size_t length = strlen(name);
    bool found = false;
    while (p && *p) {
        p += strspn(p, space);
        size_t size = strcspn(p, space);
        if (size == length && !memcmp(p, name, size)) {
            found = true;
            break;
        }
        p += size;
    }
    xmlFree(value);
    return found;
}

static xmlNodePtr find_class(xmlNodePtr node, const char *name) {
    for (; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (has_class(node, name)) return node;
        xmlNodePtr child = find_class(node->children, name);
        if (child) return child;
    }
    return NULL;
}

static const char *game_prefix(CatalogSource source) {
    if (source == CATALOG_ROMSHQ) return ROMSHQ_GAME;
    return source == CATALOG_ROMSNS ? ROMSNS_LIST : LIST_URL;
}

static bool site_url(const xmlChar *href, char out[CATALOG_URL], CatalogSource source) {
    if (!href) return false;
    const char *base = source == CATALOG_ROMSHQ ? ROMSHQ "/" :
                       source == CATALOG_ROMSNS ? ROMSNS_LIST : SITE "/";
    xmlChar *url = xmlBuildURI(href, BAD_CAST base);
    bool valid = url && !strncmp((char *)url, base, strlen(base)) &&
        strlen((char *)url) < CATALOG_URL;
    if (valid) strcpy(out, (char *)url);
    xmlFree(url);
    return valid;
}

static bool game_page(const xmlChar *href, char out[CATALOG_URL], CatalogSource source) {
    const char *prefix = game_prefix(source);
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
    xmlNodePtr heading = source == CATALOG_ROMSNS ? find_class(anchor->children, "rgc-title-in") : NULL;
    if (!heading) heading = find_tag(anchor->children, "h3");
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
        const char *slug = game->page + strlen(game_prefix(source));
        size_t n = strcspn(slug, "/");
        if (n >= sizeof(game->title)) n = sizeof(game->title) - 1;
        for (size_t i = 0; i < n; ++i) game->title[i] = slug[i] == '-' ? ' ' : slug[i];
    }
    if (source == CATALOG_ROMSNS) {
        static const char port[] = " (Homebrew Port)";
        size_t length = strlen(game->title);
        if (length > sizeof(port) - 1 &&
            !strcasecmp(game->title + length - (sizeof(port) - 1), port)) {
            game->title[length - (sizeof(port) - 1)] = 0;
            clean_catalog_title(game->title);
            if (strlen(game->title) + sizeof(port) <= sizeof(game->title))
                strcat(game->title, port);
        } else clean_catalog_title(game->title);
    } else clean_catalog_title(game->title);
}

static bool contains(const Game *games, size_t count, const char *url) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(games[i].page, url)) return true;
    return false;
}

bool romsns_is_homebrew_port(const Game *game) {
    if (!game) return false;
    if (strcasestr(game->title, "(Homebrew Port)")) return true;
    static const char suffix[] = "-homebrew-port";
    size_t end = strlen(game->page);
    if (end && game->page[end - 1] == '/') --end;
    size_t length = sizeof(suffix) - 1;
    return end >= length && !strncasecmp(game->page + end - length, suffix, length);
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
            bool ok = (source != CATALOG_ROMSNS || has_class(node, "rgc-card")) &&
                      game_page(href, game.page, source);
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

static unsigned romsns_pages(xmlNodePtr root) {
    xmlNodePtr nav = find_class(root, "rgc-pagination");
    xmlNodePtr list = nav ? find_tag(nav->children, "ul") : NULL;
    unsigned max = 0;
    for (xmlNodePtr node = list ? list->children : NULL; node; node = node->next) {
        xmlNodePtr link = find_tag(node->children, "a");
        if (!link) continue;
        char label[CATALOG_TEXT];
        node_text(link, label);
        char *end;
        unsigned long page = strtoul(label, &end, 10);
        if (end != label && !*end && page <= CATALOG_MAX_PAGES && page > max) max = (unsigned)page;
    }
    return max;
}

static bool load_page(CURL *curl, CatalogIndexJob *job, unsigned page,
                      Game *games, size_t *count, CatalogFetchStatus *status) {
    char url[CATALOG_URL];
    if (job->source == CATALOG_ROMSHQ) {
        if (page == 1) strcpy(url, ROMSHQ_LIST);
        else snprintf(url, sizeof(url), ROMSHQ_LIST "&page=%u", page);
    } else {
        const char *list = job->source == CATALOG_ROMSNS ? ROMSNS_LIST : LIST_URL;
        if (page > 1) snprintf(url, sizeof(url), "%spage/%u/", list, page);
        else strcpy(url, list);
    }
    htmlDocPtr doc = fetch_page(curl, &job->cancel, url, status, job->source != CATALOG_ROMSNS);
    if (!doc) return false;
    *count = 0;
    read_catalog(xmlDocGetRootElement(doc), games, count, job->source);
    unsigned pages = job->source == CATALOG_ROMSNS ? romsns_pages(xmlDocGetRootElement(doc)) :
                     job->source == CATALOG_NSROMS ? declared_pages(doc) :
                     romshq_pages(xmlDocGetRootElement(doc));
    if (pages > atomic_load(&job->pages)) atomic_store(&job->pages, pages);
    xmlFreeDoc(doc);
    return true;
}

static bool labeled_size(const char *label, char size[32]) {
    static const char *const units[] = {"TiB", "GiB", "MiB", "KiB", "TB", "GB", "MB", "KB", "bytes", "B"};
    for (const char *p = label; *p; ++p) {
        if (!isdigit((unsigned char)*p) || (p > label && isalnum((unsigned char)p[-1])) ||
            (p - label >= 2 && (p[-1] == '.' || p[-1] == ',') && isdigit((unsigned char)p[-2])))
            continue;
        const char *end = p + strspn(p, "0123456789");
        if ((*end == '.' || *end == ',') && isdigit((unsigned char)end[1]))
            end += 1 + strspn(end + 1, "0123456789");
        int digits = (int)(end - p);
        if (digits > 12) { p = end - 1; continue; }
        const char *unit = end + strspn(end, " \t\r\n");
        if ((unsigned char)unit[0] == 0xc2 && (unsigned char)unit[1] == 0xa0) unit += 2;
        for (size_t i = 0; i < sizeof(units) / sizeof(*units); ++i) {
            size_t length = strlen(units[i]);
            if (!strncasecmp(unit, units[i], length) && !isalnum((unsigned char)unit[length])) {
                snprintf(size, 32, "%.*s %s", digits, p, units[i]);
                return true;
            }
        }
        p = end - 1;
    }
    return false;
}

static bool size_attribute(xmlNodePtr node, const char *name, bool bytes, char size[32]) {
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    const char *label = (const char *)value;
    bool ok = value && labeled_size(label, size);
    if (value && !ok && bytes && isdigit((unsigned char)*label)) {
        char *end;
        errno = 0;
        unsigned long long length = strtoull(label, &end, 10);
        if (!errno && length && !*end) {
            if (length >= 1073741824ULL) snprintf(size, 32, "%.2f GiB", length / 1073741824.0);
            else if (length >= 1048576ULL) snprintf(size, 32, "%.2f MiB", length / 1048576.0);
            else if (length >= 1024ULL) snprintf(size, 32, "%.2f KiB", length / 1024.0);
            else snprintf(size, 32, "%llu B", length);
            ok = true;
        }
    }
    xmlFree(value);
    return ok;
}

static const char *dlarchive_id(const char *url) {
    if (!url || strncmp(url, DRIVE_PATH, sizeof(DRIVE_PATH) - 1)) return NULL;
    const char *query = url + sizeof(DRIVE_PATH) - 1;
    if (*query == '/') ++query;
    return strncmp(query, "?id=", 4) ? NULL : query + 4;
}

static bool download_link_node(xmlNodePtr node) {
    if (xmlHasProp(node, BAD_CAST "data-romshq-download")) return true;
    if (!is_element(node, "a")) return false;
    xmlChar *href = xmlGetProp(node, BAD_CAST "href");
    const char *url = (const char *)href;
    bool found = url && (dlarchive_id(url) ||
        !strncmp(url, "https://datanodes.to/", sizeof("https://datanodes.to/") - 1) ||
        !strncmp(url, "https://1fichier.com/", sizeof("https://1fichier.com/") - 1));
    xmlFree(href);
    return found;
}

static unsigned download_links(xmlNodePtr node) {
    unsigned count = 0;
    for (; node && count < 2; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (download_link_node(node)) ++count;
        else count += download_links(node->children);
    }
    return count;
}

static bool size_from_children(xmlNodePtr node, char size[32]) {
    for (; node; node = node->next) {
        if (node->type == XML_TEXT_NODE && node->content &&
            labeled_size((const char *)node->content, size)) return true;
        if (node->type == XML_ELEMENT_NODE && size_from_children(node->children, size)) return true;
    }
    return false;
}

static void read_file_size(xmlNodePtr node, char size[32]) {
    for (unsigned depth = 0; node && depth < 3 && node->type == XML_ELEMENT_NODE; ++depth, node = node->parent) {
        if (depth && download_links(node->children) != 1) break;
        if (size_attribute(node, "data-size-bytes", true, size) ||
            size_attribute(node, "data-file-size", true, size) ||
            size_attribute(node, "data-size", true, size)) return;
        char label[CATALOG_TEXT];
        node_text(node, label);
        if (size_from_children(node->children, size) || labeled_size(label, size)) return;
    }
}

static void strip_nsp_suffix(char *title) {
    size_t end = strlen(title);
    while (end) {
        while (end && isspace((unsigned char)title[end - 1])) --end;
        if (end < 3 || strncasecmp(title + end - 3, "nsp", 3)) break;
        size_t start = end - 3;
        if (start && title[start - 1] != '.' && !isspace((unsigned char)title[start - 1])) break;
        end = start;
        if (end && title[end - 1] == '.') --end;
        title[end] = 0;
    }
    title[end] = 0;
}

static void romsns_file_label(xmlNodePtr link, GameFile *file) {
    for (xmlNodePtr node = link; node && node->type == XML_ELEMENT_NODE; node = node->parent) {
        if (has_class(node, "roms-card")) break;
        for (xmlNodePtr previous = node->prev; previous; previous = previous->prev) {
            if (previous->type != XML_ELEMENT_NODE) continue;
            if (has_class(previous, "roms-grid") || has_class(previous, "roms-head")) break;
            if (!has_class(previous, "roms-sub")) continue;
            char label[CATALOG_TEXT];
            node_text(previous, label);
            labeled_size(label, file->size);
            char *end = strchr(label, ':');
            if (end) {
                while (end > label && isspace((unsigned char)end[-1])) --end;
                *end = 0;
                if (*label) strcpy(file->title, label);
            }
            return;
        }
    }
}

static void read_files(xmlNodePtr node, GameDetails *details, bool romsns) {
    for (; node && details->count < CATALOG_FILE_COUNT; node = node->next) {
        if (is_element(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            const char *id = dlarchive_id((const char *)href);
            if (id) {
                char title[CATALOG_TEXT] = {0};
                node_text(node, title);
                const size_t n = strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
                bool unique = n && n < DLARCHIVE_ID_CAPACITY && (!id[n] || id[n] == '&') &&
                              (!romsns || !strcasecmp(title, "Fast Drive"));
                for (size_t i = 0; unique && i < details->count; ++i)
                    if (strlen(details->files[i].id) == n && !memcmp(details->files[i].id, id, n)) unique = false;
                if (unique) {
                    GameFile *file = &details->files[details->count++];
                    file->source = FILE_DLARCHIVE;
                    memcpy(file->id, id, n);
                    file->id[n] = 0;
                    strcpy(file->title, title);
                    read_file_size(node, file->size);
                    if (romsns) romsns_file_label(node, file);
                    strip_nsp_suffix(file->title);
                    if (!file->title[0]) strcpy(file->title, "Файл DLArchive");
                }
            }
            xmlFree(href);
        }
        read_files(node->children, details, romsns);
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
            bool signed_link = romshq_button_id(node, file.id);
            bool valid = signed_link;
            if (href && strlen((char *)href) < sizeof(file.url)) {
                const char *link = (const char *)href;
                if (!strncmp(link, DATANODES, sizeof(DATANODES) - 1)) {
                    file.source = FILE_DATANODES;
                    valid = true;
                    strcpy(file.url, link);
                } else if (!strncmp(link, FICHIER, sizeof(FICHIER) - 1)) {
                    file.source = FILE_1FICHIER;
                    valid = true;
                    strcpy(file.url, link);
                }
            }
            if (signed_link) file.source = FILE_ROMSHQ_SIGNED;
            xmlFree(href);
            for (size_t i = 0; valid && i < details->count; ++i) {
                const GameFile *previous = &details->files[i];
                if (previous->source == file.source && !strcmp(previous->id, file.id) &&
                    (signed_link || !strcmp(previous->url, file.url))) valid = false;
            }
            if (valid) {
                romshq_file_title(node, file.title);
                read_file_size(node, file.size);
                const char *label = file.title;
                bool excluded = !strncasecmp(label, "Mirror ", 7) &&
                    (label[7] == '2' || label[7] == '3') && !isdigit((unsigned char)label[8]);
                if (!excluded) details->files[details->count++] = file;
            }
        }
        read_romshq_files(node->children, details);
    }
}

static bool romshq_base_size(xmlNodePtr node, char size[32]) {
    for (; node; node = node->next) {
        if (is_element(node, "h3")) {
            char label[CATALOG_TEXT] = {0};
            node_text(node, label);
            xmlNodePtr value = xmlNextElementSibling(node);
            if (value && is_element(value, "p") && !strcasecmp(label, "File Size")) {
                node_text(value, label);
                if (labeled_size(label, size)) return true;
            }
        }
        if (romshq_base_size(node->children, size)) return true;
    }
    return false;
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

static bool romsns_download_page(xmlNodePtr node, char url[CATALOG_URL]) {
    for (; node; node = node->next) {
        if (is_element(node, "a")) {
            xmlChar *href = xmlGetProp(node, BAD_CAST "href");
            const char *link = (const char *)href;
            const size_t prefix = sizeof(DLARCHIVE_POST) - 1;
            bool valid = false;
            if (link && !strncmp(link, DLARCHIVE_POST, prefix)) {
                size_t digits = strspn(link + prefix, "0123456789");
                valid = digits && digits <= 20 && !link[prefix + digits];
            }
            if (valid) strcpy(url, link);
            xmlFree(href);
            if (valid) return true;
        }
        if (romsns_download_page(node->children, url)) return true;
    }
    return false;
}

static void romsns_gallery_fallback(xmlNodePtr node,
                                    char shots[CATALOG_SCREENSHOTS][CATALOG_URL], size_t *count) {
    for (; node && *count < CATALOG_SCREENSHOTS; node = node->next) {
        if (is_element(node, "img")) {
            xmlChar *src = xmlGetProp(node, BAD_CAST "data-src");
            if (!src || !*src) { xmlFree(src); src = xmlGetProp(node, BAD_CAST "src"); }
            char url[CATALOG_URL];
            bool ok = site_url(src, url, CATALOG_ROMSNS) &&
                !strncmp(url, ROMSNS "/wp-content/uploads/", sizeof(ROMSNS "/wp-content/uploads/") - 1);
            xmlFree(src);
            for (size_t i = 0; ok && i < *count; ++i)
                if (!strcmp(shots[i], url)) ok = false;
            if (ok) strcpy(shots[(*count)++], url);
        }
        romsns_gallery_fallback(node->children, shots, count);
    }
}

static void read_romsns_media(xmlNodePtr root, char description[PORT_DESCRIPTION_BYTES],
                               char screenshots[CATALOG_SCREENSHOTS][CATALOG_URL], size_t *count) {
    xmlNodePtr article = find_class(root, "entry-content");
    if (!article) return;
    xmlNodePtr intro = find_class(article->children, "dynamic-entry-content");
    if (!intro) intro = article;
    size_t length = 0;
    for (xmlNodePtr node = intro->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (has_class(node, "pv2025-heading") || has_class(node, "pv2025-wrap") ||
            has_class(node, "ams-content")) break;
        if (!is_element(node, "p") || length + 3 >= PORT_DESCRIPTION_BYTES) continue;
        size_t start = length ? length + 2 : 0;
        description[start] = 0;
        xmlChar *content = xmlNodeGetContent(node);
        copy_text(description + start, PORT_DESCRIPTION_BYTES - start, content);
        xmlFree(content);
        if (description[start]) {
            if (length) memcpy(description + length, "\n\n", 2);
            length = start + strlen(description + start);
        }
    }

    xmlNodePtr track = find_class(article->children, "pv2025-track");
    for (xmlNodePtr slide = track ? track->children : NULL;
         slide && *count < CATALOG_SCREENSHOTS; slide = slide->next) {
        if (slide->type != XML_ELEMENT_NODE || !has_class(slide, "pv2025-slide")) continue;
        xmlNodePtr img = find_tag(slide->children, "img");
        if (!img) continue;
        xmlChar *value = xmlGetProp(img, BAD_CAST "data-src");
        if (!value || !*value) {
            xmlFree(value);
            value = xmlGetProp(img, BAD_CAST "src");
        }
        // WordPress can leave Unicode characters unescaped in image paths.
        xmlChar *encoded = value ? xmlURIEscapeStr(value, BAD_CAST ":/?&=%#+,;@!$'()*[]") : NULL;
        char url[CATALOG_URL];
        bool valid = site_url(encoded, url, CATALOG_ROMSNS) &&
            !strncmp(url, ROMSNS "/wp-content/uploads/", sizeof(ROMSNS "/wp-content/uploads/") - 1);
        xmlFree(encoded);
        xmlFree(value);
        for (size_t i = 0; valid && i < *count; ++i)
            if (!strcmp(screenshots[i], url)) valid = false;
        if (valid) strcpy(screenshots[(*count)++], url);
    }
}

bool catalog_read_romsns_media(const char *page, atomic_bool *cancel,
                               char description[PORT_DESCRIPTION_BYTES],
                               char screenshots[CATALOG_SCREENSHOTS][CATALOG_URL], size_t *count) {
    description[0] = 0;
    *count = 0;
    char normalized[CATALOG_URL];
    if (!game_page(BAD_CAST page, normalized, CATALOG_ROMSNS) || atomic_load(cancel)) return false;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    CatalogFetchStatus status;
    htmlDocPtr doc = fetch_page(curl, cancel, normalized, &status, false);
    curl_easy_cleanup(curl);
    if (!doc) return false;
    read_romsns_media(xmlDocGetRootElement(doc), description, screenshots, count);
    xmlFreeDoc(doc);
    return true;
}

static void load_details(CatalogJob *job) {
    GameDetails *details = &job->details;
    strcpy(details->title, job->game.title);
    strcpy(details->cover_url, job->game.cover);
    CURL *curl = curl_easy_init();
    CatalogFetchStatus status = {.curl = CURLE_FAILED_INIT};
    bool redirects = job->source != CATALOG_ROMSNS;
    htmlDocPtr doc = curl ? fetch_page(curl, &job->cancel, job->game.page, &status, redirects) : NULL;
    if (!doc && curl && status.curl == CURLE_OPERATION_TIMEDOUT && !atomic_load(&job->cancel))
        doc = fetch_page(curl, &job->cancel, job->game.page, &status, redirects);
    job->http = status.http;
    if (doc) {
        xmlNodePtr root = xmlDocGetRootElement(doc);
        if (job->source == CATALOG_ROMSHQ) {
            xmlNodePtr files = romshq_section(root, "Download Links", "download");
            if (files) read_romshq_files(files->children, details);
            for (size_t i = 0; i < details->count; ++i) {
                GameFile *file = &details->files[i];
                if (!file->size[0] && !strcmp(file->title, "Базова гра")) {
                    char size[32];
                    if (romshq_base_size(root, size))
                        snprintf(file->size, sizeof(file->size), "≈ %.26s", size);
                }
            }
            xmlNodePtr gallery = romshq_section(root, "Screenshots", "screenshots");
            if (gallery) read_romshq_screenshots(gallery->children, details);
        } else {
            bool romsns = job->source == CATALOG_ROMSNS;
            read_files(root, details, romsns);
            char page[CATALOG_URL];
            if (romsns && !details->count && !atomic_load(&job->cancel) && romsns_download_page(root, page)) {
                htmlDocPtr intermediate = fetch_page(curl, &job->cancel, page, &status, false);
                if (!intermediate && status.curl == CURLE_OPERATION_TIMEDOUT && !atomic_load(&job->cancel))
                    intermediate = fetch_page(curl, &job->cancel, page, &status, false);
                if (intermediate) {
                    read_files(xmlDocGetRootElement(intermediate), details, true);
                    xmlFreeDoc(intermediate);
                } else {
                    job->http = status.http;
                    if (status.curl) snprintf(job->error, sizeof(job->error),
                        tr("Сторінка файлів недоступна · cURL %d"), status.curl);
                    else snprintf(job->error, sizeof(job->error),
                        tr("Сторінка файлів недоступна · HTTP %ld"), status.http);
                }
            }
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
            snprintf(job->error, sizeof(job->error), tr("Сторінка гри недоступна · cURL %d"), status.curl);
        else
            snprintf(job->error, sizeof(job->error), tr("Сторінка гри недоступна · HTTP %ld"), status.http);
    }
    if (curl) curl_easy_cleanup(curl);
}

void catalog_request(CatalogJob *job) {
    load_details(job);
}

static const char *size_probe_url(const GameFile *file) {
    if ((file->source == FILE_DATANODES || file->source == FILE_ROMSHQ_SIGNED) &&
        !strncmp(file->url, DATANODES, sizeof(DATANODES) - 1)) return file->url;
    if ((file->source == FILE_1FICHIER || file->source == FILE_ROMSHQ_SIGNED) &&
        !strncmp(file->url, FICHIER, sizeof(FICHIER) - 1)) return file->url;
    return NULL;
}

bool catalog_can_probe_size(const GameFile *file) {
    if (file->source == FILE_DLARCHIVE)
        return file->id[0] && !file->id[strspn(file->id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")];
    return (!file->size[0] || !strncmp(file->size, "≈ ", sizeof("≈ ") - 1)) && size_probe_url(file);
}

static uint64_t dlarchive_size(CURL *curl, const GameFile *file, atomic_bool *cancel) {
    char url[CATALOG_URL];
    snprintf(url, sizeof(url), "https://uptobox.dlarchive.my.id/files_api.php?action=file-info&id=%s", file->id);
    struct curl_slist *headers = curl_slist_append(NULL, "Origin: https://drive.dlarchive.my.id");
    if (!headers) return 0;
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_REFERER, "https://drive.dlarchive.my.id/");
    CatalogFetchStatus status;
    Buffer data = fetch(curl, cancel, url, 32 * 1024, &status, false, false, false);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(headers);
    if (!data.data) return 0;
    const char *start = (const char *)data.data;
    Json body = {whitespace(start, start + data.size), start + data.size};
    Json value = field(field(body, "file"), "size");
    uint64_t size = 0;
    if (literal(field(body, "ok"), "true") && value.begin) {
        const char *digits = value.begin;
        const char *end = value.end;
        if (end - digits >= 2 && *digits == '"' && end[-1] == '"') {
            ++digits;
            --end;
        }
        while (digits < end && *digits >= '0' && *digits <= '9') {
            unsigned digit = (unsigned)(*digits++ - '0');
            if (size > (UINT64_MAX - digit) / 10) {
                size = 0;
                break;
            }
            size = size * 10 + digit;
        }
        if (digits != end) size = 0;
    }
    free(data.data);
    return size;
}

static uint64_t head_size(CURL *curl, const char *url, atomic_bool *cancel) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, should_cancel);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (curl_easy_perform(curl) != CURLE_OK) return 0;
    long http = 0;
    char *type = NULL;
    curl_off_t length = -1;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
    if (http != 200 || length <= 0 || !type) return 0;
    if (strncasecmp(type, "application/octet-stream", 24) && strncasecmp(type, "application/zip", 15) &&
        strncasecmp(type, "application/x-zip", 17) && strncasecmp(type, "application/x-rar", 17) &&
        strncasecmp(type, "application/vnd.rar", 19)) return 0;
    return (uint64_t)length;
}

static bool datanodes_page_size(htmlDocPtr doc, const char *file, char size[32]) {
    const char *id = file + sizeof(DATANODES) - 1;
    size_t id_length = strcspn(id, "/?#");
    bool matches = false;
    char label[32] = {0};
    xmlNodePtr head = find_tag(xmlDocGetRootElement(doc), "head");
    for (xmlNodePtr node = head ? head->children : NULL; node; node = node->next) {
        if (!is_element(node, "meta")) continue;
        xmlChar *property = xmlGetProp(node, BAD_CAST "property");
        xmlChar *content = xmlGetProp(node, BAD_CAST "content");
        const char *text = (const char *)content;
        if (text && id_length && !xmlStrcmp(property, BAD_CAST "og:url") &&
            !strncmp(text, DATANODES, sizeof(DATANODES) - 1)) {
            const char *canonical = text + sizeof(DATANODES) - 1;
            matches = strcspn(canonical, "/?#") == id_length && !memcmp(canonical, id, id_length);
        } else if (text && !xmlStrcmp(property, BAD_CAST "og:title")) {
            const char *suffix = strrchr(text, '(');
            if (suffix && text[strlen(text) - 1] == ')') labeled_size(suffix + 1, label);
        }
        xmlFree(content);
        xmlFree(property);
    }
    if (!matches || !label[0]) return false;
    strcpy(size, label);
    return true;
}

static void datanodes_size(CURL *curl, const char *file, atomic_bool *cancel, char size[32]) {
    // The public file page can redirect to /download with a session cookie.
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    char url[CATALOG_URL];
    strcpy(url, file);
    for (unsigned hop = 0; hop < 4 && !atomic_load(cancel); ++hop) {
        CatalogFetchStatus status;
        Buffer body = fetch(curl, cancel, url, 256 * 1024, &status, false, false, true);
        if (body.data) {
            htmlDocPtr doc = htmlReadMemory((char *)body.data, (int)body.size, url, "UTF-8",
                HTML_PARSE_NONET | HTML_PARSE_NOWARNING | HTML_PARSE_NOERROR);
            free(body.data);
            if (doc) {
                datanodes_page_size(doc, file, size);
                xmlFreeDoc(doc);
            }
            return;
        }
        char *next = NULL;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &next);
        if (status.http < 300 || status.http >= 400 || !next ||
            strncmp(next, DATANODES, sizeof(DATANODES) - 1) ||
            strlen(next) >= sizeof(url) || !strcmp(url, next)) return;
        strcpy(url, next);
    }
}

void catalog_probe_size(GameFile *file, atomic_bool *cancel) {
    if (!catalog_can_probe_size(file) || atomic_load(cancel)) return;
    CURL *curl = curl_easy_init();
    if (!curl) return;
    const char *url = size_probe_url(file);
    uint64_t size = file->source == FILE_DLARCHIVE ? dlarchive_size(curl, file, cancel) :
                    head_size(curl, url, cancel);
    if (!size && url && !strncmp(url, DATANODES, sizeof(DATANODES) - 1))
        datanodes_size(curl, url, cancel, file->size);
    curl_easy_cleanup(curl);
    if (size) {
        static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
        double value = (double)size;
        unsigned unit = 0;
        while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
            value /= 1024.0;
            ++unit;
        }
        if (!unit) snprintf(file->size, sizeof(file->size), "%" PRIu64 " B", size);
        else snprintf(file->size, sizeof(file->size), "%.2f %s", value, units[unit]);
    }
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

static const char *cache_stem(CatalogSource source) {
    static const char *const stems[CATALOG_SOURCE_COUNT] = {"catalog", "romshq", "romsns"};
    return stems[source];
}

bool catalog_read_cache(CatalogIndex *index, CatalogSource source) {
    index->count = 0;
    index->saved_at = 0;
    char path[CATALOG_URL], backup[CATALOG_URL];
    const char *stem = cache_stem(source);
    snprintf(path, sizeof(path), NXLOAD_DOWNLOAD_DIR "/%s.bin", stem);
    snprintf(backup, sizeof(backup), NXLOAD_DOWNLOAD_DIR "/%s.prev", stem);
    bool cached = read_cache_path(path, index, source) || read_cache_path(backup, index, source);
    return (source == CATALOG_ROMSHQ && read_romshq_seed(index)) || cached;
}

static bool save_cache(const CatalogIndex *index, CatalogSource source) {
    char dest[CATALOG_URL], temp[CATALOG_URL], backup[CATALOG_URL];
    const char *stem = cache_stem(source);
    snprintf(dest, sizeof(dest), NXLOAD_DOWNLOAD_DIR "/%s.bin", stem);
    snprintf(temp, sizeof(temp), NXLOAD_DOWNLOAD_DIR "/%s.tmp", stem);
    snprintf(backup, sizeof(backup), NXLOAD_DOWNLOAD_DIR "/%s.prev", stem);
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
                snprintf(job->error, sizeof(job->error), tr("Стор. %u · TLS 60 · X.509 0x%lX"), page, status.verify);
            else if (status.curl)
                snprintf(job->error, sizeof(job->error), tr("Сторінка %u · cURL %d"), page, status.curl);
            else
                snprintf(job->error, sizeof(job->error), tr("Сторінка %u · HTTP %ld"), page, status.http);
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
            else snprintf(job->error, sizeof(job->error), tr("Каталог · немає ігор на сторінці %u"), page);
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
