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
 * ==========
 *
 * The XFS on-disk format make use of integer values in big-endian format, thus value convertion
 * to little-endian is needed on e.g. x86 systems. Such helpers are not exposed here, rather the
 * conversions are performed by the library and the representations found here are in "native"
 * format.
 *
 * Directories
 * ===========
 *
 * XFS has 4 or 5 ways of storing directory indexes (filename + inode number) optimized by
 * use-case. When XAL parses these, then they normalized into the form represented by the struct
 * "xal_dir_entry" and "xal_dir_index".
 *
 */
#include <inttypes.h>

struct xal_extent {
	uint64_t start_offset;
	uint64_t start_block;
	uint64_t nblocks;
	void *next;
} __attribute__((packed));

int
xal_extent_pp(struct xal_extent *extent);

struct xal_dir_entry {
	uint64_t ino;	 ///< Inode number of the directory entry; Should the AG be added here?
	uint8_t ftype;	 ///< File-type (directory, filename, symlink etc.)
	uint8_t namelen; ///< Length of the name; not counting nul-termination
	uint32_t count;	 ///< Number of extents
	struct xal_extent head; ///< First extent
	char name[256];		///< Name; not including nul-termination
	uint8_t rsvd[18];
} __attribute__((packed));

struct xal_inode {
	uint64_t ino;	 ///< Inode number of the directory entry; Should the AG be added here?
	uint8_t ftype;	 ///< File-type (directory, filename, symlink etc.)
	uint8_t namelen; ///< Length of the name; not counting nul-termination
	uint32_t count;	 ///< Number of extents
	struct xal_extent head; ///< First extent
	char name[256];		///< Name; not including nul-termination
	uint8_t rsvd[9];
	uint8_t nchildren; ///< Number of children
	void *children[];
} __attribute__((packed));

int
xal_inode_pp(struct xal_inode *inode);

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
	uint32_t agi_root;   ///< root of inode btree, format?
	uint32_t agi_level;  ///< levels in inode btree
};

int
xal_ag_pp(struct xal_ag *ag);

/**
 * XAL
 *
 * Contains a handle to the storage device along with meta-data describing the
 * data-layout.
 */
struct xal {
	union xal_handle handle;
	uint32_t blocksize;  ///< Size of a block, in bytes
	uint16_t sectsize;   ///< Size of a sector, in bytes
	uint16_t inodesize;  ///< inode size, in bytes
	uint16_t inopblock;  ///< inodes per block
	uint8_t inopblog;    ///< log2 of inopblock
	uint64_t rootino;    ///< root inode number, in global-address format
	uint32_t agblocks;   ///< Size of an allocation group, in blocks
	uint8_t agblklog;    ///< log2 of 'agblocks' (rounded up)
	uint32_t agcount;    ///< Number of allocation groups
	struct xal_ag ags[]; ///< Array of 'agcount' number of allocation-groups
};

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

int
xal_get_index(struct xal *xal, struct xal_inode **index);

int
xal_pp(struct xal *xal);

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
xal_dir_walk(struct xal_inode *dir, void *cb_func, void *cb_data);

/**
 * Pretty-print the given directory
 *
 * Returns 0 on success. On error, negative errno is returned to indicate the error.
 */
int
xal_dir_pp(struct xal_inode *dir);
