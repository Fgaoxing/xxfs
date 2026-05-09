#ifndef _LSMTREE_H
#define _LSMTREE_H

#include "dphash.h"

struct lsmtree;

struct lsmtree *lsmtree_open(const char *path, u64 flags);
void            lsmtree_close(struct lsmtree *lm);

int  lsmtree_put(struct lsmtree *lm, const void *key, u32 klen,
                 const void *val, u32 vlen);
int  lsmtree_get(struct lsmtree *lm, const void *key, u32 klen,
                 const void **val, u32 *vlen);
int  lsmtree_del(struct lsmtree *lm, const void *key, u32 klen);

u64  lsmtree_count(const struct lsmtree *lm);

int  lsmtree_get_disk(struct lsmtree *lm, const void *key, u32 klen,
                      void *buf, u32 *vlen);

#endif
