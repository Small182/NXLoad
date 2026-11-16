#define _GNU_SOURCE
#define SDL_MAIN_HANDLED
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog.h"

#define ROWS 9
#define FILE_ROWS 5
#define HISTORY_LIMIT 96

typedef enum { LIBRARY, GAME, DOWNLOADS } Screen;

typedef struct {
    char file[256];
} SavedFile;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *cover;
    TTF_Font *small;
    TTF_Font *normal;
    TTF_Font *large;
    Screen screen, previous;
    Game games[CATALOG_SEARCH_LIMIT];
    size_t game_count, game_index, file_index, saved_count, saved_index;
    unsigned page;
    bool more, confirm_install, close, catalog_busy, download_busy, download_finished;
    char query[96], notice[192], game_error[192], file_id[DLARCHIVE_ID_CAPACITY];
    char transfer_name[CATALOG_TEXT];
    SavedFile saved[HISTORY_LIMIT];
    GameDetails details;
    CatalogJob pending;
    FichierJob download;
    Thread catalog_thread, download_thread;
    uint64_t previous_bytes;
    double speed;
    uint64_t speed_time;
} App;

static App app;
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
    if (current && (buttons & (HidNpadButton_Up | HidNpadButton_StickLUp))) --current;
    if (current + 1 < count && (buttons & (HidNpadButton_Down | HidNpadButton_StickLDown))) ++current;
    return current;
}

static void catalog_worker(void *arg) { catalog_request(arg); }
static void download_worker(void *arg) { fichier_download(arg); }

static bool start_worker(Thread *thread, ThreadFunc function, void *arg) {
    Result rc = threadCreate(thread, function, arg, NULL, 128 * 1024, 0x2c, -2);
    if (R_FAILED(rc)) return false;
    rc = threadStart(thread);
    if (R_FAILED(rc)) threadClose(thread);
    return R_SUCCEEDED(rc);
}

static void start_catalog(CatalogRequest request, unsigned page) {
    if (app.catalog_busy) return;
    CatalogJob *job = &app.pending;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->cancel, false);
    atomic_init(&job->searching_page, 0);
    job->request = request;
    job->page = page;
    memcpy(job->query, app.query, sizeof(job->query));
    if (request == CATALOG_DETAILS) job->game = app.games[app.game_index];
    app.notice[0] = 0;
    app.catalog_busy = start_worker(&app.catalog_thread, catalog_worker, job);
    if (!app.catalog_busy) strcpy(app.notice, "Не вдалося запустити запит");
}

