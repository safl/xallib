/**
 * Public API for the XAL library
 *
 * On-disk Storage Format
 * ======================
 *
 * The on-disk storage format is not exposed in these headers since the intent is that the user
 * should not need to rely on them. Rather, they are handled by the library-implementation and
 * values communicated via pretty-printers and transformed into "simplified" representations.
 *
 * Endianness
 * ----------
 *
 * The XFS on-disk format make use of integer values in big-endian format, thus value convertion
 * to little-endian is needed on e.g. x86 systems. Such helpers are not exposed here, rather the
 * conversions are performed by the library and the representations found here are in "native"
 * format.
 *
 * Files and Directories
 * ---------------------
 *
 * XFS has 4 or 5 ways of storing directory indexes (filename + inode number) optimized by
 * use-case. When XAL parses these from the on-disk-format, then they normalized into the
 * form represented by 'struct xal_inode'. All inodes are backed by a overcommitted memory-map
 * represented by 'struct xal_pool'.
 *
 */
#include <inttypes.h>
#include <stddef.h>

struct xal_extent {
	uint64_t start_offset;
	uint64_t start_block;
	uint64_t nblocks;
	void *next;
} __attribute__((packed));

int
xal_extent_pp(struct xal_extent *extent);

struct xal_inode {
	uint64_t ino;	 ///< Inode number of the directory entry; Should the AG be added here?
	uint8_t ftype;	 ///< File-type (directory, filename, symlink etc.)
	uint8_t namelen; ///< Length of the name; not counting nul-termination
	uint32_t nextents;	 ///< Number of extents
	struct xal_extent extent; ///< First extent
	char name[256];		///< Name; not including nul-termination
	uint8_t rsvd[8];
	uint16_t nchildren; ///< Number of children; for directories
	void *children;	    ///< Pointer to array of 'struct xal_inode'
} __attribute__((packed));

int
xal_inode_pp(struct xal_inode *inode);

typedef void (*xal_walk_cb)(struct xal_inode *inode, void *cb_args);

struct xal_pool {
	size_t reserved; ///< Maximum number of inodes in the pool
	size_t allocated; ///< Number of reserved inodes that are allocated
	size_t growby; ///< Number of reserved inodes to allocate at a time
	size_t free; /// Index the next free inode
	struct xal_inode *inodes;
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
xal_pool_map(struct xal_pool *pool, size_t reserved, size_t allocated);

/**
 *
 */
int
xal_pool_claim(struct xal_pool *pool, size_t count, struct xal_inode **inode);

/**
 * An encapsulation of the device handle, this is done preparation for using xNVMe
 */
union xal_handle {
	int fd;
	void *ptr;
};

/**
 * XAL Allocation Group
 *
 * Contains a subset of allocation group fields, individual data for allocation
 * groups in host-endian
 *
 * Byte-order: host-endianess
 */
struct xal_ag {
	uint32_t seqno;
	uint32_t agf_length; ///< Size of allocation group, in blocks
	uint32_t agi_count;  ///< Number of allocated inodes, counting from 1
	uint32_t agi_root;   ///< Block number positioned relative to the AG
	uint32_t agi_level;  ///< levels in inode btree
};

int
xal_ag_pp(struct xal_ag *ag);

struct xal_sb {
	uint32_t blocksize; ///< Size of a block, in bytes
	uint16_t sectsize;  ///< Size of a sector, in bytes
	uint16_t inodesize; ///< inode size, in bytes
	uint16_t inopblock; ///< inodes per block
	uint8_t inopblog;   ///< log2 of inopblock
	uint64_t rootino;   ///< root inode number, in global-address format
	uint32_t agblocks;  ///< Size of an allocation group, in blocks
	uint8_t agblklog;   ///< log2 of 'agblocks' (rounded up)
	uint32_t agcount;   ///< Number of allocation groups
};

/**
 * XAL
 *
 * Contains a handle to the storage device along with meta-data describing the data-layout and a
 * pool of inodes.
 */
struct xal {
	union xal_handle handle;
	struct xal_pool pool;
	struct xal_sb sb;
	struct xal_ag ags[]; ///< Array of 'agcount' number of allocation-groups
};

int
xal_pp(struct xal *xal);

/**
 * Open the block device at 'path'
 *
 * This will retrieve the Superblock (sb) and Allocation Group (AG) headers for all AGs. These are
 * utilized to instantiate the 'struct xal' with a subset of the on-disk-format parsed to native
 * format.
 *
 * @return On success a 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_open(const char *path, struct xal **xal);

void
xal_close(struct xal *xal);

/**
 * Produce an index of the directory and files stored on the device
 */
int
xal_index(struct xal *xal, struct xal_inode **index);

/**
 * Recursively walk the given directory
 *
 * Invoking the given cb_func with cb_data for each directory-entry in the traversal. Do note that
 * not all inode-types are supported, e.g. symlinks are not represented only the types:
 *
 *   * Directory
 *   * Regular file
 *
 * Returns 0 on success. On error, negative errno is returned to indicate the error.
 */
int
xal_walk(struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data);
