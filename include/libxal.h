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

struct xal_dir_entry {
	uint8_t namelen; ///< Length of the name excluding nul-termination
	char name[257];	 ///< Nul-terminated string
	uint8_t ftype;
	uint64_t ino; ///< Inode number ??? Should the AG be added here?
} __attribute__((packed));

struct xal_dir {
	uint8_t count;
	struct xal_dir_entry entries[];
} __attribute__((packed));

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

union xal_dev_handle {
	int fd;
	void *ptr;
};

/**
 * XAL
 *
 * Contains a handle to the storage device along with meta-data describing the
 * data-layout.
 *
 * Byte-order: host-endianess
 */
struct xal {
	union xal_dev_handle handle;
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
 * Open the block device at 'path', a mountpoint from , you could think of this
 * as "mounting" it
 */
int
xal_open(const char *path, struct xal **xal);

void
xal_close(struct xal *xal);

int
xal_pp(struct xal *xal);

uint64_t
xal_get_inode_offset(struct xal *xal, uint64_t ino);

/**
 * Traverse the given mountpoint; invokes 'cb_func(cb_data, inode)' for each
 * discovered inode
 *
 * Returns 0 on success. On error, negative errno is returned to indicate the
 * error.
 */
int
xal_traverse(struct xal *xal, void *cb_func, void *cb_data);

/**
 *
 * The given buffer should be the entire inode, e.g. all of the 256, 512, or how many bytes an
 * inode is, as described by the given 'xal.inodesize'.
 */
int
xal_dir_from_shortform(struct xal *xal, void *buf, struct xal_dir **dir);

/**
 * Pretty-print the given 'struct xal_superblock'
 *
 * NOTE: Assumes that the given superblock has been converted to host endianess
 */
int
xal_sb_pp(void *buf);

int
xal_agf_pp(void *buf);

int
xal_agi_pp(void *buf);

int
xal_agfl_pp(void *buf);

int
xal_dinode_pp(void *buf);
