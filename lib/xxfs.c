#ifdef XXFS_PROFILE
#define _POSIX_C_SOURCE 199309L
#endif
#include "xxfs.h"
#include <stdio.h>
#include <string.h>

#define INO_CRC_OFF ((u32)((size_t)&((struct xxfs_inode *)0)->i_checksum))

#define fs_wlock(fs)                           \
    do {                                       \
        if (!((fs)->flags & XXFS_FLAG_NOLOCK)) \
            xxfs_os_write_lock(&(fs)->lock);   \
    } while (0)
#define fs_wunlock(fs)                         \
    do {                                       \
        if (!((fs)->flags & XXFS_FLAG_NOLOCK)) \
            xxfs_os_write_unlock(&(fs)->lock); \
    } while (0)
#define fs_rlock(fs)                           \
    do {                                       \
        if (!((fs)->flags & XXFS_FLAG_NOLOCK)) \
            xxfs_os_read_lock(&(fs)->lock);    \
    } while (0)
#define fs_runlock(fs)                         \
    do {                                       \
        if (!((fs)->flags & XXFS_FLAG_NOLOCK)) \
            xxfs_os_read_unlock(&(fs)->lock);  \
    } while (0)

static int get_parent_path(const char *norm, u32 nlen, char *parent, u32 *plen);
static const char *get_basename(const char *norm, u32 nlen);
static int dir_add_child(struct xxfs *fs, const char *dir_path, u32 dplen,
                         const char *name, u8 type);
static int dir_remove_child(struct xxfs *fs, const char *dir_path, u32 dplen,
                            const char *name);
static u64 page_alloc_raw(struct xxfs *fs);
static void dcache_flush_all(struct xxfs *fs);
static void pcache_flush_all(struct xxfs *fs);
static void xxfs_mmap_refresh(struct xxfs *fs);
static int paged_put(struct xxfs *fs, const void *key, u32 klen,
                     const void *val, u32 vlen);
static void inode_to_val(const struct xxfs_inode *ino, void *buf);

#define DPHASH_OK 0
#define DPHASH_ENOENT -2
#define DPHASH_ENOMEM -12
#define DPHASH_ENOSPC -28
#define DPHASH_EIO -5

#define GROUP_SIZE 16
#define CTRL_EMPTY 0x80
#define CTRL_DELETED 0xFE
#define MAX_LOAD_NUM 14
#define KEY_PREFIX_LEN 16

struct dp_slot {
    u64 hash;
    void *key;
    void *val;
    u32 klen;
    u32 vlen;
    u64 file_off;
};

struct dp_table {
    u8 *ctrl;
    struct dp_slot *slots;
    u64 cap;
    u64 mask;
    u64 count;
    u64 tombstones;
    u64 grow_at;
    u8 *old_ctrl;
    struct dp_slot *old_slots;
    u64 old_cap;
    u64 old_mask;
    u64 old_migrated;
    u64 old_grow_at;
};

struct paged_slot {
    u64 hash;
    u64 file_off;
    u32 klen;
    u32 vlen;
    u8 key_prefix[KEY_PREFIX_LEN];
    u8 flags;
    u8 pad[7];
};

#define SLOT_F_INLINE 1
#define PAGED_SLOTS_PER_PAGE 56
#define PAGED_INLINE_OFF (16 + PAGED_SLOTS_PER_PAGE * 48)
#define PAGED_INLINE_SPACE (4096 - PAGED_INLINE_OFF)

struct paged_page {
    u16 count;
    u16 local_depth;
    u32 inline_used;
    u64 overflow_next;
    struct paged_slot slots[PAGED_SLOTS_PER_PAGE];
    u8 inline_area[PAGED_INLINE_SPACE];
};

struct paged_dir {
    u64 *page_off;
    u32 dir_size;
    u8 global_depth;
};

struct paged_cache {
    u64 page_off;
    struct paged_page *page;
    int dirty;
};

#define PCACHE_BITS 12
#define PCACHE_SIZE (1 << PCACHE_BITS)
#define PCACHE_MASK (PCACHE_SIZE - 1)

#define DIR_CACHE_SIZE 256

#define ICACHE_CAP_INIT 16384
#define ICACHE_MAX_ENTRIES 131072
#define ICACHE_CTRL_EMPTY 0x00
#define ICACHE_CTRL_VALID 0x80

struct icache_entry {
    u64 hash;
    u8 ctrl;
    u8 dirty;
    u32 klen;
    char key[XXFS_MAX_PATH];
    struct xxfs_inode inode;
};

struct dir_cache_entry {
    char path[XXFS_MAX_PATH];
    u32 path_len;
    struct xxfs_dir_child *children;
    u32 child_count;
    u32 child_cap;
    int dirty;
    int valid;
};

struct xxfs {
    struct xxfs_os_file file;
    struct xxfs_os_lock lock;
    struct xxfs_super sb;
    struct dp_table tbl;
    struct paged_dir pdir;
    struct paged_cache pcache[PCACHE_SIZE];
    u64 pcache_tick;
    u8 *mmap;
    size_t mmap_len;
    u64 flags;
    u64 cow_gen;
    u64 wal_head;
    u64 wal_tail;
    u64 wal_seq;
    u64 pp_seq;
    int pp_active;
    int mounted;
    struct icache_entry *icache;
    u32 icache_cap;
    u32 icache_count;
    u32 icache_mask;
    u64 icache_tick;
    struct dir_cache_entry dcache[DIR_CACHE_SIZE];
#ifdef XXFS_PROFILE
    u64 prof_hash_ns;
    u64 prof_icache_ns;
    u64 prof_pcache_ns;
    u64 prof_pwrite_ns;
    u64 prof_pread_ns;
    u64 prof_alloc_ns;
    u64 prof_dcache_ns;
    u32 prof_create_cnt;
    u32 prof_write_cnt;
#endif
};

static always_inline u64 rotl64(u64 v, int n) { return (v << n) | (v >> (64 - n)); }

#ifdef XXFS_PROFILE
#include <time.h>
static inline u64 prof_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
static u64 prof_acc = 0;
#define PROF_BEGIN()           \
    do {                       \
        prof_acc = prof_now(); \
    } while (0)
#define PROF_ADD(fs, field)                   \
    do {                                      \
        (fs)->field += prof_now() - prof_acc; \
    } while (0)
#else
#define PROF_BEGIN() ((void)0)
#define PROF_ADD(fs, field) ((void)0)
#endif

static void icache_init(struct xxfs *fs)
{
    fs->icache_cap = ICACHE_CAP_INIT;
    fs->icache_mask = fs->icache_cap - 1;
    fs->icache_count = 0;
    fs->icache_tick = 0;
    fs->icache = xxfs_os_zalloc(fs->icache_cap * sizeof(struct icache_entry));
}

static void icache_destroy(struct xxfs *fs)
{
    if (fs->icache) {
        xxfs_os_free(fs->icache);
        fs->icache = NULL;
    }
}

static struct icache_entry *icache_lookup(struct xxfs *fs, u64 hash, const char *key, u32 klen)
{
    u8 ctrl = (u8)((hash & 0x7F) | ICACHE_CTRL_VALID);
    u32 idx = (u32)(hash & fs->icache_mask);
    for (u32 probe = 0; probe < 16; probe++) {
        struct icache_entry *e = &fs->icache[idx];
        if (e->ctrl == ICACHE_CTRL_EMPTY)
            return NULL;
        if (e->ctrl == ctrl && e->hash == hash)
            return e;
        idx = (idx + 1) & fs->icache_mask;
    }
    return NULL;
}

static void icache_evict_one(struct xxfs *fs)
{
    u32 idx = (u32)(fs->icache_tick & fs->icache_mask);
    for (u32 probe = 0; probe < fs->icache_cap; probe++) {
        struct icache_entry *e = &fs->icache[idx];
        if (e->ctrl != ICACHE_CTRL_EMPTY) {
            if (e->dirty) {
                e->inode.i_mtime = xxfs_os_time();
                e->inode.i_ctime = e->inode.i_mtime;
                e->inode.i_checksum = xxfs_os_crc32c(&e->inode, INO_CRC_OFF);
                
                u8 val_buf[sizeof(struct xxfs_inode)];
                inode_to_val(&e->inode, val_buf);
                paged_put(fs, e->key, e->klen, val_buf, sizeof(struct xxfs_inode));
            }
            e->ctrl = ICACHE_CTRL_EMPTY;
            e->dirty = 0;
            fs->icache_count--;
            return;
        }
        idx = (idx + 1) & fs->icache_mask;
    }
}

static void icache_grow(struct xxfs *fs)
{
    u32 new_cap = fs->icache_cap * 2;
    u32 new_mask = new_cap - 1;
    struct icache_entry *new_tbl = xxfs_os_zalloc(new_cap * sizeof(struct icache_entry));
    if (!new_tbl)
        return;
    for (u32 i = 0; i < fs->icache_cap; i++) {
        struct icache_entry *old = &fs->icache[i];
        if (old->ctrl == ICACHE_CTRL_EMPTY)
            continue;
        u32 idx = (u32)(old->hash & new_mask);
        for (u32 probe = 0; probe < new_cap; probe++) {
            if (new_tbl[idx].ctrl == ICACHE_CTRL_EMPTY) {
                new_tbl[idx] = *old;
                break;
            }
            idx = (idx + 1) & new_mask;
        }
    }
    xxfs_os_free(fs->icache);
    fs->icache = new_tbl;
    fs->icache_cap = new_cap;
    fs->icache_mask = new_mask;
}

static void icache_put(struct xxfs *fs, u64 hash, const char *key, u32 klen,
                       const struct xxfs_inode *ino)
{
    u8 ctrl = (u8)((hash & 0x7F) | ICACHE_CTRL_VALID);
    if (fs->icache_count >= ICACHE_MAX_ENTRIES) {
        icache_evict_one(fs);
    } else if (fs->icache_count * 2 >= fs->icache_cap) {
        icache_grow(fs);
    }
    u32 idx = (u32)(hash & fs->icache_mask);
    for (u32 probe = 0; probe < fs->icache_cap; probe++) {
        if (fs->icache[idx].ctrl == ICACHE_CTRL_EMPTY) {
            fs->icache[idx].hash = hash;
            fs->icache[idx].ctrl = ctrl;
            fs->icache[idx].dirty = 1;
            fs->icache[idx].klen = klen;
            xxfs_os_memcpy(fs->icache[idx].key, key, klen + 1);
            xxfs_os_memcpy(&fs->icache[idx].inode, ino, sizeof(*ino));
            fs->icache_count++;
            return;
        }
        if (fs->icache[idx].hash == hash) {
            fs->icache[idx].dirty = 1;
            xxfs_os_memcpy(&fs->icache[idx].inode, ino, sizeof(*ino));
            return;
        }
        idx = (idx + 1) & fs->icache_mask;
    }
}

static void icache_del(struct xxfs *fs, u64 hash, const char *key, u32 klen)
{
    struct icache_entry *e = icache_lookup(fs, hash, key, klen);
    if (e) {
        e->ctrl = ICACHE_CTRL_EMPTY;
        fs->icache_count--;
    }
}

static void icache_invalidate(struct xxfs *fs, u64 hash, const char *key, u32 klen)
{
    struct icache_entry *e = icache_lookup(fs, hash, key, klen);
    if (e) {
        e->ctrl = ICACHE_CTRL_EMPTY;
        fs->icache_count--;
    }
}

static void xxfs_mmap_refresh(struct xxfs *fs)
{
    s64 fsize = xxfs_os_file_size(&fs->file);
    if (fsize <= 0)
        return;
    size_t need = (size_t)fsize;
    if (fs->mmap && fs->mmap_len >= need)
        return;
    if (fs->mmap) {
        xxfs_os_file_munmap(fs->mmap, fs->mmap_len);
        fs->mmap = NULL;
        fs->mmap_len = 0;
    }
    fs->mmap = (u8 *)xxfs_os_file_mmap(&fs->file, need);
    if (fs->mmap) {
        fs->mmap_len = need;
    } else {
        fs->mmap_len = 0;
    }
}

