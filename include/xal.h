#include <unistd.h>

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