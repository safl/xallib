struct xal_be_xfs {
	struct xal_backend_base base;
	struct xnvme_dev *dev;
	void *buf;            ///< A single buffer for repetitive IO
	uint8_t *dinodes;     ///< Array of inodes in on-disk-format
	void *dinodes_map;    ///< Map of dinodes for O(1) ~ avg. lookup
	struct xal_ag *ags;   ///< Array of 'agcount' number of allocation-groups
};
XAL_STATIC_ASSERT(sizeof(struct xal_be_xfs) == XAL_BACKEND_SIZE, "Incorrect size");

int
xal_be_xfs_open(struct xnvme_dev *dev, struct xal **xal);
