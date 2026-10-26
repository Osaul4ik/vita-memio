/*
 * settings_ui.h - Settings-app (NPXS10015) hooks of storageturbo.suprx.
 * settings_app/main.c installs them at module_start.
 */
#ifndef STORAGETURBO_SETTINGS_UI_H
#define STORAGETURBO_SETTINGS_UI_H

void ts_settings_hooks_install(void);
void ts_settings_hooks_release(void);

#endif /* STORAGETURBO_SETTINGS_UI_H */
