#define _GNU_SOURCE
#include "eshop.h"

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SEARCH "https://searching.nintendo-europe.com/en/select?wt=xml&rows=24&fq=type%3AGAME%20AND%20playable_on_txt%3AHAC&q="
#define OFFICIAL "https://www.nintendo.com"
#define UK_GAMES "/en-gb/Games/Nintendo-Switch-games/"
#define SWITCH_GAMES "/Games/Nintendo-Switch-games/"
#define MEDIA "/eu/media/images/"
#define CATALOG_ROMS "https://nsroms.net/roms/"
#define US_PRODUCTS OFFICIAL "/us/store/products/"
#define US_IMAGES "https://assets.nintendo.com/image/upload/"
#define US_GAME_ART "/store/software/switch/"

static bool begins(const char *value, const char *prefix) {
    return !strncmp(value, prefix, strlen(prefix));
}

static bool ends(const char *value, const char *suffix) {
    size_t length = strlen(value), n = strlen(suffix);
    return length >= n && !strcasecmp(value + length - n, suffix);
}

static void clean_text(char *out, size_t cap, const char *value) {
    size_t used = 0;
    bool space = false;
    while (value && *value) {
        unsigned char c = (unsigned char)*value;
        if (c <= ' ') {
            space = used != 0;
            ++value;
            continue;
        }
        size_t width = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4;
        size_t i = 1;
        while (i < width && value[i] && ((unsigned char)value[i] & 0xc0) == 0x80) ++i;
        if (i != width || used + width + space >= cap) break;
        if (space) out[used++] = ' ';
        memcpy(out + used, value, width);
        used += width;
        value += width;
        space = false;
    }
    out[used] = 0;
}

// Keep edition names, numbers, and meaningful symbols when comparing titles.
static void title_key(char out[CATALOG_TEXT], const char *value, bool search_words) {
    size_t used = 0;
    bool space = false;
    while (*value && used + 5 < CATALOG_TEXT) {
        const unsigned char c = (unsigned char)*value;
        if (begins(value, "™") || begins(value, "®") || begins(value, "©")) {
            value += c < 224 ? 2 : 3;
            continue;
        }
        if (begins(value, "\xc2\xa0") || begins(value, "–") || begins(value, "—") ||
            begins(value, "’") || begins(value, "‘") || begins(value, "…")) {
            if (!begins(value, "’") && !begins(value, "‘")) space = used != 0;
            value += c < 224 ? 2 : 3;
            continue;
        }
        if (c < 128) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                (!search_words && (c == '+' || c == '&'))) {
                if (search_words && space) out[used++] = ' ';
                out[used++] = c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : (char)c;
                space = false;
            } else if (c != '\'') {
                space = used != 0;
            }
            ++value;
            continue;
        }
        size_t width = c < 224 ? 2 : c < 240 ? 3 : 4;
        size_t i = 1;
        while (i < width && value[i] && ((unsigned char)value[i] & 0xc0) == 0x80) ++i;
        if (i != width) break;
        if (search_words && space) out[used++] = ' ';
        memcpy(out + used, value, width);
        used += width;
        value += width;
        space = false;
    }
    out[used] = 0;
}

static void trim_site_suffix(char *title) {
    const char *suffixes[] = {
        "rom download free", "rom free download", "rom download", "rom",
        "switch download free", "switch free download", "switch download",
        "nintendo switch", "switch", "nsp", "xci", "nsz", "xcz"
    };
    for (;;) {
        size_t end = strlen(title);
        while (end && strchr(" )]", title[end - 1])) --end;
        size_t cut = end;
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
            size_t size = strlen(suffixes[i]);
            if (end <= size || !strchr(" -|:/([", title[end - size - 1]) ||
                strncasecmp(title + end - size, suffixes[i], size)) continue;
            cut = end - size;
            while (cut && strchr(" -|:/([", title[cut - 1])) --cut;
            break;
        }
        if (cut == end || !cut) return;
        title[cut] = 0;
    }
}

static bool product_url(const char *value, char out[CATALOG_URL]) {
    if (!value) return false;
    if (begins(value, OFFICIAL)) value += sizeof(OFFICIAL) - 1;
    const char *prefix = "";
    if (begins(value, SWITCH_GAMES)) prefix = "/en-gb";
    else if (!begins(value, UK_GAMES)) return false;
    if (!ends(value, ".html") || strpbrk(value, "?#\\ \t\r\n") ||
        strstr(value, "/../") || strstr(value, "%")) return false;
    int n = snprintf(out, CATALOG_URL, OFFICIAL "%s%s", prefix, value);
    return n > 0 && n < CATALOG_URL;
}