static always_inline u64 get_u64(const u8 *p)
{
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24) | ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48) | ((u64)p[7] << 56);
}

static always_inline u32 get_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static always_inline u64 xxh64(const void *data, u32 len)
{
    const u64 P1 = 0x9e3779b97f4a7c15ULL;
    const u64 P2 = 0x85ebca6bcae6d1d5ULL;
    const u64 P3 = 0xc2b2ae3cc27b9a21ULL;
    const u64 P4 = 0x27d4eb2f165b7d03ULL;
    const u64 P5 = 0x659e8513b3a1b327ULL;
    const u8 *p = (const u8 *)data;
    const u8 *end = p + len;
    u64 h;

    if (len >= 32) {
        u64 v1 = P1 + P2, v2 = P2, v3 = 0, v4 = (u64)(-P1);
        const u8 *limit = end - 32;
        do {
            v1 = rotl64(v1 + get_u64(p) * P2, 31) * P1;
            p += 8;
            v2 = rotl64(v2 + get_u64(p) * P2, 31) * P1;
            p += 8;
            v3 = rotl64(v3 + get_u64(p) * P2, 31) * P1;
            p += 8;
            v4 = rotl64(v4 + get_u64(p) * P2, 31) * P1;
            p += 8;
        } while (p <= limit);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h += len;
        h = rotl64(v3 * P1, 17) * P4 + h;
        h = rotl64(v4 * P1, 17) * P4 + h;
        h = rotl64(v2 * P1, 17) * P4 + h;
        h = rotl64(v1 * P1, 17) * P4 + h;
    } else {
        h = P5 + len;
    }
    if (p + 16 <= end) {
        h = rotl64(h + get_u64(p) * P3, 23) * P2 + rotl64(h + get_u64(p + 8) * P3, 23) * P2;
        p += 16;
    }
    if (p + 8 <= end) {
        h = rotl64(h + get_u64(p) * P3, 23) * P2;
        p += 8;
    }
    if (p + 4 <= end) {
        h = rotl64(h + (u64)get_u32(p) * P4, 17) * P1;
        p += 4;
    }
    while (p < end) {
        h = rotl64(h + (u64)(*p) * P5, 11) * P1;
        p++;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h < 2 ? h + 2 : h;
}

#ifdef __SSE2__
#include <emmintrin.h>
static always_inline u32 match_group(const u8 *ctrl, u8 tag)
{
    __m128i cv = _mm_loadu_si128((const __m128i *)ctrl);
    __m128i tv = _mm_set1_epi8((char)tag);
    return (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(cv, tv));
}
static always_inline u32 match_empty(const u8 *ctrl)
{
    __m128i cv = _mm_loadu_si128((const __m128i *)ctrl);
    __m128i ev = _mm_set1_epi8((char)CTRL_EMPTY);
    return (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(cv, ev));
}
#else
static always_inline u32 match_group(const u8 *ctrl, u8 tag)
{
    u32 m = 0;
    for (int i = 0; i < GROUP_SIZE; i++)
        if (ctrl[i] == tag)
            m |= (1U << i);
    return m;
}
static always_inline u32 match_empty(const u8 *ctrl)
{
    u32 m = 0;
    for (int i = 0; i < GROUP_SIZE; i++)
        if (ctrl[i] == CTRL_EMPTY)
            m |= (1U << i);
    return m;
}
#endif

static always_inline u8 ctrl_tag(u64 hash) { return (u8)(hash & 0x7F); }
static always_inline u64 group_idx(u64 hash, u64 mask) { return (hash >> 7) & (mask / GROUP_SIZE); }

static void table_init(struct dp_table *t)
{
    memset(t, 0, sizeof(*t));
}

static void table_destroy(struct dp_table *t)
{
    if (t->ctrl) {
        xxfs_os_free(t->ctrl);
        t->ctrl = NULL;
    }
    if (t->slots) {
        xxfs_os_free(t->slots);
        t->slots = NULL;
    }
    if (t->old_ctrl) {
        xxfs_os_free(t->old_ctrl);
        t->old_ctrl = NULL;
    }
    if (t->old_slots) {
        xxfs_os_free(t->old_slots);
        t->old_slots = NULL;
    }
}

static int table_alloc(struct dp_table *t, u64 cap)
{
    t->cap = cap;
    t->mask = cap - 1;
    t->grow_at = cap * MAX_LOAD_NUM / GROUP_SIZE;
    t->ctrl = xxfs_os_zalloc(cap);
    t->slots = xxfs_os_zalloc(cap * sizeof(struct dp_slot));
    if (!t->ctrl || !t->slots) {
        table_destroy(t);
        return XXFS_ENOMEM;
    }
    memset(t->ctrl, CTRL_EMPTY, cap);
    return XXFS_OK;
}

static s64 table_find(struct dp_table *t, u64 hash, const void *key, u32 klen)
{
    u8 tag = ctrl_tag(hash);
    u64 g = group_idx(hash, t->mask);
    u64 nr = t->cap / GROUP_SIZE;
    for (u64 probe = 0; probe < nr; probe++) {
        const u8 *gc = t->ctrl + g * GROUP_SIZE;
        u32 m = match_group(gc, tag);
        while (m) {
            u32 bit = (u32)__builtin_ctz(m);
            u64 idx = g * GROUP_SIZE + bit;
            if (t->slots[idx].hash == hash && t->slots[idx].klen == klen &&
                t->slots[idx].key && xxfs_os_memcmp(t->slots[idx].key, key, klen) == 0)
                return (s64)idx;
            m &= m - 1;
        }
        if (match_empty(gc))
            return -1;
        g = (g + 1) & (nr - 1);
    }
    return -1;
}

static int table_insert(struct dp_table *t, u64 hash, void *key, void *val,
                        u32 klen, u32 vlen, u64 file_off)
{
    u8 tag = ctrl_tag(hash);
    u64 g = group_idx(hash, t->mask);
    u64 nr = t->cap / GROUP_SIZE;
    s64 first_del = -1;

    for (u64 probe = 0; probe < nr; probe++) {
        const u8 *gc = t->ctrl + g * GROUP_SIZE;
        u32 m = match_group(gc, tag);
        while (m) {
            u32 bit = (u32)__builtin_ctz(m);
            u64 idx = g * GROUP_SIZE + bit;
            if (t->slots[idx].hash == hash && t->slots[idx].klen == klen &&
                t->slots[idx].key && xxfs_os_memcmp(t->slots[idx].key, key, klen) == 0) {
                t->slots[idx].val = val;
                t->slots[idx].vlen = vlen;
                t->slots[idx].file_off = file_off;
                return 1;
            }
            m &= m - 1;
        }
        if (first_del < 0) {
            u32 dm = match_group(gc, CTRL_DELETED);
            if (dm)
                first_del = (s64)(g * GROUP_SIZE + __builtin_ctz(dm));
        }
        u32 em = match_empty(gc);
        if (em) {
            u32 bit = (u32)__builtin_ctz(em);
            u64 idx = (first_del >= 0) ? (u64)first_del : g * GROUP_SIZE + bit;
            int was_tomb = (t->ctrl[idx] == CTRL_DELETED);
            t->ctrl[idx] = tag;
            t->slots[idx].hash = hash;
            t->slots[idx].key = key;
            t->slots[idx].val = val;
            t->slots[idx].klen = klen;
            t->slots[idx].vlen = vlen;
            t->slots[idx].file_off = file_off;
            t->count++;
            if (was_tomb)
                t->tombstones--;
            return 0;
        }
        g = (g + 1) & (nr - 1);
    }
    if (first_del >= 0) {
        u64 idx = (u64)first_del;
        t->ctrl[idx] = tag;
        t->slots[idx].hash = hash;
        t->slots[idx].key = key;
        t->slots[idx].val = val;
        t->slots[idx].klen = klen;
        t->slots[idx].vlen = vlen;
        t->slots[idx].file_off = file_off;
        t->count++;
        t->tombstones--;
        return 0;
    }
    return -1;
}

static void table_migrate_batch(struct dp_table *t, u32 batch)
{
    if (!t->old_ctrl)
        return;
    for (u32 i = 0; i < batch && t->old_migrated < t->old_cap; i++) {
        u64 idx = t->old_migrated++;
        if (t->old_ctrl[idx] != CTRL_EMPTY && t->old_ctrl[idx] != CTRL_DELETED) {
            struct dp_slot *s = &t->old_slots[idx];
            table_insert(t, s->hash, s->key, s->val, s->klen, s->vlen, s->file_off);
        }
    }
    if (t->old_migrated >= t->old_cap) {
        xxfs_os_free(t->old_ctrl);
        t->old_ctrl = NULL;
        xxfs_os_free(t->old_slots);
        t->old_slots = NULL;
        t->old_cap = 0;
    }
}

static int table_grow(struct dp_table *t)
{
    if (t->old_ctrl) {
        table_migrate_batch(t, (u32)t->old_cap);
        if (t->old_ctrl)
            return 0;
    }
    u64 new_cap = t->cap * 2;
    if (new_cap < 32)
        new_cap = 32;
    t->old_ctrl = t->ctrl;
    t->old_slots = t->slots;
    t->old_cap = t->cap;
    t->old_mask = t->mask;
    t->old_migrated = 0;
    t->old_grow_at = t->grow_at;
    int rc = table_alloc(t, new_cap);
    if (rc != XXFS_OK) {
        t->ctrl = t->old_ctrl;
        t->slots = t->old_slots;
        t->cap = t->old_cap;
        t->mask = t->old_mask;
        t->old_ctrl = NULL;
        t->old_slots = NULL;
        return rc;
    }
    t->count = 0;
    t->tombstones = 0;
    table_migrate_batch(t, (u32)t->old_cap);
    return 0;
}

static int table_put(struct dp_table *t, u64 hash, void *key, void *val,
                     u32 klen, u32 vlen, u64 file_off)
{
    if (t->count + t->tombstones >= t->grow_at)
        table_grow(t);
    if (t->old_ctrl)
        table_migrate_batch(t, 64);
    int rc = table_insert(t, hash, key, val, klen, vlen, file_off);
    if (rc < 0) {
        table_grow(t);
        rc = table_insert(t, hash, key, val, klen, vlen, file_off);
    }
    return rc;
}

static int table_del(struct dp_table *t, u64 hash, const void *key, u32 klen)
{
    s64 idx = table_find(t, hash, key, klen);
    if (idx < 0)
        return XXFS_ENOENT;
    struct dp_slot *s = &t->slots[idx];
    if (s->key)
        xxfs_os_free(s->key);
    if (s->val)
        xxfs_os_free(s->val);
    s->key = NULL;
    s->val = NULL;
    s->hash = 0;
    s->klen = 0;
    s->vlen = 0;
    s->file_off = 0;
    t->ctrl[idx] = CTRL_DELETED;
    t->count--;
    t->tombstones++;
    return XXFS_OK;
}

static always_inline u32 dir_index(u64 hash, u8 depth)
{
    if (depth == 0)
        return 0;
    return (u32)(hash & ((1ULL << depth) - 1));
}

static u64 page_alloc(struct xxfs *fs)
{
    u64 off = fs->sb.s_data_off + (fs->sb.s_free_blocks > 0 ? (fs->sb.s_block_count - fs->sb.s_free_blocks) * XXFS_BLOCK_SIZE : 0);
    if (fs->sb.s_free_blocks == 0) {
        s64 new_size = (s64)off + XXFS_BLOCK_SIZE;
        if (xxfs_os_file_extend(&fs->file, new_size))
            return 0;
        fs->sb.s_block_count++;
    } else {
        fs->sb.s_free_blocks--;
    }
    return off;
}

static u64 page_alloc_n(struct xxfs *fs, u64 n)
{
    if (n == 0)
        return 0;
    if (n == 1)
        return page_alloc(fs);
    
    u64 off = fs->sb.s_data_off + (fs->sb.s_free_blocks > 0 ? (fs->sb.s_block_count - fs->sb.s_free_blocks) * XXFS_BLOCK_SIZE : 0);
    
    if (fs->sb.s_free_blocks >= n) {
        fs->sb.s_free_blocks -= n;
        return off;
    }
    
    u64 avail = fs->sb.s_free_blocks;
    u64 need = n - avail;
    
    s64 new_size = (s64)(off + need * XXFS_BLOCK_SIZE);
    if (xxfs_os_file_extend(&fs->file, new_size))
        return 0;
    
    fs->sb.s_block_count += need;
    fs->sb.s_free_blocks = 0;
    
    return off;
}

static u64 page_alloc_raw(struct xxfs *fs)
{
    s64 fsize = xxfs_os_file_size(&fs->file);
    if (fsize < 0)
        return 0;
    u64 off = (u64)fsize;
    off = (off + XXFS_BLOCK_SIZE - 1) & ~((u64)XXFS_BLOCK_SIZE - 1);
    s64 new_size = (s64)(off + XXFS_BLOCK_SIZE);
    if (xxfs_os_file_extend(&fs->file, new_size))
        return 0;
    return off;
}

static struct paged_page *page_load(struct xxfs *fs, u64 off)
{
    u32 idx = (u32)((off >> XXFS_BLOCK_BITS) & PCACHE_MASK);
    struct paged_cache *c = &fs->pcache[idx];
    if (c->page_off == off && c->page) {
        return c->page;
    }
    if (c->dirty && c->page) {
        PROF_BEGIN();
        xxfs_os_file_pwrite(&fs->file, c->page,
                            sizeof(struct paged_page), (s64)c->page_off);
        PROF_ADD(fs, prof_pwrite_ns);
        c->dirty = 0;
    }
    if (c->page) {
        xxfs_os_free(c->page);
        c->page = NULL;
    }
    struct paged_page *pg = xxfs_os_zalloc(sizeof(struct paged_page));
    if (!pg)
        return NULL;
    PROF_BEGIN();
    if (xxfs_os_file_pread(&fs->file, pg, sizeof(struct paged_page), (s64)off) < 0) {
        PROF_ADD(fs, prof_pread_ns);
        xxfs_os_free(pg);
        return NULL;
    }
    PROF_ADD(fs, prof_pread_ns);
    c->page_off = off;
    c->page = pg;
    c->dirty = 0;
    return pg;
}

static int paged_dir_double(struct paged_dir *d)
{
    u32 new_size = d->dir_size * 2;
    u64 *new_off = xxfs_os_zalloc(new_size * sizeof(u64));
    if (!new_off)
        return XXFS_ENOMEM;
    for (u32 i = 0; i < d->dir_size; i++) {
        new_off[i] = d->page_off[i];
        new_off[i + d->dir_size] = d->page_off[i];
    }
    xxfs_os_free(d->page_off);
    d->page_off = new_off;
    d->dir_size = new_size;
    d->global_depth++;
    return XXFS_OK;
}

static int write_record(struct xxfs *fs, const void *key, u32 klen,
                        const void *val, u32 vlen, u64 *out_off)
{
    u32 rec_len = 8 + klen + vlen;
    PROF_BEGIN();
    u64 write_off = page_alloc(fs);
    PROF_ADD(fs, prof_alloc_ns);
    if (!write_off)
        return XXFS_ENOSPC;
    u8 stack_buf[512];
    u8 *rec;
    if (rec_len <= sizeof(stack_buf)) {
        rec = stack_buf;
    } else {
        rec = xxfs_os_alloc(rec_len);
        if (!rec)
            return XXFS_ENOMEM;
    }
    xxfs_os_memcpy(rec, &klen, 4);
    xxfs_os_memcpy(rec + 4, &vlen, 4);
    xxfs_os_memcpy(rec + 8, key, klen);
    xxfs_os_memcpy(rec + 8 + klen, val, vlen);
    PROF_BEGIN();
    int rc = xxfs_os_file_pwrite(&fs->file, rec, rec_len, (s64)write_off);
    PROF_ADD(fs, prof_pwrite_ns);
    if (rec != stack_buf)
        xxfs_os_free(rec);
    if (rc)
        return XXFS_EIO;
    *out_off = write_off;
    return XXFS_OK;
}

static void page_writeback(struct xxfs *fs, struct paged_page *pg, u64 off)
{
    u32 idx = (u32)((off >> XXFS_BLOCK_BITS) & PCACHE_MASK);
    if (fs->pcache[idx].page_off == off && fs->pcache[idx].page == pg) {
        fs->pcache[idx].dirty = 1;
        return;
    }
    PROF_BEGIN();
    xxfs_os_file_pwrite(&fs->file, pg, sizeof(struct paged_page), (s64)off);
    PROF_ADD(fs, prof_pwrite_ns);
}

static void icache_flush_all(struct xxfs *fs)
{
    if (!fs->icache)
        return;
    
    for (u32 i = 0; i < fs->icache_cap; i++) {
        struct icache_entry *e = &fs->icache[i];
        if (e->ctrl != ICACHE_CTRL_EMPTY && (e->ctrl & ICACHE_CTRL_VALID) && e->dirty) {
            e->inode.i_mtime = xxfs_os_time();
            e->inode.i_ctime = e->inode.i_mtime;
            e->inode.i_checksum = xxfs_os_crc32c(&e->inode, INO_CRC_OFF);
            
            u8 val_buf[sizeof(struct xxfs_inode)];
            inode_to_val(&e->inode, val_buf);
            paged_put(fs, e->key, e->klen, val_buf, sizeof(struct xxfs_inode));
            e->dirty = 0;
        }
    }
}

static void pcache_flush_all(struct xxfs *fs)
{
    for (int i = 0; i < PCACHE_SIZE; i++) {
        if (fs->pcache[i].dirty && fs->pcache[i].page) {
            xxfs_os_file_pwrite(&fs->file, fs->pcache[i].page,
                                sizeof(struct paged_page), (s64)fs->pcache[i].page_off);
            fs->pcache[i].dirty = 0;
        }
    }
}

static void page_cache_invalidate(struct xxfs *fs, u64 off)
{
    u32 idx = (u32)((off >> XXFS_BLOCK_BITS) & PCACHE_MASK);
    struct paged_cache *c = &fs->pcache[idx];
    if (c->page_off == off && c->page) {
        if (c->dirty)
            xxfs_os_file_pwrite(&fs->file, c->page,
                                sizeof(struct paged_page), (s64)off);
        xxfs_os_free(c->page);
        c->page = NULL;
        c->page_off = 0;
        c->dirty = 0;
    }
}

static int page_split_and_retry(struct xxfs *fs, u64 page_off,
                                const void *key, u32 klen,
                                const void *val, u32 vlen)
{
    struct paged_dir *d = &fs->pdir;
    struct paged_page *pg = page_load(fs, page_off);
    if (!pg)
        return XXFS_EIO;

    if (pg->local_depth >= d->global_depth) {
        if (d->global_depth >= 20)
            return XXFS_ENOSPC;
        int rc = paged_dir_double(d);
        if (rc != XXFS_OK)
            return rc;
        pg = page_load(fs, page_off);
        if (!pg)
            return XXFS_EIO;
    }

    u8 new_depth = pg->local_depth + 1;
    u32 split_bit = 1U << (new_depth - 1);

    struct paged_slot old_slots[PAGED_SLOTS_PER_PAGE];
    u8 old_inline[PAGED_INLINE_SPACE];
    u16 old_count = pg->count;
    u32 old_inline_used = pg->inline_used;
    xxfs_os_memcpy(old_slots, pg->slots, sizeof(pg->slots));
    xxfs_os_memcpy(old_inline, pg->inline_area, old_inline_used);

    u64 new_off = page_alloc(fs);
    if (!new_off)
        return XXFS_ENOSPC;

    xxfs_os_memset(pg->slots, 0, sizeof(pg->slots));
    pg->count = 0;
    pg->inline_used = 0;
    pg->local_depth = new_depth;
    pg->overflow_next = 0;

    struct paged_page new_pg;
    xxfs_os_memset(&new_pg, 0, sizeof(new_pg));
    new_pg.local_depth = new_depth;

    for (u16 i = 0; i < old_count; i++) {
        struct paged_slot *s = &old_slots[i];
        struct paged_page *target = (dir_index(s->hash, new_depth) & split_bit) ? &new_pg : pg;
        struct paged_slot *ts = &target->slots[target->count++];
        *ts = *s;
        if (s->flags & SLOT_F_INLINE) {
            u32 need = s->klen + s->vlen;
            ts->file_off = target->inline_used;
            xxfs_os_memcpy(target->inline_area + target->inline_used,
                           old_inline + s->file_off, need);
            target->inline_used += need;
        }
    }

    for (u32 i = 0; i < d->dir_size; i++) {
        if (d->page_off[i] == page_off && (i & split_bit))
            d->page_off[i] = new_off;
    }

    xxfs_os_file_pwrite(&fs->file, &new_pg, sizeof(new_pg), (s64)new_off);
    page_cache_invalidate(fs, new_off);
    page_writeback(fs, pg, page_off);

    return paged_put(fs, key, klen, val, vlen);
}

static int paged_put(struct xxfs *fs, const void *key, u32 klen,
                     const void *val, u32 vlen)
{
    u64 h = xxh64(key, klen);
    struct paged_dir *d = &fs->pdir;
    u32 dir_idx = dir_index(h, d->global_depth);
    if (d->page_off[dir_idx] == (u64)-1) {
        u64 off = page_alloc(fs);
        if (!off)
            return XXFS_ENOSPC;
        d->page_off[dir_idx] = off;
        struct paged_page empty;
        memset(&empty, 0, sizeof(empty));
        empty.local_depth = d->global_depth;
        if (xxfs_os_file_pwrite(&fs->file, &empty, sizeof(empty), (s64)off))
            return XXFS_EIO;
        page_cache_invalidate(fs, off);
    }
    u64 cur_off = d->page_off[dir_idx];
    while (cur_off != 0) {
        struct paged_page *pg = page_load(fs, cur_off);
        if (!pg)
            return XXFS_EIO;
        for (u16 i = 0; i < pg->count; i++) {
            struct paged_slot *s = &pg->slots[i];
            if (s->hash == h && s->klen == klen) {
                u32 pn = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
                if (xxfs_os_memcmp(s->key_prefix, key, pn) == 0) {
                    if (s->flags & SLOT_F_INLINE) {
                        if (vlen <= s->vlen) {
                            xxfs_os_memcpy(pg->inline_area + s->file_off + klen, val, vlen);
                            s->vlen = vlen;
                            page_writeback(fs, pg, cur_off);
                            return XXFS_OK;
                        }
                    } else if (vlen <= s->vlen) {
                        xxfs_os_file_pwrite(&fs->file, val, vlen,
                                            (s64)(s->file_off + 8 + klen));
                        s->vlen = vlen;
                        page_writeback(fs, pg, cur_off);
                        return XXFS_OK;
                    }
                    u64 write_off;
                    int rc = write_record(fs, key, klen, val, vlen, &write_off);
                    if (rc != XXFS_OK)
                        return rc;
                    s->vlen = vlen;
                    s->file_off = write_off;
                    s->flags = 0;
                    page_writeback(fs, pg, cur_off);
                    return XXFS_OK;
                }
            }
        }
        if (pg->count >= PAGED_SLOTS_PER_PAGE) {
            if (pg->overflow_next == 0) {
                u64 new_off = page_alloc(fs);
                if (!new_off)
                    return page_split_and_retry(fs, cur_off, key, klen, val, vlen);
                pg->overflow_next = new_off;
                page_writeback(fs, pg, cur_off);
                struct paged_page empty;
                memset(&empty, 0, sizeof(empty));
                empty.local_depth = pg->local_depth;
                if (xxfs_os_file_pwrite(&fs->file, &empty, sizeof(empty), (s64)new_off))
                    return XXFS_EIO;
                page_cache_invalidate(fs, new_off);
            }
            cur_off = pg->overflow_next;
            continue;
        }
        struct paged_slot *s = &pg->slots[pg->count++];
        s->hash = h;
        s->klen = klen;
        s->vlen = vlen;
        s->flags = 0;
        u32 pn = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
        xxfs_os_memcpy(s->key_prefix, key, pn);
        u32 inline_need = klen + vlen;
        if (pg->inline_used + inline_need <= PAGED_INLINE_SPACE) {
            s->file_off = pg->inline_used;
            xxfs_os_memcpy(pg->inline_area + pg->inline_used, key, klen);
            xxfs_os_memcpy(pg->inline_area + pg->inline_used + klen, val, vlen);
            pg->inline_used += inline_need;
            s->flags = SLOT_F_INLINE;
        } else {
            u64 write_off;
            int rc = write_record(fs, key, klen, val, vlen, &write_off);
            if (rc != XXFS_OK) {
                pg->count--;
                return rc;
            }
            s->file_off = write_off;
        }
        page_writeback(fs, pg, cur_off);
        return XXFS_OK;
    }
    return XXFS_ENOSPC;
}

static int paged_insert(struct xxfs *fs, const void *key, u32 klen,
                        const void *val, u32 vlen)
{
    u64 h = xxh64(key, klen);
    struct paged_dir *d = &fs->pdir;
    u32 dir_idx = dir_index(h, d->global_depth);
    if (d->page_off[dir_idx] == (u64)-1) {
        u64 off = page_alloc(fs);
        if (!off)
            return XXFS_ENOSPC;
        d->page_off[dir_idx] = off;
        struct paged_page empty;
        memset(&empty, 0, sizeof(empty));
        empty.local_depth = d->global_depth;
        if (xxfs_os_file_pwrite(&fs->file, &empty, sizeof(empty), (s64)off))
            return XXFS_EIO;
        page_cache_invalidate(fs, off);
    }
    u64 cur_off = d->page_off[dir_idx];
    while (cur_off != 0) {
        struct paged_page *pg = page_load(fs, cur_off);
        if (!pg)
            return XXFS_EIO;
        if (pg->count >= PAGED_SLOTS_PER_PAGE) {
            if (pg->overflow_next == 0) {
                u64 new_off = page_alloc(fs);
                if (!new_off)
                    return page_split_and_retry(fs, cur_off, key, klen, val, vlen);
                pg->overflow_next = new_off;
                page_writeback(fs, pg, cur_off);
                struct paged_page empty;
                memset(&empty, 0, sizeof(empty));
                empty.local_depth = pg->local_depth;
                if (xxfs_os_file_pwrite(&fs->file, &empty, sizeof(empty), (s64)new_off))
                    return XXFS_EIO;
                page_cache_invalidate(fs, new_off);
            }
            cur_off = pg->overflow_next;
            continue;
        }
        struct paged_slot *s = &pg->slots[pg->count++];
        s->hash = h;
        s->klen = klen;
        s->vlen = vlen;
        s->flags = 0;
        u32 pn = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
        xxfs_os_memcpy(s->key_prefix, key, pn);
        u32 inline_need = klen + vlen;
        if (pg->inline_used + inline_need <= PAGED_INLINE_SPACE) {
            s->file_off = pg->inline_used;
            xxfs_os_memcpy(pg->inline_area + pg->inline_used, key, klen);
            xxfs_os_memcpy(pg->inline_area + pg->inline_used + klen, val, vlen);
            pg->inline_used += inline_need;
            s->flags = SLOT_F_INLINE;
        } else {
            u64 write_off;
            int rc = write_record(fs, key, klen, val, vlen, &write_off);
            if (rc != XXFS_OK) {
                pg->count--;
                return rc;
            }
            s->file_off = write_off;
        }
        page_writeback(fs, pg, cur_off);
        return XXFS_OK;
    }
    return XXFS_ENOSPC;
}

static int paged_get(struct xxfs *fs, const void *key, u32 klen,
                     void *buf, u32 *vlen)
{
    u64 h = xxh64(key, klen);
    struct paged_dir *d = &fs->pdir;
    u32 dir_idx = dir_index(h, d->global_depth);
    if (dir_idx >= d->dir_size || d->page_off[dir_idx] == (u64)-1)
        return XXFS_ENOENT;
    u64 cur_off = d->page_off[dir_idx];
    while (cur_off != 0) {
        struct paged_page *pg = page_load(fs, cur_off);
        if (!pg)
            return XXFS_EIO;
        for (u16 i = 0; i < pg->count; i++) {
            struct paged_slot *s = &pg->slots[i];
            if (s->hash != h || s->klen != klen)
                continue;
            u32 pn = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
            if (xxfs_os_memcmp(s->key_prefix, key, pn) != 0)
                continue;
            if (s->flags & SLOT_F_INLINE) {
                if (klen > KEY_PREFIX_LEN) {
                    if (xxfs_os_memcmp(key, pg->inline_area + s->file_off, klen) != 0)
                        continue;
                }
                if (s->vlen > *vlen)
                    return XXFS_ENOSPC;
                xxfs_os_memcpy(buf, pg->inline_area + s->file_off + klen, s->vlen);
                *vlen = s->vlen;
                return XXFS_OK;
            }
            u32 rec_len = 8 + klen + s->vlen;
            u8 stack_buf[512];
            u8 *rec;
            if (rec_len <= sizeof(stack_buf)) {
                rec = stack_buf;
            } else {
                rec = xxfs_os_alloc(rec_len);
                if (!rec)
                    continue;
            }
            if (xxfs_os_file_pread(&fs->file, rec, rec_len, (s64)s->file_off) < 0) {
                if (rec != stack_buf)
                    xxfs_os_free(rec);
                continue;
            }
            if (klen > KEY_PREFIX_LEN) {
                if (xxfs_os_memcmp(key, rec + 8, klen) != 0) {
                    if (rec != stack_buf)
                        xxfs_os_free(rec);
                    continue;
                }
            }
            if (s->vlen > *vlen) {
                if (rec != stack_buf)
                    xxfs_os_free(rec);
                return XXFS_ENOSPC;
            }
            xxfs_os_memcpy(buf, rec + 8 + klen, s->vlen);
            if (rec != stack_buf)
                xxfs_os_free(rec);
            *vlen = s->vlen;
            return XXFS_OK;
        }
        cur_off = pg->overflow_next;
    }
    return XXFS_ENOENT;
}

static int paged_del(struct xxfs *fs, const void *key, u32 klen)
{
    u64 h = xxh64(key, klen);
    struct paged_dir *d = &fs->pdir;
    u32 dir_idx = dir_index(h, d->global_depth);
    if (dir_idx >= d->dir_size || d->page_off[dir_idx] == (u64)-1)
        return XXFS_ENOENT;
    u64 cur_off = d->page_off[dir_idx];
    while (cur_off != 0) {
        struct paged_page *pg = page_load(fs, cur_off);
        if (!pg)
            return XXFS_EIO;
        for (u16 i = 0; i < pg->count; i++) {
            struct paged_slot *s = &pg->slots[i];
            if (s->hash != h || s->klen != klen)
                continue;
            u32 pn = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
            if (xxfs_os_memcmp(s->key_prefix, key, pn) != 0)
                continue;
            if (i < pg->count - 1)
                pg->slots[i] = pg->slots[pg->count - 1];
            pg->count--;
            page_writeback(fs, pg, cur_off);
            return XXFS_OK;
        }
        cur_off = pg->overflow_next;
    }
    return XXFS_ENOENT;
}

static void pdir_write(struct xxfs *fs)
{
    struct paged_dir *d = &fs->pdir;
    if (!d->page_off || d->dir_size == 0)
        return;
    u64 off = fs->sb.s_pdir_off;
    u32 bytes = d->dir_size * sizeof(u64);
    if (off == 0) {
        off = page_alloc_raw(fs);
        if (!off)
            return;
        fs->sb.s_pdir_off = off;
    }
    if (bytes > XXFS_BLOCK_SIZE) {
        u64 new_off = page_alloc_raw(fs);
        if (!new_off)
            return;
        off = new_off;
        fs->sb.s_pdir_off = off;
    }
    xxfs_os_file_pwrite(&fs->file, d->page_off, bytes, (s64)off);
    fs->sb.s_pdir_size = d->dir_size;
    fs->sb.s_pdir_depth = d->global_depth;
}

static void super_write(struct xxfs *fs)
{
    pdir_write(fs);
    u32 off = (u32)((char *)&fs->sb.s_checksum - (char *)&fs->sb);
    fs->sb.s_checksum = xxfs_os_crc32c(&fs->sb, off);
    xxfs_os_file_pwrite(&fs->file, &fs->sb, sizeof(fs->sb), 0);
}

static int super_read(struct xxfs *fs)
{
    int r = xxfs_os_file_pread(&fs->file, &fs->sb, sizeof(fs->sb), 0);
    if (r < 0 || (u32)r < sizeof(fs->sb))
        return XXFS_EIO;
    u32 off = (u32)((char *)&fs->sb.s_checksum - (char *)&fs->sb);
    u32 crc = xxfs_os_crc32c(&fs->sb, off);
    if (crc != fs->sb.s_checksum)
        return XXFS_EIO;
    if (fs->sb.s_magic != XXFS_MAGIC)
        return XXFS_EIO;
    return XXFS_OK;
}

static void path_normalize(const char *path, char *out, u32 *out_len)
{
    u32 len = 0;
    const char *p = path;
    if (*p != '/') {
        out[len++] = '/';
    }
    while (*p) {
        if (*p == '/' && len > 0 && out[len - 1] == '/') {
            p++;
            continue;
        }
        if (*p == '.' && (*(p + 1) == '/' || *(p + 1) == 0) &&
            (len == 0 || out[len - 1] == '/')) {
            p++;
            continue;
        }
        out[len++] = *p++;
        if (len >= XXFS_MAX_PATH - 1)
            break;
    }
    while (len > 1 && out[len - 1] == '/')
        len--;
    out[len] = 0;
    *out_len = len;
}

static void inode_to_val(const struct xxfs_inode *ino, void *buf)
{
    xxfs_os_memcpy(buf, ino, sizeof(struct xxfs_inode));
}

static void val_to_inode(const void *buf, struct xxfs_inode *ino)
{
    xxfs_os_memcpy(ino, buf, sizeof(struct xxfs_inode));
}

struct xxfs *xxfs_mount(const char *path, u64 flags)
{
    struct xxfs *fs = xxfs_os_zalloc(sizeof(struct xxfs));
    if (!fs)
        return NULL;

    if (xxfs_os_file_open(&fs->file, path, 0)) {
        xxfs_os_free(fs);
        return NULL;
    }

    xxfs_os_lock_init(&fs->lock);
    fs->flags = flags;

    if (super_read(fs) != XXFS_OK) {
        xxfs_os_file_close(&fs->file);
        xxfs_os_lock_destroy(&fs->lock);
        xxfs_os_free(fs);
        return NULL;
    }

    fs->cow_gen = fs->sb.s_cow_generation;
    fs->wal_seq = fs->sb.s_wal_seq;
    fs->mounted = 1;

    xxfs_mmap_refresh(fs);

    icache_init(fs);
    table_alloc(&fs->tbl, 1024);

    fs->pdir.global_depth = 0;
    fs->pdir.dir_size = 1;
    fs->pdir.page_off = xxfs_os_zalloc(sizeof(u64));
    fs->pdir.page_off[0] = (u64)-1;

    if (fs->sb.s_pdir_off && fs->sb.s_pdir_size > 0) {
        u32 dsz = fs->sb.s_pdir_size;
        u8 depth = fs->sb.s_pdir_depth;
        u64 *buf = xxfs_os_zalloc(dsz * sizeof(u64));
        if (buf) {
            int r = xxfs_os_file_pread(&fs->file, buf, dsz * sizeof(u64),
                                       (s64)fs->sb.s_pdir_off);
            if (r > 0) {
                xxfs_os_free(fs->pdir.page_off);
                fs->pdir.page_off = buf;
                fs->pdir.dir_size = dsz;
                fs->pdir.global_depth = depth;
            } else {
                xxfs_os_free(buf);
            }
        }
    } else {
        s64 fsize = xxfs_os_file_size(&fs->file);
        u64 scan = fs->sb.s_data_off;

        while (scan + sizeof(struct paged_page) <= (u64)fsize) {
            struct paged_page pg;
            int r = xxfs_os_file_pread(&fs->file, &pg, sizeof(pg), (s64)scan);
            if (r < 0)
                break;
            if (pg.count > 0 && pg.count <= PAGED_SLOTS_PER_PAGE) {
                u8 needed = pg.local_depth;
                while (fs->pdir.global_depth < needed)
                    paged_dir_double(&fs->pdir);
                u32 di = dir_index(pg.slots[0].hash, fs->pdir.global_depth);
                if (di < fs->pdir.dir_size && fs->pdir.page_off[di] == (u64)-1)
                    fs->pdir.page_off[di] = scan;
            }
            scan += sizeof(struct paged_page);
        }
    }

    return fs;
}

void xxfs_umount(struct xxfs *fs)
{
    if (!fs)
        return;
    fs_wlock(fs);
    dcache_flush_all(fs);
    pcache_flush_all(fs);
    super_write(fs);
    xxfs_os_file_sync(&fs->file);
    for (int i = 0; i < PCACHE_SIZE; i++) {
        if (fs->pcache[i].page)
            xxfs_os_free(fs->pcache[i].page);
    }
    for (int i = 0; i < DIR_CACHE_SIZE; i++) {
        if (fs->dcache[i].children)
            xxfs_os_free(fs->dcache[i].children);
    }
    if (fs->pdir.page_off)
        xxfs_os_free(fs->pdir.page_off);
    table_destroy(&fs->tbl);
    icache_destroy(fs);
    if (fs->mmap)
        xxfs_os_file_munmap(fs->mmap, fs->mmap_len);
    fs_wunlock(fs);
    xxfs_os_file_close(&fs->file);
    xxfs_os_lock_destroy(&fs->lock);
    xxfs_os_free(fs);
}

int xxfs_create(struct xxfs *fs, const char *path, u16 mode, u16 uid, u16 gid)
{
    if (!fs || !path)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    fs_wlock(fs);

    PROF_BEGIN();
    u64 h = xxh64(norm, nlen);
    PROF_ADD(fs, prof_hash_ns);

    PROF_BEGIN();
    struct icache_entry *ice = icache_lookup(fs, h, norm, nlen);
    PROF_ADD(fs, prof_icache_ns);
    if (ice) {
        fs_wunlock(fs);
        return XXFS_EEXIST;
    }

    struct xxfs_inode ino;
    u64 *p = (u64 *)&ino;
    for (int i = 0; i < (int)(sizeof(ino) / 8); i++)
        p[i] = 0;
    ino.i_mode = mode;
    ino.i_uid = uid;
    ino.i_gid = gid;
    ino.i_file_type = XXFS_FT_REG;
    ino.i_nlinks = 1;
    ino.i_generation = fs->cow_gen;

    const char *name = norm + nlen;
    while (name > norm && *(name - 1) != '/')
        name--;
    u32 name_len = (u32)(norm + nlen - name);
    if (name_len > XXFS_MAX_NAME)
        name_len = XXFS_MAX_NAME;
    xxfs_os_memcpy(ino.i_name, name, name_len);

    char parent[XXFS_MAX_PATH];
    u32 plen;
    get_parent_path(norm, nlen, parent, &plen);

    u8 val_buf[sizeof(struct xxfs_inode)];
    inode_to_val(&ino, val_buf);

    PROF_BEGIN();
    int rc = paged_insert(fs, norm, nlen, val_buf, sizeof(struct xxfs_inode));
    PROF_ADD(fs, prof_pcache_ns);

    if (rc == XXFS_OK) {
        icache_put(fs, h, norm, nlen, &ino);
        dir_add_child(fs, parent, plen, name, XXFS_FT_REG);
        fs->sb.s_inodes_count++;
        fs->sb.s_free_inodes--;
    }

#ifdef XXFS_PROFILE
    fs->prof_create_cnt++;
#endif
    fs_wunlock(fs);
    return rc;
}

int xxfs_mkdir(struct xxfs *fs, const char *path, u16 mode, u16 uid, u16 gid)
{
    if (!fs || !path)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    fs_wlock(fs);

    u64 h = xxh64(norm, nlen);
    struct icache_entry *ice = icache_lookup(fs, h, norm, nlen);
    if (ice) {
        fs_wunlock(fs);
        return XXFS_EEXIST;
    }

    struct xxfs_inode ino;
    u64 *p = (u64 *)&ino;
    for (int i = 0; i < (int)(sizeof(ino) / 8); i++)
        p[i] = 0;
    ino.i_mode = mode;
    ino.i_uid = uid;
    ino.i_gid = gid;
    ino.i_file_type = XXFS_FT_DIR;
    ino.i_nlinks = 2;
    ino.i_generation = fs->cow_gen;

    const char *name = get_basename(norm, nlen);
    u32 name_len = (u32)strlen(name);
    if (name_len > XXFS_MAX_NAME)
        name_len = XXFS_MAX_NAME;
    xxfs_os_memcpy(ino.i_name, name, name_len);
    ino.i_name[name_len] = 0;

    char parent[XXFS_MAX_PATH];
    u32 plen;
    get_parent_path(norm, nlen, parent, &plen);

    u8 val_buf[sizeof(struct xxfs_inode)];
    inode_to_val(&ino, val_buf);
    int rc = paged_insert(fs, norm, nlen, val_buf, sizeof(struct xxfs_inode));

    if (rc == XXFS_OK) {
        icache_put(fs, h, norm, nlen, &ino);
        dir_add_child(fs, parent, plen, name, XXFS_FT_DIR);
        fs->sb.s_inodes_count++;
    }

    fs_wunlock(fs);
    return rc;
}

int xxfs_unlink(struct xxfs *fs, const char *path)
{
    if (!fs || !path)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    fs_wlock(fs);

    u64 h = xxh64(norm, nlen);
    struct icache_entry *ice = icache_lookup(fs, h, norm, nlen);
    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);

    if (ice) {
        xxfs_os_memcpy(&ino, &ice->inode, sizeof(ino));
    } else {
        if (paged_get(fs, norm, nlen, &ino, &vlen) != XXFS_OK) {
            fs_wunlock(fs);
            return XXFS_ENOENT;
        }
        icache_put(fs, h, norm, nlen, &ino);
    }

    if (ino.i_file_type == XXFS_FT_DIR) {
        fs_wunlock(fs);
        return XXFS_EISDIR;
    }

    int rc = paged_del(fs, norm, nlen);
    if (rc == XXFS_OK) {
        icache_del(fs, h, norm, nlen);
        char parent[XXFS_MAX_PATH];
        u32 plen;
        get_parent_path(norm, nlen, parent, &plen);
        const char *bname = get_basename(norm, nlen);
        dir_remove_child(fs, parent, plen, bname);
        fs->sb.s_inodes_count--;
    }

    fs_wunlock(fs);
    return rc;
}

