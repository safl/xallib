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
#include <xal_odf.h>
#include <xal_pool.h>
#include <xal_pp.h>

#define BUF_NBYTES 4096 * 32UL ///< Number of bytes in a buffer
#define CHUNK_NINO 64	       ///< Number of inodes in a chunk

int
process_inode_ino(struct xal *xal, uint64_t ino, struct xal_inode *self);

uint32_t
ino_abs_to_rel(struct xal *xal, uint64_t inoabs)
{
	return inoabs & ((1ULL << (xal->sb.agblklog + xal->sb.inopblog)) - 1);
}

void
xal_ino_decode_relative(struct xal *xal, uint32_t ino, uint32_t *agbno, uint32_t *agbino)
{
	// Block Number relative to Allocation Group
	*agbno = (ino >> xal->sb.inopblog) & ((1ULL << xal->sb.agblklog) - 1);

	// Inode number relative to Block
	*agbino = ino & ((1ULL << xal->sb.inopblog) - 1);
}

void
xal_ino_decode_absolute(struct xal *xal, uint64_t ino, uint32_t *seqno, uint32_t *agbno,
			uint32_t *agbino)
{
	// Allocation Group Number -- represented usually stored in 'ag.seqno'
	*seqno = ino >> (xal->sb.inopblog + xal->sb.agblklog);

	xal_ino_decode_relative(xal, ino, agbno, agbino);
}

uint64_t
xal_get_inode_offset(struct xal *xal, uint64_t ino)
{
	uint32_t seqno, agbno, agbino;
	uint64_t offset;

	xal_ino_decode_absolute(xal, ino, &seqno, &agbno, &agbino);

	// Absolute Inode offset in bytes
	offset = (seqno * xal->sb.agblocks + agbno) * xal->sb.blocksize;

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
	free(xal->dinodes);
	free(xal);
}

int
_pread(struct xal *xal, void *buf, size_t count, off_t offset)
{
	ssize_t nbytes;

	memset(buf, 0, count);
	nbytes = pread(xal->handle.fd, buf, count, offset);
	if ((nbytes == -1) || ((size_t)nbytes != count)) {
		perror("pread(...);\n");
		return -EIO;
	}

	return 0;
}

/**
 * Retrieve and decode the allocation group headers for a given allocation group
 *
 * This will retrieve the block containing the superblock and allocation-group headers. A subset of
 * the allocation group headers is decoded and xal->ags[seqo] is populated with the decoded data.
 *
 * Assumes the following:
 *
 * - The handle (xal.handle) set setup
 * - The superblock (xal.sb) is initiatialized
 *
 * @param buf IO buffer, sufficiently large to hold a block of data
 * @param seqno The sequence number of the allocation group aka agno
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
retrieve_and_decode_allocation_group(uint32_t seqno, void *buf, struct xal *xal)
{
	uint8_t *cursor = buf;
	off_t offset = (off_t)seqno * (off_t)xal->sb.agblocks * (off_t)xal->sb.blocksize;
	struct xal_odf_agi *agi = (void *)(cursor + xal->sb.sectsize * 2);
	struct xal_odf_agf *agf = (void *)(cursor + xal->sb.sectsize);
	int err;

	err = _pread(xal, buf, xal->sb.sectsize * 4, offset);
	if (err) {
		perror("_pread();\n");
		return err;
	}

	xal->ags[seqno].seqno = seqno;
	xal->ags[seqno].offset = offset;
	xal->ags[seqno].agf_length = be32toh(agf->length);
	xal->ags[seqno].agi_count = be32toh(agi->agi_count);
	xal->ags[seqno].agi_level = be32toh(agi->agi_level);
	xal->ags[seqno].agi_root = be32toh(agi->agi_root);

	/** minimalistic verification of headers **/
	assert(be32toh(agf->magicnum) == XAL_ODF_AGF_MAGIC);
	assert(be32toh(agi->magicnum) == XAL_ODF_AGI_MAGIC);
	assert(seqno == be32toh(agi->seqno));
	assert(seqno == be32toh(agf->seqno));

	return 0;
}

