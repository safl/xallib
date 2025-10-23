
/**
 * A pool of mmap backed memory for fixed-size elements.
 *
 * This is utilized for inodes and extents. The useful feature is that we can have a contigous
 * virtual address space which can grow without having to move elements nor change pointers to
 * them, as one would otherwise have to do with malloc()/realloc().
 */
struct xal_pool {
	size_t reserved;     ///< Maximum number of elements in the pool
	size_t allocated;    ///< Number of reserved elements that are allocated
	size_t growby;	     ///< Number of reserved elements to allocate at a time
	size_t free;	     ///< Index / position of the next free element
	size_t element_size; ///< Size of a single element in bytes
	void *memory;	     ///< Memory space for elements
};

int
xal_pool_unmap(struct xal_pool *pool);

/**
 * Initialize the given pool of 'struct xal_inode'
 *
 * This will produce a pool of 'reserved' number of inodes, that is, overcommitted memory which is
 * not usable. A subset of this memory, specifically memory for an 'allocated' amount of inodes is
 * made available for read / write, and written to by "zeroing" out the memory.
 *
 * See the xal_pool_claim() helper, which provides arrays of allocated memory usable for
 * inode-storage. The number of allocated inodes are grown, when claimed, until the reserved space
 * is exhausted.
 */
int
xal_pool_map(struct xal_pool *pool, size_t reserved, size_t allocated, size_t element_size);

/**
 * Claims the current inode-offset
 *
 * This is utilized when decoding device-entries and the amount is not known ahead of time,
 * thus, the current position is written to 'inode', subsequent calls to
 */
void
xal_pool_current_inode(struct xal_pool *pool, struct xal_inode **inode);

void
xal_pool_current_extent(struct xal_pool *pool, struct xal_extent **extent);

/**
 *
 */
int
xal_pool_claim_extents(struct xal_pool *pool, size_t count, struct xal_extent **extents);

int
xal_pool_claim_inodes(struct xal_pool *pool, size_t count, struct xal_inode **inodes);

int
xal_pool_clear(struct xal_pool *pool);
