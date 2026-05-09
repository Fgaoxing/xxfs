#define _POSIX_C_SOURCE 199309L
#include "xxfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_IMG "/tmp/xxfs_bench.img"
#define BENCH_SIZE_MB 256
#define MAX_FILES 100000

static u64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static void print_result(const char *name, u64 elapsed_ns, u32 count)
{
    double us = (double)elapsed_ns / 1000.0;
    double ms = us / 1000.0;
    double sec = ms / 1000.0;
    double ops = (double)count / sec;
    double lat_us = us / (double)count;
    if (sec >= 1.0)
        printf("  %-28s %8u ops  %8.2f ms  %10.0f ops/s  %7.2f us/op\n",
               name, count, ms, ops, lat_us);
    else
        printf("  %-28s %8u ops  %8.2f us  %10.0f ops/s  %7.2f us/op\n",
               name, count, us, ops, lat_us);
}

static int bench_create(struct xxfs *fs, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/bench_%08u", i);
        xxfs_create(fs, path, 0644, 0, 0);
    }
    u64 t1 = now_ns();
    print_result("create (sequential)", t1 - t0, n);
    return 0;
}

static int bench_stat(struct xxfs *fs, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/bench_%08u", i);
        struct xxfs_inode ino;
        xxfs_stat(fs, path, &ino);
    }
    u64 t1 = now_ns();
    print_result("stat (sequential)", t1 - t0, n);
    return 0;
}

static int bench_stat_random(struct xxfs *fs, u32 n, u32 iter)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < iter; i++) {
        u32 idx = (u32)rand() % n;
        char path[64];
        snprintf(path, sizeof(path), "/bench_%08u", idx);
        struct xxfs_inode ino;
        xxfs_stat(fs, path, &ino);
    }
    u64 t1 = now_ns();
    print_result("stat (random)", t1 - t0, iter);
    return 0;
}

static int bench_write_inline(struct xxfs *fs, u32 n)
{
    const char *data = "Hello, XXFS benchmark data!";
    u32 dlen = (u32)strlen(data);
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/bench_%08u", i);
        u32 written;
        xxfs_write(fs, path, data, 0, dlen, &written);
    }
    u64 t1 = now_ns();
    print_result("write inline (sequential)", t1 - t0, n);
    return 0;
}

static int bench_read_inline(struct xxfs *fs, u32 n)
{
    char buf[256];
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/bench_%08u", i);
        u32 rb;
        xxfs_read(fs, path, buf, 0, sizeof(buf), &rb);
    }
    u64 t1 = now_ns();
    print_result("read inline (sequential)", t1 - t0, n);
    return 0;
}

static int bench_read_random(struct xxfs *fs, u32 n, u32 iter)
{
    char buf[256];
    u64 t0 = now_ns();
    for (u32 i = 0; i < iter; i++) {
        u32 idx = (u32)rand() % n;
        char path[64];
        snprintf(path, sizeof(path), "/bench_%08u", idx);
        u32 rb;
        xxfs_read(fs, path, buf, 0, sizeof(buf), &rb);
    }
    u64 t1 = now_ns();
    print_result("read inline (random)", t1 - t0, iter);
    return 0;
}

static int bench_unlink(struct xxfs *fs, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/bench_%08u", i);
        xxfs_unlink(fs, path);
    }
    u64 t1 = now_ns();
    print_result("unlink (sequential)", t1 - t0, n);
    return 0;
}

static int bench_mkdir(struct xxfs *fs, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dir_%08u", i);
        xxfs_mkdir(fs, path, 0755, 0, 0);
    }
    u64 t1 = now_ns();
    print_result("mkdir (sequential)", t1 - t0, n);
    return 0;
}

static int bench_readdir(struct xxfs *fs, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        struct xxfs_readdir_ctx ctx;
        xxfs_readdir(fs, "/", &ctx);
        xxfs_readdir_free(&ctx);
    }
    u64 t1 = now_ns();
    print_result("readdir /", t1 - t0, n);
    return 0;
}

static int bench_rmdir(struct xxfs *fs, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dir_%08u", i);
        xxfs_rmdir(fs, path);
    }
    u64 t1 = now_ns();
    print_result("rmdir (sequential)", t1 - t0, n);
    return 0;
}

static int bench_write_large(struct xxfs *fs, u32 block_count)
{
    xxfs_create(fs, "/largefile", 0644, 0, 0);
    u8 block[4096];
    for (u32 i = 0; i < sizeof(block); i++)
        block[i] = (u8)(i & 0xFF);

    u64 t0 = now_ns();
    for (u32 i = 0; i < block_count; i++) {
        u32 written;
        xxfs_write(fs, "/largefile", block, (u64)i * 4096, 4096, &written);
    }
    u64 t1 = now_ns();
    print_result("write 4K blocks (sequential)", t1 - t0, block_count);

    u64 total_bytes = (u64)block_count * 4096;
    double sec = (double)(t1 - t0) / 1e9;
    double mb_s = (double)total_bytes / (1024.0 * 1024.0) / sec;
    printf("  throughput: %.2f MB/s\n", mb_s);

    xxfs_unlink(fs, "/largefile");
    return 0;
}

