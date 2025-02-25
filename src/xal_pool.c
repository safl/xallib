#define _GNU_SOURCE
#include <errno.h>
#include <libxal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <xal_pool.h>

int
xal_pool_unmap(struct xal_pool *pool)
{
	return munmap(pool, pool->reserved);
}

int
xal_pool_grow(struct xal_pool *pool, size_t growby)
{
	size_t growby_nbytes = growby * sizeof(*pool->inodes);
	size_t allocated_nbytes = growby_nbytes + pool->allocated * sizeof(*pool->inodes);

	if (mprotect(pool, allocated_nbytes, PROT_READ | PROT_WRITE)) {
		return -errno;
	}
	memset(&pool->inodes[pool->free], 0, growby_nbytes);

	pool->allocated += growby;

	return 0;
}

int
xal_pool_map(struct xal_pool *pool, size_t reserved, size_t allocated)
{
	struct xal_inode *cand;

	if (pool->reserved) {
		return -EINVAL;
	}
	if (allocated > reserved) {
		return -EINVAL;
	}

	pool->reserved = reserved;
	pool->allocated = 0;
	pool->growby = allocated;
	pool->free = 0;

	cand = mmap(NULL, reserved * sizeof(*cand), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (MAP_FAILED == cand) {
		return -errno;
	}
	xal_pool_grow(pool, allocated);

	pool->inodes = cand;

	return 0;
}

int
xal_pool_claim(struct xal_pool *pool, size_t count, struct xal_inode **inode)
{
	int err;

	if (count > pool->growby) {
		return -EINVAL;
	}

	err = xal_pool_grow(pool, pool->growby);
	if (err) {
		return err;
	}

	*inode = &pool->inodes[pool->free++];

	return 0;
}