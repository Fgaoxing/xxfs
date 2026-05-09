#include "lsmtree.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SL_MAX_HEIGHT 20
#define MEMTABLE_CAP 4096
#define SST_MAX_TABLES 64

struct sl_node {
    u64 hash;
    void *key;
    u32 klen;
    void *val;
    u32 vlen;
    s64 file_off;
    u8 deleted;
    int height;
    struct sl_node *next[];
};

struct sst_entry {
    u64 hash;
    u32 klen;
    u32 vlen;
    s64 file_off;
    u8 deleted;
};

struct sst_table {
    struct sst_entry *entries;
    u32 count;
    u32 cap;
    int fd;
};

struct lm_arena {
    struct lm_arena *next;
    u32 used;
    u32 cap;
    u8 data[];
};

struct lsmtree {
    struct sl_node *head;
    int max_height;
    u64 count;

    struct sst_table ssts[SST_MAX_TABLES];
    int nr_ssts;

    struct lm_arena *arena_head;
    struct lm_arena *arena_cur;

    int data_fd;
    s64 data_off;
    u64 flags;
    pthread_rwlock_t rwlock;
};

#define LM_RLOCK(lm)                              \
    do {                                          \
        if (!((lm)->flags & DPHASH_LOCKLESS))     \
            pthread_rwlock_rdlock(&(lm)->rwlock); \
    } while (0)
#define LM_RUNLOCK(lm)                            \
    do {                                          \
        if (!((lm)->flags & DPHASH_LOCKLESS))     \
            pthread_rwlock_unlock(&(lm)->rwlock); \
    } while (0)
#define LM_WLOCK(lm)                              \
    do {                                          \
        if (!((lm)->flags & DPHASH_LOCKLESS))     \
            pthread_rwlock_wrlock(&(lm)->rwlock); \
    } while (0)
#define LM_WUNLOCK(lm)                            \
    do {                                          \
        if (!((lm)->flags & DPHASH_LOCKLESS))     \
            pthread_rwlock_unlock(&(lm)->rwlock); \
    } while (0)

static always_inline u64 __rotl64_lm(u64 x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static always_inline u32 __get_u32_lm(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static always_inline u64 __get_u64_lm(const u8 *p)
{
    return (u64)__get_u32_lm(p) | ((u64)__get_u32_lm(p + 4) << 32);
}

static always_inline u64 __xxh3_lm(const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    const u8 *const end = p + len;
    u64 h;

    if (likely(len >= 16)) {
        const u8 *limit = end - 16;
        u64 v1 = __get_u64_lm(p) * 0xc2b2ae3cc27b9a21ULL;
        p += 8;
        u64 v2 = __get_u64_lm(p) * 0xc2b2ae3cc27b9a21ULL;
        p += 8;
        do {
            v1 += __get_u64_lm(p) * 0xc2b2ae3cc27b9a21ULL;
            v1 = __rotl64_lm(v1, 31) * 0x9e3779b97f4a7c13ULL;
            v2 += __get_u64_lm(p + 8) * 0xc2b2ae3cc27b9a21ULL;
            v2 = __rotl64_lm(v2, 31) * 0x9e3779b97f4a7c13ULL;
            p += 16;
        } while (likely(p <= limit));
        h = __rotl64_lm(v1, 37) * 0x85ebca6b + __rotl64_lm(v2, 37) * 0xc2b2ae3c;
        h += len;
    } else if (len >= 8) {
        u64 v = __get_u64_lm(p) * 0xc2b2ae3cc27b9a21ULL;
        v = __rotl64_lm(v, 31) * 0x9e3779b97f4a7c13ULL;
        h = v ^ (__get_u64_lm(end - 8) * 0xc2b2ae3cc27b9a21ULL);
        h = __rotl64_lm(h, 49) * 0x9e3779b97f4a7c13ULL;
        h += len;
    } else if (len >= 4) {
        h = (u64)__get_u32_lm(p) * 0x9e3779b97f4a7c13ULL;
        h = __rotl64_lm(h, 17) * 0x9e3779b97f4a7c13ULL;
        h += (u64)__get_u32_lm(end - 4);
        h += len;
    } else if (len > 0) {
        u8 buf[4] = {0};
        memcpy(buf, p, len);
        h = (u64)__get_u32_lm(buf) * 0x9e3779b97f4a7c13ULL;
        h += len;
    } else {
        return 0x9e3779b97f4a7c13ULL;
    }
    h ^= h >> 33;
    h *= 0x62a9d9ed799705f5ULL;
    h ^= h >> 29;
    h *= 0x3244f6a7e7e6a1c7ULL;
    h ^= h >> 32;
    return h < 2 ? h + 2 : h;
}

static void *__arena_alloc_lm(struct lsmtree *lm, u32 size)
{
    size = (size + 7) & ~(u32)7;
    if (unlikely(!lm->arena_cur ||
                 lm->arena_cur->used + size > lm->arena_cur->cap)) {
        u32 cap = size > (64 << 10) ? size : (64 << 10);
        struct lm_arena *blk = malloc(sizeof(*blk) + cap);
        if (unlikely(!blk))
            return NULL;
        blk->next = NULL;
        blk->used = 0;
        blk->cap = cap;
        if (lm->arena_cur)
            lm->arena_cur->next = blk;
        else
            lm->arena_head = blk;
        lm->arena_cur = blk;
    }
    void *ptr = lm->arena_cur->data + lm->arena_cur->used;
    lm->arena_cur->used += size;
    return ptr;
}

static int __random_height(void)
{
    int h = 1;
    while (h < SL_MAX_HEIGHT && (rand() & 1))
        h++;
    return h;
}

static int __sl_cmp(const struct sl_node *n, u64 hash,
                    const void *key, u32 klen)
{
    if (n->hash < hash)
        return -1;
    if (n->hash > hash)
        return 1;
    if (n->klen < klen)
        return -1;
    if (n->klen > klen)
        return 1;
    return memcmp(n->key, key, klen);
}

static int __pwrite_lm(int fd, const void *buf, size_t len, off_t off)
{
    const u8 *p = buf;
    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, off);
        if (n <= 0)
            return -1;
        p += n;
        off += n;
        len -= n;
    }
    return 0;
}

