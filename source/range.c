#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "range.h"

#include <curl/curl.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MIB (UINT64_C(1024) * 1024)
#define MIN_PARALLEL_SIZE (32 * MIB)
#define RANGE_WRITE_BUFFER (1024 * 1024)
#define RANGE_RECEIVE_BUFFER (256 * 1024)
#define MAX_CONNECTIONS 4
#define MAX_RANGE_RETRIES 2
#define WRITE_QUEUE_SIZE 8
#define RANGE_RESUME_MAGIC "NXLRNG02"
#define RANGE_MAX_CHUNKS 262144
typedef struct {
    char magic[8];
    uint64_t length, block_size;
    char etag[160];
    uint32_t chunks;
} RangeResumeHeader;
static bool bit_set(const unsigned char *bits, size_t index) {
    return (bits[index / 8] & (1u << (index % 8))) != 0;
}

typedef struct {
    unsigned char *data;
    uint64_t offset;
    size_t length;
} WriteBlock;

typedef struct {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t ready, space;
    int fd;
    unsigned head, count;
    bool active, closing, discard, failed;
    WriteBlock blocks[WRITE_QUEUE_SIZE];
} RangeWriter;

typedef struct {
    long status;
    uint64_t first, last, total;
    bool valid_range, encoded, has_etag, textual;
    char etag[sizeof(((RangeInfo *)0)->etag)];
    char disposition[sizeof(((RangeInfo *)0)->disposition)];
} RangeResponse;

typedef struct {
    CURL *curl;
    bool active, write_failed;
    RangeWriter *writer;
    FichierJob *job;
    const RangeInfo *info;
    RangeResponse response;
    uint64_t first, last, request_first, received, written;
    unsigned retries;
    unsigned char *buffer;
    size_t buffered;
} RangePart;

static size_t range_header(char *data, size_t size, size_t count, void *ptr) {
    RangeResponse *response = ptr;
    size_t n = size * count;
    char line[512];
    if (!n || n >= sizeof(line)) return n;
    memcpy(line, data, n);
    line[n] = '\0';
    line[strcspn(line, "\r\n")] = '\0';
    if (!strncmp(line, "HTTP/", 5)) {
        memset(response, 0, sizeof(*response));
        const char *status = strchr(line, ' ');
        if (status) response->status = strtol(status + 1, NULL, 10);
    } else if (!strncasecmp(line, "Content-Range:", 14)) {
        char extra;
        response->valid_range = sscanf(line + 14, " bytes %" SCNu64 "-%" SCNu64 "/%" SCNu64 " %c",
            &response->first, &response->last, &response->total, &extra) == 3 &&
            response->first <= response->last && response->last < response->total &&
            response->total <= INT64_MAX;
    } else if (!strncasecmp(line, "ETag:", 5)) {
        const char *value = line + 5 + strspn(line + 5, " \t");
        size_t length = strlen(value);
        while (length && (value[length - 1] == ' ' || value[length - 1] == '\t')) --length;
        response->has_etag = true;
        if (length > 2 && length < sizeof(response->etag) &&
            value[0] == '"' && value[length - 1] == '"') {
            size_t i = 1;
            while (i + 1 < length && (unsigned char)value[i] >= 0x21 &&
                   value[i] != '"' && value[i] != 0x7f) ++i;
            if (i + 1 != length) return n;
            memcpy(response->etag, value, length);
            response->etag[length] = 0;
        }
    } else if (!strncasecmp(line, "Content-Encoding:", 17)) {
        const char *value = line + 17 + strspn(line + 17, " \t");
        response->encoded = strcasecmp(value, "identity") != 0;
    } else if (!strncasecmp(line, "Content-Type:", 13)) {
        const char *value = line + 13 + strspn(line + 13, " \t");
        response->textual = !strncasecmp(value, "text/", 5) ||
            !strncasecmp(value, "application/json", 16) ||
            !strncasecmp(value, "application/xhtml+xml", 21);
    } else if (!strncasecmp(line, "Content-Disposition:", 20)) {
        const char *value = line + 20 + strspn(line + 20, " \t");
        if (strlen(value) < sizeof(response->disposition)) strcpy(response->disposition, value);
    }
    return n;
}