static void update_cover(void) {
    SDL_DestroyTexture(app.cover);
    app.cover = NULL;
    if (app.details.cover && app.details.cover_bytes) {
        SDL_RWops *src = SDL_RWFromConstMem(app.details.cover, (int)app.details.cover_bytes);
        if (src) app.cover = IMG_LoadTexture_RW(app.renderer, src, 1);
    }
    free(app.details.cover);
    app.details.cover = NULL;
    app.details.cover_bytes = 0;
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

static void finish_jobs(void) {
    if (app.catalog_busy && R_SUCCEEDED(waitSingle(waiterForThread(&app.catalog_thread), 0))) {
        threadWaitForExit(&app.catalog_thread);
        threadClose(&app.catalog_thread);
        app.catalog_busy = false;
        CatalogJob *job = &app.pending;
        bool cancelled = atomic_load(&job->cancel);
        if (job->request == CATALOG_DETAILS) {
            if (!cancelled) {
                app.details = job->details;
                strcpy(app.game_error, job->error);
                update_cover();
            } else {
                free(job->details.cover);
            }
            job->details.cover = NULL;
        } else if (!cancelled) {
            if (job->count || job->request == CATALOG_SEARCH || !job->error[0]) {
                memcpy(app.games, job->games, job->count * sizeof(Game));
                app.game_count = job->count;
                app.game_index = 0;
                app.page = job->page;
                app.more = job->more;
            }
            strcpy(app.notice, job->error);
        }
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
        app.speed = (bytes - app.previous_bytes) * 1000.0 / (now - app.speed_time);
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
    if (app.catalog_busy) {
        if (buttons & HidNpadButton_B) {
            atomic_store(&app.pending.cancel, true);
            if (app.screen == GAME) app.screen = LIBRARY;
        }
        return;
    }
    if (app.screen == GAME) {
        app.file_index = move(app.file_index, app.details.count, buttons);
        if (buttons & HidNpadButton_B) { app.notice[0] = 0; app.screen = LIBRARY; }
        if ((buttons & HidNpadButton_A) && app.details.count)
            start_download(&app.details.files[app.file_index]);
        return;
    }
    app.game_index = move(app.game_index, app.game_count, buttons);
    if ((buttons & HidNpadButton_X) && keyboard()) {
        start_catalog(CATALOG_SEARCH, 1);
    } else if ((buttons & HidNpadButton_B) && app.query[0]) {
        app.query[0] = 0;
        start_catalog(CATALOG_PAGE, CATALOG_DEFAULT_PAGE);
    } else if (!app.query[0] && (buttons & HidNpadButton_L) && app.page > 1) {
        start_catalog(CATALOG_PAGE, app.page - 1);
    } else if (!app.query[0] && (buttons & HidNpadButton_R) && app.more) {
        start_catalog(CATALOG_PAGE, app.page + 1);
    } else if (buttons & HidNpadButton_A) {
        if (!app.game_count) {
            start_catalog(app.query[0] ? CATALOG_SEARCH : CATALOG_PAGE, app.page);
        } else {
            memset(&app.details, 0, sizeof(app.details));
            SDL_DestroyTexture(app.cover);
            app.cover = NULL;
            app.game_error[0] = 0;
            app.file_index = 0;
            app.screen = GAME;
            start_catalog(CATALOG_DETAILS, app.page);
        }
    }
}

static void draw_library(void) {
    rect(40, 106, 500, 530, panel);
    rect(556, 106, 684, 530, panel);
    char header[160];
    snprintf(header, sizeof(header), app.query[0] ? "Пошук · %s" : "Каталог NSRoms", app.query);
    text(app.normal, header, 60, 125, 450, white);
    if (!app.query[0]) {
        snprintf(header, sizeof(header), "Сторінка %u", app.page);
        text(app.small, header, 60, 603, 420, gray);
    }
    size_t first = first_visible(app.game_index, ROWS);
    for (size_t i = first; i < app.game_count && i < first + ROWS; ++i)
        row(i, app.game_index, 55, 170 + (int)(i - first) * 46, 470, app.games[i].title);
    if (app.catalog_busy) {
        unsigned page = atomic_load(&app.pending.searching_page);
        if (page) snprintf(header, sizeof(header), "Пошук на сторінці %u…", page);
        else strcpy(header, "Завантаження каталогу…");
        text(app.normal, header, 588, 152, 604, blue);
    } else if (app.game_count) {
        text(app.large, app.games[app.game_index].title, 588, 150, 610, white);
        text(app.normal, "A  Відкрити картку гри", 588, 216, 604, blue);
    } else {
        text(app.normal, app.query[0] ? "Ігор за цим запитом не знайдено" : "Каталог порожній",
             588, 152, 610, gray);
    }
    text(app.small, app.notice, 588, 595, 610, gray);
}

static void draw_cover(void) {
    rect(56, 118, 308, 420, panel);
    if (!app.cover) return;
    int w, h;
    if (SDL_QueryTexture(app.cover, NULL, NULL, &w, &h) || w <= 0 || h <= 0) return;
    double scale = SDL_min(308.0 / w, 420.0 / h);
    SDL_Rect dst = {56 + (308 - (int)(w * scale)) / 2, 118 + (420 - (int)(h * scale)) / 2,
                    (int)(w * scale), (int)(h * scale)};
    SDL_RenderCopy(app.renderer, app.cover, NULL, &dst);
}

static void draw_game(void) {
    rect(40, 106, 345, 530, panel);
    rect(401, 106, 839, 530, panel);
    draw_cover();
    const char *title = app.details.title[0] ? app.details.title : app.games[app.game_index].title;
    text(app.large, title, 430, 130, 772, white);
    text(app.small, app.details.info, 430, 187, 772, gray);
    text(app.normal, "Файли DLArchive", 430, 224, 770, blue);
    if (app.catalog_busy) {
        text(app.normal, "Завантаження картки…", 430, 275, 770, gray);
    } else if (app.game_error[0] || !app.details.count) {
        text(app.normal, app.game_error[0] ? app.game_error : "Для цієї гри немає файлів DLArchive",
             430, 275, 770, gray);
    } else {
        size_t first = first_visible(app.file_index, FILE_ROWS);
        for (size_t i = first; i < app.details.count && i < first + FILE_ROWS; ++i)
            row(i, app.file_index, 429, 267 + (int)(i - first) * 48, 782, app.details.files[i].title);
    }
    text(app.small, app.notice, 430, 595, 772, gray);
}

static void draw_downloads(void) {
    rect(40, 106, 1200, 530, panel);
    text(app.large, "Завантаження", 66, 128, 1100, white);
    if (app.download_busy || app.download_finished) {
        uint64_t have = atomic_load(&app.download.received), total = atomic_load(&app.download.total);
        double percent = total ? SDL_min(100.0, 100.0 * have / total) : 0;
        char line[192];
        text(app.normal, app.transfer_name, 66, 182, 1100, white);
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
            snprintf(line, sizeof(line), "Збережено на microSD  ·  %.1f МіБ", have / 1048576.0);
        } else {
            snprintf(line, sizeof(line), "%s  ·  API HTTP %ld  ·  Файл HTTP %ld",
                     app.download.result, app.download.api_http, app.download.http);
        }
        text(app.small, line, 66, 249, 1148, blue);
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
    if (app.download_busy)
        text(app.small, "Завантаження триває", 950, 40, 290, blue);
    if (app.screen == LIBRARY) draw_library();
    else if (app.screen == GAME) draw_game();
    else draw_downloads();
    const char *footer = app.screen == LIBRARY ?
        "A  Відкрити     X  Пошук     L / R  Сторінки     Y  Завантаження     +  Вихід" :
        app.screen == GAME ? "A  Завантажити     B  Каталог     Y  Завантаження     +  Вихід" :
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

int main(void) {
    Result sockets = socketInitializeDefault();
    bool curl_ready = R_SUCCEEDED(sockets) && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
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
        app.page = CATALOG_DEFAULT_PAGE;
        start_catalog(CATALOG_PAGE, app.page);
        uint64_t next_frame = 0;
        while (appletMainLoop() && !app.close) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {}
            padUpdate(&pad);
            finish_jobs();
            u64 buttons = padGetButtonsDown(&pad);
            input(buttons);
            uint64_t now = SDL_GetTicks64();
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
    SDL_DestroyTexture(app.cover);
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
    if (R_SUCCEEDED(sockets)) socketExit();
    return 0;
}