static bool artwork_url(const char *value, char out[CATALOG_URL]) {
    if (!value) return false;
    if (begins(value, OFFICIAL)) value += sizeof(OFFICIAL) - 1;
    if (begins(value, "/media/images/")) {
        int n = snprintf(out, CATALOG_URL, OFFICIAL "/eu%s", value);
        return n > 0 && n < CATALOG_URL;
    }
    if (!begins(value, MEDIA)) return false;
    int n = snprintf(out, CATALOG_URL, OFFICIAL "%s", value);
    return n > 0 && n < CATALOG_URL;
}

static xmlNodePtr result_list(xmlNodePtr node) {
    for (; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && !xmlStrcmp(node->name, BAD_CAST "result"))
            return node->children;
        xmlNodePtr nested = result_list(node->children);
        if (nested) return nested;
    }
    return NULL;
}

static xmlChar *field(xmlNodePtr doc, const char *name) {
    for (xmlNodePtr child = doc->children; child; child = child->next) {
        xmlChar *key = xmlGetProp(child, BAD_CAST "name");
        bool match = key && !xmlStrcmp(key, BAD_CAST name);
        xmlFree(key);
        if (match) return xmlNodeGetContent(child);
    }
    return NULL;
}

static void text_field(xmlNodePtr doc, const char *name, char *out, size_t capacity) {
    xmlChar *value = field(doc, name);
    clean_text(out, capacity, (char *)value);
    xmlFree(value);
}

static void read_record(xmlNodePtr doc, EshopGame *game) {
    text_field(doc, "title", game->title, sizeof(game->title));
    text_field(doc, "excerpt", game->description, sizeof(game->description));
    text_field(doc, "publisher", game->publisher, sizeof(game->publisher));
    text_field(doc, "date_from", game->released, sizeof(game->released));
    if (strlen(game->released) > 10) game->released[10] = 0;
    const char *images[] = {"image_url_sq_s", "image_url", "image_url_tm_s"};
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); ++i) {
        xmlChar *value = field(doc, images[i]);
        bool ok = artwork_url((char *)value, game->cover);
        xmlFree(value);
        if (ok) return;
    }
}

static void match_records(xmlDocPtr document, const char *key, const char *alternate, EshopGame *game) {
    unsigned best_rank = 0;
    char best_page[CATALOG_URL] = {0};
    xmlNodePtr best = NULL;
    bool ambiguous = false;
    for (xmlNodePtr doc = result_list(xmlDocGetRootElement(document)); doc; doc = doc->next) {
        if (doc->type != XML_ELEMENT_NODE || xmlStrcmp(doc->name, BAD_CAST "doc")) continue;
        xmlChar *name = field(doc, "title");
        char normalized[CATALOG_TEXT] = {0};
        if (name) title_key(normalized, (char *)name, false);
        xmlFree(name);
        if (!normalized[0]) continue;
        unsigned rank = !strcmp(normalized, key) ? 2 : !strcmp(normalized, alternate) ? 1 : 0;
        if (!rank || rank < best_rank) continue;

        xmlChar *value = field(doc, "url");
        char page[CATALOG_URL];
        bool valid = product_url((char *)value, page);
        xmlFree(value);
        if (!valid) continue;
        if (rank > best_rank) {
            best_rank = rank;
            best = doc;
            strcpy(best_page, page);
            ambiguous = false;
        } else if (strcmp(page, best_page)) {
            ambiguous = true;
        }
    }
    if (best && !ambiguous) {
        strcpy(game->page, best_page);
        read_record(best, game);
    }
}

