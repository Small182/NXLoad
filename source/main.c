#define _GNU_SOURCE
#define SDL_MAIN_HANDLED
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <dirent.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog.h"
#include "game_info.h"
#include "unpack.h"

#define ROWS 9
#define FILE_ROWS 5
#define HISTORY_LIMIT 96
#define COVER_SLOTS 24

typedef enum { LIBRARY, GAME, DOWNLOADS } Screen;
typedef enum { GAME_OVERVIEW, GAME_SCREENSHOTS, GAME_FILES } GameTab;

typedef struct {
    char file[256];
} SavedFile;

typedef struct {
    char url[CATALOG_URL];
    SDL_Texture *texture;
    uint64_t last_used;
} CachedCover;

typedef struct {
    char url[CATALOG_URL];
    atomic_bool cancel;
    unsigned char *bytes;
    size_t count;
    bool screenshot;
} CoverJob;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *cover;
    SDL_Texture *description;
    TTF_Font *small;
    TTF_Font *normal;
    TTF_Font *large;
    Screen screen, previous;
    size_t rows[MERGED_CAPACITY];
    size_t game_count, game_index, file_index, saved_count, saved_index;
    CatalogIndex catalogs[2];
    CatalogEntry entries[MERGED_CAPACITY];
    size_t entry_count;
    CatalogIndexJob index_job;
    Thread index_thread;
    bool index_busy, show_partial_index;
    unsigned refresh_waiting;
    CatalogSource file_source;
    bool file_source_chosen, romshq_candidate;
    char index_notice[2][CATALOG_TEXT];
    Game active_game, game_sources[2];
    GameTab game_tab;
    size_t screenshot_index;
    int description_scroll;
    bool show_original;
    GameMetadata metadata;
    GameGallery gallery;
    GalleryJob gallery_job;
    Thread gallery_thread;
    bool gallery_busy, gallery_requested;
    bool confirm_install, close, catalog_busy, download_busy, download_finished;
    char query[96], notice[192], game_error[192], file_id[DLARCHIVE_ID_CAPACITY];
    char transfer_name[CATALOG_TEXT], transfer_url[2048];
    SavedFile saved[HISTORY_LIMIT];
    GameDetails details;
    CatalogJob pending;
    FichierJob download;
    Thread catalog_thread, download_thread, cover_thread;
    bool cover_busy;
    CoverJob cover_job;
    CachedCover covers[COVER_SLOTS];
    uint64_t previous_bytes;
    double speed;
    uint64_t speed_time;
} App;

static App app;
static const u64 up_buttons = HidNpadButton_Up | HidNpadButton_StickLUp;
static const u64 down_buttons = HidNpadButton_Down | HidNpadButton_StickLDown;
static const SDL_Color background = {15, 19, 31, 255};
static const SDL_Color panel = {24, 31, 48, 255};
static const SDL_Color selected = {35, 63, 104, 255};
static const SDL_Color white = {241, 244, 250, 255};
static const SDL_Color gray = {166, 181, 210, 255};
static const SDL_Color blue = {125, 182, 255, 255};

static void rect(int x, int y, int w, int h, SDL_Color color) {
    if (w <= 0 || h <= 0) return;
    SDL_Rect box = { x, y, w, h };
    SDL_SetRenderDrawColor(app.renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(app.renderer, &box);
}

static void text(TTF_Font *font, const char *str, int x, int y, int width, SDL_Color color) {
    if (!str || !*str || width <= 0) return;
    char short_text[512];
    snprintf(short_text, sizeof(short_text), "%.*s", (int)sizeof(short_text) - 4, str);
    size_t length = strlen(short_text);
    int pixels = 0;
    if (TTF_SizeUTF8(font, short_text, &pixels, NULL) == 0 && pixels > width) {
        while (length > 0) {
            do { --length; } while (length && (short_text[length] & 0xc0) == 0x80);
            strcpy(short_text + length, "...");
            if (TTF_SizeUTF8(font, short_text, &pixels, NULL) == 0 && pixels <= width) break;
        }
    }
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, short_text, color);
    if (!surface) return;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(app.renderer, surface);
    if (texture) {
        SDL_Rect box = {x, y, surface->w, surface->h};
        SDL_RenderCopy(app.renderer, texture, NULL, &box);
        SDL_DestroyTexture(texture);
    }
    SDL_FreeSurface(surface);
}

static void row(size_t i, size_t cursor, int x, int y, int width, const char *label) {
    if (i == cursor) {
        rect(x, y, width, 42, selected);
        rect(x, y, 3, 42, blue);
    }
    text(app.normal, label, x + 11, y + 8, width - 22, i == cursor ? white : gray);
}

static size_t first_visible(size_t selected_index, size_t rows) {
    return selected_index < rows ? 0 : selected_index - rows + 1;
}

static size_t move(size_t current, size_t count, u64 buttons) {
    if (current && (buttons & up_buttons)) --current;
    if (current + 1 < count && (buttons & down_buttons)) ++current;
    return current;
}

static void catalog_worker(void *arg) { catalog_request(arg); }
static void gallery_worker(void *arg) { load_game_gallery(arg); }
static void index_worker(void *arg) { catalog_build_index(arg); }
static void download_worker(void *arg) {
    FichierJob *job = arg;
    if (job->local_unpack) {
        job->result = unpack_rar(job, job->url, DOWNLOAD_DIR);
        return;
    }
    fichier_download(job);
    if (!atomic_load(&job->cancelled) && !strcmp(job->result, "Download saved") &&
        (rar_name(job->path) || rar_signature(job->path)))
        job->result = unpack_rar(job, job->path, DOWNLOAD_DIR);
}

static bool start_worker(Thread *thread, ThreadFunc function, void *arg) {
    Result rc = threadCreate(thread, function, arg, NULL, 128 * 1024, 0x2c, -2);
    if (R_FAILED(rc)) return false;
    rc = threadStart(thread);
    if (R_FAILED(rc)) threadClose(thread);
    return R_SUCCEEDED(rc);
}

static const char *source_name(CatalogSource source) {
    return source == CATALOG_ROMSHQ ? "ROMsHQ" : "NSRoms";
}

static const Game *game_at(size_t row) {
    const CatalogEntry *entry = &app.entries[app.rows[row]];
    return entry->games[CATALOG_NSROMS] ? entry->games[CATALOG_NSROMS] : entry->games[CATALOG_ROMSHQ];
}