int xxfs_rmdir(struct xxfs *fs, const char *path)
{
    if (!fs || !path)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    if (nlen == 1 && norm[0] == '/')
        return XXFS_EINVAL;

    fs_wlock(fs);

    u64 h = xxh64(norm, nlen);
    struct icache_entry *ice = icache_lookup(fs, h, norm, nlen);
    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);

    if (ice) {
        xxfs_os_memcpy(&ino, &ice->inode, sizeof(ino));
    } else {
        if (paged_get(fs, norm, nlen, &ino, &vlen) != XXFS_OK) {
            fs_wunlock(fs);
            return XXFS_ENOENT;
        }
        icache_put(fs, h, norm, nlen, &ino);
    }

    if (ino.i_file_type != XXFS_FT_DIR) {
        fs_wunlock(fs);
        return XXFS_ENOTDIR;
    }

    int rc = paged_del(fs, norm, nlen);
    if (rc == XXFS_OK) {
        icache_del(fs, h, norm, nlen);
        char parent[XXFS_MAX_PATH];
        u32 plen;
        get_parent_path(norm, nlen, parent, &plen);
        const char *bname = get_basename(norm, nlen);
        dir_remove_child(fs, parent, plen, bname);
        fs->sb.s_inodes_count--;
        if (fs->flags & XXFS_FLAG_SYNC)
            super_write(fs);
    }

    fs_wunlock(fs);
    return rc;
}

