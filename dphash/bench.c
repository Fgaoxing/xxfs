#include "dphash.h"
#include "bptree.h"
#include "lsmtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_PUT    200000
#define N_GET    10000
#define N_DISK   10000

static inline double ns_elapsed(struct timespec *s, struct timespec *e)
{
	return (e->tv_sec - s->tv_sec) * 1e9 + (e->tv_nsec - s->tv_nsec);
}

static void make_key(char *buf, int i) { snprintf(buf, 32, "key_%08d", i); }
static void make_val(char *buf, int i) { snprintf(buf, 32, "val_%08d", i); }

#define BENCH(name, iters, code) do { \
	struct timespec _s, _e; \
	clock_gettime(CLOCK_MONOTONIC, &_s); \
	code; \
	clock_gettime(CLOCK_MONOTONIC, &_e); \
	printf("  %-25s %8.1f ns/op\n", name, ns_elapsed(&_s, &_e) / (iters)); \
} while(0)

static void test_correctness(void)
{
	printf("=== Correctness (Swiss Table) ===\n");
	const char *dir = "/tmp/dphash_test_c";
	struct dphash *dp = dphash_open(dir, DPHASH_NOSYNC);
	if (!dp) { fprintf(stderr, "open fail\n"); exit(1); }

	char k[32], v[32];
	const int n = 1000;

	for (int i = 0; i < n; i++) {
		make_key(k, i); make_val(v, i);
		if (dphash_put(dp, k, (u32)strlen(k), v, (u32)strlen(v)) != DPHASH_OK) {
			fprintf(stderr, "put %d fail\n", i); exit(1);
		}
	}
	for (int i = 0; i < n; i++) {
		make_key(k, i);
		char buf[256]; u32 vlen = sizeof(buf);
		if (dphash_get(dp, k, (u32)strlen(k), buf, &vlen) != DPHASH_OK) {
			fprintf(stderr, "get %d fail\n", i); exit(1);
		}
		make_val(v, i);
		if (vlen != strlen(v) || memcmp(buf, v, vlen) != 0) {
			fprintf(stderr, "mismatch %d\n", i); exit(1);
		}
	}
	for (int i = 0; i < n; i += 2) {
		make_key(k, i);
		if (dphash_del(dp, k, (u32)strlen(k)) != DPHASH_OK) {
			fprintf(stderr, "del %d fail\n", i); exit(1);
		}
	}
	for (int i = 0; i < n; i++) {
		make_key(k, i);
		char buf[256]; u32 vlen = sizeof(buf);
		int rc = dphash_get(dp, k, (u32)strlen(k), buf, &vlen);
		if (i % 2 == 0 && rc != DPHASH_ENOENT) { fprintf(stderr, "not gone %d\n", i); exit(1); }
		if (i % 2 == 1 && rc != DPHASH_OK) { fprintf(stderr, "gone %d\n", i); exit(1); }
	}
	printf("  count = %lu, mem = %zu bytes\n", (unsigned long)dphash_count(dp), dphash_mem_usage(dp));
	dphash_close(dp);
	printf("  PASS\n\n");

	printf("=== Correctness (Paged) ===\n");
	const char *dir2 = "/tmp/dphash_test_paged";
	struct dphash *dp2 = dphash_open(dir2, DPHASH_PAGED);
	if (!dp2) { fprintf(stderr, "paged open fail\n"); exit(1); }

	for (int i = 0; i < n; i++) {
		make_key(k, i); make_val(v, i);
		if (dphash_put(dp2, k, (u32)strlen(k), v, (u32)strlen(v)) != DPHASH_OK) {
			fprintf(stderr, "paged put %d fail\n", i); exit(1);
		}
	}
	for (int i = 0; i < n; i++) {
		make_key(k, i);
		char buf[256]; u32 vlen = sizeof(buf);
		if (dphash_get(dp2, k, (u32)strlen(k), buf, &vlen) != DPHASH_OK) {
			fprintf(stderr, "paged get %d fail\n", i); exit(1);
		}
		make_val(v, i);
		if (vlen != strlen(v) || memcmp(buf, v, vlen) != 0) {
			fprintf(stderr, "paged mismatch %d\n", i); exit(1);
		}
	}
	printf("  count = %lu, mem = %zu bytes\n", (unsigned long)dphash_count(dp2), dphash_mem_usage(dp2));
	dphash_close(dp2);
	printf("  PASS\n\n");
}