static bool match_query(const Game *game) {
    const char *query = app.query;
    while (*query) {
        while (isspace((unsigned char)*query)) ++query;
        if (!*query) break;
        const char *start = query;
        while (*query && !isspace((unsigned char)*query)) ++query;
        char token[sizeof(app.query)];
        size_t n = (size_t)(query - start);
        memcpy(token, start, n);
        token[n] = 0;
        if (!strcasestr(game->title, token) && !strcasestr(game->page, token)) return false;
    }
    return true;
}

static void rebuild_view(const char *keep) {
    size_t old_index = app.game_index;
    app.game_count = 0;
    app.game_index = 0;
    bool found = false;
    for (size_t i = 0; i < app.entry_count; ++i) {
        const Game *nsroms = app.entries[i].games[CATALOG_NSROMS];
        const Game *romshq = app.entries[i].games[CATALOG_ROMSHQ];
        if ((!nsroms || !match_query(nsroms)) && (!romshq || !match_query(romshq))) continue;
        if (keep && ((nsroms && !strcmp(nsroms->page, keep)) || (romshq && !strcmp(romshq->page, keep)))) {
            app.game_index = app.game_count;
            found = true;
        }
        app.rows[app.game_count++] = i;
    }
    if (keep && !found && app.game_count)
        app.game_index = SDL_min(old_index, app.game_count - 1);
}

static void start_next_index(void) {
    if (app.index_busy || !app.refresh_waiting) return;
    CatalogSource source = (app.refresh_waiting & (1u << CATALOG_NSROMS)) ? CATALOG_NSROMS : CATALOG_ROMSHQ;
    app.refresh_waiting &= ~(1u << source);
    CatalogIndexJob *job = &app.index_job;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->cancel, false);
    atomic_init(&job->paused, app.download_busy);
    atomic_init(&job->published, 0);
    atomic_init(&job->page, 0);
    atomic_init(&job->pages, 0);
    job->source = source;
    app.show_partial_index = app.catalogs[source].count == 0;
    app.index_notice[source][0] = 0;
    app.index_busy = start_worker(&app.index_thread, index_worker, job);
    if (!app.index_busy) strcpy(app.index_notice[source], "Не вдалося запустити оновлення");
}

static void refresh_catalogs(void) {
    app.refresh_waiting |= (1u << CATALOG_NSROMS) | (1u << CATALOG_ROMSHQ);
    if (app.index_busy) app.refresh_waiting &= ~(1u << app.index_job.source);
    start_next_index();
}

static void start_game(void) {
    if (app.catalog_busy) return;
    CatalogJob *job = &app.pending;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->cancel, false);
    job->game = app.game_sources[app.file_source];
    job->source = app.file_source;
    app.notice[0] = 0;
    app.catalog_busy = start_worker(&app.catalog_thread, catalog_worker, job);
    if (!app.catalog_busy) strcpy(app.game_error, "Не вдалося запустити запит");
}

static SDL_Texture *decode_cover(const unsigned char *bytes, size_t count, bool screenshot) {
    if (!bytes || !count) return NULL;
    SDL_RWops *src = SDL_RWFromConstMem(bytes, (int)count);
    SDL_Surface *image = src ? IMG_Load_RW(src, 1) : NULL;
    if (!image) return NULL;

    SDL_Surface *scaled = NULL;
    double factor = SDL_min((screenshot ? 800.0 : 480.0) / image->w, 420.0 / image->h);
    if (factor < 1.0) {
        int w = SDL_max(1, (int)(factor * image->w));
        int h = SDL_max(1, (int)(factor * image->h));
        scaled = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (scaled && SDL_BlitScaled(image, NULL, scaled, NULL) < 0) {
            SDL_FreeSurface(scaled);
            scaled = NULL;
        }
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(app.renderer, scaled ? scaled : image);
    if (scaled) SDL_FreeSurface(scaled);
    SDL_FreeSurface(image);
    return texture;
}

static void cover_worker(void *arg) {
    CoverJob *job = arg;
    job->bytes = job->screenshot ? load_gallery_image(job->url, &job->cancel, &job->count) :
                 catalog_fetch_cover(job->url, &job->cancel, &job->count);
}

static CachedCover *find_cover(const char *url) {
    if (!url[0]) return NULL;
    for (size_t i = 0; i < COVER_SLOTS; ++i) {
        if (!strcmp(app.covers[i].url, url)) {
            app.covers[i].last_used = SDL_GetTicks64();
            return &app.covers[i];
        }
    }
    return NULL;
}

static void pump_covers(void) {
    CoverJob *job = &app.cover_job;
    if (app.cover_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.cover_thread), 0))) {
        threadWaitForExit(&app.cover_thread);
        threadClose(&app.cover_thread);
        app.cover_busy = false;
        CachedCover *entry = &app.covers[0];
        for (size_t i = 0; i < COVER_SLOTS; ++i) {
            CachedCover *candidate = &app.covers[i];
            if (!candidate->url[0]) {
                entry = candidate;
                break;
            }
            if (candidate->last_used < entry->last_used) entry = candidate;
        }
        SDL_DestroyTexture(entry->texture);
        strcpy(entry->url, job->url);
        entry->texture = decode_cover(job->bytes, job->count, job->screenshot);
        entry->last_used = SDL_GetTicks64();
        free(job->bytes);
        job->bytes = NULL;
    }
    if (app.cover_busy || app.download_busy || app.gallery_busy) return;

    const char *url = NULL;
    bool screenshot = false;
    if (app.screen == GAME) {
        if (app.game_tab == GAME_SCREENSHOTS && app.gallery.count) {
            const char *image = app.gallery.screenshots[app.screenshot_index];
            if (!find_cover(image)) {
                screenshot = true;
                url = image;
            }
        }
    } else if (app.screen == LIBRARY && app.game_count) {
        const char *selected_cover = game_at(app.game_index)->cover;
        if (selected_cover[0] && !find_cover(selected_cover)) {
            url = selected_cover;
        } else {
            size_t first = first_visible(app.game_index, ROWS);
            for (size_t i = first; i < app.game_count && i < first + ROWS; ++i) {
                const char *candidate = game_at(i)->cover;
                if (candidate[0] && !find_cover(candidate)) {
                    url = candidate;
                    break;
                }
            }
        }
    }
    if (url) {
        strcpy(job->url, url);
        atomic_init(&job->cancel, false);
        job->count = 0;
        job->screenshot = screenshot;
        app.cover_busy = start_worker(&app.cover_thread, cover_worker, job);
    }
}

