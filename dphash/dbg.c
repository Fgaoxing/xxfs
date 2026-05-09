#include "dphash.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void make_key(char *buf, int i) { snprintf(buf, 32, "key_%08d", i); }
static void make_val(char *buf, int i) { snprintf(buf, 32, "val_%08d", i); }

int main(void)
{
	const int n = 100000;
	const char *dir = "/tmp/dphash_debug_paged";
	struct dphash *dp = dphash_open(dir, DPHASH_PAGED | DPHASH_LOCKLESS);
	if (!dp) { fprintf(stderr, "open fail\n"); return 1; }

	char k[32], v[32];
	int fail = 0;

	for (int i = 0; i < n; i++) {
		make_key(k, i); make_val(v, i);
		int rc = dphash_put(dp, k, (u32)strlen(k), v, (u32)strlen(v));
		if (rc != DPHASH_OK) {
			fprintf(stderr, "PUT FAIL i=%d rc=%d\n", i, rc);
			fail++;
			continue;
		}

		char buf[256]; u32 vlen = sizeof(buf);
		rc = dphash_get(dp, k, (u32)strlen(k), buf, &vlen);
		if (rc != DPHASH_OK) {
			fprintf(stderr, "IMMEDIATE GET FAIL i=%d rc=%d key=%s\n", i, rc, k);
			fail++;
		} else if (vlen != strlen(v) || memcmp(buf, v, vlen) != 0) {
			fprintf(stderr, "MISMATCH i=%d\n", i);
			fail++;
		}
	}

	printf("Immediate verify: %d failures / %d entries\n", fail, n);

	fail = 0;
	for (int i = 0; i < n; i++) {
		make_key(k, i);
		char buf[256]; u32 vlen = sizeof(buf);
		int rc = dphash_get(dp, k, (u32)strlen(k), buf, &vlen);
		if (rc != DPHASH_OK) {
			fprintf(stderr, "FINAL GET FAIL i=%d rc=%d key=%s\n", i, rc, k);
			fail++;
		} else {
			make_val(v, i);
			if (vlen != strlen(v) || memcmp(buf, v, vlen) != 0) {
				fprintf(stderr, "FINAL MISMATCH i=%d\n", i);
				fail++;
			}
		}
	}

	printf("Final verify: %d failures / %d entries\n", fail, n);
	dphash_close(dp);
	return fail > 0 ? 1 : 0;
}