int xxfs_stat(struct xxfs *fs, const char *path, struct xxfs_inode *out)
{
    if (!fs || !path || !out)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    fs_rlock(fs);

    u64 h = xxh64(norm, nlen);
    struct icache_entry *ice = icache_lookup(fs, h, norm, nlen);
    if (ice) {
        xxfs_os_memcpy(out, &ice->inode, sizeof(*out));
        fs_runlock(fs);
        return XXFS_OK;
    }

    u32 vlen = sizeof(struct xxfs_inode);
    int rc = paged_get(fs, norm, nlen, out, &vlen);
    if (rc == XXFS_OK)
        icache_put(fs, h, norm, nlen, out);

    fs_runlock(fs);
    return rc;
}

int xxfs_write(struct xxfs *fs, const char *path, const void *buf, u64 off, u32 len, u32 *written)
{
    if (!fs || !path || !buf || !len)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    fs_wlock(fs);

    u64 h = xxh64(norm, nlen);
    struct icache_entry *ice = icache_lookup(fs, h, norm, nlen);
    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);

    if (ice) {
        xxfs_os_memcpy(&ino, &ice->inode, sizeof(ino));
    } else {
        if (paged_get(fs, norm, nlen, &ino, &vlen) != XXFS_OK) {
            fs_wunlock(fs);
            return XXFS_ENOENT;
        }
    }

    if (ino.i_file_type == XXFS_FT_DIR) {
        fs_wunlock(fs);
        return XXFS_EISDIR;
    }

    u64 end_off = off + len;
    if (end_off <= XXFS_INLINE_MAX) {
        xxfs_os_memcpy(ino.i_inline + off, buf, len);
        if (end_off > ino.i_size)
            ino.i_size = end_off;
        
        icache_put(fs, h, norm, nlen, &ino);
        if (written)
            *written = len;
        fs_wunlock(fs);
        return XXFS_OK;
    }

    if (ino.i_size > 0 && ino.i_blocks == 0) {
        u64 old_size = ino.i_size;
        if (old_size > XXFS_INLINE_MAX)
            old_size = XXFS_INLINE_MAX;
        u64 new_off = page_alloc(fs);
        if (!new_off) {
            fs_wunlock(fs);
            return XXFS_ENOSPC;
        }
        if (xxfs_os_file_pwrite(&fs->file, ino.i_inline, (size_t)old_size, (s64)new_off)) {
            fs_wunlock(fs);
            return XXFS_EIO;
        }
        ino.i_extent_off = new_off;
        ino.i_extent_len = 1;
        ino.i_blocks = 1;
    }

    u64 need_end = off + len;
    u64 need_blocks = (need_end + XXFS_BLOCK_SIZE - 1) / XXFS_BLOCK_SIZE;
    
    if (need_blocks > ino.i_blocks) {
        u64 add_blocks = need_blocks - ino.i_blocks;
        u64 new_off = page_alloc_n(fs, add_blocks);
        if (!new_off) {
            fs_wunlock(fs);
            return XXFS_ENOSPC;
        }
        
        if (ino.i_blocks > 0 && ino.i_extent_off) {
            if (new_off == ino.i_extent_off + ino.i_blocks * XXFS_BLOCK_SIZE) {
                // 连续分配，无需拷贝
            } else {
                // 不连续，需要拷贝旧数据
                u8 *tmp = xxfs_os_alloc((size_t)(ino.i_blocks * XXFS_BLOCK_SIZE));
                if (tmp) {
                    xxfs_os_file_pread(&fs->file, tmp, (size_t)(ino.i_blocks * XXFS_BLOCK_SIZE), (s64)ino.i_extent_off);
                    xxfs_os_file_pwrite(&fs->file, tmp, (size_t)(ino.i_blocks * XXFS_BLOCK_SIZE), (s64)new_off);
                    xxfs_os_free(tmp);
                }
            }
        }
        ino.i_extent_off = new_off;
        ino.i_extent_len = (u32)need_blocks;
        ino.i_blocks = need_blocks;
    }

    if (xxfs_os_file_pwrite(&fs->file, buf, len, (s64)(ino.i_extent_off + off))) {
        fs_wunlock(fs);
        return XXFS_EIO;
    }
    if (need_end > ino.i_size)
        ino.i_size = need_end;
    
    icache_put(fs, h, norm, nlen, &ino);
    if (written)
        *written = len;
    
    fs_wunlock(fs);
    return XXFS_OK;
}

