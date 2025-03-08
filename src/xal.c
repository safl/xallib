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

#define BUF_NBYTES 4096

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

	printf("seqno: %" PRIu32 ", agbno: %" PRIu32 ", agbino: %" PRIu32 ", ino: %" PRIu64 "\n",
	       seqno, agbno, agbino, ino);

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
	free(xal);
}

int
read_4k(struct xal *xal, void *buf, off_t offset)
{
	ssize_t nbytes;

	memset(buf, 0, BUF_NBYTES);
	nbytes = pread(xal->handle.fd, buf, BUF_NBYTES, offset);
	if (nbytes != BUF_NBYTES) {
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

	err = read_4k(xal, buf, offset);
	if (err) {
		perror("read_4k();\n");
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

	err = read_4k(*xal, buf, 0);
	if (err) {
		perror("read_4k();\n");
		return -errno;
	}

	cand = realloc(*xal, sizeof(*cand) + sizeof(*(cand->ags)) * be32toh(psb->agcount));
	if (!cand) {
		perror("realloc();\n");
		return -errno;
	}

	printf("cand: %p, xal: %p, %p\n", (void*)cand, (void*)xal, (void*)*xal);

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
		// TODO: do error handling here
		err = retrieve_and_decode_allocation_group(seqno, buf, cand);
		if (err) {
			xal_close(cand);
			return err;
		}
	}

	// Setup inode memory-pool
	err = xal_pool_map(&cand->pool, 40000000UL, cand->sb.icount);
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

int
process_inode_btree(struct xal *xal, void *buf, struct xal_inode *self)
{
	struct xal_odf_dinode *dinode = buf;
	uint8_t *cursor = buf;

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
		process_inode_btree(xal, buf, self);
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
 * Observation(s)
 * ==============
 *
 * siblings
 * It looks like the inode-allocation-block-tree (iab3) uses the short-form for sibling block
 * numbers. This seems to make sense since the iab3 described inodes within the allocation-group,
 * and as such, need to only represent block numbers relative to the ag, and thus, only need 32bit
 * per sibling.
 *
 * 'blkno'
 * The 'blkno' field in the btree is an absolute block number, that is, unlike the block numbers of
 * siblings, then this 'blkno' is absolute. Also, this number is described in units of sectors!
 *
 * This wrinkles my brain but I guess there are good reasons to do so, must be something related
 * to quickly verifying that a block retrieved from disk using fs-wide addressed is the expected
 * block.
 *
 * The really convenient thing about this field is that it helped me understand that the siblings
 * where short-form. When looking at the data on disk, then I thought it has:
 *
 *  leftsibling = 0xFF...F
 *
 * and the rightsibling was block-address.
 */
int
preprocess_inodes_iabt3(struct xal *xal, struct xal_ag *ag, uint64_t blkno)
{
	char buf[BUF_NBYTES] = { 0 };
	struct xal_odf_btree_iab3_sfmt *iab3 = (void*)buf;
	off_t offset = blkno * xal->sb.blocksize;
	uint64_t ag_inode_count = 0;
	int err;

	if (blkno == ag->agi_root) {
		offset += ag->offset;
	}

	err = read_4k(xal, buf, offset);
	if (err) {
		perror("read_4k();\n");
		return err;
	}
	
	iab3->magic.num = iab3->magic.num;
	iab3->level = be16toh(iab3->level);
	iab3->numrecs = be16toh(iab3->numrecs);
	iab3->leftsib = be32toh(iab3->leftsib);
	iab3->rightsib = be32toh(iab3->rightsib);
	iab3->blkno = be64toh(iab3->blkno);

	printf("blkno(%" PRIu64 ", 0x%" PRIX64 "), offset(%" PRIu64 "): ", blkno, blkno, offset);
	xal_odf_btree_iab3_sfmt_pp(iab3);

	if (iab3->level) {
		return 0;
	}

	if (iab3->rightsib != 0xFFFFFFFF) {
		printf("Going deeper on the right\n");
		preprocess_inodes_iabt3(xal, ag, iab3->rightsib);
	}

	{
		for (uint16_t reci = 0; reci < iab3->numrecs; ++reci) {
			struct xal_odf_inobt_rec *rec =
			    (void *)(buf + sizeof(*iab3) + reci * sizeof(*rec));
			off_t chunk_offset;

			rec->startino = be32toh(rec->startino);
			rec->holemask = be16toh(rec->holemask);
			// count is 1 byte, so no be-conversion
			rec->free = be64toh(rec->free);

			xal_odf_inobt_rec_pp(rec);

			// The inode area starts at a fixed location, after eight blocks of
			// meta-data for sb, ag and the root nodes / blocks for agi, agf, and agfl
			chunk_offset = (ag->seqno * xal->sb.agblocks + 16) * xal->sb.blocksize;

			for (uint8_t chunk_index = 0; chunk_index < rec->count; ++chunk_index) {
				uint8_t inodebuf[BUF_NBYTES] = {0};
				uint32_t inorel = rec->startino + chunk_index;
				ssize_t nbytes;

				nbytes = pread(xal->handle.fd, inodebuf, xal->sb.sectsize,
					       chunk_offset + (chunk_index * xal->sb.inodesize) +
						   (rec->startino - xal->sb.rootino) *
						       xal->sb.inodesize);
				/// How does these things work!?
				if (nbytes != xal->sb.sectsize) {
					return -EIO;
				}

				ag_inode_count++;

				{
					uint32_t seqno_abs, agbno_abs, agbino_abs = 0;
					uint32_t seqno_rel = ag->seqno, agbno_rel, agbino_rel = 0;
					struct xal_odf_dinode *dinode = (void *)inodebuf;

					xal_ino_decode_absolute(xal, be64toh(dinode->ino),
								&seqno_abs, &agbno_abs,
								&agbino_abs);

					xal_ino_decode_relative(xal, inorel, &agbno_rel,
								&agbino_rel);

					printf("# startino: %" PRIu32 "\n", rec->startino);
					printf("# inorel: %" PRIu32 "\n",
					       ino_abs_to_rel(xal, be64toh(dinode->ino)));
					printf("# startino + index: %" PRIu32 "\n", inorel);
					xal_odf_dinode_pp(inodebuf);

					printf("seqno: ag(%" PRIu32 "), abs(%" PRIu32
					       "), rel(%" PRIu32 ")\n",
					       ag->seqno, seqno_abs, seqno_rel);
					printf("agbno: abs(%" PRIu32 "), rel(%" PRIu32 ")\n",
					       agbno_abs, agbno_rel);
					printf("agbino: abs(%" PRIu32 "), rel(%" PRIu32 ")\n",
					       agbino_abs, agbino_rel);
				}
			}
		}
	}

	printf("seqno: %" PRIu32 ", ag_inode_count: %" PRIu64 "\n", ag->seqno, ag_inode_count);

	return 0;
}

int
preprocess_inodes(struct xal *xal)
{
	for (uint32_t seqno = 0; seqno < xal->sb.agcount; ++seqno) {
		struct xal_ag *ag = &xal->ags[seqno];

		printf("# seqno: %" PRIu32 "\n", seqno);
		preprocess_inodes_iabt3(xal, ag, ag->agi_root);
	}

	return 0;
}

int
xal_index(struct xal *xal, struct xal_inode **index)
{
	struct xal_inode *root;
	int err;

	preprocess_inodes(xal);

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
