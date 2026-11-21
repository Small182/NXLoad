#include "trailers.h"

#include <curl/curl.h>
#include <inttypes.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/stream_cb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TRAILER_HOST "assets.nintendo.com/video/upload/"
#define TRAILER_480 "c_limit,w_854,h_480,vc_h264/"
#define TRAILER_SCHEME "nxtrailer"
#define STREAM_BLOCK (512 * 1024)
#define STREAM_LIMIT (256 * 1024 * 1024)
#define FRAME_WIDTH 854
#define FRAME_HEIGHT 480
#define FRAME_PITCH 3456

typedef struct {
    CURL *connection;
    atomic_bool cancelled;
    char url[CATALOG_URL];
    uint64_t offset, length, block_start, first, last, total;
    size_t used;
    char data[STREAM_BLOCK];
} VideoStream;

static size_t video_header(char *line, size_t size, size_t count, void *arg) {
    VideoStream *stream = arg;
    size_t bytes = size * count;
    if (bytes >= 5 && !memcmp(line, "HTTP/", 5)) stream->total = 0;
    if (bytes < 14 || bytes >= 96 || strncasecmp(line, "Content-Range:", 14)) return bytes;
    char header[96];
    memcpy(header, line + 14, bytes - 14);
    header[bytes - 14] = 0;
    if (sscanf(header, " bytes %" SCNu64 "-%" SCNu64 "/%" SCNu64,
               &stream->first, &stream->last, &stream->total) != 3) stream->total = 0;
    return bytes;
}

static size_t video_body(char *data, size_t size, size_t count, void *arg) {
    VideoStream *stream = arg;
    size_t bytes = size * count;
    if (!stream->total || bytes > sizeof(stream->data) - stream->used) return 0;
    memcpy(stream->data + stream->used, data, bytes);
    stream->used += bytes;
    return bytes;
}

static int video_progress(void *arg, curl_off_t total, curl_off_t now, curl_off_t up, curl_off_t sent) {
    (void)total; (void)now; (void)up; (void)sent;
    return atomic_load(&((VideoStream *)arg)->cancelled);
}

static bool fetch_video_block(VideoStream *stream, uint64_t offset) {
    if (atomic_load(&stream->cancelled)) return false;
    uint64_t end = offset + sizeof(stream->data) - 1;
    if (stream->length && end >= stream->length) end = stream->length - 1;
    char range[48];
    snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64, offset, end);
    stream->used = 0;
    stream->total = 0;
    curl_easy_setopt(stream->connection, CURLOPT_URL, stream->url);
    curl_easy_setopt(stream->connection, CURLOPT_RANGE, range);
    CURLcode result = curl_easy_perform(stream->connection);
    long http = 0;
    curl_easy_getinfo(stream->connection, CURLINFO_RESPONSE_CODE, &http);
    if (result != CURLE_OK || http != 206 || !stream->total || stream->total > STREAM_LIMIT ||
        (stream->length && stream->length != stream->total) || stream->first != offset ||
        stream->last != (end < stream->total ? end : stream->total - 1) ||
        stream->last < offset || stream->used != stream->last - offset + 1) {
        stream->used = 0;
        return false;
    }
    stream->block_start = offset;
    stream->length = stream->total;
    return true;
}

static int64_t video_read(void *arg, char *data, uint64_t count) {
    VideoStream *stream = arg;
    if (atomic_load(&stream->cancelled)) return -1;
    if (!count || stream->offset == stream->length) return 0;
    if ((stream->offset < stream->block_start ||
         stream->offset - stream->block_start >= stream->used) &&
        !fetch_video_block(stream, stream->offset)) return -1;
    size_t start = (size_t)(stream->offset - stream->block_start);
    size_t remaining = stream->used - start;
    if (count < remaining) remaining = (size_t)count;
    memcpy(data, stream->data + start, remaining);
    stream->offset += remaining;
    return (int64_t)remaining;
}

static int64_t video_seek(void *arg, int64_t offset) {
    VideoStream *stream = arg;
    if (offset < 0 || (uint64_t)offset > stream->length || atomic_load(&stream->cancelled))
        return MPV_ERROR_GENERIC;
    stream->offset = (uint64_t)offset;
    return offset;
}

