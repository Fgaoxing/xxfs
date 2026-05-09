#ifndef _BPTREE_H
#define _BPTREE_H

#include "dphash.h"

struct bptree;

struct bptree *bptree_open(const char *path, u64 flags);
void           bptree_close(struct bptree *bt);

int  bptree_put(struct bptree *bt, const void *key, u32 klen,
                const void *val, u32 vlen);
int  bptree_get(struct bptree *bt, const void *key, u32 klen,
                const void **val, u32 *vlen);
int  bptree_del(struct bptree *bt, const void *key, u32 klen);

u64  bptree_count(const struct bptree *bt);

int  bptree_get_disk(struct bptree *bt, const void *key, u32 klen,
                     void *buf, u32 *vlen);

#endif
