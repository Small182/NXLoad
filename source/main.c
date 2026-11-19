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
#include <pthread.h>
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
#include "language.h"
#include "unpack.h"
#include "updater.h"
#include "trailers.h"

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
#define FILE_LIST_Y 259
#define FILE_LIST_BOTTOM 592
#define FILE_STEP 48
#define FAVORITES_PATH NXLOAD_APP_DIR "/favorites.txt"
#define FAVORITES_TAB GENRE_COUNT
#define HISTORY_LIMIT 96
#define COVER_SLOTS 72
#define COVER_LOOKAHEAD (2 * GRID_ROWS * GRID_COLUMNS)
#define TEXT_SLOTS 128
#define COVER_WORKERS 3
#define GRID_THUMB_SIZE 180
#define DOWNLOAD_QUEUE_LIMIT 24

typedef enum { LIBRARY, GAME, DOWNLOADS } Screen;
typedef enum { BACKLIGHT_UNCHANGED, BACKLIGHT_APPLET, BACKLIGHT_LBL } BacklightControl;
typedef enum { SLEEP_UNCHANGED, SLEEP_APPLET, SLEEP_MEDIA } SleepControl;
typedef enum { TOP_NONE, TOP_SEARCH, TOP_DOWNLOADS, TOP_SETTINGS } TopFocus;
typedef enum {
    SETTINGS_DEFAULT, SETTINGS_RAR, SETTINGS_NSP, SETTINGS_AUTO_CATALOG,
    SETTINGS_NSROMS, SETTINGS_ROMSHQ, SETTINGS_ROMSNS, SETTINGS_THEME, SETTINGS_LANGUAGE,
    SETTINGS_REFRESH_CATALOG, SETTINGS_UPDATE, SETTINGS_ROWS
} SettingRow;

#define TAB_Y 96
#define TAB_HEIGHT 41
#define SETTINGS_Y 152
#define SETTINGS_STEP 41
#define SETTINGS_VISIBLE 10
#define TOP_Y 21
#define TOP_HEIGHT 53
#define DOWNLOADS_X 1108
#define HEADER_ACTION_X 976
#define HEADER_ACTION_WIDTH (DOWNLOADS_X - HEADER_ACTION_X - 8)
#define COMPACT_BACK_X 1055
#define COMPACT_BACK_WIDTH 112
#define SETTINGS_X 1175
#define TOP_END 1240
#define DOWNLOADS_DIM_X 1000
typedef enum { GAME_OVERVIEW, GAME_MEDIA, GAME_FILES } GameTab;
typedef enum { SORT_RATING, SORT_NAME, SORT_RELEASE, SORT_COUNT } SortMode;

static const char *const sort_names[] = {"За рейтингом", "За алфавітом", "За датою виходу"};
static const char *const genre_names[] = {
    "Усі жанри", "Екшн", "Пригоди", "Аркада", "Настільні", "Освітні", "Файтинг",
    "Шутер від першої особи", "Музика", "Для вечірок", "Платформер", "Головоломки",
    "Гонки", "RPG", "Шутер", "Симуляція", "Спорт", "Стратегії", "Інше",
};
#define GENRE_COUNT (sizeof(genre_names) / sizeof(genre_names[0]))
static const size_t genre_tabs[] = {0, 1, 2, 13, 11, 12, 17, FAVORITES_TAB};
static const int tab_widths[] = {96, 122, 137, 104, 178, 125, 145, 169};
#define TAB_COUNT (sizeof(genre_tabs) / sizeof(genre_tabs[0]))

typedef struct {
    bool heading;
    const GameFile *file;
    const char *label;
} FileRow;

typedef struct {
    char file[256];
} SavedFile;

typedef struct {
    char name[CATALOG_TEXT], url[2048], id[DLARCHIVE_ID_CAPACITY];
    GameFileSource source;
    bool unpack, install_local, install_after_download, app_update;
    UpdateBuild update;
} DownloadRequest;

typedef struct {
    char url[CATALOG_URL];
    SDL_Texture *texture;
    uint64_t last_used;
} CachedCover;

typedef struct {
    char url[CATALOG_URL];
    atomic_bool cancel;
    SDL_Surface *image;
    Thread thread;
    CURL *connection;
    bool screenshot, busy;
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
    int grid_scroll, file_scroll;
    bool favorites_open, dimmed;
    BacklightControl backlight;
    SleepControl sleep_control;
    char (*favorites)[CATALOG_TEXT];
    size_t favorites_count;
    uint64_t deleted_until, prefetch_at;
    SortMode sort, draft_sort, genre_sort[GENRE_COUNT];
    bool filter_sort_changed;
    size_t genre, draft_genre, filter_row;
    bool filters_open, genre_focus, settings_open;
    TopFocus top_focus;
    size_t settings_row;
    Settings settings;
    uint64_t background_check_at;
    CatalogIndex catalogs[CATALOG_SOURCE_COUNT];
    CatalogEntry entries[MERGED_CAPACITY];
    size_t entry_count;
    CatalogIndexJob index_job;
    Thread index_thread;
    bool index_busy, show_partial_index;
    unsigned refresh_waiting;
    bool files_loaded[CATALOG_SOURCE_COUNT];
    char file_errors[CATALOG_SOURCE_COUNT][CATALOG_TEXT], favorite_key[CATALOG_TEXT];
    char index_notice[CATALOG_SOURCE_COUNT][CATALOG_TEXT], settings_notice[192], update_notice[192];
    Game active_game, game_sources[CATALOG_SOURCE_COUNT];
    const CatalogMetadata *active_metadata;
    GameTab game_tab;
    size_t screenshot_index;
    int description_scroll;
    bool show_original, fullscreen_shot;
    GameMetadata metadata;
    GameGallery gallery;
    GalleryJob gallery_job;
    Thread gallery_thread;
    bool gallery_busy, gallery_requested;
    GameTrailers trailers;
    TrailerJob trailer_job;
    Thread trailer_thread;
    bool trailer_busy, trailer_requested, trailer_open;
    size_t trailer_index;
    uint64_t trailer_controls_until;
    TrailerState trailer_state;
    bool confirm_install, choose_download, close, download_busy, download_finished;
    bool catalog_busy[CATALOG_SOURCE_COUNT];
    size_t download_choice;
    char update_target[256], staged_update_sha[41];
    bool update_ready, update_checking, update_offered;
    UpdateCheck update_check;
    Thread update_thread;
    char query[96], notice[192], delete_file[sizeof(((SavedFile *)0)->file)];
    DownloadRequest active_transfer, draft_transfer, queue[DOWNLOAD_QUEUE_LIMIT];
    size_t queue_count, queue_index;
    bool queue_focus;
    SavedFile saved[HISTORY_LIMIT];
    GameDetails details[CATALOG_SOURCE_COUNT];
    CatalogJob pending[CATALOG_SOURCE_COUNT];
    struct {
        GameFile file;
        char page[CATALOG_URL];
        atomic_bool cancel;
        Thread thread;
        bool busy;
    } size_job;
    FichierJob download;
    Thread catalog_thread[CATALOG_SOURCE_COUNT], download_thread;
    CoverJob cover_jobs[COVER_WORKERS];
    CachedCover covers[COVER_SLOTS];
    uint64_t previous_bytes;
    double speed;
    unsigned speed_stage;
    uint64_t speed_time;
} App;

static App app;
static struct {
    char label[512];
    SDL_Texture *texture;
    TTF_Font *font;
    SDL_Color color;
    int width, height, fit;
    uint64_t used;
} text_cache[TEXT_SLOTS];
static uint64_t text_use;
static pthread_mutex_t cover_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static const u64 up_buttons = HidNpadButton_Up | HidNpadButton_StickLUp;
static const u64 down_buttons = HidNpadButton_Down | HidNpadButton_StickLDown;
static const u64 left_buttons = HidNpadButton_Left | HidNpadButton_StickLLeft;
static const u64 right_buttons = HidNpadButton_Right | HidNpadButton_StickLRight;
static SDL_Color background = {15, 19, 31, 255};
static SDL_Color panel = {24, 31, 48, 255};
static SDL_Color selected = {35, 63, 104, 255};
static SDL_Color white = {241, 244, 250, 255};
static SDL_Color gray = {166, 181, 210, 255};
static SDL_Color blue = {125, 182, 255, 255};

static void apply_theme(void) {
    bool light = app.settings.light_theme;
    background = light ? (SDL_Color){238, 243, 249, 255} : (SDL_Color){15, 19, 31, 255};
    panel = light ? (SDL_Color){255, 255, 255, 255} : (SDL_Color){24, 31, 48, 255};
    selected = light ? (SDL_Color){207, 226, 248, 255} : (SDL_Color){35, 63, 104, 255};
    white = light ? (SDL_Color){20, 34, 54, 255} : (SDL_Color){241, 244, 250, 255};
    gray = light ? (SDL_Color){68, 87, 113, 255} : (SDL_Color){166, 181, 210, 255};
    blue = light ? (SDL_Color){0, 82, 170, 255} : (SDL_Color){125, 182, 255, 255};
}

static void rect(int x, int y, int w, int h, SDL_Color color) {
    if (w <= 0 || h <= 0) return;
    SDL_Rect box = { x, y, w, h };
    SDL_SetRenderDrawColor(app.renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(app.renderer, &box);
}

static void aligned_text(TTF_Font *font, const char *str, int x, int y, int width,
                         SDL_Color color, int alignment) {
    str = tr(str);
    if (!str || !*str || width <= 0) return;
    size_t slot = 0;
    for (size_t i = 0; i < TEXT_SLOTS; ++i) {
        if (text_cache[i].font == font && text_cache[i].fit == width &&
            !memcmp(&text_cache[i].color, &color, sizeof(color)) && !strcmp(text_cache[i].label, str)) {
            slot = i;
            break;
        }
        if (text_cache[i].used < text_cache[slot].used) slot = i;
    }

    if (text_cache[slot].font != font || text_cache[slot].fit != width ||
        memcmp(&text_cache[slot].color, &color, sizeof(color)) || strcmp(text_cache[slot].label, str)) {
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
            if (text_cache[slot].texture) SDL_DestroyTexture(text_cache[slot].texture);
            text_cache[slot].texture = texture;
            text_cache[slot].font = font;
            text_cache[slot].color = color;
            text_cache[slot].fit = width;
            text_cache[slot].width = surface->w;
            text_cache[slot].height = surface->h;
            snprintf(text_cache[slot].label, sizeof(text_cache[slot].label), "%s", str);
        }
        SDL_FreeSurface(surface);
        if (!texture) return;
    }

    text_cache[slot].used = ++text_use;
    int offset = alignment ? SDL_max(0, width - text_cache[slot].width) / alignment : 0;
    SDL_Rect box = {x + offset, y, text_cache[slot].width, text_cache[slot].height};
    SDL_RenderCopy(app.renderer, text_cache[slot].texture, NULL, &box);
}

static void text(TTF_Font *font, const char *str, int x, int y, int width, SDL_Color color) {
    aligned_text(font, str, x, y, width, color, 0);
}

static void centered_text(TTF_Font *font, const char *label, int x, int y, int width, SDL_Color color) {
    aligned_text(font, label, x, y, width, color, 2);
}

