#define _DPHASH_OS_IMPL
#include "dphash.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>

void *dph_os_alloc(size_t size)
{
	return malloc(size);
}

void *dph_os_zalloc(size_t size)
{
	return calloc(1, size);
}

void dph_os_free(void *ptr)
{
	free(ptr);
}

void dph_os_lock_init(struct dphash_lock *l)
{
	pthread_rwlock_t *rw = malloc(sizeof(pthread_rwlock_t));
	if (rw) {
		pthread_rwlock_init(rw, NULL);
		l->opaque[0] = rw;
	}
}

void dph_os_lock_destroy(struct dphash_lock *l)
{
	pthread_rwlock_t *rw = l->opaque[0];
	if (rw) {
		pthread_rwlock_destroy(rw);
		free(rw);
		l->opaque[0] = NULL;
	}
}

void dph_os_read_lock(struct dphash_lock *l)
{
	pthread_rwlock_rdlock((pthread_rwlock_t *)l->opaque[0]);
}

void dph_os_read_unlock(struct dphash_lock *l)
{
	pthread_rwlock_unlock((pthread_rwlock_t *)l->opaque[0]);
}

void dph_os_write_lock(struct dphash_lock *l)
{
	pthread_rwlock_wrlock((pthread_rwlock_t *)l->opaque[0]);
}

void dph_os_write_unlock(struct dphash_lock *l)
{
	pthread_rwlock_unlock((pthread_rwlock_t *)l->opaque[0]);
}

int dph_os_file_open(struct dphash_file *f, const char *path)
{
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s/data.dat", path);
	int fd = open(buf, O_CREAT | O_RDWR, 0644);
	if (fd < 0) return -1;
	int *pfd = malloc(sizeof(int));
	if (!pfd) { close(fd); return -1; }
	*pfd = fd;
	f->opaque[0] = pfd;
	return 0;
}

void dph_os_file_close(struct dphash_file *f)
{
	int *pfd = f->opaque[0];
	if (pfd) {
		close(*pfd);
		free(pfd);
		f->opaque[0] = NULL;
	}
}

s64 dph_os_file_size(struct dphash_file *f)
{
	int *pfd = f->opaque[0];
	if (!pfd) return -1;
	struct stat st;
	if (fstat(*pfd, &st)) return -1;
	return st.st_size;
}

int dph_os_file_pwrite(struct dphash_file *f, const void *buf,
		       size_t len, s64 off)
{
	int *pfd = f->opaque[0];
	if (!pfd) return -1;
	const u8 *p = buf;
	while (len > 0) {
		ssize_t n = pwrite(*pfd, p, len, off);
		if (n <= 0) return -1;
		p += n; off += n; len -= n;
	}
	return 0;
}

int dph_os_file_pread(struct dphash_file *f, void *buf,
		      size_t len, s64 off)
{
	int *pfd = f->opaque[0];
	if (!pfd) return -1;
	u8 *p = buf;
	while (len > 0) {
		ssize_t n = pread(*pfd, p, len, off);
		if (n <= 0) return -1;
		p += n; off += n; len -= n;
	}
	return 0;
}

int dph_os_file_sync(struct dphash_file *f)
{
	int *pfd = f->opaque[0];
	if (!pfd) return -1;
	return fsync(*pfd);
}

int dph_os_file_truncate(struct dphash_file *f, s64 size)
{
	int *pfd = f->opaque[0];
	if (!pfd) return -1;
	return ftruncate(*pfd, size);
}

int dph_os_file_extend(struct dphash_file *f, s64 size)
{
	int *pfd = f->opaque[0];
	if (!pfd) return -1;
	return ftruncate(*pfd, size);
}

void *dph_os_file_mmap(struct dphash_file *f, size_t len)
{
	int *pfd = f->opaque[0];
	if (!pfd) return NULL;
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, *pfd, 0);
	if (p == MAP_FAILED) return NULL;
	return p;
}

void dph_os_file_munmap(void *ptr, size_t len)
{
	if (ptr) munmap(ptr, len);
}

int dph_os_file_msync(void *ptr, size_t len)
{
	if (!ptr) return -1;
	return msync(ptr, len, MS_SYNC);
}

int dph_os_mkdir(const char *path)
{
	if (mkdir(path, 0755) && errno != EEXIST)
		return -1;
	return 0;
}

void *dph_os_memcpy(void *dst, const void *src, size_t n)
{
	return memcpy(dst, src, n);
}

int dph_os_memcmp(const void *a, const void *b, size_t n)
{
	return memcmp(a, b, n);
}

void *dph_os_memset(void *dst, int c, size_t n)
{
	return memset(dst, c, n);
}
