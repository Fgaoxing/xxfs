#ifndef _DPHASH_OS_H
#define _DPHASH_OS_H

#include <stddef.h>
#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t s64;
typedef int32_t s32;
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
typedef _Bool bool;
#define true 1
#define false 0
#endif

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#define always_inline __attribute__((always_inline)) inline
#define __cold __attribute__((cold))
#define __aligned(x) __attribute__((aligned(x)))
#define prefetch_r(p) __builtin_prefetch((p), 0, 1)
#define prefetch_w(p) __builtin_prefetch((p), 1, 1)

#define DPHASH_OK 0
#define DPHASH_ENOENT -2
#define DPHASH_EEXIST -17
#define DPHASH_ENOMEM -12
#define DPHASH_ENOSPC -28
#define DPHASH_EIO -5
#define DPHASH_EINVAL -22

#define DPHASH_NOSYNC 0x00U
#define DPHASH_FSYNC 0x01U
#define DPHASH_PAGED 0x04U
#define DPHASH_LOCKLESS 0x08U

struct dphash;

struct dphash *dphash_open(const char *path, u64 flags);
void dphash_close(struct dphash *dp);

int dphash_put(struct dphash *dp, const void *key, u32 klen,
               const void *val, u32 vlen);
int dphash_get(struct dphash *dp, const void *key, u32 klen,
               void *buf, u32 *vlen);
int dphash_get_disk(struct dphash *dp, const void *key, u32 klen,
                    void *buf, u32 *vlen);
int dphash_del(struct dphash *dp, const void *key, u32 klen);

u64 dphash_count(const struct dphash *dp);
int dphash_sync(struct dphash *dp);

size_t dphash_mem_usage(const struct dphash *dp);

#ifdef _DPHASH_OS_IMPL

struct dphash_lock {
    void *opaque[4];
};

struct dphash_file {
    void *opaque[4];
};

void *dph_os_alloc(size_t size);
void *dph_os_zalloc(size_t size);
void dph_os_free(void *ptr);

void dph_os_lock_init(struct dphash_lock *l);
void dph_os_lock_destroy(struct dphash_lock *l);
void dph_os_read_lock(struct dphash_lock *l);
void dph_os_read_unlock(struct dphash_lock *l);
void dph_os_write_lock(struct dphash_lock *l);
void dph_os_write_unlock(struct dphash_lock *l);

int dph_os_file_open(struct dphash_file *f, const char *path);
void dph_os_file_close(struct dphash_file *f);
s64 dph_os_file_size(struct dphash_file *f);
int dph_os_file_pwrite(struct dphash_file *f, const void *buf,
                       size_t len, s64 off);
int dph_os_file_pread(struct dphash_file *f, void *buf,
                      size_t len, s64 off);
int dph_os_file_sync(struct dphash_file *f);
int dph_os_file_truncate(struct dphash_file *f, s64 size);
int dph_os_file_extend(struct dphash_file *f, s64 size);
void *dph_os_file_mmap(struct dphash_file *f, size_t len);
void dph_os_file_munmap(void *ptr, size_t len);
int dph_os_file_msync(void *ptr, size_t len);
int dph_os_mkdir(const char *path);

void *dph_os_memcpy(void *dst, const void *src, size_t n);
int dph_os_memcmp(const void *a, const void *b, size_t n);
void *dph_os_memset(void *dst, int c, size_t n);

#endif

#endif
