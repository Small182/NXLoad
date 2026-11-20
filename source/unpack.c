#define _GNU_SOURCE
#include "unpack.h"
#include "storage.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <fcntl.h>
#include <switch.h>
#endif

#define OUTPUT_PATH 320
#define COPY_BUFFER (256 * 1024)

typedef struct {
    char name[160], temporary[OUTPUT_PATH], destination[OUTPUT_PATH];
    bool published;
} ExtractedFile;

typedef struct {
    ExtractedFile *files;
    size_t count, capacity;
} PortAssets;

bool package_name(const char *name) {
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".nsp") || !strcasecmp(ext, ".xci") ||
                   !strcasecmp(ext, ".nsz") || !strcasecmp(ext, ".xcz"));
}

bool archive_name(const char *name) {
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".rar") || !strcasecmp(ext, ".zip"));
}

bool archive_signature(const char *path) {
    unsigned char signature[8];
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t size = fread(signature, 1, sizeof(signature), file);
    fclose(file);
    if (size >= 4 && (!memcmp(signature, "PK\x03\x04", 4) || !memcmp(signature, "PK\x05\x06", 4)))
        return true;
    return size >= 7 && !memcmp(signature, "Rar!\x1a\x07", 6) &&
           (signature[6] == 0 || (size == 8 && signature[6] == 1 && signature[7] == 0));
}

