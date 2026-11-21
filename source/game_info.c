#define _GNU_SOURCE
#include "language.h"
#include "game_info.h"
#include "json.h"
#include "trailers.h"

#include <curl/curl.h>
#include <ctype.h>
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
        if (end && (size_t)(end - inputs) < sizeof(result.developer))
            memcpy(result.developer, inputs, (size_t)(end - inputs) + 1);
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
                          CatalogFetchStatus *status, bool trusted_url) {
    size_t size;
    unsigned char *data = catalog_fetch_document(url, &job->cancel, &size, status);
    if (!data) return false;
    htmlDocPtr page = htmlReadMemory((char *)data, (int)size, url, "UTF-8",
                                    HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    free(data);
    if (!page) return false;
    xmlNodePtr root = xmlDocGetRootElement(page);
    if (trusted_url || match_heading(root, key, alternate)) extract_gallery(root, &job->result, false);
    xmlFreeDoc(page);
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

#define STORE_SCREENSHOTS_PAGE "https://www.nintendo.com/us/store/products/"
#define STORE_SCREENSHOTS_CDN "https://assets.nintendo.com/image/upload/c_limit,w_1280,h_720,f_jpg,q_80/"
#define STORE_SCREENSHOTS_PATH "store/software/switch/"

/* Nintendo Store image paths are embedded in its product HTML as
 * store/software/switch/<14-digit NSUID>/<64-character asset ID>. Unlike
 * Nintendo Life's section markup, this also works if the page layout changes. */
/* Resolve media from the product for the currently opened game. The Apollo
 * cache also embeds recommended games; scanning the whole HTML accidentally
 * shows unrelated screenshots and trailers. */
static Json nintendo_product(const char *html) {
    const char *script = strstr(html, "<script id=\"__NEXT_DATA__\"");
    const char *start = script ? strchr(script, '>') : NULL;
    const char *end = start ? strstr(++start, "</script>") : NULL;
    if (!end) return (Json){0};

    Json root = {start, end};
    Json props = field(field(root, "props"), "pageProps");
    Json current = field(field(props, "analytics"), "product");
    char sku[24];
    if (!string_field(current, "sku", sku, sizeof(sku)) ||
        !sku[0] || sku[strspn(sku, "0123456789")]) return (Json){0};
    char key[64];
    snprintf(key, sizeof(key), "Product:{\\\"sku\\\":\\\"%s\\\"}", sku);
    Json product = field(field(props, "initialApolloState"), key);
    return literal(field(field(product, "platform"), "code"), "\"NINTENDO_SWITCH\"") ?
           product : (Json){0};
}

static bool store_gallery_ids(const char *html, GameGallery *gallery) {
    Json product = nintendo_product(html);
    char nsuid[24];
    if (!string_field(product, "nsuid", nsuid, sizeof(nsuid)) ||
        strlen(nsuid) != 14 || nsuid[strspn(nsuid, "0123456789")]) return false;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), STORE_SCREENSHOTS_PATH "%s/", nsuid);
    size_t prefix_size = strlen(prefix);
    Json assets = field(product, "productGallery");
    const char *cursor = NULL;
    while (gallery->count < GALLERY_LIMIT) {
        Json asset = next_object(assets, &cursor);
        if (!asset.begin) break;
        char id[160];
        if (!literal(field(asset, "resourceType"), "\"image\"") ||
            !literal(field(asset, "type"), "\"upload\"") ||
            !string_field(asset, "publicId", id, sizeof(id))) continue;
        const char *path = id + (id[0] == '/');
        if (!begins(path, prefix) || strlen(path + prefix_size) != 64 ||
            strspn(path + prefix_size, "0123456789abcdefABCDEF") != 64) continue;
        char url[CATALOG_URL];
        int n = snprintf(url, sizeof(url), STORE_SCREENSHOTS_CDN "%s", path);
        if (n < 0 || (size_t)n >= sizeof(url)) continue;
        bool duplicate = false;
        for (size_t i = 0; i < gallery->count; ++i)
            if (!strcmp(gallery->screenshots[i], url)) duplicate = true;
        if (!duplicate) strcpy(gallery->screenshots[gallery->count++], url);
    }
    if (gallery->count) gallery->issue[0] = 0;
    return gallery->count != 0;
}