static xmlDocPtr search(const char *terms, atomic_bool *cancel, CatalogFetchStatus *status) {
    if (!*terms) return NULL;
    char *escaped = curl_easy_escape(NULL, terms, 0);
    if (!escaped) return NULL;
    char url[sizeof(SEARCH) + CATALOG_TEXT * 3];
    snprintf(url, sizeof(url), "%s%s", SEARCH, escaped);
    curl_free(escaped);
    size_t size;
    unsigned char *data = catalog_fetch_document(url, cancel, &size, status);
    if (!data) return NULL;
    xmlDocPtr doc = xmlReadMemory((char *)data, (int)size, NULL, NULL,
                                 XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    free(data);
    return doc;
}

static void screenshots(xmlNodePtr node, EshopGame *game) {
    const char *attributes[] = {"data-md", "data-xs", "data-lg", "data-xl", "data-src", "src", "href"};
    for (; node && game->screenshot_count < ESHOP_SCREENSHOTS; node = node->next) {
        if (node->type == XML_ELEMENT_NODE &&
            (!xmlStrcmp(node->name, BAD_CAST "img") || !xmlStrcmp(node->name, BAD_CAST "a"))) {
            for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); ++i) {
                xmlChar *value = xmlGetProp(node, BAD_CAST attributes[i]);
                char image[CATALOG_URL];
                bool ok = value && strstr((char *)value, "/06_screenshots/") &&
                          artwork_url((char *)value, image);
                xmlFree(value);
                if (!ok) continue;
                size_t j = 0;
                while (j < game->screenshot_count && strcmp(game->screenshots[j], image)) ++j;
                if (j == game->screenshot_count) strcpy(game->screenshots[game->screenshot_count++], image);
                break;
            }
        }
        screenshots(node->children, game);
    }
}

static bool product_summary(xmlNodePtr node, const char *wanted, bool *after_title, EshopGame *game) {
    for (; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && !xmlStrcmp(node->name, BAD_CAST "h1")) {
            xmlChar *name = xmlNodeGetContent(node);
            char key[CATALOG_TEXT] = {0};
            if (name) title_key(key, (char *)name, false);
            xmlFree(name);
            if (!strcmp(key, wanted)) *after_title = true;
        } else if (*after_title && node->type == XML_ELEMENT_NODE &&
                   !xmlStrcmp(node->name, BAD_CAST "p")) {
            xmlChar *value = xmlNodeGetContent(node);
            char paragraph[sizeof(game->description)];
            clean_text(paragraph, sizeof(paragraph), (char *)value);
            xmlFree(value);
            size_t length = strlen(paragraph);
            if (length >= 120 && length > strlen(game->description)) {
                strcpy(game->description, paragraph);
                return true;
            }
        }
        if (product_summary(node->children, wanted, after_title, game)) return true;
    }
    return false;
}

