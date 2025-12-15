#include <asm-generic/errno.h>
#include <libxnvme.h>
#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
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
#include <xal_pp.h>

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
	struct xal_backend_base *be = (struct xal_backend_base *)&xal->be;

	switch (be->type) {
		case XAL_BACKEND_FIEMAP:
			return fsbno * xal->sb.blocksize;

		case XAL_BACKEND_XFS:
			uint64_t ag, bno;

			ag = fsbno >> xal->sb.agblklog;
			bno = fsbno & ((1 << xal->sb.agblklog) - 1);

			return (ag * xal->sb.agblocks + bno) * xal->sb.blocksize;

		default:
			XAL_DEBUG("FAILED: Unknown backend type(%d)", be->type);
			return -EINVAL;
	}
}

void
xal_close(struct xal *xal)
{
	struct xal_backend_base *be;

	if (!xal) {
		return;
	}

	xal_pool_unmap(&xal->inodes);
	xal_pool_unmap(&xal->extents);

	be = (struct xal_backend_base *)&xal->be;
	be->close(xal);

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
			err = xal_be_xfs_open(dev, xal);
			if (err) {
				XAL_DEBUG("FAILED: xal_be_xfs_open(); err(%d)", err);
				return err;
			}

			break;

		case XAL_BACKEND_FIEMAP:
			if (strlen(mountpoint) == 0) {
				err = retrieve_mountpoint(ident->uri, mountpoint);
				if (err) {
					XAL_DEBUG("FAILED: retrieve_mountpoint(); err(%d)", err);
					return err;
				}
			}

			err = xal_be_fiemap_open(xal, mountpoint, opts);
			if (err) {
				XAL_DEBUG("FAILED: xal_be_fiemap_open(); err(%d)", err);
				return err;
			}

			break;

		default:
			XAL_DEBUG("FAILED: Unexpected backend(%d)", opts->be);
			return -EINVAL;
	}

	(*xal)->dev = dev;

	return 0;
}

int
xal_index(struct xal *xal)
{
	struct xal_backend_base *be = (struct xal_backend_base *)&xal->be;

	return be->index(xal);
}

static int
_walk(struct xal *xal, struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data, int depth)
{
	int err;

	if (atomic_load(&xal->dirty)) {
		XAL_DEBUG("FAILED: File system has changed");
		return -ESTALE;
	}

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

bool
xal_is_dirty(struct xal *xal)
{
	return atomic_load(&xal->dirty);
}

int
xal_get_seq_lock(struct xal *xal)
{
	return atomic_load(&xal->seq_lock);
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

int
xal_extent_in_bytes(struct xal *xal, const struct xal_extent *extent, struct xal_extent_converted *output)
{
	if (!extent) {
		XAL_DEBUG("FAILED: no extent given");
		return -EINVAL;
	}

	output->start_offset = extent->start_offset * xal->sb.blocksize;
	output->size = extent->nblocks * xal->sb.blocksize;
	output->start_block = xal_fsbno_offset(xal, extent->start_block);
	output->unit = XAL_EXTENT_UNIT_BYTES;

	return 0;
}

int
xal_extent_in_lba(struct xal *xal, const struct xal_extent *extent, struct xal_extent_converted *output)
{
	const struct xnvme_spec_idfy_ns *ns;
	uint8_t fidx;
	uint lba_blksze;

	if (!extent) {
		XAL_DEBUG("FAILED: no extent given");
		return -EINVAL;
	}

	ns = xnvme_dev_get_ns(xal->dev);
	if (!ns) {
		XAL_DEBUG("FAILED: xnvme_dev_get_ns(); errno(%d)", errno);
		return -errno;
	}

	fidx = ns->flbas.format;
	if (ns->nlbaf > 16) {
		fidx += ns->flbas.format_msb << 4;
	}

	lba_blksze = 1U << ns->lbaf[fidx].ds;
	XAL_DEBUG("INFO: Found lba block size %d", lba_blksze);

	output->start_offset = extent->start_offset * xal->sb.blocksize / lba_blksze;
	output->size = extent->nblocks * xal->sb.blocksize / lba_blksze;
	output->start_block = xal_fsbno_offset(xal, extent->start_block) / lba_blksze;
	output->unit = XAL_EXTENT_UNIT_LBA;

	return 0;
}
