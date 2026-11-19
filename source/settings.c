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
    unsigned install, rar, nsp, legacy_auto_update, catalogs;
    long long legacy_update_checked, nsroms, romshq, romsns_checked = 0;
    unsigned ns_enabled = 1, roms_enabled = 1, romsns_enabled = 1, light = 0, english = 0;
    bool ok = fscanf(file, "NXLOAD1 %u %u %u %u %u %lld %lld %lld",
                     &install, &rar, &nsp, &legacy_auto_update, &catalogs, &legacy_update_checked, &nsroms, &romshq) == 8;
    if (ok) {
        unsigned ns, roms, theme;
        if (fscanf(file, "%u %u %u", &ns, &roms, &theme) == 3 && ns <= 1 && roms <= 1 && theme <= 1) {
            ns_enabled = ns;
            roms_enabled = roms;
            light = theme;
            unsigned language;
            if (fscanf(file, "%u", &language) == 1 && language <= 1) {
                english = language;
                unsigned enabled;
                long long checked;
                if (fscanf(file, "%u %lld", &enabled, &checked) == 2 && enabled <= 1 && checked >= 0) {
                    romsns_enabled = enabled;
                    romsns_checked = checked;
                }
            }
        }
    }
    fclose(file);
    if (!ok || install > 1 || rar > 1 || nsp > 1 || legacy_auto_update > 1 || catalogs > 1 ||
        legacy_update_checked < 0 || nsroms < 0 || romshq < 0) return false;
    *s = (Settings){
        .install_default = install, .delete_rar = rar, .delete_nsp = nsp,
        .auto_catalog = catalogs, .source_enabled = {ns_enabled, roms_enabled, romsns_enabled},
        .light_theme = light, .english = english, .catalog_checked = {(time_t)nsroms, (time_t)romshq, (time_t)romsns_checked},
    };
    return true;
}

Settings load_settings(void) {
    Settings s = {.delete_rar = true, .auto_catalog = true, .source_enabled = {true, true, true}};
    if (!read_settings(PATH, &s)) read_settings(BACKUP, &s);
    return s;
}

bool save_settings(const Settings *s) {
    if (!nxload_ensure_storage()) return false;
    FILE *file = fopen(TEMP, "wb");
    if (!file) return false;
    // Retain the old file layout so a rollback can read these settings.
    bool ok = fprintf(file, "NXLOAD1\n%u %u %u 0 %u 0 %lld %lld\n%u %u %u %u\n%u %lld\n",
                      (unsigned)s->install_default, (unsigned)s->delete_rar, (unsigned)s->delete_nsp,
                      (unsigned)s->auto_catalog, (long long)s->catalog_checked[0],
                      (long long)s->catalog_checked[1], (unsigned)s->source_enabled[0],
                      (unsigned)s->source_enabled[1], (unsigned)s->light_theme, (unsigned)s->english,
                      (unsigned)s->source_enabled[CATALOG_ROMSNS],
                      (long long)s->catalog_checked[CATALOG_ROMSNS]) > 0 &&
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