static void bench_put(void)
{
	printf("--- Put (%d, all LOCKLESS) ---\n", N_PUT);

	{
		const char *dir = "/tmp/bench_dphash_put";
		struct dphash *dp = dphash_open(dir, DPHASH_NOSYNC | DPHASH_LOCKLESS);
		if (!dp) goto skip1;
		char (*keys)[32] = malloc(N_PUT * 32);
		char (*vals)[32] = malloc(N_PUT * 32);
		if (!keys || !vals) { free(keys); free(vals); dphash_close(dp); goto skip1; }
		for (int i = 0; i < N_PUT; i++) { make_key(keys[i], i); make_val(vals[i], i); }
		BENCH("DPHash-Swiss", N_PUT, {
			for (int i = 0; i < N_PUT; i++)
				dphash_put(dp, keys[i], (u32)strlen(keys[i]), vals[i], (u32)strlen(vals[i]));
		});
		printf("    mem = %zu MB\n", dphash_mem_usage(dp) >> 20);
		free(keys); free(vals); dphash_close(dp);
	}
skip1:

	{
		const char *dir = "/tmp/bench_dphash_paged_put";
		struct dphash *dp = dphash_open(dir, DPHASH_PAGED | DPHASH_LOCKLESS);
		if (!dp) goto skip2;
		char (*keys)[32] = malloc(N_PUT * 32);
		char (*vals)[32] = malloc(N_PUT * 32);
		if (!keys || !vals) { free(keys); free(vals); dphash_close(dp); goto skip2; }
		for (int i = 0; i < N_PUT; i++) { make_key(keys[i], i); make_val(vals[i], i); }
		BENCH("DPHash-Paged", N_PUT, {
			for (int i = 0; i < N_PUT; i++)
				dphash_put(dp, keys[i], (u32)strlen(keys[i]), vals[i], (u32)strlen(vals[i]));
		});
		printf("    mem = %zu MB\n", dphash_mem_usage(dp) >> 20);
		free(keys); free(vals); dphash_close(dp);
	}
skip2:

	{
		const char *dir = "/tmp/bench_bptree_put";
		struct bptree *bt = bptree_open(dir, DPHASH_NOSYNC | DPHASH_LOCKLESS);
		if (!bt) goto skip3;
		char (*keys)[32] = malloc(N_PUT * 32);
		char (*vals)[32] = malloc(N_PUT * 32);
		if (!keys || !vals) { free(keys); free(vals); bptree_close(bt); goto skip3; }
		for (int i = 0; i < N_PUT; i++) { make_key(keys[i], i); make_val(vals[i], i); }
		BENCH("B+Tree", N_PUT, {
			for (int i = 0; i < N_PUT; i++)
				bptree_put(bt, keys[i], (u32)strlen(keys[i]), vals[i], (u32)strlen(vals[i]));
		});
		free(keys); free(vals); bptree_close(bt);
	}
skip3:

	{
		const char *dir = "/tmp/bench_lsmtree_put";
		struct lsmtree *lm = lsmtree_open(dir, DPHASH_NOSYNC | DPHASH_LOCKLESS);
		if (!lm) return;
		char (*keys)[32] = malloc(N_PUT * 32);
		char (*vals)[32] = malloc(N_PUT * 32);
		if (!keys || !vals) { free(keys); free(vals); lsmtree_close(lm); return; }
		for (int i = 0; i < N_PUT; i++) { make_key(keys[i], i); make_val(vals[i], i); }
		BENCH("LSM-Tree", N_PUT, {
			for (int i = 0; i < N_PUT; i++)
				lsmtree_put(lm, keys[i], (u32)strlen(keys[i]), vals[i], (u32)strlen(vals[i]));
		});
		free(keys); free(vals); lsmtree_close(lm);
	}
}

