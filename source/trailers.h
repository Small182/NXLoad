#ifndef NXLOAD_TRAILERS_H
#define NXLOAD_TRAILERS_H

#include <SDL2/SDL.h>

#include "catalog.h"

#define TRAILER_LIMIT 8

typedef struct {
    char urls[TRAILER_LIMIT][CATALOG_URL];
    size_t count;
    char issue[128];
} GameTrailers;

typedef struct {
    char page[CATALOG_URL];
    char title[CATALOG_TEXT];
    atomic_bool cancel;
    GameTrailers result;
} TrailerJob;

typedef enum {
    TRAILER_LOADING,
    TRAILER_PLAYING,
    TRAILER_PAUSED,
    TRAILER_ENDED,
    TRAILER_ERROR
} TrailerState;

void load_game_trailers(TrailerJob *job);
bool trailer_play(SDL_Renderer *renderer, const char *url);
void trailer_stop(void);
void trailer_pause(void);
void trailer_seek(bool forward);
TrailerState trailer_draw(SDL_Renderer *renderer);
void trailer_shutdown(void);

#endif