static bool store_gallery(GalleryJob *job, CatalogFetchStatus *last_status) {
    *last_status = (CatalogFetchStatus){0};
    const char *titles[] = {job->title, job->catalog_title};
    char previous[CATALOG_TEXT] = {0};
    for (size_t i = 0; i < 2 && !atomic_load(&job->cancel); ++i) {
        if (!titles[i][0]) continue;
        char name[CATALOG_TEXT], slug[CATALOG_TEXT], url[CATALOG_URL];
        snprintf(name, sizeof(name), "%s", titles[i]);
        clean_heading(name);
        remove_suffix(name, "nintendo switch");
        remove_suffix(name, "switch");
        make_slug(slug, name);
        // Nintendo Life uses underscores, but Nintendo Store uses hyphens.
        for (char *p = slug; *p; ++p)
            if (*p == '_') *p = '-';
        if (!slug[0] || !strcmp(slug, previous)) continue;
        strcpy(previous, slug);
        snprintf(url, sizeof(url), STORE_SCREENSHOTS_PAGE "%s-switch/", slug);
        CatalogFetchStatus status = {0};
        size_t size = 0;
        unsigned char *html = catalog_fetch_document(url, &job->cancel, &size, &status);
        *last_status = status;
        if (!html) {
            if (status.curl || (status.http && status.http != 404)) break;
            continue;
        }
        bool found = store_gallery_ids((const char *)html, &job->result);
        free(html);
        if (found) return true;
    }
    return false;
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

void load_game_gallery(GalleryJob *job) {
    if (job->romsns_page[0]) {
        bool loaded = catalog_read_romsns_media(job->romsns_page, &job->cancel, job->port_description,
                                                job->result.screenshots, &job->result.count);
        if (!loaded || !job->result.count)
            strcpy(job->result.issue, "Скриншоти ROMsNS недоступні");
        return;
    }
    char name[CATALOG_TEXT], key[CATALOG_TEXT], alternate[CATALOG_TEXT] = {0};
    snprintf(name, sizeof(name), "%s", job->title[0] ? job->title :
             job->heading[0] ? job->heading : job->catalog_title);
    if (!job->title[0]) clean_heading(name);
    title_key(key, name);
    if (bundled_gallery(key, &job->result)) return;

    /* TitleDB's old img-eshop.cdn.nintendo.net/i/<hash>.jpg links fail on
     * some Switch networks (cURL 7). They are metadata, not confirmed
     * images. Never let them displace galleries from live providers. */
    CatalogFetchStatus nl_status = {0};
    if (job->nintendo_life[0] && !atomic_load(&job->cancel)) {
        char url[CATALOG_URL];
        snprintf(url, sizeof(url), NL "%.*s", (int)(sizeof(url) - sizeof(NL)), job->nintendo_life);
        if (fetch_gallery(job, url, key, alternate, &nl_status, true)) return;
    } else {
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
        const char *platforms[] = {NL_SWITCH, NL_ESHOP};
        for (size_t i = 0; i < 2 && !atomic_load(&job->cancel); ++i) {
            if (!slugs[i][0] || (i && !strcmp(slugs[0], slugs[1]))) continue;
            for (size_t j = 0; j < 2 && !atomic_load(&job->cancel); ++j) {
                char url[CATALOG_URL];
                snprintf(url, sizeof(url), NL "%s%s", platforms[j], slugs[i]);
                if (fetch_gallery(job, url, key, alternate, &nl_status, false)) return;
                /* Connection errors mean this host is unreachable right now;
                 * do not retry several more URL variations on the same host. */
                if (nl_status.curl) break;
            }
            if (nl_status.curl) break;
        }
    }
    if (atomic_load(&job->cancel)) return;

    /* Prefer Nintendo's current Cloudinary product gallery to the obsolete
     * eShop image CDN. Use store_gallery's own title checks and NSUID. */
    CatalogFetchStatus store_status = {0};
    if (store_gallery(job, &store_status)) return;
    if (atomic_load(&job->cancel)) return;

    if (store_status.curl)
        snprintf(job->result.issue, sizeof(job->result.issue),
                 "Nintendo Store · cURL %d", store_status.curl);
    else if (store_status.http && store_status.http != 200 && store_status.http != 404)
        snprintf(job->result.issue, sizeof(job->result.issue),
                 "Nintendo Store · HTTP %ld", store_status.http);
    else if (nl_status.curl)
        snprintf(job->result.issue, sizeof(job->result.issue),
                 "Nintendo Life · cURL %d; Nintendo Store: немає галереї", nl_status.curl);
    else if (nl_status.http && nl_status.http != 200 && nl_status.http != 404)
        snprintf(job->result.issue, sizeof(job->result.issue),
                 "Nintendo Life · HTTP %ld; Nintendo Store: немає галереї", nl_status.http);
    else
        strcpy(job->result.issue, "Скриншоти не знайдено в Nintendo Life та Nintendo Store");
}

#define NINTENDO_PRODUCTS "https://www.nintendo.com/us/store/products/"
#define NINTENDO_VIDEO "https://assets.nintendo.com/video/upload/"
#define SWITCH_VIDEO_PATH "store/software/switch/"
#define TRAILER_MP4 "c_limit,w_854,h_480,vc_h264/"

static bool nintendo_trailers(const char *html, GameTrailers *result) {
    Json product = nintendo_product(html);
    if (!product.begin) return false;

    char nsuid[24];
    if (!string_field(product, "nsuid", nsuid, sizeof(nsuid)) ||
        strlen(nsuid) != 14 || nsuid[strspn(nsuid, "0123456789")]) return false;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), SWITCH_VIDEO_PATH "%s/Video/", nsuid);
    size_t prefix_size = strlen(prefix);
    Json gallery = field(product, "productGallery");
    const char *cursor = NULL;
    while (result->count < TRAILER_LIMIT) {
        Json asset = next_object(gallery, &cursor);
        if (!asset.begin) break;
        char id[160];
        if (!literal(field(asset, "resourceType"), "\"video\"") ||
            !literal(field(asset, "type"), "\"upload\"") ||
            !string_field(asset, "publicId", id, sizeof(id))) continue;
        const char *path = id + (id[0] == '/');
        if (!begins(path, prefix) || strlen(path + prefix_size) != 64 ||
            strspn(path + prefix_size, "0123456789abcdef") != 64) continue;
        char url[CATALOG_URL];
        snprintf(url, sizeof(url), NINTENDO_VIDEO "%s%s.mp4", TRAILER_MP4, path);
        bool duplicate = false;
        for (size_t i = 0; i < result->count; ++i)
            if (!strcmp(result->urls[i], url)) duplicate = true;
        if (!duplicate) strcpy(result->urls[result->count++], url);
    }
    return true;
}