static void update_cover(void) {
    SDL_DestroyTexture(app.cover);
    app.cover = decode_cover(app.details.cover, app.details.cover_bytes, false);
    free(app.details.cover);
    app.details.cover = NULL;
    app.details.cover_bytes = 0;
}

static void update_description(void) {
    SDL_DestroyTexture(app.description);
    app.description = NULL;
    app.description_scroll = 0;
    const char *summary = app.show_original ? app.metadata.original : app.metadata.description;
    if (!*summary) return;
    SDL_Surface *paragraph = TTF_RenderUTF8_Blended_Wrapped(app.normal, summary, gray, 750);
    if (!paragraph) return;
    app.description = SDL_CreateTextureFromSurface(app.renderer, paragraph);
    SDL_FreeSurface(paragraph);
}

static void pump_gallery(void) {
    GalleryJob *job = &app.gallery_job;
    if (app.gallery_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.gallery_thread), 0))) {
        threadWaitForExit(&app.gallery_thread);
        threadClose(&app.gallery_thread);
        app.gallery_busy = false;
        if (!atomic_load(&job->cancel) && !strcmp(job->page, app.active_game.page))
            app.gallery = job->result;
    }
    if (app.screen == GAME && app.game_tab == GAME_SCREENSHOTS &&
        !app.gallery_busy && !app.gallery_requested &&
        !app.download_busy && !app.catalog_busy && !app.cover_busy) {
        memset(job, 0, sizeof(*job));
        atomic_init(&job->cancel, false);
        strcpy(job->page, app.active_game.page);
        strcpy(job->title, app.metadata.title);
        strcpy(job->heading, app.details.title);
        strcpy(job->catalog_title, app.active_game.title);
        strcpy(job->fallback_hashes, app.metadata.fallback_hashes);
        app.gallery_requested = true;
        app.gallery_busy = start_worker(&app.gallery_thread, gallery_worker, job);
        if (!app.gallery_busy)
            strcpy(app.gallery.issue, "Не вдалося запустити запит Nintendo Life");
    }
}

static int file_compare(const void *a, const void *b) {
    return strcasecmp(((const SavedFile *)a)->file, ((const SavedFile *)b)->file);
}

static bool saved_rar(const char *name) {
    if (rar_name(name)) return true;
    char path[320];
    int bytes = snprintf(path, sizeof(path), DOWNLOAD_DIR "/%s", name);
    return bytes > 0 && (size_t)bytes < sizeof(path) && rar_signature(path);
}

static void refresh_saved(void) {
    app.saved_count = 0;
    DIR *dir = opendir(DOWNLOAD_DIR);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) && app.saved_count < HISTORY_LIMIT) {
            size_t n = strlen(entry->d_name);
            if (n < sizeof(app.saved[0].file) && (package_name(entry->d_name) || rar_name(entry->d_name) ||
                (strstr(entry->d_name, ".bin") && saved_rar(entry->d_name)))) {
                strcpy(app.saved[app.saved_count++].file, entry->d_name);
            }
        }
        closedir(dir);
    }
    qsort(app.saved, app.saved_count, sizeof(app.saved[0]), file_compare);
    if (app.saved_index >= app.saved_count) app.saved_index = 0;
}

static void remake_catalog(void) {
    char keep[CATALOG_URL] = {0};
    if (app.game_count) strcpy(keep, game_at(app.game_index)->page);
    app.entry_count = merge_catalogs(app.catalogs, app.entries);
    rebuild_view(keep[0] ? keep : NULL);
}

static void open_catalogs(void) {
    time_t now = time(NULL);
    for (unsigned source = 0; source < 2; ++source) {
        CatalogIndex *index = &app.catalogs[source];
        bool cached = catalog_read_cache(index, (CatalogSource)source);
        if (!cached || now < index->saved_at || now - index->saved_at >= CATALOG_CACHE_TTL)
            app.refresh_waiting |= 1u << source;
    }
    remake_catalog();
    start_next_index();
}

static void pump_index(void) {
    if (!app.index_busy) {
        start_next_index();
        return;
    }
    CatalogIndexJob *job = &app.index_job;
    CatalogIndex *index = &app.catalogs[job->source];
    bool game_request = app.screen == GAME &&
        (app.catalog_busy || app.gallery_busy ||
         (app.game_tab == GAME_SCREENSHOTS && !app.gallery_requested));
    atomic_store(&job->paused, app.download_busy || game_request);
    size_t published = atomic_load(&job->published);
    if (app.show_partial_index && published > index->count) {
        size_t from = index->count;
        memcpy(index->games + from, job->index.games + from, (published - from) * sizeof(Game));
        index->count = published;
        remake_catalog();
    }
    if (R_FAILED(waitSingle(waiterForThread(&app.index_thread), 0))) return;
    threadWaitForExit(&app.index_thread);
    threadClose(&app.index_thread);
    app.index_busy = false;
    if (job->complete) {
        char keep[CATALOG_URL] = {0};
        if (app.game_count) strcpy(keep, game_at(app.game_index)->page);
        memcpy(index->games, job->index.games, job->index.count * sizeof(Game));
        index->count = job->index.count;
        index->saved_at = job->index.saved_at;
        app.entry_count = merge_catalogs(app.catalogs, app.entries);
        rebuild_view(keep[0] ? keep : NULL);
    }
    strcpy(app.index_notice[job->source], job->error);
}

static void finish_jobs(void) {
    if (app.catalog_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.catalog_thread), 0))) {
        threadWaitForExit(&app.catalog_thread);
        threadClose(&app.catalog_thread);
        app.catalog_busy = false;
        CatalogJob *job = &app.pending;
        if (!atomic_load(&job->cancel)) {
            app.details = job->details;
            if (app.details.screenshot_count && job->source == CATALOG_ROMSHQ) {
                if (app.gallery_busy) atomic_store(&app.gallery_job.cancel, true);
                memset(&app.gallery, 0, sizeof(app.gallery));
                app.screenshot_index = 0;
                app.gallery.count = app.details.screenshot_count;
                memcpy(app.gallery.screenshots, app.details.screenshots,
                       app.gallery.count * sizeof(app.gallery.screenshots[0]));
                app.gallery.from_romshq = true;
                app.gallery_requested = true;
            }
            strcpy(app.game_error, job->error);
            update_cover();
            if (!app.metadata.title[0] &&
                read_game_metadata(&app.metadata, app.active_game.title, app.details.title))
                update_description();
        } else {
            free(job->details.cover);
        }
        job->details.cover = NULL;
    }
    if (app.download_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.download_thread), 0))) {
        threadWaitForExit(&app.download_thread);
        threadClose(&app.download_thread);
        app.download_busy = false;
        app.download_finished = true;
        refresh_saved();
    }
    if (app.download_busy && SDL_GetTicks64() - app.speed_time >= 500) {
        uint64_t now = SDL_GetTicks64(), bytes = atomic_load(&app.download.received);
        app.speed = bytes >= app.previous_bytes ?
            (bytes - app.previous_bytes) * 1000.0 / (now - app.speed_time) : 0;
        app.previous_bytes = bytes;
        app.speed_time = now;
    }
}