static void right_text(TTF_Font *font, const char *label, int x, int y, int width, SDL_Color color) {
    aligned_text(font, label, x, y, width, color, 1);
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
    const bool focused = !app.settings_open;
    const bool downloads = app.screen == DOWNLOADS;
    const bool library = app.screen == LIBRARY;
    if (focused && app.top_focus == TOP_SEARCH) {
        const int x = downloads ? COMPACT_BACK_X : HEADER_ACTION_X;
        const int width = downloads ? COMPACT_BACK_WIDTH : HEADER_ACTION_WIDTH;
        rect(x, TOP_Y, width, TOP_HEIGHT, selected);
    }
    if (focused && app.top_focus == TOP_DOWNLOADS)
        rect(DOWNLOADS_X, TOP_Y, 57, TOP_HEIGHT, selected);
    if ((focused && app.top_focus == TOP_SETTINGS) || app.settings_open)
        rect(SETTINGS_X, TOP_Y, TOP_END - SETTINGS_X, TOP_HEIGHT, selected);

    if (library && !app.settings_open) {
        icon_circle(HEADER_ACTION_X + 17, 42, 8, 2, white);
        for (int i = 0; i < 3; ++i)
            SDL_RenderDrawLine(app.renderer, HEADER_ACTION_X + 22 + i, 47, HEADER_ACTION_X + 28 + i, 53);
        text(app.normal, "Пошук", HEADER_ACTION_X + 35, 32, HEADER_ACTION_WIDTH - 39, white);
    } else if (downloads || app.settings_open) {
        right_text(app.normal, "‹ Назад", COMPACT_BACK_X + 8, 32, COMPACT_BACK_WIDTH - 16, white);
    } else {
        right_text(app.normal, "‹ Назад", HEADER_ACTION_X + 8, 32, HEADER_ACTION_WIDTH - 16, white);
    }
    if (!downloads && !app.settings_open) {
        const int arrow_x = DOWNLOADS_X + 28, arrow_y = 34;
        bool blink = app.download_busy && !app.active_transfer.app_update &&
                     SDL_GetTicks64() % 1600 < 800;
        SDL_Color icon = blink ? blue : white;
        if (app.download_busy && !app.active_transfer.app_update && !blink) icon = gray;
        rect(arrow_x - 4, arrow_y, 9, 16, icon);
        for (int i = 0; i < 13; ++i)
            rect(arrow_x - 12 + i, arrow_y + 14 + i, 25 - 2 * i, 1, icon);
    }

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
    if (offset / GRID_STEP_Y != app.grid_scroll / GRID_STEP_Y) app.prefetch_at = 0;
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
static void trailer_worker(void *arg) { load_game_trailers(arg); }
static void index_worker(void *arg) { catalog_build_index(arg); }
static void update_check_worker(void *arg) { check_update(arg); }
static void download_worker(void *arg) {
    FichierJob *job = arg;
    const char *path = job->url;
    if (job->app_update) {
        const char *error = stage_update(job, path, &app.active_transfer.update);
        job->update_staged = error == NULL;
        job->result = error ? error : "Оновлення готове. Встановлення після завершення черги.";
        return;
    }
    if (!job->local_unpack && !job->local_install) {
        fichier_download(job);
        if (atomic_load(&job->cancelled) || strcmp(job->result, "Download saved")) return;
        path = job->path;
    }
    bool archive_delete_failed = false, nsp_delete_failed = false;
    if (job->local_unpack || archive_name(path) || archive_signature(path)) {
        UnpackedPackages packages = {0};
        const char *error = unpack_archive(job, path, NXLOAD_DOWNLOAD_DIR,
                                      job->install_after_download ? &packages : NULL);
        if (error) {
            job->result = error;
            return;
        }
        if (!job->install_after_download) {
            archive_delete_failed = job->delete_rar && unlink(path) != 0;
            job->result = archive_delete_failed ? "Розпаковано · не вдалося видалити архів" :
                          job->delete_rar ? "Архів розпаковано й видалено" : "Архів розпаковано й збережено";
            return;
        }
        for (size_t i = 0; i < packages.count; ++i) {
            if (!nsp_name(packages.paths[i])) {
                job->result = "Архів розпаковано. NXLoad встановлює NSP. Інші формати можна відкрити в DBI.";
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
        archive_delete_failed = job->delete_rar && unlink(path) != 0;
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
    job->result = archive_delete_failed || nsp_delete_failed ?
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
    static const char *const names[CATALOG_SOURCE_COUNT] = {"NSRoms", "ROMsHQ", "ROMsNS"};
    return names[source];
}

static const Game *entry_game(const CatalogEntry *entry) {
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source)
        if (entry->games[source]) return entry->games[source];
    return NULL;
}

static const Game *game_at(size_t row) {
    return entry_game(&app.entries[app.rows[row]]);
}

static const char *entry_title(const CatalogEntry *entry) {
    return entry->metadata ? entry->metadata->title : entry_game(entry)->title;
}

static size_t favorite_index(const char *key) {
    for (size_t i = 0; i < app.favorites_count; ++i)
        if (!strcasecmp(app.favorites[i], key)) return i;
    return app.favorites_count;
}

static void load_favorites(void) {
    FILE *file = fopen(FAVORITES_PATH, "r");
    if (!file) file = fopen(FAVORITES_PATH ".prev", "r");
    if (!file) return;
    char key[CATALOG_TEXT + 2];
    while (fgets(key, sizeof(key), file)) {
        size_t length = strcspn(key, "\r\n");
        if (!length || length >= CATALOG_TEXT) continue;
        key[length] = 0;
        if (favorite_index(key) < app.favorites_count) continue;
        void *memory = realloc(app.favorites, (app.favorites_count + 1) * sizeof(*app.favorites));
        if (!memory) break;
        app.favorites = memory;
        strcpy(app.favorites[app.favorites_count++], key);
    }
    fclose(file);
}

static void make_favorite_key(char key[CATALOG_TEXT], const char *title) {
    snprintf(key, CATALOG_TEXT, "%.191s", title);
    clean_catalog_title(key);
    for (char *c = key; *c; ++c)
        if (*c == '\n' || *c == '\r') *c = ' ';
}

static bool set_favorite(const char *title) {
    char key[CATALOG_TEXT];
    make_favorite_key(key, title);
    if (!key[0] || !nxload_ensure_storage()) return false;
    size_t index = favorite_index(key);
    bool add = index == app.favorites_count;
    if (add) {
        void *memory = realloc(app.favorites, (app.favorites_count + 1) * sizeof(*app.favorites));
        if (!memory) return false;
        app.favorites = memory;
    }
    FILE *file = fopen(FAVORITES_PATH ".tmp", "w");
    if (!file) return false;
    bool ok = true;
    for (size_t i = 0; i < app.favorites_count && ok; ++i)
        if (i != index) ok = fprintf(file, "%s\n", app.favorites[i]) > 0;
    if (ok && add) ok = fprintf(file, "%s\n", key) > 0;
    if (ok) ok = !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;
    bool moved = false;
    if (ok && rename(FAVORITES_PATH ".tmp", FAVORITES_PATH)) {
        ok = errno == EEXIST &&
             (!unlink(FAVORITES_PATH ".prev") || errno == ENOENT);
        if (ok) {
            moved = !rename(FAVORITES_PATH, FAVORITES_PATH ".prev");
            ok = moved && !rename(FAVORITES_PATH ".tmp", FAVORITES_PATH);
        }
    }
    if (ok) unlink(FAVORITES_PATH ".prev");
    else if (moved) rename(FAVORITES_PATH ".prev", FAVORITES_PATH);
    if (!ok) {
        unlink(FAVORITES_PATH ".tmp");
        return false;
    }
    if (add) strcpy(app.favorites[app.favorites_count++], key);
    else {
        --app.favorites_count;
        memmove(app.favorites + index, app.favorites + index + 1,
                (app.favorites_count - index) * sizeof(*app.favorites));
    }
    return true;
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
        for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
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
    app.prefetch_at = 0;
    app.grid_scroll = 0;
    app.game_count = 0;
    app.genre_count = 0;
    app.game_index = 0;
    for (size_t i = 0; i < app.entry_count; ++i) {
        const CatalogEntry *entry = &app.entries[i];
        if (app.genre && (!entry->metadata || !(entry->metadata->genres & (1u << (app.genre - 1))))) continue;
        if (app.favorites_open) {
            char key[CATALOG_TEXT];
            make_favorite_key(key, entry_title(entry));
            if (favorite_index(key) == app.favorites_count) continue;
        }
        ++app.genre_count;
        if (!match_query(entry)) continue;
        app.rows[app.game_count++] = i;
    }
    qsort(app.rows, app.game_count, sizeof(app.rows[0]), compare_rows);
    if (!keep || !app.game_count) return;
    app.game_index = SDL_min(old_index, app.game_count - 1);
    for (size_t i = 0; i < app.game_count; ++i) {
        const CatalogEntry *entry = &app.entries[app.rows[i]];
        bool matches = false;
        for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
            const Game *game = entry->games[source];
            if (game && !strcmp(game->page, keep)) matches = true;
        }
        if (matches) {
            app.game_index = i;
            break;
        }
    }
    int row_shift = (int)(app.game_index / GRID_COLUMNS) - (int)(old_index / GRID_COLUMNS);
    app.grid_scroll = clamp_grid_scroll(old_scroll + row_shift * GRID_STEP_Y);
}

static void start_next_index(void) {
    if (app.index_busy || !app.refresh_waiting) return;
    CatalogSource source = CATALOG_NSROMS;
    if ((app.refresh_waiting & (1u << CATALOG_ROMSNS)) && !app.catalogs[CATALOG_ROMSNS].count)
        source = CATALOG_ROMSNS;
    else
        while (!(app.refresh_waiting & (1u << source))) ++source;
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
    app.refresh_waiting |= (1u << CATALOG_SOURCE_COUNT) - 1;
    if (app.index_busy) app.refresh_waiting &= ~(1u << app.index_job.source);
    start_next_index();
}

static void request_game_files(void) {
    if (app.screen != GAME || app.game_tab == GAME_OVERVIEW) return;
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
        if (app.game_tab == GAME_MEDIA && source != CATALOG_ROMSHQ) continue;
        if (!app.settings.source_enabled[source] || app.files_loaded[source] || app.catalog_busy[source] ||
            !app.game_sources[source].page[0]) continue;
        CatalogJob *job = &app.pending[source];
        memset(job, 0, sizeof(*job));
        atomic_init(&job->cancel, false);
        job->game = app.game_sources[source];
        job->source = (CatalogSource)source;
        app.catalog_busy[source] = start_worker(&app.catalog_thread[source], catalog_worker, job);
        if (!app.catalog_busy[source]) {
            app.files_loaded[source] = true;
            strcpy(app.file_errors[source], "Не вдалося запустити запит");
        }
    }
}

static void size_worker(void *unused) {
    (void)unused;
    catalog_probe_size(&app.size_job.file, &app.size_job.cancel);
}

static bool same_file(const GameFile *a, const GameFile *b) {
    return a->source == b->source && !strcmp(a->id, b->id) && !strcmp(a->url, b->url);
}

static void pump_file_sizes(void) {
    if (app.size_job.busy) {
        if (R_FAILED(waitSingle(waiterForThread(&app.size_job.thread), 0))) return;
        threadWaitForExit(&app.size_job.thread);
        threadClose(&app.size_job.thread);
        app.size_job.busy = false;
        if (!atomic_load(&app.size_job.cancel) && !strcmp(app.size_job.page, app.active_game.page)) {
            for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
                GameDetails *details = &app.details[source];
                for (size_t i = 0; i < details->count; ++i) {
                    GameFile *file = &details->files[i];
                    if (!same_file(file, &app.size_job.file)) continue;
                    if (app.size_job.file.size[0]) strcpy(file->size, app.size_job.file.size);
                    file->size_checked = true;
                }
            }
        }
    }
    if (app.screen != GAME || app.game_tab != GAME_FILES || app.download_busy) return;
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
        if (!app.settings.source_enabled[source]) continue;
        GameDetails *details = &app.details[source];
        for (size_t i = 0; i < details->count; ++i) {
            GameFile *file = &details->files[i];
            if (file->size_checked || !catalog_can_probe_size(file)) continue;
            file->size_checked = true;
            app.size_job.file = *file;
            strcpy(app.size_job.page, app.active_game.page);
            atomic_store(&app.size_job.cancel, false);
            app.size_job.busy = start_worker(&app.size_job.thread, size_worker, NULL);
            return;
        }
    }
}

static SDL_Surface *decode_cover(const unsigned char *bytes, size_t count, bool screenshot) {
    if (!bytes || !count) return NULL;
    SDL_RWops *src = SDL_RWFromConstMem(bytes, (int)count);
    SDL_Surface *image = src ? IMG_Load_RW(src, 1) : NULL;
    if (!image) return NULL;

    SDL_Surface *scaled = NULL;
    double factor = SDL_min((screenshot ? 1280.0 : 320.0) / image->w,
                            (screenshot ? 720.0 : 320.0) / image->h);
    if (factor < 1.0) {
        int w = SDL_max(1, (int)(factor * image->w));
        int h = SDL_max(1, (int)(factor * image->h));
        scaled = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (scaled && SDL_BlitScaled(image, NULL, scaled, NULL) < 0) {
            SDL_FreeSurface(scaled);
            scaled = NULL;
        }
    }
    if (!scaled) return image;
    SDL_FreeSurface(image);
    return scaled;
}

