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
#include <unistd.h>
#include <xal.h>
#include <xal_pool.h>
#include <xal_pp.h>

#define BUF_NBYTES 4096

int
process_inode_ino(struct xal *xal, uint64_t ino, struct xal_inode *self);

uint64_t
xal_get_inode_offset(struct xal *xal, uint64_t ino)
{
	uint64_t agno, agbno, agbino, offset;

	// Allocation Group Number
	agno = ino >> xal->sb.agblklog;

	// Block Number relative to Allocation Group
	agbno = (ino & ((1ULL << xal->sb.agblklog) - 1)) >> xal->sb.inopblog;

	// Inode number relative to Block in Allocation Group
	agbino = ino & ((1ULL << xal->sb.inopblog) - 1);

	// Absolute Inode offset in bytes
	offset = (agno * xal->sb.agblocks + agbno) * xal->sb.blocksize;

	return offset + (agbino * xal->sb.inodesize);
}

void
xal_close(struct xal *xal)
{
	if (!xal) {
		return;
	}

	xal_pool_unmap(&xal->pool);
	close(xal->handle.fd);
	free(xal);
}

int
xal_open(const char *path, struct xal **xal)
{
	struct xal base = {0};
	char buf[BUF_NBYTES] = {0};
	const struct xal_odf_sb *psb = (void *)buf;
	struct xal *cand;
	ssize_t nbytes;
	int err;

	base.handle.fd = open(path, O_RDONLY);
	if (-1 == base.handle.fd) {
		perror("Failed opening device");
		return -errno;
	}

	// Read Primary Superblock and AG headers
	nbytes = pread(base.handle.fd, buf, BUF_NBYTES, 0);
	if (nbytes != BUF_NBYTES) {
		perror("Reading Primary Superblock failed.");
		return -EIO;
	}

	// Setup the Superblock information subset; using big-endian conversion
	base.sb.blocksize = be32toh(psb->blocksize);
	base.sb.sectsize = be16toh(psb->sectsize);
	base.sb.inodesize = be16toh(psb->inodesize);
	base.sb.inopblock = be16toh(psb->sb_inopblock);
	base.sb.inopblog = psb->sb_inopblog;
	base.sb.rootino = be64toh(psb->rootino);
	base.sb.agblocks = be32toh(psb->agblocks);
	base.sb.agblklog = psb->sb_agblklog;
	base.sb.agcount = be32toh(psb->agcount);

	cand = calloc(1, sizeof(*cand) + sizeof(*(cand->ags)) * base.sb.agcount);
	if (!cand) {
		perror("Failed allocating Reading Primary Superblock failed.");
		return -errno;
	}
	*cand = base;

	// Retrieve allocation-group meta, convert it, and store it.
	for (uint32_t agno = 0; agno < cand->sb.agcount; ++agno) {
		struct xal_agf *agf = (void *)(buf + cand->sb.sectsize);
		struct xal_agi *agi = (void *)(buf + cand->sb.sectsize * 2);
		off_t offset;

		memset(buf, 0, BUF_NBYTES);

		offset = (off_t)agno * (off_t)cand->sb.agblocks * (off_t)cand->sb.blocksize;
		nbytes = pread(cand->handle.fd, buf, BUF_NBYTES, offset);
		if (nbytes != BUF_NBYTES) {
			perror("Reading AG Headers failed");
			free(cand);
			return -EIO;
		}

		cand->ags[agno].seqno = agno;
		cand->ags[agno].agf_length = be32toh(agf->length);
		cand->ags[agno].agi_count = be32toh(agi->agi_count);
		cand->ags[agno].agi_level = be32toh(agi->agi_level);
		cand->ags[agno].agi_root = be32toh(agi->agi_root);

		/** minimalistic verification of headers **/
		assert(be32toh(agf->magicnum) == XAL_AGF_MAGIC);
		assert(be32toh(agi->magicnum) == XAL_AGI_MAGIC);
		assert(agno == be32toh(agi->seqno));
		assert(agno == be32toh(agf->seqno));
	}

	// Setup inode memory-pool
	err = xal_pool_map(&cand->pool, 40000000UL, 100000UL);
	if (err) {
		printf("xal_pool_map(...); err(%d)\n", err);
		return err;
	}

	// All is good; promote the candidate
	*xal = cand;

	return 0;
}

