#define _GNU_SOURCE
#include "language.h"
#include "game_info.h"
#include "trailers.h"
#include "steam_media.h"
#include "json.h"

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
#ifndef BUILTIN_GALLERY_PATH
#define BUILTIN_GALLERY_PATH "romfs:/gallery/"
#endif
#define BUILTIN_IMAGE_LIMIT (2 * 1024 * 1024)
#ifndef CATALOG_META_PATH
#define CATALOG_META_PATH "romfs:/catalog-meta.tsv"
#endif

#define NLIB_SCREEN_API "https://api.nlib.cc/nx/"
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

static bool clean_heading(char *name) {
    static const char * const suffixes[] = {
        "rom download free", "rom free download", "rom download", "rom", "download free", "download",
        "nsp", "xci", "nsz", "xcz"
    };
    if (!strncasecmp(name, "download", 8) && name[8] && strchr(" -:|", name[8])) {
        const char *title = name + 8 + strspn(name + 8, " -:|");
        if (*title) memmove(name, title, strlen(title) + 1);
    }
    bool decorated = false;
    bool removed;
    do {
        removed = false;
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(*suffixes); ++i) {
            if (remove_suffix(name, suffixes[i])) {
                removed = true;
                decorated = true;
                break;
            }
        }
    } while (removed);
    return decorated;
}

typedef struct {
    const char *key;
    CatalogMetadata value;
} MetadataRow;

static char *metadata_text;
static MetadataRow *metadata_rows;
static size_t metadata_count;

void free_catalog_metadata(void) {
    free(metadata_rows);
    free(metadata_text);
    metadata_rows = NULL;
    metadata_text = NULL;
    metadata_count = 0;
}

bool load_catalog_metadata(void) {
    free_catalog_metadata();
    FILE *file = fopen(CATALOG_META_PATH, "rb");
    if (!file) return false;
    long size = !fseek(file, 0, SEEK_END) ? ftell(file) : -1;
    if (size > 0 && size <= 24 * 1024 * 1024 && !fseek(file, 0, SEEK_SET))
        metadata_text = malloc((size_t)size + 1);
    bool read = metadata_text && fread(metadata_text, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!read) goto invalid;
    metadata_text[size] = 0;
    char *next = metadata_text;
    if (strcmp(strsep(&next, "\n"), "NXLINFO2")) goto invalid;
    size_t lines = 0;
    for (const char *p = next; p && *p; ++p) if (*p == '\n') ++lines;
    if (!lines || lines > 100000) goto invalid;
    metadata_rows = calloc(lines, sizeof(*metadata_rows));
    if (!metadata_rows) goto invalid;
    for (size_t i = 0; i < lines; ++i) {
        char *row = strsep(&next, "\n");
        const char *values[8];
        for (size_t field = 0; field < 8; ++field) {
            values[field] = strsep(&row, "\t");
            if (!values[field]) goto invalid;
        }
        if (row || !values[0][0] || !values[1][0] ||
            (i && strcmp(metadata_rows[i - 1].key, values[0]) >= 0)) goto invalid;
        metadata_rows[i] = (MetadataRow){values[0], {
            values[1], values[2], values[3], values[4], (unsigned)strtoul(values[5], NULL, 10),
            (unsigned)strtoul(values[6], NULL, 10), (unsigned)strtoul(values[7], NULL, 10),
        }};
    }
    metadata_count = lines;
    return true;

invalid:
    free_catalog_metadata();
    return false;
}

static const CatalogMetadata *exact_catalog_metadata(const char *name) {
    char key[CATALOG_TEXT];
    title_key(key, name);
    size_t first = 0, end = metadata_count;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        int comparison = strcmp(key, metadata_rows[middle].key);
        if (!comparison) return &metadata_rows[middle].value;
        if (comparison > 0) first = middle + 1;
        else end = middle;
    }
    return NULL;
}

void clean_catalog_title(char name[CATALOG_TEXT]) {
    if (!clean_heading(name) || exact_catalog_metadata(name)) return;
    char key[CATALOG_TEXT];
    title_key(key, name);
    if (!strcmp(key, "12switch") || !strcmp(key, "everybody12switch")) return;
    if (!remove_suffix(name, "nintendo switch")) remove_suffix(name, "switch");
}

static const CatalogMetadata *find_catalog_metadata(const Game *game) {
    if (!game) return NULL;
    char title[CATALOG_TEXT];
    strcpy(title, game->title);
    clean_heading(title);
    const CatalogMetadata *metadata = exact_catalog_metadata(title);
    if (!metadata && (remove_suffix(title, "nintendo switch") || remove_suffix(title, "switch")))
        metadata = exact_catalog_metadata(title);
    return metadata;
}

typedef struct {
    char key[CATALOG_TEXT];
    const CatalogMetadata *metadata;
    size_t index;
    CatalogSource source;
} MergeKey;

static int compare_merge_key(const void *a, const void *b) {
    const MergeKey *left = a, *right = b;
    int order = strcmp(left->key, right->key);
    if (order) return order;
    if (left->source != right->source) return (int)left->source - (int)right->source;
    return (left->index > right->index) - (left->index < right->index);
}