static FichierJob *new_transfer(const char *name) {
    if (app.download_busy) {
        strcpy(app.notice, "Поточна операція ще триває");
        return NULL;
    }
    FichierJob *job = &app.download;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->cancelled, false);
    atomic_init(&job->unpacking, false);
    atomic_init(&job->received, 0);
    atomic_init(&job->total, 0);
    atomic_init(&job->streams, 0);
    atomic_init(&job->waiting, 0);
    snprintf(app.transfer_name, sizeof(app.transfer_name), "%s", name);
    app.transfer_url[0] = 0;
    app.file_id[0] = 0;
    return job;
}

static void launch_transfer(void) {
    FichierJob *job = &app.download;
    app.speed_time = SDL_GetTicks64();
    app.previous_bytes = 0;
    app.speed = 0;
    app.download_finished = false;
    app.download_busy = start_worker(&app.download_thread, download_worker, job);
    if (!app.download_busy) {
        strcpy(app.notice, "Не вдалося запустити операцію");
        return;
    }
    app.notice[0] = 0;
    if (app.screen != DOWNLOADS) app.previous = app.screen;
    app.screen = DOWNLOADS;
}

static void start_download(const GameFile *file) {
    FichierJob *job = new_transfer(file->title);
    if (!job) return;
    strcpy(app.file_id, file->id);
    job->dlarchive = file->source == FILE_DLARCHIVE;
    job->dlarchive_id = app.file_id;
    job->romshq = file->source == FILE_ROMSHQ_SIGNED;
    job->romshq_id = job->romshq ? app.file_id : NULL;
    job->datanodes = file->source == FILE_DATANODES;
    strcpy(app.transfer_url, job->romshq ? app.game_sources[app.file_source].page : file->url);
    job->url = app.transfer_url;
    launch_transfer();
}

static bool keyboard(char *out, size_t capacity, const char *title, const char *guide, const char *button) {
    SwkbdConfig config;
    if (R_FAILED(swkbdCreate(&config, 0))) return false;
    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetHeaderText(&config, title);
    swkbdConfigSetGuideText(&config, guide);
    swkbdConfigSetOkButtonText(&config, button);
    swkbdConfigSetInitialText(&config, out);
    swkbdConfigSetStringLenMax(&config, capacity - 1);
    char value[2048];
    Result rc = swkbdShow(&config, value, capacity);
    swkbdClose(&config);
    if (R_FAILED(rc)) return false;
    const char *start = value + strspn(value, " \t\r\n");
    size_t length = strlen(start);
    while (length && strchr(" \t\r\n", start[length - 1])) --length;
    if (!length) return false;
    memcpy(out, start, length);
    out[length] = 0;
    return true;
}

static void download_link(void) {
    char url[2048] = {0};
    if (!keyboard(url, sizeof(url), "Завантаження за посиланням", "Встав URL ROMsHQ /d/ або DataNodes", "Запустити"))
        return;
    FichierJob *job = new_transfer("Файл за посиланням");
    if (!job) return;
    strcpy(app.transfer_url, url);
    job->url = app.transfer_url;
    job->romshq = !strncmp(url, "https://romshq.com/d/", sizeof("https://romshq.com/d/") - 1);
    job->datanodes = !job->romshq;
    launch_transfer();
}

static void start_unpack(const char *file) {
    FichierJob *job = new_transfer(file);
    if (!job) return;
    job->local_unpack = true;
    snprintf(app.transfer_url, sizeof(app.transfer_url), DOWNLOAD_DIR "/%s", file);
    job->url = app.transfer_url;
    launch_transfer();
}

static void open_installer(void) {
    const char *paths[] = {
        "sdmc:/switch/DBI/DBI.nro", "sdmc:/switch/DBI.nro",
        "sdmc:/switch/DBI/dbi.nro", "sdmc:/switch/dbi.nro"
    };
    app.confirm_install = false;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (!access(paths[i], R_OK) && envHasNextLoad() &&
            R_SUCCEEDED(envSetNextLoad(paths[i], paths[i]))) {
            app.close = true;
            return;
        }
    }
    strcpy(app.notice, "DBI не знайдено. Потрібен DBI.nro в папці switch/DBI/.");
}

static bool guess_romshq_page(Game *game, const char *title) {
    char slug[CATALOG_TEXT];
    size_t used = 0;
    bool dash = false;
    for (const unsigned char *p = (const unsigned char *)title; *p; ++p) {
        unsigned char c = *p;
        // Trademark and registered marks do not belong to a game URL slug.
        if (c == 0xe2 && p[1] == 0x84 && p[2] == 0xa2) { p += 2; continue; }
        if (c == 0xc2 && p[1] == 0xae) { ++p; continue; }
        if (c >= 128) return false; // Do not invent slugs for non-ASCII titles.
        if (isalnum(c)) {
            if (used + 2 >= sizeof(slug)) return false;
            if (dash && used) slug[used++] = '-';
            slug[used++] = (char)tolower(c);
            dash = false;
        } else if (c != 39 && c != 96) {
            dash = true;
        }
    }
    if (used < 3) return false;
    slug[used] = 0;
    snprintf(game->page, sizeof(game->page), "https://romshq.com/game/%s", slug);
    snprintf(game->title, sizeof(game->title), "%s", title);
    return true;
}

static void enter_game_files(void) {
    if (app.catalog_busy) atomic_store(&app.pending.cancel, true);
    app.game_tab = GAME_FILES;
    app.file_source_chosen = false;
    app.file_index = 0;
    app.game_error[0] = 0;
}

