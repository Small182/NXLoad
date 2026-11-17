#ifndef NXLOAD_GAME_INFO_H
#define NXLOAD_GAME_INFO_H

#include "catalog.h"

#define GALLERY_LIMIT 32

typedef struct {
    char title[CATALOG_TEXT];
    char publisher[CATALOG_TEXT];
    char released[16];
    char description[1536];
} GameMetadata;

typedef struct {
    char screenshots[GALLERY_LIMIT][CATALOG_URL];
    size_t count;
    char issue[CATALOG_TEXT];
} GameGallery;

typedef struct {
    char page[CATALOG_URL];
    char title[CATALOG_TEXT];
    char heading[CATALOG_TEXT];
    char catalog_title[CATALOG_TEXT];
    GameGallery result;
    atomic_bool cancel;
} GalleryJob;

bool read_game_metadata(GameMetadata *metadata, const char *title, const char *heading);
void load_game_gallery(GalleryJob *job);

#endif
