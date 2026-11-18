#define _GNU_SOURCE
#include "settings.h"
#include "storage.h"

#include <stdio.h>
#include <unistd.h>

#define PATH NXLOAD_APP_DIR "/settings.cfg"
#define BACKUP PATH ".prev"
#define TEMP PATH ".tmp"

static bool read_settings(const char *path, Settings *s) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned install, rar, nsp, app, catalogs;
    long long checked, nsroms, romshq;
    bool ok = fscanf(file, "NXLOAD1 %u %u %u %u %u %lld %lld %lld",
                     &install, &rar, &nsp, &app, &catalogs, &checked, &nsroms, &romshq) == 8;
    fclose(file);
    if (!ok || install > 1 || rar > 1 || nsp > 1 || app > 1 || catalogs > 1 ||
        checked < 0 || nsroms < 0 || romshq < 0) return false;
    *s = (Settings){
        .install_default = install, .delete_rar = rar, .delete_nsp = nsp,
        .auto_update = app, .auto_catalog = catalogs,
        .update_checked = (time_t)checked, .catalog_checked = {(time_t)nsroms, (time_t)romshq},
    };
    return true;
}

Settings load_settings(void) {
    Settings s = {.delete_rar = true, .auto_update = true, .auto_catalog = true};
    if (!read_settings(PATH, &s)) read_settings(BACKUP, &s);
    return s;
}

bool save_settings(const Settings *s) {
    if (!nxload_ensure_storage()) return false;
    FILE *file = fopen(TEMP, "wb");
    if (!file) return false;
    bool ok = fprintf(file, "NXLOAD1\n%u %u %u %u %u %lld %lld %lld\n",
                      (unsigned)s->install_default, (unsigned)s->delete_rar, (unsigned)s->delete_nsp,
                      (unsigned)s->auto_update, (unsigned)s->auto_catalog,
                      (long long)s->update_checked, (long long)s->catalog_checked[0],
                      (long long)s->catalog_checked[1]) > 0 &&
              !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;

    bool moved = false;
    if (ok) {
        struct stat st;
        if (!stat(PATH, &st)) {
            if (!unlink(BACKUP) || errno == ENOENT) moved = !rename(PATH, BACKUP);
            ok = moved;
        } else {
            ok = errno == ENOENT;
        }
        if (ok) ok = !rename(TEMP, PATH);
        if (ok) unlink(BACKUP);
        else if (moved) rename(BACKUP, PATH);
    }
    if (!ok) unlink(TEMP);
    return ok;
}

bool daily_check_due(time_t now, time_t checked) {
    return now > 0 && (checked <= 0 || now < checked || now - checked >= 24 * 60 * 60);
}
