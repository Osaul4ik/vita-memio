/* The tester's recovery record: pack/unpack round trip and corruption. */
#include <string.h>

#include "ts_test.h"
#include "../tester/restore.h"
#include "../tester/restore.c"

int ts_failures;

int main(void)
{
	TsSettings s = { 1, 0, 1, TS_HUD_EXTENDED, TS_POS_BOTTOM_LEFT, 0, 1 }, o;
	RestoreRec r;

	restore_pack(&r, &s);
	CHECK(restore_unpack(&r, &o) == 0 && memcmp(&o, &s, sizeof(s)) == 0);

	for (size_t i = 0; i < sizeof(r); i++) {      /* any single flipped bit is caught */
		for (int b = 0; b < 8; b++) {
			RestoreRec x = r;

			((unsigned char *)&x)[i] ^= (unsigned char)(1u << b);
			memset(&o, 0x55, sizeof(o));
			CHECK(restore_unpack(&x, &o) != 0 || memcmp(&o, &s, sizeof(s)) == 0);
		}
	}
	{                                              /* a legal checksum over an illegal value */
		RestoreRec x;
		TsSettings bad = s;

		bad.hud = 9;
		restore_pack(&x, &bad);
		CHECK(restore_unpack(&x, &o) != 0);
	}
	printf(ts_failures ? "test_restore: %d FAILED\n" : "test_restore: ok\n", ts_failures);
	return ts_failures ? 1 : 0;
}