size_t merge_catalogs(const CatalogIndex indexes[CATALOG_SOURCE_COUNT], CatalogEntry entries[MERGED_CAPACITY]) {
    size_t total = 0;
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) total += indexes[source].count;
    if (!total) return 0;
    MergeKey *keys = malloc(total * sizeof(*keys));
    size_t count = 0;
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
        for (size_t i = 0; i < indexes[source].count; ++i) {
            const Game *game = &indexes[source].games[i];
            const CatalogMetadata *metadata = find_catalog_metadata(game);
            if (!keys) {
                CatalogEntry *entry = &entries[count++];
                *entry = (CatalogEntry){.metadata = metadata};
                entry->games[source] = game;
                continue;
            }
            MergeKey *key = &keys[count++];
            key->index = i;
            key->source = (CatalogSource)source;
            key->metadata = metadata;
            if (metadata) {
                title_key(key->key, metadata->title);
            } else {
                char title[CATALOG_TEXT];
                strcpy(title, game->title);
                clean_catalog_title(title);
                title_key(key->key, title);
            }
        }
    }
    if (!keys) return count;
    qsort(keys, total, sizeof(*keys), compare_merge_key);
    count = 0;
    for (size_t i = 0; i < total; ++i) {
        const MergeKey *key = &keys[i];
        if (!i || !key->key[0] || strcmp(key->key, keys[i - 1].key))
            entries[count++] = (CatalogEntry){0};
        CatalogEntry *entry = &entries[count - 1];
        if (!entry->games[key->source])
            entry->games[key->source] = &indexes[key->source].games[key->index];
        if (key->metadata && (!entry->metadata ||
             (!entry->metadata->nintendo_life[0] && key->metadata->nintendo_life[0])))
            entry->metadata = key->metadata;
    }
    free(keys);
    return count;
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
    char *fields[] = {result.title, result.publisher, result.released,
                      result.description, result.original, result.fallback_hashes};
    const size_t capacities[] = {sizeof(result.title), sizeof(result.publisher),
                                 sizeof(result.released), sizeof(result.description),
                                 sizeof(result.original), sizeof(result.fallback_hashes)};
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
        char *end = memchr(inputs, 0, available);
        if (!end || (size_t)(end - inputs) >= capacities[i]) return false;
        size_t bytes = (size_t)(end - inputs) + 1;
        memcpy(fields[i], inputs, bytes);
        inputs += bytes;
        available -= bytes;
    }
    if (available) {
        const char *end = memchr(inputs, 0, available);
        if (end && (size_t)(end - inputs) < sizeof(result.developer)) {
            size_t n = (size_t)(end - inputs) + 1;
            memcpy(result.developer, inputs, n);
            inputs += n;
            available -= n;
        }
    }
    if (available) {
        const char *end = memchr(inputs, 0, available);
        if (end && end - inputs == 16 &&
            strspn(inputs, "0123456789abcdefABCDEF") == 16)
            memcpy(result.title_id, inputs, 17);
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
                 !memcmp(magic, "NXLTDB03", sizeof(magic)) && read_u32(file, &count) &&
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

static bool match_title_key(const char *actual, const char *expected) {
    if (!expected[0]) return false;
    if (!strcmp(actual, expected)) return true;
    // Nintendo Life appends platform badges inside some H1 elements.
    // Ignore only a recognised trailing badge, never a differing title.
    static const char *const badges[] = {"nintendoswitch", "switch2", "switch"};
    size_t length = strlen(actual), wanted = strlen(expected);
    for (size_t i = 0; i < sizeof(badges) / sizeof(*badges); ++i) {
        size_t suffix = strlen(badges[i]);
        if (length == wanted + suffix && !memcmp(actual, expected, wanted) &&
            !strcmp(actual + wanted, badges[i])) return true;
    }
    return false;
}

static bool match_heading(xmlNodePtr node, const char *key, const char *alternate) {
    for (; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && !xmlStrcmp(node->name, BAD_CAST "h1")) {
            xmlChar *content = xmlNodeGetContent(node);
            char actual[CATALOG_TEXT];
            title_key(actual, content ? (char *)content : "");
            xmlFree(content);
            if (actual[0] && (match_title_key(actual, key) || match_title_key(actual, alternate))) return true;
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

/* Nintendo Life pages contain stable screenshot URLs in their HTML. Scanning
 * the actual URL pattern is independent of changes to section/figure markup
 * and avoids libxml parser discrepancies between Eden and Switch. */
static void scan_nl_screenshots(const char *html, GameGallery *gallery) {
    const char *cursor = html;
    while (gallery->count < GALLERY_LIMIT &&
           (cursor = strstr(cursor, NL_IMAGE)) != NULL) {
        const char *digits = cursor + sizeof(NL_IMAGE) - 1;
        size_t n = strspn(digits, "0123456789");
        if (n > 0 && n <= 12 && !strncmp(digits + n, "/large.jpg", 10)) {
            size_t len = (size_t)(digits - cursor) + n + 10;
            if (len < CATALOG_URL) {
                char candidate[CATALOG_URL];
                memcpy(candidate, cursor, len);
                candidate[len] = 0;
                bool duplicate = false;
                for (size_t i = 0; i < gallery->count; ++i)
                    if (!strcmp(candidate, gallery->screenshots[i])) duplicate = true;
                if (!duplicate) strcpy(gallery->screenshots[gallery->count++], candidate);
            }
        }
        cursor = digits + (n ? n : 1);
    }
}

static bool fetch_gallery(GalleryJob *job, const char *url, const char *key, const char *alternate,
                          CatalogFetchStatus *status, bool trusted_screenshots_page) {
    size_t size;
    unsigned char *data = catalog_fetch_document(url, &job->cancel, &size, status);
    if (!data) return false;
    if (trusted_screenshots_page) {
        scan_nl_screenshots((const char *)data, &job->result);
        free(data);
        return job->result.count != 0;
    }
    htmlDocPtr page = htmlReadMemory((char *)data, (int)size, url, "UTF-8",
                                    HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (page) {
        xmlNodePtr root = xmlDocGetRootElement(page);
        if (match_heading(root, key, alternate)) {
            // Stable JPEG references can exist outside any screenshot section.
            scan_nl_screenshots((const char *)data, &job->result);
            if (!job->result.count) extract_gallery(root, &job->result, false);
        }
        xmlFreeDoc(page);
    }
    free(data);
    return job->result.count != 0;
}

static bool bundled_gallery(const char *key, GameGallery *gallery) {
    if (!key[0] || strspn(key, "abcdefghijklmnopqrstuvwxyz0123456789") != strlen(key))
        return false;
    char manifest[CATALOG_URL], line[24];
    snprintf(manifest, sizeof(manifest), BUILTIN_GALLERY_PATH "%s/images.txt", key);
    FILE *file = fopen(manifest, "r");
    if (!file) return false;
    while (gallery->count < GALLERY_LIMIT && fgets(line, sizeof(line), file)) {
        size_t digits = strspn(line, "0123456789");
        if (!digits || strcmp(line + digits, "\n")) break;
        snprintf(gallery->screenshots[gallery->count++], CATALOG_URL,
                 BUILTIN_GALLERY_PATH "%s/%.*s.jpg", key, (int)digits, line);
    }
    fclose(file);
    return gallery->count != 0;
}

unsigned char *load_gallery_image(const char *url, atomic_bool *cancel, size_t *bytes,
                                  CatalogFetchStatus *status) {
    *status = (CatalogFetchStatus){0};
    if (!begins(url, BUILTIN_GALLERY_PATH)) return catalog_fetch_cover_status(url, cancel, bytes, status);
    *bytes = 0;
    if (atomic_load(cancel)) return NULL;
    FILE *file = fopen(url, "rb");
    if (!file) return NULL;
    unsigned char *data = NULL;
    if (!fseek(file, 0, SEEK_END)) {
        long size = ftell(file);
        if (size > 0 && size <= BUILTIN_IMAGE_LIMIT && !fseek(file, 0, SEEK_SET)) {
            data = malloc((size_t)size);
            if (data && fread(data, 1, (size_t)size, file) == (size_t)size)
                *bytes = (size_t)size;
        }
    }
    fclose(file);
    if (*bytes) return data;
    free(data);
    return NULL;
}


#define GEMATSU_GALLERY "https://www.gematsu.com/gallery/"
#define GEMATSU_IMAGES "https://www.gematsu.com/wp-content/uploads/"

/* Gematsu hosts individual gallery pages with actual gameplay screenshots.
 * Only accept canonical pages for the exact game slug; other slugs can
 * silently redirect to unrelated games (e.g. Hollow Knight -> Silksong). */
static bool gematsu_canonical(const char *html, const char *url) {
    char canonical[CATALOG_URL + 48];
    int n = snprintf(canonical, sizeof(canonical),
                     "<link rel=\"canonical\" href=\"%s\"", url);
    return n > 0 && (size_t)n < sizeof(canonical) && strstr(html, canonical);
}

static bool gematsu_image(const char *start, size_t length, char url[CATALOG_URL]) {
    if (!length || length >= CATALOG_URL) return false;
    memcpy(url, start, length);
    url[length] = 0;
    if (!begins(url, GEMATSU_IMAGES)) return false;
    const char *ext = strrchr(url, '.');
    return ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") ||
                   !strcasecmp(ext, ".png") || !strcasecmp(ext, ".webp"));
}

/* WordPress serves 768/1024px images as separate static files. Prefer those
 * over multi-megabyte originals that exceed the screenshot download budget.
 * Only extract srcset from within the current gallery figure. */
static bool gematsu_resized_image(const char *start, const char *figure_end,
                                  const char *original, char chosen[CATALOG_URL]) {
    const char *attr = strstr(start, "data-srcset=\"");
    if (!attr || attr >= figure_end) return false;
    const char *p = attr + sizeof("data-srcset=\"") - 1;
    const char *end = strchr(p, '"');
    if (!end || end > figure_end) return false;
    bool png = false;
    const char *ext = strrchr(original, '.');
    if (ext) png = !strcasecmp(ext, ".png");
    unsigned maximum = png ? 768 : 1024, best = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',')) ++p;
        const char *u = p;
        while (p < end && *p != ' ' && *p != ',' && *p != '\t') ++p;
        size_t length = (size_t)(p - u);
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        char *after = NULL;
        unsigned long width = strtoul(p, &after, 10);
        if (after != p && after < end && *after == 'w') {
            char candidate[CATALOG_URL];
            if (width >= 480 && width <= maximum && width > best &&
                gematsu_image(u, length, candidate)) {
                strcpy(chosen, candidate);
                best = (unsigned)width;
            }
            p = after + 1;
        }
        while (p < end && *p != ',') ++p;
        if (p < end) ++p;
    }
    return best != 0;
}

