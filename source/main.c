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
#include "eshop.h"

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
    size_t rows[CATALOG_CAPACITY];
    size_t game_count, game_index, file_index, saved_count, saved_index;
    CatalogIndex catalog;
    CatalogIndexJob index_job;
    Thread index_thread;
    bool index_busy, show_partial_index;
    char index_notice[CATALOG_TEXT];
    Game active_game;
    GameTab game_tab;
    size_t screenshot_index;
    int description_scroll;
    EshopGame shop;
    EshopJob shop_job;
    Thread shop_thread;
    bool shop_busy, shop_requested;
    bool confirm_install, close, catalog_busy, download_busy, download_finished;
    char query[96], notice[192], game_error[192], file_id[DLARCHIVE_ID_CAPACITY];
    char transfer_name[CATALOG_TEXT];
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
static void shop_worker(void *arg) { eshop_lookup(arg); }
static void index_worker(void *arg) { catalog_build_index(arg); }
static void download_worker(void *arg) { fichier_download(arg); }

static bool start_worker(Thread *thread, ThreadFunc function, void *arg) {
    Result rc = threadCreate(thread, function, arg, NULL, 128 * 1024, 0x2c, -2);
    if (R_FAILED(rc)) return false;
    rc = threadStart(thread);
    if (R_FAILED(rc)) threadClose(thread);
    return R_SUCCEEDED(rc);
}

static const Game *game_at(size_t row) {
    return &app.catalog.games[app.rows[row]];
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
    for (size_t i = 0; i < app.catalog.count; ++i) {
        const Game *game = &app.catalog.games[i];
        if (!match_query(game)) continue;
        if (keep && !strcmp(game->page, keep)) {
            app.game_index = app.game_count;
            found = true;
        }
        app.rows[app.game_count++] = i;
    }
    if (keep && !found && app.game_count)
        app.game_index = SDL_min(old_index, app.game_count - 1);
}

static void start_index(void) {
    if (app.index_busy) return;
    CatalogIndexJob *job = &app.index_job;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->cancel, false);
    atomic_init(&job->paused, app.download_busy);
    atomic_init(&job->published, 0);
    atomic_init(&job->page, 0);
    atomic_init(&job->pages, 0);
    app.show_partial_index = app.catalog.count == 0;
    app.index_notice[0] = 0;
    app.index_busy = start_worker(&app.index_thread, index_worker, job);
    if (!app.index_busy) strcpy(app.index_notice, "Не вдалося запустити оновлення каталогу");
}

static void start_game(void) {
    if (app.catalog_busy) return;
    CatalogJob *job = &app.pending;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->cancel, false);
    job->game = app.active_game;
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
    job->bytes = catalog_fetch_cover(job->url, &job->cancel, &job->count);
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
    if (app.cover_busy || app.download_busy || app.shop_busy) return;

    const char *url = NULL;
    bool screenshot = false;
    if (app.screen == GAME) {
        if (app.game_tab == GAME_SCREENSHOTS && app.shop.screenshot_count) {
            const char *image = app.shop.screenshots[app.screenshot_index];
            if (!find_cover(image)) {
                screenshot = true;
                url = image;
            }
        }
        if (!url && app.shop.cover[0] && !find_cover(app.shop.cover)) url = app.shop.cover;
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
    const char *summary = app.shop.description;
    if (!*summary) return;
    SDL_Surface *paragraph = TTF_RenderUTF8_Blended_Wrapped(app.normal, summary, gray, 750);
    if (!paragraph) return;
    app.description = SDL_CreateTextureFromSurface(app.renderer, paragraph);
    SDL_FreeSurface(paragraph);
}

static void pump_shop(void) {
    EshopJob *job = &app.shop_job;
    if (app.shop_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.shop_thread), 0))) {
        threadWaitForExit(&app.shop_thread);
        threadClose(&app.shop_thread);
        app.shop_busy = false;
        if (!atomic_load(&job->cancel) && !strcmp(job->game.page, app.active_game.page)) {
            app.shop = job->match;
            update_description();
        }
    }
    if (app.screen == GAME && !app.shop_busy && !app.shop_requested &&
        !app.download_busy && !app.catalog_busy && !app.cover_busy) {
        memset(job, 0, sizeof(*job));
        atomic_init(&job->cancel, false);
        job->game = app.active_game;
        if (app.details.title[0])
            strcpy(job->heading, app.details.title);
        app.shop_requested = true;
        app.shop_busy = start_worker(&app.shop_thread, shop_worker, job);
        if (!app.shop_busy) strcpy(app.shop.issue, "Не вдалося запустити запит Nintendo eShop");
    }
}

static bool package_name(const char *name) {
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".nsp") || !strcasecmp(ext, ".xci") ||
                   !strcasecmp(ext, ".nsz") || !strcasecmp(ext, ".xcz"));
}

