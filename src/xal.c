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
#include <xal_odf.h>
#include <xal_pool.h>
#include <xal_pp.h>

#define BUF_NBYTES 4096 * 32UL	     ///< Number of bytes in a buffer
#define CHUNK_NINO 64		     ///< Number of inodes in a chunk
#define BUF_BLOCKSIZE 4096	     ///< Number of bytes in a block
#define BLOCK_MAX_NBYTES 16UL * 1024 ///< This is utilized for stack-variables; max 16K blocksizes

int
decode_dentry(void *buf, struct xal_inode *dentry);

static void
btree_block_meta(struct xal *xal, size_t *maxrecs, size_t *keys, size_t *pointers)
{
	size_t hdr_nbytes = sizeof(struct xal_odf_btree_lfmt);
	size_t mrecs = (xal->sb.blocksize - hdr_nbytes) / 16;

	if (maxrecs) {
		*maxrecs = mrecs;
	}
	if (keys) {
		*keys = hdr_nbytes;
	}
	if (pointers) {
		*pointers = hdr_nbytes + mrecs * 8;
	}
}

void
decode_xfs_extent(uint64_t l0, uint64_t l1, struct xal_extent *extent)
{
	// Extract start offset (l0:9-62)
	extent->start_offset = (l0 << 1) >> 10;

	// Extract start block (l0:0-8 and l1:21-63)
	extent->start_block = ((l0 & 0x1FF) << 43) | (l1 >> 21);

	// Extract block count (l1:0-20)
	extent->nblocks = l1 & 0x1FFFFF;

	extent->flag = l1 >> 63;
}

/**
 * Find the dinode with inode number 'ino'
 *
 * @return On success, 0 is returned. On error, -errno is returned to indicate the error.
 */
int
dinodes_get(struct xal *xal, uint64_t ino, void **dinode)
{
	for (uint64_t idx = 0; idx < xal->sb.nallocated; ++idx) {
		struct xal_odf_dinode *cand = (void *)(xal->dinodes + idx * xal->sb.inodesize);

		if (be64toh(cand->ino) != ino) {
			continue;
		}

		/** Halt when the dinode is invalid */
		assert(cand->di_magic == 0x4E49);

		*dinode = cand;

		return 0;
	}

	return -EINVAL;
}

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

uint64_t
xal_ino_decode_absolute_offset(struct xal *xal, uint64_t ino)
{
	uint32_t seqno, agbno, agbino;
	uint64_t offset;

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

	xnvme_buf_free(xal->dev, xal->buf);
	xal_pool_unmap(&xal->inodes);
	xal_pool_unmap(&xal->extents);
	free(xal->dinodes);
	free(xal);
}

