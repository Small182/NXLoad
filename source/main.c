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
#include "cover_cache.h"
#include "game_info.h"
#include "install.h"
#include "storage.h"
#include "settings.h"
#include "unpack.h"
#include "updater.h"

#define GRID_COLUMNS 6
#define GRID_ROWS 2
#define GRID_X 40
#define GRID_Y 159
#define GRID_STEP_X 202
#define GRID_STEP_Y 219
#define GRID_WIDTH 190
#define GRID_HEIGHT 190
#define GRID_VIEW_TOP (GRID_Y - 6)
#define GRID_VIEW_BOTTOM (GRID_Y + (GRID_ROWS - 1) * GRID_STEP_Y + GRID_HEIGHT + 6)
#define GRID_DRAG_THRESHOLD 8
#define FILE_ROWS 5
#define HISTORY_LIMIT 96
#define COVER_SLOTS 36
#define COVER_WORKERS 3
#define GRID_THUMB_SIZE 180
#define DOWNLOAD_QUEUE_LIMIT 24

typedef enum { LIBRARY, GAME, DOWNLOADS } Screen;
typedef enum { TOP_NONE, TOP_SEARCH, TOP_DOWNLOADS, TOP_SETTINGS } TopFocus;
typedef enum {
    SETTINGS_DEFAULT, SETTINGS_RAR, SETTINGS_NSP, SETTINGS_AUTO_CATALOG,
    SETTINGS_REFRESH_CATALOG, SETTINGS_UPDATE, SETTINGS_ROWS
} SettingRow;

#define TAB_Y 96
#define TAB_HEIGHT 41
#define SETTINGS_Y 152
#define SETTINGS_STEP 59
#define TOP_Y 21
#define TOP_HEIGHT 53
#define SEARCH_X 929
#define DOWNLOADS_X 1108
#define SETTINGS_X 1175
#define TOP_END 1240
typedef enum { GAME_OVERVIEW, GAME_SCREENSHOTS, GAME_FILES } GameTab;
typedef enum { SORT_RATING, SORT_NAME, SORT_RELEASE, SORT_COUNT } SortMode;

static const char *const sort_names[] = {"За рейтингом", "За алфавітом", "За датою виходу"};
static const char *const genre_names[] = {
    "Усі жанри", "Екшн", "Пригоди", "Аркада", "Настільні", "Освітні", "Файтинг",
    "Шутер від першої особи", "Музика", "Для вечірок", "Платформер", "Головоломки",
    "Гонки", "RPG", "Шутер", "Симуляція", "Спорт", "Стратегія", "Інше",
};
#define GENRE_COUNT (sizeof(genre_names) / sizeof(genre_names[0]))
static const size_t genre_tabs[] = {0, 1, 2, 13, 11, 12, 17};
static const int tab_widths[] = {156, 156, 156, 156, 192, 156, 156};
#define TAB_COUNT (sizeof(genre_tabs) / sizeof(genre_tabs[0]))

typedef struct {
    char file[256];
} SavedFile;

typedef struct {
    char name[CATALOG_TEXT], url[2048], id[DLARCHIVE_ID_CAPACITY];
    GameFileSource source;
    bool unpack, install_local, install_after_download, app_update;
} DownloadRequest;

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
    Thread thread;
    CURL *connection;
    bool screenshot, busy, from_disk;
} CoverJob;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *description;
    TTF_Font *small;
    TTF_Font *normal;
    TTF_Font *large;
    Screen screen, previous;
    size_t rows[MERGED_CAPACITY];
    size_t game_count, genre_count, game_index, file_index, saved_count, saved_index;
    int grid_scroll;
    SortMode sort, draft_sort, genre_sort[GENRE_COUNT];
    bool filter_sort_changed;
    size_t genre, draft_genre, filter_row;
    bool filters_open, genre_focus, settings_open;
    TopFocus top_focus;
    size_t settings_row;
    Settings settings;
    uint64_t background_check_at;
    CatalogIndex catalogs[2];
    CatalogEntry entries[MERGED_CAPACITY];
    size_t entry_count;
    CatalogIndexJob index_job;
    Thread index_thread;
    bool index_busy, show_partial_index;
    unsigned refresh_waiting;
    CatalogSource file_source;
    bool file_source_chosen, romshq_candidate;
    char index_notice[2][CATALOG_TEXT], settings_notice[192], update_notice[192];
    Game active_game, game_sources[2];
    const CatalogMetadata *active_metadata;
    GameTab game_tab;
    size_t screenshot_index;
    int description_scroll;
    bool show_original;
    GameMetadata metadata;
    GameGallery gallery;
    GalleryJob gallery_job;
    Thread gallery_thread;
    bool gallery_busy, gallery_requested;
    bool confirm_install, choose_download, close, catalog_busy, download_busy, download_finished;
    size_t download_choice;
    char update_target[256];
    bool update_ready;
    char query[96], notice[192], game_error[192], delete_file[sizeof(((SavedFile *)0)->file)];
    DownloadRequest active_transfer, draft_transfer, queue[DOWNLOAD_QUEUE_LIMIT];
    size_t queue_count, queue_index;
    bool queue_focus;
    SavedFile saved[HISTORY_LIMIT];
    GameDetails details;
    CatalogJob pending;
    FichierJob download;
    Thread catalog_thread, download_thread;
    CoverJob cover_jobs[COVER_WORKERS];
    CachedCover covers[COVER_SLOTS];
    uint64_t previous_bytes;
    double speed;
    uint64_t speed_time;
} App;

static App app;
static const u64 up_buttons = HidNpadButton_Up | HidNpadButton_StickLUp;
static const u64 down_buttons = HidNpadButton_Down | HidNpadButton_StickLDown;
static const u64 left_buttons = HidNpadButton_Left | HidNpadButton_StickLLeft;
static const u64 right_buttons = HidNpadButton_Right | HidNpadButton_StickLRight;
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

static void centered_text(TTF_Font *font, const char *label, int x, int y, int width, SDL_Color color) {
    int pixels = 0;
    TTF_SizeUTF8(font, label, &pixels, NULL);
    int offset = SDL_max(0, (width - pixels) / 2);
    text(font, label, x + offset, y, width - offset, color);
}

static void right_text(TTF_Font *font, const char *label, int x, int y, int width, SDL_Color color) {
    int pixels = 0;
    TTF_SizeUTF8(font, label, &pixels, NULL);
    int offset = SDL_max(0, width - pixels);
    text(font, label, x + offset, y, width - offset, color);
}

static void icon_circle(int x, int y, int radius, int thickness, SDL_Color color) {
    int inner = radius - thickness;
    SDL_SetRenderDrawColor(app.renderer, color.r, color.g, color.b, color.a);
    for (int dy = -radius; dy <= radius; ++dy) {
        int outer = radius;
        while (outer * outer + dy * dy > radius * radius) --outer;
        int hole = inner;
        while (hole >= 0 && hole * hole + dy * dy >= inner * inner) --hole;
        if (hole < 0) {
            SDL_RenderDrawLine(app.renderer, x - outer, y + dy, x + outer, y + dy);
        } else {
            SDL_RenderDrawLine(app.renderer, x - outer, y + dy, x - hole - 1, y + dy);
            SDL_RenderDrawLine(app.renderer, x + hole + 1, y + dy, x + outer, y + dy);
        }
    }
}