static void cover_worker(void *arg) {
    CoverJob *job = arg;
    size_t count = 0;
    if (job->screenshot) {
        unsigned char *bytes = load_gallery_image(job->url, &job->cancel, &count);
        if (!atomic_load(&job->cancel)) job->image = decode_cover(bytes, count, true);
        free(bytes);
        return;
    }

    unsigned char *bytes = cover_cache_read(job->url, &count);
    if (bytes) {
        if (!atomic_load(&job->cancel)) {
            job->image = decode_cover(bytes, count, false);
            if (!job->image) {
                pthread_mutex_lock(&cover_cache_lock);
                cover_cache_discard(job->url);
                pthread_mutex_unlock(&cover_cache_lock);
            }
        }
        free(bytes);
    }
    if (job->image || atomic_load(&job->cancel)) return;
    if (!job->connection) job->connection = curl_easy_init();
    if (!job->connection) return;
    bytes = catalog_fetch_thumbnail(job->connection, job->url, &job->cancel, &count);
    if (!atomic_load(&job->cancel)) {
        job->image = decode_cover(bytes, count, false);
        if (job->image) {
            pthread_mutex_lock(&cover_cache_lock);
            cover_cache_write(job->url, bytes, count);
            pthread_mutex_unlock(&cover_cache_lock);
        }
    }
    free(bytes);
}

static CachedCover *lookup_cover(const char *url, bool displayed) {
    if (!url[0]) return NULL;
    for (size_t i = 0; i < COVER_SLOTS; ++i) {
        CachedCover *entry = &app.covers[i];
        if (strcmp(entry->url, url)) continue;
        uint64_t now = SDL_GetTicks64();
        if (entry->texture && displayed) entry->last_used = now;
        else if (!entry->texture && now - entry->last_used >= 30000) {
            entry->url[0] = 0;
            return NULL;
        }
        return entry;
    }
    return NULL;
}

static CachedCover *find_cover(const char *url) {
    return lookup_cover(url, true);
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
        CachedCover *cover = lookup_cover(urls[i], false);
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
        if (app.game_tab == GAME_MEDIA) {
            if (!job->screenshot) return false;
            size_t first = app.screenshot_index ? app.screenshot_index - 1 : 0;
            size_t end = SDL_min(app.gallery.count, app.screenshot_index + 2);
            for (size_t i = first; i < end; ++i)
                if (!strcmp(job->url, app.gallery.screenshots[i])) return true;
            return false;
        }
        return !job->screenshot && uses_cover(&app.active_game, app.active_metadata, job->url);
    }
    if (app.screen != LIBRARY || job->screenshot) return false;
    size_t first = grid_first(), end = grid_end();
    if (!app.download_busy) {
        first = first > COVER_LOOKAHEAD ? first - COVER_LOOKAHEAD : 0;
        end = SDL_min(app.game_count, end + COVER_LOOKAHEAD);
    }
    for (size_t i = first; i < end; ++i) {
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
        SDL_Texture *texture = job->image ? SDL_CreateTextureFromSurface(app.renderer, job->image) : NULL;
        CachedCover *entry = &app.covers[0];
        for (size_t i = 0; i < COVER_SLOTS; ++i) {
            CachedCover *candidate = &app.covers[i];
            if (!candidate->url[0]) {
                entry = candidate;
                break;
            }
            if (candidate->last_used < entry->last_used) entry = candidate;
        }
        if (entry->texture) SDL_DestroyTexture(entry->texture);
        strcpy(entry->url, job->url);
        entry->texture = texture;
        entry->last_used = SDL_GetTicks64();
    }
    if (job->image) SDL_FreeSurface(job->image);
    job->image = NULL;
}

static const char *next_cover(bool *screenshot) {
    *screenshot = false;
    if (app.screen == GAME) {
        if (app.game_tab != GAME_MEDIA)
            return missing_cover(&app.active_game, app.active_metadata);
        if (!app.gallery.count) return NULL;
        *screenshot = true;
        size_t shots[] = {app.screenshot_index, app.screenshot_index + 1,
                          app.screenshot_index ? app.screenshot_index - 1 : SIZE_MAX};
        size_t limit = app.download_busy ? 1 : sizeof(shots) / sizeof(*shots);
        for (size_t i = 0; i < limit; ++i) {
            if (shots[i] >= app.gallery.count) continue;
            const char *image = app.gallery.screenshots[shots[i]];
            if (!lookup_cover(image, false) && !cover_pending(image)) return image;
        }
        return NULL;
    }
    if (app.screen != LIBRARY || !app.game_count) return NULL;
    const CatalogEntry *selected_entry = &app.entries[app.rows[app.game_index]];
    const char *url = missing_cover(entry_game(selected_entry), selected_entry->metadata);
    if (url) return url;
    size_t first = grid_first(), end = grid_end();
    for (size_t i = first; i < end; ++i) {
        const CatalogEntry *entry = &app.entries[app.rows[i]];
        url = missing_cover(entry_game(entry), entry->metadata);
        if (url) return url;
    }
    uint64_t now = SDL_GetTicks64();
    if (app.download_busy || now < app.prefetch_at) return NULL;
    for (size_t distance = 0; distance < COVER_LOOKAHEAD; ++distance) {
        size_t positions[] = {end + distance, distance < first ? first - 1 - distance : SIZE_MAX};
        for (size_t direction = 0; direction < 2; ++direction) {
            if (positions[direction] >= app.game_count) continue;
            const CatalogEntry *entry = &app.entries[app.rows[positions[direction]]];
            url = missing_cover(entry_game(entry), entry->metadata);
            if (url) return url;
        }
    }
    app.prefetch_at = now + 250;
    return NULL;
}

static bool pump_covers(void) {
    bool finished = false;
    for (size_t i = 0; i < COVER_WORKERS; ++i) {
        CoverJob *job = &app.cover_jobs[i];
        if (!job->busy) continue;
        if (!cover_visible(job)) atomic_store(&job->cancel, true);
        if (!finished && R_SUCCEEDED(waitSingle(waiterForThread(&job->thread), 0))) {
            finish_cover(job);
            finished = true;
        }
    }
    if (app.gallery_busy && !app.gallery.count) return finished;
    for (size_t i = 0; i < COVER_WORKERS; ++i) {
        CoverJob *job = &app.cover_jobs[i];
        if (job->busy) continue;
        const char *url = next_cover(&job->screenshot);
        if (!url) break;
        strcpy(job->url, url);
        atomic_init(&job->cancel, false);
        job->image = NULL;
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
    if (app.screen == GAME && app.game_tab == GAME_MEDIA &&
        !app.gallery_busy && !app.gallery_requested) {
        memset(job, 0, sizeof(*job));
        atomic_init(&job->cancel, false);
        strcpy(job->page, app.active_game.page);
        if (app.active_metadata && app.active_metadata->nintendo_life[0]) {
            strcpy(job->nintendo_life, app.active_metadata->nintendo_life);
            strcpy(job->title, app.active_metadata->title);
        } else {
            strcpy(job->title, app.metadata.title);
        }
        strcpy(job->heading, app.details[CATALOG_NSROMS].title[0] ?
               app.details[CATALOG_NSROMS].title : app.details[CATALOG_ROMSHQ].title);
        strcpy(job->catalog_title, app.active_game.title);
        strcpy(job->fallback_hashes, app.metadata.fallback_hashes);
        app.gallery_requested = true;
        app.gallery_busy = start_worker(&app.gallery_thread, gallery_worker, job);
        if (!app.gallery_busy)
            strcpy(app.gallery.issue, "Не вдалося запустити запит Nintendo Life");
    }
}

static void pump_trailers(void) {
    TrailerJob *job = &app.trailer_job;
    if (app.trailer_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.trailer_thread), 0))) {
        threadWaitForExit(&app.trailer_thread);
        threadClose(&app.trailer_thread);
        app.trailer_busy = false;
        if (!atomic_load(&job->cancel) && !strcmp(job->page, app.active_game.page))
            app.trailers = job->result;
    }
    if (app.screen != GAME || app.game_tab != GAME_MEDIA ||
        app.trailer_busy || app.trailer_requested) return;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->cancel, false);
    strcpy(job->page, app.active_game.page);
    const char *title = app.active_metadata ? app.active_metadata->title :
        app.metadata.title[0] ? app.metadata.title : app.active_game.title;
    strcpy(job->title, title);
    app.trailer_requested = true;
    app.trailer_busy = start_worker(&app.trailer_thread, trailer_worker, job);
}

static int file_compare(const void *a, const void *b) {
    return strcasecmp(((const SavedFile *)a)->file, ((const SavedFile *)b)->file);
}

static bool saved_archive(const char *name) {
    if (archive_name(name)) return true;
    char path[320];
    int bytes = snprintf(path, sizeof(path), NXLOAD_DOWNLOAD_DIR "/%s", name);
    return bytes > 0 && (size_t)bytes < sizeof(path) && archive_signature(path);
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
            if (n < sizeof(app.saved[0].file) && (package_name(entry->d_name) || archive_name(entry->d_name) ||
                (ext && !strcasecmp(ext, ".bin") && (saved_archive(entry->d_name) || saved_nsp(entry->d_name))))) {
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
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
        if (app.index_busy && app.index_job.source == (CatalogSource)source) continue;
        time_t cached = app.catalogs[source].saved_at;
        time_t checked = app.settings.catalog_checked[source];
        if (daily_check_due(now, cached) && daily_check_due(now, checked))
            app.refresh_waiting |= 1u << source;
    }
}

static void open_catalogs(void) {
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source)
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
    bool game_request = app.gallery_busy || (app.game_tab == GAME_MEDIA && !app.gallery_requested);
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source)
        game_request |= app.catalog_busy[source];
    game_request &= app.screen == GAME;
    atomic_store(&job->paused, app.download_busy || game_request);
    size_t published = atomic_load(&job->published);
    bool romsns_batch_ready = job->source != CATALOG_ROMSNS || !index->count ||
                             published >= index->count + CATALOG_PAGE_GAMES;
    if (app.show_partial_index && published > index->count && romsns_batch_ready) {
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
    } else if (app.show_partial_index && published > index->count) {
        memcpy(index->games + index->count, job->index.games + index->count,
               (published - index->count) * sizeof(Game));
        index->count = published;
        remake_catalog();
    }
    strcpy(app.index_notice[job->source], job->error);
}

static void launch_next_transfer(void);

static unsigned transfer_stage(void) {
    if (atomic_load(&app.download.installing)) return 2;
    if (atomic_load(&app.download.unpacking)) return 1;
    return 0;
}

static void finish_jobs(void) {
    if (app.update_checking && R_SUCCEEDED(waitSingle(waiterForThread(&app.update_thread), 0))) {
        threadWaitForExit(&app.update_thread);
        threadClose(&app.update_thread);
        app.update_checking = false;
        app.update_offered = app.update_check.build.id[0] != 0;
        snprintf(app.update_notice, sizeof(app.update_notice), "%s",
                 app.update_check.error ? app.update_check.error : app.update_offered ?
                 "Доступне оновлення NXLoad" : "Вже встановлено актуальну збірку NXLoad");
    }
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
        Thread *thread = &app.catalog_thread[source];
        if (!app.catalog_busy[source] || R_FAILED(waitSingle(waiterForThread(thread), 0))) continue;
        threadWaitForExit(thread);
        threadClose(thread);
        app.catalog_busy[source] = false;
        CatalogJob *job = &app.pending[source];
        if (!atomic_load(&job->cancel) &&
            (app.screen == GAME || (app.screen == DOWNLOADS && app.previous == GAME)) &&
            !strcmp(job->game.page, app.game_sources[source].page)) {
            app.files_loaded[source] = true;
            app.details[source] = job->details;
            strcpy(app.file_errors[source], job->error);
            if (app.settings.source_enabled[source] && source == CATALOG_ROMSHQ &&
                job->details.screenshot_count && !app.gallery.count) {
                if (app.gallery_busy) atomic_store(&app.gallery_job.cancel, true);
                app.screenshot_index = 0;
                app.gallery.count = job->details.screenshot_count;
                memcpy(app.gallery.screenshots, job->details.screenshots,
                       app.gallery.count * sizeof(app.gallery.screenshots[0]));
                app.gallery.from_romshq = true;
                app.gallery_requested = true;
            }
            if (job->details.cover_url[0])
                strcpy(app.active_game.cover, job->details.cover_url);
            if (!app.metadata.title[0] &&
                read_game_metadata(&app.metadata, app.active_game.title, job->details.title))
                update_description();
        }
    }
    if (app.download_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.download_thread), 0))) {
        threadWaitForExit(&app.download_thread);
        threadClose(&app.download_thread);
        app.download_busy = false;
        app.download_finished = true;
        if (app.download.update_staged) {
            app.update_ready = true;
            strcpy(app.staged_update_sha, app.active_transfer.update.sha);
        }
        if (app.active_transfer.app_update)
            snprintf(app.update_notice, sizeof(app.update_notice), "%s", tr(app.download.result));
        refresh_saved();
        if (app.queue_count && !app.active_transfer.app_update)
            snprintf(app.notice, sizeof(app.notice), tr("Попереднє: %.170s"), tr(app.download.result));
    }
    if (app.download_busy) {
        unsigned stage = transfer_stage();
        uint64_t now = SDL_GetTicks64(), bytes = atomic_load(&app.download.received);
        if (stage != app.speed_stage || bytes < app.previous_bytes) {
            app.speed = 0;
            app.speed_stage = stage;
            app.previous_bytes = bytes;
            app.speed_time = now;
        } else if (now - app.speed_time >= 500) {
            double sample = (bytes - app.previous_bytes) * 1000.0 / (now - app.speed_time);
            app.speed = sample && app.speed ? 0.7 * app.speed + 0.3 * sample : sample;
            app.previous_bytes = bytes;
            app.speed_time = now;
        }
    }
    if (!app.download_busy) {
        if (app.queue_count) launch_next_transfer();
        else if (app.update_ready) app.close = true;
    }
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
    app.speed_stage = 0;
    app.download_finished = false;
    app.download_busy = start_worker(&app.download_thread, download_worker, job);
    if (!app.download_busy) {
        job->result = "Не вдалося запустити операцію";
        if (request->app_update) strcpy(app.update_notice, job->result);
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
    snprintf(request.name, sizeof(request.name), "%.120s · %.64s", title, tr(file->title));
    strcpy(request.id, file->id);
    strcpy(request.url, file->source == FILE_ROMSHQ_SIGNED ? app.game_sources[CATALOG_ROMSHQ].page : file->url);
    choose_transfer(&request);
}

