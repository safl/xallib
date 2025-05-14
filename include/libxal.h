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
#include <libxal_util.h>
#include <libxnvme.h>
#include <stddef.h>
#include <sys/types.h>

#define XAL_INODE_NAME_MAXLEN 255

struct xal_extent {
	uint64_t start_offset;
	uint64_t start_block;
	uint64_t nblocks;
	uint8_t flag;
} __attribute__((packed));

int
xal_extent_pp(struct xal_extent *extent);

struct xal_inode;

struct xal_dentries {
	struct xal_inode *inodes; ///< Pointer to array of 'struct xal_inode'
	uint32_t count;		  ///< Number of children; for directories
};

struct xal_extents {
	struct xal_extent *extent; ///< Pointer to array of 'struct xal_extent'
	uint32_t count;		   ///< Number of extents
};

union xal_inode_content {
	struct xal_dentries dentries;
	struct xal_extents extents;
};

struct xal_inode {
	uint64_t ino;  ///< Inode number of the directory entry; Should the AG be added here?
	uint64_t size; ///< Size in bytes
	union xal_inode_content content;
	uint8_t ftype;			      ///< File-type (directory, filename, symlink etc.)
	uint8_t namelen;		      ///< Length of the name; not counting nul-termination
	char name[XAL_INODE_NAME_MAXLEN + 1]; ///< Name; not including nul-termination
	uint8_t reserved[30];
	struct xal_inode *parent;
};

int
xal_inode_pp(struct xal_inode *inode);

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
 *
 */
int
xal_pool_claim_extents(struct xal_pool *pool, size_t count, struct xal_extent **extents);

int
xal_pool_claim_inodes(struct xal_pool *pool, size_t count, struct xal_inode **inodes);

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
	off_t offset;	     ///< Offset on disk in bytes; seqno * agblocks * blocksize
	uint32_t agf_length; ///< Size of allocation group, in blocks
	uint32_t agi_count;  ///< Number of allocated inodes, counting from 1
	uint32_t agi_root;   ///< Block number positioned relative to the AG
	uint32_t agi_level;  ///< levels in inode btree
};

int
xal_ag_pp(struct xal_ag *ag);

struct xal_sb {
	uint32_t blocksize;  ///< Size of a block, in bytes
	uint16_t sectsize;   ///< Size of a sector, in bytes
	uint16_t inodesize;  ///< inode size, in bytes
	uint16_t inopblock;  ///< inodes per block
	uint8_t inopblog;    ///< log2 of inopblock
	uint64_t icount;     ///< allocated inodes
	uint64_t nallocated; ///< Allocated inodes - sum of agi_count
	uint64_t rootino;    ///< root inode number, in global-address format
	uint32_t agblocks;   ///< Size of an allocation group, in blocks
	uint8_t agblklog;    ///< log2 of 'agblocks' (rounded up)
	uint32_t agcount;    ///< Number of allocation groups
};

/**
 * XAL
 *
 * Contains a handle to the storage device along with meta-data describing the data-layout and a
 * pool of inodes.
 */
struct xal {
	struct xnvme_dev *dev;
	void *buf;		 ///< A single buffer for repetitive IO
	uint8_t *dinodes;	 ///< Array of inodes in on-disk-format
	struct xal_pool inodes;	 ///< Pool of inodes in host-native format
	struct xal_pool extents; ///< Pool of extents in host-native format
	struct xal_inode *root;	 ///< Root of the file-system
	struct xal_sb sb;
	struct xal_ag ags[]; ///< Array of 'agcount' number of allocation-groups
};

typedef void (*xal_walk_cb)(struct xal *xal, struct xal_inode *inode, void *cb_args, int level);

int
xal_pp(struct xal *xal);

/**
 * Open and decode the file-system meta-data on the given device
 *
 * This will retrieve the Superblock (sb) and Allocation Group (AG) headers for all AGs. These are
 * utilized to instantiate the 'struct xal' with a subset of the on-disk-format parsed to native
 * format.
 *
 * @param dev Pointer to xnvme device handled as retrieved with xnvme_dev_open()
 * @param xal Pointer
 *
 * @return On success a 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_open(struct xnvme_dev *dev, struct xal **xal);

void
xal_close(struct xal *xal);

/**
 * Retrieve inodes from disk and decode the on-disk-format of the retrieved data
 *
 * @param xal Pointer to the xal
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_dinodes_retrieve(struct xal *xal);

/**
 * Produce an index of the directory and files stored on the device
 *
 * Assumes that you have retrieved all the inodes from disk via xal_dinodes_retrieve()
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_index(struct xal *xal);

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
xal_walk(struct xal *xal, struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data);

/**
 * Decodes the given inode number in AG-Relative Inode number format
 *
 * For details on the format then see the description in section "13.3.1 Inode Numbers"
 *
 * @param xal Pointer to the xal-instance that the inode belongs to
 * @param ino The absolute inode number to decode
 * @param agbno Pointer to store the AG-relative block number
 * @param agbino The inode relative to the AG-relative block
 */
void
xal_ino_decode_relative(struct xal *xal, uint32_t ino, uint32_t *agbno, uint32_t *agbino);

/**
 * Decodes the inode number in Absolute Inode number format
 *
 * For details on the format then see the description in section "13.3.1 Inode Numbers"
 *
 * @param xal Pointer to the xal-instance that the inode belongs to
 * @param ino The absolute inode number to decode
 * @param seqno Pointer to store the AG number
 * @param agbno Pointer to store the AG-relative block number
 * @param agbino The inode relative to the AG-relative block
 */
void
xal_ino_decode_absolute(struct xal *xal, uint64_t ino, uint32_t *seqno, uint32_t *agbno,
			uint32_t *agbino);

/**
 * Compute the byte-offset on disk of the given inode in absolute inode number format
 *
 * @param xal Pointer to the xal-instance that the inode belongs to
 * @param ino The absolute inode number to decode
 *
 * @returns The byte-offset on success.
 */
uint64_t
xal_ino_decode_absolute_offset(struct xal *xal, uint64_t ino);

uint64_t
xal_fsbno_offset(struct xal *xal, uint64_t fsbno);

int
xal_inode_path_pp(struct xal_inode *inode);

