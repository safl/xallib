#include <asm-generic/errno.h>
#include <libxnvme.h>
#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <khash.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_be_xfs.h>
#include <xal_odf.h>
#include <xal_pool.h>
#include <xal_pp.h>

KHASH_MAP_INIT_INT64(ino_to_dinode, struct xal_odf_dinode *);

uint32_t
ino_abs_to_rel(struct xal *xal, uint64_t inoabs)
{
	return inoabs & ((1ULL << (xal->sb.agblklog + xal->sb.inopblog)) - 1);
}

void
xal_ino_decode_relative(struct xal *xal, uint32_t ino, uint64_t *agbno, uint32_t *agbino)
{
	// Block Number relative to Allocation Group
	*agbno = (ino >> xal->sb.inopblog) & ((1ULL << xal->sb.agblklog) - 1);

	// Inode number relative to Block
	*agbino = ino & ((1ULL << xal->sb.inopblog) - 1);
}

void
xal_ino_decode_absolute(struct xal *xal, uint64_t ino, uint32_t *seqno, uint64_t *agbno,
			uint32_t *agbino)
{
	// Allocation Group Number -- represented usually stored in 'ag.seqno'
	*seqno = ino >> (xal->sb.inopblog + xal->sb.agblklog);

	xal_ino_decode_relative(xal, ino, agbno, agbino);
}

/**
 * Calculate the on-disk offset of the given filesystem block number
 *
 * Format Assumption
 * =================
 * |       agno        |       bno        |
 * | 64 - agblklog     |  agblklog        |
 */
uint64_t
xal_fsbno_offset(struct xal *xal, uint64_t fsbno)
{
	uint64_t ag, bno;

	ag = fsbno >> xal->sb.agblklog;
	bno = fsbno & ((1 << xal->sb.agblklog) - 1);

	return (ag * xal->sb.agblocks + bno) * xal->sb.blocksize;
}

/**
 * Compute the absolute disk offset of the given agbno reltive to the ag with 'seqno'
 */
uint64_t
xal_agbno_absolute_offset(struct xal *xal, uint32_t seqno, uint64_t agbno)
{
	// Absolute Inode offset in bytes
	return (seqno * (uint64_t)xal->sb.agblocks + agbno) * xal->sb.blocksize;
}

uint64_t
xal_ino_decode_absolute_offset(struct xal *xal, uint64_t ino)
{
	uint32_t seqno, agbino;
	uint64_t offset, agbno;

	xal_ino_decode_absolute(xal, ino, &seqno, &agbno, &agbino);

	// Absolute Inode offset in bytes
	offset = (seqno * (uint64_t)xal->sb.agblocks + agbno) * xal->sb.blocksize;

	return offset + ((uint64_t)agbino * xal->sb.inodesize);
}

void
xal_close(struct xal *xal)
{
	if (!xal) {
		return;
	}

	if (xal->dev) {
		xnvme_buf_free(xal->dev, xal->buf);
	}
	xal_pool_unmap(&xal->inodes);
	xal_pool_unmap(&xal->extents);
	kh_destroy(ino_to_dinode, xal->dinodes_map);
	free(xal->dinodes);
	free(xal);
}

static int
retrieve_mountpoint(const char *dev_uri, char *mntpnt)
{
	FILE *f;
	char d[XAL_PATH_MAXLEN + 1], m[XAL_PATH_MAXLEN + 1];
	bool found = false;

	f = fopen("/proc/mounts", "r");
	if (!f) {
		XAL_DEBUG("FAILED: could not open /proc/mounts; errno(%d)", errno);
		return -errno;
	}

	while (fscanf(f, "%s %s%*[^\n]\n", d, m) == 2) {
		if (strcmp(d, dev_uri) == 0) {
			strcpy(mntpnt, m);
			found = true;
			break;
		}
	}

	fclose(f);

	if (!found) {
		XAL_DEBUG("FAILED: device(%s) not mounted", dev_uri);
		return -EINVAL;
	}

	return 0;
}

int
xal_open(struct xnvme_dev *dev, struct xal **xal, struct xal_opts *opts)
{
	const struct xnvme_ident *ident;
	struct xal_opts opts_default = {0};
	char mountpoint[XAL_PATH_MAXLEN + 1] = {0};
	int err;

	if (!dev) {
		return -EINVAL;
	}

	if (!opts) {
		opts = &opts_default;
	}

	ident = xnvme_dev_get_ident(dev);
	if (!ident) {
		XAL_DEBUG("FAILED: xnvme_dev_get_ident()");
		return -EINVAL;
	}

	if (!opts->be) {
		err = retrieve_mountpoint(ident->uri, mountpoint);
		if (err) {
			XAL_DEBUG("INFO: Failed retrieve_mountpoint(), this is OK");
			opts->be = XAL_BACKEND_XFS;
			err = 0;
		} else {
			XAL_DEBUG("INFO: dev(%s) mounted at path(%s)", ident->uri, mountpoint);
			opts->be = XAL_BACKEND_FIEMAP;
		}
	}

	switch (opts->be) {
		case XAL_BACKEND_XFS:
			return xal_open_be_xfs(dev, xal);

		case XAL_BACKEND_FIEMAP:
			if (strlen(mountpoint) == 0) {
				err = retrieve_mountpoint(ident->uri, mountpoint);
				if (err) {
					XAL_DEBUG("FAILED: retrieve_mountpoint(); err(%d)", err);
					return err;
				}
			}

			return xal_open_be_fiemap(xal, mountpoint);

		default:
			XAL_DEBUG("FAILED: Unexpected backend(%d)", opts->be);
			return -EINVAL;
	}
}

int
xal_index(struct xal *xal)
{
	if (strlen(xal->mountpoint)) {
		return xal_index_fiemap(xal);
	}

	return xal_index_xfs(xal);
}

static int
_walk(struct xal *xal, struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data, int depth)
{
	int err;
	if (cb_func) {
		err = cb_func(xal, inode, cb_data, depth);
		if (err) {
			return err;
		}
	}

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR: {
		struct xal_inode *inodes = inode->content.dentries.inodes;

		for (uint32_t i = 0; i < inode->content.dentries.count; ++i) {
			err = _walk(xal, &inodes[i], cb_func, cb_data, depth + 1);
			if (err) {
				return err;
			}
		}
	} break;

	case XAL_ODF_DIR3_FT_REG_FILE:
		return 0;

	default:
		XAL_DEBUG("FAILED: Unknown / unsupported ftype: %d", inode->ftype);
		return -EINVAL;
	}

	return 0;
}

int
xal_walk(struct xal *xal, struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data)
{
	return _walk(xal, inode, cb_func, cb_data, 0);
}

struct xal_inode *
xal_get_root(struct xal *xal)
{
	return xal->root;
}

uint32_t
xal_get_sb_blocksize(struct xal *xal)
{
	return xal->sb.blocksize;
}

int
xal_inode_path_pp(struct xal_inode *inode)
{
	int wrtn = 0;

	if (!inode) {
		return wrtn;
	}
	if (!inode->parent) {
		return wrtn;
	}

	wrtn += xal_inode_path_pp(inode->parent);
	wrtn += printf("/%.*s", inode->namelen, inode->name);

	return wrtn;
}

bool
xal_inode_is_dir(struct xal_inode *inode)
{
	return inode->ftype == XAL_ODF_DIR3_FT_DIR;
}

bool
xal_inode_is_file(struct xal_inode *inode)
{
	return inode->ftype == XAL_ODF_DIR3_FT_REG_FILE;
}
