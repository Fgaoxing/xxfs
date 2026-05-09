#include "xxfs.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: xxfs-mkfs <image> <size_mb>\n");
        return 1;
    }
    const char *path = argv[1];
    u64 size_mb = (u64)atol(argv[2]);
    if (size_mb < 1) {
        fprintf(stderr, "size must be >= 1 MB\n");
        return 1;
    }
    int rc = xxfs_mkfs(path, size_mb, 0);
    if (rc != XXFS_OK) {
        fprintf(stderr, "mkfs failed: %d\n", rc);
        return 1;
    }
    printf("XXFS: formatted %s (%lu MB)\n", path, (unsigned long)size_mb);
    return 0;
}
