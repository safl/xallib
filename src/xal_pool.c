#define _GNU_SOURCE
#include <errno.h>
#include <libxal.h>
#include <stdio.h>
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
	size_t growby_nbytes = growby * pool->element_size;
	size_t allocated_nbytes = growby_nbytes + pool->allocated * pool->element_size;
	uint8_t *cursor = pool->memory;

	if (mprotect(pool->memory, allocated_nbytes, PROT_READ | PROT_WRITE)) {
		XAL_DEBUG("FAILED: mprotect(...); errno(%d)", errno);
		return -errno;
	}
	memset(&cursor[pool->free * pool->element_size], 0, growby_nbytes);

	pool->allocated += growby;

	return 0;
}

int
xal_pool_map(struct xal_pool *pool, size_t reserved, size_t allocated, size_t element_size)
{
	int err;

	if (pool->reserved) {
		XAL_DEBUG("FAILED: xal_pool_map(...); errno(%d)", EINVAL);
		return -EINVAL;
	}
	if (allocated > reserved) {
		XAL_DEBUG("FAILED: xal_pool_map(...); errno(%d)", EINVAL);
		return -EINVAL;
	}

	pool->reserved = reserved;
	pool->allocated = 0;
	pool->growby = allocated;
	pool->element_size = element_size;
	pool->free = 0;

	pool->memory =
	    mmap(NULL, reserved * element_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (MAP_FAILED == pool->memory) {
		XAL_DEBUG("FAILED: mmap(...); errno(%d)", errno);
		return -errno;
	}

	err = xal_pool_grow(pool, allocated);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_grow(...); err(%d)", err);
		xal_pool_unmap(pool);
		return err;
	}

	return 0;
}

int
xal_pool_claim_inodes(struct xal_pool *pool, size_t count, struct xal_inode **inode)
{
	uint8_t *cursor = pool->memory;
	int err;

	if (count > pool->growby) {
		XAL_DEBUG("FAILED: count > pool->growby");
		return -EINVAL;
	}

	if (pool->allocated <= (pool->free + count)) {
		err = xal_pool_grow(pool, pool->growby);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_grow(); err(%d)", err);
			return err;
		}
	}

	if (inode) {
		*inode = (void *)&cursor[pool->free * pool->element_size];
	}
	pool->free += count;

	return 0;
}

int
xal_pool_claim_extents(struct xal_pool *pool, size_t count, struct xal_extent **extents)
{
	uint8_t *cursor = pool->memory;
	int err;

	if (count > pool->growby) {
		XAL_DEBUG("FAILED: count > pool->growby");
		return -EINVAL;
	}

	if (pool->allocated <= (pool->free + count)) {
		err = xal_pool_grow(pool, pool->growby);
		if (err) {
			XAL_DEBUG("xal_pool_grow(); err(%d)", err);
			return err;
		}
	}

	if (extents) {
		*extents = (void *)&cursor[pool->free * pool->element_size];
	}
	pool->free += count;

	return 0;
}