int xxfs_read(struct xxfs *fs, const char *path, void *buf, u64 off, u32 len, u32 *read_bytes)
{
    if (!fs || !path || !buf || !len)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    fs_rlock(fs);

    u64 h = xxh64(norm, nlen);
    struct icache_entry *ice = icache_lookup(fs, h, norm, nlen);
    struct xxfs_inode ino;

    if (ice) {
        xxfs_os_memcpy(&ino, &ice->inode, sizeof(ino));
    } else {
        u32 vlen = sizeof(ino);
        if (paged_get(fs, norm, nlen, &ino, &vlen) != XXFS_OK) {
            fs_runlock(fs);
            return XXFS_ENOENT;
        }
        icache_put(fs, h, norm, nlen, &ino);
    }

    if (off >= ino.i_size) {
        fs_runlock(fs);
        if (read_bytes)
            *read_bytes = 0;
        return XXFS_OK;
    }

    u64 avail = ino.i_size - off;
    if (len > avail)
        len = (u32)avail;

    if (ino.i_size <= XXFS_INLINE_MAX && ino.i_blocks == 0) {
        xxfs_os_memcpy(buf, ino.i_inline + off, len);
        if (read_bytes)
            *read_bytes = len;
        fs_runlock(fs);
        return XXFS_OK;
    }

    if (ino.i_extent_off && ino.i_blocks > 0) {
        if (xxfs_os_file_pread(&fs->file, buf, len, (s64)(ino.i_extent_off + off)) < 0) {
            fs_runlock(fs);
            return XXFS_EIO;
        }
        if (read_bytes)
            *read_bytes = len;
        fs_runlock(fs);
        return XXFS_OK;
    }

    fs_runlock(fs);
    return XXFS_EIO;
}