static int file_compare(const void *a, const void *b) {
    return strcasecmp(((const SavedFile *)a)->file, ((const SavedFile *)b)->file);
}

static void refresh_saved(void) {
    app.saved_count = 0;
    DIR *dir = opendir(DOWNLOAD_DIR);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) && app.saved_count < HISTORY_LIMIT) {
            size_t n = strlen(entry->d_name);
            if (n < sizeof(app.saved[0].file) && package_name(entry->d_name)) {
                strcpy(app.saved[app.saved_count++].file, entry->d_name);
            }
        }
        closedir(dir);
    }
    qsort(app.saved, app.saved_count, sizeof(app.saved[0]), file_compare);
    if (app.saved_index >= app.saved_count) app.saved_index = 0;
}

static void pump_index(void) {
    if (!app.index_busy) return;
    CatalogIndexJob *job = &app.index_job;
    bool game_request = app.screen == GAME && (app.catalog_busy || app.shop_busy || !app.shop_requested);
    atomic_store(&job->paused, app.download_busy || game_request);
    size_t published = atomic_load(&job->published);
    if (app.show_partial_index && published > app.catalog.count) {
        char keep[CATALOG_URL] = {0};
        if (app.game_count) strcpy(keep, game_at(app.game_index)->page);
        size_t from = app.catalog.count;
        memcpy(app.catalog.games + from, job->index.games + from, (published - from) * sizeof(Game));
        app.catalog.count = published;
        rebuild_view(keep);
    }
    if (R_FAILED(waitSingle(waiterForThread(&app.index_thread), 0))) return;
    threadWaitForExit(&app.index_thread);
    threadClose(&app.index_thread);
    app.index_busy = false;
    if (job->complete) {
        char keep[CATALOG_URL] = {0};
        if (app.game_count) strcpy(keep, game_at(app.game_index)->page);
        memcpy(app.catalog.games, job->index.games, job->index.count * sizeof(Game));
        app.catalog.count = job->index.count;
        app.catalog.saved_at = job->index.saved_at;
        rebuild_view(keep);
    }
    strcpy(app.index_notice, job->error);
}

