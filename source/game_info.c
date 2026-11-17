#define _GNU_SOURCE
#include "game_info.h"

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#ifndef TITLEDB_PATH
#define TITLEDB_PATH "romfs:/titledb.bin"
#endif

#define NL "https://www.nintendolife.com"
#define NL_SWITCH "/games/nintendo-switch/"
#define NL_ESHOP "/games/switch-eshop/"
#define NL_IMAGE "https://images.nintendolife.com/screenshots/"
#define SOURCE_PAGE "https://nsroms.net/roms/"

static bool begins(const char *value, const char *prefix) {
    return !strncmp(value, prefix, strlen(prefix));
}

static void title_key(char key[CATALOG_TEXT], const char *name) {
    size_t used = 0;
    while (*name) {
        unsigned char c = (unsigned char)*name;
        if (begins(name, "™") || begins(name, "®") || begins(name, "©") ||
            begins(name, "\xc2\xa0") || begins(name, "–") || begins(name, "—") ||
            begins(name, "’") || begins(name, "‘") || begins(name, "…")) {
            name += c < 224 ? 2 : 3;
            continue;
        }
        if (c < 128) {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '&') {
                if (used + 1 >= CATALOG_TEXT) {
                    used = 0;
                    break;
                }
                key[used++] = (char)c;
            }
            ++name;
            continue;
        }
        size_t width = c < 224 ? 2 : c < 240 ? 3 : 4;
        size_t i = 1;
        while (i < width && name[i] && ((unsigned char)name[i] & 0xc0) == 0x80) ++i;
        if (i != width || used + width >= CATALOG_TEXT) {
            used = 0;
            break;
        }
        memcpy(key + used, name, width);
        used += width;
        name += width;
    }
    key[used] = 0;
}

static bool remove_suffix(char *name, const char *suffix) {
    const char *separators = " -|:/([";
    size_t end = strlen(name), size = strlen(suffix);
    while (end && strchr(" )]", name[end - 1])) --end;
    if (end <= size || !strchr(separators, name[end - size - 1]) ||
        strncasecmp(name + end - size, suffix, size)) return false;
    end -= size;
    while (end && strchr(separators, name[end - 1])) --end;
    if (!end) return false;
    name[end] = 0;
    return true;
}

static void clean_heading(char *name) {
    static const char * const suffixes[] = {
        "rom download free", "rom free download", "rom download", "rom",
        "switch download free", "switch free download", "switch download", "nsp", "xci", "nsz", "xcz"
    };
    bool removed;
    do {
        removed = false;
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(*suffixes); ++i) {
            if (remove_suffix(name, suffixes[i])) {
                removed = true;
                break;
            }
        }
    } while (removed);
}

static bool read_u32(FILE *file, uint32_t *value) {
    unsigned char b[4];
    if (fread(b, 1, sizeof(b), file) != sizeof(b)) return false;
    *value = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return true;
}

static bool read_key(FILE *file, char key[CATALOG_TEXT]) {
    for (size_t i = 0; i < CATALOG_TEXT; ++i) {
        int c = fgetc(file);
        if (c == EOF) return false;
        key[i] = (char)c;
        if (!c) return true;
    }
    return false;
}

static bool read_metadata_record(FILE *file, GameMetadata *metadata) {
    unsigned char size[2], compressed[sizeof(*metadata) + 64];
    char decoded[sizeof(*metadata)];
    if (fread(size, 1, sizeof(size), file) != sizeof(size)) return false;
    unsigned length = size[0] | (unsigned)size[1] << 8;
    if (!length || length > sizeof(compressed) || fread(compressed, 1, length, file) != length)
        return false;
    unsigned long available = sizeof(decoded);
    if (uncompress((unsigned char *)decoded, &available, compressed, length) != Z_OK) return false;
    char *inputs = decoded;
    GameMetadata result = {0};
    char *fields[] = {result.title, result.publisher, result.released, result.description};
    const size_t capacities[] = {sizeof(result.title), sizeof(result.publisher),
                                 sizeof(result.released), sizeof(result.description)};
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
        char *end = memchr(inputs, 0, available);
        if (!end || (size_t)(end - inputs) >= capacities[i]) return false;
        size_t bytes = (size_t)(end - inputs) + 1;
        memcpy(fields[i], inputs, bytes);
        inputs += bytes;
        available -= bytes;
    }
    *metadata = result;
    return metadata->title[0] != 0;
}