int xxfs_chmod(struct xxfs *fs, const char *path, u16 mode)
{
    if (!fs || !path)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    fs_wlock(fs);
    u64 h = xxh64(norm, nlen);
    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);
    if (paged_get(fs, norm, nlen, &ino, &vlen) != XXFS_OK) {
        fs_wunlock(fs);
        return XXFS_ENOENT;
    }
    ino.i_mode = mode;
    ino.i_ctime = xxfs_os_time();
    ino.i_checksum = xxfs_os_crc32c(&ino, INO_CRC_OFF);
    u8 val_buf[sizeof(struct xxfs_inode)];
    inode_to_val(&ino, val_buf);
    int rc = paged_put(fs, norm, nlen, val_buf, sizeof(struct xxfs_inode));
    if (rc == XXFS_OK)
        icache_put(fs, h, norm, nlen, &ino);
    fs_wunlock(fs);
    return rc;
}

int xxfs_chown(struct xxfs *fs, const char *path, u16 uid, u16 gid)
{
    if (!fs || !path)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    fs_wlock(fs);
    u64 h = xxh64(norm, nlen);
    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);
    if (paged_get(fs, norm, nlen, &ino, &vlen) != XXFS_OK) {
        fs_wunlock(fs);
        return XXFS_ENOENT;
    }
    ino.i_uid = uid;
    ino.i_gid = gid;
    ino.i_ctime = xxfs_os_time();
    ino.i_checksum = xxfs_os_crc32c(&ino, INO_CRC_OFF);
    u8 val_buf[sizeof(struct xxfs_inode)];
    inode_to_val(&ino, val_buf);
    int rc = paged_put(fs, norm, nlen, val_buf, sizeof(struct xxfs_inode));
    if (rc == XXFS_OK)
        icache_put(fs, h, norm, nlen, &ino);
    fs_wunlock(fs);
    return rc;
}

int xxfs_utime(struct xxfs *fs, const char *path, u64 atime, u64 mtime)
{
    if (!fs || !path)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    fs_wlock(fs);
    u64 h = xxh64(norm, nlen);
    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);
    if (paged_get(fs, norm, nlen, &ino, &vlen) != XXFS_OK) {
        fs_wunlock(fs);
        return XXFS_ENOENT;
    }
    ino.i_atime = atime;
    ino.i_mtime = mtime;
    ino.i_ctime = xxfs_os_time();
    ino.i_checksum = xxfs_os_crc32c(&ino, INO_CRC_OFF);
    u8 val_buf[sizeof(struct xxfs_inode)];
    inode_to_val(&ino, val_buf);
    int rc = paged_put(fs, norm, nlen, val_buf, sizeof(struct xxfs_inode));
    if (rc == XXFS_OK)
        icache_put(fs, h, norm, nlen, &ino);
    fs_wunlock(fs);
    return rc;
}

int xxfs_rename(struct xxfs *fs, const char *old_path, const char *new_path)
{
    if (!fs || !old_path || !new_path)
        return XXFS_EINVAL;
    char old_norm[XXFS_MAX_PATH], new_norm[XXFS_MAX_PATH];
    u32 olen, nlen;
    path_normalize(old_path, old_norm, &olen);
    path_normalize(new_path, new_norm, &nlen);

    fs_wlock(fs);

    u64 old_h = xxh64(old_norm, olen);
    u64 new_h = xxh64(new_norm, nlen);

    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);
    if (paged_get(fs, old_norm, olen, &ino, &vlen) != XXFS_OK) {
        fs_wunlock(fs);
        return XXFS_ENOENT;
    }

    ino.i_ctime = xxfs_os_time();
    const char *name = new_norm + nlen;
    while (name > new_norm && *(name - 1) != '/')
        name--;
    u32 name_len = (u32)(new_norm + nlen - name);
    if (name_len > XXFS_MAX_NAME)
        name_len = XXFS_MAX_NAME;
    xxfs_os_memcpy(ino.i_name, name, name_len);
    ino.i_name[name_len] = 0;
    ino.i_checksum = xxfs_os_crc32c(&ino, INO_CRC_OFF);

    u8 val_buf[sizeof(struct xxfs_inode)];
    inode_to_val(&ino, val_buf);

    paged_del(fs, old_norm, olen);
    icache_del(fs, old_h, old_norm, olen);
    int rc = paged_put(fs, new_norm, nlen, val_buf, sizeof(struct xxfs_inode));
    if (rc == XXFS_OK) {
        icache_put(fs, new_h, new_norm, nlen, &ino);
    } else {
        inode_to_val(&ino, val_buf);
        paged_put(fs, old_norm, olen, val_buf, sizeof(struct xxfs_inode));
        icache_put(fs, old_h, old_norm, olen, &ino);
    }

    fs_wunlock(fs);
    return rc;
}

int xxfs_symlink(struct xxfs *fs, const char *target, const char *linkpath)
{
    if (!fs || !target || !linkpath)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(linkpath, norm, &nlen);
    u32 tlen = (u32)strlen(target);
    if (tlen > XXFS_INLINE_MAX)
        return XXFS_ENAMETOOLONG;

    fs_wlock(fs);

    u64 h = xxh64(norm, nlen);
    struct icache_entry *ice = icache_lookup(fs, h, norm, nlen);
    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);
    if (ice || paged_get(fs, norm, nlen, &ino, &vlen) == XXFS_OK) {
        if (!ice)
            icache_put(fs, h, norm, nlen, &ino);
        fs_wunlock(fs);
        return XXFS_EEXIST;
    }

    memset(&ino, 0, sizeof(ino));
    ino.i_file_type = XXFS_FT_LNK;
    ino.i_nlinks = 1;
    ino.i_size = tlen;
    u64 now = xxfs_os_time();
    ino.i_atime = now;
    ino.i_mtime = now;
    ino.i_ctime = now;
    ino.i_btime = now;
    xxfs_os_memcpy(ino.i_inline, target, tlen);
    ino.i_checksum = xxfs_os_crc32c(&ino, INO_CRC_OFF);

    u8 val_buf[sizeof(struct xxfs_inode)];
    inode_to_val(&ino, val_buf);
    int rc = paged_put(fs, norm, nlen, val_buf, sizeof(struct xxfs_inode));
    if (rc == XXFS_OK)
        icache_put(fs, h, norm, nlen, &ino);
    fs_wunlock(fs);
    return rc;
}

int xxfs_readlink(struct xxfs *fs, const char *path, char *buf, u32 *len)
{
    if (!fs || !path || !buf || !len)
        return XXFS_EINVAL;
    struct xxfs_inode ino;
    int rc = xxfs_stat(fs, path, &ino);
    if (rc != XXFS_OK)
        return rc;
    if (ino.i_file_type != XXFS_FT_LNK)
        return XXFS_EINVAL;
    u32 copy_len = (u32)ino.i_size;
    if (copy_len > *len - 1)
        copy_len = *len - 1;
    xxfs_os_memcpy(buf, ino.i_inline, copy_len);
    buf[copy_len] = 0;
    *len = copy_len;
    return XXFS_OK;
}

u64 xxfs_count(const struct xxfs *fs)
{
    return fs ? fs->sb.s_inodes_count : 0;
}

int xxfs_sync(struct xxfs *fs)
{
    if (!fs)
        return XXFS_EINVAL;
    fs_wlock(fs);
    icache_flush_all(fs);
    dcache_flush_all(fs);
    pcache_flush_all(fs);
    super_write(fs);
    xxfs_os_file_sync(&fs->file);
    xxfs_mmap_refresh(fs);
    fs_wunlock(fs);
    return XXFS_OK;
}

int xxfs_open(struct xxfs *fs, const char *path, u32 flags, struct xxfs_file **fp)
{
    if (!fs || !path || !fp)
        return XXFS_EINVAL;
    
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);
    
    fs_rlock(fs);
    
    u64 h = xxh64(norm, nlen);
    struct icache_entry *ice = icache_lookup(fs, h, norm, nlen);
    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);
    
    if (ice) {
        xxfs_os_memcpy(&ino, &ice->inode, sizeof(ino));
    } else {
        if (paged_get(fs, norm, nlen, &ino, &vlen) != XXFS_OK) {
            fs_runlock(fs);
            return XXFS_ENOENT;
        }
    }
    
    if (ino.i_file_type == XXFS_FT_DIR) {
        fs_runlock(fs);
        return XXFS_EISDIR;
    }
    
    struct xxfs_file *f = xxfs_os_alloc(sizeof(*f));
    if (!f) {
        fs_runlock(fs);
        return XXFS_ENOMEM;
    }
    
    f->hash = h;
    f->extent_off = ino.i_extent_off;
    f->size = ino.i_size;
    f->blocks = ino.i_blocks;
    xxfs_os_memcpy(f->path, norm, nlen + 1);
    f->path_len = nlen;
    f->flags = flags;
    
    fs_runlock(fs);
    
    *fp = f;
    return XXFS_OK;
}

