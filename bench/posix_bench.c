#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

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
    printf("  %-28s %8u ops  %10.2f us  %10.0f ops/s  %7.2f us/op\n",
           name, count, us, ops, lat_us);
}

static int bench_create(const char *dir, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/bench_%08u", dir, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_EXCL, 0644);
        if (fd < 0) { continue; }
        close(fd);
    }
    u64 t1 = now_ns();
    print_result("create (sequential)", t1 - t0, n);
    return 0;
}

static int bench_stat(const char *dir, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/bench_%08u", dir, i);
        struct stat st;
        stat(path, &st);
    }
    u64 t1 = now_ns();
    print_result("stat (sequential)", t1 - t0, n);
    return 0;
}

static int bench_stat_random(const char *dir, u32 n, u32 iter)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < iter; i++) {
        u32 idx = (u32)rand() % n;
        char path[512];
        snprintf(path, sizeof(path), "%s/bench_%08u", dir, idx);
        struct stat st;
        stat(path, &st);
    }
    u64 t1 = now_ns();
    print_result("stat (random)", t1 - t0, iter);
    return 0;
}

static int bench_write_small(const char *dir, u32 n)
{
    const char *data = "Hello, POSIX benchmark data!";
    u32 dlen = (u32)strlen(data);
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/bench_%08u", dir, i);
        int fd = open(path, O_WRONLY);
        if (fd < 0) continue;
        write(fd, data, dlen);
        close(fd);
    }
    u64 t1 = now_ns();
    print_result("write small (sequential)", t1 - t0, n);
    return 0;
}

static int bench_read_small(const char *dir, u32 n)
{
    char buf[256];
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/bench_%08u", dir, i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        read(fd, buf, sizeof(buf));
        close(fd);
    }
    u64 t1 = now_ns();
    print_result("read small (sequential)", t1 - t0, n);
    return 0;
}

static int bench_read_random(const char *dir, u32 n, u32 iter)
{
    char buf[256];
    u64 t0 = now_ns();
    for (u32 i = 0; i < iter; i++) {
        u32 idx = (u32)rand() % n;
        char path[512];
        snprintf(path, sizeof(path), "%s/bench_%08u", dir, idx);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        read(fd, buf, sizeof(buf));
        close(fd);
    }
    u64 t1 = now_ns();
    print_result("read small (random)", t1 - t0, iter);
    return 0;
}

static int bench_write_large(const char *dir, u32 block_count)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/largefile", dir);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;

    u8 block[4096];
    for (u32 i = 0; i < sizeof(block); i++)
        block[i] = (u8)(i & 0xFF);

    u64 t0 = now_ns();
    for (u32 i = 0; i < block_count; i++) {
        write(fd, block, 4096);
    }
    fsync(fd);
    u64 t1 = now_ns();
    close(fd);

    print_result("write 4K blocks (sequential)", t1 - t0, block_count);
    u64 total_bytes = (u64)block_count * 4096;
    double sec = (double)(t1 - t0) / 1e9;
    double mb_s = (double)total_bytes / (1024.0 * 1024.0) / sec;
    printf("  throughput: %.2f MB/s\n", mb_s);

    unlink(path);
    return 0;
}

static int bench_mkdir(const char *dir, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/dir_%08u", dir, i);
        mkdir(path, 0755);
    }
    u64 t1 = now_ns();
    print_result("mkdir (sequential)", t1 - t0, n);
    return 0;
}

static int bench_readdir(const char *dir, u32 iter)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < iter; i++) {
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            (void)de;
        }
        closedir(d);
    }
    u64 t1 = now_ns();
    print_result("readdir", t1 - t0, iter);
    return 0;
}

static int bench_rmdir(const char *dir, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/dir_%08u", dir, i);
        rmdir(path);
    }
    u64 t1 = now_ns();
    print_result("rmdir (sequential)", t1 - t0, n);
    return 0;
}

static int bench_sync(const char *dir, u32 iter)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < iter; i++) {
        sync();
    }
    u64 t1 = now_ns();
    print_result("sync", t1 - t0, iter);
    return 0;
}

static int bench_unlink(const char *dir, u32 n)
{
    u64 t0 = now_ns();
    for (u32 i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/bench_%08u", dir, i);
        unlink(path);
    }
    u64 t1 = now_ns();
    print_result("unlink (sequential)", t1 - t0, n);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: posix-bench <test_dir> [options]\n");
        fprintf(stderr, "options:\n");
        fprintf(stderr, "  -n <count>    number of files (default: 10000)\n");
        fprintf(stderr, "  -i <iter>     random iterations (default: same as -n)\n");
        return 1;
    }

    const char *dir = argv[1];
    u32 nfiles = 10000;
    u32 rand_iter = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            nfiles = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            rand_iter = (u32)atoi(argv[++i]);
    }

    if (rand_iter == 0)
        rand_iter = nfiles;
    if (nfiles > MAX_FILES)
        nfiles = MAX_FILES;

    srand((unsigned)time(NULL));

    printf("POSIX Benchmark on: %s\n", dir);
    printf("files: %u  rand_iter: %u\n\n", nfiles, rand_iter);

    printf("--- Create / Stat ---\n");
    bench_create(dir, nfiles);
    bench_stat(dir, nfiles);
    bench_stat_random(dir, nfiles, rand_iter);

    printf("\n--- Write / Read (small) ---\n");
    bench_write_small(dir, nfiles);
    bench_read_small(dir, nfiles);
    bench_read_random(dir, nfiles, rand_iter);

    printf("\n--- Large File ---\n");
    bench_write_large(dir, 256);

    printf("\n--- Directory ---\n");
    u32 dir_count = nfiles > 1000 ? 1000 : nfiles;
    bench_mkdir(dir, dir_count);
    bench_readdir(dir, 100);
    bench_rmdir(dir, dir_count);

    printf("\n--- Sync ---\n");
    bench_sync(dir, 5);

    printf("\n--- Delete ---\n");
    bench_unlink(dir, nfiles);

    printf("\nDone.\n");
    return 0;
}