int
_pread(struct xnvme_dev *dev, void *buf, size_t count, off_t offset)
{
	struct xnvme_cmd_ctx ctx = xnvme_cmd_ctx_from_dev(dev);
	const struct xnvme_geo *geo = xnvme_dev_get_geo(dev);
	int err;

	if (count > geo->mdts_nbytes) {
		XAL_DEBUG("FAILED: _pread(...) -- count(%zu) > mdts_nbytes(%" PRIu32 ")", count,
			  geo->mdts_nbytes);
		return -EINVAL;
	}
	if (count % geo->lba_nbytes) {
		XAL_DEBUG("FAILED: _pread(...) -- unaligned count(%zu);", count);
		return -EINVAL;
	}
	if (offset % geo->lba_nbytes) {
		XAL_DEBUG("FAILED: _pread(...) -- unaligned offset(%zu);", offset);
		return -EINVAL;
	}

	memset(buf, 0, count);

	err = xnvme_nvm_read(&ctx, xnvme_dev_get_nsid(dev), offset / geo->lba_nbytes,
			     (count / geo->lba_nbytes) - 1, buf, NULL);
	if (err || xnvme_cmd_ctx_cpl_status(&ctx)) {
		XAL_DEBUG("FAILED: xnvme_nvm_read(...);");
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
 * - The superblock (xal.sb) is initiatialized
 *
 * @param buf IO buffer, sufficiently large to hold a block of data
 * @param seqno The sequence number of the allocation group aka agno
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
retrieve_and_decode_allocation_group(struct xnvme_dev *dev, void *buf, uint32_t seqno,
				     struct xal *xal)
{
	uint8_t *cursor = buf;
	off_t offset = (off_t)seqno * (off_t)xal->sb.agblocks * (off_t)xal->sb.blocksize;
	struct xal_odf_agi *agi = (void *)(cursor + xal->sb.sectsize * 2);
	struct xal_odf_agf *agf = (void *)(cursor + xal->sb.sectsize);
	int err;

	err = _pread(dev, buf, xal->sb.sectsize * 4, offset);
	if (err) {
		XAL_DEBUG("FAILED: _pread()");
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
 * Retrieve the superblock from disk and decode the on-disk-format and allocate 'xal' instance
 *
 * This will allocate the memory backing 'xal'
 *
 * @param dev Pointer to device instance
 * @param buf IO buffer, sufficiently large to hold a block of data
 * @param xal Double-pointer to the xal
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
retrieve_and_decode_primary_superblock(struct xnvme_dev *dev, void *buf, struct xal **xal)
{
	const struct xal_odf_sb *psb = buf;
	struct xal *cand;
	uint32_t agcount;
	int err;

	err = _pread(dev, buf, 4096, 0);
	if (err) {
		XAL_DEBUG("FAILED: _pread()\n");
		return -errno;
	}

	agcount = be32toh(psb->agcount);

	cand = calloc(1, sizeof(*cand) + sizeof(*(cand->ags)) * agcount);
	if (!cand) {
		XAL_DEBUG("FAILED: realloc()\n");
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
	cand->sb.agcount = agcount;

	*xal = cand;

	return 0;
}

int
xal_open(struct xnvme_dev *dev, struct xal **xal)
{
	struct xal *cand;
	void *buf;
	int err;

	buf = xnvme_buf_alloc(dev, BUF_NBYTES);
	if (!buf) {
		XAL_DEBUG("FAILED: xnvme_buf_alloc()");
		return -errno;
	}

	err = retrieve_and_decode_primary_superblock(dev, buf, &cand);
	if (err) {
		XAL_DEBUG("FAILED: retrieve_and_decode_primary_superblock()");
		xnvme_buf_free(dev, buf);
		return -errno;
	}

	cand->dev = dev;
	cand->buf = buf;

	for (uint32_t seqno = 0; seqno < cand->sb.agcount; ++seqno) {
		err = retrieve_and_decode_allocation_group(dev, buf, seqno, cand);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_and_decode_allocation_group(inodes); err(%d)",
				  err);
			goto failed;
		}

		cand->sb.nallocated += cand->ags[seqno].agi_count;
	}

	err =
	    xal_pool_map(&cand->inodes, 40000000UL, cand->sb.nallocated, sizeof(struct xal_inode));
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_map(inodes); err(%d)", err);
		goto failed;
	}

	err = xal_pool_map(&cand->extents, 40000000UL, cand->sb.nallocated,
			   sizeof(struct xal_extent));
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_map(extents); err(%d)", err);
		goto failed;
	}

	*xal = cand; // All is good; promote the candidate

	return 0;

failed:
	xal_close(cand);

	return err;
}

int
process_ino(struct xal *xal, uint64_t ino, struct xal_inode *self);

uint64_t
getPhysicalblockFromFs(struct xal *xal, uint64_t fsblock)
{
	int agnum = fsblock >> xal->sb.agblklog;
	uint64_t blknum = fsblock & ((1 << xal->sb.agblklog) - 1);
	uint64_t physicalblknum = agnum * xal->sb.agblocks + blknum;
	return physicalblknum;
}

int
readLeafData(struct xal *xal, struct xal_inode *self, uint64_t bmbtptrs,
	     struct xal_btree_lblock *lfd)
{
	int err = 0;

	uint64_t block_number = bmbtptrs;
	uint64_t physicalblk = getPhysicalblockFromFs(xal, block_number);
	uint64_t block_offset = (physicalblk * xal->sb.blocksize);

	uint8_t *buf;
	buf = xnvme_buf_alloc(xal->dev, BUF_NBYTES);
	if (!buf) {
		XAL_DEBUG("FAILED: xnvme_buf_alloc(); errno(%d)", errno);
		return -errno;
	}

	uint8_t *block_databuf;
	block_databuf = xnvme_buf_alloc(xal->dev, BUF_NBYTES);
	if (!buf) {
		XAL_DEBUG("FAILED: xnvme_buf_alloc(); errno(%d)", errno);
		return -errno;
	}

	err = _pread(xal->dev, block_databuf, xal->sb.blocksize, block_offset);
	if (err) {
		XAL_DEBUG("FAILED: _pread()");
		goto exit;
	}

	uint8_t *cursor = (void *)block_databuf;
	cursor += sizeof(struct xal_btree_lblock); ///< Advance past lblock
	struct xal_extent extent = {0};
	uint64_t l0, l1;

	for (int iter = 0; iter < be16toh(lfd->btree_numrecs); iter++) {
		l0 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		l1 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		decode_xfs_extent(l0, l1, &extent);

		for (size_t blk = 0; blk < extent.nblocks; ++blk) {
			physicalblk = getPhysicalblockFromFs(xal, extent.start_block + blk);
			size_t ofz_disk = (physicalblk)*xal->sb.blocksize;
			struct xfs_odf_dir_blk_hdr *hdr = (void *)(buf);

			err = _pread(xal->dev, buf, xal->sb.blocksize, ofz_disk);
			if (err) {
				XAL_DEBUG("FAILED: !_pread(directory-extent)");
				goto exit;
			}

			if ((be32toh(hdr->magic) != XAL_ODF_DIR3_DATA_MAGIC) &&
			    (be32toh(hdr->magic) != XAL_ODF_DIR3_BLOCK_MAGIC)) {
				continue;
			}
			for (uint64_t ofz = 64; ofz < xal->sb.blocksize;) {

				uint8_t *dentry_cursor = buf + ofz;
				struct xal_inode dentry = {0};
				ofz += decode_dentry(dentry_cursor, &dentry);
				/**
				 * Seems like the only way to determine that there are no more
				 * entries are if one start to decode uinvalid entries.
				 * Such as a namelength of 0 or inode number 0.
				 * Thus, checking for that here.
				 **/
				if ((!dentry.ino) || (!dentry.namelen)) {
					continue;
				}
				/**
				 * Skip processing the mandatory dentries: '.' and '..'
				 */
				if ((dentry.namelen == 1) && (dentry.name[0] == '.')) {
					continue;
				}
				if ((dentry.namelen == 2) && (dentry.name[0] == '.') &&
				    (dentry.name[1] == '.')) {
					continue;
				}

				self->content.dentries.inodes[self->content.dentries.count] =
				    dentry;
				self->content.dentries.count += 1;
			}
		}
	}

exit:
	xnvme_buf_free(xal->dev, buf);
	xnvme_buf_free(xal->dev, block_databuf);

	return err;
}

int
readBlockData(struct xal *xal, void *buf, uint64_t block_number)
{
	int err;

	// Calculate the Physical blcok from FS Block
	uint64_t physicalblk = getPhysicalblockFromFs(xal, block_number);

	// Calculate the block offset
	off_t block_offset = physicalblk * xal->sb.blocksize;

	err = _pread(xal->dev, buf, xal->sb.blocksize, block_offset);
	if (err) {
		XAL_DEBUG("FAILED: _pread()\n");
		return -errno;
	}
	return 0;
}

/**
 * B+tree Directories decoding and inode population
 *
 * @see XFS Algorithms & Data Structures - 3rd Edition - 20.5 B+tree Directories" for details
 */
int
process_dinode_directory_btree(struct xal *xal, struct xal_odf_dinode *dinode,
			       struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint16_t numrec; // Number of records in this block
	uint64_t startoff[4];
	uint64_t bmbtptrs[4];

	int err;
	uint8_t *leafbuf[4];

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

	cursor += 2;
	numrec = be16toh(*((uint16_t *)cursor));

	cursor += 2;
	for (int i = 0; i < 4; i++) {
		startoff[i] = be64toh(*((uint64_t *)cursor));
		cursor += 8;
	}

	cursor += (16 * 8);
	for (int i = 0; i < 4; i++) {
		bmbtptrs[i] = be64toh(*((uint64_t *)cursor));
		cursor += 8;
	}

	// Allocate the buffer
	for (int i = 0; i < numrec; i++) {
		leafbuf[i] = xnvme_buf_alloc(xal->dev, BUF_NBYTES);
		if (!leafbuf[i]) {
			XAL_DEBUG("FAILED: xnvme_buf_alloc(); errno(%d)", errno);
			return -errno;
		}
	}

	err = xal_pool_claim_inodes(&xal->inodes, 1, &self->content.dentries.inodes);
	if (err) {
		XAL_DEBUG("FAILED: !xal_pool_claim_inodes(); err(%d)", err)
		goto exit;
	}

	for (int i = 0; i < numrec; i++) {
		struct xal_btree_lblock *lfd = (struct xal_btree_lblock *)leafbuf[i];
		readBlockData(xal, leafbuf[i], bmbtptrs[i]);
		readLeafData(xal, self, bmbtptrs[i], lfd);
	}
	return 0;

exit:

	for (int i = 0; i < numrec; i++) {
		if (leafbuf[i]) {
			xnvme_buf_free(xal->dev, leafbuf[i]);
		}
	}

	return err;
}

int
process_file_btree_leaf(struct xal *xal, uint64_t fsbno, struct xal_inode *self)
{
	uint64_t ofz = xal_fsbno_offset(xal, fsbno);
	struct xal_odf_btree_lfmt leaf = {0};
	uint16_t level, numrecs;
	uint64_t leftsib, rightsib;
	int err;

	XAL_DEBUG("ENTER:   fsbno(0x%" PRIx64 " @ %" PRIu64 ") in ino(0x%" PRIx64 ")", fsbno, ofz,
		  self->ino);

	err = _pread(xal->dev, xal->buf, xal->sb.blocksize, ofz);
	if (err) {
		XAL_DEBUG("FAILED: _pread(); err: %d", err);
		return err;
	}
	memcpy(&leaf, xal->buf, sizeof(leaf));

	if (XAL_ODF_BMAP_CRC_MAGIC != be32toh(leaf.magic.num)) {
		XAL_DEBUG("FAILED: expected magic(BMA3) got magic('%.4s', 0x%" PRIx32 "); ",
			  leaf.magic.text, leaf.magic.num);
		return -EINVAL;
	}

	level = be16toh(leaf.level);
	if (level != 0) {
		XAL_DEBUG("FAILED: expecting a leaf; got level(%" PRIu16 ")", level);
		return -EINVAL;
	}
	numrecs = be16toh(leaf.numrecs);
	leftsib = be64toh(leaf.leftsib);
	rightsib = be64toh(leaf.rightsib);

	XAL_DEBUG("INFO:    magic(%.4s, 0x%" PRIx32 ")", leaf.magic.text, leaf.magic.num);
	XAL_DEBUG("INFO:    level(%" PRIu16 ")", level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", numrecs);
	XAL_DEBUG("INFO:  leftsib(0x%016" PRIx64 " @ %" PRIu64 ")", leftsib,
		  xal_fsbno_offset(xal, leftsib));
	XAL_DEBUG("INFO:    fsbno(0x%016" PRIx64 " @ %" PRIu64 ")", fsbno, ofz);
	XAL_DEBUG("INFO: rightsib(0x%016" PRIx64 " @ %" PRIu64 ")", rightsib,
		  xal_fsbno_offset(xal, rightsib));

	err = xal_pool_claim_extents(&xal->extents, numrecs, NULL);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim_extents(); err(%d)", err);
		return err;
	}
	self->content.extents.count += numrecs;

	for (uint16_t rec = 0; rec < numrecs; ++rec) {
		size_t idx_ofz = self->content.extents.count - numrecs;
		struct xal_extent *extent = &self->content.extents.extent[idx_ofz + rec];
		uint8_t *cursor = xal->buf;
		uint64_t l0, l1;

		cursor += sizeof(leaf) + 16ULL * rec;

		l0 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		l1 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		decode_xfs_extent(l0, l1, extent);
	}

	XAL_DEBUG("EXIT");

	/**
	It seems like this is not needed. It looks like the parent node has all the siblings in the
	array of pointers, these left/right pointers **should** then point to each other, but they
	do not have to, and if both the parent invokes the leaf-decoder for each record, and the
	leaf-decoder then calls recursively, then each leaf is processed (n (n+1))/2 times, which is
	less than ideal :)
	It is left as a comment here, and removed entirely in the node-decoder.

	if (rightsib != 0xFFFFFFFFFFFFFFFF) {
		err = process_file_btree_leaf(xal, rightsib, self);
	}
	*/

	return err;
}

int
process_file_btree_node(struct xal *xal, uint64_t fsbno, struct xal_inode *self)
{
	uint64_t pointers[BLOCK_MAX_NBYTES / 8] = {0};

	uint64_t ofz = xal_fsbno_offset(xal, fsbno);

	struct xal_odf_btree_lfmt node = {0};
	size_t pointers_ofz;
	uint16_t level, numrecs;
	uint64_t leftsib, rightsib;
	size_t maxrecs;
	int err;

	XAL_DEBUG("ENTER:   fsbno(0x%" PRIx64 " @ %" PRIu64 ") in ino(0x%" PRIx64 ")", fsbno, ofz,
		  self->ino);

	if (xal->sb.blocksize > BLOCK_MAX_NBYTES) {
		XAL_DEBUG("FAILED: blocksize(%" PRIu32 ") > BLOCK_MAX_NBYTES(%" PRIu64 ")",
			  xal->sb.blocksize, BLOCK_MAX_NBYTES);
		return -EINVAL;
	}

	btree_block_meta(xal, &maxrecs, NULL, &pointers_ofz);

	XAL_DEBUG("INFO: maxrecs(%zu)", maxrecs);
	XAL_DEBUG("INFO: pointers_ofz(%zu)", pointers_ofz);

	err = _pread(xal->dev, xal->buf, xal->sb.blocksize, ofz);
	if (err) {
		XAL_DEBUG("FAILED: _pread(); err: %d", err);
		return err;
	}
	memcpy(&node, xal->buf, sizeof(node));
	memcpy(&pointers, xal->buf + pointers_ofz, xal->sb.blocksize - pointers_ofz);

	if (XAL_ODF_BMAP_CRC_MAGIC != be32toh(node.magic.num)) {
		XAL_DEBUG("FAILED: expected magic(BMA3) got magic('%.4s', 0x%" PRIx32 "); ",
			  node.magic.text, node.magic.num);
		return -EINVAL;
	}

	level = be16toh(node.level);
	if (!level) {
		XAL_DEBUG("FAILED: expecting a node; got level(%" PRIu16 ")", level);
		return -EINVAL;
	}
	numrecs = be16toh(node.numrecs);
	leftsib = be64toh(node.leftsib);
	rightsib = be64toh(node.rightsib);

	XAL_DEBUG("INFO:    magic(%.4s, 0x%" PRIx32 ")", node.magic.text, node.magic.num);
	XAL_DEBUG("INFO:    level(%" PRIu16 ")", level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", numrecs);
	XAL_DEBUG("INFO:  leftsib(0x%016" PRIx64 " @ %" PRIu64 ")", leftsib,
		  xal_fsbno_offset(xal, leftsib));
	XAL_DEBUG("INFO:    fsbno(0x%016" PRIx64 " @ %" PRIu64 ")", fsbno, ofz);
	XAL_DEBUG("INFO: rightsib(0x%016" PRIx64 " @ %" PRIu64 ")", rightsib,
		  xal_fsbno_offset(xal, rightsib));

	XAL_DEBUG("#### Processing Pointers ###");
	for (uint16_t rec = 0; rec < numrecs; ++rec) {
		uint64_t pointer = be64toh(pointers[rec]);

		XAL_DEBUG("INFO:      ptr[%" PRIu16 "] = 0x%" PRIx64, rec, pointer);

		err = (level == 1) ? process_file_btree_leaf(xal, pointer, self)
				   : process_file_btree_node(xal, pointer, self);
		if (err) {
			XAL_DEBUG("FAILED: file FMT_BTREE ino(0x%" PRIx64 ") @ ofz(%" PRIu64 ")",
				  self->ino, xal_ino_decode_absolute_offset(xal, self->ino));
			return err;
		}
	}

	XAL_DEBUG("INFO: ---===={[ EXIT: process_file_btree_node() ====---");

	return err;
}

/**
 * Derive the values needed to decode the records of a btree-root-node embedded in a dinode
 *
 * @param xal The xal instance
 * @param dinode The dinode in question
 * @param maxrecs Optional Maximum number of records in the dinode
 * @param keys Optional pointer to store dinode-offset to keys
 * @param pointers Optional pointer to store dinode-offset to pointers
 */
static void
btree_dinode_meta(struct xal *xal, struct xal_odf_dinode *dinode, size_t *maxrecs, size_t *keys,
		  size_t *pointers)
{
	size_t core_nbytes = sizeof(*dinode);
	size_t attr_ofz = core_nbytes + dinode->di_forkoff * 8UL;
	size_t attr_nbytes = xal->sb.inodesize - attr_ofz;
	size_t data_nbytes = xal->sb.inodesize - core_nbytes - attr_nbytes;
	size_t mrecs = (data_nbytes - 4) / 16;

	if (maxrecs) {
		*maxrecs = mrecs;
	}
	if (keys) {
		*keys = core_nbytes + 2 + 2;
	}
	if (pointers) {
		*pointers = core_nbytes + 2 + 2 + mrecs * 8;
	}
}

/**
 * B+tree Extent List decoding and inode population
 *
 * @see XFS Algorithms & Data Structures - 3rd Edition - 19.2 B+tree Extent List" for details
 *
 * Assumptions
 * ===========
 *
 * - Keys and pointers within the inode are 64 bits wide
 */
int
process_file_btree_root(struct xal *xal, struct xal_odf_dinode *dinode, struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint16_t level;	  // Level in the btree, expecting >= 1
	uint16_t numrecs; // Number of records in the inode itself
	size_t ofz_ptr;	  // Offset from start of dinode to start of embedded pointers
	int err;

	XAL_DEBUG("ENTER: ino(0x%" PRIx64 " @ %" PRIu64 ")", self->ino,
		  xal_ino_decode_absolute_offset(xal, self->ino));

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

	level = be16toh(*((uint16_t *)cursor));
	cursor += 2;

	numrecs = be16toh(*((uint16_t *)cursor));
	cursor += 2;

	if (level < 1) {
		XAL_DEBUG("FAILED: level(%" PRIu16 "); expected > 0", level);
		return -EINVAL;
	}

	XAL_DEBUG("INFO:    level(%" PRIu16 ")", level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", numrecs);

	/**
	The keys are great if we were seeking to a given position within the file, however, since
	we just want all the extent info, then we have no use for the keys except for debugging /
	sanity checking during developement.
	For example, comparing the decoded keys against the keys parsed by 'xfs_db' and when
	inspecting the disk via hexdump, then one can look for the key-values in the data-dump.
	 */
	for (uint16_t rec = 0; rec < numrecs; ++rec) {
		uint64_t key = be64toh(*((uint64_t *)cursor));
		cursor += sizeof(uint64_t);

		XAL_DEBUG("INFO:      key[%" PRIu16 "] = %" PRIu64, rec, key);
	}

	/**
	For some reason, there is an unexplained 64-byte gap between the last key and the start of
	the pointers. This was observed in a dinode with level 1. At level 2, different offsets were
	observed.
	When inspecting the data on disk then there is a bunch of zeroes... after fiddling
	then it looks like some kind of padding is happening... the amount of zeroes depended on
	numrec, the more records, the less zeroes.

	Looking at a disk with hexdump, then looking at a start-sequence starting 'level', then 192
	bytes later then the file-attributes start. This kinda looks like there is allocated 3 x 64
	bytes to: level, numrecs, keys, ptrs.

	Thus, although the XFS-documentation visualizes it as 'pointers' start right after 'keys',
	then it looks more like some padded and aligned struct:

	struct foo {
		uint16_t level;
		uint16_t numrecs;
		uint64_t keys[?];
		uint64_t pointers[?];
	}

	Another observation is that the first pointer seem to start af 96 bytes. This is exactly
	half of the 3x64. Since there has to be room for level and numrecs, then it would seem like
	a maxrecs would be something like: floor(((3 * 64 / 2) - 4) / 8) = 11.

	So, will attempt to use it as offset from the last the key to the first pointer.
	Specifically: "cursor += (maxrecs - numrecs) * 8;"... no nothing adds up here...

	Ohhh! There is a 'di_forkoff' in the dinode! Just RTFM Chapter 18 On-disk Inode.

	core_offset: 176 bytes
	attr_offset: di_forkoff (8bit value that needs to be multiplied)
	Then the amount of bytes for records become:

	nbytes = be8toh(di_forkoff) * 8 - core_offset?
	*/

	btree_dinode_meta(xal, dinode, NULL, NULL, &ofz_ptr);

	// Let's try resetting the cursor...
	cursor = (uint8_t *)dinode + ofz_ptr;

	/**
	We will need a lot more extents, however, we start by claiming one, such that the inode
	gets its content initialized. Subsequent claims will be growing from that point on. This is
	of course inherently non-thread-safe, so should the decoding at any point be re-implemented
	in parallel, then this is one of the main things to handle.
	*/
	err = xal_pool_claim_extents(&xal->extents, 1, &self->content.extents.extent);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim_extents(); err(%d)", err);
		return err;
	}
	self->content.extents.count = 1;

	XAL_DEBUG("#### Processing Pointers ###");
	for (uint16_t rec = 0; rec < numrecs; ++rec) {
		uint64_t pointer = be64toh(*((uint64_t *)cursor));

		cursor += sizeof(pointer);

		XAL_DEBUG("INFO:      ptr[%" PRIu16 "] = 0x%" PRIx64, rec, pointer);

		err = (level == 1) ? process_file_btree_leaf(xal, pointer, self)
				   : process_file_btree_node(xal, pointer, self);
		if (err) {
			XAL_DEBUG("FAILED: file FMT_BTREE ino(0x%" PRIx64 " @ %" PRIu64 ")",
				  self->ino, xal_ino_decode_absolute_offset(xal, self->ino));
			return err;
		}
	}

	XAL_DEBUG("EXIT")

	return 0;
}

/**
 * Short Form Directories decoding and inode population
 *
 * @see XFS Algorithms & Data Structures - 3rd Edition - 20.1 Short Form Directories
 */
int
process_dinode_inline_shortform_dentries(struct xal *xal, struct xal_odf_dinode *dinode,
					 struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint8_t count, i8count;
	int err;

	XAL_DEBUG("INFO: Short Form Directories ino(0x%" PRIx64 ")", be64toh(dinode->ino));

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

	count = *cursor;
	cursor += 1; ///< Advance past count

	i8count = *cursor;
	cursor += 1; ///< Advance past i8count

	cursor += i8count ? 8 : 4; ///< Advance past parent inode number

	self->content.dentries.count = count;

	err = xal_pool_claim_inodes(&xal->inodes, count, &self->content.dentries.inodes);
	if (err) {
		return err;
	}

	/** DECODE: namelen[1], offset[2], name[namelen], ftype[1], ino[4] | ino[8] */
	for (int i = 0; i < count; ++i) {
		struct xal_inode *dentry = &self->content.dentries.inodes[i];

		dentry->namelen = *cursor;
		cursor += 1 + 2; ///< Advance past 'namelen' and 'offset[2]'

		memcpy(dentry->name, cursor, dentry->namelen);
		cursor += dentry->namelen; ///< Advance past 'name'

		dentry->ftype = *cursor;
		cursor += 1; ///< Advance past 'ftype'

		if (i8count) {
			i8count--;
			dentry->ino = be64toh(*(uint64_t *)cursor);
			cursor += 8; ///< Advance past 64-bit inode number
		} else {
			dentry->ino = be32toh(*(uint32_t *)cursor);
			cursor += 4; ///< Advance past 32-bit inode number
		}
	}

	for (int i = 0; i < count; ++i) {
		struct xal_inode *dentry = &self->content.dentries.inodes[i];
		err = process_ino(xal, dentry->ino, dentry);
		if (err) {
			XAL_DEBUG("FAILED: process_ino()");
			return err;
		}
	}

	return 0;
}

int
process_dinode_inline_file_extents(struct xal *xal, struct xal_odf_dinode *dinode,
				   struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint64_t nextents;
	int err;

	XAL_DEBUG("INFO: Inline File Extents ino(0x%" PRIx64 ")", be64toh(dinode->ino));

	/**
	 * For some reason then di_big_nextents is populated. As far as i understand that should
	 * not happen for format=0x2 "extents" as this should have all extent-records inline in the
	 * inode. Thus this abomination... just grabbing whatever has a value...
	 */
	nextents =
	    (dinode->di_nextents) ? be32toh(dinode->di_nextents) : be64toh(dinode->di_big_nextents);

	err = xal_pool_claim_extents(&xal->extents, nextents, &self->content.extents.extent);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim()...");
		return err;
	}

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

	for (uint64_t i = 0; i < nextents; ++i) {
		uint64_t l0, l1;

		l0 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		l1 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		decode_xfs_extent(l0, l1, &self->content.extents.extent[i]);
	}

	return 0;
}

/**
 * Decode the dentry starting at the given buffer
 *
 * @return The size, in bytes and including alignment padding, of the decoded directory entry.
 */
int
decode_dentry(void *buf, struct xal_inode *dentry)
{
	uint8_t *cursor = buf;
	uint16_t nbytes = 8 + 1 + 1 + 2;

	// xfs dir unused entries case with freetag value as 0xffff
	uint16_t freetag = be16toh(*(uint16_t *)cursor);
	if (freetag == 0xffff) {
		cursor += 2; // Advance length of uint16_t freetag
		uint16_t length = be16toh(*(uint16_t *)cursor);
		nbytes = length;
		return nbytes;
	}

	// xfs dir data entry case
	dentry->ino = be64toh(*(uint64_t *)cursor);
	cursor += 8;

	dentry->namelen = *cursor;
	cursor += 1;

	memcpy(dentry->name, cursor, dentry->namelen);
	cursor += dentry->namelen;

	dentry->ftype = *cursor;
	cursor += 1;

	// NOTE: Read 2byte tag is skipped
	// cursor += 2;

	nbytes += dentry->namelen;
	nbytes = ((nbytes + 7) / 8) * 8; ///< Ensure alignment to 8 byte boundary

	return nbytes;
}

/**
 * Processing a multi-block directory with extents in inline format
 * ================================================================
 *
 * - Extract and decode the extents embedded within the dinode
 *
 *   - Unlike data-extents, then these directory-extents will not be stored the tree
 *
 * - Retrieve the blocks, from disk, described by the extents
 *
 * - Decode the directory entry-descriptions into 'xal_inode'
 *
 *   - Initially setting up 'self.content.dentries.inodes'
 *   - Incrementing 'self.content.dentries.count'
 *   - WARNING: When this is running, then no one else should be claiming memory from the pool
 *
 * An upper bound on extents
 * -------------------------
 *
 * There is an upper-bound of how many extents there can be in this case, that is, the amount that
 * can reside inside the inode. An approximation to this amount is:
 *
 *   nextents_max = (xal.sb.inodesize - header) / 16
 *
 * Thus, with an inode-size of 512 and the dinode size of about 176, then there is room for at most
 * 21 extents.
 *
 * An upper bound on directory entries
 * -----------------------------------
 *
 * An extent can describe a very large range of blocks, thus, it seems like there is not a trivial
 * way to put a useful upper-bound on it. E.g. even with a very small amount of extents, then each
 * of these have a 'count' of blocks. This 200bits worth of blocks... thats an awful lot of blocks.
 */
int
process_dinode_inline_directory_extents(struct xal *xal, struct xal_odf_dinode *dinode,
					struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint64_t nextents;
	uint8_t *buf;
	int err;

	XAL_DEBUG("INFO: Inline Directory Extents ino(0x%" PRIx64 ")", be64toh(dinode->ino));

	buf = xnvme_buf_alloc(xal->dev, BUF_NBYTES);
	if (!buf) {
		XAL_DEBUG("FAILED: xnvme_buf_alloc(); errno(%d)", errno);
		return -errno;
	}

	/**
	 * For some reason then di_big_nextents is populated. As far as i understand that should
	 * not happen for format=0x2 "extents" as this should have all extent-records inline in the
	 * inode. Thus this abomination... just grabbing whatever has a value...
	 */
	nextents =
	    (dinode->di_nextents) ? be32toh(dinode->di_nextents) : be64toh(dinode->di_big_nextents);

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

	/**
	 * A single inode is claimed, this is to get the pointer to the start of the array,
	 * additional calls to claim will be called as extents/dentries are decoded, however, only
	 * the first call provides a pointer, since the start of the array, that consists of all
	 * the children is only rooted once.
	 */

	err = xal_pool_claim_inodes(&xal->inodes, 1, &self->content.dentries.inodes);
	if (err) {
		XAL_DEBUG("FAILED: !xal_pool_claim_inodes(); err(%d)", err)
		goto exit;
	}

	for (uint64_t i = 0; i < nextents; ++i) {
		struct xal_extent extent = {0};
		uint64_t l0, l1;

		l0 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		l1 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		decode_xfs_extent(l0, l1, &extent);

		for (size_t blk = 0; blk < extent.nblocks; ++blk) {
			size_t ofz_disk = (extent.start_block + blk) * xal->sb.blocksize;
			struct xfs_odf_dir_blk_hdr *hdr = (void *)(buf);

			err = _pread(xal->dev, buf, xal->sb.blocksize, ofz_disk);
			if (err) {
				XAL_DEBUG("FAILED: !_pread(directory-extent)");
				goto exit;
			}
			if ((be32toh(hdr->magic) != XAL_ODF_DIR3_DATA_MAGIC) &&
			    (be32toh(hdr->magic) != XAL_ODF_DIR3_BLOCK_MAGIC)) {
				continue;
			}

			for (uint64_t ofz = 64; ofz < xal->sb.blocksize;) {
				uint8_t *dentry_cursor = buf + ofz;
				struct xal_inode dentry = {0};

				ofz += decode_dentry(dentry_cursor, &dentry);

				/**
				 * Seems like the only way to determine that there are no more
				 * entries are if one start to decode uinvalid entries.
				 * Such as a namelength of 0 or inode number 0.
				 * Thus, checking for that here.
				 */
				if ((!dentry.ino) || (!dentry.namelen)) {
					break;
				}

				/**
				 * Skip processing the mandatory dentries: '.' and '..'
				 */
				if ((dentry.namelen == 1) && (dentry.name[0] == '.')) {
					continue;
				}
				if ((dentry.namelen == 2) && (dentry.name[0] == '.') &&
				    (dentry.name[1] == '.')) {
					continue;
				}

				self->content.dentries.inodes[self->content.dentries.count] =
				    dentry;

				err = process_ino(
				    xal,
				    self->content.dentries.inodes[self->content.dentries.count].ino,
				    &self->content.dentries.inodes[self->content.dentries.count]);
				if (err) {
					XAL_DEBUG("FAILED: process_ino(...)")
					goto exit;
				}

				self->content.dentries.count += 1;

				err = xal_pool_claim_inodes(&xal->inodes, 1, NULL);
				if (err) {
					XAL_DEBUG("FAILED: xal_pool_claim_inodes(...)");
					goto exit;
				}
			}
		}
	}

exit:
	xnvme_buf_free(xal->dev, buf);

	return err;
}

/**
 * Internal helper recursively traversing the on-disk-format to build an index of the file-system
 *
 * - Retrieve the dinode
 * -
 *
 */
int
process_ino(struct xal *xal, uint64_t ino, struct xal_inode *self)
{
	struct xal_odf_dinode *dinode;
	int err;

	err = dinodes_get(xal, ino, (void **)&dinode);
	if (err) {
		XAL_DEBUG("FAILED: dinodes_get();");
		return err;
	}

	if (!self->ftype) {
		uint16_t mode = be16toh(dinode->di_mode);

		if (S_ISDIR(mode)) {
			self->ftype = XAL_ODF_DIR3_FT_DIR;
		} else if (S_ISREG(mode)) {
			self->ftype = XAL_ODF_DIR3_FT_REG_FILE;
		} else {
			return -EINVAL;
		}
	}

	self->size = be64toh(dinode->di_size);
	self->ino = be64toh(dinode->ino);

	switch (dinode->di_format) {
	case XAL_DINODE_FMT_BTREE:
		switch (self->ftype) {
		case XAL_ODF_DIR3_FT_DIR:
			err = process_dinode_directory_btree(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_directory_btree(); err(%d)", err);
				return err;
			}
			break;

		case XAL_ODF_DIR3_FT_REG_FILE:
			err = process_file_btree_root(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_file_btree(); err(%d)", err);
				return err;
			}
			break;

		default:
			XAL_DEBUG("FAILED: Unsupported file-type in BTREE fmt");
			return -ENOSYS;
		}
		break;

	case XAL_DINODE_FMT_EXTENTS:
		switch (self->ftype) {
		case XAL_ODF_DIR3_FT_DIR:
			err = process_dinode_inline_directory_extents(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_inline_directory_extent()");
				return err;
			}
			break;

		case XAL_ODF_DIR3_FT_REG_FILE:
			err = process_dinode_inline_file_extents(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_inline_file_extents()");
				return err;
			}
			break;

		default:
			XAL_DEBUG("FAILED: Unsupported file-type in EXTENTS fmt");
			return -ENOSYS;
		}
		break;

	case XAL_DINODE_FMT_LOCAL: ///< Decode directory listing in inode
		switch (self->ftype) {
		case XAL_ODF_DIR3_FT_DIR:
			err = process_dinode_inline_shortform_dentries(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_inline_shortform_dentries()");
				return err;
			}
			break;

		case XAL_ODF_DIR3_FT_REG_FILE:
			XAL_DEBUG("FAILED: file in LOCAL fmt -- not implemented.");
			return -ENOSYS;

		default:
			XAL_DEBUG("FAILED: Unsupported file-type in BTREE fmt");
			return -ENOSYS;
		}
		break;

	case XAL_DINODE_FMT_DEV:
	case XAL_DINODE_FMT_UUID:
		XAL_DEBUG("FAILED: Unsupported FMT_DEV or FMT_UUID");
		return -ENOSYS;
	}

	return 0;
}

/**
 * Retrieve all the allocated inodes stored within the given allocation group
 *
 * It is assumed that the inode-allocation-b+tree is rooted at the given 'blkno'
 */
int
retrieve_dinodes_via_iabt3(struct xal *xal, struct xal_ag *ag, uint64_t blkno, uint64_t *index)
{
	struct xal_odf_btree_sfmt *iab3;
	off_t offset;
	uint8_t *inodechunk;
	char *buf;
	int err;

	buf = xnvme_buf_alloc(xal->dev, BUF_NBYTES);
	if (!buf) {
		XAL_DEBUG("FAILED: xnvme_buf_alloc(); errno(%d)", errno);
		return -errno;
	}

	inodechunk = xnvme_buf_alloc(xal->dev, BUF_NBYTES);
	if (!inodechunk) {
		XAL_DEBUG("FAILED: xnvme_buf_alloc(); errno(%d)", errno);
		err = -errno;
		goto exit;
	}

	/** Compute the absolute offset for the block and retrieve it **/
	offset = (xal->sb.agblocks * ag->seqno + blkno) * xal->sb.blocksize;
	err = _pread(xal->dev, buf, xal->sb.blocksize, offset);
	if (err) {
		XAL_DEBUG("FAILED: _pread()");
		goto exit;
	}

	iab3 = (void *)buf;
	iab3->magic.num = iab3->magic.num;
	iab3->level = be16toh(iab3->level);
	iab3->numrecs = be16toh(iab3->numrecs);
	iab3->leftsib = be32toh(iab3->leftsib);
	iab3->rightsib = be32toh(iab3->rightsib);
	iab3->blkno = be64toh(iab3->blkno);

	assert(be32toh(iab3->magic.num) == XAL_ODF_IBT_CRC_MAGIC);

	if (iab3->level) {
		XAL_DEBUG("INFO: iab3->level(%" PRIu16 ")?", iab3->level);
		return 0;
	}

	for (uint16_t reci = 0; reci < iab3->numrecs; ++reci) {
		struct xal_odf_inobt_rec *rec = (void *)(buf + sizeof(*iab3) + reci * sizeof(*rec));
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

			err = _pread(xal->dev, inodechunk, chunk_nbytes, chunk_offset);
			if (err) {
				XAL_DEBUG("FAILED: _pread(chunk)");
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
		XAL_DEBUG("INFO: Going deeper on the right!");
		retrieve_dinodes_via_iabt3(xal, ag, iab3->rightsib, index);
	}

exit:
	xnvme_buf_free(xal->dev, buf);
	xnvme_buf_free(xal->dev, inodechunk);

	return err;
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
xal_index(struct xal *xal)
{
	int err;

	if (!xal->dinodes) {
		return -EINVAL;
	}

	err = xal_pool_claim_inodes(&xal->inodes, 1, &xal->root);
	if (err) {
		return err;
	}

	xal->root->ino = xal->sb.rootino;
	xal->root->ftype = XAL_ODF_DIR3_FT_DIR;
	xal->root->namelen = 1;
	xal->root->content.extents.count = 0;
	xal->root->content.dentries.count = 0;
	xal->root->name[0] = '/';

	return process_ino(xal, xal->root->ino, xal->root);
}

int
_walk(struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data, int depth)
{
	if (cb_func) {
		cb_func(inode, cb_data, depth);
	}

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR: {
		struct xal_inode *inodes = inode->content.dentries.inodes;

		for (uint32_t i = 0; i < inode->content.dentries.count; ++i) {
			_walk(&inodes[i], cb_func, cb_data, depth + 1);
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
xal_walk(struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data)
{
	return _walk(inode, cb_func, cb_data, 0);
}

