/*
 * settings.h - the kernel module owns vita-memio's settings: loaded from
 * ur0:data/vita-memio/settings.bin at boot, changed through vmSetSetting(),
 * written back by the worker. See settings.c.
 */
#ifndef VITAMEMIO_SETTINGS_H
#define VITAMEMIO_SETTINGS_H

#include "../include/vitamemio.h"

#define VM_SETTINGS_PATH  VM_DATA_DIR "/settings.bin"
/* A save is written here first and then renamed over VM_SETTINGS_PATH. */
#define VM_SETTINGS_TMP   VM_DATA_DIR "/settings.tmp"

/* The live settings. Read freely (each field is one aligned word); write only
 * through vm_settings_set(). */
extern volatile VmSettings g_vm_settings;

void vm_settings_init(void);                 /* defaults, then the saved file  */
int  vm_settings_set(int key, int value);    /* validate + apply; VM_ERR_ARG   */
void vm_settings_copy(VmSettings *out);
void vm_settings_flush(void);                /* save if changed (worker only)  */
int  vm_settings_pending(void);              /* a save still to do or retry    */

#endif /* VITAMEMIO_SETTINGS_H */
