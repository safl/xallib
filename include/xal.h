#include <unistd.h>

#define BUF_NBYTES 4096 * 32UL		    ///< Number of bytes in a buffer
#define CHUNK_NINO 64			    ///< Number of inodes in a chunk
#define BUF_BLOCKSIZE 4096		    ///< Number of bytes in a block
#define ODF_BLOCK_DIR_BYTES_MAX 64UL * 1024 ///< Maximum size of a directory block
#define ODF_BLOCK_FS_BYTES_MAX 64UL * 1024  ///< Maximum size of a filestem block
#define ODF_INODE_MAX_NBYTES 2048	    ///< Maximum size of an inode

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
	void *buf;		 ///< A single buffer for repetitive IO
	uint8_t *dinodes;	 ///< Array of inodes in on-disk-format
	void *dinodes_map;	 ///< Map of dinodes for O(1) ~ avg. lookup
	struct xal_pool inodes;	 ///< Pool of inodes in host-native format
	struct xal_pool extents; ///< Pool of extents in host-native format
	struct xal_inode *root;	 ///< Root of the file-system
	struct xal_sb sb;
	struct xal_ag ags[]; ///< Array of 'agcount' number of allocation-groups
};