static void open_file_source(CatalogSource source) {
    if (!app.game_sources[source].page[0]) {
        snprintf(app.game_error, sizeof(app.game_error),
                 "%s: цю гру не знайдено в каталозі джерела", source_name(source));
        return;
    }
    if (app.catalog_busy) {
        strcpy(app.game_error, "Попередній запит ще завершується. Повтори A.");
        return;
    }
    app.file_source = source;
    app.file_source_chosen = true;
    app.file_index = 0;
    app.game_error[0] = 0;
    memset(&app.details, 0, sizeof(app.details));
    start_game();
}

static void input(u64 buttons) {
    if (!buttons) return;
    if (app.confirm_install) {
        if (buttons & HidNpadButton_A) open_installer();
        if (buttons & HidNpadButton_B) app.confirm_install = false;
        return;
    }
    if (buttons & HidNpadButton_Plus) { app.close = true; return; }
    if (buttons & HidNpadButton_Y) {
        app.notice[0] = 0;
        if (app.screen == DOWNLOADS) app.screen = app.previous;
        else {
            refresh_saved();
            app.previous = app.screen;
            app.screen = DOWNLOADS;
        }
        return;
    }
    if (app.screen == DOWNLOADS) {
        app.saved_index = move(app.saved_index, app.saved_count, buttons);
        if (buttons & HidNpadButton_B) app.screen = app.previous;
        if ((buttons & HidNpadButton_X) && app.download_busy)
            atomic_store(&app.download.cancelled, true);
        if ((buttons & HidNpadButton_ZR) && !app.download_busy) download_link();
        if ((buttons & HidNpadButton_A) && app.saved_count && !app.download_busy) {
            const char *selected_file = app.saved[app.saved_index].file;
            if (saved_rar(selected_file)) start_unpack(selected_file);
            else app.confirm_install = true;
        }
        return;
    }
    if (app.screen == GAME) {
        if (buttons & HidNpadButton_B) {
            if (app.game_tab == GAME_FILES) {
                if (app.file_source_chosen) {
                    if (app.catalog_busy) atomic_store(&app.pending.cancel, true);
                    app.file_source_chosen = false;
                    app.game_error[0] = 0;
                } else {
                    app.game_tab = GAME_OVERVIEW;
                }
                return;
            }
            if (app.catalog_busy) atomic_store(&app.pending.cancel, true);
            if (app.gallery_busy) atomic_store(&app.gallery_job.cancel, true);
            app.notice[0] = 0;
            app.screen = LIBRARY;
            return;
        }
        GameTab previous_tab = app.game_tab;
        if ((buttons & HidNpadButton_L) && app.game_tab > GAME_OVERVIEW) --app.game_tab;
        if ((buttons & HidNpadButton_R) && app.game_tab < GAME_FILES) ++app.game_tab;
        if (app.game_tab == GAME_FILES && previous_tab != GAME_FILES) enter_game_files();
        if (app.game_tab == GAME_FILES) {
            if (!app.file_source_chosen) {
                if (buttons & (up_buttons | HidNpadButton_Left | HidNpadButton_StickLLeft))
                    app.file_source = CATALOG_NSROMS;
                if (buttons & (down_buttons | HidNpadButton_Right | HidNpadButton_StickLRight))
                    app.file_source = CATALOG_ROMSHQ;
                if (buttons & HidNpadButton_A) open_file_source(app.file_source);
            } else {
                app.file_index = move(app.file_index, app.details.count, buttons);
                if ((buttons & HidNpadButton_A) && !app.catalog_busy && app.details.count)
                    start_download(&app.details.files[app.file_index]);
            }
        } else if (app.game_tab == GAME_SCREENSHOTS) {
            if (app.screenshot_index && (buttons & (HidNpadButton_Left | HidNpadButton_StickLLeft)))
                --app.screenshot_index;
            if (app.screenshot_index + 1 < app.gallery.count &&
                (buttons & (HidNpadButton_Right | HidNpadButton_StickLRight))) ++app.screenshot_index;
            if (buttons & HidNpadButton_A) enter_game_files();
        } else {
            if ((buttons & HidNpadButton_X) && app.metadata.original[0]) {
                app.show_original = !app.show_original;
                update_description();
            }
            int height = 0;
            if (app.description) SDL_QueryTexture(app.description, NULL, NULL, NULL, &height);
            if (buttons & up_buttons) app.description_scroll = SDL_max(0, app.description_scroll - 25);
            if (buttons & down_buttons)
                app.description_scroll = SDL_min(SDL_max(0, height - 244), app.description_scroll + 25);
            if (buttons & HidNpadButton_A) enter_game_files();
        }
        return;
    }
    app.game_index = move(app.game_index, app.game_count, buttons);
    if ((buttons & HidNpadButton_X) &&
               keyboard(app.query, sizeof(app.query), "Пошук ігор", "Введи назву гри", "Пошук")) {
        rebuild_view(NULL);
    } else if ((buttons & HidNpadButton_B) && app.query[0]) {
        app.query[0] = 0;
        rebuild_view(NULL);
    } else if (buttons & HidNpadButton_ZR) {
        refresh_catalogs();
    } else if ((buttons & HidNpadButton_L) && app.game_count) {
        app.game_index -= SDL_min(app.game_index, (size_t)40);
    } else if ((buttons & HidNpadButton_R) && app.game_count) {
        app.game_index = SDL_min(app.game_index + 40, app.game_count - 1);
    } else if (buttons & HidNpadButton_A) {
        if (!app.game_count) {
            if (!app.entry_count && !app.index_busy) refresh_catalogs();
        } else if (!app.catalog_busy) {
            const CatalogEntry *entry = &app.entries[app.rows[app.game_index]];
            app.active_game = *game_at(app.game_index);
            memset(app.game_sources, 0, sizeof(app.game_sources));
            for (unsigned source = 0; source < 2; ++source)
                if (entry->games[source]) app.game_sources[source] = *entry->games[source];
            app.show_original = false;
            memset(&app.details, 0, sizeof(app.details));
            memset(&app.metadata, 0, sizeof(app.metadata));
            memset(&app.gallery, 0, sizeof(app.gallery));
            read_game_metadata(&app.metadata, app.active_game.title, "");
            app.romshq_candidate = false;
            if (!app.game_sources[CATALOG_ROMSHQ].page[0] && app.metadata.title[0])
                app.romshq_candidate =
                    guess_romshq_page(&app.game_sources[CATALOG_ROMSHQ], app.metadata.title);
            app.gallery_requested = false;
            SDL_DestroyTexture(app.cover);
            app.cover = NULL;
            update_description();
            app.game_error[0] = 0;
            app.file_index = 0;
            app.screenshot_index = 0;
            app.game_tab = GAME_OVERVIEW;
            app.screen = GAME;
            app.file_source = entry->games[CATALOG_NSROMS] ? CATALOG_NSROMS : CATALOG_ROMSHQ;
            app.file_source_chosen = false;
        }
    }
}

