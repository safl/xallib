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
#include <xal_pp.h>

// Declare the hash map type
KHASH_MAP_INIT_STR(filename_to_extent, struct xal_extents)

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

	kh_destroy(filename_to_extent, xal->file_extent_map);
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

			return xal_be_fiemap_open(xal, mountpoint, opts);

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

int
build_inode_path1(struct xal_inode *inode, char **buffer)
{
	// First, count how many nodes we need to process
	int count = 0;
	struct xal_inode *current = inode;
	while (current && current->parent) {
		count++;
		current = current->parent;
	}
	// Allocate an array to store pointers to all nodes
	struct xal_inode **nodes = malloc(count * sizeof(struct xal_inode *));
	if (!nodes) {
		*buffer = NULL;
		return -1;
	}
	// Fill the array with node pointers (in leaf-to-root order)
	current = inode;
	int i = 0;
	while (current && current->parent) {
		nodes[i++] = current;
		current = current->parent;
	}
	// Calculate the required buffer size
	size_t size = 0;
	for (i = 0; i < count; i++) {
		size += 1 + nodes[i]->namelen; // slash + name length
	}
	// Allocate memory for the string
	*buffer = malloc(size); // Removed +1 for null terminator
	if (!*buffer) {
		free(nodes);
		return -1;
	}
	// Build the string in root-to-leaf order
	size_t pos = 0;
	for (i = count - 1; i >= 0; i--) {
		if (pos > 0) { // Don't add a leading slash
			(*buffer)[pos++] = '/';
		}
		memcpy(&(*buffer)[pos], nodes[i]->name, nodes[i]->namelen);
		pos += nodes[i]->namelen;
	}
	// Removed the null termination line: (*buffer)[pos] = '\0';
	free(nodes); // Free the temporary array
	return 0;    // Success
}

int
build_inode_path(struct xal_inode *inode, char *buffer)
{
	int count = 0;
	struct xal_inode *current = inode;
	while (current && current->parent) {
		count++;
		current = current->parent;
	}

	char *path_components[count + 1];
	int component_count = 0;

	// Collect all path components
	current = inode;
	while (current) {
		path_components[component_count++] = current->name;
		current = current->parent;
	}

	// Build the string in root-to-leaf order
	size_t pos = 0;
	for (int i = component_count - 1; i >= 0; i--) {
		if (pos > 0) {		 // Don't add a leading slash
			if (pos < 55) { // Leave space for potential null terminator
				buffer[pos++] = '/';
			} else {
				break; // Buffer is full
			}
		}
		size_t name_len = strlen(path_components[i]);
		if (pos + name_len >= 56) {
			name_len = 56 - pos; // Truncate to fit in buffer
		}
		memcpy(&buffer[pos], path_components[i], name_len);
		pos += name_len;
	}

	// Add null termination at the end
	if (pos < 56) {
		buffer[pos] = '\0';
	} else {
		buffer[55] = '\0'; // Ensure null termination even if buffer is full
	}
	return 0; // Success
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
	khash_t(filename_to_extent) *map = (khash_t(filename_to_extent) *)map_ptr;
	khiter_t k;
	int absent;

	// Check if the key already exists
	k = kh_put(filename_to_extent, map, key, &absent);
	if (absent) {
		// Key doesn't exist, insert new entry
		kh_value(map, k) = value;
	} else {
		// Key exists, update the value
		printf("Key = %s : Existed \n", key);
		kh_value(map, k) = value;
	}
}

// Function to search for a key in the hash map
struct xal_extents *
search_map(khash_t(filename_to_extent) * map, const char *key)
{
	khiter_t k = kh_get(filename_to_extent, map, key);
	if (k == kh_end(map)) {
		return NULL; // Key not found
	}
	return &kh_value(map, k);
}

int
set_file_extent_info(struct xal *xal, const char *key, struct xal_extents value)
{
	// Insert key-value pairs
	insert_map(xal->file_extent_map, key, value);
	return 0;
}

void
get_file_extent_info(struct xal *xal, const char *key, struct xal_extents **extents)
{
	//Using key as filename, get the extents from Hashmap


	//struct xal_extents *extents = search_map(xal->file_extent_map, key);
	*extents = search_map(xal->file_extent_map, key);

	#if 0
	uint64_t start_offset, start_block, nblocks;
	uint8_t flag;
	size_t fofz_begin, fofz_end, bofz_begin, bofz_end;
	uint32_t blocksize = xal_get_sb_blocksize(xal);

	if (extents != NULL) {

			start_offset = extents->extent->start_offset;
			start_block = extents->extent->start_block;
			nblocks = extents->extent->nblocks;
			flag = extents->extent->flag;

			fofz_begin = (extents->extent->start_offset * blocksize) / 512;
			fofz_end = fofz_begin + (extents->extent->nblocks * blocksize) / 512 - 1;
			bofz_begin = xal_fsbno_offset(xal, extents->extent->start_block) / 512;
			bofz_end = bofz_begin + (extents->extent->nblocks * blocksize) / 512 - 1;

			printf("fofz_begin = %lu, fofz_end = %lu, bofz_begin = %lu, bofz_end = %lu\n", fofz_begin, fofz_end, bofz_begin, bofz_end);
			printf("start_offset = %lu, start_block = %lu, nblocks = %lu, flag = %hhu\n", start_offset, start_block, nblocks, flag);
	} else {
		printf("Key not found.\n");
	}
	#endif 
}

void
create_file_extent_hash_map(struct xal *xal)
{
	// Create a new hash map
	xal->file_extent_map = kh_init(filename_to_extent);
	if (!xal->file_extent_map) {
		XAL_DEBUG("FAILED: kh_init()");
	}
}

