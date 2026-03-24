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
	void (*close)(struct xal *xal);
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
	struct xnvme_dev *dev;
	struct xal_pool inodes;  ///< Pool of inodes in host-native format
	struct xal_pool extents; ///< Pool of extents in host-native format
	uint32_t root_idx;       ///< Index of the root inode in the inodes pool
	struct xal_sb sb;
	uint8_t be[XAL_BACKEND_SIZE];
	atomic_bool dirty;       ///< Whether the file system has changed since last index
	atomic_int seq_lock;     ///< An uneven number indicates the struct is being modified and is not safe to read
	bool shared_view;        ///< If true, pool memory is owned externally; xal_close() will not unmap it
};