static bool keyboard(char *out, size_t capacity, const char *title, const char *guide, const char *button) {
    SwkbdConfig config;
    if (R_FAILED(swkbdCreate(&config, 0))) return false;
    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetHeaderText(&config, tr(title));
    swkbdConfigSetGuideText(&config, tr(guide));
    swkbdConfigSetOkButtonText(&config, tr(button));
    swkbdConfigSetInitialText(&config, out);
    swkbdConfigSetStringLenMax(&config, capacity - 1);
    char value[2048];
    Result rc = swkbdShow(&config, value, capacity);
    swkbdClose(&config);
    if (R_FAILED(rc)) return false;
    const char *start = value + strspn(value, " \t\r\n");
    size_t length = strlen(start);
    while (length && strchr(" \t\r\n", start[length - 1])) --length;
    memcpy(out, start, length);
    out[length] = 0;
    return true;
}

static void open_downloads(void);

static void start_update_check(bool manual) {
    if (app.update_ready || app.update_checking || app.update_offered ||
        (app.download_busy && app.active_transfer.app_update)) return;
    for (size_t i = 0; i < app.queue_count; ++i)
        if (app.queue[i].app_update) return;
    if (!app.update_target[0] || access(app.update_target, R_OK)) {
        if (manual) strcpy(app.update_notice, "Для оновлення запусти NXLoad.nro з microSD");
        return;
    }
    atomic_init(&app.update_check.cancelled, false);
    app.update_checking = start_worker(&app.update_thread, update_check_worker, &app.update_check);
    strcpy(app.update_notice, app.update_checking ? "Перевірка оновлення NXLoad…" :
           "Не вдалося запустити перевірку оновлення");
}

static bool update_dialog_visible(void) {
    return app.update_offered && !app.fullscreen_shot && !app.trailer_open && !app.choose_download &&
           !app.confirm_install && !app.delete_file[0] && !app.filters_open;
}

static void respond_to_update(bool install) {
    app.update_offered = false;
    if (!install) {
        app.update_notice[0] = 0;
        return;
    }
    if (app.queue_count == DOWNLOAD_QUEUE_LIMIT) {
        strcpy(app.update_notice, "Черга заповнена. Перевір оновлення пізніше.");
        return;
    }
    DownloadRequest request = {.name = "Оновлення NXLoad", .app_update = true,
                               .update = app.update_check.build};
    strcpy(request.url, app.update_target);
    const char *message = enqueue_transfer(&request);
    if (app.download_busy) {
        app.settings_open = false;
        open_downloads();
        snprintf(app.update_notice, sizeof(app.update_notice), "%s",
                 *message ? message : "Завантаження оновлення NXLoad…");
    }
}

static void pump_daily_checks(uint64_t ticks) {
    if (ticks < app.background_check_at) return;
    app.background_check_at = ticks + 60000;
    schedule_daily_catalogs(time(NULL));
}

static void start_saved(const char *file, bool unpack) {
    DownloadRequest request = {.unpack = unpack, .install_local = !unpack};
    snprintf(request.name, sizeof(request.name), "%.191s", file);
    snprintf(request.url, sizeof(request.url), NXLOAD_DOWNLOAD_DIR "/%s", file);
    if (unpack) choose_transfer(&request);
    else strcpy(app.notice, enqueue_transfer(&request));
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
        app.notice[0] = 0;
        app.deleted_until = SDL_GetTicks64() + 3000;
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
    app.game_tab = GAME_FILES;
}

static size_t file_rows(FileRow rows[CATALOG_SOURCE_COUNT * (CATALOG_FILE_COUNT + 2)]) {
    size_t count = 0;
    for (unsigned i = 0; i < CATALOG_SOURCE_COUNT; ++i) {
        if (!app.settings.source_enabled[i]) continue;
        CatalogSource source = (CatalogSource)i;
        rows[count++] = (FileRow){true, NULL, source_name(source)};
        const GameDetails *details = &app.details[source];
        if (details->count) {
            for (size_t j = 0; j < details->count; ++j)
                rows[count++] = (FileRow){false, &details->files[j], details->files[j].title};
        } else {
            const char *status = !app.game_sources[source].page[0] ? "Гри немає в каталозі" :
                !app.files_loaded[source] ? "Перевірка файлів…" :
                app.file_errors[source][0] ? app.file_errors[source] : "Файлів немає";
            rows[count++] = (FileRow){false, NULL, status};
        }
    }
    return count;
}

static int clamp_file_scroll(int offset, size_t count) {
    int maximum = (int)count * FILE_STEP - (FILE_LIST_BOTTOM - FILE_LIST_Y);
    return SDL_clamp(offset, 0, SDL_max(0, maximum));
}

static void move_file_selection(u64 buttons) {
    FileRow rows[CATALOG_SOURCE_COUNT * (CATALOG_FILE_COUNT + 2)];
    size_t count = file_rows(rows);
    if (!count) return;
    size_t index = SDL_min(app.file_index, count - 1);
    if (!rows[index].file || (buttons & (up_buttons | down_buttons))) {
        size_t candidate = index;
        if (rows[index].file) candidate = move(index, count, buttons);
        while (candidate < count && !rows[candidate].file) {
            if (buttons & up_buttons) {
                if (candidate == 0) break;
                --candidate;
            } else ++candidate;
        }
        if (candidate < count && rows[candidate].file) index = candidate;
    }
    app.file_index = index;
    if (buttons & (up_buttons | down_buttons)) {
        int top = (int)index * FILE_STEP;
        int bottom = top + FILE_STEP;
        if (index && rows[index - 1].heading) top -= FILE_STEP;
        app.file_scroll = clamp_file_scroll(SDL_clamp(app.file_scroll,
            bottom - (FILE_LIST_BOTTOM - FILE_LIST_Y), top), count);
    }
    if ((buttons & HidNpadButton_A) && rows[index].file)
        select_file_action(rows[index].file);
}

static size_t current_genre_tab(void) {
    if (app.favorites_open) return TAB_COUNT - 1;
    for (size_t i = 0; i < TAB_COUNT; ++i)
        if (genre_tabs[i] == app.genre) return i;
    return TAB_COUNT;
}

static void select_genre(size_t genre) {
    app.favorites_open = genre == FAVORITES_TAB;
    app.genre = app.favorites_open ? 0 : genre;
    app.sort = app.genre_sort[app.genre];
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
        case SETTINGS_NSROMS: value = &app.settings.source_enabled[CATALOG_NSROMS]; break;
        case SETTINGS_ROMSHQ: value = &app.settings.source_enabled[CATALOG_ROMSHQ]; break;
        case SETTINGS_ROMSNS: value = &app.settings.source_enabled[CATALOG_ROMSNS]; break;
        case SETTINGS_THEME: value = &app.settings.light_theme; break;
        case SETTINGS_LANGUAGE: value = &app.settings.english; break;
        case SETTINGS_REFRESH_CATALOG:
            if (activate) {
                refresh_catalogs();
                app.settings_notice[0] = 0;
            }
            return;
        case SETTINGS_UPDATE:
            if (activate) start_update_check(true);
            return;
        default: return;
    }
    *value = !*value;
    if (save_settings(&app.settings)) {
        app.settings_notice[0] = 0;
        app.background_check_at = 0;
        apply_theme();
        set_english(app.settings.english);
        if (row >= SETTINGS_NSROMS && row <= SETTINGS_ROMSNS) {
            app.file_index = 0;
            app.file_scroll = 0;
        }
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
        app.prefetch_at = 0;
        int row_y = (int)(index / GRID_COLUMNS) * GRID_STEP_Y;
        app.grid_scroll = clamp_grid_scroll(SDL_clamp(app.grid_scroll,
            row_y - (GRID_ROWS - 1) * GRID_STEP_Y, row_y));
    }
}

static void input(u64 buttons);

static void open_downloads(void) {
    if (app.screen == DOWNLOADS) return;
    if (!app.download_busy) refresh_saved();
    app.notice[0] = 0;
    app.deleted_until = 0;
    app.top_focus = TOP_NONE;
    app.previous = app.screen;
    app.screen = DOWNLOADS;
}

static bool navigate_header(u64 buttons) {
    if (!app.top_focus) return false;
    if (buttons & (left_buttons | right_buttons)) {
        if (app.screen == DOWNLOADS)
            app.top_focus = buttons & left_buttons ? TOP_SEARCH : TOP_SETTINGS;
        else if ((buttons & left_buttons) && app.top_focus > TOP_SEARCH) --app.top_focus;
        else if ((buttons & right_buttons) && app.top_focus < TOP_SETTINGS) ++app.top_focus;
    }
    if (buttons & down_buttons) app.top_focus = TOP_NONE;
    if (buttons & HidNpadButton_A) {
        if (app.top_focus == TOP_SETTINGS) toggle_settings();
        else if (app.top_focus == TOP_DOWNLOADS) open_downloads();
        else if (app.top_focus == TOP_SEARCH) {
            app.top_focus = TOP_NONE;
            input(HidNpadButton_B);
        }
    }
    return true;
}

static bool scroll_description(int delta) {
    int height = 0, previous = app.description_scroll;
    if (app.description) SDL_QueryTexture(app.description, NULL, NULL, NULL, &height);
    app.description_scroll = SDL_clamp(previous + delta, 0, SDL_max(0, height - 244));
    return app.description_scroll != previous;
}

static void step_screenshot(bool next) {
    if (next && app.screenshot_index + 1 < app.gallery.count) ++app.screenshot_index;
    if (!next && app.screenshot_index) --app.screenshot_index;
}

static void open_trailer(size_t index) {
    if (index >= app.trailers.count) return;
    app.trailer_index = index;
    app.trailer_open = true;
    app.trailer_controls_until = SDL_GetTicks64() + 3000;
    app.trailer_state = trailer_play(app.renderer, app.trailers.urls[index]) ?
        TRAILER_LOADING : TRAILER_ERROR;
}

static void close_trailer(void) {
    trailer_stop();
    app.trailer_open = false;
}

static void step_trailer(bool next) {
    if (next && app.trailer_index + 1 < app.trailers.count)
        open_trailer(app.trailer_index + 1);
    else if (!next && app.trailer_index)
        open_trailer(app.trailer_index - 1);
}

static void toggle_trailer(void) {
    if (app.trailer_state == TRAILER_ENDED || app.trailer_state == TRAILER_ERROR)
        open_trailer(app.trailer_index);
    else trailer_pause();
    app.trailer_controls_until = SDL_GetTicks64() + 3000;
}