void load_game_trailers(TrailerJob *job) {
    char name[CATALOG_TEXT], slugs[2][CATALOG_TEXT] = {{0}};
    snprintf(name, sizeof(name), "%s", job->title);
    clean_heading(name);
    remove_suffix(name, "nintendo switch");
    remove_suffix(name, "switch");
    make_slug(slugs[0], name);
    for (char *p = slugs[0]; *p; ++p) if (*p == '_') *p = '-';

    const char *path = strrchr(job->page, '/');
    if (path && !path[1]) {
        --path;
        while (path > job->page && *path != '/') --path;
    }
    if (path) {
        ++path;
        size_t n = strspn(path, "abcdefghijklmnopqrstuvwxyz0123456789-");
        if (n > 7 && !memcmp(path + n - 7, "-switch", 7)) n -= 7;
        if (n < sizeof(slugs[1]))
            snprintf(slugs[1], sizeof(slugs[1]), "%.*s", (int)n, path);
    }

    long last_http = 0;
    int last_curl = 0;
    for (size_t i = 0; i < 2 && !atomic_load(&job->cancel); ++i) {
        if (!slugs[i][0] || (i && !strcmp(slugs[0], slugs[1]))) continue;
        char url[CATALOG_URL];
        snprintf(url, sizeof(url), NINTENDO_PRODUCTS "%s-switch/", slugs[i]);
        CatalogFetchStatus status = {0};
        size_t size;
        unsigned char *body = catalog_fetch_document(url, &job->cancel, &size, &status);
        last_http = status.http;
        last_curl = status.curl;
        if (!body) continue;
        nintendo_trailers((const char *)body, &job->result);
        free(body);
        if (job->result.count) break;
    }
    if (!job->result.count && !atomic_load(&job->cancel)) {
        if (last_curl) snprintf(job->result.issue, sizeof(job->result.issue),
                                "Nintendo Store · cURL %d", last_curl);
        else if (last_http && last_http != 200)
            snprintf(job->result.issue, sizeof(job->result.issue),
                     "Nintendo Store · HTTP %ld", last_http);
        else strcpy(job->result.issue, "Трейлерів у Nintendo Store не знайдено");
    }
}