static htmlDocPtr fetch_product(const char *url, atomic_bool *cancel, CatalogFetchStatus *status) {
    size_t size;
    unsigned char *data = catalog_fetch_document(url, cancel, &size, status);
    if (!data) return NULL;
    htmlDocPtr page = htmlReadMemory((char *)data, (int)size, NULL, "UTF-8",
                                    HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    free(data);
    return page;
}

static void request_issue(EshopGame *game, const char *step, CatalogFetchStatus status) {
    if (status.curl)
        snprintf(game->issue, sizeof(game->issue), "%s · cURL %d", step, status.curl);
    else if (status.http && status.http != 200)
        snprintf(game->issue, sizeof(game->issue), "%s · HTTP %ld", step, status.http);
    else
        snprintf(game->issue, sizeof(game->issue), "%s · дані недоступні", step);
}

static void european_product(EshopJob *job, const char *words, const char *raw, const char *trimmed) {
    EshopGame *game = &job->match;
    CatalogFetchStatus status = {0};
    xmlDocPtr doc = search(words, &job->cancel, &status);
    if (!doc) {
        request_issue(game, "Пошук Nintendo eShop", status);
        return;
    }
    match_records(doc, raw, trimmed, game);
    xmlFreeDoc(doc);
    if (!game->page[0]) {
        strcpy(game->issue, "Точного збігу в Nintendo eShop не знайдено");
        return;
    }
    if (atomic_load(&job->cancel)) return;
    htmlDocPtr page = fetch_product(game->page, &job->cancel, &status);
    if (!page) {
        request_issue(game, "Сторінка Nintendo eShop", status);
        return;
    }
    char wanted[CATALOG_TEXT];
    title_key(wanted, game->title, false);
    bool after_title = false;
    product_summary(xmlDocGetRootElement(page), wanted, &after_title, game);
    screenshots(xmlDocGetRootElement(page), game);
    xmlFreeDoc(page);
}

static bool us_artwork(const char *src, char out[CATALOG_URL]) {
    if (!src || !begins(src, US_IMAGES)) return false;
    const char *path = strstr(src, US_GAME_ART);
    if (!path || strspn(path, "/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(path))
        return false;
    int count = snprintf(out, CATALOG_URL, US_IMAGES "f_jpg,q_auto,w_800%s", path);
    return count > 0 && count < CATALOG_URL;
}

static xmlChar *us_title(xmlNodePtr node, const char *raw, const char *trimmed) {
    for (; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && !xmlStrcmp(node->name, BAD_CAST "h1")) {
            xmlChar *name = xmlNodeGetContent(node);
            char key[CATALOG_TEXT];
            title_key(key, name ? (char *)name : "", false);
            if (key[0] && (!strcmp(key, raw) || !strcmp(key, trimmed))) return name;
            xmlFree(name);
        }
        xmlChar *nested = us_title(node->children, raw, trimmed);
        if (nested) return nested;
    }
    return NULL;
}

static void us_images(xmlNodePtr node, const char *title, EshopGame *game) {
    size_t length = strlen(title);
    for (; node && game->screenshot_count < ESHOP_SCREENSHOTS; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && !xmlStrcmp(node->name, BAD_CAST "img")) {
            xmlChar *alt = xmlGetProp(node, BAD_CAST "alt");
            if (alt && begins((char *)alt, title) && alt[length] == ' ' &&
                alt[length + 1] && !alt[length + 1 + strspn((char *)alt + length + 1, "0123456789")]) {
                xmlChar *src = xmlGetProp(node, BAD_CAST "src");
                char image[CATALOG_URL];
                if (us_artwork((char *)src, image)) {
                    size_t i = 0;
                    while (i < game->screenshot_count && strcmp(game->screenshots[i], image)) ++i;
                    if (i == game->screenshot_count) strcpy(game->screenshots[game->screenshot_count++], image);
                }
                xmlFree(src);
            }
            xmlFree(alt);
        } else if (!game->cover[0] && node->type == XML_ELEMENT_NODE &&
                   !xmlStrcmp(node->name, BAD_CAST "meta")) {
            xmlChar *property = xmlGetProp(node, BAD_CAST "property");
            if (!xmlStrcmp(property, BAD_CAST "og:image")) {
                xmlChar *src = xmlGetProp(node, BAD_CAST "content");
                us_artwork((char *)src, game->cover);
                xmlFree(src);
            }
            xmlFree(property);
        }
        us_images(node->children, title, game);
    }
}

static void american_product(EshopJob *job, const char *raw, const char *trimmed) {
    if (!begins(job->game.page, CATALOG_ROMS)) return;
    const char *slug = job->game.page + sizeof(CATALOG_ROMS) - 1;
    size_t length = strspn(slug, "abcdefghijklmnopqrstuvwxyz0123456789-");
    const size_t suffix = sizeof("-switch") - 1;
    if (length <= suffix || (slug[length] && strcmp(slug + length, "/")) ||
        memcmp(slug + length - suffix, "-switch", suffix))
        return;
    char url[CATALOG_URL];
    int size = snprintf(url, sizeof(url), US_PRODUCTS "%.*s/", (int)length, slug);
    if (size <= 0 || size >= CATALOG_URL) return;

    CatalogFetchStatus status = {0};
    htmlDocPtr page = fetch_product(url, &job->cancel, &status);
    if (!page) {
        if (!job->match.issue[0]) request_issue(&job->match, "Сторінка Nintendo Store", status);
        return;
    }
    xmlNodePtr root = xmlDocGetRootElement(page);
    xmlChar *name = us_title(root, raw, trimmed);
    if (name) {
        EshopGame *game = &job->match;
        if (!game->page[0]) {
            strcpy(game->page, url);
            clean_text(game->title, sizeof(game->title), (char *)name);
        }
        bool after_title = false;
        char wanted[CATALOG_TEXT];
        title_key(wanted, (char *)name, false);
        product_summary(root, wanted, &after_title, game);
        if (!game->screenshot_count) us_images(root, (char *)name, game);
        if (game->description[0] && game->screenshot_count) game->issue[0] = 0;
    }
    xmlFree(name);
    xmlFreeDoc(page);
}

void eshop_lookup(EshopJob *job) {
    char words[CATALOG_TEXT], raw_key[CATALOG_TEXT], trimmed_key[CATALOG_TEXT], title[CATALOG_TEXT];
    const char *name = job->heading[0] ? job->heading : job->game.title;
    clean_text(title, sizeof(title), name);
    trim_site_suffix(title);
    title_key(raw_key, name, false);
    title_key(words, title, true);
    title_key(trimmed_key, title, false);
    european_product(job, words, raw_key, trimmed_key);
    if ((!job->match.description[0] || !job->match.screenshot_count) && !atomic_load(&job->cancel))
        american_product(job, raw_key, trimmed_key);
}