static void gematsu_collect_images(const char *html, GameGallery *gallery) {
    /* The image links are inside this carousel, not the article/sidebar.
     * Do not use an unscoped search for <img> or the page shows other games. */
    const char *carousel = strstr(html, "<div class=\"carousel\"");
    if (!carousel) return;
    const char *end = strstr(carousel, "</section>");
    if (!end) end = carousel + strlen(carousel);
    const char *cursor = carousel;
    static const char marker[] = "class=\"btn bytes-image__link\" href=\"";
    while (gallery->count < GALLERY_LIMIT &&
           (cursor = strstr(cursor, marker)) && cursor < end) {
        const char *image_start = cursor + sizeof(marker) - 1;
        const char *image_end = strchr(image_start, '"');
        if (!image_end || image_end >= end) break;
        char original[CATALOG_URL], selected[CATALOG_URL];
        cursor = image_end + 1;
        if (!gematsu_image(image_start, (size_t)(image_end - image_start), original))
            continue;
        strcpy(selected, original);
        const char *figure_end = strstr(cursor, "</figure>");
        if (figure_end && figure_end < end) {
            char resized[CATALOG_URL];
            if (gematsu_resized_image(cursor, figure_end, original, resized))
                strcpy(selected, resized);
        }
        bool duplicate = false;
        for (size_t i = 0; i < gallery->count; ++i)
            if (!strcmp(gallery->screenshots[i], selected)) duplicate = true;
        if (!duplicate) strcpy(gallery->screenshots[gallery->count++], selected);
    }
    if (gallery->count) gallery->issue[0] = 0;
}

