#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "xxfs.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
    if (S_ISBLK(st.st_mode)) {
        u64 blk_size = 0;
        if (ioctl((int)p[0], BLKGETSIZE64, &blk_size) < 0)
            return -1;
        return (s64)blk_size;
    }
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
#ifdef __SSE4_2__
    const u8 *p = (const u8 *)data;
    u64 crc = 0xFFFFFFFFUL;
    while (len >= 8) {
        crc = __builtin_ia32_crc32di((u32)crc, *(const u64 *)p);
        p += 8;
        len -= 8;
    }
    while (len >= 4) {
        crc = __builtin_ia32_crc32si((u32)crc, *(const u32 *)p);
        p += 4;
        len -= 4;
    }
    while (len > 0) {
        crc = __builtin_ia32_crc32qi((u32)crc, *p);
        p++;
        len--;
    }
    return (u32)(crc ^ 0xFFFFFFFFUL);
#else
    crc32c_init();
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = crc32c_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
#endif
}

static const u8 _hamming_parity[256] = {
    0x00,0x69,0x6a,0x03,0x6c,0x05,0x06,0x6f,
    0x70,0x19,0x1a,0x73,0x1c,0x75,0x76,0x1f,
    0x78,0x11,0x12,0x7b,0x14,0x7d,0x7e,0x17,
    0x08,0x61,0x62,0x0b,0x64,0x0d,0x0e,0x67,
    0x60,0x09,0x0a,0x63,0x0c,0x65,0x66,0x0f,
    0x10,0x79,0x7a,0x13,0x7c,0x15,0x16,0x7f,
    0x18,0x71,0x72,0x1b,0x74,0x1d,0x1e,0x77,
    0x68,0x01,0x02,0x6b,0x04,0x6d,0x6e,0x07,
    0x48,0x21,0x22,0x4b,0x24,0x4d,0x4e,0x27,
    0x38,0x51,0x52,0x3b,0x54,0x3d,0x3e,0x57,
    0x30,0x59,0x5a,0x33,0x5c,0x35,0x36,0x5f,
    0x40,0x29,0x2a,0x43,0x2c,0x45,0x46,0x2f,
    0x28,0x41,0x42,0x2b,0x44,0x2d,0x2e,0x47,
    0x58,0x31,0x32,0x5b,0x34,0x5d,0x5e,0x37,
    0x50,0x39,0x3a,0x53,0x3c,0x55,0x56,0x3f,
    0x20,0x49,0x4a,0x23,0x4c,0x25,0x26,0x4f,
    0x90,0xf9,0xfa,0x93,0xfc,0x95,0x96,0xff,
    0xe0,0x89,0x8a,0xe3,0x8c,0xe5,0xe6,0x8f,
    0xe8,0x81,0x82,0xeb,0x84,0xed,0xee,0x87,
    0x98,0xf1,0xf2,0x9b,0xf4,0x9d,0x9e,0xf7,
    0xf0,0x99,0x9a,0xf3,0x9c,0xf5,0xf6,0x9f,
    0x80,0xe9,0xea,0x83,0xec,0x85,0x86,0xef,
    0x88,0xe1,0xe2,0x8b,0xe4,0x8d,0x8e,0xe7,
    0xd8,0xb1,0xb2,0xdb,0xb4,0xdd,0xde,0xb7,
    0xc8,0xa1,0xa2,0xcb,0xa4,0xcd,0xce,0xa7,
    0xb0,0xd9,0xda,0xb3,0xdc,0xb5,0xb6,0xdf,
    0xa0,0xc9,0xca,0xa3,0xcc,0xa5,0xa6,0xcf,
    0xb8,0xd1,0xd2,0xbb,0xd4,0xbd,0xbe,0xd7,
    0xc0,0xa9,0xaa,0xc3,0xac,0xc5,0xc6,0xaf,
    0xd0,0xb9,0xba,0xd3,0xbc,0xc5,0xc6,0xaf,
    0xa8,0xc1,0xc2,0xab,0xc4,0xad,0xae,0xc7,
};

void xxfs_os_ecc_compute(const void *data, size_t len, u8 *ecc_out)
{
    const u64 *p = (const u64 *)data;
    u32 nwords = (u32)((len + 7) / 8);
    for (u32 i = 0; i < nwords; i++) {
        u64 w = p[i];
        u8 b0 = _hamming_parity[(u8)(w)];
        u8 b1 = _hamming_parity[(u8)(w >> 8)];
        u8 b2 = _hamming_parity[(u8)(w >> 16)];
        u8 b3 = _hamming_parity[(u8)(w >> 24)];
        u8 b4 = _hamming_parity[(u8)(w >> 32)];
        u8 b5 = _hamming_parity[(u8)(w >> 40)];
        u8 b6 = _hamming_parity[(u8)(w >> 48)];
        u8 b7 = _hamming_parity[(u8)(w >> 56)];
        u8 ecc = b0 ^ b1 ^ b2 ^ b3 ^ b4 ^ b5 ^ b6 ^ b7;
        ecc |= (u8)(__builtin_parityll(w) << 7);
        ecc_out[i] = ecc;
    }
}

int xxfs_os_ecc_correct(void *data, size_t len, const u8 *ecc_stored)
{
    u8 ecc_computed[256];
    xxfs_os_ecc_compute(data, len, ecc_computed);
    
    const u64 *p64 = (const u64 *)data;
    u32 nwords = (u32)((len + 7) / 8);
    int corrected = 0;
    
    for (u32 i = 0; i < nwords; i++) {
        u8 syndrome = ecc_stored[i] ^ ecc_computed[i];
        if (syndrome == 0)
            continue;
        
        if (syndrome & 0x80) {
            u8 bit_pos = syndrome & 0x7F;
            if (bit_pos == 0 || bit_pos > 64)
                return XXFS_ECC_UNCORRECTABLE;
            u64 mask = (1ULL << (bit_pos - 1));
            u64 *w = (u64 *)p64 + i;
            *w ^= mask;
            corrected++;
        } else {
            return XXFS_ECC_UNCORRECTABLE;
        }
    }
    
    return corrected ? XXFS_ECC_CORRECTED : XXFS_ECC_OK;
}