static void touch_swipe(int y, int dx, int dy) {
    if (app.filters_open || app.delete_file[0] || app.confirm_install || app.choose_download || app.settings_open) return;
    if (abs(dx) < 42 || abs(dx) <= abs(dy)) return;
    if (app.trailer_open) {
        trailer_seek(dx < 0);
        app.trailer_controls_until = SDL_GetTicks64() + 3000;
        return;
    }
    if (app.fullscreen_shot) { step_screenshot(dx < 0); return; }
    if (app.screen == LIBRARY && y >= TAB_Y && y < TAB_Y + TAB_HEIGHT) next_genre_tab(dx < 0);
    if (app.screen == GAME && y >= 201 && y < 245)
        input(dx < 0 ? HidNpadButton_Right : HidNpadButton_Left);
    else if (app.screen == GAME && app.game_tab == GAME_MEDIA && y >= 259 && y < FILE_LIST_BOTTOM)
        step_screenshot(dx < 0);
}

static void touch_tap(int x, int y) {
    if (app.trailer_open) {
        if (x < 175 && y < 90) close_trailer();
        else if (y >= 652 && x > 1010 && app.trailers.count > 1) step_trailer(x >= 1140);
        else toggle_trailer();
        return;
    }
    if (app.fullscreen_shot) {
        if (x < 150 && y < 90) app.fullscreen_shot = false;
        else if (x < 240) step_screenshot(false);
        else if (x > 1040) step_screenshot(true);
        return;
    }
    if (update_dialog_visible()) {
        if (x >= 224 && x < 1050 && y >= 418 && y < 488) respond_to_update(x < 640);
        return;
    }
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
    const bool downloads = app.screen == DOWNLOADS;
    const bool back_hit = x >= COMPACT_BACK_X && x < COMPACT_BACK_X + COMPACT_BACK_WIDTH;
    const bool settings_hit = x >= SETTINGS_X && x < TOP_END;
    if (app.settings_open) {
        if (y >= TOP_Y && y < TOP_Y + TOP_HEIGHT &&
            (back_hit || settings_hit)) {
            toggle_settings();
        } else if (x >= 180 && x < 1100 && y >= SETTINGS_Y) {
            size_t visible = (size_t)(y - SETTINGS_Y) / SETTINGS_STEP;
            size_t row = first_visible(app.settings_row, SETTINGS_VISIBLE) + visible;
            if (visible < SETTINGS_VISIBLE && row < SETTINGS_ROWS && (y - SETTINGS_Y) % SETTINGS_STEP < 38) {
                app.settings_row = row;
                change_setting(row, true);
            }
        }
        return;
    }
    if (y >= TOP_Y && y < TOP_Y + TOP_HEIGHT) {
        if (settings_hit) toggle_settings();
        else if (downloads) {
            if (back_hit) input(HidNpadButton_B);
        } else if (x >= DOWNLOADS_X && x < SETTINGS_X) open_downloads();
        else if (x >= HEADER_ACTION_X && x < HEADER_ACTION_X + HEADER_ACTION_WIDTH)
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
        } else if (app.game_tab == GAME_FILES && x >= 430 && x < 1211 &&
                   y >= FILE_LIST_Y && y < FILE_LIST_BOTTOM) {
            FileRow rows[CATALOG_SOURCE_COUNT * (CATALOG_FILE_COUNT + 2)];
            size_t count = file_rows(rows);
            int offset = y - FILE_LIST_Y + app.file_scroll;
            size_t index = (size_t)offset / FILE_STEP;
            if (index < count && rows[index].file && offset % FILE_STEP < 42) {
                app.file_index = index;
                app.top_focus = TOP_NONE;
                select_file_action(rows[index].file);
            }
        } else if (app.game_tab == GAME_MEDIA && x >= 945 && x < 1204 &&
                   y >= 593 && y < 633 && app.trailers.count) {
            open_trailer(0);
        } else if (app.game_tab == GAME_MEDIA && y >= 259 && y < FILE_LIST_BOTTOM) {
            if (x >= 430 && x < 550) step_screenshot(false);
            else if (x >= 1080 && x < 1204) step_screenshot(true);
            else if (x >= 550 && x < 1080 && app.gallery.count) app.fullscreen_shot = true;
        }
    } else if (y >= 332 && y < 332 + FILE_ROWS * 46) {
        bool queue = x >= 650;
        app.top_focus = TOP_NONE;
        size_t current = queue ? app.queue_index : app.saved_index;
        size_t chosen = first_visible(current, FILE_ROWS) + (size_t)(y - 332) / 46;
        bool repeated = queue == app.queue_focus && current == chosen;
        app.queue_focus = queue;
        if (queue && chosen < app.queue_count) app.queue_index = chosen;
        if (!queue && chosen < app.saved_count) {
            app.saved_index = chosen;
            if (repeated) input(HidNpadButton_A);
        }
    } else if (y >= 657 && y < 698) {
        app.top_focus = TOP_NONE;
        if (x >= DOWNLOADS_DIM_X) input(HidNpadButton_Minus);
        else if (x >= 880) input(HidNpadButton_B);
        else if (x >= 685) input(HidNpadButton_Y);
        else if (x >= 455) input(HidNpadButton_X);
    }
}

typedef enum { DRAG_NONE, DRAG_GRID, DRAG_DESCRIPTION, DRAG_FILES, DRAG_SAVED, DRAG_QUEUE, DRAG_SETTINGS } DragArea;

static DragArea drag_area(int x, int y) {
    if (app.fullscreen_shot || update_dialog_visible()) return DRAG_NONE;
    if (app.filters_open || app.delete_file[0] || app.confirm_install || app.choose_download) return DRAG_NONE;
    if (app.settings_open)
        return x >= 180 && x < 1100 && y >= SETTINGS_Y &&
               y < SETTINGS_Y + SETTINGS_VISIBLE * SETTINGS_STEP ? DRAG_SETTINGS : DRAG_NONE;
    if (app.screen == LIBRARY && app.game_count && y >= GRID_VIEW_TOP && y < GRID_VIEW_BOTTOM) return DRAG_GRID;
    if (app.screen == GAME && x >= 430 && x < 1211) {
        if (app.game_tab == GAME_OVERVIEW && y >= 269 && y < 513) return DRAG_DESCRIPTION;
        if (app.game_tab == GAME_FILES && y >= FILE_LIST_Y && y < FILE_LIST_BOTTOM) return DRAG_FILES;
    }
    if (app.screen == DOWNLOADS && y >= 332 && y < 332 + FILE_ROWS * 46)
        return x < 650 ? DRAG_SAVED : DRAG_QUEUE;
    return DRAG_NONE;
}

static bool drag_scroll(DragArea area, int delta, int *remainder) {
    if (area == DRAG_GRID) return scroll_grid(delta);
    if (area == DRAG_DESCRIPTION) return scroll_description(delta);
    if (area == DRAG_FILES) {
        FileRow rows[CATALOG_SOURCE_COUNT * (CATALOG_FILE_COUNT + 2)];
        size_t count = file_rows(rows);
        int previous = app.file_scroll;
        app.file_scroll = clamp_file_scroll(previous + delta, count);
        if (previous == app.file_scroll) return false;
        app.top_focus = TOP_NONE;
        size_t first = (size_t)(app.file_scroll + FILE_STEP - 1) / FILE_STEP;
        size_t end = SDL_min(count, (size_t)(app.file_scroll + FILE_LIST_BOTTOM - FILE_LIST_Y) / FILE_STEP);
        if (app.file_index < first || app.file_index >= end) {
            app.file_index = first;
            for (size_t i = first; i < end; ++i)
                if (rows[i].file) { app.file_index = i; break; }
        }
        return true;
    }

    *remainder += delta;
    int steps = *remainder / 46;
    *remainder %= 46;
    if (!steps) return false;
    if (area == DRAG_SETTINGS) {
        app.settings_row = (size_t)SDL_clamp((int)app.settings_row + steps, 0, SETTINGS_ROWS - 1);
        return true;
    }

    app.top_focus = TOP_NONE;
    app.queue_focus = area == DRAG_QUEUE;
    size_t *index = app.queue_focus ? &app.queue_index : &app.saved_index;
    size_t count = app.queue_focus ? app.queue_count : app.saved_count;
    if (count) *index = (size_t)SDL_clamp((int)*index + steps, 0, (int)count - 1);
    return true;
}

static void darken_download_screen(void) {
    app.dimmed = true;
    if (appletGetOperationMode() != AppletOperationMode_Handheld ||
        appletGetFocusState() != AppletFocusState_InFocus) return;
    if (R_SUCCEEDED(appletSetLcdBacklightOffEnabled(true))) {
        app.backlight = BACKLIGHT_APPLET;
        return;
    }
    if (R_FAILED(lblInitialize())) return;
    LblBacklightSwitchStatus status;
    if (R_SUCCEEDED(lblGetBacklightSwitchStatus(&status)) &&
        (status == LblBacklightSwitchStatus_Enabled || status == LblBacklightSwitchStatus_Enabling) &&
        R_SUCCEEDED(lblSwitchBacklightOff(0))) {
        app.backlight = BACKLIGHT_LBL;
    } else {
        lblExit();
    }
}

static bool wake_download_screen(void) {
    if (app.backlight == BACKLIGHT_APPLET && R_FAILED(appletSetLcdBacklightOffEnabled(false)))
        return false;
    if (app.backlight == BACKLIGHT_LBL) {
        if (R_FAILED(lblSwitchBacklightOn(0))) return false;
        lblExit();
    }
    app.backlight = BACKLIGHT_UNCHANGED;
    app.dimmed = false;
    return true;
}

static void keep_console_awake(bool enabled) {
    if (enabled) {
        if (app.sleep_control != SLEEP_UNCHANGED) return;
        bool already_disabled = false;
        if (R_SUCCEEDED(appletIsAutoSleepDisabled(&already_disabled)) && already_disabled)
            return;
        if (R_SUCCEEDED(appletSetAutoSleepDisabled(true)))
            app.sleep_control = SLEEP_APPLET;
        else if (R_SUCCEEDED(appletSetMediaPlaybackState(true)))
            app.sleep_control = SLEEP_MEDIA;
    } else if (app.sleep_control != SLEEP_UNCHANGED) {
        Result rc = app.sleep_control == SLEEP_APPLET ? appletSetAutoSleepDisabled(false) :
                    appletSetMediaPlaybackState(false);
        if (R_SUCCEEDED(rc)) app.sleep_control = SLEEP_UNCHANGED;
    }
}

static void app_lifecycle_hook(AppletHookType hook, void *unused) {
    (void)unused;
    bool focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (hook == AppletHookType_OnFocusState)
        keep_console_awake(focused);
    else if (hook == AppletHookType_OnExitRequest)
        keep_console_awake(false);

    if (app.dimmed && (hook == AppletHookType_OnExitRequest ||
                      hook == AppletHookType_OnOperationMode ||
                      (hook == AppletHookType_OnFocusState && !focused)))
        wake_download_screen();
}

static bool poll_touch(bool controller_input) {
    static int start_x, start_y, x, y, remainder;
    static bool touching, cancelled, dragged;
    static DragArea area;
    HidTouchScreenState state;
    if (!hidGetTouchScreenStates(&state, 1)) return false;
    if (app.dimmed && state.count && !controller_input && !cancelled) {
        wake_download_screen();
        touching = dragged = false;
        cancelled = true;
        return true;
    }
    if (state.count > 1 || (controller_input && state.count)) cancelled = true;
    if (state.count == 1 && !cancelled) {
        int previous_y = y;
        x = (int)state.touches[0].x;
        y = (int)state.touches[0].y;
        if (!touching) {
            start_x = x;
            start_y = y;
            touching = true;
            area = drag_area(x, y);
            remainder = 0;
            return false;
        }
        if (area != DRAG_NONE && (dragged || abs(x - start_x) >= GRID_DRAG_THRESHOLD ||
                                  abs(y - start_y) >= GRID_DRAG_THRESHOLD)) {
            int delta = (dragged ? previous_y : start_y) - y;
            dragged = true;
            return drag_scroll(area, delta, &remainder);
        }
    } else if (!state.count) {
        bool activate = touching && !cancelled && !dragged && !controller_input;
        touching = cancelled = dragged = false;
        if (activate) {
            int dx = x - start_x, dy = y - start_y;
            if (abs(dx) >= 42 || abs(dy) >= 42) touch_swipe(start_y, dx, dy);
            else touch_tap(x, y);
            return true;
        }
    }
    return false;
}