static bool gematsu_gallery(GalleryJob *job) {
    const char *names[] = {job->title, job->catalog_title, job->heading};
    char previous[3][CATALOG_TEXT] = {{0}};
    size_t seen = 0;
    for (size_t i = 0; i < 3 && !atomic_load(&job->cancel); ++i) {
        if (!names[i][0]) continue;
        char name[CATALOG_TEXT], slug[CATALOG_TEXT], url[CATALOG_URL];
        snprintf(name, sizeof(name), "%s", names[i]);
        clean_heading(name);
        remove_suffix(name, "nintendo switch");
        remove_suffix(name, "switch");
        make_slug(slug, name);
        for (char *p = slug; *p; ++p)
            if (*p == '_') *p = '-';
        if (!slug[0]) continue;
        bool already = false;
        for (size_t j = 0; j < seen; ++j)
            if (!strcmp(slug, previous[j])) already = true;
        if (already) continue;
        strcpy(previous[seen++], slug);
        int n = snprintf(url, sizeof(url), GEMATSU_GALLERY "%s", slug);
        if (n <= 0 || n >= (int)sizeof(url)) continue;
        size_t size = 0;
        CatalogFetchStatus status = {0};
        unsigned char *html = catalog_fetch_document(url, &job->cancel, &size, &status);
        if (!html) {
            // If Gematsu is unreachable, do not repeat several costly queries.
            if (status.curl) break;
            continue;
        }
        if (!gematsu_canonical((const char *)html, url)) {
            free(html);
            continue;
        }
        char prefix[CATALOG_URL + 10];
        n = snprintf(prefix, sizeof(prefix), "href=\"" GEMATSU_GALLERY "%s/", slug);
        if (n <= 0 || n >= (int)sizeof(prefix)) {
            free(html);
            continue;
        }
        const char *cursor = (const char *)html;
        unsigned pages = 0;
        char seen_date[3][CATALOG_URL] = {{0}};
        while (pages < 3 && (cursor = strstr(cursor, prefix)) && !atomic_load(&job->cancel)) {
            const char *date = cursor + strlen(prefix), *after = date;
            size_t length = strspn(date, "abcdefghijklmnopqrstuvwxyz0123456789-");
            cursor = date + (length ? length : 1);
            if (length < 8 || length >= 55 || date[length] != '"') continue;
            char page[CATALOG_URL];
            n = snprintf(page, sizeof(page), GEMATSU_GALLERY "%s/%.*s",
                         slug, (int)length, date);
            if (n <= 0 || n >= (int)sizeof(page)) continue;
            bool repeat = false;
            for (unsigned j = 0; j < pages; ++j)
                if (!strcmp(page, seen_date[j])) repeat = true;
            if (repeat) continue;
            strcpy(seen_date[pages++], page);
            size_t bytes = 0;
            CatalogFetchStatus detail_status = {0};
            unsigned char *detail = catalog_fetch_document(page, &job->cancel,
                                                            &bytes, &detail_status);
            if (detail) {
                if (gematsu_canonical((const char *)detail, page))
                    gematsu_collect_images((const char *)detail, &job->result);
                free(detail);
            }
            if (job->result.count) break;
        }
        free(html);
        if (job->result.count) return true;
    }
    return false;
}

/* TitleDB supplies canonical Switch Title IDs. Nlib serves independent,
 * version-specific JPEG screenshots without Nintendo's blocked DNS hosts.
 * Its JSON uses http:// image links, but HTTPS works; reconstruct URLs
 * explicitly rather than following insecure redirects or trusting the host. */
static bool nlib_gallery(GalleryJob *job) {
    const char *tid = job->title_id;
    if (strlen(tid) != 16 ||
        strspn(tid, "0123456789abcdefABCDEF") != 16) return false;

    char url[CATALOG_URL];
    snprintf(url, sizeof(url), NLIB_SCREEN_API "%s/screens", tid);
    CatalogFetchStatus status = {0};
    size_t len = 0;
    unsigned char *data = catalog_fetch_document(url, &job->cancel, &len, &status);
    if (!data || atomic_load(&job->cancel)) {
        free(data);
        return false;
    }

    Json root = {(const char *)data, (const char *)data + len};
    Json screen_list = field(root, "screenshots");
    Json count_field = field(root, "count");
    if (!screen_list.begin || *screen_list.begin != '[' || !count_field.begin) {
        free(data);
        return false;
    }
    size_t count_size = (size_t)(count_field.end - count_field.begin);
    if (!count_size || count_size > 3 ||
        strspn(count_field.begin, "0123456789") != count_size) {
        free(data);
        return false;
    }
    unsigned promised = (unsigned)atoi(count_field.begin);
    if (!promised || promised > GALLERY_LIMIT) promised = GALLERY_LIMIT;

    const char *cursor = whitespace(screen_list.begin + 1, screen_list.end);
    while (job->result.count < promised &&
           cursor < screen_list.end && *cursor == '"' &&
           !atomic_load(&job->cancel)) {
        const char *end = value_end(cursor, screen_list.end, 0);
        if (!end) break;
        char link[CATALOG_URL];
        size_t bytes = (size_t)(end - cursor - 2);
        if (bytes && bytes < sizeof(link)) {
            memcpy(link, cursor + 1, bytes);
            link[bytes] = 0;
            /* URLs must match the exact Title ID, no foreign hosts,
             * arbitrary paths, query parameters or redirect targets. */
            const char *path = strstr(link, "/nx/");
            if (path && (!strncmp(link, "http://api.nlib.cc/nx/", 22) ||
                         !strncmp(link, "https://api.nlib.cc/nx/", 23))) {
                char expected[80];
                snprintf(expected, sizeof(expected), "/nx/%s/screen/", tid);
                if (!strncmp(path, expected, strlen(expected))) {
                    const char *suffix = path + strlen(expected);
                    size_t digits = strspn(suffix, "0123456789");
                    if (digits && digits <= 3 && !suffix[digits]) {
                        unsigned index = (unsigned)atoi(suffix);
                        if (index >= 1 && index <= GALLERY_LIMIT) {
                            snprintf(job->result.screenshots[job->result.count],
                                     CATALOG_URL, NLIB_SCREEN_API "%s/screen/%u", tid, index);
                            ++job->result.count;
                        }
                    }
                }
            }
        }
        cursor = whitespace(end, screen_list.end);
        if (cursor < screen_list.end && *cursor == ',')
            cursor = whitespace(cursor + 1, screen_list.end);
        else
            break;
    }
    free(data);
    if (job->result.count) {
        job->result.issue[0] = 0;
        return true;
    }
    return false;
}

