#include "xxfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_errors;

static int check_super(struct xxfs *fs)
{
    printf("[super] checking superblock...\n");
    struct xxfs_fs_info info;
    if (xxfs_info(fs, &info) != XXFS_OK) {
        printf("  ERROR: cannot read filesystem info\n");
        g_errors++;
        return -1;
    }
    printf("  version=%u block_size=%u block_count=%lu free_blocks=%lu inodes=%lu\n",
           info.version, info.block_size,
           (unsigned long)info.block_count, (unsigned long)info.free_blocks,
           (unsigned long)info.inodes_count);
    return 0;
}

static int check_root(struct xxfs *fs)
{
    printf("[root] checking root directory...\n");
    struct xxfs_inode ino;
    int rc = xxfs_stat(fs, "/", &ino);
    if (rc != XXFS_OK) {
        printf("  ERROR: root inode not found! (rc=%d)\n", rc);
        g_errors++;
        return -1;
    }
    if (ino.i_file_type != XXFS_FT_DIR) {
        printf("  ERROR: root is not a directory (type=%u)\n", ino.i_file_type);
        g_errors++;
    }
    printf("  root: type=%s mode=%o size=%lu checksum=0x%08x\n",
           ino.i_file_type == XXFS_FT_DIR ? "dir" : "unknown",
           ino.i_mode, (unsigned long)ino.i_size, ino.i_checksum);

    struct xxfs_readdir_ctx ctx;
    rc = xxfs_readdir(fs, "/", &ctx);
    if (rc == XXFS_OK) {
        printf("  root children: %u\n", ctx.count);
        for (u32 i = 0; i < ctx.count; i++) {
            printf("    %s  %s\n",
                   ctx.entries[i].dc_type == XXFS_FT_DIR ? "dir " : "file",
                   ctx.entries[i].dc_name);
        }
        xxfs_readdir_free(&ctx);
    }
    return 0;
}

static int check_path(struct xxfs *fs, const char *path)
{
    struct xxfs_inode ino;
    int rc = xxfs_stat(fs, path, &ino);
    if (rc != XXFS_OK) {
        printf("  ERROR: cannot stat %s (rc=%d)\n", path, rc);
        g_errors++;
        return -1;
    }
    u32 crc_off = (u32)((char *)&ino.i_checksum - (char *)&ino);
    u32 crc = xxfs_os_crc32c(&ino, crc_off);
    if (crc != ino.i_checksum) {
        printf("  ERROR: checksum mismatch for %s (expected 0x%08x got 0x%08x)\n",
               path, ino.i_checksum, crc);
        g_errors++;
    }
    if (ino.i_file_type == XXFS_FT_DIR) {
        struct xxfs_readdir_ctx ctx;
        rc = xxfs_readdir(fs, path, &ctx);
        if (rc == XXFS_OK) {
            for (u32 i = 0; i < ctx.count; i++) {
                char child[XXFS_MAX_PATH];
                if (strcmp(path, "/") == 0)
                    snprintf(child, sizeof(child), "/%s", ctx.entries[i].dc_name);
                else
                    snprintf(child, sizeof(child), "%s/%s", path, ctx.entries[i].dc_name);
                check_path(fs, child);
            }
            xxfs_readdir_free(&ctx);
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: xxfs-fsck <image> [--repair]\n");
        return 1;
    }

    const char *img = argv[1];
    struct xxfs *fs = xxfs_mount(img, 0);
    if (!fs) {
        fprintf(stderr, "fsck: cannot mount %s\n", img);
        return 1;
    }

    printf("XXFS fsck - checking %s\n\n", img);
    g_errors = 0;

    check_super(fs);
    check_root(fs);

    printf("\n[tree] recursively checking all inodes...\n");
    check_path(fs, "/");

    printf("\n");
    if (g_errors == 0)
        printf("fsck: no errors found\n");
    else
        printf("fsck: %d error(s) found\n", g_errors);

    xxfs_umount(fs);
    return g_errors > 0 ? 1 : 0;
}