static void bench_get(void)
{
	printf("--- Get (%d preloaded, %d lookups, all LOCKLESS) ---\n", N_GET, N_GET * 10);

	{
		const char *dir = "/tmp/bench_dphash_get";
		struct dphash *dp = dphash_open(dir, DPHASH_NOSYNC | DPHASH_LOCKLESS);
		if (!dp) goto skip1;
		char keys[N_GET][32], vals[N_GET][32];
		for (int i = 0; i < N_GET; i++) {
			make_key(keys[i], i); make_val(vals[i], i);
			dphash_put(dp, keys[i], (u32)strlen(keys[i]), vals[i], (u32)strlen(vals[i]));
		}
		int lookups = N_GET * 10;
		BENCH("DPHash-Swiss", lookups, {
			for (int i = 0; i < lookups; i++) {
				char buf[256]; u32 vl = sizeof(buf);
				dphash_get(dp, keys[i % N_GET], (u32)strlen(keys[i % N_GET]), buf, &vl);
			}
		});
		dphash_close(dp);
	}
skip1:

	{
		const char *dir = "/tmp/bench_dphash_paged_get";
		struct dphash *dp = dphash_open(dir, DPHASH_PAGED | DPHASH_LOCKLESS);
		if (!dp) goto skip2;
		char keys[N_GET][32], vals[N_GET][32];
		for (int i = 0; i < N_GET; i++) {
			make_key(keys[i], i); make_val(vals[i], i);
			dphash_put(dp, keys[i], (u32)strlen(keys[i]), vals[i], (u32)strlen(vals[i]));
		}
		int lookups = N_GET * 10;
		BENCH("DPHash-Paged", lookups, {
			for (int i = 0; i < lookups; i++) {
				char buf[256]; u32 vl = sizeof(buf);
				dphash_get(dp, keys[i % N_GET], (u32)strlen(keys[i % N_GET]), buf, &vl);
			}
		});
		printf("    mem = %zu KB\n", dphash_mem_usage(dp) >> 10);
		dphash_close(dp);
	}
skip2:

	{
		const char *dir = "/tmp/bench_bptree_get";
		struct bptree *bt = bptree_open(dir, DPHASH_NOSYNC | DPHASH_LOCKLESS);
		if (!bt) goto skip3;
		char keys[N_GET][32], vals[N_GET][32];
		for (int i = 0; i < N_GET; i++) {
			make_key(keys[i], i); make_val(vals[i], i);
			bptree_put(bt, keys[i], (u32)strlen(keys[i]), vals[i], (u32)strlen(vals[i]));
		}
		int lookups = N_GET * 10;
		BENCH("B+Tree", lookups, {
			for (int i = 0; i < lookups; i++) {
				const void *val; u32 vlen;
				bptree_get(bt, keys[i % N_GET], (u32)strlen(keys[i % N_GET]), &val, &vlen);
			}
		});
		bptree_close(bt);
	}
skip3:

	{
		const char *dir = "/tmp/bench_lsmtree_get";
		struct lsmtree *lm = lsmtree_open(dir, DPHASH_NOSYNC | DPHASH_LOCKLESS);
		if (!lm) return;
		char keys[N_GET][32], vals[N_GET][32];
		for (int i = 0; i < N_GET; i++) {
			make_key(keys[i], i); make_val(vals[i], i);
			lsmtree_put(lm, keys[i], (u32)strlen(keys[i]), vals[i], (u32)strlen(vals[i]));
		}
		int lookups = N_GET * 10;
		BENCH("LSM-Tree", lookups, {
			for (int i = 0; i < lookups; i++) {
				const void *val; u32 vlen;
				lsmtree_get(lm, keys[i % N_GET], (u32)strlen(keys[i % N_GET]), &val, &vlen);
			}
		});
		lsmtree_close(lm);
	}
}

static void bench_disk_put(void)
{
	printf("--- DiskPut (fsync, %d, all LOCKLESS) ---\n", N_DISK);

	{
		const char *dir = "/tmp/bench_dphash_diskput";
		struct dphash *dp = dphash_open(dir, DPHASH_FSYNC | DPHASH_LOCKLESS);
		if (!dp) goto skip1;
		char k[32], v[32];
		BENCH("DPHash-Swiss", N_DISK, {
			for (int i = 0; i < N_DISK; i++) {
				make_key(k, i); make_val(v, i);
				dphash_put(dp, k, (u32)strlen(k), v, (u32)strlen(v));
			}
		});
		dphash_close(dp);
	}
skip1:

	{
		const char *dir = "/tmp/bench_dphash_paged_diskput";
		struct dphash *dp = dphash_open(dir, DPHASH_PAGED | DPHASH_FSYNC | DPHASH_LOCKLESS);
		if (!dp) goto skip2;
		char k[32], v[32];
		BENCH("DPHash-Paged", N_DISK, {
			for (int i = 0; i < N_DISK; i++) {
				make_key(k, i); make_val(v, i);
				dphash_put(dp, k, (u32)strlen(k), v, (u32)strlen(v));
			}
		});
		dphash_close(dp);
	}
skip2:

	{
		const char *dir = "/tmp/bench_bptree_diskput";
		struct bptree *bt = bptree_open(dir, DPHASH_FSYNC | DPHASH_LOCKLESS);
		if (!bt) goto skip3;
		char k[32], v[32];
		BENCH("B+Tree", N_DISK, {
			for (int i = 0; i < N_DISK; i++) {
				make_key(k, i); make_val(v, i);
				bptree_put(bt, k, (u32)strlen(k), v, (u32)strlen(v));
			}
		});
		bptree_close(bt);
	}
skip3:

	{
		const char *dir = "/tmp/bench_lsmtree_diskput";
		struct lsmtree *lm = lsmtree_open(dir, DPHASH_FSYNC | DPHASH_LOCKLESS);
		if (!lm) return;
		char k[32], v[32];
		BENCH("LSM-Tree", N_DISK, {
			for (int i = 0; i < N_DISK; i++) {
				make_key(k, i); make_val(v, i);
				lsmtree_put(lm, k, (u32)strlen(k), v, (u32)strlen(v));
			}
		});
		lsmtree_close(lm);
	}
}