static int __pread_lm(int fd, void *buf, size_t len, off_t off)
{
    u8 *p = buf;
    while (len > 0) {
        ssize_t n = pread(fd, p, len, off);
        if (n <= 0)
            return -1;
        p += n;
        off += n;
        len -= n;
    }
    return 0;
}

static void __flush_memtable(struct lsmtree *lm)
{
    if (lm->nr_ssts >= SST_MAX_TABLES)
        return;

    int idx = lm->nr_ssts;
    u32 cap = 0;

    struct sl_node *n = lm->head->next[0];
    while (n) {
        cap++;
        n = n->next[0];
    }
    if (cap == 0)
        return;

    struct sst_table *t = &lm->ssts[idx];
    t->entries = malloc(cap * sizeof(struct sst_entry));
    t->count = 0;
    t->cap = cap;
    t->fd = lm->data_fd;

    n = lm->head->next[0];
    while (n) {
        struct sst_entry *e = &t->entries[t->count++];
        e->hash = n->hash;
        e->klen = n->klen;
        e->vlen = n->vlen;
        e->file_off = n->file_off;
        e->deleted = n->deleted;
        n = n->next[0];
    }

    lm->nr_ssts++;

    for (int i = 0; i < SL_MAX_HEIGHT; i++)
        lm->head->next[i] = NULL;
    lm->max_height = 1;
}

static const struct sl_node *__sl_find(const struct lsmtree *lm,
                                       u64 hash, const void *key, u32 klen)
{
    const struct sl_node *cur = lm->head;
    for (int i = lm->max_height - 1; i >= 0; i--) {
        while (cur->next[i] &&
               __sl_cmp(cur->next[i], hash, key, klen) < 0)
            cur = cur->next[i];
    }
    cur = cur->next[0];
    if (cur && __sl_cmp(cur, hash, key, klen) == 0)
        return cur;
    return NULL;
}

static const struct sst_entry *__sst_find(const struct sst_table *t,
                                          u64 hash, const void *key, u32 klen)
{
    u32 lo = 0, hi = t->count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        const struct sst_entry *e = &t->entries[mid];
        if (e->hash < hash) {
            lo = mid + 1;
            continue;
        }
        if (e->hash > hash) {
            hi = mid;
            continue;
        }
        if (e->klen != klen) {
            if (e->klen < klen)
                lo = mid + 1;
            else
                hi = mid;
            continue;
        }
        return e;
    }
    return NULL;
}