static bool find_metadata(FILE *file, uint32_t count, const char *name, GameMetadata *metadata) {
    char wanted[CATALOG_TEXT];
    title_key(wanted, name);
    if (!wanted[0]) return false;
    uint32_t low = 0, high = count;
    while (low < high) {
        uint32_t row = low + (high - low) / 2, offset;
        char key[CATALOG_TEXT];
        if (fseek(file, 12L + 4L * row, SEEK_SET) || !read_u32(file, &offset) ||
            offset < 12 + 4 * count || offset > 128 * 1024 * 1024 ||
            fseek(file, (long)offset, SEEK_SET) || !read_key(file, key)) return false;
        int order = strcmp(key, wanted);
        if (!order) return read_metadata_record(file, metadata);
        if (order < 0) low = row + 1;
        else high = row;
    }
    return false;
}

static bool metadata_candidates(FILE *file, uint32_t count, const char *title,
                                const char *heading, GameMetadata *metadata) {
    char names[2][CATALOG_TEXT];
    snprintf(names[0], sizeof(names[0]), "%s", title);
    snprintf(names[1], sizeof(names[1]), "%s", heading);
    clean_heading(names[0]);
    clean_heading(names[1]);
    for (size_t i = 0; i < 2; ++i)
        if (find_metadata(file, count, names[i], metadata)) return true;
    for (size_t i = 0; i < 2; ++i)
        if ((remove_suffix(names[i], "nintendo switch") || remove_suffix(names[i], "switch")) &&
            find_metadata(file, count, names[i], metadata)) return true;
    return false;
}

bool read_game_metadata(GameMetadata *metadata, const char *title, const char *heading) {
    FILE *file = fopen(TITLEDB_PATH, "rb");
    if (!file) return false;
    char magic[8];
    uint32_t count = 0;
    bool found = fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
                 !memcmp(magic, "NXLTDB01", sizeof(magic)) && read_u32(file, &count) &&
                 count <= 100000 && metadata_candidates(file, count, title, heading, metadata);
    fclose(file);
    return found;
}

static void make_slug(char slug[CATALOG_TEXT], const char *title) {
    size_t used = 0;
    bool separator = false;
    while (*title && used + 2 < CATALOG_TEXT) {
        unsigned char c = (unsigned char)*title++;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            if (separator && used && c != '-' && slug[used - 1] != '-') slug[used++] = '_';
            slug[used++] = (char)c;
            separator = false;
        } else if (c == ' ' || c == '_' || c == ':' || c == '/') {
            separator = true;
        }
    }
    slug[used] = 0;
}

static bool match_heading(xmlNodePtr node, const char *key, const char *alternate) {
    for (; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && !xmlStrcmp(node->name, BAD_CAST "h1")) {
            xmlChar *content = xmlNodeGetContent(node);
            char actual[CATALOG_TEXT];
            title_key(actual, content ? (char *)content : "");
            xmlFree(content);
            if (actual[0] && (!strcmp(actual, key) || !strcmp(actual, alternate))) return true;
        }
        if (match_heading(node->children, key, alternate)) return true;
    }
    return false;
}

static bool valid_screenshot(const char *url) {
    if (!url || !begins(url, NL_IMAGE) || strlen(url) >= CATALOG_URL) return false;
    const char *id = url + sizeof(NL_IMAGE) - 1;
    size_t digits = strspn(id, "0123456789");
    return digits && !strcmp(id + digits, "/large.jpg");
}