static void draw_header(void) {
    const bool focused = app.screen == LIBRARY && !app.settings_open;
    if (focused && app.top_focus == TOP_SEARCH) rect(SEARCH_X, TOP_Y, 163, TOP_HEIGHT, selected);
    if (focused && app.top_focus == TOP_DOWNLOADS)
        rect(DOWNLOADS_X, TOP_Y, 57, TOP_HEIGHT, selected);
    if ((focused && app.top_focus == TOP_SETTINGS) || app.settings_open)
        rect(SETTINGS_X, TOP_Y, TOP_END - SETTINGS_X, TOP_HEIGHT, selected);

    if (app.screen == LIBRARY && !app.settings_open) {
        icon_circle(SEARCH_X + 19, 42, 8, 2, white);
        for (int i = 0; i < 3; ++i)
            SDL_RenderDrawLine(app.renderer, SEARCH_X + 24 + i, 47, SEARCH_X + 30 + i, 53);
        text(app.normal, "Пошук", SEARCH_X + 44, 32, 105, white);
    } else {
        text(app.normal, "‹ Назад", SEARCH_X + 10, 32, 150, white);
    }
    const int arrow_x = DOWNLOADS_X + 28, arrow_y = 34;
    rect(arrow_x - 4, arrow_y, 9, 16, white);
    for (int i = 0; i < 13; ++i) rect(arrow_x - 12 + i, arrow_y + 14 + i, 25 - 2 * i, 1, white);

    const int gear_x = SETTINGS_X + 31, gear_y = 47;
    static const SDL_Point teeth[] = {{0,-11}, {8,-8}, {11,0}, {8,8},
                                      {0,11}, {-8,8}, {-11,0}, {-8,-8}};
    icon_circle(gear_x, gear_y, 11, 6, white);
    for (size_t i = 0; i < sizeof(teeth) / sizeof(teeth[0]); ++i)
        rect(gear_x + teeth[i].x - 2, gear_y + teeth[i].y - 2, 5, 5, white);
    if (app.update_ready) icon_circle(gear_x + 16, gear_y - 14, 4, 4, blue);
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

static int clamp_grid_scroll(int offset) {
    int rows = (int)((app.game_count + GRID_COLUMNS - 1) / GRID_COLUMNS);
    return SDL_clamp(offset, 0, SDL_max(0, rows - GRID_ROWS) * GRID_STEP_Y);
}

static size_t grid_first(void) {
    return (size_t)(app.grid_scroll / GRID_STEP_Y) * GRID_COLUMNS;
}

static size_t grid_end(void) {
    int bottom = app.grid_scroll + GRID_VIEW_BOTTOM - GRID_VIEW_TOP;
    size_t rows = (size_t)(bottom + GRID_STEP_Y - 1) / GRID_STEP_Y;
    return SDL_min(app.game_count, rows * GRID_COLUMNS);
}

static bool scroll_grid(int delta) {
    int offset = clamp_grid_scroll(app.grid_scroll + delta);
    if (offset == app.grid_scroll) return false;
    app.grid_scroll = offset;
    int first_row = (offset + GRID_STEP_Y / 2) / GRID_STEP_Y;
    int selected_row = (int)(app.game_index / GRID_COLUMNS);
    int row = SDL_clamp(selected_row, first_row, first_row + GRID_ROWS - 1);
    size_t index = (size_t)row * GRID_COLUMNS + app.game_index % GRID_COLUMNS;
    app.game_index = SDL_min(index, app.game_count - 1);
    app.genre_focus = false;
    app.top_focus = TOP_NONE;
    return true;
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
    const char *path = job->url;
    if (job->app_update) {
        const char *error = stage_update(job, path);
        job->update_staged = error == NULL;
        job->result = error ? error : "Оновлення готове. Натисни +, щоб вийти й застосувати.";
        return;
    }
    if (!job->local_unpack && !job->local_install) {
        fichier_download(job);
        if (atomic_load(&job->cancelled) || strcmp(job->result, "Download saved")) return;
        path = job->path;
    }
    bool rar_delete_failed = false, nsp_delete_failed = false;
    if (job->local_unpack || rar_name(path) || rar_signature(path)) {
        UnpackedPackages packages = {0};
        const char *error = unpack_rar(job, path, NXLOAD_DOWNLOAD_DIR,
                                      job->install_after_download ? &packages : NULL);
        if (error) {
            job->result = error;
            return;
        }
        rar_delete_failed = job->delete_rar && unlink(path) != 0;
        job->result = rar_delete_failed ? "RAR розпаковано · не вдалося видалити архів" :
                      job->delete_rar ? "RAR розпаковано · архів видалено" : "RAR розпаковано · архів збережено";
        if (!job->install_after_download) return;
        for (size_t i = 0; i < packages.count; ++i) {
            if (!nsp_name(packages.paths[i])) {
                job->result = "RAR розпаковано. NXLoad встановлює NSP. Інші формати можна відкрити в DBI.";
                return;
            }
        }
        for (size_t i = 0; i < packages.count; ++i) {
            error = install_nsp(job, packages.paths[i]);
            if (error) {
                job->result = error;
                return;
            }
            if (job->delete_nsp && unlink(packages.paths[i])) nsp_delete_failed = true;
        }
    } else if (job->install_after_download || job->local_install) {
        if (!nsp_name(path) && !nsp_signature(path)) {
            job->result = "Файл збережено. NXLoad встановлює NSP. Інші формати можна відкрити в DBI.";
            return;
        }
        const char *error = install_nsp(job, path);
        if (error) {
            job->result = error;
            return;
        }
        nsp_delete_failed = job->delete_nsp && unlink(path) != 0;
    } else {
        return;
    }
    job->result = rar_delete_failed || nsp_delete_failed ?
        "Встановлено на microSD · не всі файли вдалося видалити" :
        job->delete_nsp ? "Встановлено на microSD · NSP видалено" : "Встановлено на microSD · NSP збережено";
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

static const Game *entry_game(const CatalogEntry *entry) {
    return entry->games[CATALOG_NSROMS] ? entry->games[CATALOG_NSROMS] : entry->games[CATALOG_ROMSHQ];
}

static const Game *game_at(size_t row) {
    return entry_game(&app.entries[app.rows[row]]);
}

static const char *entry_title(const CatalogEntry *entry) {
    return entry->metadata ? entry->metadata->title : entry_game(entry)->title;
}

static bool match_query(const CatalogEntry *entry) {
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
        if (strcasestr(entry_title(entry), token)) continue;
        bool found = false;
        for (unsigned source = 0; source < 2; ++source) {
            const Game *game = entry->games[source];
            if (game && (strcasestr(game->title, token) || strcasestr(game->page, token))) found = true;
        }
        if (!found) return false;
    }
    return true;
}

static int compare_rows(const void *left, const void *right) {
    const CatalogEntry *a = &app.entries[*(const size_t *)left];
    const CatalogEntry *b = &app.entries[*(const size_t *)right];
    if (app.sort != SORT_NAME) {
        const CatalogMetadata empty = {0};
        const CatalogMetadata *x = a->metadata ? a->metadata : &empty;
        const CatalogMetadata *y = b->metadata ? b->metadata : &empty;
        unsigned first = app.sort == SORT_RELEASE ? x->released : x->rating;
        unsigned second = app.sort == SORT_RELEASE ? y->released : y->rating;
        if (first != second) return first > second ? -1 : 1;
    }
    int order = strcasecmp(entry_title(a), entry_title(b));
    return order ? order : strcmp(entry_game(a)->page, entry_game(b)->page);
}

static void rebuild_view(const char *keep) {
    size_t old_index = app.game_index;
    int old_scroll = app.grid_scroll;
    app.grid_scroll = 0;
    app.game_count = 0;
    app.genre_count = 0;
    app.game_index = 0;
    for (size_t i = 0; i < app.entry_count; ++i) {
        const CatalogEntry *entry = &app.entries[i];
        if (app.genre && (!entry->metadata || !(entry->metadata->genres & (1u << (app.genre - 1))))) continue;
        ++app.genre_count;
        if (!match_query(entry)) continue;
        app.rows[app.game_count++] = i;
    }
    qsort(app.rows, app.game_count, sizeof(app.rows[0]), compare_rows);
    if (!keep || !app.game_count) return;
    app.game_index = SDL_min(old_index, app.game_count - 1);
    for (size_t i = 0; i < app.game_count; ++i) {
        const CatalogEntry *entry = &app.entries[app.rows[i]];
        const Game *nsroms = entry->games[CATALOG_NSROMS];
        const Game *romshq = entry->games[CATALOG_ROMSHQ];
        if ((nsroms && !strcmp(nsroms->page, keep)) || (romshq && !strcmp(romshq->page, keep))) {
            app.game_index = i;
            break;
        }
    }
    int row_shift = (int)(app.game_index / GRID_COLUMNS) - (int)(old_index / GRID_COLUMNS);
    app.grid_scroll = clamp_grid_scroll(old_scroll + row_shift * GRID_STEP_Y);
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
    if (!app.index_busy) {
        strcpy(app.index_notice[source], "Не вдалося запустити оновлення");
    } else {
        time_t now = time(NULL);
        if (now > 0) {
            app.settings.catalog_checked[source] = now;
            save_settings(&app.settings);
        }
    }
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
    if (job->screenshot) {
        job->bytes = load_gallery_image(job->url, &job->cancel, &job->count);
        return;
    }
    job->bytes = cover_cache_read(job->url, &job->count);
    job->from_disk = job->bytes != NULL;
    if (job->from_disk || atomic_load(&job->cancel)) return;
    if (!job->connection) job->connection = curl_easy_init();
    if (job->connection)
        job->bytes = catalog_fetch_thumbnail(job->connection, job->url, &job->cancel, &job->count);
}

static CachedCover *find_cover(const char *url) {
    if (!url[0]) return NULL;
    for (size_t i = 0; i < COVER_SLOTS; ++i) {
        CachedCover *entry = &app.covers[i];
        if (strcmp(entry->url, url)) continue;
        uint64_t now = SDL_GetTicks64();
        if (entry->texture) entry->last_used = now;
        else if (now - entry->last_used >= 30000) {
            entry->url[0] = 0;
            return NULL;
        }
        return entry;
    }
    return NULL;
}

static void cover_urls(const Game *game, const CatalogMetadata *metadata, const char *urls[3]) {
    urls[0] = metadata ? metadata->eshop_icon : "";
    urls[1] = metadata ? metadata->cover : "";
    urls[2] = game->cover;
}

static bool cover_pending(const char *url) {
    for (size_t i = 0; i < COVER_WORKERS; ++i)
        if (app.cover_jobs[i].busy && !strcmp(app.cover_jobs[i].url, url)) return true;
    return false;
}

static const char *missing_cover(const Game *game, const CatalogMetadata *metadata) {
    const char *urls[3];
    cover_urls(game, metadata, urls);
    for (size_t i = 0; i < 3; ++i) {
        if (!urls[i][0]) continue;
        CachedCover *cover = find_cover(urls[i]);
        if (cover && cover->texture) return NULL;
        if (!cover) return cover_pending(urls[i]) ? NULL : urls[i];
    }
    return NULL;
}

static SDL_Texture *cover_texture(const Game *game, const CatalogMetadata *metadata) {
    const char *urls[3];
    cover_urls(game, metadata, urls);
    for (size_t i = 0; i < 3; ++i) {
        CachedCover *cover = find_cover(urls[i]);
        if (cover && cover->texture) return cover->texture;
    }
    return NULL;
}

static bool uses_cover(const Game *game, const CatalogMetadata *metadata, const char *url) {
    const char *urls[3];
    cover_urls(game, metadata, urls);
    for (size_t i = 0; i < 3; ++i)
        if (!strcmp(url, urls[i])) return true;
    return false;
}

static bool cover_visible(const CoverJob *job) {
    if (app.screen == GAME) {
        if (app.game_tab == GAME_SCREENSHOTS)
            return job->screenshot && app.gallery.count &&
                   !strcmp(job->url, app.gallery.screenshots[app.screenshot_index]);
        return !job->screenshot && uses_cover(&app.active_game, app.active_metadata, job->url);
    }
    if (app.screen != LIBRARY || job->screenshot) return false;
    for (size_t i = grid_first(), end = grid_end(); i < end; ++i) {
        const CatalogEntry *entry = &app.entries[app.rows[i]];
        if (uses_cover(entry_game(entry), entry->metadata, job->url)) return true;
    }
    return false;
}

static void finish_cover(CoverJob *job) {
    threadWaitForExit(&job->thread);
    threadClose(&job->thread);
    job->busy = false;
    if (!atomic_load(&job->cancel)) {
        SDL_Texture *texture = decode_cover(job->bytes, job->count, job->screenshot);
        if (job->from_disk && !texture) {
            cover_cache_discard(job->url);
        } else {
            if (!job->screenshot && !job->from_disk && texture)
                cover_cache_write(job->url, job->bytes, job->count);
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
            entry->texture = texture;
            entry->last_used = SDL_GetTicks64();
        }
    }
    free(job->bytes);
    job->bytes = NULL;
}

static const char *next_cover(bool *screenshot) {
    *screenshot = false;
    if (app.screen == GAME) {
        if (app.game_tab != GAME_SCREENSHOTS)
            return missing_cover(&app.active_game, app.active_metadata);
        if (!app.gallery.count || app.download_busy) return NULL;
        const char *image = app.gallery.screenshots[app.screenshot_index];
        *screenshot = true;
        return !find_cover(image) && !cover_pending(image) ? image : NULL;
    }
    if (app.screen != LIBRARY || !app.game_count) return NULL;
    const CatalogEntry *selected_entry = &app.entries[app.rows[app.game_index]];
    const char *url = missing_cover(entry_game(selected_entry), selected_entry->metadata);
    if (url) return url;
    for (size_t i = grid_first(), end = grid_end(); i < end; ++i) {
        const CatalogEntry *entry = &app.entries[app.rows[i]];
        url = missing_cover(entry_game(entry), entry->metadata);
        if (url) return url;
    }
    return NULL;
}

static bool pump_covers(void) {
    bool finished = false;
    for (size_t i = 0; i < COVER_WORKERS; ++i) {
        CoverJob *job = &app.cover_jobs[i];
        if (!job->busy) continue;
        if (!cover_visible(job)) atomic_store(&job->cancel, true);
        if (R_SUCCEEDED(waitSingle(waiterForThread(&job->thread), 0))) {
            finish_cover(job);
            finished = true;
        }
    }
    if (app.gallery_busy) return finished;
    for (size_t i = 0; i < COVER_WORKERS; ++i) {
        CoverJob *job = &app.cover_jobs[i];
        if (job->busy) continue;
        const char *url = next_cover(&job->screenshot);
        if (!url) break;
        strcpy(job->url, url);
        atomic_init(&job->cancel, false);
        job->from_disk = false;
        job->count = 0;
        job->busy = start_worker(&job->thread, cover_worker, job);
    }
    return finished;
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
        !app.download_busy && !app.catalog_busy) {
        memset(job, 0, sizeof(*job));
        atomic_init(&job->cancel, false);
        strcpy(job->page, app.active_game.page);
        if (app.active_metadata && app.active_metadata->nintendo_life[0]) {
            strcpy(job->nintendo_life, app.active_metadata->nintendo_life);
            strcpy(job->title, app.active_metadata->title);
        } else {
            strcpy(job->title, app.metadata.title);
        }
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
    int bytes = snprintf(path, sizeof(path), NXLOAD_DOWNLOAD_DIR "/%s", name);
    return bytes > 0 && (size_t)bytes < sizeof(path) && rar_signature(path);
}

static bool saved_nsp(const char *name) {
    if (nsp_name(name)) return true;
    const char *ext = strrchr(name, '.');
    if (!ext || strcasecmp(ext, ".bin")) return false;
    char path[320];
    int bytes = snprintf(path, sizeof(path), NXLOAD_DOWNLOAD_DIR "/%s", name);
    return bytes > 0 && (size_t)bytes < sizeof(path) && nsp_signature(path);
}

static void refresh_saved(void) {
    app.saved_count = 0;
    DIR *dir = opendir(NXLOAD_DOWNLOAD_DIR);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) && app.saved_count < HISTORY_LIMIT) {
            size_t n = strlen(entry->d_name);
            const char *ext = strrchr(entry->d_name, '.');
            if (n < sizeof(app.saved[0].file) && (package_name(entry->d_name) || rar_name(entry->d_name) ||
                (ext && !strcasecmp(ext, ".bin") && (saved_rar(entry->d_name) || saved_nsp(entry->d_name))))) {
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

static void schedule_daily_catalogs(time_t now) {
    if (!app.settings.auto_catalog) return;
    for (unsigned source = 0; source < 2; ++source) {
        if (app.index_busy && app.index_job.source == (CatalogSource)source) continue;
        time_t cached = app.catalogs[source].saved_at;
        time_t checked = app.settings.catalog_checked[source];
        if (daily_check_due(now, cached) && daily_check_due(now, checked))
            app.refresh_waiting |= 1u << source;
    }
}

static void open_catalogs(void) {
    for (unsigned source = 0; source < 2; ++source)
        catalog_read_cache(&app.catalogs[source], (CatalogSource)source);
    remake_catalog();
    schedule_daily_catalogs(time(NULL));
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

static void launch_next_transfer(void);

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
            if (job->details.cover_url[0])
                strcpy(app.active_game.cover, job->details.cover_url);
            if (!app.metadata.title[0] &&
                read_game_metadata(&app.metadata, app.active_game.title, app.details.title))
                update_description();
        }
    }
    if (app.download_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.download_thread), 0))) {
        threadWaitForExit(&app.download_thread);
        threadClose(&app.download_thread);
        app.download_busy = false;
        app.download_finished = true;
        if (app.download.update_staged) app.update_ready = true;
        if (app.active_transfer.app_update)
            snprintf(app.update_notice, sizeof(app.update_notice), "%s", app.download.result);
        refresh_saved();
        if (app.queue_count && !app.active_transfer.app_update)
            snprintf(app.notice, sizeof(app.notice), "Попереднє: %.170s", app.download.result);
    }
    if (app.download_busy && SDL_GetTicks64() - app.speed_time >= 500) {
        uint64_t now = SDL_GetTicks64(), bytes = atomic_load(&app.download.received);
        app.speed = bytes >= app.previous_bytes ?
            (bytes - app.previous_bytes) * 1000.0 / (now - app.speed_time) : 0;
        app.previous_bytes = bytes;
        app.speed_time = now;
    }
    if (!app.download_busy && app.queue_count) launch_next_transfer();
}