static void fit_cover(SDL_Texture *texture, int x, int y, int width, int height) {
    int w, h;
    if (!texture || SDL_QueryTexture(texture, NULL, NULL, &w, &h) || w <= 0 || h <= 0) return;
    double scale = SDL_min((double)width / w, (double)height / h);
    int dw = SDL_max(1, (int)(w * scale)), dh = SDL_max(1, (int)(h * scale));
    SDL_Rect dst = { x + (width - dw) / 2, y + (height - dh) / 2, dw, dh };
    SDL_RenderCopy(app.renderer, texture, NULL, &dst);
}

static void draw_library(void) {
    rect(40, 106, 500, 530, panel);
    rect(556, 106, 684, 530, panel);
    char header[160];
    if (app.query[0]) snprintf(header, sizeof(header), "Пошук · %s", app.query);
    else strcpy(header, "Каталог ігор");
    text(app.normal, header, 60, 125, 450, white);
    snprintf(header, sizeof(header), app.query[0] ? "Знайдено: %zu" : "Ігор: %zu", app.game_count);
    text(app.small, header, 60, 603, 420, gray);
    size_t first = first_visible(app.game_index, ROWS);
    for (size_t i = first; i < app.game_count && i < first + ROWS; ++i) {
        int y = 170 + (int)(i - first) * 46;
        if (i == app.game_index) {
            rect(55, y, 470, 42, selected);
            rect(55, y, 3, 42, blue);
        }
        CachedCover *cover = find_cover(game_at(i)->cover);
        if (cover) fit_cover(cover->texture, 64, y + 3, 41, 36);
        text(app.normal, game_at(i)->title, 116, y + 8, 398,
             i == app.game_index ? white : gray);
    }
    if (app.game_count) {
        text(app.large, game_at(app.game_index)->title, 588, 150, 610, white);
        CachedCover *cover = find_cover(game_at(app.game_index)->cover);
        if (cover) fit_cover(cover->texture, 588, 214, 620, 314);
        const CatalogEntry *entry = &app.entries[app.rows[app.game_index]];
        const char *sources = entry->games[CATALOG_NSROMS] ?
            (entry->games[CATALOG_ROMSHQ] ? "NSRoms · ROMsHQ" : "NSRoms") : "ROMsHQ";
        text(app.small, sources, 588, 541, 604, blue);
    } else if (!app.index_busy) {
        text(app.normal, app.query[0] ? "Ігор за цим запитом не знайдено" : "Каталог порожній",
             588, 152, 610, gray);
    }
    for (unsigned source = 0; source < 2; ++source) {
        const char *name = source_name((CatalogSource)source);
        int y = 568 + 19 * (int)source;
        if (app.index_busy && app.index_job.source == (CatalogSource)source) {
            unsigned page = atomic_load(&app.index_job.page), pages = atomic_load(&app.index_job.pages);
            if (app.download_busy) snprintf(header, sizeof(header), "%s · оновлення призупинено", name);
            else if (page && pages) snprintf(header, sizeof(header), "%s · %u / %u", name, page, pages);
            else if (page) snprintf(header, sizeof(header), "%s · сторінка %u", name, page);
            else snprintf(header, sizeof(header), "%s · оновлення…", name);
            text(app.small, header, 588, y, 610, blue);
        } else if (app.index_notice[source][0]) {
            snprintf(header, sizeof(header), "%s · %.120s", name, app.index_notice[source]);
            text(app.small, header, 588, y, 610, gray);
        }
    }
    text(app.small, app.notice, 588, 609, 610, gray);
}

static void draw_cover(void) {
    rect(56, 127, 308, 308, background);
    CachedCover *preview = find_cover(app.active_game.cover);
    SDL_Texture *cover = app.cover ? app.cover : preview ? preview->texture : NULL;
    fit_cover(cover, 56, 127, 308, 308);
    text(app.normal, "Nintendo Switch", 65, 459, 293, white);
    text(app.small, app.metadata.publisher, 65, 498, 293, gray);
    if (app.metadata.released[0]) {
        char release[48];
        snprintf(release, sizeof(release), "Вихід · %s", app.metadata.released);
        text(app.small, release, 65, 529, 293, gray);
    }
    if (app.metadata.title[0])
        text(app.small, "Дані TitleDB", 65, 596, 293, blue);
}

static void draw_game_overview(void) {
    if (app.description) {
        int width, height;
        if (SDL_QueryTexture(app.description, NULL, NULL, &width, &height) == 0) {
            int visible = SDL_min(height - app.description_scroll, 244);
            SDL_Rect src = {0, app.description_scroll, width, visible};
            SDL_Rect dst = {430, 269, width, visible};
            SDL_RenderCopy(app.renderer, app.description, &src, &dst);
            if (height > 244)
                text(app.small, "↑ / ↓  Читати опис", 430, 522, 769, gray);
        }
    } else {
        text(app.normal, !app.metadata.title[0] && app.catalog_busy ? "Пошук у TitleDB…" :
             "Опису в TitleDB немає", 430, 269, 769, gray);
    }
    if (app.metadata.original[0]) {
        text(app.small, app.show_original ? "Англійський оригінал" : "Український опис", 430, 548, 769, gray);
        text(app.small, app.show_original ? "X  Українською" : "X  Показати оригінал", 430, 577, 769, blue);
    }
}

static void draw_game_screenshots(void) {
    if (!app.gallery.count) {
        const char *status = app.gallery_busy || !app.gallery_requested ? "Пошук скриншотів…" :
                             app.gallery.issue[0] ? app.gallery.issue :
                             "Скриншоти Nintendo Life недоступні";
        text(app.normal, status, 430, 273, 769, gray);
        return;
    }
    const char *url = app.gallery.screenshots[app.screenshot_index];
    CachedCover *shot = find_cover(url);
    rect(430, 259, 774, 309, background);
    if (shot && shot->texture) fit_cover(shot->texture, 430, 259, 774, 309);
    else text(app.normal, shot ? "Скриншот недоступний" :
              app.download_busy ? "Перегляд доступний після завершення завантаження" :
              "Завантаження скриншота…", 452, 281, 724, gray);
    char count[80];
    snprintf(count, sizeof(count), "← / →   %zu / %zu  ·  %s", app.screenshot_index + 1,
             app.gallery.count, app.gallery.from_romshq ? "ROMsHQ" : app.gallery.from_titledb ? "TitleDB" : "Nintendo Life");
    text(app.small, count, 430, 584, 769, blue);
}