int xxfs_close(struct xxfs *fs, struct xxfs_file *fp)
{
    if (!fs || !fp)
        return XXFS_EINVAL;
    
    fs_wlock(fs);
    
    struct icache_entry *ice = icache_lookup(fs, fp->hash, fp->path, fp->path_len);
    if (ice) {
        ice->inode.i_size = fp->size;
        ice->inode.i_blocks = fp->blocks;
        ice->inode.i_mtime = xxfs_os_time();
        ice->inode.i_ctime = ice->inode.i_mtime;
        ice->inode.i_checksum = xxfs_os_crc32c(&ice->inode, INO_CRC_OFF);
        
        u8 val_buf[sizeof(struct xxfs_inode)];
        inode_to_val(&ice->inode, val_buf);
        paged_put(fs, fp->path, fp->path_len, val_buf, sizeof(struct xxfs_inode));
    }
    
    xxfs_os_free(fp);
    
    fs_wunlock(fs);
    return XXFS_OK;
}

ssize_t xxfs_write_fd(struct xxfs *fs, struct xxfs_file *fp, const void *buf, size_t count, u64 off)
{
    if (!fs || !fp || !buf || count == 0)
        return XXFS_EINVAL;
    
    u64 end_off = off + count;
    
    if (end_off > XXFS_INLINE_MAX && fp->blocks == 0) {
        fs_wlock(fs);
        
        u64 need_blocks = (end_off + XXFS_BLOCK_SIZE - 1) / XXFS_BLOCK_SIZE;
        u64 new_off = page_alloc_n(fs, need_blocks);
        if (!new_off) {
            fs_wunlock(fs);
            return XXFS_ENOSPC;
        }
        
        fp->extent_off = new_off;
        fp->blocks = need_blocks;
        
        fs_wunlock(fs);
    } else if (end_off > fp->blocks * XXFS_BLOCK_SIZE) {
        fs_wlock(fs);
        
        u64 need_blocks = (end_off + XXFS_BLOCK_SIZE - 1) / XXFS_BLOCK_SIZE;
        u64 add_blocks = need_blocks - fp->blocks;
        u64 new_off = page_alloc_n(fs, add_blocks);
        if (!new_off) {
            fs_wunlock(fs);
            return XXFS_ENOSPC;
        }
        
        if (fp->blocks > 0 && fp->extent_off) {
            if (new_off != fp->extent_off + fp->blocks * XXFS_BLOCK_SIZE) {
                u8 *tmp = xxfs_os_alloc((size_t)(fp->blocks * XXFS_BLOCK_SIZE));
                if (tmp) {
                    xxfs_os_file_pread(&fs->file, tmp, (size_t)(fp->blocks * XXFS_BLOCK_SIZE), (s64)fp->extent_off);
                    xxfs_os_file_pwrite(&fs->file, tmp, (size_t)(fp->blocks * XXFS_BLOCK_SIZE), (s64)new_off);
                    xxfs_os_free(tmp);
                }
            }
        }
        
        fp->extent_off = new_off;
        fp->blocks = need_blocks;
        
        fs_wunlock(fs);
    }
    
    if (xxfs_os_file_pwrite(&fs->file, buf, count, (s64)(fp->extent_off + off))) {
        return XXFS_EIO;
    }
    
    if (end_off > fp->size)
        fp->size = end_off;
    
    return (ssize_t)count;
}

int xxfs_mkfs(const char *path, u64 size_mb, u32 flags)
{
    (void)flags;
    if (!path || size_mb < 1)
        return XXFS_EINVAL;

    struct xxfs_os_file f;
    if (xxfs_os_file_open(&f, path, 3))
        return XXFS_EIO;

    u64 total_bytes = size_mb * 1024ULL * 1024ULL;
    u64 total_blocks = total_bytes / XXFS_BLOCK_SIZE;
    if (total_blocks < 100)
        total_blocks = 100;

    if (xxfs_os_file_truncate(&f, (s64)total_bytes)) {
        xxfs_os_file_close(&f);
        return XXFS_EIO;
    }

    struct xxfs_super sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_magic = XXFS_MAGIC;
    sb.s_version = XXFS_VERSION;
    sb.s_block_size = XXFS_BLOCK_SIZE;
    sb.s_block_size_bits = XXFS_BLOCK_BITS;
    sb.s_block_count = total_blocks;
    sb.s_free_blocks = total_blocks - 4;
    sb.s_inodes_count = 1;
    sb.s_free_inodes = total_blocks / 4;
    sb.s_root_ino = 1;
    sb.s_cow_generation = 1;
    sb.s_main_idx_root = 0;
    sb.s_wal_off = 1 * XXFS_BLOCK_SIZE;
    sb.s_wal_blocks = XXFS_WAL_BLOCKS;
    sb.s_wal_seq = 0;
    sb.s_wal_commit_seq = 0;
    sb.s_pp_ping_off = (1 + XXFS_WAL_BLOCKS) * XXFS_BLOCK_SIZE;
    sb.s_pp_pong_off = (1 + XXFS_WAL_BLOCKS + XXFS_PP_BLOCKS) * XXFS_BLOCK_SIZE;
    sb.s_pp_blocks = XXFS_PP_BLOCKS;
    sb.s_pp_active = 0;
    sb.s_pp_ping_seq = 0;
    sb.s_pp_pong_seq = 0;
    sb.s_bg_count = (u32)((total_blocks + XXFS_BG_BLOCKS - 1) / XXFS_BG_BLOCKS);
    sb.s_bg_blocks = XXFS_BG_BLOCKS;
    sb.s_bg_desc_off = (1 + XXFS_WAL_BLOCKS + 2 * XXFS_PP_BLOCKS) * XXFS_BLOCK_SIZE;
    sb.s_data_off = sb.s_bg_desc_off + ((u64)sb.s_bg_count * 64 + XXFS_BLOCK_SIZE - 1) / XXFS_BLOCK_SIZE * XXFS_BLOCK_SIZE;
    sb.s_mtime = xxfs_os_time();
    sb.s_wtime = sb.s_mtime;
    sb.s_state = 1;
    sb.s_checksum = xxfs_os_crc32c(&sb, (u32)((char *)&sb.s_checksum - (char *)&sb));

    if (xxfs_os_file_pwrite(&f, &sb, sizeof(sb), 0)) {
        xxfs_os_file_close(&f);
        return XXFS_EIO;
    }

    struct xxfs_inode root_ino;
    memset(&root_ino, 0, sizeof(root_ino));
    root_ino.i_mode = 0755;
    root_ino.i_file_type = XXFS_FT_DIR;
    root_ino.i_nlinks = 2;
    root_ino.i_atime = sb.s_mtime;
    root_ino.i_mtime = sb.s_mtime;
    root_ino.i_ctime = sb.s_mtime;
    root_ino.i_btime = sb.s_mtime;
    root_ino.i_generation = 1;
    xxfs_os_memcpy(root_ino.i_name, "/", 2);
    root_ino.i_checksum = xxfs_os_crc32c(&root_ino, INO_CRC_OFF);

    u64 rec_off = sb.s_data_off;
    u8 rec_hdr[8];
    u32 root_klen = 1;
    u32 root_vlen = sizeof(struct xxfs_inode);
    xxfs_os_memcpy(rec_hdr, &root_klen, 4);
    xxfs_os_memcpy(rec_hdr + 4, &root_vlen, 4);
    xxfs_os_file_pwrite(&f, rec_hdr, 8, (s64)rec_off);
    xxfs_os_file_pwrite(&f, "/", 1, (s64)(rec_off + 8));
    xxfs_os_file_pwrite(&f, &root_ino, sizeof(root_ino), (s64)(rec_off + 9));

    u64 page_off = sb.s_data_off + XXFS_BLOCK_SIZE;
    struct paged_page root_page;
    memset(&root_page, 0, sizeof(root_page));
    root_page.count = 1;
    root_page.local_depth = 0;
    root_page.overflow_next = 0;
    u64 root_hash = xxh64("/", 1);
    root_page.slots[0].hash = root_hash;
    root_page.slots[0].file_off = rec_off;
    root_page.slots[0].klen = 1;
    root_page.slots[0].vlen = sizeof(struct xxfs_inode);
    xxfs_os_memcpy(root_page.slots[0].key_prefix, "/", 1);
    xxfs_os_file_pwrite(&f, &root_page, sizeof(root_page), (s64)page_off);

    u64 pdir_off = page_off + sizeof(struct paged_page);
    pdir_off = (pdir_off + XXFS_BLOCK_SIZE - 1) & ~((u64)XXFS_BLOCK_SIZE - 1);
    u64 pdir_entry = page_off;
    xxfs_os_file_pwrite(&f, &pdir_entry, sizeof(u64), (s64)pdir_off);

    sb.s_pdir_off = pdir_off;
    sb.s_pdir_size = 1;
    sb.s_pdir_depth = 0;
    sb.s_free_blocks = total_blocks - (u32)((pdir_off + XXFS_BLOCK_SIZE - sb.s_data_off) / XXFS_BLOCK_SIZE) - 4;
    sb.s_checksum = xxfs_os_crc32c(&sb, (u32)((char *)&sb.s_checksum - (char *)&sb));
    xxfs_os_file_pwrite(&f, &sb, sizeof(sb), 0);

    xxfs_os_file_sync(&f);
    xxfs_os_file_close(&f);
    return XXFS_OK;
}

static void dcache_flush_entry(struct xxfs *fs, struct dir_cache_entry *e)
{
    if (!e->dirty || !e->valid)
        return;
    u64 dh = xxh64(e->path, e->path_len);
    struct icache_entry *ice = icache_lookup(fs, dh, e->path, e->path_len);
    struct xxfs_inode dino;
    if (ice) {
        xxfs_os_memcpy(&dino, &ice->inode, sizeof(dino));
    } else {
        u32 vlen = sizeof(dino);
        if (paged_get(fs, e->path, e->path_len, &dino, &vlen) != XXFS_OK)
            return;
    }
    if (e->child_count == 0) {
        dino.i_size = 0;
        dino.i_extent_off = 0;
        dino.i_extent_len = 0;
        dino.i_blocks = 0;
    } else {
        u64 new_bytes = (u64)e->child_count * sizeof(struct xxfs_dir_child);
        u32 new_blocks = (u32)((new_bytes + XXFS_BLOCK_SIZE - 1) / XXFS_BLOCK_SIZE);
        if (dino.i_extent_off && dino.i_blocks >= new_blocks) {
            xxfs_os_file_pwrite(&fs->file, e->children, (size_t)new_bytes,
                                (s64)dino.i_extent_off);
            dino.i_size = new_bytes;
        } else {
            u64 new_off = page_alloc(fs);
            if (!new_off)
                return;
            if (xxfs_os_file_pwrite(&fs->file, e->children, (size_t)new_bytes, (s64)new_off))
                return;
            dino.i_extent_off = new_off;
            dino.i_extent_len = new_blocks;
            dino.i_blocks = new_blocks;
            dino.i_size = new_bytes;
        }
    }
    dino.i_mtime = xxfs_os_time();
    dino.i_ctime = dino.i_mtime;
    dino.i_checksum = xxfs_os_crc32c(&dino, INO_CRC_OFF);
    u8 val_buf[sizeof(struct xxfs_inode)];
    xxfs_os_memcpy(val_buf, &dino, sizeof(dino));
    int rc = paged_put(fs, e->path, e->path_len, val_buf, sizeof(dino));
    if (rc == XXFS_OK)
        icache_put(fs, dh, e->path, e->path_len, &dino);
    e->dirty = 0;
}

static struct dir_cache_entry *dcache_find(struct xxfs *fs, const char *path, u32 plen)
{
    u64 dh = xxh64(path, plen);
    u32 idx = (u32)(dh & (DIR_CACHE_SIZE - 1));
    struct dir_cache_entry *e = &fs->dcache[idx];
    if (e->valid && e->path_len == plen &&
        xxfs_os_memcmp(e->path, path, plen) == 0)
        return e;
    return NULL;
}