static void remove_queued(size_t index) {
    --app.queue_count;
    memmove(&app.queue[index], &app.queue[index + 1],
            (app.queue_count - index) * sizeof(app.queue[0]));
    if (app.queue_index && app.queue_index >= app.queue_count) --app.queue_index;
}

static void launch_next_transfer(void) {
    app.active_transfer = app.queue[0];
    remove_queued(0);
    const DownloadRequest *request = &app.active_transfer;
    FichierJob *job = &app.download;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->cancelled, false);
    atomic_init(&job->unpacking, false);
    atomic_init(&job->installing, false);
    atomic_init(&job->received, 0);
    atomic_init(&job->total, 0);
    atomic_init(&job->streams, 0);
    atomic_init(&job->waiting, 0);
    job->url = request->url;
    job->dlarchive = request->source == FILE_DLARCHIVE && !request->unpack;
    job->dlarchive_id = request->id;
    job->romshq = request->source == FILE_ROMSHQ_SIGNED && !request->unpack;
    job->romshq_id = job->romshq && request->id[0] ? request->id : NULL;
    job->datanodes = request->source == FILE_DATANODES && !request->unpack;
    job->local_unpack = request->unpack;
    job->local_install = request->install_local;
    job->install_after_download = request->install_after_download;
    job->app_update = request->app_update;
    job->delete_rar = app.settings.delete_rar;
    job->delete_nsp = app.settings.delete_nsp;
    app.speed_time = SDL_GetTicks64();
    app.previous_bytes = 0;
    app.speed = 0;
    app.download_finished = false;
    app.download_busy = start_worker(&app.download_thread, download_worker, job);
    if (!app.download_busy) {
        job->result = "Не вдалося запустити операцію";
        app.download_finished = true;
    }
}