static bool entry_name(const char *path, char *name, size_t capacity) {
    if (!path || !*path || *path == '/' || *path == '\\') return false;
    unsigned char first = (unsigned char)path[0];
    if (((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z')) &&
        path[1] == ':') return false;

    const char *base = path;
    for (const char *p = path; ; ++p) {
        if (*p == '/' || *p == '\\' || !*p) {
            size_t length = (size_t)(p - base);
            if (length == 2 && base[0] == '.' && base[1] == '.') return false;
            if (!*p) break;
            base = p + 1;
        }
    }
    if (!package_name(base) || *base == '.') return false;

    const char *ext = strrchr(base, '.');
    size_t stem = (size_t)(ext - base), extension = strlen(ext);
    if (capacity <= extension + 1) return false;
    size_t max_stem = capacity - extension - 1;
    if (stem > max_stem) {
        stem = max_stem;
        while (stem && ((unsigned char)base[stem] & 0xc0) == 0x80) --stem;
    }
    // Only the basename is written. Map characters FAT does not accept.
    for (size_t i = 0; i < stem; ++i) {
        unsigned char c = (unsigned char)base[i];
        name[i] = c < 32 || c == 127 || strchr(":<>|?*\"", c) ? '_' : (char)c;
    }
    memcpy(name + stem, ext, extension + 1);
    return stem != 0;
}

static void entry_detail(FichierJob *job, const char *reason, const char *path) {
    snprintf(job->tls_detail, sizeof(job->tls_detail), "%s: ", reason);
    size_t prefix = strlen(job->tls_detail);
    size_t length = strnlen(path, sizeof(job->tls_detail) - prefix - 1);
    while (length && ((unsigned char)path[length] & 0xc0) == 0x80) --length;
    memcpy(job->tls_detail + prefix, path, length);
    job->tls_detail[prefix + length] = 0;
    for (char *c = job->tls_detail; *c; ++c)
        if ((unsigned char)*c < 32 || *c == 127) *c = '?';
}

static bool unused_path(const char *path) {
    struct stat entry;
    return lstat(path, &entry) != 0 && errno == ENOENT;
}

static int create_staging_directory(const char *directory, char staging[OUTPUT_PATH]) {
    unsigned long long timestamp = (unsigned long long)time(NULL);
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        int length = snprintf(staging, OUTPUT_PATH, "%s/nxload-unpack-%llx-%u", directory, timestamp, attempt);
        if (length < 0 || length >= OUTPUT_PATH) return ENAMETOOLONG;
        if (!mkdir(staging, 0777)) return 0;
        if (errno != EEXIST) return errno;
    }
    return EEXIST;
}

static const char *port_path(const char *path) {
    while (path && *path) {
        size_t length = strcspn(path, "/\\");
        if (length == 6 && !strncasecmp(path, "switch", length))
            return path + length + (path[length] != 0);
        if (!path[length]) break;
        path += length + 1;
    }
    return NULL;
}

static bool port_destination(const char *path, const char *relative, char out[OUTPUT_PATH]) {
    for (const char *part = path; *part;) {
        size_t length = strcspn(part, "/\\");
        if (!length || (part[length - 1] == '.' && (length != 1 || part >= relative)) ||
            part[length - 1] == ' ') return false;
        for (size_t i = 0; i < length; ++i) {
            unsigned char c = (unsigned char)part[i];
            if (c < 32 || c == 127 || strchr(":<>|?*\"", c)) return false;
        }
        part += length;
        if (*part) ++part;
    }
    int written = snprintf(out, OUTPUT_PATH, NXLOAD_SWITCH_DIR "/%s", relative);
    if (written < 0 || written >= OUTPUT_PATH) return false;
    for (char *p = out + sizeof(NXLOAD_SWITCH_DIR); *p; ++p)
        if (*p == '\\') *p = '/';
    return true;
}

static bool port_parent_directories(char *path) {
    for (char *p = path + sizeof(NXLOAD_SWITCH_DIR) - 1; p; p = strchr(p + 1, '/')) {
        *p = 0;
        struct stat info;
        bool ok = !mkdir(path, 0777) || (errno == EEXIST && !lstat(path, &info) && S_ISDIR(info.st_mode));
        *p = '/';
        if (!ok) return false;
    }
    return true;
}

static bool same_output(const ExtractedFile *output, unsigned char *buffer, atomic_bool *cancel) {
    struct stat source, destination;
    if (stat(output->temporary, &source) || lstat(output->destination, &destination) ||
        !S_ISREG(destination.st_mode) || source.st_size != destination.st_size) return false;
    FILE *left = fopen(output->temporary, "rb"), *right = fopen(output->destination, "rb");
    bool same = left && right;
    off_t remaining = source.st_size;
    while (same && remaining && !atomic_load(cancel)) {
        size_t bytes = remaining < COPY_BUFFER / 2 ? (size_t)remaining : COPY_BUFFER / 2;
        same = fread(buffer, 1, bytes, left) == bytes &&
               fread(buffer + COPY_BUFFER / 2, 1, bytes, right) == bytes &&
               !memcmp(buffer, buffer + COPY_BUFFER / 2, bytes);
        remaining -= (off_t)bytes;
    }
    if (left) fclose(left);
    if (right) fclose(right);
    return same && !remaining && !atomic_load(cancel);
}

static void clean_outputs(ExtractedFile *outputs, size_t count, bool rollback) {
    for (size_t i = 0; i < count; ++i) {
        ExtractedFile *output = &outputs[i];
        if (output->published) {
            if (rollback) unlink(output->destination);
        } else if (output->temporary[0]) unlink(output->temporary);
    }
    free(outputs);
}

static const char *write_entry(struct archive *reader, FichierJob *job, ExtractedFile *output,
                               uint64_t size, unsigned char *buffer) {
    int fd = mkstemp(output->temporary);
    if (fd < 0) {
        output->temporary[0] = 0;
        return nxload_storage_error(errno, "Не вдалося створити тимчасовий файл");
    }
#ifdef __SWITCH__
    if (size > UINT32_MAX) {
        close(fd);
        if (unlink(output->temporary) ||
            R_FAILED(fsdevCreateFile(output->temporary, 0, FsCreateOption_BigFile)))
            return "Не вдалося створити великий пакет на microSD";
        fd = open(output->temporary, O_WRONLY);
        if (fd < 0) return nxload_storage_error(errno, "Не вдалося відкрити великий пакет на microSD");
    }
#endif
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        return "Не вдалося відкрити тимчасовий файл";
    }

    uint64_t written = 0;
    const char *error = NULL;
    while (!atomic_load(&job->cancelled)) {
        la_ssize_t bytes = archive_read_data(reader, buffer, COPY_BUFFER);
        if (!bytes) break;
        if (bytes < 0 || (uint64_t)bytes > size - written) {
            error = "Архів пошкоджений, неповний або потребує іншого тому";
            break;
        }
        if (fwrite(buffer, 1, (size_t)bytes, file) != (size_t)bytes) {
            error = nxload_storage_error(errno, "Помилка запису під час розпакування");
            break;
        }
        written += (uint64_t)bytes;
        atomic_fetch_add(&job->received, (uint64_t)bytes);
#ifdef __SWITCH__
        if (atomic_load(&job->yield_for_game_requests)) svcSleepThread(1000000);
#endif
    }
    if (atomic_load(&job->cancelled)) error = "Розпакування скасовано";
    if (!error && written != size) error = "Пакет в архіві неповний";
    if (fclose(file) != 0 && !error)
        error = nxload_storage_error(errno, "Не вдалося зберегти розпакований пакет");
    return error;
}