static struct dir_cache_entry *dcache_alloc(struct xxfs *fs, const char *path, u32 plen)
{
    struct dir_cache_entry *e = dcache_find(fs, path, plen);
    if (e)
        return e;
    u64 dh = xxh64(path, plen);
    u32 idx = (u32)(dh & (DIR_CACHE_SIZE - 1));
    e = &fs->dcache[idx];
    if (e->valid && e->dirty)
        dcache_flush_entry(fs, e);
    if (e->valid && e->children) {
        xxfs_os_free(e->children);
        e->children = NULL;
    }
    xxfs_os_memcpy(e->path, path, plen);
    e->path[plen] = 0;
    e->path_len = plen;
    e->children = NULL;
    e->child_count = 0;
    e->child_cap = 0;
    e->dirty = 0;
    e->valid = 1;
    return e;
}

static void dcache_flush_all(struct xxfs *fs)
{
    for (int i = 0; i < DIR_CACHE_SIZE; i++) {
        if (fs->dcache[i].dirty)
            dcache_flush_entry(fs, &fs->dcache[i]);
    }
}

static int dir_add_child(struct xxfs *fs, const char *dir_path, u32 dplen,
                         const char *name, u8 type)
{
    struct dir_cache_entry *e = dcache_find(fs, dir_path, dplen);
    if (!e) {
        e = dcache_alloc(fs, dir_path, dplen);
        u64 dh = xxh64(dir_path, dplen);
        struct icache_entry *ice = icache_lookup(fs, dh, dir_path, dplen);
        if (ice && ice->inode.i_file_type == XXFS_FT_DIR) {
            struct xxfs_inode *dino = &ice->inode;
            if (dino->i_size > 0 && dino->i_extent_off) {
                u64 child_bytes = dino->i_size;
                u32 cnt = (u32)(child_bytes / sizeof(struct xxfs_dir_child));
                e->children = xxfs_os_alloc((size_t)child_bytes);
                if (e->children) {
                    xxfs_os_file_pread(&fs->file, e->children, (size_t)child_bytes,
                                       (s64)dino->i_extent_off);
                    e->child_count = cnt;
                    e->child_cap = cnt;
                }
            }
        } else {
            struct xxfs_inode dino;
            u32 vlen = sizeof(dino);
            if (paged_get(fs, dir_path, dplen, &dino, &vlen) != XXFS_OK)
                return XXFS_ENOENT;
            if (dino.i_size > 0 && dino.i_extent_off) {
                u64 child_bytes = dino.i_size;
                u32 cnt = (u32)(child_bytes / sizeof(struct xxfs_dir_child));
                e->children = xxfs_os_alloc((size_t)child_bytes);
                if (e->children) {
                    xxfs_os_file_pread(&fs->file, e->children, (size_t)child_bytes,
                                       (s64)dino.i_extent_off);
                    e->child_count = cnt;
                    e->child_cap = cnt;
                }
            }
        }
    }
    u32 nlen = (u32)strlen(name);
    if (e->child_count >= e->child_cap) {
        u32 new_cap = e->child_cap ? e->child_cap * 2 : 16;
        struct xxfs_dir_child *new_ch = xxfs_os_alloc(new_cap * sizeof(struct xxfs_dir_child));
        if (!new_ch)
            return XXFS_ENOMEM;
        if (e->children && e->child_count > 0)
            xxfs_os_memcpy(new_ch, e->children, e->child_count * sizeof(struct xxfs_dir_child));
        if (e->children)
            xxfs_os_free(e->children);
        e->children = new_ch;
        e->child_cap = new_cap;
    }
    memset(&e->children[e->child_count], 0, sizeof(struct xxfs_dir_child));
    if (nlen > XXFS_MAX_NAME)
        nlen = XXFS_MAX_NAME;
    e->children[e->child_count].dc_hash = xxh64(name, nlen);
    xxfs_os_memcpy(e->children[e->child_count].dc_name, name, nlen);
    e->children[e->child_count].dc_type = type;
    e->child_count++;
    e->dirty = 1;
    return XXFS_OK;
}

static int dir_remove_child(struct xxfs *fs, const char *dir_path, u32 dplen,
                            const char *name)
{
    struct dir_cache_entry *e = dcache_find(fs, dir_path, dplen);
    if (!e) {
        e = dcache_alloc(fs, dir_path, dplen);
        u64 dh = xxh64(dir_path, dplen);
        struct icache_entry *ice = icache_lookup(fs, dh, dir_path, dplen);
        if (ice && ice->inode.i_file_type == XXFS_FT_DIR) {
            struct xxfs_inode *dino = &ice->inode;
            if (dino->i_size > 0 && dino->i_extent_off) {
                u64 child_bytes = dino->i_size;
                u32 cnt = (u32)(child_bytes / sizeof(struct xxfs_dir_child));
                e->children = xxfs_os_alloc((size_t)child_bytes);
                if (e->children) {
                    xxfs_os_file_pread(&fs->file, e->children, (size_t)child_bytes,
                                       (s64)dino->i_extent_off);
                    e->child_count = cnt;
                    e->child_cap = cnt;
                }
            }
        } else {
            struct xxfs_inode dino;
            u32 vlen = sizeof(dino);
            if (paged_get(fs, dir_path, dplen, &dino, &vlen) != XXFS_OK)
                return XXFS_ENOENT;
            if (dino.i_size > 0 && dino.i_extent_off) {
                u64 child_bytes = dino.i_size;
                u32 cnt = (u32)(child_bytes / sizeof(struct xxfs_dir_child));
                e->children = xxfs_os_alloc((size_t)child_bytes);
                if (e->children) {
                    xxfs_os_file_pread(&fs->file, e->children, (size_t)child_bytes,
                                       (s64)dino.i_extent_off);
                    e->child_count = cnt;
                    e->child_cap = cnt;
                }
            }
        }
    }
    u32 nlen = (u32)strlen(name);
    u64 target_hash = xxh64(name, nlen);
    int found = -1;
    for (u32 i = 0; i < e->child_count; i++) {
        if (e->children[i].dc_hash == target_hash &&
            xxfs_os_memcmp(e->children[i].dc_name, name, nlen + 1) == 0) {
            found = (int)i;
            break;
        }
    }
    if (found < 0)
        return XXFS_ENOENT;
    if ((u32)found < e->child_count - 1)
        e->children[found] = e->children[e->child_count - 1];
    e->child_count--;
    e->dirty = 1;
    return XXFS_OK;
}

static int get_parent_path(const char *norm, u32 nlen, char *parent, u32 *plen)
{
    if (nlen <= 1) {
        parent[0] = '/';
        parent[1] = 0;
        *plen = 1;
        return XXFS_OK;
    }
    const char *last_slash = norm + nlen;
    while (last_slash > norm && *(last_slash - 1) != '/')
        last_slash--;
    if (last_slash <= norm + 1) {
        parent[0] = '/';
        parent[1] = 0;
        *plen = 1;
    } else {
        u32 l = (u32)(last_slash - norm - 1);
        xxfs_os_memcpy(parent, norm, l);
        parent[l] = 0;
        *plen = l;
    }
    return XXFS_OK;
}

static const char *get_basename(const char *norm, u32 nlen)
{
    const char *name = norm + nlen;
    while (name > norm && *(name - 1) != '/')
        name--;
    return name;
}

int xxfs_readdir(struct xxfs *fs, const char *path, struct xxfs_readdir_ctx *ctx)
{
    if (!fs || !path || !ctx)
        return XXFS_EINVAL;
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    path_normalize(path, norm, &nlen);

    memset(ctx, 0, sizeof(*ctx));

    fs_rlock(fs);

    struct dir_cache_entry *e = dcache_find(fs, norm, nlen);
    if (e) {
        if (e->child_count == 0) {
            fs_runlock(fs);
            return XXFS_OK;
        }
        ctx->count = e->child_count;
        ctx->cap = e->child_count;
        ctx->entries = xxfs_os_alloc(e->child_count * sizeof(struct xxfs_dir_child));
        if (!ctx->entries) {
            fs_runlock(fs);
            return XXFS_ENOMEM;
        }
        xxfs_os_memcpy(ctx->entries, e->children,
                       e->child_count * sizeof(struct xxfs_dir_child));
        fs_runlock(fs);
        return XXFS_OK;
    }

    struct xxfs_inode ino;
    u32 vlen = sizeof(ino);
    if (paged_get(fs, norm, nlen, &ino, &vlen) != XXFS_OK) {
        fs_runlock(fs);
        return XXFS_ENOENT;
    }
    if (ino.i_file_type != XXFS_FT_DIR) {
        fs_runlock(fs);
        return XXFS_ENOTDIR;
    }
    if (ino.i_size == 0 || !ino.i_extent_off) {
        fs_runlock(fs);
        return XXFS_OK;
    }

    u64 child_bytes = ino.i_size;
    struct xxfs_dir_child *children = xxfs_os_alloc((size_t)child_bytes);
    if (!children) {
        fs_runlock(fs);
        return XXFS_ENOMEM;
    }
    if (xxfs_os_file_pread(&fs->file, children, (size_t)child_bytes,
                           (s64)ino.i_extent_off) < 0) {
        xxfs_os_free(children);
        fs_runlock(fs);
        return XXFS_EIO;
    }

    u32 count = (u32)(child_bytes / sizeof(struct xxfs_dir_child));
    ctx->count = count;
    ctx->cap = count;
    ctx->entries = children;

    fs_runlock(fs);
    return XXFS_OK;
}

void xxfs_readdir_free(struct xxfs_readdir_ctx *ctx)
{
    if (ctx && ctx->entries) {
        xxfs_os_free(ctx->entries);
        ctx->entries = NULL;
    }
    if (ctx) {
        ctx->count = 0;
        ctx->cap = 0;
    }
}

int xxfs_defrag_file(struct xxfs *fs, const char *path)
{
    (void)fs;
    (void)path;
    return XXFS_OK;
}
int xxfs_defrag_all(struct xxfs *fs)
{
    (void)fs;
    return XXFS_OK;
}
int xxfs_defrag_index(struct xxfs *fs)
{
    (void)fs;
    return XXFS_OK;
}
int xxfs_snapshot_create(struct xxfs *fs, const char *name)
{
    (void)fs;
    (void)name;
    return XXFS_OK;
}
int xxfs_snapshot_rollback(struct xxfs *fs, const char *name)
{
    (void)fs;
    (void)name;
    return XXFS_OK;
}
int xxfs_lrc_verify(struct xxfs *fs, u32 group_id)
{
    (void)fs;
    (void)group_id;
    return XXFS_OK;
}
int xxfs_lrc_repair(struct xxfs *fs, u32 group_id, u32 index)
{
    (void)fs;
    (void)group_id;
    (void)index;
    return XXFS_OK;
}

int xxfs_info(const struct xxfs *fs, struct xxfs_fs_info *info)
{
    if (!fs || !info)
        return XXFS_EINVAL;
    info->version = fs->sb.s_version;
    info->block_size = fs->sb.s_block_size;
    info->block_count = fs->sb.s_block_count;
    info->free_blocks = fs->sb.s_free_blocks;
    info->inodes_count = fs->sb.s_inodes_count;
    info->cow_generation = fs->sb.s_cow_generation;
    return XXFS_OK;
}

#ifdef XXFS_PROFILE
int xxfs_get_profile(struct xxfs *fs, struct xxfs_profile *prof)
{
    if (!fs || !prof)
        return XXFS_EINVAL;
    prof->hash_ns = fs->prof_hash_ns;
    prof->icache_ns = fs->prof_icache_ns;
    prof->pcache_ns = fs->prof_pcache_ns;
    prof->pwrite_ns = fs->prof_pwrite_ns;
    prof->pread_ns = fs->prof_pread_ns;
    prof->alloc_ns = fs->prof_alloc_ns;
    prof->dcache_ns = fs->prof_dcache_ns;
    prof->create_cnt = fs->prof_create_cnt;
    prof->write_cnt = fs->prof_write_cnt;
    return XXFS_OK;
}
#endif
