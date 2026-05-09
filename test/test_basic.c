#include "xxfs.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define TEST_FILE "/tmp/xxfs_test.img"

static int test_mkfs(void)
{
    printf("=== test_mkfs ===\n");
    int rc = xxfs_mkfs(TEST_FILE, 64, 0);
    if (rc != XXFS_OK) { fprintf(stderr, "mkfs failed: %d\n", rc); return 1; }
    printf("  mkfs OK\n");
    return 0;
}

static int test_mount_umount(void)
{
    printf("=== test_mount_umount ===\n");
    struct xxfs *fs = xxfs_mount(TEST_FILE, 0);
    if (!fs) { fprintf(stderr, "mount failed\n"); return 1; }
    printf("  mount OK, inodes=%lu\n", (unsigned long)xxfs_count(fs));
    xxfs_umount(fs);
    printf("  umount OK\n");
    return 0;
}

static int test_create_stat(void)
{
    printf("=== test_create_stat ===\n");
    struct xxfs *fs = xxfs_mount(TEST_FILE, XXFS_FLAG_SYNC);
    if (!fs) return 1;

    int rc = xxfs_create(fs, "/hello.txt", 0644, 1000, 1000);
    if (rc != XXFS_OK) { fprintf(stderr, "create failed: %d\n", rc); xxfs_umount(fs); return 1; }
    printf("  create /hello.txt OK\n");

    struct xxfs_inode ino;
    rc = xxfs_stat(fs, "/hello.txt", &ino);
    if (rc != XXFS_OK) { fprintf(stderr, "stat failed: %d\n", rc); xxfs_umount(fs); return 1; }
    printf("  stat: mode=%o type=%d uid=%d gid=%d size=%lu name=%s\n",
           ino.i_mode, ino.i_file_type, ino.i_uid, ino.i_gid,
           (unsigned long)ino.i_size, ino.i_name);
    assert(ino.i_file_type == XXFS_FT_REG);
    assert(ino.i_uid == 1000);
    assert(ino.i_size == 0);

    rc = xxfs_create(fs, "/hello.txt", 0644, 1000, 1000);
    if (rc != XXFS_EEXIST) { fprintf(stderr, "duplicate create should fail\n"); xxfs_umount(fs); return 1; }
    printf("  duplicate create rejected OK\n");

    xxfs_umount(fs);
    return 0;
}

static int test_mkdir(void)
{
    printf("=== test_mkdir ===\n");
    struct xxfs *fs = xxfs_mount(TEST_FILE, XXFS_FLAG_SYNC);
    if (!fs) return 1;

    int rc = xxfs_mkdir(fs, "/mydir", 0755, 0, 0);
    if (rc != XXFS_OK) { fprintf(stderr, "mkdir failed: %d\n", rc); xxfs_umount(fs); return 1; }
    printf("  mkdir /mydir OK\n");

    struct xxfs_inode ino;
    rc = xxfs_stat(fs, "/mydir", &ino);
    if (rc != XXFS_OK) { fprintf(stderr, "stat dir failed: %d\n", rc); xxfs_umount(fs); return 1; }
    assert(ino.i_file_type == XXFS_FT_DIR);
    printf("  stat dir: type=%d\n", ino.i_file_type);

    xxfs_umount(fs);
    return 0;
}

static int test_write_read(void)
{
    printf("=== test_write_read ===\n");
    struct xxfs *fs = xxfs_mount(TEST_FILE, XXFS_FLAG_SYNC);
    if (!fs) return 1;

    int rc = xxfs_create(fs, "/data.txt", 0644, 0, 0);
    if (rc != XXFS_OK) { fprintf(stderr, "create failed: %d\n", rc); xxfs_umount(fs); return 1; }

    const char *data = "Hello, XXFS World!";
    u32 written;
    rc = xxfs_write(fs, "/data.txt", data, 0, (u32)strlen(data), &written);
    if (rc != XXFS_OK) { fprintf(stderr, "write failed: %d\n", rc); xxfs_umount(fs); return 1; }
    printf("  write %u bytes OK\n", written);

    char buf[256] = {0};
    u32 read_bytes;
    rc = xxfs_read(fs, "/data.txt", buf, 0, sizeof(buf), &read_bytes);
    if (rc != XXFS_OK) { fprintf(stderr, "read failed: %d\n", rc); xxfs_umount(fs); return 1; }
    printf("  read %u bytes: '%s'\n", read_bytes, buf);
    assert(read_bytes == strlen(data));
    assert(memcmp(buf, data, strlen(data)) == 0);

    xxfs_umount(fs);
    return 0;
}

static int test_unlink(void)
{
    printf("=== test_unlink ===\n");
    struct xxfs *fs = xxfs_mount(TEST_FILE, XXFS_FLAG_SYNC);
    if (!fs) return 1;

    int rc = xxfs_create(fs, "/temp.txt", 0644, 0, 0);
    if (rc != XXFS_OK) { fprintf(stderr, "create failed\n"); xxfs_umount(fs); return 1; }

    rc = xxfs_unlink(fs, "/temp.txt");
    if (rc != XXFS_OK) { fprintf(stderr, "unlink failed: %d\n", rc); xxfs_umount(fs); return 1; }
    printf("  unlink OK\n");

    struct xxfs_inode ino;
    rc = xxfs_stat(fs, "/temp.txt", &ino);
    if (rc != XXFS_ENOENT) { fprintf(stderr, "stat should fail after unlink\n"); xxfs_umount(fs); return 1; }
    printf("  stat after unlink returns ENOENT OK\n");

    xxfs_umount(fs);
    return 0;
}