static bool has_range(const RangeResponse *response, uint64_t first, uint64_t last) {
    return response->status == 206 && response->valid_range && !response->encoded && !response->textual &&
        response->first == first && response->last == last;
}

static int range_progress(void *arg, curl_off_t total, curl_off_t now, curl_off_t up, curl_off_t sent) {
    (void)total;
    (void)now;
    (void)up;
    (void)sent;
    return atomic_load(&((FichierJob *)arg)->cancelled);
}

static void configure(CURL *curl, const RangeSource *source, FichierJob *job, RangeResponse *response) {
    curl_easy_setopt(curl, CURLOPT_URL, source->url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_CAINFO, source->ca_path);
    curl_easy_setopt(curl, CURLOPT_REFERER, source->referer);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NXLoad/1.0.5");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, range_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, range_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, job);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}

typedef struct {
    RangeResponse response;
    size_t count;
} RangeProbe;

static size_t probe_body(char *data, size_t size, size_t count, void *arg) {
    (void)data;
    RangeProbe *probe = arg;
    size_t n = size * count;
    if (probe->count || n != 1 || !has_range(&probe->response, 0, 0)) return 0;
    probe->count = n;
    return n;
}

bool range_probe(const RangeSource *source, FichierJob *job, RangeInfo *info) {
    memset(info, 0, sizeof(*info));
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    RangeProbe probe = {0};
    configure(curl, source, job, &probe.response);
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, probe_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &probe);
    CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    info->status = probe.response.status;
    // Some download CDNs support byte ranges but omit ETag.
    if (code != CURLE_OK || probe.count != 1 ||
        probe.response.total < MIN_PARALLEL_SIZE) return false;
    info->length = probe.response.total;
    strcpy(info->etag, probe.response.etag);
    strcpy(info->disposition, probe.response.disposition);
    return true;
}

static unsigned char *write_buffer(void) {
    return aligned_alloc(4096, RANGE_WRITE_BUFFER);
}

