#include "bptree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

#define BT_ORDER 64
#define BT_MAX   (2 * BT_ORDER - 1)

struct bt_entry {
	u64 hash;
	void *key;
	u32 klen;
	void *val;
	u32 vlen;
	s64 file_off;
	u8  deleted;
};

struct bt_node {
	int n;
	int leaf;
	struct bt_entry e[BT_MAX];
	struct bt_node *c[BT_MAX + 1];
};

struct bt_arena {
	struct bt_arena *next;
	u32 used;
	u32 cap;
	u8 data[];
};

struct bptree {
	struct bt_node *root;
	struct bt_arena *arena_head;
	struct bt_arena *arena_cur;
	u64 count;
	int data_fd;
	s64 data_off;
	u64 flags;
	pthread_rwlock_t rwlock;
};

#define BT_RLOCK(bt)   do { if (!((bt)->flags & DPHASH_LOCKLESS)) pthread_rwlock_rdlock(&(bt)->rwlock); } while(0)
#define BT_RUNLOCK(bt) do { if (!((bt)->flags & DPHASH_LOCKLESS)) pthread_rwlock_unlock(&(bt)->rwlock); } while(0)
#define BT_WLOCK(bt)   do { if (!((bt)->flags & DPHASH_LOCKLESS)) pthread_rwlock_wrlock(&(bt)->rwlock); } while(0)
#define BT_WUNLOCK(bt) do { if (!((bt)->flags & DPHASH_LOCKLESS)) pthread_rwlock_unlock(&(bt)->rwlock); } while(0)

static always_inline u64 __rotl64_bt(u64 x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static always_inline u32 __get_u32_bt(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) |
	       ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static always_inline u64 __get_u64_bt(const u8 *p)
{
	return (u64)__get_u32_bt(p) | ((u64)__get_u32_bt(p + 4) << 32);
}

static always_inline u64 __xxh3_bt(const void *data, u32 len)
{
	const u8 *p = (const u8 *)data;
	const u8 * const end = p + len;
	u64 h;

	if (likely(len >= 16)) {
		const u8 *limit = end - 16;
		u64 v1 = __get_u64_bt(p) * 0xc2b2ae3cc27b9a21ULL; p += 8;
		u64 v2 = __get_u64_bt(p) * 0xc2b2ae3cc27b9a21ULL; p += 8;
		do {
			v1 += __get_u64_bt(p) * 0xc2b2ae3cc27b9a21ULL;
			v1 = __rotl64_bt(v1, 31) * 0x9e3779b97f4a7c13ULL;
			v2 += __get_u64_bt(p + 8) * 0xc2b2ae3cc27b9a21ULL;
			v2 = __rotl64_bt(v2, 31) * 0x9e3779b97f4a7c13ULL;
			p += 16;
		} while (likely(p <= limit));
		h = __rotl64_bt(v1, 37) * 0x85ebca6b + __rotl64_bt(v2, 37) * 0xc2b2ae3c;
		h += len;
	} else if (len >= 8) {
		u64 v = __get_u64_bt(p) * 0xc2b2ae3cc27b9a21ULL;
		v = __rotl64_bt(v, 31) * 0x9e3779b97f4a7c13ULL;
		h = v ^ (__get_u64_bt(end - 8) * 0xc2b2ae3cc27b9a21ULL);
		h = __rotl64_bt(h, 49) * 0x9e3779b97f4a7c13ULL;
		h += len;
	} else if (len >= 4) {
		h = (u64)__get_u32_bt(p) * 0x9e3779b97f4a7c13ULL;
		h = __rotl64_bt(h, 17) * 0x9e3779b97f4a7c13ULL;
		h += (u64)__get_u32_bt(end - 4);
		h += len;
	} else if (len > 0) {
		u8 buf[4] = {0};
		memcpy(buf, p, len);
		h = (u64)__get_u32_bt(buf) * 0x9e3779b97f4a7c13ULL;
		h += len;
	} else {
		return 0x9e3779b97f4a7c13ULL;
	}
	h ^= h >> 33; h *= 0x62a9d9ed799705f5ULL;
	h ^= h >> 29; h *= 0x3244f6a7e7e6a1c7ULL;
	h ^= h >> 32;
	return h < 2 ? h + 2 : h;
}

static void *__arena_alloc_bt(struct bptree *bt, u32 size)
{
	size = (size + 7) & ~(u32)7;
	if (unlikely(!bt->arena_cur ||
		     bt->arena_cur->used + size > bt->arena_cur->cap)) {
		u32 cap = size > (64 << 10) ? size : (64 << 10);
		struct bt_arena *blk = malloc(sizeof(*blk) + cap);
		if (unlikely(!blk)) return NULL;
		blk->next = NULL;
		blk->used = 0;
		blk->cap = cap;
		if (bt->arena_cur)
			bt->arena_cur->next = blk;
		else
			bt->arena_head = blk;
		bt->arena_cur = blk;
	}
	void *ptr = bt->arena_cur->data + bt->arena_cur->used;
	bt->arena_cur->used += size;
	return ptr;
}

static struct bt_node *__node_new(int leaf)
{
	struct bt_node *n = calloc(1, sizeof(*n));
	if (n) n->leaf = leaf;
	return n;
}

static void __node_free(struct bt_node *n)
{
	if (!n) return;
	if (!n->leaf) {
		for (int i = 0; i <= n->n; i++)
			__node_free(n->c[i]);
	}
	free(n);
}

static int __entry_cmp(const struct bt_entry *a, u64 hash,
		       const void *key, u32 klen)
{
	if (a->hash < hash) return -1;
	if (a->hash > hash) return 1;
	if (a->klen < klen) return -1;
	if (a->klen > klen) return 1;
	return memcmp(a->key, key, klen);
}

static int __node_find(const struct bt_node *n, u64 hash,
		       const void *key, u32 klen)
{
	int lo = 0, hi = n->n - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		int c = __entry_cmp(&n->e[mid], hash, key, klen);
		if (c == 0) return mid;
		if (c < 0) lo = mid + 1;
		else hi = mid - 1;
	}
	return -(lo + 1);
}