static int64_t video_size(void *arg) {
    return (int64_t)((VideoStream *)arg)->length;
}

static void video_close(void *arg) {
    VideoStream *stream = arg;
    curl_easy_cleanup(stream->connection);
    free(stream);
}

static void video_cancel(void *arg) {
    atomic_store(&((VideoStream *)arg)->cancelled, true);
}

static int video_open(void *unused, char *uri, mpv_stream_cb_info *info) {
    (void)unused;
    static const char prefix[] = TRAILER_SCHEME "://" TRAILER_HOST TRAILER_480;
    if (strncmp(uri, prefix, sizeof(prefix) - 1)) return MPV_ERROR_LOADING_FAILED;
    const char *path = uri + sizeof(prefix) - 1;
    size_t length = strlen(path);
    if (length < 5 || strcmp(path + length - 4, ".mp4") ||
        strspn(path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/") != length - 4)
        return MPV_ERROR_LOADING_FAILED;

    VideoStream *stream = calloc(1, sizeof(*stream));
    if (!stream) return MPV_ERROR_NOMEM;
    atomic_init(&stream->cancelled, false);
    stream->connection = curl_easy_init();
    if (!stream->connection) {
        free(stream);
        return MPV_ERROR_NOMEM;
    }
    CURL *curl = stream->connection;
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    // Switch networks may fail to reach the IPv6 addresses for Nintendo CDN.
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NXLoad");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, video_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, stream);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, video_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, stream);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, video_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, stream);

    int n = snprintf(stream->url, sizeof(stream->url), "https://" TRAILER_HOST TRAILER_480 "%s", path);
    if (n < 0 || (size_t)n >= sizeof(stream->url)) {
        video_close(stream);
        return MPV_ERROR_LOADING_FAILED;
    }
    if (!fetch_video_block(stream, 0)) {
        // The original asset can support seeking before a resized rendition is cached.
        snprintf(stream->url, sizeof(stream->url), "https://" TRAILER_HOST "%s", path);
        if (!fetch_video_block(stream, 0)) {
            video_close(stream);
            return MPV_ERROR_LOADING_FAILED;
        }
    }

    *info = (mpv_stream_cb_info){
        .cookie = stream, .read_fn = video_read, .seek_fn = video_seek,
        .size_fn = video_size, .close_fn = video_close, .cancel_fn = video_cancel
    };
    return 0;
}

static mpv_handle *player;
static mpv_render_context *context;
static SDL_Texture *texture;
static void *pixels;
static TrailerState state;
static bool frame_ready, file_loaded;
static uint64_t load_request;
static int64_t entry_id;

void trailer_shutdown(void) {
    if (context) mpv_render_context_free(context);
    if (player) mpv_terminate_destroy(player);
    SDL_DestroyTexture(texture);
    free(pixels);
    context = NULL;
    player = NULL;
    texture = NULL;
    pixels = NULL;
}

static bool prepare_player(SDL_Renderer *renderer) {
    if (player) return true;
    player = mpv_create();
    if (!player) return false;
    mpv_set_option_string(player, "config", "no");
    mpv_set_option_string(player, "load-scripts", "no");
    mpv_set_option_string(player, "ytdl", "no");
    mpv_set_option_string(player, "terminal", "no");
    mpv_set_option_string(player, "vo", "libmpv");
    mpv_set_option_string(player, "hwdec", "no");
    mpv_set_option_string(player, "ao", "sdl");
    mpv_set_option_string(player, "demuxer", "lavf");
    mpv_set_option_string(player, "demuxer-lavf-format", "mov");
    mpv_set_option_string(player, "demuxer-max-bytes", "4MiB");
    mpv_set_option_string(player, "demuxer-max-back-bytes", "2MiB");
    mpv_render_param init[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW}, {0}
    };
    if (mpv_initialize(player) >= 0 &&
        mpv_stream_cb_add_ro(player, TRAILER_SCHEME, NULL, video_open) >= 0 &&
        mpv_render_context_create(&context, player, init) >= 0) {
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING,
                                    FRAME_WIDTH, FRAME_HEIGHT);
        pixels = aligned_alloc(64, FRAME_PITCH * FRAME_HEIGHT);
        if (texture && pixels) return true;
    }
    trailer_shutdown();
    return false;
}