/**
 * Retrieve the superblock from disk and decode the on-disk-format
 *
 * This will grow the memory backing 'xal' as it will make room for allocation-groups.
 *
 * @param buf IO buffer, sufficiently large to hold a block of data
 * @param xal Double-pointer to the xal
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
retrieve_and_decode_primary_superblock(void *buf, struct xal **xal)
{
	const struct xal_odf_sb *psb = buf;
	struct xal *cand;
	int err;

	err = _pread(*xal, buf, 4096, 0);
	if (err) {
		perror("_pread();\n");
		return -errno;
	}

	cand = realloc(*xal, sizeof(*cand) + sizeof(*(cand->ags)) * be32toh(psb->agcount));
	if (!cand) {
		perror("realloc();\n");
		return -errno;
	}

	// Setup the Superblock information subset; using big-endian conversion
	cand->sb.blocksize = be32toh(psb->blocksize);
	cand->sb.sectsize = be16toh(psb->sectsize);
	cand->sb.inodesize = be16toh(psb->inodesize);
	cand->sb.inopblock = be16toh(psb->inopblock);
	cand->sb.inopblog = psb->inopblog;
	cand->sb.icount = be64toh(psb->icount);
	cand->sb.rootino = be64toh(psb->rootino);
	cand->sb.agblocks = be32toh(psb->agblocks);
	cand->sb.agblklog = psb->agblklog;
	cand->sb.agcount = be32toh(psb->agcount);

	*xal = cand;

	return 0;
}

int
xal_open(const char *path, struct xal **xal)
{
	uint8_t buf[BUF_NBYTES];
	struct xal *cand;
	int err;

	cand = calloc(1, sizeof(*cand));
	if (!cand) {
		perror("calloc();;\n");
		return -errno;
	}

	cand->handle.fd = open(path, O_RDONLY);
	if (-1 == cand->handle.fd) {
		perror("Failed opening device;\n");
		xal_close(cand);
		return -errno;
	}

	err = retrieve_and_decode_primary_superblock(buf, &cand);
	if (err) {
		perror("_alloc_and_initialize_using_odf_buf();\n");
		xal_close(cand);
		return err;
	}

	for (uint32_t seqno = 0; seqno < cand->sb.agcount; ++seqno) {
		err = retrieve_and_decode_allocation_group(seqno, buf, cand);
		if (err) {
			xal_close(cand);
			return err;
		}

		cand->sb.nallocated += cand->ags[seqno].agi_count;
	}

	// Setup inode memory-pool
	err = xal_pool_map(&cand->pool, 40000000UL, cand->sb.nallocated);
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

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

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
process_inode_extents(void *buf, struct xal_inode *self)
{
	struct xal_odf_dinode *dinode = buf;
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

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

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
	struct xal_odf_dinode *dinode = (void *)buf;
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
		process_inode_extents(buf, self);
		break;

	case XAL_DINODE_FMT_LOCAL: ///< Decode directory listing in inode
		process_inode_shortform(xal, buf, self);
		/// This could also be a small file?
		break;

	case XAL_DINODE_FMT_UUID:
		break;
	}

	return 0;
}

/**
 * Retrieve all the allocated inodes stored within the give allocation group
 *
 * It is assumed that the inode-allocation-b+tree is rooted at the given 'blkno'
 */
