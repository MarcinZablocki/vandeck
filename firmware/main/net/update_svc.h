#pragma once
#include <stdbool.h>

// Firmware updates from GitHub releases (CONFIG_VD_UPDATE_REPO). Each release carries version.json and vandeck.bin;
// tools/release.sh publishes them. Settings > About chooses Off / Notify / Automatic (g_cfg.upd_mode).
void update_svc_init(void);
bool update_svc_enabled(void);   // a repository is configured
void update_svc_check(void);     // check now (in the background)
void update_svc_install(void);   // install the available release now, with the progress screen