static void __node_split_child(struct bt_node *x, int i)
{
	struct bt_node *y = x->c[i];
	struct bt_node *z = __node_new(y->leaf);
	z->n = BT_ORDER - 1;

	for (int j = 0; j < BT_ORDER - 1; j++)
		z->e[j] = y->e[j + BT_ORDER];

	if (!y->leaf)
		for (int j = 0; j < BT_ORDER; j++)
			z->c[j] = y->c[j + BT_ORDER];

	y->n = BT_ORDER - 1;

	for (int j = x->n; j >= i + 1; j--)
		x->c[j + 1] = x->c[j];
	x->c[i + 1] = z;

	for (int j = x->n - 1; j >= i; j--)
		x->e[j + 1] = x->e[j];
	x->e[i] = y->e[BT_ORDER - 1];
	x->n++;
}

static void __insert_nonfull(struct bt_node *n, u64 hash,
			     void *key, u32 klen, void *val, u32 vlen,
			     s64 file_off)
{
	int i = n->n - 1;

	if (n->leaf) {
		while (i >= 0 && __entry_cmp(&n->e[i], hash, key, klen) > 0) {
			n->e[i + 1] = n->e[i];
			i--;
		}
		n->e[i + 1].hash = hash;
		n->e[i + 1].key = key;
		n->e[i + 1].klen = klen;
		n->e[i + 1].val = val;
		n->e[i + 1].vlen = vlen;
		n->e[i + 1].file_off = file_off;
		n->e[i + 1].deleted = 0;
		n->n++;
	} else {
		while (i >= 0 && __entry_cmp(&n->e[i], hash, key, klen) > 0)
			i--;
		i++;

		if (n->c[i]->n == BT_MAX) {
			__node_split_child(n, i);
			if (__entry_cmp(&n->e[i], hash, key, klen) < 0)
				i++;
		}
		__insert_nonfull(n->c[i], hash, key, klen, val, vlen, file_off);
	}
}

static int __pwrite_bt(int fd, const void *buf, size_t len, off_t off)
{
	const u8 *p = buf;
	while (len > 0) {
		ssize_t n = pwrite(fd, p, len, off);
		if (n <= 0) return -1;
		p += n; off += n; len -= n;
	}
	return 0;
}

static int __pread_bt(int fd, void *buf, size_t len, off_t off)
{
	u8 *p = buf;
	while (len > 0) {
		ssize_t n = pread(fd, p, len, off);
		if (n <= 0) return -1;
		p += n; off += n; len -= n;
	}
	return 0;
}

struct bptree *bptree_open(const char *path, u64 flags)
{
	struct bptree *bt = calloc(1, sizeof(*bt));
	if (!bt) return NULL;

	bt->data_fd = -1;
	bt->flags = flags;
	pthread_rwlock_init(&bt->rwlock, NULL);

	if (mkdir(path, 0755) && errno != EEXIST)
		goto fail;

	char buf[1024];
	snprintf(buf, sizeof(buf), "%s/bptree.dat", path);
	bt->data_fd = open(buf, O_CREAT | O_RDWR, 0644);
	if (bt->data_fd < 0) goto fail;

	bt->root = __node_new(1);
	if (!bt->root) goto fail;

	if (!(flags & DPHASH_LOCKLESS))
		pthread_rwlock_init(&bt->rwlock, NULL);

	return bt;

fail:
	bptree_close(bt);
	return NULL;
}