static int test_rename(void)
{
    printf("=== test_rename ===\n");
    struct xxfs *fs = xxfs_mount(TEST_FILE, XXFS_FLAG_SYNC);
    if (!fs) return 1;

    xxfs_create(fs, "/old.txt", 0644, 0, 0);
    const char *data = "renamed file";
    u32 written;
    xxfs_write(fs, "/old.txt", data, 0, (u32)strlen(data), &written);

    int rc = xxfs_rename(fs, "/old.txt", "/new.txt");
    if (rc != XXFS_OK) { fprintf(stderr, "rename failed: %d\n", rc); xxfs_umount(fs); return 1; }
    printf("  rename OK\n");

    struct xxfs_inode ino;
    if (xxfs_stat(fs, "/old.txt", &ino) != XXFS_ENOENT) { fprintf(stderr, "old should not exist\n"); xxfs_umount(fs); return 1; }
    if (xxfs_stat(fs, "/new.txt", &ino) != XXFS_OK) { fprintf(stderr, "new should exist\n"); xxfs_umount(fs); return 1; }
    printf("  old gone, new exists OK\n");

    char buf[256] = {0};
    u32 rb;
    xxfs_read(fs, "/new.txt", buf, 0, sizeof(buf), &rb);
    assert(rb == strlen(data));
    assert(memcmp(buf, data, strlen(data)) == 0);
    printf("  data after rename intact OK\n");

    xxfs_umount(fs);
    return 0;
}

static int test_chmod_chown(void)
{
    printf("=== test_chmod_chown ===\n");
    struct xxfs *fs = xxfs_mount(TEST_FILE, XXFS_FLAG_SYNC);
    if (!fs) return 1;

    xxfs_create(fs, "/perm.txt", 0644, 0, 0);

    int rc = xxfs_chmod(fs, "/perm.txt", 0755);
    if (rc != XXFS_OK) { fprintf(stderr, "chmod failed\n"); xxfs_umount(fs); return 1; }

    rc = xxfs_chown(fs, "/perm.txt", 123, 456);
    if (rc != XXFS_OK) { fprintf(stderr, "chown failed\n"); xxfs_umount(fs); return 1; }

    struct xxfs_inode ino;
    xxfs_stat(fs, "/perm.txt", &ino);
    assert(ino.i_mode == 0755);
    assert(ino.i_uid == 123);
    assert(ino.i_gid == 456);
    printf("  chmod=%o chown uid=%d gid=%d OK\n", ino.i_mode, ino.i_uid, ino.i_gid);

    xxfs_umount(fs);
    return 0;
}

static int test_symlink(void)
{
    printf("=== test_symlink ===\n");
    struct xxfs *fs = xxfs_mount(TEST_FILE, XXFS_FLAG_SYNC);
    if (!fs) return 1;

    xxfs_create(fs, "/target.txt", 0644, 0, 0);
    int rc = xxfs_symlink(fs, "/target.txt", "/link.txt");
    if (rc != XXFS_OK) { fprintf(stderr, "symlink failed: %d\n", rc); xxfs_umount(fs); return 1; }
    printf("  symlink OK\n");

    char buf[256];
    u32 len = sizeof(buf);
    rc = xxfs_readlink(fs, "/link.txt", buf, &len);
    if (rc != XXFS_OK) { fprintf(stderr, "readlink failed: %d\n", rc); xxfs_umount(fs); return 1; }
    printf("  readlink: '%s'\n", buf);
    assert(memcmp(buf, "/target.txt", 11) == 0);

    xxfs_umount(fs);
    return 0;
}

static int test_batch(void)
{
    printf("=== test_batch (1000 files) ===\n");
    struct xxfs *fs = xxfs_mount(TEST_FILE, XXFS_FLAG_SYNC);
    if (!fs) return 1;

    const int n = 1000;
    int fail = 0;
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/file_%08d", i);
        int rc = xxfs_create(fs, path, 0644, 0, 0);
        if (rc != XXFS_OK) { fail++; continue; }
        char data[32];
        snprintf(data, sizeof(data), "val_%08d", i);
        u32 written;
        rc = xxfs_write(fs, path, data, 0, (u32)strlen(data), &written);
        if (rc != XXFS_OK) { fail++; }
    }
    printf("  create+write: %d failures / %d\n", fail, n);

    fail = 0;
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/file_%08d", i);
        char data[32];
        snprintf(data, sizeof(data), "val_%08d", i);
        char buf[256] = {0};
        u32 rb;
        int rc = xxfs_read(fs, path, buf, 0, sizeof(buf), &rb);
        if (rc != XXFS_OK || rb != strlen(data) || memcmp(buf, data, rb) != 0) {
            fail++;
        }
    }
    printf("  read verify: %d failures / %d\n", fail, n);

    printf("  total inodes: %lu\n", (unsigned long)xxfs_count(fs));
    xxfs_umount(fs);
    return fail > 0 ? 1 : 0;
}

int main(void)
{
    int fail = 0;
    fail += test_mkfs();
    fail += test_mount_umount();
    fail += test_create_stat();
    fail += test_mkdir();
    fail += test_write_read();
    fail += test_unlink();
    fail += test_rename();
    fail += test_chmod_chown();
    fail += test_symlink();
    fail += test_batch();
    printf("\n=== %s ===\n", fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fail;
}