static bool write_block(int fd, const WriteBlock *block) {
    off_t offset = (off_t)block->offset;
    if ((uint64_t)offset != block->offset || lseek(fd, offset, SEEK_SET) != offset) return false;
    size_t done = 0;
    while (done < block->length) {
        ssize_t n = write(fd, block->data + done, block->length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

static void *write_worker(void *arg) {
    RangeWriter *writer = arg;
    pthread_mutex_lock(&writer->lock);
    while (!writer->discard) {
        if (!writer->count) {
            if (writer->closing) break;
            pthread_cond_wait(&writer->ready, &writer->lock);
            continue;
        }
        WriteBlock block = writer->blocks[writer->head];
        pthread_mutex_unlock(&writer->lock);
        bool ok = write_block(writer->fd, &block);
        pthread_mutex_lock(&writer->lock);
        if (!ok) {
            writer->failed = true;
            pthread_cond_broadcast(&writer->space);
            break;
        }
        writer->head = (writer->head + 1) % WRITE_QUEUE_SIZE;
        --writer->count;
        pthread_cond_signal(&writer->space);
    }
    pthread_mutex_unlock(&writer->lock);
    return NULL;
}

static void free_write_queue(RangeWriter *writer) {
    for (unsigned i = 0; i < WRITE_QUEUE_SIZE; ++i) {
        free(writer->blocks[i].data);
        writer->blocks[i].data = NULL;
    }
}

static void start_writer(RangeWriter *writer) {
    for (unsigned i = 0; i < WRITE_QUEUE_SIZE; ++i) {
        writer->blocks[i].data = write_buffer();
        if (!writer->blocks[i].data) {
            free_write_queue(writer);
            return;
        }
    }
    writer->active = pthread_create(&writer->thread, NULL, write_worker, writer) == 0;
    if (!writer->active) free_write_queue(writer);
}

static bool finish_writer(RangeWriter *writer, bool commit) {
    if (writer->active) {
        pthread_mutex_lock(&writer->lock);
        writer->closing = true;
        writer->discard = !commit;
        pthread_cond_signal(&writer->ready);
        pthread_mutex_unlock(&writer->lock);
        pthread_join(writer->thread, NULL);
    }
    free_write_queue(writer);
    pthread_cond_destroy(&writer->space);
    pthread_cond_destroy(&writer->ready);
    pthread_mutex_destroy(&writer->lock);
    return !writer->failed;
}

static bool flush_part(RangePart *part) {
    if (!part->buffered) return true;
    RangeWriter *writer = part->writer;
    WriteBlock block = {part->buffer, part->first + part->written, part->buffered};
    if (!writer->active) {
        if (!write_block(writer->fd, &block)) {
            part->write_failed = true;
            return false;
        }
    } else {
        pthread_mutex_lock(&writer->lock);
        while (writer->count == WRITE_QUEUE_SIZE && !writer->failed &&
               !atomic_load(&part->job->cancelled))
            pthread_cond_wait(&writer->space, &writer->lock);
        if (writer->failed || atomic_load(&part->job->cancelled)) {
            part->write_failed = writer->failed;
            pthread_mutex_unlock(&writer->lock);
            return false;
        }
        unsigned tail = (writer->head + writer->count) % WRITE_QUEUE_SIZE;
        // Swap buffers so the network thread can resume without copying this block.
        part->buffer = writer->blocks[tail].data;
        writer->blocks[tail] = block;
        ++writer->count;
        pthread_cond_signal(&writer->ready);
        pthread_mutex_unlock(&writer->lock);
    }
    part->written += block.length;
    part->buffered = 0;
    return true;
}

static size_t part_body(char *data, size_t size, size_t count, void *arg) {
    RangePart *part = arg;
    size_t total = size * count;
    if (atomic_load(&part->job->cancelled) ||
        !has_range(&part->response, part->request_first, part->last) ||
        part->response.total != part->info->length ||
        (part->info->etag[0] && part->response.has_etag &&
         strcmp(part->response.etag, part->info->etag)) ||
        total > part->last - part->first + 1 - part->received) return 0;
    size_t copied = 0;
    while (copied < total) {
        size_t n = RANGE_WRITE_BUFFER - part->buffered;
        if (n > total - copied) n = total - copied;
        memcpy(part->buffer + part->buffered, data + copied, n);
        part->buffered += n;
        copied += n;
        if (part->buffered == RANGE_WRITE_BUFFER && !flush_part(part)) return 0;
    }
    part->received += total;
    atomic_fetch_add(&part->job->received, total);
    return total;
}

static bool request_remaining(CURLM *multi, RangePart *part) {
    part->request_first = part->first + part->received;
    part->write_failed = false;
    memset(&part->response, 0, sizeof(part->response));
    char range[48];
    snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64, part->request_first, part->last);
    if (curl_easy_setopt(part->curl, CURLOPT_RANGE, range) != CURLE_OK) return false;
    part->active = curl_multi_add_handle(multi, part->curl) == CURLM_OK;
    return part->active;
}

static bool schedule_part(CURLM *multi, RangePart *part, uint64_t *next, uint64_t block_size) {
    part->first = *next;
    uint64_t length = part->info->length - *next;
    if (length > block_size) length = block_size;
    *next += length;
    part->last = *next - 1;
    part->received = part->written = 0;
    part->buffered = 0;
    part->retries = 0;
    return request_remaining(multi, part);
}

static bool retryable_range_error(CURLcode code) {
    return code == CURLE_PARTIAL_FILE || code == CURLE_RECV_ERROR ||
        code == CURLE_SEND_ERROR || code == CURLE_OPERATION_TIMEDOUT ||
        code == CURLE_COULDNT_CONNECT || code == CURLE_GOT_NOTHING;
}


/* Journal a completed chunk only after its bytes are durably on microSD. */
static bool checkpoint_chunk(RangeWriter *writer, FILE *state, unsigned char *bits,
                             size_t chunk) {
    if (writer->active) {
        pthread_mutex_lock(&writer->lock);
        while (writer->count && !writer->failed)
            pthread_cond_wait(&writer->space, &writer->lock);
        bool failed = writer->failed;
        pthread_mutex_unlock(&writer->lock);
        if (failed) return false;
    }
    if (fsync(writer->fd)) return false;
    unsigned char value = bits[chunk / 8] | (1u << (chunk % 8));
    long offset = (long)sizeof(RangeResumeHeader) + (long)(chunk / 8);
    if (fseek(state, offset, SEEK_SET) || fwrite(&value, 1, 1, state) != 1 ||
        fflush(state) || fsync(fileno(state))) return false;
    bits[chunk / 8] = value;
    return true;
}
static void skip_completed(uint64_t *next, uint64_t length, uint64_t block_size,
                           const unsigned char *bits) {
    while (*next < length && bit_set(bits, (size_t)(*next / block_size))) {
        uint64_t span = length - *next;
        if (span > block_size) span = block_size;
        *next += span;
    }
}
static uint64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

RangeResult range_download(const RangeSource *source, const RangeInfo *info, FichierJob *job,
                           int fd, const char *partial_path) {
    off_t length = (off_t)info->length;
    if ((uint64_t)length != info->length) return RANGE_RETRY_SINGLE;
    const unsigned lanes = MAX_CONNECTIONS;
    uint64_t block_size = info->length / (lanes * 6);
    if (block_size < 8 * MIB) block_size = 8 * MIB;
    if (block_size > 64 * MIB) block_size = 64 * MIB;
    block_size -= block_size % RANGE_WRITE_BUFFER;
    size_t chunks = (size_t)((info->length - 1) / block_size + 1);
    if (!chunks || chunks > RANGE_MAX_CHUNKS) return RANGE_RETRY_SINGLE;
    size_t bytes = (chunks + 7) / 8;
    unsigned char *bits = calloc(bytes, 1);
    if (!bits) return RANGE_SD_ERROR;
    char state_path[272];
    snprintf(state_path, sizeof(state_path), "%s.range", partial_path);
    FILE *state = fopen(state_path, "rb+");
    struct stat existing = {0};
    bool same_file_size = !fstat(fd, &existing) && existing.st_size == length;
    RangeResumeHeader header = {0}, previous = {0};
    memcpy(header.magic, RANGE_RESUME_MAGIC, sizeof(header.magic));
    header.length = info->length;
    header.block_size = block_size;
    header.chunks = (uint32_t)chunks;
    snprintf(header.etag, sizeof(header.etag), "%s", info->etag);
    bool valid = state && same_file_size &&
        fread(&previous, sizeof(previous), 1, state) == 1 &&
        !memcmp(header.magic, previous.magic, sizeof(header.magic)) &&
        header.length == previous.length &&
        header.block_size == previous.block_size &&
        header.chunks == previous.chunks &&
        !memcmp(header.etag, previous.etag, sizeof(header.etag)) &&
        fread(bits, 1, bytes, state) == bytes && fgetc(state) == EOF;
    if (!valid) {
        if (state) fclose(state);
        state = fopen(state_path, "wb+");
        memset(bits, 0, bytes);
        if (!state || ftruncate(fd, 0) || ftruncate(fd, length) || fsync(fd) ||
            fwrite(&header, sizeof(header), 1, state) != 1 ||
            fwrite(bits, 1, bytes, state) != bytes || fflush(state) ||
            fsync(fileno(state))) {
            if (state) fclose(state);
            free(bits);
            return RANGE_SD_ERROR;
        }
    }
    uint64_t saved = 0;
    for (size_t i = 0; i < chunks; ++i)
        if (bit_set(bits, i)) {
            uint64_t span = info->length - (uint64_t)i * block_size;
            saved += span > block_size ? block_size : span;
        }
    atomic_store(&job->received, saved);

    RangePart parts[MAX_CONNECTIONS] = {0};
    RangeWriter writer = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .ready = PTHREAD_COND_INITIALIZER, .space = PTHREAD_COND_INITIALIZER, .fd = fd
    };
    CURLM *multi = curl_multi_init();
    char if_range[sizeof(info->etag) + 12];
    snprintf(if_range, sizeof(if_range), "If-Range: %s", info->etag);
    struct curl_slist *headers = info->etag[0] ? curl_slist_append(NULL, if_range) : NULL;
    RangeResult result = RANGE_RETRY_SINGLE;
    uint64_t started = milliseconds();
    uint64_t next = 0;
    unsigned active = 0;
    if (!multi || (info->etag[0] && !headers)) goto done;
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)lanes);
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)lanes);
    start_writer(&writer);
    atomic_store(&job->total, info->length);
    atomic_store(&job->streams, lanes);
    for (unsigned i = 0; i < lanes; ++i) {
        skip_completed(&next, info->length, block_size, bits);
        if (next >= info->length) break;
        RangePart *part = &parts[i];
        part->job = job;
        part->info = info;
        part->writer = &writer;
        part->buffer = write_buffer();
        part->curl = curl_easy_init();
        if (!part->buffer || !part->curl) goto done;
        configure(part->curl, source, job, &part->response);
        curl_easy_setopt(part->curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(part->curl, CURLOPT_BUFFERSIZE, (long)RANGE_RECEIVE_BUFFER);
        curl_easy_setopt(part->curl, CURLOPT_WRITEFUNCTION, part_body);
        curl_easy_setopt(part->curl, CURLOPT_WRITEDATA, part);
        curl_easy_setopt(part->curl, CURLOPT_PRIVATE, part);
        if (!schedule_part(multi, part, &next, block_size)) goto done;
        ++active;
    }
    while (active && !atomic_load(&job->cancelled)) {
        int running;
        if (curl_multi_perform(multi, &running) != CURLM_OK) goto done;
        bool rescheduled = false;
        int pending;
        CURLMsg *message;
        while ((message = curl_multi_info_read(multi, &pending))) {
            if (message->msg != CURLMSG_DONE) continue;
            char *private = NULL;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &private);
            RangePart *part = (RangePart *)private;
            if (!part) goto done;
            CURLcode code = message->data.result;
            curl_multi_remove_handle(multi, part->curl);
            part->active = false;
            --active;
            job->http = part->response.status;
            if (part->write_failed) {
                result = RANGE_SD_ERROR;
                goto done;
            }
            if (part->response.status == 429) {
                result = RANGE_RATE_LIMIT;
                goto done;
            }
            uint64_t length = part->last - part->first + 1;
            if (code != CURLE_OK || part->received != length) {
                if (atomic_load(&job->cancelled) || part->received == length ||
                    part->retries >= MAX_RANGE_RETRIES || !retryable_range_error(code)) goto done;
                ++part->retries;
                if (!request_remaining(multi, part)) goto done;
                ++active;
                rescheduled = true;
                continue;
            }
            if (!flush_part(part) ||
                !checkpoint_chunk(&writer, state, bits, (size_t)(part->first / block_size))) {
                result = RANGE_SD_ERROR;
                goto done;
            }
            saved += length;
            skip_completed(&next, info->length, block_size, bits);
            if (next < info->length) {
                if (!schedule_part(multi, part, &next, block_size)) goto done;
                ++active;
                rescheduled = true;
            }
        }
        if (active && !rescheduled && curl_multi_poll(multi, NULL, 0, 150, NULL) != CURLM_OK) goto done;
    }
    if (!active && !atomic_load(&job->cancelled) && atomic_load(&job->received) == info->length)
        result = RANGE_OK;
done:
    for (unsigned i = 0; i < lanes; ++i) {
        RangePart *part = &parts[i];
        if (part->active) curl_multi_remove_handle(multi, part->curl);
        if (part->curl) curl_easy_cleanup(part->curl);
        free(part->buffer);
    }
    curl_slist_free_all(headers);
    if (multi) curl_multi_cleanup(multi);
    if (!finish_writer(&writer, result == RANGE_OK)) result = RANGE_SD_ERROR;
    if (result != RANGE_OK) atomic_store(&job->received, saved);
    if (state) fclose(state);
    free(bits);
    if (result == RANGE_RETRY_SINGLE && saved)
        result = RANGE_INTERRUPTED;
    if (result == RANGE_OK) {
        uint64_t ended = milliseconds();
        if (ended > started) job->avg_speed = (uint64_t)(info->length * 1000.0 / (ended - started));
    }
    return result;
}
