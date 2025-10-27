#include <unistd.h>

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

/**
 * XAL
 * 
 * Contains a handle to the storage device along with meta-data describing the data-layout and a
 * pool of inodes.
 *
 * @struct xal
 */
struct xal {
	struct xal_pool inodes;	 ///< Pool of inodes in host-native format
	struct xal_pool extents; ///< Pool of extents in host-native format
	struct xal_inode *root;	 ///< Root of the file-system
	struct xal_sb sb;
	uint8_t be[XAL_BACKEND_SIZE];
};