static const char *archive_basename(const char *path) {
    if (!path) return NULL;
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!strncmp(base, "._", 2) || !strcmp(base, ".DS_Store")) return NULL;
    return base;
}

static const char *check_archive_entry(struct archive_entry *entry, bool allow_empty) {
    if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
        return "В архіві пакет є посиланням";
    if (archive_entry_filetype(entry) != AE_IFREG)
        return "Непідтримуваний тип пакета в архіві";
    if (archive_entry_is_encrypted(entry) > 0)
        return "Пакет в архіві захищений паролем";
    la_int64_t size = archive_entry_size(entry);
    if (!archive_entry_size_is_set(entry) || size < 0 || (!allow_empty && !size))
        return "В архіві некоректний розмір пакета";
    return NULL;
}

static const char *stage_entry(struct archive *reader, FichierJob *job, ExtractedFile *output,
                               const char *staging, uint64_t size, unsigned char *buffer) {
    int n = snprintf(output->temporary, sizeof(output->temporary), "%s/file-XXXXXX", staging);
    if (n <= 0 || (size_t)n >= sizeof(output->temporary)) {
        output->temporary[0] = 0;
        return "Шлях до microSD задовгий";
    }
    return write_entry(reader, job, output, size, buffer);
}

static const char *stage_port_asset(struct archive *reader, FichierJob *job, PortAssets *assets,
                                    const char *staging, const char *path, const char *relative,
                                    uint64_t size, unsigned char *buffer) {
    if (assets->count == assets->capacity) {
        size_t next = assets->capacity ? assets->capacity * 2 : 16;
        ExtractedFile *files = next > assets->capacity && next <= SIZE_MAX / sizeof(*files) ?
            realloc(assets->files, next * sizeof(*files)) : NULL;
        if (!files) return "Не вдалося підготувати розархіватор";
        assets->files = files;
        assets->capacity = next;
    }
    ExtractedFile *output = &assets->files[assets->count++];
    *output = (ExtractedFile){0};
    if (!port_destination(path, relative, output->destination)) return "Неприпустимий шлях файлу порту";
    return stage_entry(reader, job, output, staging, size, buffer);
}