struct lsmtree *lsmtree_open(const char *path, u64 flags)
{
    struct lsmtree *lm = calloc(1, sizeof(*lm));
    if (!lm)
        return NULL;

    lm->data_fd = -1;
    lm->flags = flags;
    if (!(flags & DPHASH_LOCKLESS))
        pthread_rwlock_init(&lm->rwlock, NULL);

    if (mkdir(path, 0755) && errno != EEXIST)
        goto fail;

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/lsmtree.dat", path);
    lm->data_fd = open(buf, O_CREAT | O_RDWR, 0644);
    if (lm->data_fd < 0)
        goto fail;

    lm->head = calloc(1, sizeof(struct sl_node) +
                             SL_MAX_HEIGHT * sizeof(struct sl_node *));
    if (!lm->head)
        goto fail;
    lm->head->height = SL_MAX_HEIGHT;
    lm->max_height = 1;

    return lm;

fail:
    lsmtree_close(lm);
    return NULL;
}

void lsmtree_close(struct lsmtree *lm)
{
    if (!lm)
        return;
    LM_WLOCK(lm);

    for (int i = 0; i < lm->nr_ssts; i++)
        free(lm->ssts[i].entries);

    struct sl_node *n = lm->head ? lm->head->next[0] : NULL;
    while (n) {
        struct sl_node *next = n->next[0];
        free(n);
        n = next;
    }
    free(lm->head);

    struct lm_arena *a = lm->arena_head;
    while (a) {
        struct lm_arena *next = a->next;
        free(a);
        a = next;
    }

    if (lm->data_fd >= 0)
        close(lm->data_fd);
    LM_WUNLOCK(lm);
    if (!(lm->flags & DPHASH_LOCKLESS))
        pthread_rwlock_destroy(&lm->rwlock);
    free(lm);
}

int lsmtree_put(struct lsmtree *lm, const void *key, u32 klen,
                const void *val, u32 vlen)
{
    if (!lm || !key || !val)
        return DPHASH_EINVAL;

    u64 h = __xxh3_lm(key, klen);

    void *kcopy = __arena_alloc_lm(lm, klen);
    if (!kcopy)
        return DPHASH_ENOMEM;
    memcpy(kcopy, key, klen);

    void *vcopy = __arena_alloc_lm(lm, vlen);
    if (!vcopy)
        return DPHASH_ENOMEM;
    memcpy(vcopy, val, vlen);

    s64 off = -1;
    if (lm->flags & DPHASH_FSYNC) {
        u32 total = 8 + klen + vlen;
        u8 *rec = malloc(total);
        if (rec) {
            u32 kl = klen, vl = vlen;
            memcpy(rec, &kl, 4);
            memcpy(rec + 4, &vl, 4);
            memcpy(rec + 8, key, klen);
            memcpy(rec + 8 + klen, val, vlen);
            off = lm->data_off;
            __pwrite_lm(lm->data_fd, rec, total, off);
            lm->data_off += total;
            free(rec);
        }
    }

    LM_WLOCK(lm);

    const struct sl_node *existing = __sl_find(lm, h, kcopy, klen);
    if (existing) {
        LM_WUNLOCK(lm);
        return DPHASH_OK;
    }

    int height = __random_height();
    struct sl_node *node = calloc(1, sizeof(struct sl_node) +
                                         height * sizeof(struct sl_node *));
    if (!node) {
        LM_WUNLOCK(lm);
        return DPHASH_ENOMEM;
    }

    node->hash = h;
    node->key = kcopy;
    node->klen = klen;
    node->val = vcopy;
    node->vlen = vlen;
    node->file_off = off;
    node->deleted = 0;
    node->height = height;

    if (height > lm->max_height) {
        for (int i = lm->max_height; i < height; i++)
            lm->head->next[i] = NULL;
        lm->max_height = height;
    }

    struct sl_node *cur = lm->head;
    for (int i = lm->max_height - 1; i >= 0; i--) {
        while (cur->next[i] && __sl_cmp(cur->next[i], h, kcopy, klen) < 0)
            cur = cur->next[i];
        if (i < height) {
            node->next[i] = cur->next[i];
            cur->next[i] = node;
        }
    }

    lm->count++;

    u32 mem_count = 0;
    struct sl_node *tmp = lm->head->next[0];
    while (tmp) {
        mem_count++;
        tmp = tmp->next[0];
    }
    if (mem_count >= MEMTABLE_CAP)
        __flush_memtable(lm);

    LM_WUNLOCK(lm);
    return DPHASH_OK;
}

