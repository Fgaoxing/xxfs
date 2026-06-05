#include "xxfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: mkfs.xxfs <device|image> [size_mb]\n");
        fprintf(stderr, "  device: /dev/sdXN  (auto-detect size)\n");
        fprintf(stderr, "  image:  file path  (requires size_mb)\n");
        return 1;
    }
    const char *path = argv[1];
    int is_blkdev = (strncmp(path, "/dev/", 5) == 0);
    u64 size_mb = 0;
    u32 flags = 0;

    if (is_blkdev) {
        flags = 0x100;
    } else {
        if (argc < 3) {
            fprintf(stderr, "error: image file requires size_mb\n");
            return 1;
        }
        size_mb = (u64)atol(argv[2]);
        if (size_mb < 1) {
            fprintf(stderr, "size must be >= 1 MB\n");
            return 1;
        }
    }

    int rc = xxfs_mkfs(path, size_mb, flags);
    if (rc != XXFS_OK) {
        fprintf(stderr, "mkfs failed: %d\n", rc);
        return 1;
    }
    printf("XXFS: formatted %s%s\n", path,
           is_blkdev ? " (block device)" : "");
    return 0;
}
