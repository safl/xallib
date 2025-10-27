struct xal_inotify {
	enum xal_watchmode watch_mode;
	int fd;           ///< File descriptor for inotify events, if opened with some xal_watchmode, else 0
	void *inode_map;  ///< Map of inodes from inotify watch descriptors
};

void
xal_be_fiemap_inotify_close(struct xal_inotify *inotify);

int
xal_be_fiemap_inotify_init(struct xal_inotify *inotify, enum xal_watchmode watch_mode);

int
xal_be_fiemap_inotify_add_watcher(struct xal_inotify *inotify, char *path, struct xal_inode *inode);