static bool same_transfer(const DownloadRequest *a, const DownloadRequest *b) {
    return a->source == b->source && a->unpack == b->unpack && a->install_local == b->install_local && a->app_update == b->app_update &&
           !strcmp(a->url, b->url) && !strcmp(a->id, b->id);
}

static const char *enqueue_transfer(const DownloadRequest *request) {
    if (app.download_busy && same_transfer(request, &app.active_transfer))
        return "Цей файл уже обробляється";
    for (size_t i = 0; i < app.queue_count; ++i)
        if (same_transfer(request, &app.queue[i])) return "Цей файл уже в черзі";
    if (app.queue_count == DOWNLOAD_QUEUE_LIMIT) return "Черга заповнена";
    app.queue[app.queue_count++] = *request;
    if (app.download_busy) return "Додано до черги";
    bool app_update = request->app_update;
    launch_next_transfer();
    if (!app_update) {
        app.notice[0] = 0;
        if (app.screen != DOWNLOADS) app.previous = app.screen;
        app.screen = DOWNLOADS;
    }
    return "";
}

static void choose_transfer(const DownloadRequest *request) {
    app.draft_transfer = *request;
    app.download_choice = app.settings.install_default ? 1 : 0;
    app.choose_download = true;
}

static void select_file_action(const GameFile *file) {
    DownloadRequest request = {.source = file->source};
    const char *title = app.active_metadata ? app.active_metadata->title : app.active_game.title;
    snprintf(request.name, sizeof(request.name), "%.120s · %.64s", title, file->title);
    strcpy(request.id, file->id);
    strcpy(request.url, file->source == FILE_ROMSHQ_SIGNED ? app.game_sources[app.file_source].page : file->url);
    choose_transfer(&request);
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

static void start_update(void) {
    if (app.update_ready) return;
    if (!app.update_target[0] || access(app.update_target, R_OK)) {
        strcpy(app.update_notice, "Для оновлення запусти NXLoad.nro з microSD");
        return;
    }
    if (!update_configured()) {
        strcpy(app.update_notice, "Додай токен GitHub у switch/nxload/github-token.txt");
        return;
    }
    DownloadRequest request = {.name = "Оновлення NXLoad", .app_update = true};
    strcpy(request.url, app.update_target);
    const char *message = enqueue_transfer(&request);
    snprintf(app.update_notice, sizeof(app.update_notice), "%s",
             *message ? message : "Перевірка нової збірки…");
}

static void pump_daily_checks(uint64_t ticks) {
    if (ticks < app.background_check_at) return;
    app.background_check_at = ticks + 60000;
    schedule_daily_catalogs(time(NULL));
}

static void download_link(void) {
    DownloadRequest request = {.name = "Файл за посиланням"};
    if (!keyboard(request.url, sizeof(request.url), "Завантаження за посиланням",
                  "Встав URL ROMsHQ /d/ або DataNodes", "Додати"))
        return;
    request.source = !strncmp(request.url, "https://romshq.com/d/", sizeof("https://romshq.com/d/") - 1) ||
                     !strncmp(request.url, "https://www.romshq.com/d/", sizeof("https://www.romshq.com/d/") - 1) ?
                     FILE_ROMSHQ_SIGNED : FILE_DATANODES;
    choose_transfer(&request);
}

static void start_saved(const char *file, bool unpack) {
    DownloadRequest request = {.unpack = unpack, .install_local = !unpack};
    snprintf(request.name, sizeof(request.name), "%.191s", file);
    snprintf(request.url, sizeof(request.url), NXLOAD_DOWNLOAD_DIR "/%s", file);
    strcpy(app.notice, enqueue_transfer(&request));
}

static void delete_saved_file(void) {
    char path[320];
    snprintf(path, sizeof(path), NXLOAD_DOWNLOAD_DIR "/%s", app.delete_file);
    if (app.download_busy && (app.active_transfer.unpack || app.active_transfer.install_local) &&
        !strcmp(path, app.active_transfer.url)) {
        strcpy(app.notice, "Файл зараз обробляється");
    } else if (unlink(path)) {
        strcpy(app.notice, "Не вдалося видалити файл");
    } else {
        strcpy(app.notice, "Файл видалено");
        for (size_t i = app.queue_count; i > 0; --i)
            if ((app.queue[i - 1].unpack || app.queue[i - 1].install_local) &&
                !strcmp(path, app.queue[i - 1].url)) remove_queued(i - 1);
        size_t out = 0;
        for (size_t i = 0; i < app.saved_count; ++i)
            if (strcmp(app.saved[i].file, app.delete_file)) app.saved[out++] = app.saved[i];
        app.saved_count = out;
        if (app.saved_index && app.saved_index >= app.saved_count) --app.saved_index;
    }
    app.delete_file[0] = 0;
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

static size_t current_genre_tab(void) {
    for (size_t i = 0; i < TAB_COUNT; ++i)
        if (genre_tabs[i] == app.genre) return i;
    return TAB_COUNT;
}

static void select_genre(size_t genre) {
    app.genre = genre;
    app.sort = app.genre_sort[genre];
    rebuild_view(NULL);
}

static int tab_left(size_t tab) {
    int left = 40;
    for (size_t i = 0; i < tab; ++i) left += tab_widths[i] + 12;
    return left;
}

static void toggle_settings(void) {
    app.settings_open = !app.settings_open;
    if (app.settings_open) {
        app.settings_row = 0;
        app.settings_notice[0] = 0;
    }
}

static void change_setting(size_t row, bool activate) {
    bool *value = NULL;
    switch (row) {
        case SETTINGS_DEFAULT: value = &app.settings.install_default; break;
        case SETTINGS_RAR: value = &app.settings.delete_rar; break;
        case SETTINGS_NSP: value = &app.settings.delete_nsp; break;
        case SETTINGS_AUTO_CATALOG: value = &app.settings.auto_catalog; break;
        case SETTINGS_REFRESH_CATALOG:
            if (activate) {
                refresh_catalogs();
                app.settings_notice[0] = 0;
            }
            return;
        case SETTINGS_UPDATE:
            if (activate) start_update();
            return;
        default: return;
    }
    *value = !*value;
    if (save_settings(&app.settings)) {
        app.settings_notice[0] = 0;
        app.background_check_at = 0;
    } else {
        *value = !*value;
        strcpy(app.settings_notice, "Не вдалося зберегти налаштування на microSD");
    }
}

static void next_genre_tab(bool right) {
    size_t current = current_genre_tab();
    size_t next = current == TAB_COUNT ? 0 : (current + (right ? 1 : TAB_COUNT - 1)) % TAB_COUNT;
    select_genre(genre_tabs[next]);
}

static void move_in_library(u64 buttons) {
    if (buttons & (HidNpadButton_L | HidNpadButton_R)) {
        next_genre_tab((buttons & HidNpadButton_R) != 0);
        return;
    }
    if (app.top_focus) {
        if ((buttons & left_buttons) && app.top_focus > TOP_SEARCH) --app.top_focus;
        if ((buttons & right_buttons) && app.top_focus < TOP_SETTINGS) ++app.top_focus;
        if (buttons & down_buttons) {
            app.top_focus = TOP_NONE;
            app.genre_focus = true;
        }
        return;
    }
    if (app.genre_focus) {
        if (buttons & (left_buttons | right_buttons)) next_genre_tab((buttons & right_buttons) != 0);
        if (buttons & up_buttons) {
            app.top_focus = current_genre_tab() >= TAB_COUNT - 2 ? TOP_SETTINGS : TOP_SEARCH;
            app.genre_focus = false;
        }
        if (buttons & (down_buttons | HidNpadButton_A)) app.genre_focus = false;
        return;
    }
    size_t index = app.game_index;
    if (buttons & up_buttons) {
        if (index >= GRID_COLUMNS) index -= GRID_COLUMNS;
        else app.genre_focus = true;
    } else if ((buttons & down_buttons) && app.game_count &&
               index / GRID_COLUMNS < (app.game_count - 1) / GRID_COLUMNS)
        index = SDL_min(index + GRID_COLUMNS, app.game_count - 1);
    else if ((buttons & left_buttons) && index % GRID_COLUMNS) --index;
    else if ((buttons & right_buttons) && index % GRID_COLUMNS < GRID_COLUMNS - 1 &&
             index + 1 < app.game_count) ++index;
    if (index != app.game_index) {
        app.game_index = index;
        int row_y = (int)(index / GRID_COLUMNS) * GRID_STEP_Y;
        app.grid_scroll = clamp_grid_scroll(SDL_clamp(app.grid_scroll,
            row_y - (GRID_ROWS - 1) * GRID_STEP_Y, row_y));
    }
}

static void input(u64 buttons);

static void touch_swipe(int x, int y, int dx, int dy) {
    bool horizontal = abs(dx) > abs(dy);
    bool next = (horizontal ? dx : dy) < 0;
    if (app.filters_open || app.delete_file[0] || app.confirm_install || app.choose_download || app.settings_open) return;
    if (app.screen == LIBRARY) {
        if (y >= TAB_Y && y < TAB_Y + TAB_HEIGHT && horizontal) {
            next_genre_tab(next);
        }
    } else if (app.screen == GAME) {
        if (horizontal && (app.game_tab != GAME_SCREENSHOTS || (y >= 201 && y < 245)))
            input(next ? HidNpadButton_R : HidNpadButton_L);
        else if (horizontal)
            input(next ? HidNpadButton_Right : HidNpadButton_Left);
        else if (app.game_tab == GAME_OVERVIEW && !horizontal && app.description) {
            int height = 0;
            SDL_QueryTexture(app.description, NULL, NULL, NULL, &height);
            app.description_scroll = SDL_min(SDL_max(0, app.description_scroll - dy), SDL_max(0, height - 244));
        } else if (app.game_tab == GAME_FILES && app.file_source_chosen && !horizontal) {
            input(next ? HidNpadButton_Down : HidNpadButton_Up);
        }
    } else if (!horizontal) {
        app.queue_focus = x >= 650;
        input(next ? HidNpadButton_Down : HidNpadButton_Up);
    }
}

static void touch_tap(int x, int y) {
    if (app.choose_download) {
        if (x >= 225 && x < 1055 && ((y >= 324 && y < 366) || (y >= 384 && y < 426))) {
            app.download_choice = y >= 384;
            input(HidNpadButton_A);
        } else if (x >= 225 && x < 1055 && y >= 446 && y < 493) {
            input(HidNpadButton_B);
        }
        return;
    }
    if (app.delete_file[0] || app.confirm_install) {
        if (x >= 224 && x < 1050 && y >= 417 && y < 490)
            input(x < 640 ? HidNpadButton_A : HidNpadButton_B);
        return;
    }
    if (app.filters_open) {
        if (x >= 253 && x < 1027 && y >= 270 && y < 369) {
            app.filter_row = y >= 327;
            input(x < 640 ? HidNpadButton_Left : HidNpadButton_Right);
        } else if (x >= 253 && x < 1027 && y >= 430 && y < 502) {
            input(x < 660 ? HidNpadButton_A : HidNpadButton_B);
        }
        return;
    }
    if (app.settings_open) {
        if (y >= TOP_Y && y < TOP_Y + TOP_HEIGHT &&
            ((x >= SEARCH_X && x < DOWNLOADS_X) || (x >= SETTINGS_X && x < TOP_END))) {
            toggle_settings();
        } else if (x >= 180 && x < 1100 && y >= SETTINGS_Y) {
            size_t row = (size_t)(y - SETTINGS_Y) / SETTINGS_STEP;
            if (row < SETTINGS_ROWS && (y - SETTINGS_Y) % SETTINGS_STEP < 45) {
                app.settings_row = row;
                change_setting(row, true);
            }
        }
        return;
    }
    if (y >= TOP_Y && y < TOP_Y + TOP_HEIGHT) {
        if (x >= SETTINGS_X && x < TOP_END) toggle_settings();
        else if (x >= DOWNLOADS_X && x < SETTINGS_X) input(HidNpadButton_Y);
        else if (x >= SEARCH_X && x < DOWNLOADS_X)
            input(app.screen == LIBRARY ? HidNpadButton_X : HidNpadButton_B);
        return;
    }
    if (app.screen == LIBRARY) {
        if (x >= 40 && y >= TAB_Y && y < TAB_Y + TAB_HEIGHT) {
            for (size_t i = 0; i < TAB_COUNT; ++i) {
                int left = tab_left(i);
                if (x < left || x >= left + tab_widths[i]) continue;
                app.genre_focus = false;
                app.top_focus = TOP_NONE;
                select_genre(genre_tabs[i]);
                break;
            }
        } else if (x >= 1000 && y >= 607 && y < 644) {
            input(HidNpadButton_ZL);
        } else if (x >= GRID_X && y >= GRID_VIEW_TOP && y < GRID_VIEW_BOTTOM) {
            int grid_x = x - GRID_X, grid_y = y - GRID_Y + app.grid_scroll;
            if (grid_y < 0) return;
            int col = grid_x / GRID_STEP_X, row = grid_y / GRID_STEP_Y;
            size_t index = (size_t)row * GRID_COLUMNS + (size_t)col;
            if (col < GRID_COLUMNS && index < app.game_count &&
                grid_x % GRID_STEP_X < GRID_WIDTH && grid_y % GRID_STEP_Y < GRID_HEIGHT) {
                app.game_index = index;
                app.genre_focus = false;
                app.top_focus = TOP_NONE;
                input(HidNpadButton_A);
            }
        }
    } else if (app.screen == GAME) {
        if (x >= 430 && x < 1190 && y >= 201 && y < 245) {
            GameTab tab = (GameTab)((x - 430) / 252);
            if (tab <= GAME_FILES && tab != app.game_tab) {
                if (tab == GAME_FILES) enter_game_files();
                else app.game_tab = tab;
            }
        } else if (app.game_tab == GAME_FILES && !app.file_source_chosen &&
                   x >= 430 && y >= 322 && y < 488) {
            open_file_source(x < 825 ? CATALOG_NSROMS : CATALOG_ROMSHQ);
        } else if (app.game_tab == GAME_FILES && app.file_source_chosen &&
                   x >= 430 && !app.catalog_busy && !app.game_error[0] &&
                   y >= 267 && y < 267 + FILE_ROWS * 48) {
            size_t file = first_visible(app.file_index, FILE_ROWS) + (size_t)(y - 267) / 48;
            if (file < app.details.count) {
                app.file_index = file;
                input(HidNpadButton_A);
            }
        }
    } else if (y >= 332 && y < 332 + FILE_ROWS * 46) {
        bool queue = x >= 650;
        size_t current = queue ? app.queue_index : app.saved_index;
        size_t chosen = first_visible(current, FILE_ROWS) + (size_t)(y - 332) / 46;
        bool repeated = queue == app.queue_focus && current == chosen;
        app.queue_focus = queue;
        if (queue && chosen < app.queue_count) app.queue_index = chosen;
        if (!queue && chosen < app.saved_count) {
            app.saved_index = chosen;
            if (repeated) input(HidNpadButton_A);
        }
    } else if (y >= 606 && y < 645) {
        if (x >= 1000) input(HidNpadButton_ZR);
        else if (x >= 800) input(HidNpadButton_X);
        else if (x >= 650 && app.download_busy) input(HidNpadButton_ZL);
    }
}

static bool poll_touch(bool controller_input) {
    static int start_x, start_y, x, y;
    static bool touching, cancelled, on_grid, dragged;
    HidTouchScreenState state;
    if (!hidGetTouchScreenStates(&state, 1)) return false;
    if (state.count > 1 || (controller_input && state.count)) cancelled = true;
    if (state.count == 1 && !cancelled) {
        int previous_y = y;
        x = (int)state.touches[0].x;
        y = (int)state.touches[0].y;
        if (!touching) {
            start_x = x;
            start_y = y;
            touching = true;
            on_grid = app.screen == LIBRARY && app.game_count && !app.filters_open &&
                      !app.settings_open && !app.delete_file[0] && !app.confirm_install &&
                      !app.choose_download && y >= GRID_VIEW_TOP && y < GRID_VIEW_BOTTOM;
            return false;
        }
        if (on_grid && (dragged || abs(x - start_x) >= GRID_DRAG_THRESHOLD ||
                        abs(y - start_y) >= GRID_DRAG_THRESHOLD)) {
            int delta = (dragged ? previous_y : start_y) - y;
            dragged = true;
            return scroll_grid(delta);
        }
    } else if (!state.count) {
        bool activate = touching && !cancelled && !dragged && !controller_input;
        touching = cancelled = on_grid = dragged = false;
        if (activate) {
            int dx = x - start_x, dy = y - start_y;
            if (abs(dx) >= 42 || abs(dy) >= 42) touch_swipe(start_x, start_y, dx, dy);
            else touch_tap(x, y);
            return true;
        }
    }
    return false;
}

static void input(u64 buttons) {
    if (!buttons) return;
    if (app.choose_download) {
        app.download_choice = move(app.download_choice, 2, buttons);
        if (buttons & HidNpadButton_B) app.choose_download = false;
        else if (buttons & HidNpadButton_A) {
            app.choose_download = false;
            app.draft_transfer.install_after_download = app.download_choice == 1;
            strcpy(app.notice, enqueue_transfer(&app.draft_transfer));
        }
        return;
    }
    if (app.delete_file[0]) {
        if (buttons & HidNpadButton_A) delete_saved_file();
        if (buttons & HidNpadButton_B) app.delete_file[0] = 0;
        return;
    }
    if (app.confirm_install) {
        if (buttons & HidNpadButton_A) open_installer();
        if (buttons & HidNpadButton_B) app.confirm_install = false;
        return;
    }
    if (buttons & HidNpadButton_Plus) { app.close = true; return; }
    if (app.settings_open) {
        if (buttons & (HidNpadButton_B | HidNpadButton_Minus)) toggle_settings();
        else {
            app.settings_row = move(app.settings_row, SETTINGS_ROWS, buttons);
            if (buttons & (HidNpadButton_A | left_buttons | right_buttons))
                change_setting(app.settings_row, (buttons & HidNpadButton_A) != 0);
        }
        return;
    }
    if (!app.filters_open && (buttons & HidNpadButton_Minus)) { toggle_settings(); return; }
    if (app.filters_open) {
        app.filter_row = move(app.filter_row, 2, buttons);
        if (buttons & (left_buttons | right_buttons)) {
            bool next = (buttons & right_buttons) != 0;
            if (!app.filter_row) {
                app.draft_sort = (app.draft_sort + (next ? 1 : SORT_COUNT - 1)) % SORT_COUNT;
                app.filter_sort_changed = true;
            } else {
                app.draft_genre = (app.draft_genre + (next ? 1 : GENRE_COUNT - 1)) % GENRE_COUNT;
                if (!app.filter_sort_changed) app.draft_sort = app.genre_sort[app.draft_genre];
            }
        }
        if (buttons & HidNpadButton_A) {
            char keep[CATALOG_URL] = "";
            if (app.game_count && app.genre == app.draft_genre)
                strcpy(keep, game_at(app.game_index)->page);
            app.sort = app.draft_sort;
            app.genre = app.draft_genre;
            app.genre_sort[app.genre] = app.sort;
            rebuild_view(keep[0] ? keep : NULL);
            app.filters_open = false;
        } else if (buttons & (HidNpadButton_B | HidNpadButton_ZL)) {
            app.filters_open = false;
        }
        return;
    }
    if (buttons & HidNpadButton_Y) {
        app.notice[0] = 0;
        if (app.screen == DOWNLOADS) app.screen = app.previous;
        else {
            if (!app.download_busy) refresh_saved();
            app.previous = app.screen;
            app.screen = DOWNLOADS;
        }
        return;
    }
    if (app.screen == DOWNLOADS) {
        if (buttons & (left_buttons | HidNpadButton_L)) app.queue_focus = false;
        if (buttons & (right_buttons | HidNpadButton_R)) app.queue_focus = true;
        if (app.queue_focus) app.queue_index = move(app.queue_index, app.queue_count, buttons);
        else app.saved_index = move(app.saved_index, app.saved_count, buttons);
        if (buttons & HidNpadButton_B) { app.screen = app.previous; return; }
        if ((buttons & HidNpadButton_ZL) && app.download_busy)
            atomic_store(&app.download.cancelled, true);
        if (buttons & HidNpadButton_ZR) { download_link(); return; }
        if (buttons & HidNpadButton_X) {
            if (app.queue_focus && app.queue_count) remove_queued(app.queue_index);
            else if (!app.queue_focus && app.saved_count)
                strcpy(app.delete_file, app.saved[app.saved_index].file);
            return;
        }
        if ((buttons & HidNpadButton_A) && !app.queue_focus && app.saved_count) {
            const char *selected_file = app.saved[app.saved_index].file;
            if (saved_rar(selected_file)) start_saved(selected_file, true);
            else if (saved_nsp(selected_file)) start_saved(selected_file, false);
            else if (!app.download_busy && !app.queue_count) app.confirm_install = true;
            else strcpy(app.notice, "DBI можна відкрити після завершення черги");
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
                if ((buttons & HidNpadButton_A) && !app.catalog_busy && app.details.count && !app.game_error[0])
                    select_file_action(&app.details.files[app.file_index]);
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
    if (buttons & HidNpadButton_ZL) {
        app.draft_sort = app.sort;
        app.draft_genre = app.genre;
        app.filter_row = 0;
        app.filter_sort_changed = false;
        app.filters_open = true;
        return;
    }
    if (app.top_focus && (buttons & HidNpadButton_A)) {
        if (app.top_focus == TOP_SETTINGS) { toggle_settings(); return; }
        if (app.top_focus == TOP_DOWNLOADS) { input(HidNpadButton_Y); return; }
        buttons = HidNpadButton_X;
    }
    bool was_genre_focus = app.genre_focus;
    move_in_library(buttons);
    if ((buttons & HidNpadButton_X) &&
               keyboard(app.query, sizeof(app.query), "Пошук ігор", "Введи назву гри", "Пошук")) {
        rebuild_view(NULL);
    } else if ((buttons & HidNpadButton_B) && app.query[0]) {
        app.query[0] = 0;
        rebuild_view(NULL);
    } else if ((buttons & HidNpadButton_A) && !was_genre_focus && !app.genre_focus) {
        if (app.game_count && !app.catalog_busy) {
            const CatalogEntry *entry = &app.entries[app.rows[app.game_index]];
            app.active_game = *game_at(app.game_index);
            app.active_metadata = entry->metadata;
            memset(app.game_sources, 0, sizeof(app.game_sources));
            for (unsigned source = 0; source < 2; ++source)
                if (entry->games[source]) app.game_sources[source] = *entry->games[source];
            app.show_original = false;
            memset(&app.details, 0, sizeof(app.details));
            memset(&app.metadata, 0, sizeof(app.metadata));
            memset(&app.gallery, 0, sizeof(app.gallery));
            read_game_metadata(&app.metadata, app.active_game.title, entry_title(entry));
            app.romshq_candidate = false;
            if (!app.game_sources[CATALOG_ROMSHQ].page[0] &&
                (app.active_metadata || app.metadata.title[0]))
                app.romshq_candidate =
                    guess_romshq_page(&app.game_sources[CATALOG_ROMSHQ], app.metadata.title[0] ?
                                      app.metadata.title : entry_title(entry));
            app.gallery_requested = false;
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

static void draw_grid_cover(SDL_Texture *cover, int x, int y, int size) {
    int w, h;
    if (!cover || SDL_QueryTexture(cover, NULL, NULL, &w, &h) || w <= 0 || h <= 0) {
        rect(x, y, size, size, panel);
        return;
    }
    int edge = SDL_min(w, h);
    SDL_Rect src = {(w - edge) / 2, (h - edge) / 2, edge, edge};
    SDL_Rect dst = {x, y, size, size};
    SDL_RenderCopy(app.renderer, cover, &src, &dst);
}

static void draw_library(void) {
    for (size_t i = 0; i < TAB_COUNT; ++i) {
        int x = tab_left(i), width = tab_widths[i];
        bool active = app.genre == genre_tabs[i];
        if (active) {
            rect(x, TAB_Y, width, TAB_HEIGHT, selected);
            rect(x, TAB_Y + TAB_HEIGHT - 3, width, 3, blue);
            if (app.genre_focus) rect(x, TAB_Y, 3, TAB_HEIGHT, blue);
        }
        centered_text(app.normal, i ? genre_names[genre_tabs[i]] : "Усі",
                      x + 8, TAB_Y + 7, width - 16, active ? white : gray);
    }
    const SDL_Rect viewport = {0, GRID_VIEW_TOP, 1280, GRID_VIEW_BOTTOM - GRID_VIEW_TOP};
    SDL_RenderSetClipRect(app.renderer, &viewport);
    for (size_t i = grid_first(), end = grid_end(); i < end; ++i) {
        const CatalogEntry *entry = &app.entries[app.rows[i]];
        bool active = i == app.game_index && !app.genre_focus && !app.top_focus;
        int size = active ? GRID_WIDTH : GRID_THUMB_SIZE;
        int inset = (GRID_WIDTH - size) / 2;
        int x = GRID_X + (int)(i % GRID_COLUMNS) * GRID_STEP_X + inset;
        int y = GRID_Y + (int)(i / GRID_COLUMNS) * GRID_STEP_Y - app.grid_scroll + inset;
        draw_grid_cover(cover_texture(entry_game(entry), entry->metadata), x, y, size);
        unsigned rating = entry->metadata ? entry->metadata->rating : 0;
        char score[12] = "—";
        if (rating) snprintf(score, sizeof(score), "%u.%u", rating / 10, rating % 10);
        int badge_x = x + size - 48, badge_y = y + size - 48;
        const SDL_Color badge = {17, 24, 38, 230};
        SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
        rect(badge_x, badge_y, 42, 42, badge);
        SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
        centered_text(app.small, score, badge_x, badge_y + 11, 42, rating ? white : gray);
        if (active) {
            SDL_SetRenderDrawColor(app.renderer, blue.r, blue.g, blue.b, blue.a);
            for (int i = 2; i <= 5; ++i) {
                SDL_Rect outline = {x - i, y - i, size + 2 * i, size + 2 * i};
                SDL_RenderDrawRect(app.renderer, &outline);
            }
        }
    }
    SDL_RenderSetClipRect(app.renderer, NULL);
    if (app.game_count) {
        const CatalogEntry *entry = &app.entries[app.rows[app.game_index]];
        text(app.normal, entry_title(entry), 40, 593, 905, white);
    } else if (!app.index_busy) {
        text(app.normal, app.entry_count ? "Ігор за цими фільтрами не знайдено" : "Каталог порожній",
             48, 176, 790, gray);
    }
    char count[192];
    snprintf(count, sizeof(count), "Всього ігор: %zu", app.entry_count);
    text(app.small, count, 40, 631, 300, gray);
    if (app.genre) {
        snprintf(count, sizeof(count), "%s: %zu", genre_names[app.genre], app.genre_count);
        text(app.small, count, 340, 631, 340, gray);
    }
    if (app.query[0]) {
        snprintf(count, sizeof(count), "Знайдено: %zu", app.game_count);
        text(app.small, count, app.genre ? 700 : 340, 631, 230, gray);
    }
    right_text(app.small, sort_names[app.sort], 1000, 621, 240, blue);
}

static void draw_settings(void) {
    static const char *const labels[] = {
        "Дія з файлом за замовчуванням", "Видаляти RAR після розпакування",
        "Видаляти NSP після встановлення", "Щоденне оновлення каталогів",
        "Оновити каталоги зараз", "Перевірити оновлення NXLoad",
    };
    const char *const on = "Увімкнено", *const off = "Вимкнено";
    const char *const values[] = {
        app.settings.install_default ? "Завантажити й встановити" : "Завантажити",
        app.settings.delete_rar ? on : off, app.settings.delete_nsp ? on : off,
        app.settings.auto_catalog ? on : off, "", "",
    };
    text(app.large, "Налаштування", 195, 89, 920, white);
    for (size_t i = 0; i < SETTINGS_ROWS; ++i) {
        int y = SETTINGS_Y + (int)i * SETTINGS_STEP;
        rect(180, y, 920, 45, i == app.settings_row ? selected : panel);
        text(app.normal, labels[i], 195, y + 9, 560, white);
        if (*values[i])
            right_text(app.small, values[i], 755, y + 12, 330, i == app.settings_row ? blue : gray);
    }
    if (app.index_busy || app.refresh_waiting) {
        char status[128];
        unsigned page = app.index_busy ? atomic_load(&app.index_job.page) : 0;
        unsigned pages = app.index_busy ? atomic_load(&app.index_job.pages) : 0;
        if (app.download_busy) strcpy(status, "Оновлення каталогів чекає завершення завантаження");
        else if (pages) snprintf(status, sizeof(status), "Оновлення каталогів · %u / %u", page, pages);
        else strcpy(status, "Оновлення каталогів…");
        text(app.small, status, 195, 566, 895, gray);
    } else if (app.index_notice[0][0] || app.index_notice[1][0]) {
        char status[384];
        snprintf(status, sizeof(status), "%s%s%s", app.index_notice[0],
                 app.index_notice[0][0] && app.index_notice[1][0] ? "  ·  " : "", app.index_notice[1]);
        text(app.small, status, 195, 566, 895, gray);
    }
    const char *update = app.update_ready ? "NXLoad оновиться під час виходу" :
        app.download_busy && app.active_transfer.app_update ? "Перевірка або завантаження NXLoad…" :
        app.update_notice;
    text(app.small, update, 195, 592, 895, blue);
    text(app.small, app.settings_notice, 195, 618, 895, blue);
    text(app.small, "A  Обрати    B  Назад    +  Вихід", 40, 665, 1200, gray);
}

static void draw_filters(void) {
    rect(226, 165, 828, 360, panel);
    rect(226, 165, 828, 3, blue);
    text(app.large, "Сортування та жанри", 253, 190, 780, white);
    char line[192];
    snprintf(line, sizeof(line), "Порядок     ‹  %s  ›", sort_names[app.draft_sort]);
    row(0, app.filter_row, 253, 270, 774, line);
    snprintf(line, sizeof(line), "Жанр        ‹  %s  ›", genre_names[app.draft_genre]);
    row(1, app.filter_row, 253, 327, 774, line);
    text(app.small, "← / →  Змінити", 263, 417, 747, gray);
    text(app.small, "A  Застосувати", 263, 458, 386, blue);
    text(app.small, "B  Назад", 675, 458, 352, gray);
}

static void draw_cover(void) {
    rect(56, 127, 308, 308, background);
    SDL_Texture *cover = cover_texture(&app.active_game, app.active_metadata);
    fit_cover(cover, 56, 127, 308, 308);
    text(app.normal, "Nintendo Switch", 65, 459, 293, white);
    text(app.small, app.metadata.publisher, 65, 498, 293, gray);
    if (app.metadata.released[0]) {
        char release[48];
        snprintf(release, sizeof(release), "Вихід · %s", app.metadata.released);
        text(app.small, release, 65, 529, 293, gray);
    }
    if (app.active_metadata && app.active_metadata->rating) {
        char score[48];
        unsigned rating = app.active_metadata->rating;
        snprintf(score, sizeof(score), "%u.%u / 10", rating / 10, rating % 10);
        text(app.small, score, 65, 561, 293, blue);
    }
    if (app.active_metadata && app.active_metadata->nintendo_life[0])
        text(app.small, "Nintendo Life", 65, 596, 293, blue);
    else if (app.metadata.title[0])
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
    const char *title = app.active_metadata ? app.active_metadata->title :
                        app.metadata.title[0] ? app.metadata.title :
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
        text(app.normal, app.active_transfer.name, 66, 182, 825, white);
        if (streams > 1) {
            char connections[48];
            snprintf(connections, sizeof(connections), "HTTP Range · %u з’єднань", streams);
            text(app.small, connections, 920, 183, 290, blue);
        }
        rect(66, 225, 1148, 8, selected);
        rect(66, 225, (int)(1148 * percent / 100.0), 8, blue);
        if (app.download_busy && atomic_load(&app.download.installing)) {
            if (total)
                snprintf(line, sizeof(line), "Встановлення на microSD · %.1f%% · %.1f / %.1f МіБ",
                         percent, have / 1048576.0, total / 1048576.0);
            else strcpy(line, "Перевірка пакета NSP…");
        } else if (app.download_busy && atomic_load(&app.download.unpacking)) {
            snprintf(line, sizeof(line), "Розпакування RAR · %.1f / %.1f МіБ", have / 1048576.0, total / 1048576.0);
        } else if (app.download_busy && total) {
            snprintf(line, sizeof(line), "%.1f%%  ·  %.1f / %.1f МіБ  ·  %.1f МіБ/с",
                     percent, have / 1048576.0, total / 1048576.0, app.speed / 1048576.0);
        } else if (app.download_busy && have) {
            snprintf(line, sizeof(line), "%.1f МіБ  ·  %.1f МіБ/с", have / 1048576.0, app.speed / 1048576.0);
        } else if (app.download_busy) {
            strcpy(line, app.active_transfer.app_update ? "Перевірка збірки GitHub Actions…" : "Запит адреси файлу…");
        } else if (!strcmp(app.download.result, "Download saved")) {
            snprintf(line, sizeof(line), "Збережено · %.1f МіБ · середня %.1f МіБ/с",
                     have / 1048576.0, app.download.avg_speed / 1048576.0);
        } else if (atomic_load(&app.download.installing) || atomic_load(&app.download.unpacking) ||
                   app.active_transfer.install_local || app.active_transfer.install_after_download || app.active_transfer.app_update) {
            snprintf(line, sizeof(line), "%s", app.download.result);
        } else {
            snprintf(line, sizeof(line), "%s  ·  API HTTP %ld  ·  Файл HTTP %ld",
                     app.download.result, app.download.api_http, app.download.http);
        }
        text(app.small, line, 66, 249, 1148, blue);
        if (app.download_finished)
            text(app.small, app.download.tls_detail, 66, 270, 1148, gray);
    }
    rect(650, 298, 1, 288, selected);
    text(app.normal, "Збережені файли", 66, 293, 560, app.queue_focus ? gray : white);
    char queue_title[80];
    snprintf(queue_title, sizeof(queue_title), "У черзі · %zu", app.queue_count);
    text(app.normal, queue_title, 677, 293, 537, app.queue_focus ? white : gray);
    size_t first = first_visible(app.saved_index, FILE_ROWS);
    for (size_t i = first; i < app.saved_count && i < first + FILE_ROWS; ++i)
        row(i, app.queue_focus ? SIZE_MAX : app.saved_index, 66,
            332 + (int)(i - first) * 46, 558, app.saved[i].file);
    first = first_visible(app.queue_index, FILE_ROWS);
    for (size_t i = first; i < app.queue_count && i < first + FILE_ROWS; ++i) {
        if (app.queue[i].install_after_download) {
            char label[CATALOG_TEXT];
            snprintf(label, sizeof(label), "↓ + Встановити · %.145s", app.queue[i].name);
            row(i, app.queue_focus ? app.queue_index : SIZE_MAX, 677,
                332 + (int)(i - first) * 46, 537, label);
        } else {
            row(i, app.queue_focus ? app.queue_index : SIZE_MAX, 677,
                332 + (int)(i - first) * 46, 537, app.queue[i].name);
        }
    }
    if (!app.saved_count) text(app.small, "Поки немає збережених файлів", 66, 342, 550, gray);
    if (!app.queue_count) text(app.small, "Черга порожня", 677, 342, 535, gray);
    text(app.small, app.notice, 66, 608, 570, gray);
    if (app.download_busy) text(app.small, "ZL  Скасувати", 650, 608, 145, blue);
    text(app.small, app.queue_focus ? "X  Прибрати з черги" : "X  Видалити файл", 805, 608, 190, blue);
    text(app.small, "ZR  Додати URL", 1005, 608, 205, blue);
}

static void draw(void) {
    rect(0, 0, 1280, 720, background);
    text(app.large, "NXLoad", 40, 24, 320, white);
    text(app.small, NXLOAD_VERSION, 206, 44, 120, gray);
    if (app.download_busy && !app.active_transfer.app_update)
        text(app.small, atomic_load(&app.download.installing) ? "Встановлення…" :
             atomic_load(&app.download.unpacking) ? "Розпакування…" : "Завантаження…", 685, 40, 190, blue);
    draw_header();
    if (app.settings_open) {
        draw_settings();
        SDL_RenderPresent(app.renderer);
        return;
    }
    if (app.screen == LIBRARY) draw_library();
    else if (app.screen == GAME) draw_game();
    else draw_downloads();
    const char *footer = app.screen == LIBRARY ?
        "A  Відкрити    X  Пошук    L / R  Жанри    ZL  Сортування    Y  Завантаження    −  Налаштування" :
        app.screen == GAME ? (app.game_tab == GAME_FILES ?
        (app.file_source_chosen ?
        "A  Дія з файлом    B  Джерела    L / R  Вкладки    Y  Завантаження    +  Вихід" :
        "← / →  Джерело    A  Обрати    B  Огляд    L / R  Вкладки    +  Вихід") :
        app.game_tab == GAME_OVERVIEW && app.metadata.original[0] ?
        "A  До файлів    X  Мова опису    L / R  Вкладки    B  Каталог    Y  Завантаження    +  Вихід" :
        "A  До файлів    L / R  Вкладки    B  Каталог    Y  Завантаження    +  Вихід") :
        "← / →  Файли / Черга    A  Встановити / розпакувати    B  Назад    +  Вихід";
    text(app.small, footer, 40, 665, 1200, gray);
    if (app.filters_open) draw_filters();
    if (app.choose_download) {
        rect(190, 200, 900, 305, panel);
        text(app.large, "Дія з файлом", 224, 230, 828, white);
        text(app.small, app.draft_transfer.name, 224, 284, 828, gray);
        row(0, app.download_choice, 225, 324, 828, "Завантажити");
        row(1, app.download_choice, 225, 384, 828, "Завантажити й встановити");
        text(app.small, "A  Обрати     B  Назад", 224, 454, 828, gray);
    } else if (app.confirm_install) {
        rect(190, 200, 900, 305, selected);
        text(app.large, "Відкрити встановлювач DBI?", 224, 230, 828, white);
        text(app.normal, app.saved[app.saved_index].file, 224, 298, 828, white);
        text(app.small, "У DBI відкрий Browse SD Card → " NXLOAD_DOWNLOAD_RELATIVE, 224, 355, 828, gray);
        text(app.normal, "A  Відкрити DBI", 224, 428, 396, blue);
        text(app.normal, "B  Назад", 656, 428, 390, gray);
    } else if (app.delete_file[0]) {
        rect(190, 200, 900, 305, selected);
        text(app.large, "Видалити файл?", 224, 230, 828, white);
        text(app.normal, app.delete_file, 224, 298, 828, white);
        text(app.normal, "A  Видалити", 224, 428, 396, blue);
        text(app.normal, "B  Скасувати", 656, 428, 390, gray);
    }
    SDL_RenderPresent(app.renderer);
}

static TTF_Font *font_open(const PlFontData *font, int points) {
    SDL_RWops *memory = SDL_RWFromConstMem(font->address, font->size);
    return memory ? TTF_OpenFontRW(memory, 1, points) : NULL;
}

void nx_tls_threading_init(void);
void nx_tls_threading_exit(void);

int main(int argc, char **argv) {
    const char *self = argc > 0 && argv && argv[0] ? argv[0] : "";
    size_t self_len = strlen(self);
    if (self_len < sizeof(app.update_target) && self_len >= 10 &&
        !strncmp(self, "sdmc:/", 6) && !strstr(self, "/../") &&
        !strcasecmp(self + self_len - 4, ".nro"))
        strcpy(app.update_target, self);
    else
        strcpy(app.update_target, NXLOAD_APP_DIR "/NXLoad.nro");
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
    hidInitializeTouchScreen();
    if (ok) {
        SDL_RenderSetLogicalSize(app.renderer, 1280, 720);
        app.settings = load_settings();
        load_catalog_metadata();
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
            pump_daily_checks(now);
            if (held != vertical || (buttons & (up_buttons | down_buttons))) {
                next_scroll = now + 280;
            } else if (vertical && now >= next_scroll) {
                buttons |= vertical;
                next_scroll = now + 110;
            }
            held = vertical;
            input(buttons);
            bool touch_changed = poll_touch(buttons != 0);
            pump_index();
            pump_gallery();
            bool cover_finished = pump_covers();
            if (buttons || touch_changed || cover_finished || now >= next_frame) {
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
    }
    if (app.download_busy) {
        atomic_store(&app.download.cancelled, true);
        threadWaitForExit(&app.download_thread);
        threadClose(&app.download_thread);
    }
    for (size_t i = 0; i < COVER_WORKERS; ++i)
        if (app.cover_jobs[i].busy) atomic_store(&app.cover_jobs[i].cancel, true);
    for (size_t i = 0; i < COVER_WORKERS; ++i) {
        CoverJob *job = &app.cover_jobs[i];
        if (job->busy) finish_cover(job);
        if (job->connection) curl_easy_cleanup(job->connection);
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
    free_catalog_metadata();
    if (romfs_ready) romfsExit();
    if (app.update_ready) {
        if (apply_update(app.update_target)) {
            if (envHasNextLoad()) envSetNextLoad(app.update_target, app.update_target);
        } else {
            consoleInit(NULL);
            printf("Could not apply NXLoad update. The previous NRO or its .bak and the new .new file are on the SD card.\nPress + to exit.\n");
            while (appletMainLoop()) {
                padUpdate(&pad);
                if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
                consoleUpdate(NULL);
            }
            consoleExit(NULL);
        }
    }
    return 0;
}
