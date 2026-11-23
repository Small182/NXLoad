#ifndef NXLOAD_SETTINGS_H
#define NXLOAD_SETTINGS_H

#include <stdbool.h>
#include <time.h>

#include "catalog.h"

typedef struct {
    bool install_default;
    bool delete_rar, delete_nsp;
    bool auto_catalog, source_enabled[CATALOG_SOURCE_COUNT], light_theme, english;
    time_t catalog_checked[CATALOG_SOURCE_COUNT];
} Settings;

Settings load_settings(void);
bool save_settings(const Settings *s);
bool daily_check_due(time_t now, time_t checked);

#endif
