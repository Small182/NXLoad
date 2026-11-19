#define _GNU_SOURCE
#include "language.h"
#include "install.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ENTRY_LIMIT 1024
#define HEADER_LIMIT (1024 * 1024)
#define CNMT_LIMIT (16 * 1024 * 1024)
#define COPY_BUFFER (256 * 1024)
#define CARD_KEY_AREA_SIZE 0x1000

typedef struct {
    uint64_t offset, size;
    const char *name;
    NcmContentId id;
    NcmPlaceHolderId placeholder;
    bool pending, registered, existing, selected;
} PackageEntry;

typedef struct {
    FILE *file;
    char *names;
    PackageEntry *entries;
    size_t count;
    PackageEntry *meta;
    bool multiple_meta;
} Package;

typedef struct {
    uint64_t records_start, names_start, data_start, data_size;
    size_t count, stride, names_size;
} PackageFs;

typedef struct {
    u64 id;
    u32 version;
    u8 type, platform;
    u16 extended_size, contents, dependencies;
    u8 attributes, storage, install_type, committed;
    u32 required_download_version;
    u8 reserved[4];
} PackagedMeta;

typedef struct {
    NcmContentMetaKey key;
    u64 application_id;
    unsigned char *data;
    size_t size;
} InstallMeta;

_Static_assert(sizeof(PackagedMeta) == 32, "Invalid CNMT header size");

static bool ends_with(const char *name, const char *suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n >= s && !strcasecmp(name + n - s, suffix);
}