static void input(u64 buttons) {
    if (!buttons) return;
    if (app.dimmed) {
        if (buttons & HidNpadButton_Minus) wake_download_screen();
        return;
    }
    if (update_dialog_visible()) {
        if (buttons & HidNpadButton_A) respond_to_update(true);
        else if (buttons & HidNpadButton_B) respond_to_update(false);
        else if (buttons & HidNpadButton_Plus) app.close = true;
        return;
    }
    if (app.trailer_open) {
        app.trailer_controls_until = SDL_GetTicks64() + 3000;
        if (buttons & HidNpadButton_B) close_trailer();
        else if (buttons & HidNpadButton_Plus) app.close = true;
        else if (buttons & HidNpadButton_L) step_trailer(false);
        else if (buttons & HidNpadButton_R) step_trailer(true);
        else if (buttons & (left_buttons | right_buttons)) trailer_seek((buttons & right_buttons) != 0);
        else if (buttons & HidNpadButton_A) toggle_trailer();
        return;
    }
    if (app.fullscreen_shot) {
        if (buttons & HidNpadButton_B) app.fullscreen_shot = false;
        else if (buttons & (left_buttons | HidNpadButton_L)) step_screenshot(false);
        else if (buttons & (right_buttons | HidNpadButton_R)) step_screenshot(true);
        else if (buttons & HidNpadButton_Plus) app.close = true;
        return;
    }
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
    if (!app.filters_open && (buttons & HidNpadButton_Minus)) {
        if (app.screen == DOWNLOADS) darken_download_screen();
        else toggle_settings();
        return;
    }
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
            if (app.draft_genre != app.genre) app.favorites_open = false;
            app.genre = app.draft_genre;
            app.genre_sort[app.genre] = app.sort;
            rebuild_view(keep[0] ? keep : NULL);
            app.filters_open = false;
        } else if (buttons & (HidNpadButton_B | HidNpadButton_ZL)) {
            app.filters_open = false;
        }
        return;
    }
    if (app.screen == DOWNLOADS) {
        if (buttons & HidNpadButton_B) {
            app.top_focus = TOP_NONE;
            app.screen = app.previous;
            return;
        }
        if ((buttons & HidNpadButton_Y) && app.download_busy) {
            atomic_store(&app.download.cancelled, true);
            return;
        }
        if (navigate_header(buttons)) return;
        if (buttons & (left_buttons | HidNpadButton_L)) app.queue_focus = false;
        if (buttons & (right_buttons | HidNpadButton_R)) app.queue_focus = true;
        if ((buttons & up_buttons) && !(app.queue_focus ? app.queue_index : app.saved_index)) {
            app.top_focus = app.queue_focus ? TOP_SETTINGS : TOP_SEARCH;
            return;
        }
        if (app.queue_focus) app.queue_index = move(app.queue_index, app.queue_count, buttons);
        else app.saved_index = move(app.saved_index, app.saved_count, buttons);
        if (buttons & HidNpadButton_X) {
            if (app.queue_focus && app.queue_count) remove_queued(app.queue_index);
            else if (!app.queue_focus && app.saved_count)
                strcpy(app.delete_file, app.saved[app.saved_index].file);
            return;
        }
        if ((buttons & HidNpadButton_A) && !app.queue_focus && app.saved_count) {
            const char *selected_file = app.saved[app.saved_index].file;
            if (saved_archive(selected_file)) start_saved(selected_file, true);
            else if (saved_nsp(selected_file)) start_saved(selected_file, false);
            else if (!app.download_busy && !app.queue_count) app.confirm_install = true;
            else strcpy(app.notice, "DBI можна відкрити після завершення черги");
        }
        return;
    }
    if (app.screen == GAME) {
        if (buttons & HidNpadButton_B) {
            app.top_focus = TOP_NONE;
            if (app.game_tab == GAME_FILES) {
                app.game_tab = GAME_OVERVIEW;
                return;
            }
            for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source)
                if (app.catalog_busy[source]) atomic_store(&app.pending[source].cancel, true);
            if (app.gallery_busy) atomic_store(&app.gallery_job.cancel, true);
            if (app.trailer_busy) atomic_store(&app.trailer_job.cancel, true);
            if (app.size_job.busy) atomic_store(&app.size_job.cancel, true);
            app.notice[0] = 0;
            app.screen = LIBRARY;
            return;
        }
        if (buttons & HidNpadButton_Y) {
            if (set_favorite(app.favorite_key)) {
                if (app.favorites_open) rebuild_view(app.active_game.page);
                app.notice[0] = 0;
            } else strcpy(app.notice, "Не вдалося зберегти «Вибране»");
            return;
        }
        if (navigate_header(buttons)) return;
        if (buttons & (left_buttons | right_buttons)) {
            if ((buttons & left_buttons) && app.game_tab > GAME_OVERVIEW) --app.game_tab;
            if ((buttons & right_buttons) && app.game_tab < GAME_FILES) ++app.game_tab;
            return;
        }
        if ((buttons & up_buttons) &&
            ((app.game_tab == GAME_OVERVIEW && !app.description_scroll) ||
             app.game_tab == GAME_MEDIA ||
             (app.game_tab == GAME_FILES && !app.file_scroll && app.file_index <= 1))) {
            app.top_focus = TOP_DOWNLOADS;
            return;
        }
        if (app.game_tab == GAME_FILES) {
            move_file_selection(buttons);
        } else if (app.game_tab == GAME_MEDIA) {
            if (buttons & HidNpadButton_L) step_screenshot(false);
            if (buttons & HidNpadButton_R) step_screenshot(true);
            if ((buttons & HidNpadButton_X) && app.trailers.count) open_trailer(0);
            if (buttons & HidNpadButton_A) {
                if (app.gallery.count) app.fullscreen_shot = true;
                else if (app.trailers.count) open_trailer(0);
            }
        } else {
            if ((buttons & HidNpadButton_X) && app.metadata.original[0]) {
                app.show_original = !app.show_original;
                update_description();
            }
            if (buttons & up_buttons) scroll_description(-25);
            if (buttons & down_buttons) scroll_description(25);
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
        if (app.top_focus == TOP_DOWNLOADS) { open_downloads(); return; }
        buttons = HidNpadButton_X;
    }
    if (buttons & HidNpadButton_B) {
        app.game_index = 0;
        app.grid_scroll = 0;
        app.prefetch_at = 0;
        app.genre_focus = false;
        app.top_focus = TOP_NONE;
        return;
    }
    if ((buttons & HidNpadButton_Y) && app.game_count && !app.top_focus && !app.genre_focus) {
        const CatalogEntry *entry = &app.entries[app.rows[app.game_index]];
        char keep[CATALOG_URL];
        strcpy(keep, entry_game(entry)->page);
        if (set_favorite(entry_title(entry))) {
            if (app.favorites_open) rebuild_view(keep);
            app.notice[0] = 0;
        } else strcpy(app.notice, "Не вдалося зберегти «Вибране»");
        return;
    }
    bool was_genre_focus = app.genre_focus;
    move_in_library(buttons);
    if ((buttons & HidNpadButton_X) &&
               keyboard(app.query, sizeof(app.query), "Пошук ігор", "Введи назву гри", "Пошук")) {
        rebuild_view(NULL);
    } else if ((buttons & HidNpadButton_A) && !was_genre_focus && !app.genre_focus) {
        if (app.game_count) {
            const CatalogEntry *entry = &app.entries[app.rows[app.game_index]];
            app.active_game = *game_at(app.game_index);
            app.active_metadata = entry->metadata;
            memset(app.game_sources, 0, sizeof(app.game_sources));
            for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source)
                if (entry->games[source]) app.game_sources[source] = *entry->games[source];
            app.show_original = false;
            memset(&app.details, 0, sizeof(app.details));
            memset(&app.metadata, 0, sizeof(app.metadata));
            memset(&app.gallery, 0, sizeof(app.gallery));
            memset(&app.trailers, 0, sizeof(app.trailers));
            app.trailer_requested = false;
            read_game_metadata(&app.metadata, app.active_game.title, entry_title(entry));
            if (!app.game_sources[CATALOG_ROMSHQ].page[0] &&
                (app.active_metadata || app.metadata.title[0]))
                guess_romshq_page(&app.game_sources[CATALOG_ROMSHQ], app.metadata.title[0] ?
                                      app.metadata.title : entry_title(entry));
            app.gallery_requested = false;
            update_description();
            memset(app.files_loaded, 0, sizeof(app.files_loaded));
            memset(app.file_errors, 0, sizeof(app.file_errors));
            make_favorite_key(app.favorite_key, entry_title(entry));
            app.file_index = 0;
            app.file_scroll = 0;
            app.screenshot_index = 0;
            app.game_tab = GAME_OVERVIEW;
            app.screen = GAME;
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

static void draw_favorite_star(int x, int y) {
    static const SDL_Point points[] = {
        {14, 14}, {14, 0}, {18, 9}, {28, 10}, {21, 17}, {23, 27},
        {14, 22}, {5, 27}, {7, 17}, {0, 10}, {10, 9},
    };
    static const int triangles[] = {
        0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 6,
        0, 6, 7, 0, 7, 8, 0, 8, 9, 0, 9, 10, 0, 10, 1,
    };
    SDL_Vertex vertices[sizeof(points) / sizeof(*points)] = {0};
    for (size_t i = 0; i < sizeof(vertices) / sizeof(*vertices); ++i) {
        vertices[i].position = (SDL_FPoint){x + points[i].x + 1, y + points[i].y + 2};
        vertices[i].color = (SDL_Color){40, 25, 8, 255};
    }
    SDL_RenderGeometry(app.renderer, NULL, vertices, sizeof(vertices) / sizeof(*vertices),
                       triangles, sizeof(triangles) / sizeof(*triangles));
    for (size_t i = 0; i < sizeof(vertices) / sizeof(*vertices); ++i) {
        vertices[i].position.x -= 1;
        vertices[i].position.y -= 2;
        vertices[i].color = (SDL_Color){255, 201, 75, 255};
    }
    SDL_RenderGeometry(app.renderer, NULL, vertices, sizeof(vertices) / sizeof(*vertices),
                       triangles, sizeof(triangles) / sizeof(*triangles));
}

static void draw_library(void) {
    for (size_t i = 0; i < TAB_COUNT; ++i) {
        int x = tab_left(i), width = tab_widths[i];
        bool active = current_genre_tab() == i;
        if (active) {
            rect(x, TAB_Y, width, TAB_HEIGHT, selected);
            rect(x, TAB_Y + TAB_HEIGHT - 3, width, 3, blue);
            if (app.genre_focus) rect(x, TAB_Y, 3, TAB_HEIGHT, blue);
        }
        const char *label = genre_tabs[i] == FAVORITES_TAB ? "Вибране" :
                            i ? genre_names[genre_tabs[i]] : "Усі";
        centered_text(app.normal, label,
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
        if (app.favorites_count) {
            char key[CATALOG_TEXT];
            make_favorite_key(key, entry_title(entry));
            if (favorite_index(key) < app.favorites_count) draw_favorite_star(x + 8, y + 8);
        }
        unsigned rating = entry->metadata ? entry->metadata->rating : 0;
        char score[12] = "—";
        if (rating) snprintf(score, sizeof(score), "%u.%u", rating / 10, rating % 10);
        int badge_x = x + size - 48, badge_y = y + size - 48;
        SDL_Color badge = panel;
        badge.a = 235;
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
        text(app.small, app.notice, 1000, 593, 240, blue);
    } else if (!app.index_busy) {
        text(app.normal, app.favorites_open ? "У вибраному немає ігор" :
             app.entry_count ? "Ігор за цими фільтрами не знайдено" : "Каталог порожній",
             48, 176, 790, gray);
    }
    char count[192];
    snprintf(count, sizeof(count), tr("Всього ігор: %zu"), app.entry_count);
    text(app.small, count, 40, 631, 300, gray);
    if (app.genre || app.favorites_open) {
        snprintf(count, sizeof(count), "%s: %zu", tr(app.favorites_open ? "Вибране" : genre_names[app.genre]), app.genre_count);
        text(app.small, count, 340, 631, 340, gray);
    }
    if (app.query[0]) {
        snprintf(count, sizeof(count), tr("Знайдено: %zu"), app.game_count);
        text(app.small, count, (app.genre || app.favorites_open) ? 700 : 340, 631, 230, gray);
    }
    right_text(app.small, sort_names[app.sort], 1000, 621, 240, blue);
}

static void draw_settings(void) {
    static const char *const labels[] = {
        "Дія з файлом за замовчуванням", "Видаляти архів після розпакування",
        "Видаляти NSP після встановлення", "Щоденне оновлення каталогів",
        "Джерело NSRoms", "Джерело ROMsHQ", "Джерело ROMsNS", "Тема", "Мова",
        "Оновити каталоги зараз", "Перевірити оновлення NXLoad",
    };
    const char *const on = "Увімкнено", *const off = "Вимкнено";
    const char *const values[] = {
        app.settings.install_default ? "Завантажити й встановити" : "Завантажити",
        app.settings.delete_rar ? on : off, app.settings.delete_nsp ? on : off,
        app.settings.auto_catalog ? on : off,
        app.settings.source_enabled[CATALOG_NSROMS] ? on : off,
        app.settings.source_enabled[CATALOG_ROMSHQ] ? on : off,
        app.settings.source_enabled[CATALOG_ROMSNS] ? on : off,
        app.settings.light_theme ? "Світла" : "Темна",
        app.settings.english ? "English" : "Українська", "", "",
    };
    text(app.large, "Налаштування", 195, 89, 920, white);
    size_t first = first_visible(app.settings_row, SETTINGS_VISIBLE);
    for (size_t i = first; i < SETTINGS_ROWS && i < first + SETTINGS_VISIBLE; ++i) {
        int y = SETTINGS_Y + (int)(i - first) * SETTINGS_STEP;
        rect(180, y, 920, 38, i == app.settings_row ? selected : panel);
        text(app.normal, labels[i], 195, y + 9, 560, white);
        if (*values[i])
            right_text(app.small, values[i], 755, y + 12, 330, i == app.settings_row ? blue : gray);
    }
    if (app.index_busy || app.refresh_waiting) {
        char status[128];
        unsigned page = app.index_busy ? atomic_load(&app.index_job.page) : 0;
        unsigned pages = app.index_busy ? atomic_load(&app.index_job.pages) : 0;
        if (app.download_busy) strcpy(status, "Оновлення каталогів чекає завершення завантаження");
        else if (pages) snprintf(status, sizeof(status), tr("Оновлення каталогів · %u / %u"), page, pages);
        else strcpy(status, "Оновлення каталогів…");
        text(app.small, status, 195, 566, 895, gray);
    } else {
        char status[CATALOG_SOURCE_COUNT * (CATALOG_TEXT + 20)] = {0};
        char *next = status;
        for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
            if (!app.index_notice[source][0]) continue;
            size_t remaining = sizeof(status) - (size_t)(next - status);
            int written = snprintf(next, remaining, "%s%s · %s",
                                   next == status ? "" : "    ", source_name(source), tr(app.index_notice[source]));
            if (written < 0 || (size_t)written >= remaining) break;
            next += written;
        }
        text(app.small, status, 195, 566, 895, gray);
    }
    const char *update = app.update_ready ? "Оновлення очікує завершення черги" :
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
    snprintf(line, sizeof(line), tr("Порядок     ‹  %s  ›"), tr(sort_names[app.draft_sort]));
    row(0, app.filter_row, 253, 270, 774, line);
    snprintf(line, sizeof(line), tr("Жанр        ‹  %s  ›"), tr(genre_names[app.draft_genre]));
    row(1, app.filter_row, 253, 327, 774, line);
    text(app.small, "← / →  Змінити", 263, 417, 747, gray);
    text(app.small, "A  Застосувати", 263, 458, 386, blue);
    text(app.small, "B  Назад", 675, 458, 352, gray);
}

static void draw_cover(void) {
    rect(56, 127, 308, 308, background);
    SDL_Texture *cover = cover_texture(&app.active_game, app.active_metadata);
    fit_cover(cover, 56, 127, 308, 308);
    if (favorite_index(app.favorite_key) < app.favorites_count) draw_favorite_star(64, 135);
    unsigned rating = app.active_metadata ? app.active_metadata->rating : 0;
    char score[48] = "Рейтинг · —";
    if (rating) snprintf(score, sizeof(score), tr("Рейтинг · %u.%u / 10"), rating / 10, rating % 10);
    text(app.small, score, 65, 460, 293, blue);
    char release[48];
    unsigned date = app.active_metadata ? app.active_metadata->released : 0;
    if (app.metadata.released[0])
        snprintf(release, sizeof(release), tr("Дата виходу · %s"), app.metadata.released);
    else if (date)
        snprintf(release, sizeof(release), tr("Дата виходу · %04u-%02u-%02u"),
                 date / 10000, date / 100 % 100, date % 100);
    else strcpy(release, "Дата виходу · —");
    text(app.small, release, 65, 493, 293, gray);
    char studio[CATALOG_TEXT + 32];
    bool show_publisher = !app.metadata.developer[0] && app.metadata.publisher[0];
    const char *name = show_publisher ? app.metadata.publisher : app.metadata.developer;
    snprintf(studio, sizeof(studio), tr(show_publisher ? "Видавець · %s" : "Розробник · %s"),
             *name ? name : "—");
    text(app.small, studio, 65, 526, 293, gray);
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
        text(app.normal, "Опис недоступний", 430, 269, 769, gray);
    }
    if (app.metadata.original[0]) {
        text(app.small, app.show_original ? "Англійський оригінал" : "Український опис", 430, 548, 769, gray);
        text(app.small, app.show_original ? "X  Українською" : "X  Показати оригінал", 430, 577, 769, blue);
    }
}

