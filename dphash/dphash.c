#define _DPHASH_OS_IMPL
#include "dphash.h"
#include <stdio.h>

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#define PAGE_SIZE 4096
#define GROUP_SIZE 16
#define CTRL_EMPTY 0x80U
#define CTRL_DELETED 0xFEU
#define CTRL_MASK 0x7FU
#define INIT_CAP (1U << 16)
#define MAX_LOAD_NUM 7
#define MAX_LOAD_DEN 8
#define TOMBSTONE_RATIO 4
#define ARENA_BLK_SIZE (64 << 10)
#define REHASH_BATCH 64

#define PAGED_SLOT_SZ 40
#define PAGED_HDR_SZ 16
#define PAGED_SLOTS_PER ((PAGE_SIZE - PAGED_HDR_SZ) / PAGED_SLOT_SZ)
#define KEY_PREFIX_LEN 16
#define MMAP_INIT_SZ (4 << 20)
#define MAX_LOCAL_DEPTH 16

#define PCACHE_BITS 11
#define PCACHE_SIZE (1U << PCACHE_BITS)
#define PCACHE_MASK (PCACHE_SIZE - 1)

#define DPH_RLOCK(dp)                         \
    do {                                      \
        if (!((dp)->flags & DPHASH_LOCKLESS)) \
            dph_os_read_lock(&(dp)->lock);    \
    } while (0)
#define DPH_RUNLOCK(dp)                       \
    do {                                      \
        if (!((dp)->flags & DPHASH_LOCKLESS)) \
            dph_os_read_unlock(&(dp)->lock);  \
    } while (0)
#define DPH_WLOCK(dp)                         \
    do {                                      \
        if (!((dp)->flags & DPHASH_LOCKLESS)) \
            dph_os_write_lock(&(dp)->lock);   \
    } while (0)
#define DPH_WUNLOCK(dp)                       \
    do {                                      \
        if (!((dp)->flags & DPHASH_LOCKLESS)) \
            dph_os_write_unlock(&(dp)->lock); \
    } while (0)

struct dp_slot {
    u64 hash;
    void *key;
    void *val;
    u32 klen;
    u32 vlen;
    u64 file_off;
};

struct dp_arena {
    struct dp_arena *next;
    u32 used;
    u32 cap;
    u8 data[];
};

struct dp_table {
    u8 *ctrl;
    struct dp_slot *slots;
    u64 cap;
    u64 mask;
    u64 count;
    u64 tombstones;
    u8 *old_ctrl;
    struct dp_slot *old_slots;
    u64 old_cap;
    u64 old_mask;
    u64 rehash_pos;
    u8 rehashing;
};

struct paged_slot {
    u64 hash;
    u64 file_off;
    u8 key_prefix[KEY_PREFIX_LEN];
    u16 klen;
    u16 vlen;
    u8 _pad[4];
};

struct paged_page {
    u64 overflow_next;
    u16 count;
    u8 local_depth;
    u8 _pad;
    u32 _resv;
    struct paged_slot slots[PAGED_SLOTS_PER];
};

struct paged_dir {
    u64 *page_off;
    u8 global_depth;
    u32 dir_size;
    u64 count;
};

struct pcache_entry {
    u64 page_off;
    struct paged_page *page;
    u8 dirty;
    u8 valid;
};

struct page_cache {
    struct pcache_entry slots[PCACHE_SIZE];
    u32 count;
};

struct dphash {
    union {
        struct dp_table tbl;
        struct paged_dir pdir;
    };

    struct dp_arena *arena_head;
    struct dp_arena *arena_cur;

    struct dphash_file file;
    u8 *mmap;
    size_t mmap_len;
    s64 data_off;
    u64 flags;
    u64 count;
    struct dphash_lock lock;

    struct page_cache pcache;
};

