/*
 * settings.h - the kernel module owns Turbo Storage's settings: loaded from
 * ur0:data/turbostorage/settings.bin at boot, changed through tsSetSetting(),
 * written back by the 1 Hz worker. See settings.c.
 */
#ifndef STORAGETURBO_SETTINGS_H
#define STORAGETURBO_SETTINGS_H

#include "../include/storageturbo.h"

#define TS_SETTINGS_PATH  TS_DATA_DIR "/settings.bin"

/* The live settings. Read freely (each field is one aligned word); write only
 * through ts_settings_set(). */
extern volatile TsSettings g_ts_settings;

void ts_settings_init(void);                 /* defaults, then the saved file  */
int  ts_settings_set(int key, int value);    /* validate + apply; TS_ERR_ARG   */
void ts_settings_copy(TsSettings *out);
void ts_settings_flush(void);                /* save if changed (worker only)  */

#endif /* STORAGETURBO_SETTINGS_H */
