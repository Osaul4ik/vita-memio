/*
 * restore.h - the recovery record the tester keeps while a test runs.
 *
 * A test switches the plugin on and off for one device. Before it does, the
 * tester saves the user's settings in a small file (RestoreRec). If the app is
 * closed or crashes mid-test, the next start finds the file and puts the settings
 * back. No Vita calls: the encode/check logic runs in the host tests.
 */
#ifndef STORAGETURBO_TESTER_RESTORE_H
#define STORAGETURBO_TESTER_RESTORE_H

#include "../include/storageturbo.h"

#define RESTORE_PATH   TS_DATA_DIR "/tester.restore"
#define RESTORE_MAGIC  0x52535454u   /* 'TTSR' */

typedef struct RestoreRec {
	uint32_t   magic;
	TsSettings settings;
	uint32_t   sum;
} RestoreRec;

void restore_pack(RestoreRec *r, const TsSettings *s);
/* 0 when the record is intact and every value is legal, else -1. */
int restore_unpack(const RestoreRec *r, TsSettings *out);

#endif
