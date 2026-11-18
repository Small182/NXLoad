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
#include <unistd.h>
#ifdef __SWITCH__
#include <fcntl.h>
#include <switch.h>
#endif

#define PACKAGE_LIMIT 24
#define OUTPUT_PATH 320

typedef struct {
    char name[160], temporary[OUTPUT_PATH], destination[OUTPUT_PATH];
    bool published;
} ExtractedFile;

bool package_name(const char *name) {
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".nsp") || !strcasecmp(ext, ".xci") ||
                   !strcasecmp(ext, ".nsz") || !strcasecmp(ext, ".xcz"));
}

bool rar_name(const char *name) {
    const char *ext = strrchr(name, '.');
    return ext && !strcasecmp(ext, ".rar");
}

bool rar_signature(const char *path) {
    unsigned char signature[8];
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t size = fread(signature, 1, sizeof(signature), file);
    fclose(file);
    return size >= 7 && !memcmp(signature, "Rar!\x1a\x07", 6) &&
           (signature[6] == 0 || (size == 8 && signature[6] == 1 && signature[7] == 0));
}

static const char *entry_name(const char *path) {
    if (!path || !*path || *path == '/' || *path == '\\' || strchr(path, ':')) return NULL;
    const char *base = path;
    for (const char *p = path; ; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == '\\' || !c) {
            size_t length = (size_t)(p - base);
            if (!length || (length == 1 && *base == '.') ||
                (length == 2 && base[0] == '.' && base[1] == '.')) return NULL;
            if (!c) return *base == '.' || length >= sizeof(((ExtractedFile *)0)->name) ? NULL : base;
            base = p + 1;
        } else if (c < 32 || c == 127 || strchr("<>|?*\"", c)) {
            return NULL;
        }
    }
}

static bool unused_path(const char *path) {
    struct stat entry;
    return lstat(path, &entry) != 0 && errno == ENOENT;
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
        la_ssize_t bytes = archive_read_data(reader, buffer, 256 * 1024);
        if (!bytes) break;
        if (bytes < 0 || (uint64_t)bytes > size - written) {
            error = "RAR пошкоджений, неповний або потребує іншого тому";
            break;
        }
        if (fwrite(buffer, 1, (size_t)bytes, file) != (size_t)bytes) {
            error = nxload_storage_error(errno, "Помилка запису під час розпакування");
            break;
        }
        written += (uint64_t)bytes;
        atomic_fetch_add(&job->received, (uint64_t)bytes);
    }
    if (atomic_load(&job->cancelled)) error = "Розпакування скасовано";
    if (!error && written != size) error = "Пакет у RAR неповний";
    if (fclose(file) != 0 && !error)
        error = nxload_storage_error(errno, "Не вдалося зберегти розпакований пакет");
    return error;
}

const char *unpack_rar(FichierJob *job, const char *path, const char *directory) {
    atomic_store(&job->unpacking, true);
    atomic_store(&job->received, 0);
    atomic_store(&job->total, 0);
    atomic_store(&job->streams, 0);
    struct archive *reader = archive_read_new();
    ExtractedFile *outputs = calloc(PACKAGE_LIMIT, sizeof(*outputs));
    unsigned char *buffer = malloc(256 * 1024);
    const char *error = "Не вдалося підготувати розархіватор";
    size_t count = 0;
    if (!reader || !outputs || !buffer) goto done;
    if (archive_read_support_format_rar(reader) != ARCHIVE_OK ||
        archive_read_support_format_rar5(reader) != ARCHIVE_OK ||
        archive_read_open_filename(reader, path, 256 * 1024) != ARCHIVE_OK) {
        error = "Не вдалося відкрити RAR";
        goto done;
    }

    struct archive_entry *entry;
    int status = ARCHIVE_OK;
    error = NULL;
    while (!atomic_load(&job->cancelled) &&
           (status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const char *path_in_archive = archive_entry_pathname(entry);
        if (!path_in_archive || !package_name(path_in_archive)) continue;
        const char *name = entry_name(path_in_archive);
        if (!name || archive_entry_filetype(entry) != AE_IFREG ||
            archive_entry_symlink(entry) || archive_entry_hardlink(entry)) {
            error = "Неприпустимий шлях пакета в RAR";
            break;
        }
        if (archive_entry_is_encrypted(entry) > 0) {
            error = "Пакет у RAR захищений паролем";
            break;
        }
        if (count >= PACKAGE_LIMIT) {
            error = "Забагато пакетів в одному RAR";
            break;
        }
        la_int64_t length = archive_entry_size(entry);
        if (length <= 0) {
            error = "У RAR некоректний розмір пакета";
            break;
        }

        ExtractedFile *output = &outputs[count];
        int size = snprintf(output->destination, sizeof(output->destination), "%s/%s", directory, name);
        if (size <= 0 || (size_t)size >= sizeof(output->destination) || !unused_path(output->destination)) {
            error = "Пакет із такою назвою вже є або шлях недоступний";
            break;
        }
        for (size_t i = 0; i < count; ++i)
            if (!strcasecmp(outputs[i].name, name)) error = "У RAR повторюється назва пакета";
        if (error) break;

        size = snprintf(output->temporary, sizeof(output->temporary), "%s/.nxload-XXXXXX", directory);
        if (size <= 0 || (size_t)size >= sizeof(output->temporary)) {
            error = "Шлях до microSD задовгий";
            break;
        }
        strcpy(output->name, name);
        ++count;
        atomic_fetch_add(&job->total, (uint64_t)length);
        error = write_entry(reader, job, output, (uint64_t)length, buffer);
        if (error) break;
    }
    if (atomic_load(&job->cancelled)) error = "Розпакування скасовано";
    if (!error && status != ARCHIVE_EOF) error = "RAR пошкоджений, неповний або захищений";
    if (!error && !count) error = "У RAR немає пакета NSP, XCI, NSZ або XCZ";
    for (size_t i = 0; !error && i < count; ++i) {
        ExtractedFile *output = &outputs[i];
        if (!unused_path(output->destination) || rename(output->temporary, output->destination)) {
            error = "Не вдалося додати пакет до списку";
            break;
        }
        output->published = true;
    }

done:
    if (reader) archive_read_free(reader);
    for (size_t i = 0; i < count; ++i) {
        ExtractedFile *output = &outputs[i];
        if (output->published) {
            if (error) unlink(output->destination);
        } else if (output->temporary[0]) {
            unlink(output->temporary);
        }
    }
    free(outputs);
    free(buffer);
    return error;
}