static void finish_jobs(void) {
    if (app.catalog_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.catalog_thread), 0))) {
        threadWaitForExit(&app.catalog_thread);
        threadClose(&app.catalog_thread);
        app.catalog_busy = false;
        CatalogJob *job = &app.pending;
        if (!atomic_load(&job->cancel)) {
            app.details = job->details;
            strcpy(app.game_error, job->error);
            update_cover();
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

static void start_download(const GameFile *file) {
    if (app.download_busy) {
        strcpy(app.notice, "Поточне завантаження ще триває");
        return;
    }
    FichierJob *job = &app.download;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->cancelled, false);
    atomic_init(&job->received, 0);
    atomic_init(&job->total, 0);
    atomic_init(&job->streams, 0);
    atomic_init(&job->waiting, 0);
    strcpy(app.file_id, file->id);
    strcpy(app.transfer_name, file->title);
    job->dlarchive = true;
    job->dlarchive_id = app.file_id;
    app.speed_time = SDL_GetTicks64();
    app.previous_bytes = 0;
    app.speed = 0;
    app.download_finished = false;
    app.download_busy = start_worker(&app.download_thread, download_worker, job);
    if (!app.download_busy) {
        strcpy(app.notice, "Не вдалося запустити завантаження");
        return;
    }
    app.notice[0] = 0;
    app.previous = GAME;
    app.screen = DOWNLOADS;
}

static bool keyboard(void) {
    SwkbdConfig config;
    char query[sizeof(app.query)] = {0};
    if (R_FAILED(swkbdCreate(&config, 0))) return false;
    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetHeaderText(&config, "Пошук ігор");
    swkbdConfigSetGuideText(&config, "Введи назву гри");
    swkbdConfigSetOkButtonText(&config, "Пошук");
    swkbdConfigSetInitialText(&config, app.query);
    swkbdConfigSetStringLenMax(&config, sizeof(query) - 1);
    Result rc = swkbdShow(&config, query, sizeof(query));
    swkbdClose(&config);
    if (R_FAILED(rc)) return false;
    char *start = query + strspn(query, " \t\r\n");
    size_t n = strlen(start);
    while (n && strchr(" \t\r\n", start[n - 1])) --n;
    if (!n) return false;
    memcpy(app.query, start, n);
    app.query[n] = 0;
    return true;
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
        if ((buttons & HidNpadButton_A) && app.saved_count && !app.download_busy)
            app.confirm_install = true;
        return;
    }
    if (app.screen == GAME) {
        if (buttons & HidNpadButton_B) {
            if (app.catalog_busy) atomic_store(&app.pending.cancel, true);
            if (app.shop_busy) atomic_store(&app.shop_job.cancel, true);
            app.notice[0] = 0;
            app.screen = LIBRARY;
            return;
        }
        if ((buttons & HidNpadButton_L) && app.game_tab > GAME_OVERVIEW) --app.game_tab;
        if ((buttons & HidNpadButton_R) && app.game_tab < GAME_FILES) ++app.game_tab;
        if (app.game_tab == GAME_FILES) {
            app.file_index = move(app.file_index, app.details.count, buttons);
            if ((buttons & HidNpadButton_A) && !app.catalog_busy && app.details.count)
                start_download(&app.details.files[app.file_index]);
        } else if (app.game_tab == GAME_SCREENSHOTS) {
            if (app.screenshot_index && (buttons & (HidNpadButton_Left | HidNpadButton_StickLLeft)))
                --app.screenshot_index;
            if (app.screenshot_index + 1 < app.shop.screenshot_count &&
                (buttons & (HidNpadButton_Right | HidNpadButton_StickLRight))) ++app.screenshot_index;
            if (buttons & HidNpadButton_A) app.game_tab = GAME_FILES;
        } else {
            int height = 0;
            if (app.description) SDL_QueryTexture(app.description, NULL, NULL, NULL, &height);
            if (buttons & up_buttons) app.description_scroll = SDL_max(0, app.description_scroll - 25);
            if (buttons & down_buttons)
                app.description_scroll = SDL_min(SDL_max(0, height - 244), app.description_scroll + 25);
            if (buttons & HidNpadButton_A) app.game_tab = GAME_FILES;
        }
        return;
    }
    app.game_index = move(app.game_index, app.game_count, buttons);
    if ((buttons & HidNpadButton_X) && keyboard()) {
        rebuild_view(NULL);
    } else if ((buttons & HidNpadButton_B) && app.query[0]) {
        app.query[0] = 0;
        rebuild_view(NULL);
    } else if (buttons & HidNpadButton_ZR) {
        start_index();
    } else if ((buttons & HidNpadButton_L) && app.game_count) {
        app.game_index -= SDL_min(app.game_index, (size_t)40);
    } else if ((buttons & HidNpadButton_R) && app.game_count) {
        app.game_index = SDL_min(app.game_index + 40, app.game_count - 1);
    } else if (buttons & HidNpadButton_A) {
        if (!app.game_count) {
            if (!app.catalog.count) start_index();
        } else if (!app.catalog_busy) {
            app.active_game = *game_at(app.game_index);
            memset(&app.details, 0, sizeof(app.details));
            memset(&app.shop, 0, sizeof(app.shop));
            app.shop_requested = false;
            SDL_DestroyTexture(app.cover);
            app.cover = NULL;
            update_description();
            app.game_error[0] = 0;
            app.file_index = 0;
            app.screenshot_index = 0;
            app.game_tab = GAME_OVERVIEW;
            app.screen = GAME;
            start_game();
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
    snprintf(header, sizeof(header), app.query[0] ? "Пошук · %s" : "Каталог NSRoms", app.query);
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
        text(app.normal, "A  Відкрити картку гри", 588, 541, 604, blue);
    } else if (!app.index_busy) {
        text(app.normal, app.query[0] ? "Ігор за цим запитом не знайдено" : "Каталог порожній",
             588, 152, 610, gray);
    }
    if (app.index_busy) {
        unsigned page = atomic_load(&app.index_job.page), pages = atomic_load(&app.index_job.pages);
        if (app.download_busy) strcpy(header, "Оновлення каталогу призупинено на час завантаження");
        else if (page && pages) snprintf(header, sizeof(header), "Оновлення каталогу · %u / %u", page, pages);
        else if (page) snprintf(header, sizeof(header), "Оновлення каталогу · сторінка %u", page);
        else strcpy(header, "Оновлення каталогу…");
        text(app.small, header, 588, app.game_count ? 577 : 152, 610, blue);
    } else {
        text(app.small, app.index_notice, 588, 577, 610, gray);
    }
    text(app.small, app.notice, 588, 595, 610, gray);
}

static void draw_cover(void) {
    rect(56, 127, 308, 308, background);
    CachedCover *official = find_cover(app.shop.cover);
    CachedCover *preview = find_cover(app.active_game.cover);
    SDL_Texture *cover = official && official->texture ? official->texture :
                         app.cover ? app.cover : preview ? preview->texture : NULL;
    fit_cover(cover, 56, 127, 308, 308);
    text(app.normal, "Nintendo Switch", 65, 459, 293, white);
    text(app.small, app.shop.publisher, 65, 498, 293, gray);
    if (app.shop.released[0]) {
        char release[48];
        snprintf(release, sizeof(release), "Вихід · %s", app.shop.released);
        text(app.small, release, 65, 529, 293, gray);
    }
    if (app.shop.page[0])
        text(app.small, "Дані Nintendo eShop", 65, 596, 293, blue);
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
        text(app.normal, app.shop_busy || !app.shop_requested ? "Завантаження опису…" :
             app.shop.issue[0] ? app.shop.issue : "Опис із Nintendo eShop недоступний", 430, 269, 769, gray);
    }
    text(app.small, app.catalog_busy ? "Перевірка файлів NSRoms…" :
         "A  Перейти до файлів", 430, 577, 769, blue);
}