/* NSWpedia exposes a stable lightgallery with direct JPEG links. Match the
 * canonical game slug from its search page, not the first search result:
 * "Hollow Knight" must not resolve to "Hollow Knight: Silksong". */
#define NSWPEDIA "https://nswpedia.com"
#define NSWPEDIA_SEARCH NSWPEDIA "/?s="
#define NSWPEDIA_GAME NSWPEDIA "/nintendo-switch-roms/"
#define NSWPEDIA_IMAGE NSWPEDIA "/wp-content/uploads/"

static bool nswpedia_slug_match(const char *candidate, const char *wanted) {
    size_t length = strlen(wanted);
    if (!length || strncmp(candidate, wanted, length)) return false;
    if (!candidate[length]) return true;
    const char *suffix = candidate + length;
    static const char *const allowed[] = {
        "-nsp", "-xci", "-rom", "-switch", "-totk", "-v", "-game",
        "-free-download", "-download"
    };
    if (suffix[0] == '-' && suffix[1] >= '0' && suffix[1] <= '9')
        return true;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(*allowed); ++i)
        if (!strncmp(suffix, allowed[i], strlen(allowed[i]))) return true;
    return false;
}

static void nswpedia_images(const char *html, GameGallery *gallery) {
    const char *start = strstr(html, "<div id=\"lightgallery\"");
    if (!start) return;
    const char *stop = strstr(start, "</div>");
    if (!stop) return;
    static const char marker[] = "href=\"" NSWPEDIA_IMAGE;
    const char *cursor = start;
    while ((cursor = strstr(cursor, marker)) && cursor < stop &&
           gallery->count < GALLERY_LIMIT) {
        const char *image = cursor + sizeof("href=\"") - 1;
        const char *end = strchr(image, '"');
        if (!end || end >= stop) break;
        const char *tag_end = strchr(end, '>');
        if (!tag_end || tag_end >= stop) break;
        const char *type = strstr(end, "screen_shot");
        cursor = tag_end + 1;
        if (!type || type >= tag_end) continue;
        size_t length = (size_t)(end - image);
        if (!length || length >= CATALOG_URL) continue;
        char url[CATALOG_URL];
        memcpy(url, image, length);
        url[length] = 0;
        const char *dot = strrchr(url, '.');
        if (!dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") &&
                     strcasecmp(dot, ".png"))) continue;
        bool repeated = false;
        for (size_t i = 0; i < gallery->count; ++i)
            if (!strcmp(url, gallery->screenshots[i])) repeated = true;
        if (!repeated)
            strcpy(gallery->screenshots[gallery->count++], url);
    }
    if (gallery->count) gallery->issue[0] = 0;
}

static bool nswpedia_confirm_title(const char *html, const char *wanted) {
    const char *begin = strstr(html, "<title>");
    if (!begin) return false;
    begin += sizeof("<title>") - 1;
    const char *end = strstr(begin, "</title>");
    if (!end || end - begin >= CATALOG_TEXT) return false;
    char actual[CATALOG_TEXT], key[CATALOG_TEXT], expected[CATALOG_TEXT];
    memcpy(actual, begin, (size_t)(end - begin));
    actual[end - begin] = 0;
    title_key(key, actual);
    title_key(expected, wanted);
    size_t n = strlen(expected);
    if (!n || strncmp(key, expected, n)) return false;
    const char *suffix = key + n;
    return !*suffix || !strncmp(suffix, "nsp", 3) ||
           !strncmp(suffix, "xci", 3) || !strncmp(suffix, "rom", 3) ||
           !strncmp(suffix, "switch", 6) || !strncmp(suffix, "v", 1);
}

static bool nswpedia_gallery(GalleryJob *job) {
    const char *names[] = {job->title, job->catalog_title};
    char last[CATALOG_TEXT] = {0};
    for (size_t attempt = 0; attempt < 2 && !atomic_load(&job->cancel); ++attempt) {
        if (!names[attempt][0]) continue;
        char name[CATALOG_TEXT], slug[CATALOG_TEXT];
        snprintf(name, sizeof(name), "%s", names[attempt]);
        clean_heading(name);
        remove_suffix(name, "nintendo switch");
        remove_suffix(name, "switch");
        make_slug(slug, name);
        for (char *p = slug; *p; ++p)
            if (*p == '_') *p = '-';
        if (!slug[0] || !strcmp(last, slug)) continue;
        strcpy(last, slug);
        CURL *curl = curl_easy_init();
        if (!curl) break;
        char *encoded = curl_easy_escape(curl, name, 0);
        curl_easy_cleanup(curl);
        if (!encoded) break;
        char search_url[CATALOG_URL];
        int n = snprintf(search_url, sizeof(search_url), NSWPEDIA_SEARCH "%s", encoded);
        curl_free(encoded);
        if (n <= 0 || n >= (int)sizeof(search_url)) continue;
        size_t size = 0;
        CatalogFetchStatus status = {0};
        unsigned char *results = catalog_fetch_document(search_url, &job->cancel, &size, &status);
        if (!results) {
            if (status.curl) break;
            continue;
        }
        static const char prefix[] = "href=\"" NSWPEDIA_GAME;
        const char *cursor = (const char *)results;
        char candidate[CATALOG_URL] = {0};
        while ((cursor = strstr(cursor, prefix)) && !atomic_load(&job->cancel)) {
            const char *begin = cursor + sizeof("href=\"") - 1;
            const char *end = strchr(begin, '"');
            if (!end) break;
            cursor = end + 1;
            size_t length = (size_t)(end - begin);
            if (!length || length >= CATALOG_URL) continue;
            const char *lastslash = end;
            while (lastslash > begin && lastslash[-1] != '/') --lastslash;
            size_t suffix_size = (size_t)(end - lastslash);
            if (!suffix_size || suffix_size >= CATALOG_TEXT) continue;
            char article_slug[CATALOG_TEXT];
            memcpy(article_slug, lastslash, suffix_size);
            article_slug[suffix_size] = 0;
            if (!nswpedia_slug_match(article_slug, slug)) continue;
            memcpy(candidate, begin, length);
            candidate[length] = 0;
            break;
        }
        free(results);
        if (!candidate[0] || atomic_load(&job->cancel)) continue;
        size = 0;
        unsigned char *page = catalog_fetch_document(candidate, &job->cancel, &size, &status);
        if (page) {
            if (nswpedia_confirm_title((const char *)page, name))
                nswpedia_images((const char *)page, &job->result);
            free(page);
            if (job->result.count) return true;
        }
    }
    return false;
}