static void draw_game_screenshots(void) {
    if (!app.gallery.count) {
        const char *status = app.gallery_busy || !app.gallery_requested ||
            app.catalog_busy[CATALOG_ROMSHQ] ? "Пошук скриншотів…" :
            app.gallery.issue[0] ? app.gallery.issue : "Скриншоти недоступні";
        text(app.normal, status, 430, 273, 769, gray);
        return;
    }
    CachedCover *shot = find_cover(app.gallery.screenshots[app.screenshot_index]);
    SDL_Texture *texture = shot ? shot->texture : NULL;
    if (texture && app.fullscreen_shot) {
        fit_cover(texture, 0, 0, 1280, 720);
    } else if (texture) {
        int width = 0, height = 0;
        if (!SDL_QueryTexture(texture, NULL, NULL, &width, &height) && width > 0 && height > 0) {
            int view_height = SDL_min(333, SDL_max(1, 774 * height / width));
            SDL_Rect src = {0, 0, width, height};
            if (774 * height / width > view_height) {
                src.h = SDL_max(1, view_height * width / 774);
                src.y = (height - src.h) / 2;
            }
            SDL_Rect dst = {430, 259, 774, view_height};
            SDL_RenderCopy(app.renderer, texture, &src, &dst);
        }
    } else {
        text(app.normal, shot ? "Скриншот недоступний" : "Завантаження скриншота…",
             app.fullscreen_shot ? 40 : 430, 281, 724, gray);
    }
    if (!app.fullscreen_shot) {
        char count[72];
        snprintf(count, sizeof(count), tr("L / R   %zu / %zu     A  На весь екран"),
                 app.screenshot_index + 1, app.gallery.count);
        text(app.small, count, 430, 604, 769, blue);
    }
}

static void draw_game_files(void) {
    FileRow rows[CATALOG_SOURCE_COUNT * (CATALOG_FILE_COUNT + 2)];
    size_t count = file_rows(rows);
    if (!count) {
        text(app.normal, "Джерела вимкнені в налаштуваннях", 430, 275, 770, gray);
        return;
    }
    app.file_scroll = clamp_file_scroll(app.file_scroll, count);
    const SDL_Rect clip = {429, FILE_LIST_Y, 782, FILE_LIST_BOTTOM - FILE_LIST_Y};
    SDL_RenderSetClipRect(app.renderer, &clip);
    for (size_t i = (size_t)app.file_scroll / FILE_STEP; i < count; ++i) {
        int y = FILE_LIST_Y + (int)i * FILE_STEP - app.file_scroll;
        if (y >= FILE_LIST_BOTTOM) break;
        if (rows[i].file) {
            const char *size = rows[i].file->size[0] ? rows[i].file->size : "—";
            if (!strstr(rows[i].label, size)) {
                if (i == app.file_index && !app.top_focus) {
                    rect(429, y, 782, 42, selected);
                    rect(429, y, 3, 42, blue);
                }
                text(app.normal, rows[i].label, 440, y + 8, 602, white);
                right_text(app.small, size, 1048, y + 11, 152, gray);
            } else row(i, app.top_focus ? SIZE_MAX : app.file_index, 429, y, 782, rows[i].label);
        } else if (rows[i].heading) {
            rect(429, y, 782, 42, background);
            text(app.normal, rows[i].label, 440, y + 8, 758, blue);
        } else text(app.small, rows[i].label, 440, y + 11, 758, gray);
    }
    SDL_RenderSetClipRect(app.renderer, NULL);
    text(app.small, app.notice, 430, 604, 772, gray);
}

static void draw_game(void) {
    rect(40, 106, 345, 530, panel);
    rect(401, 106, 839, 530, panel);
    draw_cover();
    const char *title = app.active_metadata ? app.active_metadata->title :
                        app.metadata.title[0] ? app.metadata.title : app.active_game.title;
    text(app.large, title, 430, 130, 772, white);
    if (app.notice[0] && app.game_tab != GAME_FILES)
        right_text(app.small, app.notice, 700, 177, 510, gray);
    const char *tabs[] = {"Огляд", "Медіа", "Файли"};
    for (unsigned i = 0; i < sizeof(tabs) / sizeof(tabs[0]); ++i) {
        int x = 430 + (int)i * 252;
        bool selected_tab = (GameTab)i == app.game_tab;
        rect(x, 201, 242, 42, selected_tab ? selected : background);
        if (selected_tab) rect(x, 240, 242, 3, blue);
        text(app.normal, tabs[i], x + 13, 208, 215, selected_tab ? white : gray);
    }
    if (app.game_tab == GAME_OVERVIEW) draw_game_overview();
    else if (app.game_tab == GAME_MEDIA) {
        draw_game_screenshots();
        if (app.trailers.count) {
            char label[96];
            snprintf(label, sizeof(label), tr("X  Трейлери · %zu"), app.trailers.count);
            rect(945, 595, 259, 34, selected);
            text(app.small, label, 960, 603, 230, blue);
        }
    }
    else draw_game_files();
}

