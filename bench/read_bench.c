#define _POSIX_C_SOURCE 199309L
#include "xxfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static u64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

int main(int argc, char *argv[])
{
    const char *path = argc > 1 ? argv[1] : "/dev/sda1";
    u32 nblocks = argc > 2 ? (u32)atoi(argv[2]) : 1024;

    /* 格式化+挂载 */
    xxfs_mkfs(path, 0, strncmp(path, "/dev/", 5) == 0 ? 0x100 : 0);
    struct xxfs *fs = xxfs_mount(path, XXFS_FLAG_NOLOCK);
    if (!fs) {
        fprintf(stderr, "mount failed\n");
        return 1;
    }

    /* 创建大文件 + 写数据 + sync 落盘 */
    xxfs_create(fs, "/readtest", 0644, 0, 0);
    struct xxfs_file *fp;
    xxfs_open(fs, "/readtest", 0, &fp);

    u64 total = (u64)nblocks * 4096;
    u8 *buf = malloc(4096);
    memset(buf, 0xAB, 4096);

    /* 预分配 + 写 */
    xxfs_write_fd(fs, fp, buf, 1, total - 1);
    for (u32 i = 0; i < nblocks; i++)
        xxfs_write_fd(fs, fp, buf, 4096, (u64)i * 4096);
    xxfs_sync(fs);
    xxfs_close(fs, fp);

    printf("Wrote %u blocks (%llu bytes), synced.\n", nblocks, (unsigned long long)total);

    /* 卸载 + 重新挂载，清除内核缓存 */
    xxfs_umount(fs);

    /* 清内核页缓存 */
    system("echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null");
    printf("Dropped kernel caches.\n");

    /* 重新挂载 */
    fs = xxfs_mount(path, XXFS_FLAG_NOLOCK);
    if (!fs) {
        fprintf(stderr, "mount failed (2nd)\n");
        return 1;
    }
    xxfs_open(fs, "/readtest", 0, &fp);

    /* 4K 顺序读 */
    u64 t0 = now_ns();
    for (u32 i = 0; i < nblocks; i++) {
        xxfs_read_fd(fs, fp, buf, 4096, (u64)i * 4096);
    }
    u64 t1 = now_ns();

    double us = (double)(t1 - t0) / 1000.0;
    double sec = us / 1e6;
    double mb_s = (double)total / (1024.0 * 1024.0) / sec;
    printf("\n--- Sequential 4K Read (from device) ---\n");
    printf("  %u blocks, %.2f us total\n", nblocks, us);
    printf("  %.3f us/op,  %.2f MB/s\n", us / nblocks, mb_s);

    /* 随机读 */
    u32 rand_iter = nblocks > 100000 ? 100000 : nblocks;
    t0 = now_ns();
    for (u32 i = 0; i < rand_iter; i++) {
        u32 idx = (u32)rand() % nblocks;
        xxfs_read_fd(fs, fp, buf, 4096, (u64)idx * 4096);
    }
    t1 = now_ns();

    us = (double)(t1 - t0) / 1000.0;
    sec = us / 1e6;
    double rand_mb_s = (double)(rand_iter * 4096) / (1024.0 * 1024.0) / sec;
    printf("\n--- Random 4K Read (from device) ---\n");
    printf("  %u iterations, %.2f us total\n", rand_iter, us);
    printf("  %.3f us/op,  %.2f MB/s\n", us / rand_iter, rand_mb_s);

    xxfs_close(fs, fp);
    xxfs_unlink(fs, "/readtest");

    /* 统计 */
    struct xxfs_fs_info info;
    xxfs_info(fs, &info);
    printf("\n  free blocks: %lu / %lu\n",
           (unsigned long)info.free_blocks, (unsigned long)info.block_count);

    xxfs_umount(fs);
    free(buf);
    return 0;
}