void load_game_gallery(GalleryJob *job) {
    if (job->romsns_page[0]) {
        bool loaded = catalog_read_romsns_media(job->romsns_page, &job->cancel, job->port_description,
                                                job->result.screenshots, &job->result.count);
        if (loaded && job->result.count) return;
    }
    char name[CATALOG_TEXT], key[CATALOG_TEXT], alternate[CATALOG_TEXT] = {0};
    snprintf(name, sizeof(name), "%s", job->title[0] ? job->title :
             job->heading[0] ? job->heading : job->catalog_title);
    if (!job->title[0]) clean_heading(name);
    title_key(key, name);
    if (bundled_gallery(key, &job->result)) return;
    if (nlib_gallery(job)) return;
    if (atomic_load(&job->cancel)) return;

    /* ROMsHQ screenshots are installed by finish_jobs() when available.
     * Never generate img-eshop.cdn.nintendo.net URLs: DNS.mitm can block them.
     * Nintendo Life images are hosted on a separate, accessible domain. */
    CatalogFetchStatus status = {0};
    if (job->nintendo_life[0] && !atomic_load(&job->cancel)) {
        char url[CATALOG_URL];
        snprintf(url, sizeof(url), NL "%.*s", (int)(sizeof(url) - sizeof(NL)), job->nintendo_life);
        // The dedicated page lists more artwork than the overview carousel.
        char gallery_url[CATALOG_URL];
        int n = snprintf(gallery_url, sizeof(gallery_url), "%s/screenshots", url);
        if (n > 0 && n < (int)sizeof(gallery_url) &&
            fetch_gallery(job, gallery_url, key, alternate, &status, true)) return;
        if (!atomic_load(&job->cancel) &&
            fetch_gallery(job, url, key, alternate, &status, false)) return;
    }
    if (!atomic_load(&job->cancel) && !status.curl) {
        char slugs[2][CATALOG_TEXT] = {{0}};
        make_slug(slugs[0], name);
        // Nintendo Life omits an initial "the_" for some major titles,
        // including The Legend of Zelda: Tears of the Kingdom.
        if (!strncmp(slugs[0], "the_", 4))
            snprintf(slugs[1], sizeof(slugs[1]), "%s", slugs[0] + 4);
        if (remove_suffix(name, "nintendo switch") || remove_suffix(name, "switch")) {
            title_key(alternate, name);
            make_slug(slugs[1], name);
        } else if (!slugs[1][0] && begins(job->page, SOURCE_PAGE)) {
            const char *slug = job->page + sizeof(SOURCE_PAGE) - 1;
            size_t size = strspn(slug, "abcdefghijklmnopqrstuvwxyz0123456789-");
            if (size >= sizeof("-switch") && size < sizeof(slugs[1]) &&
                !memcmp(slug + size - (sizeof("-switch") - 1), "-switch", sizeof("-switch") - 1))
                snprintf(slugs[1], sizeof(slugs[1]), "%.*s", (int)(size - (sizeof("-switch") - 1)), slug);
        }
        const char *platforms[] = {NL_SWITCH, NL_ESHOP};
        for (size_t i = 0; i < 2 && !atomic_load(&job->cancel) && !status.curl; ++i) {
            if (!slugs[i][0] || (i && !strcmp(slugs[0], slugs[1]))) continue;
            for (size_t j = 0; j < 2 && !atomic_load(&job->cancel); ++j) {
                char url[CATALOG_URL];
                snprintf(url, sizeof(url), NL "%s%s", platforms[j], slugs[i]);
                if (fetch_gallery(job, url, key, alternate, &status, false)) return;
                if (status.curl) break;
            }
        }
    }
    if (atomic_load(&job->cancel)) return;

    // A separately hosted gallery avoids Nintendo's blocked domains and
    // also covers Switch exclusives that are not available on Steam.
    if (gematsu_gallery(job)) return;
    if (atomic_load(&job->cancel)) return;
    if (nswpedia_gallery(job)) return;
    if (atomic_load(&job->cancel)) return;

    /* Steam is a last resort, using only media from an exact title match. */
    SteamMedia *media = calloc(1, sizeof(*media));
    if (!media) {
        strcpy(job->result.issue, "Не вистачає пам'яті для галереї");
        return;
    }
    char steam_title[CATALOG_TEXT];
    snprintf(steam_title, sizeof(steam_title), "%s", job->title[0] ? job->title : job->catalog_title);
    clean_heading(steam_title);
    remove_suffix(steam_title, "nintendo switch");
    remove_suffix(steam_title, "switch");
    steam_media_lookup(steam_title, &job->cancel, media);
    if (media->screenshot_count) {
        job->result.count = media->screenshot_count;
        memcpy(job->result.screenshots, media->screenshots,
               job->result.count * sizeof(job->result.screenshots[0]));
        job->result.issue[0] = 0;
    } else if (!atomic_load(&job->cancel)) {
        if (media->curl_error)
            snprintf(job->result.issue, sizeof(job->result.issue),
                     "Steam · cURL %d; Nintendo Life недоступна", media->curl_error);
        else if (media->http_error && media->http_error != 200)
            snprintf(job->result.issue, sizeof(job->result.issue),
                     "Steam · HTTP %ld; Nintendo Life недоступна", media->http_error);
        else
            strcpy(job->result.issue, "Скриншоти не знайдено в Nintendo Life та Steam");
    }
    free(media);
}