static void bench_disk_get(void)
{
	printf("--- DiskGet (pread from file, %d lookups, all LOCKLESS) ---\n", N_DISK * 10);

	{
		const char *dir = "/tmp/bench_dphash_diskget";
		struct dphash *dp = dphash_open(dir, DPHASH_FSYNC | DPHASH_LOCKLESS);
		if (!dp) goto skip1;
		char keys[N_DISK][32];
		for (int i = 0; i < N_DISK; i++) {
			make_key(keys[i], i);
			char v[32]; make_val(v, i);
			dphash_put(dp, keys[i], (u32)strlen(keys[i]), v, (u32)strlen(v));
		}
		int lookups = N_DISK * 10;
		BENCH("DPHash-Swiss", lookups, {
			for (int i = 0; i < lookups; i++) {
				char buf[256]; u32 vl = sizeof(buf);
				dphash_get_disk(dp, keys[i % N_DISK], (u32)strlen(keys[i % N_DISK]), buf, &vl);
			}
		});
		dphash_close(dp);
	}
skip1:

	{
		const char *dir = "/tmp/bench_dphash_paged_diskget";
		struct dphash *dp = dphash_open(dir, DPHASH_PAGED | DPHASH_FSYNC | DPHASH_LOCKLESS);
		if (!dp) goto skip2;
		char keys[N_DISK][32];
		for (int i = 0; i < N_DISK; i++) {
			make_key(keys[i], i);
			char v[32]; make_val(v, i);
			dphash_put(dp, keys[i], (u32)strlen(keys[i]), v, (u32)strlen(v));
		}
		int lookups = N_DISK * 10;
		BENCH("DPHash-Paged", lookups, {
			for (int i = 0; i < lookups; i++) {
				char buf[256]; u32 vl = sizeof(buf);
				dphash_get_disk(dp, keys[i % N_DISK], (u32)strlen(keys[i % N_DISK]), buf, &vl);
			}
		});
		printf("    mem = %zu KB\n", dphash_mem_usage(dp) >> 10);
		dphash_close(dp);
	}
skip2:

	{
		const char *dir = "/tmp/bench_bptree_diskget";
		struct bptree *bt = bptree_open(dir, DPHASH_FSYNC | DPHASH_LOCKLESS);
		if (!bt) goto skip3;
		char keys[N_DISK][32];
		for (int i = 0; i < N_DISK; i++) {
			make_key(keys[i], i);
			char v[32]; make_val(v, i);
			bptree_put(bt, keys[i], (u32)strlen(keys[i]), v, (u32)strlen(v));
		}
		int lookups = N_DISK * 10;
		BENCH("B+Tree", lookups, {
			for (int i = 0; i < lookups; i++) {
				char buf[256]; u32 vl = sizeof(buf);
				bptree_get_disk(bt, keys[i % N_DISK], (u32)strlen(keys[i % N_DISK]), buf, &vl);
			}
		});
		bptree_close(bt);
	}
skip3:

	{
		const char *dir = "/tmp/bench_lsmtree_diskget";
		struct lsmtree *lm = lsmtree_open(dir, DPHASH_FSYNC | DPHASH_LOCKLESS);
		if (!lm) return;
		char keys[N_DISK][32];
		for (int i = 0; i < N_DISK; i++) {
			make_key(keys[i], i);
			char v[32]; make_val(v, i);
			lsmtree_put(lm, keys[i], (u32)strlen(keys[i]), v, (u32)strlen(v));
		}
		int lookups = N_DISK * 10;
		BENCH("LSM-Tree", lookups, {
			for (int i = 0; i < lookups; i++) {
				char buf[256]; u32 vl = sizeof(buf);
				lsmtree_get_disk(lm, keys[i % N_DISK], (u32)strlen(keys[i % N_DISK]), buf, &vl);
			}
		});
		lsmtree_close(lm);
	}
}

int main(void)
{
	printf("========================================\n");
	printf("  DPHash v3: Fair Benchmark\n");
	printf("  All LOCKLESS, DiskGet uses pread()\n");
	printf("  vs B+Tree vs LSM-Tree (C, O3+LTO)\n");
	printf("========================================\n\n");

	test_correctness();

	bench_put();
	printf("\n");
	bench_get();
	printf("\n");
	bench_disk_put();
	printf("\n");
	bench_disk_get();

	return 0;
}