int
retrieve_dinodes_via_iabt3(struct xal *xal, struct xal_ag *ag, uint64_t blkno, uint64_t *index)
{
	char buf[BUF_NBYTES] = {0};
	struct xal_odf_btree_iab3_sfmt *iab3 = (void *)buf;
	off_t offset;
	int err;

	printf("# retrieve_dinodes_via_iabt3(); seqno: %" PRIu32 "\n", ag->seqno);

	/** Compute the absolute offset for the block and retrieve it **/
	offset = (xal->sb.agblocks * ag->seqno + blkno) * xal->sb.blocksize;
	err = _pread(xal, buf, xal->sb.blocksize, offset);
	if (err) {
		perror("_pread();\n");
		return err;
	}

	iab3->magic.num = iab3->magic.num;
	iab3->level = be16toh(iab3->level);
	iab3->numrecs = be16toh(iab3->numrecs);
	iab3->leftsib = be32toh(iab3->leftsib);
	iab3->rightsib = be32toh(iab3->rightsib);
	iab3->blkno = be64toh(iab3->blkno);

	assert(be32toh(iab3->magic.num) == XAL_ODF_IBT_CRC_MAGIC);

	if (iab3->level) {
		return 0;
	}

	for (uint16_t reci = 0; reci < iab3->numrecs; ++reci) {
		struct xal_odf_inobt_rec *rec = (void *)(buf + sizeof(*iab3) + reci * sizeof(*rec));
		uint8_t inodechunk[BUF_NBYTES];
		uint32_t agbno, agbino;

		rec->startino = be32toh(rec->startino);
		rec->holemask = be16toh(rec->holemask);
		rec->count = rec->count;
		rec->freecount = rec->freecount;
		rec->free = be64toh(rec->free);

		/**
		 * Determine the block number relative to the allocation group
		 *
		 * This block should be the first block
		 */
		xal_ino_decode_relative(xal, rec->startino, &agbno, &agbino);

		/**
		 * Assumption: if the inode-offset is non-zero, then offset-calucations are
		 *             incorrect as they do not account for the only the block where the
		 *             inode-chunk is supposed to start.
		 */
		assert(agbino == 0);

		/**
		 * Populate the inode-buffer with data from all the blocks
		 */
		{
			uint64_t chunk_nbytes =
			    (CHUNK_NINO / xal->sb.inopblock) * xal->sb.blocksize;
			off_t chunk_offset = agbno * xal->sb.blocksize + ag->offset;

			assert(chunk_nbytes < BUF_NBYTES);

			err = _pread(xal, inodechunk, chunk_nbytes, chunk_offset);
			if (err) {
				printf("_pread(chunk)\n");
				return err;
			}
		}

		/**
		 * Traverse the inodes in the chunk, skipping unused and free inodes.
		 */
		for (uint8_t chunk_index = 0; chunk_index < rec->count; ++chunk_index) {
			uint8_t *chunk_cursor = &inodechunk[chunk_index * xal->sb.inodesize];
			uint64_t is_unused = (rec->holemask & (1ULL << chunk_index)) >> chunk_index;
			uint64_t is_free = (rec->free & (1ULL << chunk_index)) >> chunk_index;

			if (is_unused || is_free) {
				continue;
			}

			memcpy((void *)(&xal->dinodes[*index * xal->sb.inodesize]),
			       (void *)chunk_cursor, xal->sb.inodesize);
			*index += 1;
		}
	}

	if (iab3->rightsib != 0xFFFFFFFF) {
		printf("Going deeper on the right\n");
		retrieve_dinodes_via_iabt3(xal, ag, iab3->rightsib, index);
	}

	return 0;
}

int
xal_dinodes_retrieve(struct xal *xal)
{
	uint64_t index = 0;

	xal->dinodes = calloc(1, xal->sb.nallocated * xal->sb.inodesize);
	if (!xal->dinodes) {
		return -errno;
	}

	for (uint32_t seqno = 0; seqno < xal->sb.agcount; ++seqno) {
		struct xal_ag *ag = &xal->ags[seqno];

		retrieve_dinodes_via_iabt3(xal, ag, ag->agi_root, &index);
	}

	return 0;
}

int
xal_index(struct xal *xal, struct xal_inode **index)
{
	struct xal_inode *root;
	int err;

	xal_dinodes_retrieve(xal);

	err = xal_pool_claim(&xal->pool, 1, &root);
	if (err) {
		return err;
	}

	root->ino = xal->sb.rootino;
	root->ftype = XAL_ODF_DIR3_FT_DIR;
	root->namelen = 1;
	root->nextents = 0;
	memcpy(root->name, "/", 1);

	*index = root;

	return process_inode_ino(xal, root->ino, root);
}

int
xal_walk(struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data)
{
	if (cb_func) {
		cb_func(inode, cb_data);
	}

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR: {
		struct xal_inode *children = inode->children;

		for (uint16_t i = 0; i < inode->nchildren; ++i) {
			xal_walk(&children[i], cb_func, cb_data);
		}
	} break;

	case XAL_ODF_DIR3_FT_REG_FILE:
		return 0;

	default:
		printf("Unknown / unsupported ftype: %d", inode->ftype);
		return -EINVAL;
	}

	return 0;
}