int
process_inode_shortform(struct xal *xal, void *inode, struct xal_inode *self)
{
	struct xal_inode *children;
	uint8_t *cursor = inode;
	uint8_t count, i8count;
	int err;

	cursor += sizeof(struct xal_dinode); ///< Advance past inode data

	count = *cursor;
	cursor += 1; ///< Advance past count

	i8count = *cursor;
	cursor += 1; ///< Advance past i8count

	cursor += i8count ? 8 : 4; ///< Advance past parent inode number

	err = xal_pool_claim(&xal->pool, count, &children);
	if (err) {
		return err;
	}
	self->children = children;
	self->nchildren = count;

	/** DECODE: namelen[1], offset[2], name[namelen], ftype[1], ino[4] | ino[8] */
	for (int i = 0; i < count; ++i) {
		struct xal_inode *child = &children[i];

		child->namelen = *cursor;
		cursor += 1 + 2; ///< Advance past 'namelen' and 'offset[2]'

		memcpy(child->name, cursor, child->namelen);
		cursor += child->namelen; ///< Advance past 'name'

		child->ftype = *cursor;
		cursor += 1; ///< Advance past 'ftype'

		if (i8count) {
			i8count--;
			child->ino = be64toh(*(uint64_t *)cursor);
			cursor += 8; ///< Advance past 64-bit inode number
		} else {
			child->ino = be32toh(*(uint32_t *)cursor);
			cursor += 4; ///< Advance past 32-bit inode number
		}

		process_inode_ino(xal, child->ino, child);
	}

	return 0;
}

void
decode_xfs_extent(uint64_t l0, uint64_t l1, struct xal_extent *extent)
{
	// Extract start offset (bits 9-62)
	extent->start_offset = (l0 >> 9) & 0xFFFFFFFFFFFFF;

	// Extract start block (l0:0-8 and l1:21-63)
	extent->start_block = ((l0 & 0x1FF) << 43) | (l1 >> 21);

	// Extract block count (l1:0-20)
	extent->nblocks = l1 & 0x1FFFFF;
}

int
process_inode_extents(struct xal *xal, void *buf, struct xal_inode *self)
{
	struct xal_dinode *dinode = buf;
	uint8_t *cursor = buf;
	uint64_t nextents;

	/**
	 * For some reason then di_big_nextents is populated. As far as i understand that should
	 * not happen for format=0x2 "extents" as this should have all extent-records inline in the
	 * inode. Thus this abomination... just grabbing whatever has a value...
	 */
	nextents =
	    (dinode->di_nextents) ? be32toh(dinode->di_nextents) : be64toh(dinode->di_big_nextents);

	/** Ensure that the buffer contains the on-disk format */
	assert(dinode->di_magic == 0x4E49);

	/** Multiple extents are not implemented yet; add a memory-pool for them */
	assert(nextents <= 1);

	printf("ino: 0x%016"PRIX64"\n", self->ino);
	printf("nextents: %" PRIu64 "\n", nextents);
	printf("size: %" PRIu64 "\n", be64toh(dinode->di_size));

	cursor += sizeof(struct xal_dinode); ///< Advance past inode data

	for (uint64_t i = 0; i < nextents; ++i) {
		uint64_t l0, l1;

		l0 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		l1 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		decode_xfs_extent(l0, l1, &self->extent);
	}

	return 0;
}

/**
 * Internal helper recursively traversing the on-disk-format to build an index of the file-system
 */
int
process_inode_ino(struct xal *xal, uint64_t ino, struct xal_inode *self)
{
	uint8_t buf[BUF_NBYTES] = {0};
	struct xal_dinode *dinode = (void *)buf;
	ssize_t nbytes;

	///< Read the on-disk inode data
	nbytes = pread(xal->handle.fd, buf, xal->sb.sectsize, xal_get_inode_offset(xal, ino));
	if (nbytes != xal->sb.sectsize) {
		return -EIO;
	}

	switch (dinode->di_format) {
	case XAL_DINODE_FMT_DEV: ///< What is this?
		break;

	case XAL_DINODE_FMT_BTREE: ///< Recursively walk the btree
		break;

	case XAL_DINODE_FMT_EXTENTS: ///< Decode extent in inode
		process_inode_extents(xal, buf, self);
		break;

	case XAL_DINODE_FMT_LOCAL: ///< Decode directory listing in inode
		process_inode_shortform(xal, buf, self);
		break;

	case XAL_DINODE_FMT_UUID:
		break;
	}

	return 0;
}

int
xal_get_index(struct xal *xal, struct xal_inode **index)
{
	struct xal_inode *root;
	int err;

	err = xal_pool_claim(&xal->pool, 1, &root);
	if (err) {
		return err;
	}

	root->ino = xal->sb.rootino;
	root->ftype = XAL_XFS_DIR3_FT_DIR;
	root->namelen = 1;
	root->nextents = 0;
	memcpy(root->name, "/", 1);

	*index = root;

	return process_inode_ino(xal, root->ino, root);
}

int
xal_walk(struct xal_inode *inode, xal_callback cb_func, void *cb_data)
{
	if (cb_func) {
		cb_func(inode, cb_data);
	}
	
	switch(inode->ftype) {
	case XAL_XFS_DIR3_FT_DIR:
		{
			struct xal_inode *children = inode->children;
			
			for(uint16_t i = 0; i < inode->nchildren; ++i) {
				xal_walk(&children[i], cb_func, cb_data);
			}
		}
		break;

	case XAL_XFS_DIR3_FT_REG_FILE:
		return 0;

	default:
		printf("Unknown / unsupported ftype: %d", inode->ftype);
		return -EINVAL;
	}

	return 0;
}