// Verified to return structured Nintendo Switch trailer search results.
// Public media relaying is best-effort and may reject individual streams.
#define SWITCH_VIDEO_API_1 "https://iv.catgirl.cloud"

/* A Switch-only trailer lookup. Nintendo Store can be blocked by DNS.mitm,
 * but official Switch launch/announcement trailers are also published
 * independently. Do not substitute the Steam/PC edition's trailer. */
static bool switch_video_id(const char *id) {
    if (strlen(id) != 11) return false;
    return strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == 11;
}

static bool switch_video_title(const char *title, const char *wanted) {
    char video[CATALOG_TEXT], game[CATALOG_TEXT];
    title_key(video, title);
    title_key(game, wanted);
    if (!game[0] || !strstr(video, game)) return false;
    /* Require an explicit Switch label so PC/PS/Xbox footage is not mixed in. */
    bool has_switch = strcasestr(title, "Nintendo Switch") ||
                      strcasestr(title, "Switch Launch") ||
                      strcasestr(title, "Switch Trailer") ||
                      strcasestr(title, "Switch Gameplay");
    bool has_trailer = strcasestr(title, "Trailer") ||
                       strcasestr(title, "Overview") ||
                       strcasestr(title, "Gameplay") ||
                       strcasestr(title, "Announcement") ||
                       strcasestr(title, "Preview");
    return has_switch && has_trailer;
}

static bool switch_videos_from_instance(TrailerJob *job, const char *instance,
                                        CatalogFetchStatus *last) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    char query[CATALOG_TEXT + 32];
    snprintf(query, sizeof(query), "%s Nintendo Switch trailer", job->title);
    char *encoded = curl_easy_escape(curl, query, 0);
    curl_easy_cleanup(curl);
    if (!encoded) return false;
    char url[CATALOG_URL];
    int n = snprintf(url, sizeof(url), "%s/api/v1/search?q=%s&type=video", instance, encoded);
    curl_free(encoded);
    if (n <= 0 || n >= (int)sizeof(url)) return false;

    size_t bytes = 0;
    unsigned char *body = catalog_fetch_document(url, &job->cancel, &bytes, last);
    if (!body || atomic_load(&job->cancel)) {
        free(body);
        return false;
    }
    Json list = {(char *)body, (char *)body + bytes};
    const char *cursor = NULL;
    unsigned examined = 0;
    while (job->result.count < TRAILER_LIMIT && examined++ < 25) {
        Json item = next_object(list, &cursor);
        if (!item.begin) break;
        if (!literal(field(item, "type"), "\"video\"")) continue;
        char title[CATALOG_TEXT], id[32];
        if (!string_field(item, "title", title, sizeof(title)) ||
            !string_field(item, "videoId", id, sizeof(id)) ||
            !switch_video_id(id) || !switch_video_title(title, job->title)) continue;
        bool duplicate = false;
        for (size_t i = 0; i < job->result.count; ++i)
            if (strstr(job->result.urls[i], id)) duplicate = true;
        if (duplicate) continue;
        char uri[CATALOG_URL];
        n = snprintf(uri, sizeof(uri),
                     "%s/latest_version?id=%s&itag=18&local=true", instance, id);
        if (n > 0 && n < (int)sizeof(uri))
            strcpy(job->result.urls[job->result.count++], uri);
    }
    free(body);
    return job->result.count != 0;
}

/* Do not advertise YouTube/Invidious search hits as playable trailers
 * until a 16-byte ranged MP4 request actually succeeds. Public gateways
 * often return HTTP 500 after successful /api/v1/search responses. */
typedef struct {
    unsigned char bytes[16];
    size_t count;
} TrailerProbe;

static size_t trailer_probe_write(char *data, size_t size, size_t count, void *arg) {
    TrailerProbe *probe = arg;
    if (count && size > SIZE_MAX / count) return 0;
    size_t n = size * count;
    if (n > sizeof(probe->bytes) - probe->count) return 0;
    memcpy(probe->bytes + probe->count, data, n);
    probe->count += n;
    return n;
}

static bool trailer_probe_url(const char *url, atomic_bool *cancel,
                              CatalogFetchStatus *status) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        *status = (CatalogFetchStatus){.curl = CURLE_FAILED_INIT};
        return false;
    }
    TrailerProbe probe = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-15");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 7L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, trailer_probe_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &probe);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NXLoad/1.0.50");
    if (atomic_load(cancel)) {
        curl_easy_cleanup(curl);
        return false;
    }
    status->curl = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status->http);
    bool works = !status->curl && status->http == 206 &&
                 probe.count == sizeof(probe.bytes) &&
                 !memcmp(probe.bytes + 4, "ftyp", 4);
    curl_easy_cleanup(curl);
    return works;
}

static bool trailer_probe_stream(const char *url, atomic_bool *cancel,
                                 CatalogFetchStatus *status) {
    if (trailer_probe_url(url, cancel, status)) return true;
    if (atomic_load(cancel)) return false;
    char alternate[CATALOG_URL];
    snprintf(alternate, sizeof(alternate), "%s", url);
    char *local = strstr(alternate, "&local=true");
    if (!local) return false;
    snprintf(local, sizeof(alternate) - (size_t)(local - alternate), "&local=false");
    return trailer_probe_url(alternate, cancel, status);
}

/* Gamereactor serves independently hosted MP4 trailers. Its public
 * WordPress REST catalog supplies the original filename of each video.
 * We validate game titles, file names and the HTTP Range response before
 * exposing anything to the player. No Nintendo or YouTube DNS is needed. */
#define GR_VIDEO_API "https://www.gamereactor.eu/wp-json/wp/v2/gr_video?search="
#define GR_MEDIA "https://media.gamereactor.eu/default/"

