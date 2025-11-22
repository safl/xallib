#include <asm-generic/errno.h>
#include <libxnvme.h>
#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include "khash.h"
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

// Declare the hash map type
KHASH_MAP_INIT_STR(my_struct_map, struct xal_extents)

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

	kh_destroy(my_struct_map, xal->filemetadata_map);
	xal_pool_unmap(&xal->inodes);
	xal_pool_unmap(&xal->extents);

	be = (struct xal_backend_base *)&xal->be;
	be->close(be);

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
			return xal_be_xfs_open(dev, xal);

		case XAL_BACKEND_FIEMAP:
			if (strlen(mountpoint) == 0) {
				err = retrieve_mountpoint(ident->uri, mountpoint);
				if (err) {
					XAL_DEBUG("FAILED: retrieve_mountpoint(); err(%d)", err);
					return err;
				}
			}

			return xal_be_fiemap_open(xal, mountpoint);

		default:
			XAL_DEBUG("FAILED: Unexpected backend(%d)", opts->be);
			return -EINVAL;
	}
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
	if (cb_func) {
		cb_func(xal, inode, cb_data, depth);
	}

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR: {
		struct xal_inode *inodes = inode->content.dentries.inodes;

		for (uint32_t i = 0; i < inode->content.dentries.count; ++i) {
			_walk(xal, &inodes[i], cb_func, cb_data, depth + 1);
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

// Function to insert a key-value pair into the hash map
void
insert_map(void *map_ptr, const char *key, struct xal_extents value)
{
	khash_t(my_struct_map) *map = (khash_t(my_struct_map) *)map_ptr;
	khiter_t k;
	int absent;

	// Check if the key already exists
	k = kh_put(my_struct_map, map, key, &absent);

	if (absent) {
		// Key doesn't exist, insert new entry
		kh_value(map, k) = value;
	} else {
		// Key exists, update the value
		kh_value(map, k) = value;
	}
}

// Function to search for a key in the hash map
struct xal_extents *
search_map(khash_t(my_struct_map) * map, const char *key)
{
	khiter_t k = kh_get(my_struct_map, map, key);
	if (k == kh_end(map)) {
		return NULL; // Key not found
	}
	return &kh_value(map, k);
}

int
hash_table_insert(struct xal *xal, const char *key, struct xal_extents value)
{
	// Insert key-value pairs
	insert_map(xal->filemetadata_map, key, value);
	return 0;
}

void
hash_table_search(struct xal *xal, const char *key)
{
	uint64_t start_offset;
	uint64_t start_block;
	uint64_t nblocks;
	uint8_t flag;
	size_t fofz_begin;
	size_t fofz_end;
	size_t bofz_begin;
	size_t bofz_end;

	struct xal_extents *extents = search_map(xal->filemetadata_map, key);
	if (extents != NULL) {
		for (unsigned int i = 0; i < extents->count; i++) {
			start_offset = extents->extent->start_offset;
			start_block = extents->extent->start_block;
			nblocks = extents->extent->nblocks;
			flag = extents->extent->flag;

			fofz_begin = extents->filemetadata[0].fofz_begin;
			fofz_end = extents->filemetadata[0].fofz_end;
			bofz_begin = extents->filemetadata[0].bofz_begin;
			bofz_end = extents->filemetadata[0].bofz_end;
			printf("\nFile: %s Metadata\n", key);
			printf(
			    "fofz_begin = %lu, fofz_end = %lu, bofz_begin = %lu, bofz_end = %lu\n",
			    fofz_begin, fofz_end, bofz_begin, bofz_end);
			printf(
			    "start_offset = %lu, start_block = %lu, nblocks = %lu, flag = %hhu\n",
			    start_offset, start_block, nblocks, flag);
		}
	} else {
		printf("Key not found.\n");
	}
}

void
create_hash_map(struct xal *xal)
{
	// Create a new hash map
	xal->filemetadata_map = kh_init(my_struct_map);
	if (!xal->filemetadata_map) {
		XAL_DEBUG("FAILED: kh_init()");
	}
}