static const char *unpack_port_7z(FichierJob *job, const char *path, const char *staging,
                                  PortAssets *assets, unsigned char *buffer) {
    struct archive *reader = archive_read_new();
    const char *error = "Не вдалося відкрити switch.7z";
    const size_t start = assets->count;
    if (!reader) return error;
    if (archive_read_support_format_7zip(reader) != ARCHIVE_OK ||
        archive_read_open_filename(reader, path, COPY_BUFFER) != ARCHIVE_OK) goto done;

    struct archive_entry *entry;
    int status = ARCHIVE_OK;
    bool wrapped = false;
    error = NULL;
    while (!atomic_load(&job->cancelled) &&
           (status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (!archive_basename(name)) continue;
        const char *relative = port_path(name);
        bool directory = archive_entry_filetype(entry) == AE_IFDIR;
        if (!wrapped && relative && (directory || *relative)) {
            // An explicit switch directory takes precedence over loose files.
            while (assets->count > start && !unlink(assets->files[assets->count - 1].temporary))
                --assets->count;
            if (assets->count > start) {
                error = "Не вдалося прибрати тимчасовий файл порту";
                break;
            }
            wrapped = true;
        }
        if (directory || (wrapped && (!relative || !*relative))) continue;
        if (!wrapped) {
            relative = name;
            while (relative[0] == '.' && (relative[1] == '/' || relative[1] == '\\')) relative += 2;
        }
        error = check_archive_entry(entry, true);
        if (!error) error = stage_port_asset(reader, job, assets, staging, name, relative,
                                             (uint64_t)archive_entry_size(entry), buffer);
        if (error) {
            const char *detail = archive_error_string(reader);
            entry_detail(job, "switch.7z", detail && *detail ? detail : name);
            break;
        }
    }
    if (atomic_load(&job->cancelled)) error = "Розпакування скасовано";
    if (!error && status != ARCHIVE_EOF) error = "Не вдалося розпакувати switch.7z";
    if (!error && assets->count == start) error = "У switch.7z немає файлів порту";

done:
    if (error) {
        const char *detail = archive_error_string(reader);
        if (detail && *detail) {
            if (!atomic_load(&job->cancelled) &&
                (strstr(detail, "Unexpected codec ID") || strstr(detail, "Unknown codec ID") ||
                 strstr(detail, "codec is unsupported") || strstr(detail, "Unsupported codec") ||
                 strstr(detail, "Unsupported filter")))
                error = "Непідтримуваний метод стиснення switch.7z";
            if (!job->tls_detail[0]) entry_detail(job, "switch.7z", detail);
        }
    }
    archive_read_free(reader);
    return error;
}

const char *unpack_archive(FichierJob *job, const char *path, const char *directory,
                       UnpackedPackages *packages) {
    job->tls_detail[0] = 0;
    atomic_store(&job->received, 0);
    atomic_store(&job->total, 0);
    atomic_store(&job->streams, 0);
    atomic_store(&job->unpacking, true);
    struct archive *reader = archive_read_new();
    ExtractedFile *outputs = calloc(ARCHIVE_PACKAGE_LIMIT, sizeof(*outputs));
    PortAssets assets = {0};
    unsigned char *buffer = malloc(COPY_BUFFER);
    const char *error = "Не вдалося підготувати розархіватор";
    size_t count = 0;
    bool inspect_port = job->homebrew_port || job->local_unpack;
    bool has_port_files = false;
    char staging[OUTPUT_PATH];
    bool staged = false;
    if (!reader || !outputs || !buffer) goto done;
    if (archive_read_support_format_rar(reader) != ARCHIVE_OK ||
        archive_read_support_format_rar5(reader) != ARCHIVE_OK ||
        archive_read_support_format_zip_seekable(reader) != ARCHIVE_OK ||
        archive_read_open_filename(reader, path, 256 * 1024) != ARCHIVE_OK) {
        error = "Не вдалося відкрити архів";
        goto done;
    }
    int directory_error = create_staging_directory(directory, staging);
    if (directory_error) {
        error = nxload_storage_error(directory_error, "Не вдалося створити тимчасову папку розпакування");
#ifdef __SWITCH__
        if (directory_error != ENAMETOOLONG)
            snprintf(job->tls_detail, sizeof(job->tls_detail), "mkdir · errno %d · FS 0x%08x",
                     directory_error, fsdevGetLastResult());
        else
#endif
            snprintf(job->tls_detail, sizeof(job->tls_detail), "mkdir · errno %d", directory_error);
        goto done;
    }
    staged = true;

    struct archive_entry *entry;
    int status = ARCHIVE_OK;
    error = NULL;
    while (!atomic_load(&job->cancelled) &&
           (status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const char *path_in_archive = archive_entry_pathname(entry);
        const char *base = archive_basename(path_in_archive);
        if (!base || archive_entry_filetype(entry) == AE_IFDIR) continue;
        const char *relative = inspect_port ? port_path(path_in_archive) : NULL;
        bool package = package_name(base);
        bool asset = relative && *relative && !package;
        bool port_archive = inspect_port && !strcasecmp(base, "switch.7z");
        if (asset || port_archive) {
            has_port_files = true;
            if (!job->install_after_download) continue;
        } else if (!package) continue;

        error = check_archive_entry(entry, asset && !port_archive);
        if (error) {
            entry_detail(job, "ARCHIVE", path_in_archive);
            break;
        }
        uint64_t size = (uint64_t)archive_entry_size(entry);
        if (port_archive) {
            ExtractedFile nested = {0};
            error = stage_entry(reader, job, &nested, staging, size, buffer);
            if (!error) error = unpack_port_7z(job, nested.temporary, staging, &assets, buffer);
            if (nested.temporary[0] && unlink(nested.temporary) && !error)
                error = "Не вдалося прибрати тимчасовий файл порту";
            if (error && !job->tls_detail[0] && !archive_error_string(reader))
                entry_detail(job, "ARCHIVE", path_in_archive);
            if (error) break;
            continue;
        }
        if (asset) {
            error = stage_port_asset(reader, job, &assets, staging, path_in_archive, relative, size, buffer);
            if (error && !archive_error_string(reader)) entry_detail(job, "SWITCH PATH", path_in_archive);
            if (error) break;
            continue;
        }
        if (count >= ARCHIVE_PACKAGE_LIMIT) {
            error = "Забагато пакетів в одному архіві";
            break;
        }
        ExtractedFile *output = &outputs[count];
        if (!entry_name(path_in_archive, output->name, sizeof(output->name))) {
            error = "Неприпустимий шлях пакета в архіві";
            entry_detail(job, "ARCHIVE PATH", path_in_archive);
            break;
        }
        int n = snprintf(output->destination, sizeof(output->destination), "%s/%s", directory, output->name);
        if (n <= 0 || (size_t)n >= sizeof(output->destination) ||
            (!inspect_port && !unused_path(output->destination))) {
            error = "Пакет із такою назвою вже є або шлях недоступний";
            break;
        }
        for (size_t i = 0; i < count; ++i)
            if (!strcasecmp(outputs[i].name, output->name)) error = "В архіві повторюється назва пакета";
        if (error) break;
        ++count;
        error = stage_entry(reader, job, output, staging, size, buffer);
        if (error && !archive_error_string(reader)) entry_detail(job, "ARCHIVE", path_in_archive);
        if (error) break;
    }
    if (atomic_load(&job->cancelled)) error = "Розпакування скасовано";
    if (!error && status != ARCHIVE_EOF) error = "Архів пошкоджений, неповний або захищений";
    if (!error && !count) error = "В архіві немає пакета NSP, XCI, NSZ або XCZ";
    for (size_t i = 0; !error && assets.count && i < count; ++i) {
        const char *ext = strrchr(outputs[i].name, '.');
        if (strcasecmp(ext, ".nsp") && strcasecmp(ext, ".xci"))
            error = "Для встановлення порту потрібен пакет NSP або XCI";
    }
    for (size_t i = 0; !error && i < count; ++i) {
        ExtractedFile *output = &outputs[i];
        if (!unused_path(output->destination)) {
            if (inspect_port && same_output(output, buffer, &job->cancelled)) continue;
            error = "Пакет із такою назвою вже є або шлях недоступний";
        } else if (rename(output->temporary, output->destination)) {
            error = "Не вдалося додати пакет до списку";
        } else {
            output->published = true;
        }
    }
    if (!error && assets.count) atomic_store(&job->copying_port_files, true);
    for (size_t i = 0; !error && i < assets.count; ++i) {
        ExtractedFile *output = &assets.files[i];
        if (atomic_load(&job->cancelled)) {
            error = "Розпакування скасовано";
        } else if (!port_parent_directories(output->destination)) {
            error = "Не вдалося створити папку для файлів порту";
        } else if (!unused_path(output->destination)) {
            if (same_output(output, buffer, &job->cancelled)) continue;
            error = "Файл порту вже існує й відрізняється від файлу в архіві";
        } else if (rename(output->temporary, output->destination)) {
            error = "Не вдалося скопіювати файл порту на microSD";
        } else {
            output->published = true;
        }
        if (error) entry_detail(job, "SWITCH", output->destination);
    }
    if (atomic_load(&job->cancelled)) error = "Розпакування скасовано";

done:
    if (reader) {
        if (error && !job->tls_detail[0]) {
            const char *detail = archive_error_string(reader);
            if (detail && *detail) entry_detail(job, "libarchive", detail);
        }
        archive_read_free(reader);
    }
    if (packages && !error) {
        packages->count = count;
        packages->has_port_files = has_port_files;
        for (size_t i = 0; i < count; ++i)
            strcpy(packages->paths[i], outputs[i].destination);
    }
    clean_outputs(assets.files, assets.count, error != NULL);
    clean_outputs(outputs, count, error != NULL);
    if (staged) rmdir(staging);
    atomic_store(&job->copying_port_files, false);
    free(buffer);
    return error;
}
