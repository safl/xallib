struct xal_be_fiemap {
	struct xal_backend_base base;
	char *mountpoint;      ///< Path to mountpoint of dev
	struct xal_inotify *inotify;

	uint8_t _rsvd[24];
};
XAL_STATIC_ASSERT(sizeof(struct xal_be_fiemap) == XAL_BACKEND_SIZE, "Incorrect size");

int
xal_be_fiemap_open(struct xal **xal, char *mountpoint, struct xal_opts *opts);