static void draw_downloads(void) {
    rect(40, 106, 1200, 530, panel);
    unsigned stage = app.download_busy || app.download_finished ? transfer_stage() : 0;
    text(app.large, app.active_transfer.app_update && (app.download_busy || app.download_finished) ?
         "Оновлення NXLoad" : stage == 2 ? "Встановлення" : stage == 1 ? "Розпакування" :
         "Завантаження", 66, 128, 1100, white);
    if (app.download_busy || app.download_finished) {
        uint64_t have = atomic_load(&app.download.received), total = atomic_load(&app.download.total);
        double percent = total ? SDL_min(100.0, 100.0 * have / total) : 0;
        char line[192];
        text(app.normal, tr(app.active_transfer.name), 66, 182, 825, white);
        rect(66, 225, 1148, 8, selected);
        rect(66, 225, (int)(1148 * percent / 100.0), 8, blue);
        if (app.download_busy && stage == 2) {
            if (total)
                snprintf(line, sizeof(line), tr("Встановлення на microSD · %.1f%% · %.1f / %.1f МіБ"),
                         percent, have / 1048576.0, total / 1048576.0);
            else strcpy(line, "Перевірка пакета NSP…");
        } else if (app.download_busy && stage == 1) {
            snprintf(line, sizeof(line), tr("Розпакування архіву · %.1f / %.1f МіБ"), have / 1048576.0, total / 1048576.0);
        } else if (app.download_busy && total) {
            snprintf(line, sizeof(line), tr("%.1f%%  ·  %.1f / %.1f МіБ  ·  %.1f МіБ/с"),
                     percent, have / 1048576.0, total / 1048576.0, app.speed / 1048576.0);
        } else if (app.download_busy && have) {
            snprintf(line, sizeof(line), tr("%.1f МіБ  ·  %.1f МіБ/с"), have / 1048576.0, app.speed / 1048576.0);
        } else if (app.download_busy) {
            strcpy(line, app.active_transfer.app_update ? "Отримання файлу оновлення…" : "Запит адреси файлу…");
        } else if (!strcmp(app.download.result, "Download saved")) {
            snprintf(line, sizeof(line), tr("Збережено · %.1f МіБ · середня %.1f МіБ/с"),
                     have / 1048576.0, app.download.avg_speed / 1048576.0);
        } else if (atomic_load(&app.download.installing) || atomic_load(&app.download.unpacking) ||
                   app.active_transfer.install_local || app.active_transfer.install_after_download || app.active_transfer.app_update) {
            snprintf(line, sizeof(line), "%s", tr(app.download.result));
        } else {
            snprintf(line, sizeof(line), tr("%s  ·  API HTTP %ld  ·  Файл HTTP %ld"),
                     tr(app.download.result), app.download.api_http, app.download.http);
        }
        text(app.small, line, 66, 249, 815, blue);
        if (app.download_busy && have < total && app.speed > 0 && app.speed_stage == stage) {
            uint64_t seconds = (uint64_t)SDL_min(359999.0, (total - have) / app.speed + 0.99);
            char eta[80];
            snprintf(eta, sizeof(eta), tr("Залишилось %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64),
                     seconds / 3600, seconds / 60 % 60, seconds % 60);
            right_text(app.small, eta, 889, 249, 325, blue);
        }
        if (app.download_finished && stage != 2)
            text(app.small, app.download.tls_detail, 66, 270, 1148, gray);
    }
    rect(650, 298, 1, 288, selected);
    text(app.normal, "Збережені файли", 66, 293, 560, app.queue_focus ? gray : white);
    char queue_title[80];
    snprintf(queue_title, sizeof(queue_title), tr("У черзі · %zu"), app.queue_count);
    text(app.normal, queue_title, 677, 293, 537, app.queue_focus ? white : gray);
    size_t first = first_visible(app.saved_index, FILE_ROWS);
    for (size_t i = first; i < app.saved_count && i < first + FILE_ROWS; ++i)
        row(i, app.queue_focus ? SIZE_MAX : app.saved_index, 66,
            332 + (int)(i - first) * 46, 558, app.saved[i].file);
    first = first_visible(app.queue_index, FILE_ROWS);
    for (size_t i = first; i < app.queue_count && i < first + FILE_ROWS; ++i) {
        if (app.queue[i].install_after_download) {
            char label[CATALOG_TEXT];
            snprintf(label, sizeof(label), tr("↓ + Встановити · %.145s"), tr(app.queue[i].name));
            row(i, app.queue_focus ? app.queue_index : SIZE_MAX, 677,
                332 + (int)(i - first) * 46, 537, label);
        } else {
            row(i, app.queue_focus ? app.queue_index : SIZE_MAX, 677,
                332 + (int)(i - first) * 46, 537, app.queue[i].name);
        }
    }
    if (!app.saved_count) text(app.small, "Поки немає збережених файлів", 66, 342, 550, gray);
    if (!app.queue_count) text(app.small, "Черга порожня", 677, 342, 535, gray);
    const char *notice = SDL_GetTicks64() < app.deleted_until ? "Файл видалено" : app.notice;
    text(app.small, notice, 66, 608, 1148, gray);
}

static void draw_update_dialog(void) {
    char line[112];
    const UpdateBuild *build = &app.update_check.build;
    if (build->version[0])
        snprintf(line, sizeof(line), tr("Версія %s"), build->version);
    else
        snprintf(line, sizeof(line), tr("Збірка %" PRIu64 " · %.8s"), build->run, build->sha);
    rect(190, 200, 900, 305, selected);
    text(app.large, "Доступне оновлення NXLoad", 224, 230, 828, white);
    text(app.normal, line, 224, 298, 828, gray);
    text(app.small, "Оновлення встановиться автоматично після завершення черги.", 224, 343, 828, gray);
    text(app.small, app.update_target, 224, 374, 828, gray);
    text(app.normal, "A  Встановити", 224, 428, 396, blue);
    text(app.normal, "B  Пізніше", 656, 428, 390, gray);
}

static void draw_trailer(void) {
    const SDL_Color black = {0, 0, 0, 255}, light = {245, 245, 245, 255};
    rect(0, 0, 1280, 720, black);
    app.trailer_state = trailer_draw(app.renderer);
    const char *status = app.trailer_state == TRAILER_LOADING ? "Завантаження трейлера…" :
                         app.trailer_state == TRAILER_ERROR ? "Трейлер недоступний" :
                         app.trailer_state == TRAILER_ENDED ? "Трейлер завершено" : NULL;
    if (status) {
        rect(40, 320, 430, 52, black);
        text(app.normal, status, 55, 331, 400, light);
    }
    if (status || app.trailer_state == TRAILER_PAUSED || SDL_GetTicks64() < app.trailer_controls_until) {
        rect(0, 0, 153, 60, black);
        text(app.small, "B  Назад", 25, 19, 115, light);
        rect(0, 662, 1280, 58, black);
        const char *controls = app.trailer_state == TRAILER_ERROR || app.trailer_state == TRAILER_ENDED ?
            "A  Відтворити знову" : "A  Пауза / Відтворити    ← / →  ±10 с";
        text(app.small, controls, 40, 681, 760, light);
        if (app.trailers.count > 1) {
            char page[64];
            snprintf(page, sizeof(page), "L / R    %zu / %zu", app.trailer_index + 1, app.trailers.count);
            right_text(app.small, page, 1000, 681, 230, light);
        }
    }
}

static void draw(void) {
    if (app.trailer_open) {
        draw_trailer();
        SDL_RenderPresent(app.renderer);
        return;
    }
    if (app.dimmed) {
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        SDL_RenderPresent(app.renderer);
        return;
    }
    if (app.fullscreen_shot) {
        rect(0, 0, 1280, 720, (SDL_Color){0, 0, 0, 255});
        draw_game_screenshots();
        SDL_RenderPresent(app.renderer);
        return;
    }
    rect(0, 0, 1280, 720, background);
    text(app.large, "NXLoad", 40, 24, 320, white);
    text(app.small, NXLOAD_VERSION, 206, 44, 120, gray);
    draw_header();
    if (app.settings_open) {
        draw_settings();
        if (update_dialog_visible()) draw_update_dialog();
        SDL_RenderPresent(app.renderer);
        return;
    }
    if (app.screen == LIBRARY) draw_library();
    else if (app.screen == GAME) draw_game();
    else draw_downloads();
    if (app.screen == LIBRARY) {
        text(app.small, "A  Відкрити    B  На початок    X  Пошук    Y  Вибране    L / R  Розділи    ZL  Сортування    −  Налаштування",
             40, 665, 1200, gray);
    } else if (app.screen == GAME) {
        text(app.small, "← / →  Вкладки    Y  Вибране    B  Назад    ↑  Верхнє меню    −  Налаштування",
             40, 665, 1200, gray);
    } else {
        text(app.small, "← / →  Списки    A  Встановити / розпакувати", 40, 665, 410, gray);
        text(app.small, app.queue_focus ? "X  Прибрати з черги" : "X  Видалити файл", 455, 665, 225, gray);
        if (app.download_busy) text(app.small, "Y  Скасувати", 685, 665, 190, gray);
        text(app.small, "B  Назад", 880, 665, DOWNLOADS_DIM_X - 880, gray);
        text(app.small, "−  Вимкнути екран", DOWNLOADS_DIM_X, 665, TOP_END - DOWNLOADS_DIM_X, gray);
    }
    if (app.filters_open) draw_filters();
    if (update_dialog_visible()) draw_update_dialog();
    if (app.choose_download) {
        rect(190, 200, 900, 305, panel);
        text(app.large, "Дія з файлом", 224, 230, 828, white);
        text(app.small, app.draft_transfer.name, 224, 284, 828, gray);
        row(0, app.download_choice, 225, 324, 828, app.draft_transfer.unpack ? "Розпакувати" : "Завантажити");
        row(1, app.download_choice, 225, 384, 828, app.draft_transfer.unpack ? "Розпакувати й встановити" : "Завантажити й встановити");
        text(app.small, "A  Обрати     B  Назад", 224, 454, 828, gray);
    } else if (app.confirm_install) {
        rect(190, 200, 900, 305, selected);
        text(app.large, "Відкрити встановлювач DBI?", 224, 230, 828, white);
        text(app.normal, app.saved[app.saved_index].file, 224, 298, 828, white);
        char dbi_guide[148];
        snprintf(dbi_guide, sizeof(dbi_guide), "%s%s", tr("У DBI відкрий Browse SD Card → "), NXLOAD_DOWNLOAD_RELATIVE);
        text(app.small, dbi_guide, 224, 355, 828, gray);
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
        !strncmp(self, "sdmc:/", 6) && !strchr(self, '"') && !strstr(self, "/../") &&
        !strcasecmp(self + self_len - 4, ".nro"))
        strcpy(app.update_target, self);
    else if (self_len < sizeof(app.update_target) - 5 && self_len >= 5 && self[0] == '/' &&
             !strchr(self, '"') && !strstr(self, "/../") && !strcasecmp(self + self_len - 4, ".nro"))
        snprintf(app.update_target, sizeof(app.update_target), "sdmc:%s", self);
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
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
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
        apply_theme();
        set_english(app.settings.english);
        load_favorites();
        load_catalog_metadata();
        open_catalogs();
        start_update_check(false);
        AppletHookCookie lifecycle_hook;
        appletHook(&lifecycle_hook, app_lifecycle_hook, NULL);
        keep_console_awake(appletGetFocusState() == AppletFocusState_InFocus);
        uint64_t next_frame = 0, next_scroll = 0;
        u64 held = 0;
        while (appletMainLoop() && !app.close) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {}
            padUpdate(&pad);
            finish_jobs();
            if (app.close) break;
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
            request_game_files();
            pump_file_sizes();
            pump_index();
            pump_gallery();
            pump_trailers();
            bool cover_finished = pump_covers();
            if (app.trailer_open || buttons || touch_changed || cover_finished || now >= next_frame) {
                draw();
                next_frame = now + 150;
            }
            uint64_t elapsed = SDL_GetTicks64() - now;
            if (elapsed < 16) SDL_Delay((uint32_t)(16 - elapsed));
        }
        wake_download_screen();
        keep_console_awake(false);
        appletUnhook(&lifecycle_hook);
        if (app.backlight == BACKLIGHT_LBL) lblExit();
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
    if (app.update_checking) atomic_store(&app.update_check.cancelled, true);
    if (app.index_busy) atomic_store(&app.index_job.cancel, true);
    if (app.gallery_busy) atomic_store(&app.gallery_job.cancel, true);
    if (app.trailer_busy) atomic_store(&app.trailer_job.cancel, true);
    trailer_shutdown();
    if (app.size_job.busy) {
        atomic_store(&app.size_job.cancel, true);
        threadWaitForExit(&app.size_job.thread);
        threadClose(&app.size_job.thread);
    }
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source)
        if (app.catalog_busy[source]) atomic_store(&app.pending[source].cancel, true);
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
        if (!app.catalog_busy[source]) continue;
        threadWaitForExit(&app.catalog_thread[source]);
        threadClose(&app.catalog_thread[source]);
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
    if (app.update_checking) {
        threadWaitForExit(&app.update_thread);
        threadClose(&app.update_thread);
    }
    if (app.index_busy) {
        threadWaitForExit(&app.index_thread);
        threadClose(&app.index_thread);
    }
    if (app.gallery_busy) {
        threadWaitForExit(&app.gallery_thread);
        threadClose(&app.gallery_thread);
    }
    if (app.trailer_busy) {
        threadWaitForExit(&app.trailer_thread);
        threadClose(&app.trailer_thread);
    }
    for (size_t i = 0; i < COVER_SLOTS; ++i) SDL_DestroyTexture(app.covers[i].texture);
    for (size_t i = 0; i < TEXT_SLOTS; ++i)
        if (text_cache[i].texture) SDL_DestroyTexture(text_cache[i].texture);
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
    free(app.favorites);
    if (romfs_ready) romfsExit();
    if (app.update_ready) {
        bool installed = apply_update(app.update_target, app.staged_update_sha);
        char arguments[sizeof(app.update_target) + 2];
        snprintf(arguments, sizeof(arguments), "\"%s\"", app.update_target);
        if (!installed || (envHasNextLoad() && R_FAILED(envSetNextLoad(app.update_target, arguments)))) {
            consoleInit(NULL);
            puts(tr(installed ? "Оновлення встановлено. Відкрий цей файл у Homebrew Menu:" :
                               "Не вдалося встановити оновлення NXLoad. Перевір файл і його резервну копію:"));
            puts(app.update_target);
            puts(tr("Натисни +, щоб вийти."));
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