static int gamereactor_trailer_score(const char *title, const char *wanted) {
    char movie[CATALOG_TEXT], game[CATALOG_TEXT];
    title_key(movie, title);
    title_key(game, wanted);
    size_t size = strlen(game);
    if (!size || strncmp(movie, game, size)) return 0;
    if (strcasestr(title, "switch 2") || strcasestr(title, "playstation") ||
        strcasestr(title, "ps5") || strcasestr(title, "xbox") ||
        strcasestr(title, "GRTV News") || strcasestr(title, "review") ||
        strcasestr(title, "livestream") || strcasestr(title, "interview"))
        return 0;
    const char *after = movie + size;
    const char *promos[] = {
        "nintendoswitch", "switchlaunch", "switchtrailer",
        "official", "launch", "release", "announcement", "overview",
        "trailer", "teaser", "gameplay", "nintendodirect",
        "e3", "firstgameplay", "cinematic", "story", "coopmode",
        "captureyourimagination", "myway", "rediscoveryoursense",
        "jumpupsuperstar", "final", "reveal", "collectorsedition"
    };
    bool relevant = false;
    for (size_t i = 0; i < sizeof(promos)/sizeof(*promos); ++i)
        if (!strncmp(after, promos[i], strlen(promos[i]))) relevant = true;
    if (!relevant) return 0;
    if (strcasestr(title, "Nintendo Switch") && strcasestr(title, "Trailer")) return 4;
    if (strcasestr(title, "Official Trailer") ||
        strcasestr(title, "Launch Trailer") ||
        strcasestr(title, "Release Date Trailer")) return 3;
    if (strcasestr(title, "Trailer") || strcasestr(title, "Overview") ||
        strcasestr(title, "Nintendo Switch")) return 2;
    return 1;
}

static bool gamereactor_valid_asset(const char *asset) {
    size_t n = strlen(asset);
    return n > 5 && n < CATALOG_URL - sizeof(GR_MEDIA) &&
           !strcasecmp(asset + n - 4, ".mp4") &&
           strspn(asset,
                  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == n;
}

static bool gamereactor_trailers(TrailerJob *job, CatalogFetchStatus *status) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        status->curl = CURLE_FAILED_INIT;
        return false;
    }
    char *encoded = curl_easy_escape(curl, job->title, 0);
    curl_easy_cleanup(curl);
    if (!encoded) return false;
    char url[CATALOG_URL];
    int n = snprintf(url, sizeof(url), GR_VIDEO_API "%s&per_page=75&_fields=id,title,acf,link", encoded);
    curl_free(encoded);
    if (n <= 0 || n >= (int)sizeof(url)) return false;

    size_t length = 0;
    unsigned char *data = catalog_fetch_document(url, &job->cancel, &length, status);
    if (!data || atomic_load(&job->cancel)) {
        free(data);
        return false;
    }
    Json items = {(char *)data, (char *)data + length};
    if (*items.begin != '[') {
        free(data);
        return false;
    }

    /* Four passes prefer Nintendo Switch trailers over generic promotional
     * material while avoiding unrelated news, reviews and sequel releases. */
    for (int rank = 4; rank >= 1 && job->result.count < TRAILER_LIMIT &&
                        !atomic_load(&job->cancel); --rank) {
        const char *cursor = NULL;
        for (;;) {
            Json video = next_object(items, &cursor);
            if (!video.begin || job->result.count == TRAILER_LIMIT ||
                atomic_load(&job->cancel)) break;
            char title[CATALOG_TEXT], asset[CATALOG_URL];
            if (!string_field(field(video, "title"), "rendered", title, sizeof(title)) ||
                gamereactor_trailer_score(title, job->title) != rank ||
                !string_field(field(video, "acf"), "_gr_video_asset", asset, sizeof(asset)) ||
                !gamereactor_valid_asset(asset))
                continue;
            char media[CATALOG_URL];
            int m = snprintf(media, sizeof(media), GR_MEDIA "%s", asset);
            if (m <= 0 || m >= (int)sizeof(media)) continue;
            bool duplicate = false;
            for (size_t i = 0; i < job->result.count; ++i)
                if (!strcmp(job->result.urls[i], media)) duplicate = true;
            if (duplicate) continue;
            CatalogFetchStatus video_status = {0};
            if (!trailer_probe_url(media, &job->cancel, &video_status))
                continue;
            strcpy(job->result.urls[job->result.count++], media);
        }
    }
    free(data);
    if (job->result.count) {
        job->result.issue[0] = 0;
        return true;
    }
    return false;
}

void load_game_trailers(TrailerJob *job) {
    if (atomic_load(&job->cancel)) return;
    CatalogFetchStatus status = {0};
    if (gamereactor_trailers(job, &status)) return;
    if (atomic_load(&job->cancel)) return;
    if (switch_videos_from_instance(job, SWITCH_VIDEO_API_1, &status)) {
        size_t discovered = job->result.count;
        CatalogFetchStatus video_status = {0};
        bool streamable = false;
        for (size_t i = 0; i < discovered && i < 2 && !atomic_load(&job->cancel); ++i) {
            if (trailer_probe_stream(job->result.urls[i], &job->cancel, &video_status)) {
                streamable = true;
                break;
            }
        }
        if (streamable) return;
        job->result.count = 0;
        if (atomic_load(&job->cancel)) return;
        if (video_status.http >= 400)
            snprintf(job->result.issue, sizeof(job->result.issue),
                     "Трейлери · HTTP %ld", video_status.http);
        else if (video_status.curl)
            snprintf(job->result.issue, sizeof(job->result.issue),
                     "Трейлери · cURL %d", video_status.curl);
        else
            strcpy(job->result.issue, "Трейлери знайдено, потік недоступний");
        return;
    }
    if (atomic_load(&job->cancel)) return;
    if (status.curl)
        snprintf(job->result.issue, sizeof(job->result.issue),
                 "Switch-трейлери: відеошлюз cURL %d", status.curl);
    else if (status.http && status.http != 200)
        snprintf(job->result.issue, sizeof(job->result.issue),
                 "Switch-трейлери: відеошлюз HTTP %ld", status.http);
    else
        strcpy(job->result.issue, "Switch-трейлери не знайдено");
}