void bptree_close(struct bptree *bt)
{
	if (!bt) return;
	BT_WLOCK(bt);
	__node_free(bt->root);

	struct bt_arena *a = bt->arena_head;
	while (a) { struct bt_arena *next = a->next; free(a); a = next; }

	if (bt->data_fd >= 0) close(bt->data_fd);
	BT_WUNLOCK(bt);
	if (!(bt->flags & DPHASH_LOCKLESS))
		pthread_rwlock_destroy(&bt->rwlock);
	free(bt);
}

int bptree_put(struct bptree *bt, const void *key, u32 klen,
	       const void *val, u32 vlen)
{
	if (!bt || !key || !val) return DPHASH_EINVAL;

	u64 h = __xxh3_bt(key, klen);

	void *kcopy = __arena_alloc_bt(bt, klen);
	if (!kcopy) return DPHASH_ENOMEM;
	memcpy(kcopy, key, klen);

	void *vcopy = __arena_alloc_bt(bt, vlen);
	if (!vcopy) return DPHASH_ENOMEM;
	memcpy(vcopy, val, vlen);

	s64 off = -1;
	if (bt->flags & DPHASH_FSYNC) {
		u32 total = 8 + klen + vlen;
		u8 *rec = malloc(total);
		if (rec) {
			u32 kl = klen, vl = vlen;
			memcpy(rec, &kl, 4);
			memcpy(rec + 4, &vl, 4);
			memcpy(rec + 8, key, klen);
			memcpy(rec + 8 + klen, val, vlen);
			off = bt->data_off;
			__pwrite_bt(bt->data_fd, rec, total, off);
			bt->data_off += total;
			fsync(bt->data_fd);
			free(rec);
		}
	}

	BT_WLOCK(bt);

	if (bt->root->n == BT_MAX) {
		struct bt_node *s = __node_new(0);
		s->c[0] = bt->root;
		__node_split_child(s, 0);
		bt->root = s;
	}

	__insert_nonfull(bt->root, h, kcopy, klen, vcopy, vlen, off);
	bt->count++;

	BT_WUNLOCK(bt);
	return DPHASH_OK;
}

static const struct bt_entry *__search(const struct bt_node *n,
				       u64 hash, const void *key, u32 klen)
{
	while (n) {
		int i = __node_find(n, hash, key, klen);
		if (i >= 0) {
			if (!n->e[i].deleted)
				return &n->e[i];
			return NULL;
		}
		if (n->leaf) return NULL;
		i = -(i + 1);
		n = n->c[i];
	}
	return NULL;
}

int bptree_get(struct bptree *bt, const void *key, u32 klen,
	       const void **val, u32 *vlen)
{
	if (!bt || !key) return DPHASH_EINVAL;

	u64 h = __xxh3_bt(key, klen);
	BT_RLOCK(bt);
	const struct bt_entry *e = __search(bt->root, h, key, klen);
	if (e && e->val) {
		*val = e->val;
		*vlen = e->vlen;
		BT_RUNLOCK(bt);
		return DPHASH_OK;
	}
	BT_RUNLOCK(bt);
	return DPHASH_ENOENT;
}

int bptree_get_disk(struct bptree *bt, const void *key, u32 klen,
		    void *buf, u32 *vlen)
{
	if (!bt || !key) return DPHASH_EINVAL;

	u64 h = __xxh3_bt(key, klen);
	BT_RLOCK(bt);
	const struct bt_entry *e = __search(bt->root, h, key, klen);
	if (!e || e->deleted || e->file_off < 0) {
		BT_RUNLOCK(bt);
		return DPHASH_ENOENT;
	}

	u8 hdr[8];
	if (__pread_bt(bt->data_fd, hdr, 8, e->file_off)) {
		BT_RUNLOCK(bt);
		return DPHASH_EIO;
	}
	u32 vl;
	memcpy(&vl, hdr + 4, 4);
	if (vl > *vlen) {
		BT_RUNLOCK(bt);
		return DPHASH_ENOSPC;
	}
	if (__pread_bt(bt->data_fd, buf, vl, e->file_off + 8 + e->klen)) {
		BT_RUNLOCK(bt);
		return DPHASH_EIO;
	}
	*vlen = vl;
	BT_RUNLOCK(bt);
	return DPHASH_OK;
}

int bptree_del(struct bptree *bt, const void *key, u32 klen)
{
	if (!bt || !key) return DPHASH_EINVAL;

	u64 h = __xxh3_bt(key, klen);
	BT_WLOCK(bt);

	struct bt_node *n = bt->root;
	while (n) {
		int i = __node_find(n, h, key, klen);
		if (i >= 0) {
			n->e[i].deleted = 1;
			bt->count--;
			BT_WUNLOCK(bt);
			return DPHASH_OK;
		}
		if (n->leaf) break;
		i = -(i + 1);
		n = n->c[i];
	}

	BT_WUNLOCK(bt);
	return DPHASH_ENOENT;
}

u64 bptree_count(const struct bptree *bt)
{
	return bt ? bt->count : 0;
}
