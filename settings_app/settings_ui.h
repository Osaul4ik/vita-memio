/*
 * settings_ui.h - Settings-app (NPXS10015) hooks of vitamemio.suprx.
 * settings_app/main.c installs them at module_start.
 */
#ifndef VITAMEMIO_SETTINGS_UI_H
#define VITAMEMIO_SETTINGS_UI_H

void vm_settings_hooks_install(void);
void vm_settings_hooks_release(void);

#endif /* VITAMEMIO_SETTINGS_UI_H */