static void draw_game_screenshots(void) {
    if (!app.shop.screenshot_count) {
        const char *status = app.shop_busy || !app.shop_requested ? "Пошук скриншотів…" :
                             app.shop.issue[0] ? app.shop.issue :
                             app.shop.page[0] ? "Скриншоти Nintendo eShop недоступні" :
                             "Дані гри в Nintendo eShop недоступні";
        text(app.normal, status, 430, 273, 769, gray);
        return;
    }
    const char *url = app.shop.screenshots[app.screenshot_index];
    CachedCover *shot = find_cover(url);
    rect(430, 259, 774, 309, background);
    if (shot && shot->texture) fit_cover(shot->texture, 430, 259, 774, 309);
    else text(app.normal, shot ? "Скриншот недоступний" :
              app.download_busy ? "Перегляд доступний після завершення завантаження" :
              "Завантаження скриншота…", 452, 281, 724, gray);
    char count[80];
    snprintf(count, sizeof(count), "← / →   %zu / %zu", app.screenshot_index + 1, app.shop.screenshot_count);
    text(app.small, count, 430, 584, 769, blue);
}

static void draw_game_files(void) {
    if (app.catalog_busy) {
        text(app.normal, "Перевірка файлів NSRoms…", 430, 275, 770, gray);
    } else if (app.game_error[0] || !app.details.count) {
        text(app.normal, app.game_error[0] ? app.game_error : "Для цієї гри немає файлів DLArchive",
             430, 275, 770, gray);
    } else {
        size_t first = first_visible(app.file_index, FILE_ROWS);
        for (size_t i = first; i < app.details.count && i < first + FILE_ROWS; ++i)
            row(i, app.file_index, 429, 267 + (int)(i - first) * 48, 782, app.details.files[i].title);
    }
    if (app.details.count) text(app.small, "NSRoms · DLArchive", 430, 552, 769, gray);
    text(app.small, app.notice, 430, 595, 772, gray);
}

static void draw_game(void) {
    rect(40, 106, 345, 530, panel);
    rect(401, 106, 839, 530, panel);
    draw_cover();
    const char *title = app.shop.title[0] ? app.shop.title :
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
        if (app.download_busy && total) {
            snprintf(line, sizeof(line), "%.1f%%  ·  %.1f / %.1f МіБ  ·  %.1f МіБ/с",
                     percent, have / 1048576.0, total / 1048576.0, app.speed / 1048576.0);
        } else if (app.download_busy && have) {
            snprintf(line, sizeof(line), "%.1f МіБ  ·  %.1f МіБ/с", have / 1048576.0, app.speed / 1048576.0);
        } else if (app.download_busy) {
            strcpy(line, "Запит адреси файлу в DLArchive…");
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
    text(app.small, "1.0.8", 206, 44, 120, gray);
    if (app.download_busy)
        text(app.small, "Завантаження триває", 950, 40, 290, blue);
    if (app.screen == LIBRARY) draw_library();
    else if (app.screen == GAME) draw_game();
    else draw_downloads();
    const char *footer = app.screen == LIBRARY ?
        "A  Відкрити    X  Пошук    L / R  Гортати    ZR  Оновити    Y  Завантаження    +  Вихід" :
        app.screen == GAME ? (app.game_tab == GAME_FILES ?
        "A  Завантажити    L / R  Вкладки    B  Каталог    Y  Завантаження    +  Вихід" :
        "A  До файлів    L / R  Вкладки    B  Каталог    Y  Завантаження    +  Вихід") :
        "A  Відкрити DBI     X  Скасувати завантаження     B  Назад     +  Вихід";
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
        bool cached = catalog_read_cache(&app.catalog);
        if (cached) rebuild_view(NULL);
        time_t now = time(NULL);
        if (!cached || now < app.catalog.saved_at || now - app.catalog.saved_at >= CATALOG_CACHE_TTL)
            start_index();
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
            pump_shop();
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
    if (app.shop_busy) atomic_store(&app.shop_job.cancel, true);
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
    if (app.shop_busy) {
        threadWaitForExit(&app.shop_thread);
        threadClose(&app.shop_thread);
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
