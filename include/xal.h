#include <stdatomic.h>
#include <unistd.h>
#include <xal_pool.h>

#define BUF_NBYTES 4096 * 32UL		    ///< Number of bytes in a buffer
#define CHUNK_NINO 64			    ///< Number of inodes in a chunk
#define BUF_BLOCKSIZE 4096		    ///< Number of bytes in a block
#define ODF_BLOCK_DIR_BYTES_MAX 64UL * 1024 ///< Maximum size of a directory block
#define ODF_BLOCK_FS_BYTES_MAX 64UL * 1024  ///< Maximum size of a filestem block
#define ODF_INODE_MAX_NBYTES 2048	    ///< Maximum size of an inode
#define XAL_BACKEND_SIZE 64

struct xal_backend_base {
	enum xal_backend type;
	int (*index)(struct xal *xal);
	void (*close)(void *be_ptr);
};

struct xal_sb {
	uint32_t blocksize;    ///< Size of a block, in bytes
	uint16_t sectsize;     ///< Size of a sector, in bytes
	uint16_t inodesize;    ///< inode size, in bytes
	uint16_t inopblock;    ///< inodes per block
	uint8_t inopblog;      ///< log2 of inopblock
	uint64_t icount;       ///< allocated inodes
	uint64_t nallocated;   ///< Allocated inodes - sum of agi_count
	uint64_t rootino;      ///< root inode number, in global-address format
	uint32_t agblocks;     ///< Size of an allocation group, in blocks
	uint8_t agblklog;      ///< log2 of 'agblocks' (rounded up)
	uint32_t agcount;      ///< Number of allocation groups
	uint32_t dirblocksize; ///< Size of a directory block, in bytes
};

/**
 * XAL
 * 
 * Contains a handle to the storage device along with meta-data describing the data-layout and a
 * pool of inodes.
 *
 * @struct xal
 */
struct xal {
	void *file_extent_map;	 ///< Map of Filename with its extents
	struct xal_pool inodes;	 ///< Pool of inodes in host-native format
	struct xal_pool extents; ///< Pool of extents in host-native format
	struct xal_inode *root;	 ///< Root of the file-system
	struct xal_sb sb;
	uint8_t be[XAL_BACKEND_SIZE];
	atomic_bool dirty;       ///< Whether the file system has changed since last index
	atomic_int seq_lock;     ///< An uneven number indicates the struct is being modified and is not safe to read
};

