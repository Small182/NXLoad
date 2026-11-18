#ifndef NXLOAD_SETTINGS_H
#define NXLOAD_SETTINGS_H

#include <stdbool.h>
#include <time.h>

typedef struct {
    bool install_default;
    bool delete_rar, delete_nsp;
    bool auto_update, auto_catalog;
    time_t update_checked, catalog_checked[2];
} Settings;

Settings load_settings(void);
bool save_settings(const Settings *s);
bool daily_check_due(time_t now, time_t checked);

#endif