int lsmtree_get(struct lsmtree *lm, const void *key, u32 klen,
                const void **val, u32 *vlen)
{
    if (!lm || !key)
        return DPHASH_EINVAL;

    u64 h = __xxh3_lm(key, klen);
    LM_RLOCK(lm);

    const struct sl_node *n = __sl_find(lm, h, key, klen);
    if (n && !n->deleted && n->val) {
        *val = n->val;
        *vlen = n->vlen;
        LM_RUNLOCK(lm);
        return DPHASH_OK;
    }

    for (int i = lm->nr_ssts - 1; i >= 0; i--) {
        const struct sst_entry *e = __sst_find(&lm->ssts[i], h, key, klen);
        if (e && !e->deleted) {
            LM_RUNLOCK(lm);
            return DPHASH_ENOENT;
        }
    }

    LM_RUNLOCK(lm);
    return DPHASH_ENOENT;
}

int lsmtree_get_disk(struct lsmtree *lm, const void *key, u32 klen,
                     void *buf, u32 *vlen)
{
    if (!lm || !key)
        return DPHASH_EINVAL;

    u64 h = __xxh3_lm(key, klen);
    LM_RLOCK(lm);

    const struct sl_node *n = __sl_find(lm, h, key, klen);
    if (n && !n->deleted && n->file_off >= 0) {
        u8 hdr[8];
        if (__pread_lm(lm->data_fd, hdr, 8, n->file_off)) {
            LM_RUNLOCK(lm);
            return DPHASH_EIO;
        }
        u32 vl;
        memcpy(&vl, hdr + 4, 4);
        if (vl > *vlen) {
            LM_RUNLOCK(lm);
            return DPHASH_ENOSPC;
        }
        if (__pread_lm(lm->data_fd, buf, vl, n->file_off + 8 + n->klen)) {
            LM_RUNLOCK(lm);
            return DPHASH_EIO;
        }
        *vlen = vl;
        LM_RUNLOCK(lm);
        return DPHASH_OK;
    }

    for (int i = lm->nr_ssts - 1; i >= 0; i--) {
        const struct sst_entry *e = __sst_find(&lm->ssts[i], h, key, klen);
        if (e && !e->deleted && e->file_off >= 0) {
            u8 hdr[8];
            if (__pread_lm(lm->data_fd, hdr, 8, e->file_off)) {
                LM_RUNLOCK(lm);
                return DPHASH_EIO;
            }
            u32 vl;
            memcpy(&vl, hdr + 4, 4);
            if (vl > *vlen) {
                LM_RUNLOCK(lm);
                return DPHASH_ENOSPC;
            }
            if (__pread_lm(lm->data_fd, buf, vl, e->file_off + 8 + e->klen)) {
                LM_RUNLOCK(lm);
                return DPHASH_EIO;
            }
            *vlen = vl;
            LM_RUNLOCK(lm);
            return DPHASH_OK;
        }
    }

    LM_RUNLOCK(lm);
    return DPHASH_ENOENT;
}

int lsmtree_del(struct lsmtree *lm, const void *key, u32 klen)
{
    if (!lm || !key)
        return DPHASH_EINVAL;

    u64 h = __xxh3_lm(key, klen);
    LM_WLOCK(lm);

    struct sl_node *cur = lm->head;
    for (int i = lm->max_height - 1; i >= 0; i--) {
        while (cur->next[i] && __sl_cmp(cur->next[i], h, key, klen) < 0)
            cur = cur->next[i];
    }

    struct sl_node *target = cur->next[0];
    if (target && __sl_cmp(target, h, key, klen) == 0) {
        target->deleted = 1;
        lm->count--;
        LM_WUNLOCK(lm);
        return DPHASH_OK;
    }

    LM_WUNLOCK(lm);
    return DPHASH_ENOENT;
}

u64 lsmtree_count(const struct lsmtree *lm)
{
    return lm ? lm->count : 0;
}