static void draw_game_files(void) {
    if (!app.file_source_chosen) {
        text(app.normal, "Оберіть джерело файлів", 430, 268, 770, white);
        for (unsigned source = 0; source < 2; ++source) {
            int x = 430 + (int)source * 395;
            bool focus = app.file_source == (CatalogSource)source;
            bool available = app.game_sources[source].page[0] != 0;
            rect(x, 322, 380, 166, focus ? selected : background);
            if (focus) rect(x, 322, 380, 4, blue);
            text(app.large, source_name((CatalogSource)source), x + 20, 342, 340,
                 focus ? white : gray);
            const char *state = available ?
                source == CATALOG_ROMSHQ && app.romshq_candidate ?
                "Спробувати знайти за назвою" : "Відкрити список файлів" :
                app.index_busy && app.index_job.source == (CatalogSource)source ?
                "Каталог завантажується…" :
                app.catalogs[source].count ? "Гру не знайдено в каталозі" :
                "Каталог джерела недоступний";
            text(app.small, state, x + 20, 413, 345, available ? blue : gray);
        }
        if (app.game_error[0])
            text(app.small, app.game_error, 430, 522, 775, blue);
        text(app.small, "← / →  Джерело    A  Відкрити    B  До огляду",
             430, 584, 775, gray);
        return;
    }
    if (app.catalog_busy) {
        text(app.normal, "Перевірка файлів…", 430, 275, 770, gray);
    } else if (app.game_error[0] || !app.details.count) {
        text(app.normal, app.game_error[0] ? app.game_error : "Для цієї гри немає доступних файлів",
             430, 275, 770, gray);
    } else {
        size_t first = first_visible(app.file_index, FILE_ROWS);
        for (size_t i = first; i < app.details.count && i < first + FILE_ROWS; ++i)
            row(i, app.file_index, 429, 267 + (int)(i - first) * 48, 782, app.details.files[i].title);
    }
    char providers[96];
    snprintf(providers, sizeof(providers), "%s   ·   B  Інше джерело", source_name(app.file_source));
    text(app.small, providers, 430, 552, 769, gray);
    text(app.small, app.notice, 430, 595, 772, gray);
}

static void draw_game(void) {
    rect(40, 106, 345, 530, panel);
    rect(401, 106, 839, 530, panel);
    draw_cover();
    const char *title = app.metadata.title[0] ? app.metadata.title :
                        app.details.title[0] ? app.details.title : app.active_game.title;
    text(app.large, title, 430, 130, 772, white);
    const char *tabs[] = {"Огляд", "Скриншоти", "Файли"};
    for (unsigned i = 0; i < sizeof(tabs) / sizeof(tabs[0]); ++i) {
        int x = 430 + (int)i * 252;
        bool selected_tab = (GameTab)i == app.game_tab;
        rect(x, 201, 242, 42, selected_tab ? selected : background);
        if (selected_tab) rect(x, 240, 242, 3, blue);
        text(app.normal, tabs[i], x + 13, 208, 215, selected_tab ? white : gray);
    }
    if (app.game_tab == GAME_OVERVIEW) draw_game_overview();
    else if (app.game_tab == GAME_SCREENSHOTS) draw_game_screenshots();
    else draw_game_files();
}

static void draw_downloads(void) {
    rect(40, 106, 1200, 530, panel);
    text(app.large, "Завантаження", 66, 128, 1100, white);
    if (app.download_busy || app.download_finished) {
        uint64_t have = atomic_load(&app.download.received), total = atomic_load(&app.download.total);
        unsigned streams = atomic_load(&app.download.streams);
        double percent = total ? SDL_min(100.0, 100.0 * have / total) : 0;
        char line[192];
        text(app.normal, app.transfer_name, 66, 182, 1100, white);
        if (streams > 1) {
            char connections[48];
            snprintf(connections, sizeof(connections), "HTTP Range · %u з’єднань", streams);
            text(app.small, connections, 920, 183, 290, blue);
        }
        rect(66, 225, 1148, 8, selected);
        rect(66, 225, (int)(1148 * percent / 100.0), 8, blue);
        if (app.download_busy && atomic_load(&app.download.unpacking)) {
            snprintf(line, sizeof(line), "Розпакування RAR · %.1f / %.1f МіБ", have / 1048576.0, total / 1048576.0);
        } else if (app.download_busy && total) {
            snprintf(line, sizeof(line), "%.1f%%  ·  %.1f / %.1f МіБ  ·  %.1f МіБ/с",
                     percent, have / 1048576.0, total / 1048576.0, app.speed / 1048576.0);
        } else if (app.download_busy && have) {
            snprintf(line, sizeof(line), "%.1f МіБ  ·  %.1f МіБ/с", have / 1048576.0, app.speed / 1048576.0);
        } else if (app.download_busy) {
            strcpy(line, "Запит адреси файлу…");
        } else if (!strcmp(app.download.result, "Download saved")) {
            snprintf(line, sizeof(line), "Збережено · %.1f МіБ · середня %.1f МіБ/с",
                     have / 1048576.0, app.download.avg_speed / 1048576.0);
        } else {
            snprintf(line, sizeof(line), "%s  ·  API HTTP %ld  ·  Файл HTTP %ld",
                     app.download.result, app.download.api_http, app.download.http);
        }
        text(app.small, line, 66, 249, 1148, blue);
        if (app.download_finished)
            text(app.small, app.download.tls_detail, 66, 270, 1148, gray);
    }
    text(app.normal, "Збережені файли", 66, 290, 1100, white);
    size_t first = first_visible(app.saved_index, FILE_ROWS);
    for (size_t i = first; i < app.saved_count && i < first + FILE_ROWS; ++i)
        row(i, app.saved_index, 66, 335 + (int)(i - first) * 46, 1148, app.saved[i].file);
    if (!app.saved_count) text(app.normal, "Поки немає збережених файлів", 66, 335, 1100, gray);
    text(app.small, app.notice, 66, 595, 1148, gray);
}