static int bench_sync(struct xxfs *fs, u32 iter)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < iter; i++)
        xxfs_sync(fs);
    u64 t1 = now_ns();
    print_result("sync", t1 - t0, iter);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: xxfs-bench [options]\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -n <count>    number of files (default: 10000)\n");
    fprintf(stderr, "  -i <iter>     random iterations (default: same as -n)\n");
    fprintf(stderr, "  -s <size_mb>  image size in MB (default: 256)\n");
    fprintf(stderr, "  -o <path>     image path (default: /tmp/xxfs_bench.img)\n");
    fprintf(stderr, "  -h            show this help\n");
}

int main(int argc, char *argv[])
{
    u32 nfiles = 10000;
    u32 rand_iter = 0;
    u32 size_mb = BENCH_SIZE_MB;
    const char *img = BENCH_IMG;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            nfiles = (u32)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            rand_iter = (u32)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            size_mb = (u32)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            img = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
    }

    if (rand_iter == 0)
        rand_iter = nfiles;
    if (nfiles > MAX_FILES)
        nfiles = MAX_FILES;

    srand((unsigned)time(NULL));

    printf("XXFS Benchmark\n");
    printf("==============\n");
    printf("  files:      %u\n", nfiles);
    printf("  rand_iter:  %u\n", rand_iter);
    printf("  image:      %s (%u MB)\n", img, size_mb);
    printf("\n");

    printf("[1/12] mkfs...\n");
    xxfs_mkfs(img, size_mb, 0);

    printf("[2/12] mount...\n");
    struct xxfs *fs = xxfs_mount(img, XXFS_FLAG_NOLOCK);
    if (!fs) {
        fprintf(stderr, "bench: mount failed\n");
        return 1;
    }

    printf("\n--- Create / Stat ---\n");
    bench_create(fs, nfiles);
    bench_stat(fs, nfiles);
    bench_stat_random(fs, nfiles, rand_iter);

    printf("\n--- Write / Read (inline) ---\n");
    bench_write_inline(fs, nfiles);
    bench_read_inline(fs, nfiles);
    bench_read_random(fs, nfiles, rand_iter);

    printf("\n--- Large File ---\n");
    bench_write_large(fs, 256);

    printf("\n--- Directory ---\n");
    u32 dir_count = nfiles > 1000 ? 1000 : nfiles;
    bench_mkdir(fs, dir_count);
    bench_readdir(fs, 100);
    bench_rmdir(fs, dir_count);

    printf("\n--- Sync ---\n");
    bench_sync(fs, 5);

    printf("\n--- Delete ---\n");
    bench_unlink(fs, nfiles);

    printf("\n--- Summary ---\n");
    struct xxfs_fs_info info;
    xxfs_info(fs, &info);
    printf("  total inodes created: %lu\n", (unsigned long)info.inodes_count);
    printf("  free blocks: %lu / %lu\n",
           (unsigned long)info.free_blocks, (unsigned long)info.block_count);

#ifdef XXFS_PROFILE
    struct xxfs_profile prof;
    xxfs_get_profile(fs, &prof);
    printf("\n--- CPU Profile (create: %u ops) ---\n", prof.create_cnt);
    if (prof.create_cnt > 0) {
        double cnt = (double)prof.create_cnt;
        printf("  hash:      %7.2f us/op  (%.1f%%)\n",
               (double)prof.hash_ns / cnt / 1000.0,
               (double)prof.hash_ns / (double)(prof.hash_ns + prof.icache_ns + prof.pcache_ns + prof.dcache_ns) * 100.0);
        printf("  icache:    %7.2f us/op  (%.1f%%)\n",
               (double)prof.icache_ns / cnt / 1000.0,
               (double)prof.icache_ns / (double)(prof.hash_ns + prof.icache_ns + prof.pcache_ns + prof.dcache_ns) * 100.0);
        printf("  pcache:    %7.2f us/op  (%.1f%%)\n",
               (double)prof.pcache_ns / cnt / 1000.0,
               (double)prof.pcache_ns / (double)(prof.hash_ns + prof.icache_ns + prof.pcache_ns + prof.dcache_ns) * 100.0);
        printf("  dcache:    %7.2f us/op  (%.1f%%)\n",
               (double)prof.dcache_ns / cnt / 1000.0,
               (double)prof.dcache_ns / (double)(prof.hash_ns + prof.icache_ns + prof.pcache_ns + prof.dcache_ns) * 100.0);
        printf("  pwrite:    %7.2f us/op\n", (double)prof.pwrite_ns / cnt / 1000.0);
        printf("  pread:     %7.2f us/op\n", (double)prof.pread_ns / cnt / 1000.0);
        printf("  alloc:     %7.2f us/op\n", (double)prof.alloc_ns / cnt / 1000.0);
    }
#endif

    xxfs_umount(fs);

    printf("\nDone.\n");
    return 0;
}