static void extract_gallery(xmlNodePtr node, GameGallery *gallery, bool in_gallery) {
    for (; node && gallery->count < GALLERY_LIMIT; node = node->next) {
        bool inside = in_gallery;
        if (node->type == XML_ELEMENT_NODE) {
            if (!xmlStrcmp(node->name, BAD_CAST "section")) {
                xmlChar *id = xmlGetProp(node, BAD_CAST "id");
                inside |= !xmlStrcmp(id, BAD_CAST "screenshots");
                xmlFree(id);
            } else if (inside && !xmlStrcmp(node->name, BAD_CAST "a")) {
                xmlChar *href = xmlGetProp(node, BAD_CAST "href");
                if (valid_screenshot((char *)href)) {
                    size_t i = 0;
                    while (i < gallery->count && strcmp(gallery->screenshots[i], (char *)href)) ++i;
                    if (i == gallery->count) strcpy(gallery->screenshots[gallery->count++], (char *)href);
                }
                xmlFree(href);
            }
        }
        extract_gallery(node->children, gallery, inside);
    }
}

static bool fetch_gallery(GalleryJob *job, const char *url, const char *key, const char *alternate,
                          CatalogFetchStatus *status) {
    size_t size;
    unsigned char *data = catalog_fetch_document(url, &job->cancel, &size, status);
    if (!data) return false;
    htmlDocPtr page = htmlReadMemory((char *)data, (int)size, url, "UTF-8",
                                    HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    free(data);
    if (!page) return false;
    xmlNodePtr root = xmlDocGetRootElement(page);
    if (match_heading(root, key, alternate)) extract_gallery(root, &job->result, false);
    xmlFreeDoc(page);
    return job->result.count != 0;
}

void load_game_gallery(GalleryJob *job) {
    char name[CATALOG_TEXT], key[CATALOG_TEXT], alternate[CATALOG_TEXT] = {0};
    snprintf(name, sizeof(name), "%s", job->title[0] ? job->title :
             job->heading[0] ? job->heading : job->catalog_title);
    if (!job->title[0]) clean_heading(name);
    title_key(key, name);
    char slugs[2][CATALOG_TEXT] = {{0}};
    make_slug(slugs[0], name);
    if (!job->title[0]) {
        if (remove_suffix(name, "nintendo switch") || remove_suffix(name, "switch")) {
            title_key(alternate, name);
            make_slug(slugs[1], name);
        }
    } else if (begins(job->page, SOURCE_PAGE)) {
        const char *slug = job->page + sizeof(SOURCE_PAGE) - 1;
        size_t size = strspn(slug, "abcdefghijklmnopqrstuvwxyz0123456789-");
        if (size >= sizeof("-switch") && size < sizeof(slugs[1]) &&
            !memcmp(slug + size - (sizeof("-switch") - 1), "-switch", sizeof("-switch") - 1))
            snprintf(slugs[1], sizeof(slugs[1]), "%.*s", (int)(size - (sizeof("-switch") - 1)), slug);
    }

    CatalogFetchStatus status = {0};
    const char *platforms[] = {NL_SWITCH, NL_ESHOP};
    for (size_t i = 0; i < 2 && !atomic_load(&job->cancel); ++i) {
        if (!slugs[i][0] || (i && !strcmp(slugs[0], slugs[1]))) continue;
        for (size_t j = 0; j < 2 && !atomic_load(&job->cancel); ++j) {
            char url[CATALOG_URL];
            snprintf(url, sizeof(url), NL "%s%s", platforms[j], slugs[i]);
            if (fetch_gallery(job, url, key, alternate, &status)) return;
            if (status.curl) {
                snprintf(job->result.issue, sizeof(job->result.issue),
                         "Nintendo Life · cURL %d", status.curl);
                return;
            }
            if (status.http && status.http != 200 && status.http != 404) {
                snprintf(job->result.issue, sizeof(job->result.issue),
                         "Nintendo Life · HTTP %ld", status.http);
                return;
            }
        }
    }
    snprintf(job->result.issue, sizeof(job->result.issue), "%s",
             "Скриншоти гри в Nintendo Life не знайдено");
}