static uint32_t u32le(const unsigned char *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t u64le(const unsigned char *data) {
    return u32le(data) | (uint64_t)u32le(data + 4) << 32;
}

static bool read_at(FILE *file, uint64_t offset, void *out, size_t size) {
    return !fseeko(file, (off_t)offset, SEEK_SET) && fread(out, 1, size, file) == size;
}

static bool card_header(FILE *file, unsigned char header[64], uint64_t *base) {
    for (*base = 0; *base <= CARD_KEY_AREA_SIZE; *base += CARD_KEY_AREA_SIZE)
        if (read_at(file, *base + 0x100, header, 64) && !memcmp(header, "HEAD", 4)) return true;
    return false;
}

InstallFormat install_format(const char *path) {
    if (ends_with(path, ".nsp")) return INSTALL_NSP;
    if (ends_with(path, ".xci")) return INSTALL_XCI;
    if (ends_with(path, ".nsz") || ends_with(path, ".xcz")) return INSTALL_UNKNOWN;
    FILE *file = fopen(path, "rb");
    if (!file) return INSTALL_UNKNOWN;
    unsigned char header[64];
    uint64_t base;
    InstallFormat format = read_at(file, 0, header, 4) && !memcmp(header, "PFS0", 4) ? INSTALL_NSP :
                           card_header(file, header, &base) ? INSTALL_XCI : INSTALL_UNKNOWN;
    fclose(file);
    return format;
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool content_id(const char *name, NcmContentId *id) {
    if (strlen(name) != (ends_with(name, ".cnmt.nca") ? 41u : 36u)) return false;
    for (size_t i = 0; i < sizeof(id->c); ++i) {
        int high = hex_digit(name[2 * i]), low = hex_digit(name[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        id->c[i] = (unsigned char)(high * 16 + low);
    }
    return true;
}

static bool read_package_fs(FILE *file, uint64_t offset, uint64_t size, bool hfs, PackageFs *fs) {
    unsigned char header[16];
    if (size < sizeof(header) || !read_at(file, offset, header, sizeof(header)) ||
        memcmp(header, hfs ? "HFS0" : "PFS0", 4)) return false;
    fs->count = u32le(header + 4);
    fs->names_size = u32le(header + 8);
    fs->stride = hfs ? 64 : 24;
    if (!fs->count || fs->count > ENTRY_LIMIT || !fs->names_size || fs->names_size > HEADER_LIMIT)
        return false;
    size_t header_size = sizeof(header) + fs->stride * fs->count + fs->names_size;
    if (header_size > size) return false;
    fs->records_start = offset + sizeof(header);
    fs->names_start = fs->records_start + fs->stride * fs->count;
    fs->data_start = offset + header_size;
    fs->data_size = size - header_size;
    return true;
}

static bool read_fs_entry(FILE *file, const PackageFs *fs, const char *names, size_t i, PackageEntry *entry) {
    unsigned char record[64];
    if (!read_at(file, fs->records_start + fs->stride * i, record, fs->stride)) return false;
    uint64_t offset = u64le(record), length = u64le(record + 8);
    uint32_t name_offset = u32le(record + 16);
    if (offset > fs->data_size || length > fs->data_size - offset || name_offset >= fs->names_size)
        return false;
    const char *name = names + name_offset;
    if (!*name || !memchr(name, 0, fs->names_size - name_offset)) return false;
    entry->offset = fs->data_start + offset;
    entry->size = length;
    entry->name = name;
    return true;
}

static bool read_xci_secure(FILE *file, uint64_t file_size, PackageFs *secure) {
    // XCI CardHeader identifies the root HFS0, which holds the secure HFS0.
    unsigned char header[64];
    uint64_t base;
    if (!card_header(file, header, &base)) return false;
    uint64_t offset = u64le(header + 0x30), header_size = u64le(header + 0x38);
    if (offset < 0x200 || base > file_size || offset > file_size - base) return false;
    offset += base;
    PackageFs root;
    if (!read_package_fs(file, offset, file_size - offset, true, &root) ||
        root.data_start - offset != header_size) return false;
    char *names = malloc(root.names_size);
    if (!names) return false;
    bool valid = read_at(file, root.names_start, names, root.names_size);
    bool found = false;
    for (size_t i = 0; valid && i < root.count; ++i) {
        PackageEntry partition;
        valid = read_fs_entry(file, &root, names, i, &partition);
        if (!valid || strcmp(partition.name, "secure")) continue;
        valid = !found && read_package_fs(file, partition.offset, partition.size, true, secure);
        found = true;
    }
    free(names);
    return valid && found;
}

static bool read_package(Package *pkg, InstallFormat format) {
    if (fseeko(pkg->file, 0, SEEK_END)) return false;
    off_t size = ftello(pkg->file);
    if (size < 0) return false;
    PackageFs fs;
    bool valid = format == INSTALL_XCI ? read_xci_secure(pkg->file, (uint64_t)size, &fs) :
                 read_package_fs(pkg->file, 0, (uint64_t)size, false, &fs);
    if (!valid) return false;
    pkg->count = fs.count;
    pkg->names = malloc(fs.names_size);
    pkg->entries = calloc(pkg->count, sizeof(*pkg->entries));
    if (!pkg->names || !pkg->entries ||
        !read_at(pkg->file, fs.names_start, pkg->names, fs.names_size)) return false;
    for (size_t i = 0; i < pkg->count; ++i) {
        PackageEntry *e = &pkg->entries[i];
        if (!read_fs_entry(pkg->file, &fs, pkg->names, i, e)) return false;
        for (size_t j = 0; j < i; ++j) {
            PackageEntry *previous = &pkg->entries[j];
            if (!strcasecmp(previous->name, e->name) ||
                (e->size && previous->size && e->offset < previous->offset + previous->size &&
                 previous->offset < e->offset + e->size)) return false;
        }
        if (!ends_with(e->name, ".nca")) continue;
        if (!e->size || e->size >= (UINT64_C(1) << 40) || !content_id(e->name, &e->id)) return false;
        if (ends_with(e->name, ".cnmt.nca")) {
            if (pkg->meta) {
                pkg->multiple_meta = true;
                return false;
            }
            pkg->meta = e;
        }
    }
    return pkg->meta != NULL;
}

static PackageEntry *find_name(Package *pkg, const char *name) {
    for (size_t i = 0; i < pkg->count; ++i)
        if (!strcasecmp(pkg->entries[i].name, name)) return &pkg->entries[i];
    return NULL;
}

static void hex_id(char out[33], const void *id) {
    static const char digits[] = "0123456789abcdef";
    const unsigned char *bytes = id;
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    out[32] = 0;
}

static const char *service_error(FichierJob *job, const char *message, Result rc) {
    snprintf(job->install_error, sizeof(job->install_error), "%s (0x%08x)", tr(message), rc);
    return job->install_error;
}

static const char *prepare_entry(NcmContentStorage *sd, PackageEntry *entry, FichierJob *job) {
    Result rc = ncmContentStorageHas(sd, &entry->existing, &entry->id);
    if (R_SUCCEEDED(rc) && entry->existing) {
        s64 size = 0;
        rc = ncmContentStorageGetSizeFromContentId(sd, &size, &entry->id);
        if (R_SUCCEEDED(rc) && (size < 0 || (uint64_t)size != entry->size))
            return "Розмір наявного NCA відрізняється від пакета";
    }
    return R_FAILED(rc) ? service_error(job, "Не вдалося перевірити сховище", rc) : NULL;
}

static const char *write_content(NcmContentStorage *sd, FILE *file, PackageEntry *entry,
                                 const u8 *expected, void *buffer, FichierJob *job) {
    if (!entry->existing) {
        Result rc = ncmContentStorageGeneratePlaceHolderId(sd, &entry->placeholder);
        if (R_SUCCEEDED(rc))
            rc = ncmContentStorageCreatePlaceHolder(sd, &entry->id, &entry->placeholder, (s64)entry->size);
        if (R_FAILED(rc)) return service_error(job, "Не вдалося виділити місце для NCA", rc);
        entry->pending = true;
    }
    if (fseeko(file, (off_t)entry->offset, SEEK_SET)) return "Не вдалося прочитати пакет";
    Sha256Context sha;
    sha256ContextCreate(&sha);
    for (uint64_t position = 0; position < entry->size;) {
        if (atomic_load(&job->cancelled)) return "Встановлення скасовано";
        size_t length = entry->size - position < COPY_BUFFER ?
            (size_t)(entry->size - position) : COPY_BUFFER;
        if (fread(buffer, 1, length, file) != length) return "Пакет неповний";
        sha256ContextUpdate(&sha, buffer, length);
        if (entry->pending) {
            Result rc = ncmContentStorageWritePlaceHolder(sd, &entry->placeholder, position, buffer, length);
            if (R_FAILED(rc)) return service_error(job, "Помилка запису NCA", rc);
        }
        position += length;
        atomic_fetch_add(&job->received, length);
    }
    u8 digest[SHA256_HASH_SIZE];
    sha256ContextGetHash(&sha, digest);
    if (memcmp(digest, entry->id.c, sizeof(entry->id.c)) ||
        (expected && memcmp(digest, expected, sizeof(digest))))
        return "Контрольна сума NCA не збігається";
    return NULL;
}

static Result register_content(NcmContentStorage *sd, PackageEntry *entry) {
    if (!entry->pending) return 0;
    Result rc = ncmContentStorageRegister(sd, &entry->id, &entry->placeholder);
    if (R_SUCCEEDED(rc)) {
        entry->pending = false;
        entry->registered = true;
    }
    return rc;
}

static unsigned char *read_cnmt(NcmContentStorage *sd, const PackageEntry *meta,
                                size_t *size, Result *rc) {
    char path[FS_MAX_PATH];
    *rc = ncmContentStorageGetPath(sd, path, sizeof(path), &meta->id);
    if (R_FAILED(*rc)) return NULL;

    FsFileSystem fs;
    *rc = fsOpenFileSystemWithId(&fs, 0, FsFileSystemType_ContentMeta, path, FsContentAttributes_All);
    if (R_FAILED(*rc)) return NULL;

    FsDir dir;
    FsDirectoryEntry item;
    s64 count = 0;
    bool found = false;
    *rc = fsFsOpenDirectory(&fs, "/", FsDirOpenMode_ReadFiles, &dir);
    if (R_SUCCEEDED(*rc)) {
        while (R_SUCCEEDED(*rc = fsDirRead(&dir, &count, 1, &item)) && count == 1) {
            if (!ends_with(item.name, ".cnmt")) continue;
            if (found) {
                found = false;
                break;
            }
            found = true;
            if (strlen(item.name) >= sizeof(path) - 1) {
                found = false;
                break;
            }
            path[0] = '/';
            strcpy(path + 1, item.name);
        }
        fsDirClose(&dir);
    }
    unsigned char *data = NULL;
    if (found && R_SUCCEEDED(*rc)) {
        FsFile f;
        *rc = fsFsOpenFile(&fs, path, FsOpenMode_Read, &f);
        if (R_SUCCEEDED(*rc)) {
            s64 bytes = 0;
            *rc = fsFileGetSize(&f, &bytes);
            if (R_SUCCEEDED(*rc) && bytes >= (s64)sizeof(PackagedMeta) && bytes <= CNMT_LIMIT) {
                data = malloc((size_t)bytes);
                if (data) {
                    u64 copied = 0;
                    *rc = fsFileRead(&f, 0, data, (u64)bytes, FsReadOption_None, &copied);
                    if (R_FAILED(*rc) || copied != (u64)bytes) {
                        free(data);
                        data = NULL;
                    } else *size = (size_t)bytes;
                }
            }
            fsFileClose(&f);
        }
    }
    fsFsClose(&fs);
    return data;
}

static bool parse_cnmt(Package *pkg, const unsigned char *data, size_t size,
                       InstallMeta *meta, const NcmPackagedContentInfo **infos, u16 *count) {
    PackagedMeta h;
    memcpy(&h, data, sizeof(h));
    size_t extended = sizeof(h) + h.extended_size;
    size_t dependencies = extended + (size_t)h.contents * sizeof(NcmPackagedContentInfo);
    size_t end = dependencies + (size_t)h.dependencies * sizeof(NcmContentMetaInfo);
    if (!h.id || h.platform != NcmContentMetaPlatform_Nx || !h.contents ||
        h.contents >= ENTRY_LIMIT || h.install_type != NcmContentInstallType_Full ||
        end > size || h.extended_size < 16 || h.extended_size % _Alignof(NcmPackagedContentInfo) ||
        (h.type != NcmContentMetaType_Application && h.type != NcmContentMetaType_Patch &&
         h.type != NcmContentMetaType_AddOnContent)) return false;
    const unsigned char *extra = data + sizeof(h);
    meta->key = (NcmContentMetaKey){.id = h.id, .version = h.version, .type = h.type};
    meta->application_id = h.type == NcmContentMetaType_Application ? h.id : u64le(extra);
    if (meta->application_id < UINT64_C(0x0100000000010000) ||
        meta->application_id > UINT64_C(0x01ffffffffffffff)) return false;
    if (h.type == NcmContentMetaType_Patch && h.id != (meta->application_id | UINT64_C(0x800)))
        return false;
    if (h.type == NcmContentMetaType_AddOnContent &&
        (h.id & ~UINT64_C(0xfff)) != (meta->application_id | UINT64_C(0x1000)))
        return false;

    *infos = (const NcmPackagedContentInfo *)(data + extended);
    *count = h.contents;
    size_t actual = 1;
    for (size_t i = 0; i < h.contents; ++i) {
        const NcmContentInfo *info = &(*infos)[i].info;
        if (h.type != NcmContentMetaType_Patch || info->content_type != NcmContentType_DeltaFragment) ++actual;
    }
    NcmContentMetaHeader header = {
        .extended_header_size = h.extended_size, .content_count = (u16)actual,
        .content_meta_count = h.dependencies, .attributes = h.attributes, .storage_id = h.storage,
    };
    meta->size = sizeof(header) + h.extended_size + actual * sizeof(NcmContentInfo) + (end - dependencies);
    meta->data = malloc(meta->size);
    if (!meta->data) return false;
    NcmContentInfo self = {.content_id = pkg->meta->id, .content_type = NcmContentType_Meta};
    ncmU64ToContentInfoSize(pkg->meta->size, &self);
    unsigned char *out = meta->data;
    memcpy(out, &header, sizeof(header));
    out += sizeof(header);
    memcpy(out, extra, h.extended_size);
    out += h.extended_size;
    memcpy(out, &self, sizeof(self));
    out += sizeof(self);
    for (size_t i = 0; i < h.contents; ++i) {
        const NcmContentInfo *info = &(*infos)[i].info;
        if (h.type == NcmContentMetaType_Patch && info->content_type == NcmContentType_DeltaFragment) continue;
        memcpy(out, info, sizeof(*info));
        out += sizeof(*info);
    }
    memcpy(out, data + dependencies, end - dependencies);
    return true;
}

static Result add_application_record(Service *manager, const InstallMeta *meta) {
    const struct { u8 event; u8 padding[7]; u64 application_id; } request = {
        .event = 3, .application_id = meta->application_id,
    };
    const struct { NcmContentMetaKey key; u64 storage; } record = {
        .key = meta->key, .storage = NcmStorageId_SdCard,
    };
    return serviceDispatchIn(manager, 16, request,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
            .buffers = {{&record, sizeof(record)}});
}

static const char *check_rights(NcmContentStorage *sd, PackageEntry *e, FsRightsId *required,
                                 FichierJob *job) {
    static const FsRightsId zero = {0};
    NcmRightsId id = {0};
    Result rc = e->existing ?
        ncmContentStorageGetRightsIdFromContentId(sd, &id, &e->id, FsContentAttributes_All) :
        ncmContentStorageGetRightsIdFromPlaceHolderId(sd, &id, &e->placeholder, FsContentAttributes_All);
    if (R_FAILED(rc)) return service_error(job, "Система не змогла перевірити права NCA", rc);
    if (!memcmp(&id.rights_id, &zero, sizeof(zero))) return NULL;
    if (memcmp(required, &zero, sizeof(zero)) && memcmp(required, &id.rights_id, sizeof(zero)))
        return "У пакеті кілька різних квитків. Встанови його через DBI.";
    *required = id.rights_id;
    return NULL;
}

static bool existing_ticket(Service *es, const FsRightsId *id, bool *exists) {
    for (unsigned kind = 0; kind < 2; ++kind) {
        u32 count = 0, written = 0;
        if (R_FAILED(serviceDispatchOut(es, 9 + kind, count)) || count > 65536) return false;
        if (!count) continue;
        FsRightsId *rights = calloc(count, sizeof(*rights));
        if (!rights) return false;
        Result rc = serviceDispatchOut(es, 11 + kind, written,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
            .buffers = {{rights, count * sizeof(*rights)}});
        if (R_SUCCEEDED(rc) && written == count) {
            for (u32 i = 0; i < written; ++i)
                if (!memcmp(id, &rights[i], sizeof(*id))) { *exists = true; break; }
        }
        free(rights);
        if (R_FAILED(rc) || written != count) return false;
        if (*exists) break;
    }
    return true;
}

static const char *ensure_ticket(Service *es, const FsRightsId *id, Package *pkg,
                                  bool *imported, FichierJob *job) {
    static const FsRightsId zero = {0};
    if (!memcmp(id, &zero, sizeof(zero))) return NULL;
    Result rc = smGetService(es, "es");
    if (R_FAILED(rc)) return service_error(job, "Сервіс квитків недоступний", rc);
    bool exists = false;
    if (!existing_ticket(es, id, &exists)) return "Не вдалося перевірити наявні квитки";
    if (exists) return NULL;

    char hex[33], name[40];
    hex_id(hex, id);
    snprintf(name, sizeof(name), "%s.tik", hex);
    const PackageEntry *ticket = find_name(pkg, name);
    snprintf(name, sizeof(name), "%s.cert", hex);
    const PackageEntry *certificate = find_name(pkg, name);
    unsigned char tik[0x400], cert[0x4000];
    if (!ticket || !certificate || ticket->size < 0x2c0 || ticket->size > sizeof(tik) ||
        !certificate->size || certificate->size > sizeof(cert) ||
        !read_at(pkg->file, ticket->offset, tik, (size_t)ticket->size) ||
        !read_at(pkg->file, certificate->offset, cert, (size_t)certificate->size))
        return "Пакет потребує дійсного квитка з сертифікатом";
    // RSA-2048/SHA-256 has a 0x140-byte signature block before the ticket body.
    if (u32le(tik) != 0x010004 || memcmp(tik + 0x140 + 0x160, id, sizeof(*id)))
        return "Квиток не відповідає правам пакета або має інший формат";
    if (atomic_load(&job->cancelled)) return "Встановлення скасовано";
    rc = serviceDispatch(es, 1,
         .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                          SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
         .buffers = {{tik, (size_t)ticket->size}, {cert, (size_t)certificate->size}});
    if (R_FAILED(rc)) return service_error(job, "Система відхилила квиток", rc);
    *imported = true;
    return NULL;
}

static PackageEntry *match_content(Package *pkg, const NcmContentInfo *info) {
    char hex[33], name[37];
    hex_id(hex, &info->content_id);
    snprintf(name, sizeof(name), "%s.nca", hex);
    PackageEntry *e = find_name(pkg, name);
    u64 bytes = 0;
    ncmContentInfoSizeToU64(info, &bytes);
    return e && e->size == bytes ? e : NULL;
}

const char *install_package(FichierJob *job, const char *path, InstallFormat format) {
    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    snprintf(job->tls_detail, sizeof(job->tls_detail), "%.127s", filename);
    if (format == INSTALL_XCI) snprintf(job->dbi_file, sizeof(job->dbi_file), "%s", filename);
    Package pkg = {.file = fopen(path, "rb")};
    if (!pkg.file) return "Не вдалося відкрити збережений пакет";
    InstallMeta meta = {0};
    NcmContentStorage sd = {0};
    NcmContentMetaDatabase db = {0};
    Service manager = {0}, es = {0};
    FsRightsId required = {0};
    bool ncm_ready = false, ns_ready = false, meta_written = false, imported = false;
    bool clean = true, installed = false;
    unsigned char *cnmt = NULL, *buffer = NULL;
    const char *error = format == INSTALL_XCI ? "Не вдалося прочитати розділ secure або метадані XCI" :
                         "Некоректний або непідтримуваний пакет NSP";
    atomic_store(&job->received, 0);
    atomic_store(&job->total, 0);
    atomic_store(&job->streams, 0);
    atomic_store(&job->unpacking, false);
    atomic_store(&job->installing, true);
    if (!read_package(&pkg, format)) {
        if (pkg.multiple_meta) error = "Пакет містить кілька наборів метаданих. Встанови його через DBI.";
        goto done;
    }
    buffer = malloc(COPY_BUFFER);
    if (!buffer) { error = "Бракує пам’яті для встановлення"; goto done; }
    if (atomic_load(&job->cancelled)) { error = "Встановлення скасовано"; goto done; }
    Result rc = ncmInitialize();
    if (R_SUCCEEDED(rc)) {
        ncm_ready = true;
        rc = ncmOpenContentStorage(&sd, NcmStorageId_SdCard);
    }
    if (R_SUCCEEDED(rc)) rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    if (R_SUCCEEDED(rc)) {
        rc = nsInitialize();
        if (R_SUCCEEDED(rc)) {
            ns_ready = true;
            rc = nsGetApplicationManagerInterface(&manager);
        }
    }
    if (R_FAILED(rc)) {
        error = service_error(job, "Системний інсталятор або microSD недоступні", rc);
        goto done;
    }

    error = prepare_entry(&sd, pkg.meta, job);
    if (error) goto done;
    error = write_content(&sd, pkg.file, pkg.meta, NULL, buffer, job);
    if (error) goto done;
    rc = register_content(&sd, pkg.meta);
    if (R_FAILED(rc)) { error = service_error(job, "Не вдалося відкрити метадані пакета", rc); goto done; }

    size_t cnmt_size = 0;
    const NcmPackagedContentInfo *infos = NULL;
    u16 info_count = 0;
    cnmt = read_cnmt(&sd, pkg.meta, &cnmt_size, &rc);
    if (!cnmt || !parse_cnmt(&pkg, cnmt, cnmt_size, &meta, &infos, &info_count)) {
        error = R_FAILED(rc) ? service_error(job, "Не вдалося прочитати CNMT", rc) :
            "Невідомий формат CNMT. Підтримуються базова гра, оновлення та DLC.";
        goto done;
    }
    rc = ncmContentMetaDatabaseHas(&db, &installed, &meta.key);
    if (R_FAILED(rc)) { error = service_error(job, "Не вдалося перевірити встановлені версії", rc); goto done; }
    if (installed) { error = "Ця версія вже встановлена на microSD"; goto done; }

    uint64_t total = pkg.meta->size, needed = 0;
    for (size_t i = 0; i < info_count; ++i) {
        if (meta.key.type == NcmContentMetaType_Patch &&
            infos[i].info.content_type == NcmContentType_DeltaFragment) continue;
        PackageEntry *e = match_content(&pkg, &infos[i].info);
        if (!e || e->selected) { error = "У пакеті бракує NCA або є дублікати"; goto done; }
        e->selected = true;
        error = prepare_entry(&sd, e, job);
        if (error) goto done;
        total += e->size;
        if (!e->existing) needed += e->size;
    }

    s64 available = 0;
    rc = ncmContentStorageGetFreeSpaceSize(&sd, &available);
    if (R_FAILED(rc)) { error = service_error(job, "Не вдалося визначити вільне місце", rc); goto done; }
    if (available < 0 || (uint64_t)available < needed) {
        error = "На microSD бракує місця для встановлення. Завантажений пакет збережено.";
        goto done;
    }

    atomic_store(&job->total, total);
    for (size_t i = 0; i < info_count; ++i) {
        if (meta.key.type == NcmContentMetaType_Patch &&
            infos[i].info.content_type == NcmContentType_DeltaFragment) continue;
        PackageEntry *e = match_content(&pkg, &infos[i].info);
        error = write_content(&sd, pkg.file, e, infos[i].hash, buffer, job);
        if (error) goto done;
        error = check_rights(&sd, e, &required, job);
        if (error) goto done;
    }
    if (atomic_load(&job->cancelled)) { error = "Встановлення скасовано"; goto done; }
    error = ensure_ticket(&es, &required, &pkg, &imported, job);
    if (error) goto done;

    for (size_t i = 0; i < pkg.count; ++i) {
        rc = register_content(&sd, &pkg.entries[i]);
        if (R_FAILED(rc)) { error = service_error(job, "Не вдалося зареєструвати NCA", rc); goto done; }
    }
    rc = ncmContentMetaDatabaseSet(&db, &meta.key, meta.data, meta.size);
    if (R_SUCCEEDED(rc)) {
        meta_written = true;
        rc = ncmContentMetaDatabaseCommit(&db);
    }
    if (R_SUCCEEDED(rc)) rc = add_application_record(&manager, &meta);
    if (R_FAILED(rc)) error = service_error(job, "Не вдалося завершити встановлення", rc);

done:
    if (error && meta_written) {
        Result undo = ncmContentMetaDatabaseRemove(&db, &meta.key);
        if (R_SUCCEEDED(undo)) undo = ncmContentMetaDatabaseCommit(&db);
        clean = R_SUCCEEDED(undo);
    }
    if (error && clean && pkg.entries) {
        for (size_t i = 0; i < pkg.count; ++i) {
            PackageEntry *e = &pkg.entries[i];
            if (e->pending && R_FAILED(ncmContentStorageDeletePlaceHolder(&sd, &e->placeholder))) clean = false;
            if (e->registered && R_FAILED(ncmContentStorageDelete(&sd, &e->id))) clean = false;
        }
        if (imported && R_FAILED(serviceDispatch(&es, 3,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
            .buffers = {{&required, sizeof(required)}}))) clean = false;
    }
    if (serviceIsActive(&es)) serviceClose(&es);
    if (serviceIsActive(&manager)) serviceClose(&manager);
    if (ns_ready) nsExit();
    if (serviceIsActive(&db.s)) ncmContentMetaDatabaseClose(&db);
    if (serviceIsActive(&sd.s)) ncmContentStorageClose(&sd);
    if (ncm_ready) ncmExit();
    fclose(pkg.file);
    free(pkg.names);
    free(pkg.entries);
    free(meta.data);
    free(cnmt);
    free(buffer);
    if ((!error && clean) || installed || atomic_load(&job->cancelled)) job->dbi_file[0] = 0;
    return clean ? error : "Встановлення перерване. Система не підтвердила очищення всіх записів.";
}