static always_inline u64 __rotl64(u64 x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static always_inline u32 __get_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static always_inline u64 __get_u64(const u8 *p)
{
    return (u64)__get_u32(p) | ((u64)__get_u32(p + 4) << 32);
}

static always_inline void __put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static always_inline u64 __xxh64(const void *data, u32 len)
{
    const u64 P1 = 0x9e3779b97f4a7c15ULL;
    const u64 P2 = 0x85ebca6bcae6d1d5ULL;
    const u64 P3 = 0xc2b2ae3cc27b9a21ULL;
    const u64 P4 = 0x27d4eb2f165b7d03ULL;
    const u64 P5 = 0x659e8513b3a1b327ULL;
    const u8 *p = (const u8 *)data;
    const u8 *const end = p + len;
    u64 h;

    if (len >= 32) {
        u64 v1 = P1 + P2;
        u64 v2 = P2;
        u64 v3 = 0;
        u64 v4 = -P1;
        const u8 *limit = end - 32;
        do {
            v1 = __rotl64(v1 + __get_u64(p) * P2, 31) * P1;
            p += 8;
            v2 = __rotl64(v2 + __get_u64(p) * P2, 31) * P1;
            p += 8;
            v3 = __rotl64(v3 + __get_u64(p) * P2, 31) * P1;
            p += 8;
            v4 = __rotl64(v4 + __get_u64(p) * P2, 31) * P1;
            p += 8;
        } while (p <= limit);
        h = __rotl64(v1, 1) + __rotl64(v2, 7) + __rotl64(v3, 12) + __rotl64(v4, 18);
        h += (u64)len;
        h = __rotl64(v3 * P1, 17) * P4 + h;
        h = __rotl64(v4 * P1, 17) * P4 + h;
        h = __rotl64(v2 * P1, 17) * P4 + h;
        h = __rotl64(v1 * P1, 17) * P4 + h;
    } else {
        h = P5 + (u64)len;
    }

    if (p + 16 <= end) {
        h = __rotl64(h + __get_u64(p) * P3, 23) * P2 + __rotl64(h + __get_u64(p + 8) * P3, 23) * P2;
        p += 16;
    }

    if (p + 8 <= end) {
        h = __rotl64(h + __get_u64(p) * P3, 23) * P2;
        p += 8;
    }

    if (p + 4 <= end) {
        h = __rotl64(h + (u64)__get_u32(p) * P4, 17) * P1;
        p += 4;
    }

    while (p < end) {
        h = __rotl64(h + (u64)(*p) * P5, 11) * P1;
        p++;
    }

    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h < 2 ? h + 2 : h;
}

static void *__arena_alloc(struct dphash *dp, u32 size)
{
    size = (size + 7) & ~(u32)7;
    if (unlikely(!dp->arena_cur ||
                 dp->arena_cur->used + size > dp->arena_cur->cap)) {
        u32 cap = size > ARENA_BLK_SIZE ? size : ARENA_BLK_SIZE;
        struct dp_arena *blk = dph_os_alloc(sizeof(*blk) + cap);
        if (unlikely(!blk))
            return NULL;
        blk->next = NULL;
        blk->used = 0;
        blk->cap = cap;
        if (dp->arena_cur)
            dp->arena_cur->next = blk;
        else
            dp->arena_head = blk;
        dp->arena_cur = blk;
    }
    void *ptr = dp->arena_cur->data + dp->arena_cur->used;
    dp->arena_cur->used += size;
    return ptr;
}

static void __arena_destroy(struct dphash *dp)
{
    struct dp_arena *a = dp->arena_head;
    while (a) {
        struct dp_arena *next = a->next;
        dph_os_free(a);
        a = next;
    }
    dp->arena_head = NULL;
    dp->arena_cur = NULL;
}

static size_t __arena_total(const struct dphash *dp)
{
    size_t total = 0;
    for (struct dp_arena *a = dp->arena_head; a; a = a->next)
        total += sizeof(*a) + a->cap;
    return total;
}

#ifdef __SSE2__
static always_inline u32 __match_group(const u8 *ctrl, u8 tag)
{
    __m128i cv = _mm_loadu_si128((const __m128i *)ctrl);
    __m128i tv = _mm_set1_epi8((char)tag);
    return (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(cv, tv));
}
#else
static always_inline u32 __match_group(const u8 *ctrl, u8 tag)
{
    u32 mask = 0;
    for (u32 i = 0; i < GROUP_SIZE; i++)
        if (ctrl[i] == tag)
            mask |= (1U << i);
    return mask;
}
#endif

static always_inline u32 __match_empty(const u8 *ctrl)
{
    return __match_group(ctrl, CTRL_EMPTY);
}

static always_inline u8 __ctrl_tag(u64 hash)
{
    return (u8)((hash >> 57) & CTRL_MASK);
}

static always_inline u64 __group_idx(u64 hash, u64 mask)
{
    return (hash >> 4) & (mask >> 4);
}

static int __table_init(struct dp_table *t, u64 cap)
{
    cap = (cap + GROUP_SIZE - 1) & ~(u64)(GROUP_SIZE - 1);
    if (cap < GROUP_SIZE)
        cap = GROUP_SIZE;
    t->ctrl = dph_os_zalloc(cap);
    if (!t->ctrl)
        return DPHASH_ENOMEM;
    dph_os_memset(t->ctrl, CTRL_EMPTY, cap);
    t->slots = dph_os_zalloc(cap * sizeof(struct dp_slot));
    if (!t->slots) {
        dph_os_free(t->ctrl);
        return DPHASH_ENOMEM;
    }
    t->cap = cap;
    t->mask = cap - 1;
    t->count = 0;
    t->tombstones = 0;
    t->old_ctrl = NULL;
    t->old_slots = NULL;
    t->old_cap = 0;
    t->old_mask = 0;
    t->rehash_pos = 0;
    t->rehashing = 0;
    return DPHASH_OK;
}

static void __table_destroy(struct dp_table *t)
{
    dph_os_free(t->ctrl);
    dph_os_free(t->slots);
    if (t->old_ctrl)
        dph_os_free(t->old_ctrl);
    if (t->old_slots)
        dph_os_free(t->old_slots);
}

static int __table_insert_new(struct dp_table *t, u64 hash,
                              void *key, void *val, u32 klen, u32 vlen,
                              u64 file_off)
{
    u8 tag = __ctrl_tag(hash);
    u64 g = __group_idx(hash, t->mask);
    u64 nr_groups = t->cap / GROUP_SIZE;
    s64 first_del = -1;

    for (u64 probe = 0; probe < nr_groups; probe++) {
        const u8 *gctrl = t->ctrl + g * GROUP_SIZE;

        u32 match = __match_group(gctrl, tag);
        if (match)
            prefetch_r(&t->slots[g * GROUP_SIZE]);
        while (match) {
            u32 bit = (u32)__builtin_ctz(match);
            u64 idx = g * GROUP_SIZE + bit;
            if (t->slots[idx].hash == hash &&
                t->slots[idx].klen == klen &&
                t->slots[idx].key &&
                dph_os_memcmp(t->slots[idx].key, key, klen) == 0) {
                t->slots[idx].val = val;
                t->slots[idx].vlen = vlen;
                t->slots[idx].file_off = file_off;
                return 1;
            }
            match &= match - 1;
        }

        if (first_del < 0) {
            u32 del_mask = __match_group(gctrl, CTRL_DELETED);
            if (del_mask)
                first_del = (s64)(g * GROUP_SIZE + __builtin_ctz(del_mask));
        }

        u32 empties = __match_empty(gctrl);
        if (empties) {
            u32 bit = (u32)__builtin_ctz(empties);
            u64 idx = (first_del >= 0) ? (u64)first_del
                                       : g * GROUP_SIZE + bit;
            bool was_tomb = (t->ctrl[idx] == CTRL_DELETED);
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
        prefetch_r(t->ctrl + ((g + 1) & (nr_groups - 1)) * GROUP_SIZE);
        g = (g + 1) & (nr_groups - 1);
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

static s64 __table_find_in(const u8 *ctrl, const struct dp_slot *slots,
                           u64 cap, u64 mask, u64 hash,
                           const void *key, u32 klen)
{
    u8 tag = __ctrl_tag(hash);
    u64 g = __group_idx(hash, mask);
    u64 nr_groups = cap / GROUP_SIZE;

    for (u64 probe = 0; probe < nr_groups; probe++) {
        const u8 *gctrl = ctrl + g * GROUP_SIZE;

        u32 match = __match_group(gctrl, tag);
        if (match) {
            prefetch_r(&slots[g * GROUP_SIZE]);
        }
        while (match) {
            u32 bit = (u32)__builtin_ctz(match);
            u64 idx = g * GROUP_SIZE + bit;
            if (slots[idx].hash == hash &&
                slots[idx].klen == klen &&
                slots[idx].key &&
                dph_os_memcmp(slots[idx].key, key, klen) == 0)
                return (s64)idx;
            match &= match - 1;
        }
        if (__match_empty(gctrl))
            return -1;
        prefetch_r(ctrl + ((g + 1) & (nr_groups - 1)) * GROUP_SIZE);
        g = (g + 1) & (nr_groups - 1);
    }
    return -1;
}

static always_inline s64 __table_find(const struct dp_table *t, u64 hash,
                                      const void *key, u32 klen)
{
    s64 idx = __table_find_in(t->ctrl, t->slots, t->cap, t->mask,
                              hash, key, klen);
    if (idx >= 0)
        return idx;
    if (t->rehashing) {
        idx = __table_find_in(t->old_ctrl, t->old_slots,
                              t->old_cap, t->old_mask,
                              hash, key, klen);
        if (idx >= 0)
            return idx | (1ULL << 63);
    }
    return -1;
}

static void __table_migrate_batch(struct dp_table *t)
{
    if (!t->rehashing)
        return;

    u64 end = t->rehash_pos + REHASH_BATCH;
    if (end > t->old_cap)
        end = t->old_cap;

    for (u64 i = t->rehash_pos; i < end; i++) {
        if (t->old_ctrl[i] != CTRL_EMPTY && t->old_ctrl[i] != CTRL_DELETED) {
            __table_insert_new(t, t->old_slots[i].hash,
                               t->old_slots[i].key,
                               t->old_slots[i].val,
                               t->old_slots[i].klen,
                               t->old_slots[i].vlen,
                               t->old_slots[i].file_off);
        }
    }
    t->rehash_pos = end;

    if (t->rehash_pos >= t->old_cap) {
        dph_os_free(t->old_ctrl);
        dph_os_free(t->old_slots);
        t->old_ctrl = NULL;
        t->old_slots = NULL;
        t->old_cap = 0;
        t->old_mask = 0;
        t->rehash_pos = 0;
        t->rehashing = 0;
    }
}

static void __table_start_rehash(struct dp_table *t, u64 new_cap)
{
    new_cap = (new_cap + GROUP_SIZE - 1) & ~(u64)(GROUP_SIZE - 1);
    if (new_cap < t->cap * 2)
        new_cap = t->cap * 2;

    u8 *new_ctrl = dph_os_zalloc(new_cap);
    if (!new_ctrl)
        return;
    dph_os_memset(new_ctrl, CTRL_EMPTY, new_cap);

    struct dp_slot *new_slots = dph_os_zalloc(new_cap * sizeof(struct dp_slot));
    if (!new_slots) {
        dph_os_free(new_ctrl);
        return;
    }

    t->old_ctrl = t->ctrl;
    t->old_slots = t->slots;
    t->old_cap = t->cap;
    t->old_mask = t->mask;
    t->rehash_pos = 0;
    t->rehashing = 1;

    t->ctrl = new_ctrl;
    t->slots = new_slots;
    t->cap = new_cap;
    t->mask = new_cap - 1;
    t->tombstones = 0;
}

static always_inline bool __table_needs_grow(const struct dp_table *t)
{
    return t->count + t->tombstones >= t->cap * MAX_LOAD_NUM / MAX_LOAD_DEN;
}

static void __table_ensure_space(struct dp_table *t)
{
    if (!t->rehashing && __table_needs_grow(t)) {
        if (t->tombstones > t->count &&
            t->tombstones > t->cap / TOMBSTONE_RATIO)
            __table_start_rehash(t, t->cap);
        else
            __table_start_rehash(t, t->cap * 2);
    }
    if (t->rehashing)
        __table_migrate_batch(t);
}

static s64 __write_record(struct dphash *dp, const void *key, u32 klen,
                          const void *val, u32 vlen)
{
    u32 total = 8 + klen + vlen;
    u8 sbuf[256];
    u8 *rec = (total <= sizeof(sbuf)) ? sbuf : dph_os_alloc(total);
    if (!rec)
        return -1;
    __put_u32(rec, klen);
    __put_u32(rec + 4, vlen);
    dph_os_memcpy(rec + 8, key, klen);
    dph_os_memcpy(rec + 8 + klen, val, vlen);

    s64 off = dp->data_off;
    if (dph_os_file_pwrite(&dp->file, rec, total, off)) {
        if (rec != sbuf)
            dph_os_free(rec);
        return -1;
    }
    dp->data_off += total;
    if (rec != sbuf)
        dph_os_free(rec);

    if (dp->flags & DPHASH_FSYNC)
        dph_os_file_sync(&dp->file);

    return off;
}

static always_inline u32 __dir_index(u64 hash, u8 global_depth)
{
    if (global_depth == 0)
        return 0;
    return (u32)(hash & ((1ULL << global_depth) - 1));
}

static always_inline void __set_key_prefix(u8 *prefix, const void *key, u32 klen)
{
    u32 n = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
    dph_os_memcpy(prefix, key, n);
    if (n < KEY_PREFIX_LEN)
        dph_os_memset(prefix + n, 0, KEY_PREFIX_LEN - n);
}

static always_inline bool __key_prefix_match(const u8 *prefix, const void *key, u32 klen)
{
    u32 n = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
    return dph_os_memcmp(prefix, key, n) == 0;
}

static int __paged_dir_init(struct paged_dir *d)
{
    d->dir_size = 1;
    d->global_depth = 0;
    d->page_off = dph_os_zalloc(d->dir_size * sizeof(u64));
    if (!d->page_off)
        return DPHASH_ENOMEM;
    d->page_off[0] = (u64)-1;
    d->count = 0;
    return DPHASH_OK;
}

static void __paged_dir_destroy(struct paged_dir *d)
{
    dph_os_free(d->page_off);
}

static int __paged_dir_double(struct paged_dir *d)
{
    u32 new_size = d->dir_size * 2;
    u64 *new_off = dph_os_zalloc(new_size * sizeof(u64));
    if (!new_off)
        return DPHASH_ENOMEM;
    for (u32 i = 0; i < d->dir_size; i++) {
        new_off[i] = d->page_off[i];
        new_off[i + d->dir_size] = d->page_off[i];
    }
    dph_os_free(d->page_off);
    d->page_off = new_off;
    d->dir_size = new_size;
    d->global_depth++;
    return DPHASH_OK;
}

static struct paged_page *__pcache_get(struct dphash *dp, u64 page_off)
{
    struct page_cache *c = &dp->pcache;
    u32 idx = (u32)(page_off >> 4) & PCACHE_MASK;
    for (u32 i = 0; i < PCACHE_SIZE; i++) {
        u32 pos = (idx + i) & PCACHE_MASK;
        if (!c->slots[pos].valid)
            return NULL;
        if (c->slots[pos].page_off == page_off)
            return c->slots[pos].page;
    }
    return NULL;
}

static void __pcache_put(struct dphash *dp, u64 page_off,
                         struct paged_page *page, u8 dirty)
{
    struct page_cache *c = &dp->pcache;
    u32 idx = (u32)(page_off >> 4) & PCACHE_MASK;

    for (u32 i = 0; i < PCACHE_SIZE; i++) {
        u32 pos = (idx + i) & PCACHE_MASK;
        if (!c->slots[pos].valid) {
            c->slots[pos].page_off = page_off;
            c->slots[pos].page = page;
            c->slots[pos].dirty = dirty;
            c->slots[pos].valid = 1;
            c->count++;
            return;
        }
        if (c->slots[pos].page_off == page_off) {
            c->slots[pos].dirty |= dirty;
            return;
        }
    }

    for (u32 i = 0; i < PCACHE_SIZE; i++) {
        if (c->slots[i].valid && !c->slots[i].dirty) {
            dph_os_free(c->slots[i].page);
            c->slots[i].page_off = page_off;
            c->slots[i].page = page;
            c->slots[i].dirty = dirty;
            c->slots[i].valid = 1;
            return;
        }
    }

    for (u32 i = 0; i < PCACHE_SIZE; i++) {
        if (c->slots[i].valid && c->slots[i].dirty) {
            dph_os_file_pwrite(&dp->file, c->slots[i].page,
                               sizeof(struct paged_page),
                               c->slots[i].page_off);
            dph_os_free(c->slots[i].page);
            c->slots[i].page_off = page_off;
            c->slots[i].page = page;
            c->slots[i].dirty = dirty;
            return;
        }
    }
}

static void __pcache_flush(struct dphash *dp)
{
    struct page_cache *c = &dp->pcache;
    for (u32 i = 0; i < PCACHE_SIZE; i++) {
        if (c->slots[i].valid && c->slots[i].dirty) {
            dph_os_file_pwrite(&dp->file, c->slots[i].page,
                               sizeof(struct paged_page),
                               c->slots[i].page_off);
            c->slots[i].dirty = 0;
        }
    }
}

static void __pcache_destroy(struct dphash *dp)
{
    struct page_cache *c = &dp->pcache;
    __pcache_flush(dp);
    for (u32 i = 0; i < PCACHE_SIZE; i++) {
        if (c->slots[i].valid)
            dph_os_free(c->slots[i].page);
    }
    dph_os_memset(c, 0, sizeof(*c));
}

static always_inline struct paged_page *__page_ptr(struct dphash *dp, u64 page_off)
{
    if (dp->mmap && page_off + sizeof(struct paged_page) <= dp->mmap_len)
        return (struct paged_page *)(dp->mmap + page_off);
    return NULL;
}

static struct paged_page *__page_load(struct dphash *dp, u64 page_off)
{
    struct paged_page *direct = __page_ptr(dp, page_off);
    if (direct)
        return direct;

    struct paged_page *cached = __pcache_get(dp, page_off);
    if (cached)
        return cached;

    struct paged_page *page = dph_os_alloc(sizeof(struct paged_page));
    if (!page)
        return NULL;

    dph_os_memset(page, 0, sizeof(struct paged_page));
    dph_os_file_pread(&dp->file, page, sizeof(struct paged_page), page_off);
    __pcache_put(dp, page_off, page, 0);
    return page;
}

static int __mmap_ensure(struct dphash *dp, size_t needed)
{
    if (dp->mmap && needed <= dp->mmap_len)
        return DPHASH_OK;

    size_t new_len = dp->mmap_len ? dp->mmap_len : MMAP_INIT_SZ;
    while (new_len < needed)
        new_len <<= 1;

    if (dp->mmap) {
        dph_os_file_msync(dp->mmap, dp->mmap_len);
        dph_os_file_munmap(dp->mmap, dp->mmap_len);
    }

    if (dph_os_file_extend(&dp->file, (s64)new_len))
        return DPHASH_EIO;

    dp->mmap = dph_os_file_mmap(&dp->file, new_len);
    if (!dp->mmap) {
        dp->mmap_len = 0;
        return DPHASH_EIO;
    }
    dp->mmap_len = new_len;
    return DPHASH_OK;
}

static s64 __alloc_page(struct dphash *dp)
{
    s64 off = dp->data_off;
    dp->data_off += PAGE_SIZE;
    if (__mmap_ensure(dp, (size_t)dp->data_off))
        return -1;
    struct paged_page *p = __page_ptr(dp, (u64)off);
    if (p)
        dph_os_memset(p, 0, sizeof(struct paged_page));
    return off;
}

struct dphash *dphash_open(const char *path, u64 flags)
{
    struct dphash *dp = dph_os_zalloc(sizeof(*dp));
    if (!dp)
        return NULL;

    dph_os_memset(&dp->file, 0, sizeof(dp->file));
    dp->flags = flags;
    if (!(flags & DPHASH_LOCKLESS))
        dph_os_lock_init(&dp->lock);

    if (dph_os_mkdir(path))
        goto fail;

    if (dph_os_file_open(&dp->file, path))
        goto fail;

    s64 sz = dph_os_file_size(&dp->file);
    if (sz < 0)
        goto fail;
    dp->data_off = sz;

    if (flags & DPHASH_PAGED) {
        if (__paged_dir_init(&dp->pdir))
            goto fail;

        if (__mmap_ensure(dp, (size_t)(sz > 0 ? sz : MMAP_INIT_SZ)))
            goto fail;
    } else {
        if (__table_init(&dp->tbl, INIT_CAP))
            goto fail;
    }

    return dp;

fail:
    dphash_close(dp);
    return NULL;
}

void dphash_close(struct dphash *dp)
{
    if (!dp)
        return;
    DPH_WLOCK(dp);

    if (!(dp->flags & DPHASH_PAGED) && !(dp->flags & DPHASH_FSYNC)) {
        dph_os_file_truncate(&dp->file, 0);
        dp->data_off = 0;
        struct dp_table *t = &dp->tbl;
        for (u64 i = 0; i < t->cap; i++) {
            if (t->ctrl[i] != CTRL_EMPTY && t->ctrl[i] != CTRL_DELETED) {
                struct dp_slot *s = &t->slots[i];
                if (s->val && s->vlen > 0)
                    s->file_off = __write_record(dp, s->key,
                                                 s->klen, s->val, s->vlen);
            }
        }
        if (t->rehashing) {
            for (u64 i = 0; i < t->old_cap; i++) {
                if (t->old_ctrl[i] != CTRL_EMPTY &&
                    t->old_ctrl[i] != CTRL_DELETED) {
                    struct dp_slot *s = &t->old_slots[i];
                    if (s->val && s->vlen > 0)
                        s->file_off = __write_record(dp, s->key,
                                                     s->klen, s->val, s->vlen);
                }
            }
        }
        dph_os_file_sync(&dp->file);
    }

    if (dp->flags & DPHASH_PAGED) {
        __pcache_destroy(dp);
        if (dp->mmap) {
            dph_os_file_truncate(&dp->file, dp->data_off);
            dph_os_file_munmap(dp->mmap, dp->mmap_len);
        }
        __paged_dir_destroy(&dp->pdir);
    } else {
        __table_destroy(&dp->tbl);
    }

    __arena_destroy(dp);
    dph_os_file_close(&dp->file);
    DPH_WUNLOCK(dp);
    if (!(dp->flags & DPHASH_LOCKLESS))
        dph_os_lock_destroy(&dp->lock);
    dph_os_free(dp);
}

int dphash_put(struct dphash *dp, const void *key, u32 klen,
               const void *val, u32 vlen)
{
    if (unlikely(!dp || !key || !val))
        return DPHASH_EINVAL;

    u64 h = __xxh64(key, klen);

    DPH_WLOCK(dp);

    if (dp->flags & DPHASH_PAGED) {
        struct paged_dir *d = &dp->pdir;

        u32 rec_sz = 8 + klen + vlen;
        size_t max_needed = (size_t)(dp->data_off + rec_sz + 16 * PAGE_SIZE);
        if (__mmap_ensure(dp, max_needed)) {
            DPH_WUNLOCK(dp);
            return DPHASH_EIO;
        }

        s64 off = dp->data_off;
        dp->data_off += rec_sz;

        u32 dir_idx = __dir_index(h, d->global_depth);

        if (d->page_off[dir_idx] == (u64)-1) {
            s64 page_off = __alloc_page(dp);
            if (page_off < 0) {
                DPH_WUNLOCK(dp);
                return DPHASH_EIO;
            }
            struct paged_page *p = __page_ptr(dp, (u64)page_off);
            if (p) {
                p->count = 0;
                p->local_depth = d->global_depth;
                p->overflow_next = 0;
            }
            d->page_off[dir_idx] = (u64)page_off;
        }

        u8 *dst = dp->mmap + off;
        __put_u32(dst, klen);
        __put_u32(dst + 4, vlen);
        dph_os_memcpy(dst + 8, key, klen);
        dph_os_memcpy(dst + 8 + klen, val, vlen);

        if (dp->flags & DPHASH_FSYNC)
            dph_os_file_sync(&dp->file);

    retry_page:
        dir_idx = __dir_index(h, d->global_depth);
        u64 cur_off = d->page_off[dir_idx];
        struct paged_page *page = __page_ptr(dp, cur_off);
        if (unlikely(!page))
            page = __page_load(dp, cur_off);
        if (unlikely(!page)) {
            DPH_WUNLOCK(dp);
            return DPHASH_EIO;
        }

        if (page->count >= PAGED_SLOTS_PER) {
            if (page->local_depth < MAX_LOCAL_DEPTH) {
                if (page->local_depth == d->global_depth) {
                    if (__paged_dir_double(d)) {
                        DPH_WUNLOCK(dp);
                        return DPHASH_EIO;
                    }
                }

                u8 old_depth = page->local_depth;
                u8 new_depth = old_depth + 1;

                s64 new_off_val = __alloc_page(dp);
                if (new_off_val < 0) {
                    DPH_WUNLOCK(dp);
                    return DPHASH_EIO;
                }

                page = __page_ptr(dp, cur_off);
                if (unlikely(!page))
                    page = __page_load(dp, cur_off);
                if (unlikely(!page)) {
                    DPH_WUNLOCK(dp);
                    return DPHASH_EIO;
                }

                struct paged_page *new_page = __page_ptr(dp, (u64)new_off_val);
                if (!new_page) {
                    DPH_WUNLOCK(dp);
                    return DPHASH_EIO;
                }
                new_page->count = 0;
                new_page->local_depth = new_depth;
                new_page->overflow_next = 0;

                u64 split_bit = (u64)1 << old_depth;

                u16 old_cnt = page->count;
                struct paged_slot tmp[PAGED_SLOTS_PER];
                dph_os_memcpy(tmp, page->slots, old_cnt * sizeof(struct paged_slot));
                page->count = 0;
                new_page->count = 0;

                for (u16 i = 0; i < old_cnt; i++) {
                    if (tmp[i].hash & split_bit) {
                        new_page->slots[new_page->count++] = tmp[i];
                    } else {
                        page->slots[page->count++] = tmp[i];
                    }
                }

                page->local_depth = new_depth;

                u32 bit_pos = old_depth;
                for (u32 i = 0; i < d->dir_size; i++) {
                    if (d->page_off[i] == cur_off) {
                        if ((i >> bit_pos) & 1)
                            d->page_off[i] = (u64)new_off_val;
                    }
                }

                if (!__page_ptr(dp, cur_off))
                    __pcache_put(dp, cur_off, page, 1);
                if (!__page_ptr(dp, (u64)new_off_val))
                    __pcache_put(dp, (u64)new_off_val, new_page, 1);

                goto retry_page;
            }

            while (page->count >= PAGED_SLOTS_PER) {
                if (page->overflow_next == 0) {
                    s64 ovfl_off = __alloc_page(dp);
                    if (ovfl_off < 0) {
                        DPH_WUNLOCK(dp);
                        return DPHASH_EIO;
                    }
                    struct paged_page *ovfl = __page_ptr(dp, (u64)ovfl_off);
                    if (ovfl) {
                        ovfl->count = 0;
                        ovfl->local_depth = page->local_depth;
                        ovfl->overflow_next = 0;
                    }
                    page = __page_ptr(dp, cur_off);
                    if (unlikely(!page))
                        page = __page_load(dp, cur_off);
                    if (unlikely(!page)) {
                        DPH_WUNLOCK(dp);
                        return DPHASH_EIO;
                    }
                    page->overflow_next = (u64)ovfl_off;
                }
                cur_off = page->overflow_next;
                page = __page_ptr(dp, cur_off);
                if (unlikely(!page))
                    page = __page_load(dp, cur_off);
                if (unlikely(!page)) {
                    DPH_WUNLOCK(dp);
                    return DPHASH_EIO;
                }
            }
        }

        page->slots[page->count].hash = h;
        page->slots[page->count].file_off = (u64)off;
        page->slots[page->count].klen = (u16)klen;
        page->slots[page->count].vlen = (u16)vlen;
        __set_key_prefix(page->slots[page->count].key_prefix, key, klen);
        page->count++;

        if (!__page_ptr(dp, d->page_off[__dir_index(h, d->global_depth)]))
            __pcache_put(dp, d->page_off[__dir_index(h, d->global_depth)], page, 1);

        d->count++;
        dp->count++;
        DPH_WUNLOCK(dp);
        return DPHASH_OK;
    }

    u32 kv_aligned = ((klen + 7) & ~(u32)7) + ((vlen + 7) & ~(u32)7);
    void *kv = __arena_alloc(dp, kv_aligned);
    if (unlikely(!kv)) {
        DPH_WUNLOCK(dp);
        return DPHASH_ENOMEM;
    }
    u32 klen_a = (klen + 7) & ~(u32)7;
    void *kcopy = kv;
    void *vcopy = (u8 *)kv + klen_a;
    dph_os_memcpy(kcopy, key, klen);
    dph_os_memcpy(vcopy, val, vlen);

    u64 file_off = 0;
    if (dp->flags & DPHASH_FSYNC) {
        s64 off = __write_record(dp, key, klen, val, vlen);
        if (unlikely(off < 0)) {
            DPH_WUNLOCK(dp);
            return DPHASH_EIO;
        }
        file_off = (u64)off;
    }

    struct dp_table *t = &dp->tbl;
    __table_ensure_space(t);
    int rc = __table_insert_new(t, h, kcopy, vcopy, klen, vlen, file_off);
    if (rc < 0) {
        DPH_WUNLOCK(dp);
        return DPHASH_ENOSPC;
    }

    dp->count++;
    DPH_WUNLOCK(dp);
    return DPHASH_OK;
}

int dphash_get(struct dphash *dp, const void *key, u32 klen,
               void *buf, u32 *vlen)
{
    if (unlikely(!dp || !key))
        return DPHASH_EINVAL;

    u64 h = __xxh64(key, klen);

    DPH_RLOCK(dp);

    if (dp->flags & DPHASH_PAGED) {
        struct paged_dir *d = &dp->pdir;
        u32 dir_idx = __dir_index(h, d->global_depth);
        if (d->page_off[dir_idx] == (u64)-1) {
            DPH_RUNLOCK(dp);
            return DPHASH_ENOENT;
        }

        u64 cur_off = d->page_off[dir_idx];
        while (cur_off != 0) {
            struct paged_page *page = __page_ptr(dp, cur_off);
            if (unlikely(!page))
                page = __page_load(dp, cur_off);
            if (unlikely(!page))
                break;

            for (u16 i = 0; i < page->count; i++) {
                struct paged_slot *s = &page->slots[i];
                if (s->hash != h || s->klen != klen)
                    continue;
                u32 pn = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
                if (dph_os_memcmp(s->key_prefix, key, pn) != 0)
                    continue;

                u32 rec_len = 8 + klen + s->vlen;
                if (dp->mmap && (size_t)(s->file_off + rec_len) <= dp->mmap_len) {
                    if (klen > KEY_PREFIX_LEN) {
                        if (dph_os_memcmp(key, dp->mmap + s->file_off + 8, klen) != 0)
                            continue;
                    }
                    if (s->vlen > *vlen) {
                        DPH_RUNLOCK(dp);
                        return DPHASH_ENOSPC;
                    }
                    dph_os_memcpy(buf, dp->mmap + s->file_off + 8 + klen, s->vlen);
                    *vlen = s->vlen;
                    DPH_RUNLOCK(dp);
                    return DPHASH_OK;
                }

                u8 stack_buf[256];
                u8 *rec = (rec_len <= sizeof(stack_buf)) ? stack_buf : dph_os_alloc(rec_len);
                if (!rec)
                    continue;
                if (dph_os_file_pread(&dp->file, rec, rec_len, (s64)s->file_off)) {
                    if (rec != stack_buf)
                        dph_os_free(rec);
                    continue;
                }
                if (klen > KEY_PREFIX_LEN) {
                    if (dph_os_memcmp(key, rec + 8, klen) != 0) {
                        if (rec != stack_buf)
                            dph_os_free(rec);
                        continue;
                    }
                }
                if (s->vlen > *vlen) {
                    if (rec != stack_buf)
                        dph_os_free(rec);
                    DPH_RUNLOCK(dp);
                    return DPHASH_ENOSPC;
                }
                dph_os_memcpy(buf, rec + 8 + klen, s->vlen);
                if (rec != stack_buf)
                    dph_os_free(rec);
                *vlen = s->vlen;
                DPH_RUNLOCK(dp);
                return DPHASH_OK;
            }
            cur_off = page->overflow_next;
        }
        DPH_RUNLOCK(dp);
        return DPHASH_ENOENT;
    }

    struct dp_table *t = &dp->tbl;
    s64 idx = __table_find(t, h, key, klen);

    if (idx >= 0) {
        struct dp_slot *sl;
        if ((u64)idx & (1ULL << 63)) {
            sl = &t->old_slots[(u64)idx & ~(1ULL << 63)];
        } else {
            sl = &t->slots[(u64)idx];
        }
        if (sl->vlen > *vlen) {
            DPH_RUNLOCK(dp);
            return DPHASH_ENOSPC;
        }
        dph_os_memcpy(buf, sl->val, sl->vlen);
        *vlen = sl->vlen;
        DPH_RUNLOCK(dp);
        return DPHASH_OK;
    }

    DPH_RUNLOCK(dp);
    return DPHASH_ENOENT;
}

int dphash_get_disk(struct dphash *dp, const void *key, u32 klen,
                    void *buf, u32 *vlen)
{
    if (unlikely(!dp || !key))
        return DPHASH_EINVAL;

    u64 h = __xxh64(key, klen);

    DPH_RLOCK(dp);

    if (dp->flags & DPHASH_PAGED) {
        struct paged_dir *d = &dp->pdir;
        u32 dir_idx = __dir_index(h, d->global_depth);
        if (d->page_off[dir_idx] == (u64)-1) {
            DPH_RUNLOCK(dp);
            return DPHASH_ENOENT;
        }

        u64 cur_off = d->page_off[dir_idx];
        while (cur_off != 0) {
            struct paged_page *page = __page_ptr(dp, cur_off);
            if (unlikely(!page))
                page = __page_load(dp, cur_off);
            if (unlikely(!page))
                break;

            for (u16 i = 0; i < page->count; i++) {
                struct paged_slot *s = &page->slots[i];
                if (s->hash != h || s->klen != klen)
                    continue;
                u32 pn = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
                if (dph_os_memcmp(s->key_prefix, key, pn) != 0)
                    continue;

                u32 rec_len = 8 + klen + s->vlen;
                u8 stack_buf[256];
                u8 *rec = (rec_len <= sizeof(stack_buf)) ? stack_buf : dph_os_alloc(rec_len);
                if (!rec)
                    continue;
                if (dph_os_file_pread(&dp->file, rec, rec_len, (s64)s->file_off)) {
                    if (rec != stack_buf)
                        dph_os_free(rec);
                    continue;
                }
                if (klen > KEY_PREFIX_LEN) {
                    if (dph_os_memcmp(key, rec + 8, klen) != 0) {
                        if (rec != stack_buf)
                            dph_os_free(rec);
                        continue;
                    }
                }
                if (s->vlen > *vlen) {
                    if (rec != stack_buf)
                        dph_os_free(rec);
                    DPH_RUNLOCK(dp);
                    return DPHASH_ENOSPC;
                }
                dph_os_memcpy(buf, rec + 8 + klen, s->vlen);
                if (rec != stack_buf)
                    dph_os_free(rec);
                *vlen = s->vlen;
                DPH_RUNLOCK(dp);
                return DPHASH_OK;
            }
            cur_off = page->overflow_next;
        }
        DPH_RUNLOCK(dp);
        return DPHASH_ENOENT;
    }

    struct dp_table *t = &dp->tbl;
    s64 idx = __table_find(t, h, key, klen);

    if (idx >= 0) {
        struct dp_slot *sl;
        if ((u64)idx & (1ULL << 63)) {
            sl = &t->old_slots[(u64)idx & ~(1ULL << 63)];
        } else {
            sl = &t->slots[(u64)idx];
        }
        if (sl->file_off == 0) {
            DPH_RUNLOCK(dp);
            return DPHASH_ENOENT;
        }
        u32 rec_len = 8 + sl->klen + sl->vlen;
        u8 stack_buf[256];
        u8 *rec = (rec_len <= sizeof(stack_buf)) ? stack_buf : dph_os_alloc(rec_len);
        if (!rec) {
            DPH_RUNLOCK(dp);
            return DPHASH_ENOMEM;
        }
        if (dph_os_file_pread(&dp->file, rec, rec_len, (s64)sl->file_off)) {
            if (rec != stack_buf)
                dph_os_free(rec);
            DPH_RUNLOCK(dp);
            return DPHASH_EIO;
        }
        u32 vl;
        dph_os_memcpy(&vl, rec + 4, 4);
        if (vl > *vlen) {
            if (rec != stack_buf)
                dph_os_free(rec);
            DPH_RUNLOCK(dp);
            return DPHASH_ENOSPC;
        }
        dph_os_memcpy(buf, rec + 8 + sl->klen, vl);
        if (rec != stack_buf)
            dph_os_free(rec);
        *vlen = vl;
        DPH_RUNLOCK(dp);
        return DPHASH_OK;
    }

    DPH_RUNLOCK(dp);
    return DPHASH_ENOENT;
}

int dphash_del(struct dphash *dp, const void *key, u32 klen)
{
    if (unlikely(!dp || !key))
        return DPHASH_EINVAL;

    u64 h = __xxh64(key, klen);

    DPH_WLOCK(dp);

    if (dp->flags & DPHASH_PAGED) {
        struct paged_dir *d = &dp->pdir;
        u32 dir_idx = __dir_index(h, d->global_depth);
        if (d->page_off[dir_idx] == (u64)-1) {
            DPH_WUNLOCK(dp);
            return DPHASH_ENOENT;
        }

        u64 cur_off = d->page_off[dir_idx];
        while (cur_off != 0) {
            struct paged_page *page = __page_ptr(dp, cur_off);
            if (unlikely(!page))
                page = __page_load(dp, cur_off);
            if (unlikely(!page))
                break;

            for (u16 i = 0; i < page->count; i++) {
                struct paged_slot *s = &page->slots[i];
                if (s->hash != h || s->klen != klen)
                    continue;
                u32 pn = klen < KEY_PREFIX_LEN ? klen : KEY_PREFIX_LEN;
                if (dph_os_memcmp(s->key_prefix, key, pn) != 0)
                    continue;

                bool full_match = (klen <= KEY_PREFIX_LEN);
                if (!full_match) {
                    u8 kbuf[256];
                    u8 *kb = (klen <= sizeof(kbuf)) ? kbuf : dph_os_alloc(klen);
                    if (!kb)
                        continue;
                    bool ok = false;
                    if (dp->mmap &&
                        (size_t)(s->file_off + 8 + klen) <= dp->mmap_len) {
                        dph_os_memcpy(kb, dp->mmap + s->file_off + 8, klen);
                        ok = (dph_os_memcmp(kb, key, klen) == 0);
                    } else {
                        if (!dph_os_file_pread(&dp->file, kb, klen,
                                               s->file_off + 8))
                            ok = (dph_os_memcmp(kb, key, klen) == 0);
                    }
                    if (kb != kbuf)
                        dph_os_free(kb);
                    if (!ok)
                        continue;
                }

                page->slots[i] = page->slots[page->count - 1];
                page->count--;
                if (!__page_ptr(dp, cur_off))
                    __pcache_put(dp, cur_off, page, 1);
                d->count--;
                dp->count--;
                DPH_WUNLOCK(dp);
                return DPHASH_OK;
            }
            cur_off = page->overflow_next;
        }
        DPH_WUNLOCK(dp);
        return DPHASH_ENOENT;
    }

    struct dp_table *t = &dp->tbl;
    s64 idx = __table_find(t, h, key, klen);

    if (idx < 0) {
        DPH_WUNLOCK(dp);
        return DPHASH_ENOENT;
    }

    if ((u64)idx & (1ULL << 63)) {
        u64 old_idx = (u64)idx & ~(1ULL << 63);
        t->old_ctrl[old_idx] = CTRL_DELETED;
        dph_os_memset(&t->old_slots[old_idx], 0, sizeof(struct dp_slot));
    } else {
        t->ctrl[(u64)idx] = CTRL_DELETED;
        dph_os_memset(&t->slots[(u64)idx], 0, sizeof(struct dp_slot));
        t->tombstones++;
    }
    t->count--;
    dp->count--;
    DPH_WUNLOCK(dp);
    return DPHASH_OK;
}

u64 dphash_count(const struct dphash *dp)
{
    return dp ? dp->count : 0;
}

int dphash_sync(struct dphash *dp)
{
    if (!dp)
        return DPHASH_EINVAL;
    DPH_WLOCK(dp);

    if (dp->flags & DPHASH_PAGED) {
        __pcache_flush(dp);
    } else if (!(dp->flags & DPHASH_FSYNC)) {
        dph_os_file_truncate(&dp->file, 0);
        dp->data_off = 0;
        struct dp_table *t = &dp->tbl;
        for (u64 i = 0; i < t->cap; i++) {
            if (t->ctrl[i] != CTRL_EMPTY && t->ctrl[i] != CTRL_DELETED) {
                struct dp_slot *s = &t->slots[i];
                if (s->val && s->vlen > 0)
                    s->file_off = __write_record(dp, s->key,
                                                 s->klen, s->val, s->vlen);
            }
        }
    }

    dph_os_file_sync(&dp->file);
    DPH_WUNLOCK(dp);
    return DPHASH_OK;
}

size_t dphash_mem_usage(const struct dphash *dp)
{
    if (!dp)
        return 0;
    size_t sz = sizeof(*dp);

    if (dp->flags & DPHASH_PAGED) {
        sz += dp->pdir.dir_size * sizeof(u64);
        sz += dp->pcache.count * sizeof(struct paged_page);
    } else {
        sz += dp->tbl.cap;
        sz += dp->tbl.cap * sizeof(struct dp_slot);
        if (dp->tbl.rehashing) {
            sz += dp->tbl.old_cap;
            sz += dp->tbl.old_cap * sizeof(struct dp_slot);
        }
    }

    sz += __arena_total(dp);
    return sz;
}