bool trailer_play(SDL_Renderer *renderer, const char *url) {
    if (strncmp(url, "https://", 8) || !prepare_player(renderer)) return false;
    char uri[CATALOG_URL + 16];
    snprintf(uri, sizeof(uri), TRAILER_SCHEME "://%s", url + 8);
    const char *args[] = {"loadfile", uri, "replace", NULL};
    while (mpv_wait_event(player, 0)->event_id != MPV_EVENT_NONE) {}
    const char *resume[] = {"set", "pause", "no", NULL};
    mpv_command_async(player, 0, resume);
    frame_ready = file_loaded = false;
    entry_id = -1;
    state = TRAILER_LOADING;
    if (mpv_command_async(player, ++load_request, args) >= 0) return true;
    state = TRAILER_ERROR;
    return false;
}

void trailer_stop(void) {
    if (!player) return;
    const char *args[] = {"stop", NULL};
    mpv_command_async(player, 0, args);
}

void trailer_pause(void) {
    if (!player || (state != TRAILER_PLAYING && state != TRAILER_PAUSED)) return;
    const char *args[] = {"set", "pause", state == TRAILER_PLAYING ? "yes" : "no", NULL};
    if (mpv_command_async(player, 0, args) >= 0)
        state = state == TRAILER_PLAYING ? TRAILER_PAUSED : TRAILER_PLAYING;
}

void trailer_seek(bool forward) {
    if (!player || (state != TRAILER_PLAYING && state != TRAILER_PAUSED)) return;
    const char *args[] = {"seek", forward ? "10" : "-10", "relative", NULL};
    mpv_command_async(player, 0, args);
}

TrailerState trailer_draw(SDL_Renderer *renderer) {
    if (!player) return TRAILER_ERROR;
    for (mpv_event *event; (event = mpv_wait_event(player, 0))->event_id != MPV_EVENT_NONE;) {
        if (event->event_id == MPV_EVENT_START_FILE) {
            entry_id = ((mpv_event_start_file *)event->data)->playlist_entry_id;
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            file_loaded = true;
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *end = event->data;
            if (end->playlist_entry_id != entry_id) continue;
            if (end->reason == MPV_END_FILE_REASON_EOF) state = TRAILER_ENDED;
            else if (end->reason == MPV_END_FILE_REASON_ERROR) state = TRAILER_ERROR;
        } else if (event->event_id == MPV_EVENT_COMMAND_REPLY &&
                   event->reply_userdata == load_request && event->error < 0) {
            state = TRAILER_ERROR;
        }
    }
    uint64_t update = mpv_render_context_update(context);
    if (file_loaded && (state == TRAILER_LOADING || state == TRAILER_PLAYING || state == TRAILER_PAUSED) &&
        (!frame_ready || (update & MPV_RENDER_UPDATE_FRAME))) {
        int size[] = {FRAME_WIDTH, FRAME_HEIGHT};
        size_t pitch = FRAME_PITCH;
        int block = 0;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_SW_SIZE, size}, {MPV_RENDER_PARAM_SW_FORMAT, "bgr0"},
            {MPV_RENDER_PARAM_SW_STRIDE, &pitch}, {MPV_RENDER_PARAM_SW_POINTER, pixels},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block}, {0}
        };
        if (mpv_render_context_render(context, params) >= 0 &&
            SDL_UpdateTexture(texture, NULL, pixels, FRAME_PITCH) == 0) {
            frame_ready = true;
            if (state == TRAILER_LOADING) state = TRAILER_PLAYING;
        } else {
            state = TRAILER_ERROR;
            trailer_stop();
        }
    }
    if (frame_ready) {
        SDL_Rect dst = {0, 0, 1280, 720};
        SDL_RenderCopy(renderer, texture, NULL, &dst);
    }
    return state;
}
