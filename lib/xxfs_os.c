#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "xxfs.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void *xxfs_os_alloc(size_t size) { return malloc(size); }
void *xxfs_os_zalloc(size_t size) { return calloc(1, size); }
void xxfs_os_free(void *ptr) { free(ptr); }
void *xxfs_os_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
int xxfs_os_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
void *xxfs_os_memset(void *d, int c, size_t n) { return memset(d, c, n); }

int xxfs_os_file_open(struct xxfs_os_file *f, const char *path, int flags)
{
    int oflags = O_RDWR;
    if (flags & 1)
        oflags |= O_CREAT;
    if (flags & 2)
        oflags |= O_TRUNC;
    int fd = open(path, oflags, 0644);
    if (fd < 0)
        return -1;
    s64 *p = (s64 *)f->opaque;
    p[0] = fd;
    return 0;
}

void xxfs_os_file_close(struct xxfs_os_file *f)
{
    s64 *p = (s64 *)f->opaque;
    if (p[0] >= 0)
        close((int)p[0]);
    p[0] = -1;
}

s64 xxfs_os_file_size(struct xxfs_os_file *f)
{
    s64 *p = (s64 *)f->opaque;
    struct stat st;
    if (fstat((int)p[0], &st) < 0)
        return -1;
    return st.st_size;
}

int xxfs_os_file_pwrite(struct xxfs_os_file *f, const void *buf, size_t len, s64 off)
{
    s64 *p = (s64 *)f->opaque;
    ssize_t r = pwrite((int)p[0], buf, len, off);
    return (r < 0 || (size_t)r != len) ? -1 : 0;
}

int xxfs_os_file_pread(struct xxfs_os_file *f, void *buf, size_t len, s64 off)
{
    s64 *p = (s64 *)f->opaque;
    ssize_t r = pread((int)p[0], buf, len, off);
    return (r < 0) ? -1 : (size_t)r;
}

int xxfs_os_file_sync(struct xxfs_os_file *f)
{
    s64 *p = (s64 *)f->opaque;
    return fsync((int)p[0]);
}

int xxfs_os_file_truncate(struct xxfs_os_file *f, s64 size)
{
    s64 *p = (s64 *)f->opaque;
    return ftruncate((int)p[0], size);
}

int xxfs_os_file_extend(struct xxfs_os_file *f, s64 size)
{
    s64 *p = (s64 *)f->opaque;
    s64 cur = xxfs_os_file_size(f);
    if (cur < 0)
        return -1;
    if (cur >= size)
        return 0;
    return ftruncate((int)p[0], size);
}

void *xxfs_os_file_mmap(struct xxfs_os_file *f, size_t len)
{
    s64 *p = (s64 *)f->opaque;
    void *ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)p[0], 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

void xxfs_os_file_munmap(void *ptr, size_t len)
{
    if (ptr)
        munmap(ptr, len);
}

int xxfs_os_file_msync(void *ptr, size_t len)
{
    if (!ptr)
        return -1;
    return msync(ptr, len, MS_SYNC);
}

int xxfs_os_mkdir(const char *path, u32 mode)
{
    return mkdir(path, mode);
}

int xxfs_os_rmdir(const char *path)
{
    return rmdir(path);
}

void xxfs_os_lock_init(struct xxfs_os_lock *l)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)l->opaque;
    pthread_rwlock_init(rw, NULL);
}

void xxfs_os_lock_destroy(struct xxfs_os_lock *l)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)l->opaque;
    pthread_rwlock_destroy(rw);
}

void xxfs_os_read_lock(struct xxfs_os_lock *l)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)l->opaque;
    pthread_rwlock_rdlock(rw);
}

void xxfs_os_read_unlock(struct xxfs_os_lock *l)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)l->opaque;
    pthread_rwlock_unlock(rw);
}

void xxfs_os_write_lock(struct xxfs_os_lock *l)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)l->opaque;
    pthread_rwlock_wrlock(rw);
}

void xxfs_os_write_unlock(struct xxfs_os_lock *l)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)l->opaque;
    pthread_rwlock_unlock(rw);
}

u64 xxfs_os_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static u32 crc32c_table[256];
static int crc32c_init_done;

static void crc32c_init(void)
{
    if (crc32c_init_done)
        return;
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0x82F63B78U ^ (c >> 1)) : (c >> 1);
        crc32c_table[i] = c;
    }
    crc32c_init_done = 1;
}

u32 xxfs_os_crc32c(const void *data, size_t len)
{
    crc32c_init();
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = crc32c_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}
