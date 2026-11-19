#ifndef NXLOAD_GAME_INFO_H
#define NXLOAD_GAME_INFO_H

#include "catalog.h"

#define GALLERY_LIMIT CATALOG_SCREENSHOTS
#define GALLERY_HASHES (GALLERY_LIMIT * 64 + 1)

typedef struct {
    char title[CATALOG_TEXT];
    char publisher[CATALOG_TEXT];
    char developer[CATALOG_TEXT];
    char released[16];
    char description[6144];
    char original[6144];
    char fallback_hashes[GALLERY_HASHES];
} GameMetadata;

typedef struct {
    char screenshots[GALLERY_LIMIT][CATALOG_URL];
    size_t count;
    char issue[CATALOG_TEXT];
    bool from_titledb;
    bool from_romshq;
} GameGallery;

typedef struct {
    char page[CATALOG_URL];
    char nintendo_life[CATALOG_URL];
    char title[CATALOG_TEXT];
    char heading[CATALOG_TEXT];
    char catalog_title[CATALOG_TEXT];
    char fallback_hashes[GALLERY_HASHES];
    GameGallery result;
    atomic_bool cancel;
} GalleryJob;

bool load_catalog_metadata(void);
void free_catalog_metadata(void);
void clean_catalog_title(char name[CATALOG_TEXT]);
size_t merge_catalogs(const CatalogIndex indexes[CATALOG_SOURCE_COUNT], CatalogEntry entries[MERGED_CAPACITY]);
bool read_game_metadata(GameMetadata *metadata, const char *title, const char *heading);
void load_game_gallery(GalleryJob *job);
unsigned char *load_gallery_image(const char *url, atomic_bool *cancel, size_t *bytes);

#endif