static void draw(void) {
    rect(0, 0, 1280, 720, background);
    text(app.large, "NXLoad", 40, 24, 320, white);
    text(app.small, "1.0.16", 206, 44, 120, gray);
    if (app.download_busy)
        text(app.small, atomic_load(&app.download.unpacking) ? "Розпакування триває" : "Завантаження триває", 950, 40, 290, blue);
    if (app.screen == LIBRARY) draw_library();
    else if (app.screen == GAME) draw_game();
    else draw_downloads();
    const char *footer = app.screen == LIBRARY ?
        "A  Відкрити    X  Пошук    L / R  Гортати    ZR  Оновити    Y  Завантаження    +  Вихід" :
        app.screen == GAME ? (app.game_tab == GAME_FILES ?
        (app.file_source_chosen ?
        "A  Завантажити    B  Джерела    L / R  Вкладки    Y  Завантаження    +  Вихід" :
        "← / →  Джерело    A  Обрати    B  Огляд    L / R  Вкладки    +  Вихід") :
        app.game_tab == GAME_OVERVIEW && app.metadata.original[0] ?
        "A  До файлів    X  Мова опису    L / R  Вкладки    B  Каталог    Y  Завантаження    +  Вихід" :
        "A  До файлів    L / R  Вкладки    B  Каталог    Y  Завантаження    +  Вихід") :
        app.download_busy ? "X  Скасувати    B  Назад    +  Вихід" :
        "A  Відкрити DBI / розпакувати RAR    ZR  Посилання    B  Назад    +  Вихід";
    text(app.small, footer, 40, 665, 1200, gray);
    if (app.confirm_install) {
        rect(190, 200, 900, 305, selected);
        text(app.large, "Відкрити встановлювач DBI?", 224, 230, 828, white);
        text(app.normal, app.saved[app.saved_index].file, 224, 298, 828, white);
        text(app.small, "У DBI відкрий Browse SD Card → switch → SwitchDownloadProbe", 224, 355, 828, gray);
        text(app.normal, "A  Відкрити DBI     B  Назад", 224, 428, 828, blue);
    }
    SDL_RenderPresent(app.renderer);
}

static TTF_Font *font_open(const PlFontData *font, int points) {
    SDL_RWops *memory = SDL_RWFromConstMem(font->address, font->size);
    return memory ? TTF_OpenFontRW(memory, 1, points) : NULL;
}

void nx_tls_threading_init(void);
void nx_tls_threading_exit(void);

int main(void) {
    nx_tls_threading_init();
    bool romfs_ready = R_SUCCEEDED(romfsInit());
    Result entropy = csrngInitialize();
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.tcp_rx_buf_size = 512 * 1024;
    socket_config.tcp_rx_buf_max_size = 2 * 1024 * 1024;
    socket_config.sb_efficiency = 8;
    Result sockets = socketInitialize(&socket_config);
    if (R_FAILED(sockets)) sockets = socketInitializeDefault();
    bool curl_ready = romfs_ready && R_SUCCEEDED(entropy) && R_SUCCEEDED(sockets) && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    Result font_service = plInitialize(PlServiceType_User);
    PlFontData font = {0};
    bool ok = curl_ready && R_SUCCEEDED(font_service) &&
        R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard));
    SDL_SetMainReady();
    ok = ok && SDL_Init(SDL_INIT_VIDEO) == 0 && TTF_Init() == 0;
    if (ok) {
        IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
        SDL_CreateWindowAndRenderer(1280, 720, 0, &app.window, &app.renderer);
        app.small = font_open(&font, 18);
        app.normal = font_open(&font, 23);
        app.large = font_open(&font, 39);
        ok = app.window && app.renderer && app.small && app.normal && app.large;
    }
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    if (ok) {
        SDL_RenderSetLogicalSize(app.renderer, 1280, 720);
        open_catalogs();
        uint64_t next_frame = 0, next_scroll = 0;
        u64 held = 0;
        while (appletMainLoop() && !app.close) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {}
            padUpdate(&pad);
            finish_jobs();
            u64 buttons = padGetButtonsDown(&pad);
            u64 vertical = padGetButtons(&pad) & (up_buttons | down_buttons);
            uint64_t now = SDL_GetTicks64();
            if (held != vertical || (buttons & (up_buttons | down_buttons))) {
                next_scroll = now + 280;
            } else if (vertical && now >= next_scroll) {
                buttons |= vertical;
                next_scroll = now + 110;
            }
            held = vertical;
            input(buttons);
            pump_index();
            pump_gallery();
            pump_covers();
            if (buttons || now >= next_frame) {
                draw();
                next_frame = now + 150;
            }
            SDL_Delay(16);
        }
    } else {
        SDL_Quit();
        consoleInit(NULL);
        printf("NXLoad startup error: %s\n\nPress + to exit.\n", SDL_GetError());
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
    }
    if (app.index_busy) atomic_store(&app.index_job.cancel, true);
    if (app.gallery_busy) atomic_store(&app.gallery_job.cancel, true);
    if (app.catalog_busy) {
        atomic_store(&app.pending.cancel, true);
        threadWaitForExit(&app.catalog_thread);
        threadClose(&app.catalog_thread);
        free(app.pending.details.cover);
    }
    if (app.download_busy) {
        atomic_store(&app.download.cancelled, true);
        threadWaitForExit(&app.download_thread);
        threadClose(&app.download_thread);
    }
    if (app.cover_busy) {
        atomic_store(&app.cover_job.cancel, true);
        threadWaitForExit(&app.cover_thread);
        threadClose(&app.cover_thread);
        free(app.cover_job.bytes);
    }
    if (app.index_busy) {
        threadWaitForExit(&app.index_thread);
        threadClose(&app.index_thread);
    }
    if (app.gallery_busy) {
        threadWaitForExit(&app.gallery_thread);
        threadClose(&app.gallery_thread);
    }
    for (size_t i = 0; i < COVER_SLOTS; ++i) SDL_DestroyTexture(app.covers[i].texture);
    SDL_DestroyTexture(app.cover);
    SDL_DestroyTexture(app.description);
    if (app.small) TTF_CloseFont(app.small);
    if (app.normal) TTF_CloseFont(app.normal);
    if (app.large) TTF_CloseFont(app.large);
    if (app.renderer) SDL_DestroyRenderer(app.renderer);
    if (app.window) SDL_DestroyWindow(app.window);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    if (R_SUCCEEDED(font_service)) plExit();
    if (curl_ready) curl_global_cleanup();
    nx_tls_threading_exit();
    if (R_SUCCEEDED(sockets)) socketExit();
    if (R_SUCCEEDED(entropy)) csrngExit();
    if (romfs_ready) romfsExit();
    return 0;
}
