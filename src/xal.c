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

#define BUF_NBYTES 4096 * 32UL		    ///< Number of bytes in a buffer
#define CHUNK_NINO 64			    ///< Number of inodes in a chunk
#define BUF_BLOCKSIZE 4096		    ///< Number of bytes in a block
#define ODF_BLOCK_DIR_BYTES_MAX 64UL * 1024 ///< Maximum size of a directory block
#define ODF_BLOCK_FS_BYTES_MAX 64UL * 1024  ///< Maximum size of a filestem block
#define ODF_INODE_MAX_NBYTES 2048	    ///< Maximum size of an inode

struct pair_u64 {
	uint64_t l0;
	uint64_t l1;
};

int
decode_dentry(void *buf, struct xal_inode *dentry);

static void
btree_block_lfmt_meta(struct xal *xal, size_t *maxrecs, size_t *keys, size_t *pointers)
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

static void
btree_block_sfmt_meta(struct xal *xal, size_t *maxrecs, size_t *keys, size_t *pointers)
{
	size_t hdr_nbytes = sizeof(struct xal_odf_btree_sfmt);
	size_t mrecs = (xal->sb.blocksize - hdr_nbytes) / 8;

	if (maxrecs) {
		*maxrecs = mrecs;
	}
	if (keys) {
		*keys = hdr_nbytes;
	}
	if (pointers) {
		*pointers = hdr_nbytes + mrecs * 4;
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

	XAL_DEBUG("FAILED: no match for ino(0x%" PRIx64 ")", ino);
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

/**
 * Compute the absolute disk offset of the given agbno reltive to the ag with 'seqno'
 */
uint64_t
xal_agbno_absolute_offset(struct xal *xal, uint32_t seqno, uint32_t agbno)
{
	// Absolute Inode offset in bytes
	return (seqno * (uint64_t)xal->sb.agblocks + agbno) * xal->sb.blocksize;
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

static int
dev_read(struct xnvme_dev *dev, void *buf, size_t count, off_t offset)
{
	struct xnvme_cmd_ctx ctx = xnvme_cmd_ctx_from_dev(dev);
	const struct xnvme_geo *geo = xnvme_dev_get_geo(dev);
	int err;

	if (count > geo->mdts_nbytes) {
		XAL_DEBUG("FAILED: dev_read(...) -- count(%zu) > mdts_nbytes(%" PRIu32 ")", count,
			  geo->mdts_nbytes);
		return -EINVAL;
	}
	if (count % geo->lba_nbytes) {
		XAL_DEBUG("FAILED: dev_read(...) -- unaligned count(%zu);", count);
		return -EINVAL;
	}
	if (offset % geo->lba_nbytes) {
		XAL_DEBUG("FAILED: dev_read(...) -- unaligned offset(%zu);", offset);
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

static int
dev_read_into(struct xnvme_dev *dev, void *iobuf, size_t count, off_t offset, void *buf)
{
	int err;

	err = dev_read(dev, iobuf, count, offset);
	if (err) {
		XAL_DEBUG("FAILED: dev_read_into(); err(%d)", err);
		return err;
	}

	memcpy(buf, iobuf, count);

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

	err = dev_read(dev, buf, xal->sb.sectsize * 4, offset);
	if (err) {
		XAL_DEBUG("FAILED: dev_read()");
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

	err = dev_read(dev, buf, 4096, 0);
	if (err) {
		XAL_DEBUG("FAILED: dev_read()\n");
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
	cand->sb.dirblocksize = cand->sb.blocksize << psb->dirblklog;

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

	err = dev_read(xal->dev, block_databuf, xal->sb.blocksize, block_offset);
	if (err) {
		XAL_DEBUG("FAILED: dev_read()");
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

			err = dev_read(xal->dev, buf, xal->sb.blocksize, ofz_disk);
			if (err) {
				XAL_DEBUG("FAILED: !dev_read(directory-extent)");
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

	err = dev_read(xal->dev, buf, xal->sb.blocksize, block_offset);
	if (err) {
		XAL_DEBUG("FAILED: dev_read()\n");
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
process_dinode_dir_btree(struct xal *xal, struct xal_odf_dinode *dinode, struct xal_inode *self)
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
	int err;

	XAL_DEBUG("ENTER: File Extents -- B+Tree -- Leaf Node");

	err = dev_read(xal->dev, xal->buf, xal->sb.blocksize, ofz);
	if (err) {
		XAL_DEBUG("FAILED: dev_read(); err: %d", err);
		return err;
	}
	memcpy(&leaf, xal->buf, sizeof(leaf));

	if (XAL_ODF_BMAP_CRC_MAGIC != be32toh(leaf.magic.num)) {
		XAL_DEBUG("FAILED: expected magic(BMA3) got magic('%.4s', 0x%" PRIx32 "); ",
			  leaf.magic.text, leaf.magic.num);
		return -EINVAL;
	}

	leaf.level = be16toh(leaf.level);
	if (leaf.level != 0) {
		XAL_DEBUG("FAILED: expecting a leaf; got level(%" PRIu16 ")", leaf.level);
		return -EINVAL;
	}
	leaf.numrecs = be16toh(leaf.numrecs);
	leaf.leftsib = be64toh(leaf.leftsib);
	leaf.rightsib = be64toh(leaf.rightsib);

	XAL_DEBUG("INFO:    magic(%.4s, 0x%" PRIx32 ")", leaf.magic.text, leaf.magic.num);
	XAL_DEBUG("INFO:    level(%" PRIu16 ")", leaf.level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", leaf.numrecs);
	XAL_DEBUG("INFO:  leftsib(0x%016" PRIx64 " @ %" PRIu64 ")", leaf.leftsib,
		  xal_fsbno_offset(xal, leaf.leftsib));
	XAL_DEBUG("INFO:    fsbno(0x%016" PRIx64 " @ %" PRIu64 ")", fsbno, ofz);
	XAL_DEBUG("INFO: rightsib(0x%016" PRIx64 " @ %" PRIu64 ")", leaf.rightsib,
		  xal_fsbno_offset(xal, leaf.rightsib));

	err = xal_pool_claim_extents(&xal->extents, leaf.numrecs, NULL);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim_extents(); err(%d)", err);
		return err;
	}
	self->content.extents.count += leaf.numrecs;

	for (uint16_t rec = 0; rec < leaf.numrecs; ++rec) {
		size_t idx_ofz = self->content.extents.count - leaf.numrecs;
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

	return err;
}

int
process_file_btree_node(struct xal *xal, uint64_t fsbno, struct xal_inode *self)
{
	uint64_t pointers[ODF_BLOCK_FS_BYTES_MAX / 8] = {0};
	uint64_t ofz = xal_fsbno_offset(xal, fsbno);
	struct xal_odf_btree_lfmt node = {0};
	size_t pointers_ofz;
	size_t maxrecs;
	int err;

	XAL_DEBUG("ENTER: File Extents -- B+Tree -- Internal Node");

	if (xal->sb.blocksize > ODF_BLOCK_FS_BYTES_MAX) {
		XAL_DEBUG("FAILED: blocksize(%" PRIu32 ") > ODF_BLOCK_FS_BYTES_MAX(%" PRIu64 ")",
			  xal->sb.blocksize, ODF_BLOCK_FS_BYTES_MAX);
		return -EINVAL;
	}

	btree_block_lfmt_meta(xal, &maxrecs, NULL, &pointers_ofz);

	XAL_DEBUG("INFO: maxrecs(%zu)", maxrecs);
	XAL_DEBUG("INFO: pointers_ofz(%zu)", pointers_ofz);

	err = dev_read(xal->dev, xal->buf, xal->sb.blocksize, ofz);
	if (err) {
		XAL_DEBUG("FAILED: dev_read(); err: %d", err);
		return err;
	}
	memcpy(&node, xal->buf, sizeof(node));
	memcpy(&pointers, xal->buf + pointers_ofz, xal->sb.blocksize - pointers_ofz);

	if (XAL_ODF_BMAP_CRC_MAGIC != be32toh(node.magic.num)) {
		XAL_DEBUG("FAILED: expected magic(BMA3) got magic('%.4s', 0x%" PRIx32 "); ",
			  node.magic.text, node.magic.num);
		return -EINVAL;
	}

	node.level = be16toh(node.level);
	if (!node.level) {
		XAL_DEBUG("FAILED: expecting a node; got level(%" PRIu16 ")", node.level);
		return -EINVAL;
	}
	node.numrecs = be16toh(node.numrecs);
	node.leftsib = be64toh(node.leftsib);
	node.rightsib = be64toh(node.rightsib);

	XAL_DEBUG("INFO:    magic(%.4s, 0x%" PRIx32 ")", node.magic.text, node.magic.num);
	XAL_DEBUG("INFO:    level(%" PRIu16 ")", node.level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", node.numrecs);
	XAL_DEBUG("INFO:  leftsib(0x%016" PRIx64 " @ %" PRIu64 ")", node.leftsib,
		  xal_fsbno_offset(xal, node.leftsib));
	XAL_DEBUG("INFO:    fsbno(0x%016" PRIx64 " @ %" PRIu64 ")", fsbno, ofz);
	XAL_DEBUG("INFO: rightsib(0x%016" PRIx64 " @ %" PRIu64 ")", node.rightsib,
		  xal_fsbno_offset(xal, node.rightsib));

	XAL_DEBUG("#### Processing Pointers ###");
	for (uint16_t rec = 0; rec < node.numrecs; ++rec) {
		uint64_t pointer = be64toh(pointers[rec]);

		XAL_DEBUG("INFO:      ptr[%" PRIu16 "] = 0x%" PRIx64, rec, pointer);

		err = (node.level == 1) ? process_file_btree_leaf(xal, pointer, self)
					: process_file_btree_node(xal, pointer, self);
		if (err) {
			XAL_DEBUG("FAILED: file FMT_BTREE ino(0x%" PRIx64 ") @ ofz(%" PRIu64 ")",
				  self->ino, xal_ino_decode_absolute_offset(xal, self->ino));
			return err;
		}
	}

	XAL_DEBUG("EXIT");

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
process_dinode_file_btree_root(struct xal *xal, struct xal_odf_dinode *dinode,
			       struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint16_t level;	  // Level in the btree, expecting >= 1
	uint16_t numrecs; // Number of records in the inode itself
	size_t ofz_ptr;	  // Offset from start of dinode to start of embedded pointers
	int err;

	XAL_DEBUG("ENTER: File Extents -- B+Tree -- Root Node");

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
process_dinode_dir_local(struct xal *xal, struct xal_odf_dinode *dinode, struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint8_t count, i8count;
	int err;

	XAL_DEBUG("ENTER: Directory Entries -- Dinode Inline Shortform");

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

	count = *cursor;
	cursor += 1; ///< Advance past count

	i8count = *cursor;
	cursor += 1; ///< Advance past i8count

	cursor += i8count ? 8 : 4; ///< Advance past parent inode number

	self->content.dentries.count = count;

	err = xal_pool_claim_inodes(&xal->inodes, count, &self->content.dentries.inodes);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim_inodes(); err(%d)", err);
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

		dentry->parent = self;
		err = process_ino(xal, dentry->ino, dentry);
		if (err) {
			XAL_DEBUG("FAILED: process_ino()");
			return err;
		}
	}

	XAL_DEBUG("EXIT");
	return 0;
}

int
process_dinode_file_extents(struct xal *xal, struct xal_odf_dinode *dinode, struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint64_t nextents;
	int err;

	XAL_DEBUG("ENTER: File Extents -- Dinode Inline");

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

	XAL_DEBUG("EXIT");
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
 * Read a directory-block from disk and process the directory-entries within.
 */
int
process_dinode_dir_extents_dblock(struct xal *xal, uint64_t fsbno, struct xal_inode *self)
{
	uint8_t dblock[ODF_BLOCK_FS_BYTES_MAX] = {0};
	union xal_odf_btree_magic *magic = (void *)(dblock);
	size_t ofz_disk = xal_fsbno_offset(xal, fsbno);
	int err;

	XAL_DEBUG("ENTER");

	err = dev_read_into(xal->dev, xal->buf, xal->sb.dirblocksize, ofz_disk, dblock);
	if (err) {
		XAL_DEBUG("FAILED: !dev_read(directory-extent)");
		return err;
	}

	XAL_DEBUG("INFO: magic('%.4s', 0x%" PRIx32 "); ", magic->text, magic->num);

	if ((be32toh(magic->num) != XAL_ODF_DIR3_DATA_MAGIC) &&
	    (be32toh(magic->num) != XAL_ODF_DIR3_BLOCK_MAGIC)) {
		XAL_DEBUG("FAILED: looks like invalid magic value");
		return err;
	}

	for (uint64_t ofz = 64; ofz < xal->sb.dirblocksize;) {
		uint8_t *dentry_cursor = dblock + ofz;
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
		if ((dentry.namelen == 2) && (dentry.name[0] == '.') && (dentry.name[1] == '.')) {
			continue;
		}

		dentry.parent = self;
		self->content.dentries.inodes[self->content.dentries.count] = dentry;

		err = process_ino(xal,
				  self->content.dentries.inodes[self->content.dentries.count].ino,
				  &self->content.dentries.inodes[self->content.dentries.count]);
		if (err) {
			XAL_DEBUG("FAILED: process_ino(...)")
			return err;
		}

		self->content.dentries.count += 1;

		err = xal_pool_claim_inodes(&xal->inodes, 1, NULL);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_claim_inodes(...)");
			return err;
		}
	}

	XAL_DEBUG("EXIT");

	return 0;
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
process_dinode_dir_extents(struct xal *xal, struct xal_odf_dinode *dinode, struct xal_inode *self)
{
	struct pair_u64 *extents = (void *)(((uint8_t *)dinode) + sizeof(struct xal_odf_dinode));
	const uint32_t fsblk_per_dblk = xal->sb.dirblocksize / xal->sb.blocksize;
	uint64_t nextents = be32toh(dinode->di_nextents);
	int64_t nbytes = be64toh(dinode->size);
	int err;

	/**
	 * For some reason then di_big_nextents is populated. As far as i understand that should
	 * not happen for format=0x2 "extents" as this should have all extent-records inline in the
	 * inode. Thus this abomination... just grabbing whatever has a value...
	 */
	if (!nextents) {
		nextents = be64toh(dinode->di_big_nextents);
	}
	XAL_DEBUG("INFO:       nextents(%" PRIu64 ")", nextents);
	XAL_DEBUG("INFO: fsblk_per_dblk(%" PRIu32 ")", fsblk_per_dblk);
	XAL_DEBUG("INFO:         nbytes(%" PRIu64 ")", nbytes);
	/**
	 * A single inode is claimed, this is to get the pointer to the start of the array,
	 * additional calls to claim will be called as extents/dentries are decoded, however, only
	 * the first call provides a pointer, since the start of the array, that consists of all
	 * the children is only rooted once.
	 */
	err = xal_pool_claim_inodes(&xal->inodes, 1, &self->content.dentries.inodes);
	if (err) {
		XAL_DEBUG("FAILED: !xal_pool_claim_inodes(); err(%d)", err)
		return err;
	}

	/**
	 * Decode the extents and process each block
	 */
	for (uint64_t i = 0; (nbytes > 0) && (i < nextents); ++i) {
		struct xal_extent extent = {0};

		XAL_DEBUG("INFO: extent(%" PRIu64 "/%" PRIu64 ")", i + 1, nextents);
		XAL_DEBUG("INFO: nbytes(%" PRIu64 ")", nbytes);

		decode_xfs_extent(be64toh(extents[i].l0), be64toh(extents[i].l1), &extent);

		for (size_t fsblk = 0; fsblk < extent.nblocks; fsblk += fsblk_per_dblk) {
			uint64_t fsbno = extent.start_block + fsblk;

			XAL_DEBUG("INFO:  fsbno(0x%" PRIu64 ") @ ofz(%" PRIu64 ")", fsbno,
				  xal_fsbno_offset(xal, fsbno));
			XAL_DEBUG("INFO:  fsblk(%zu : %zu/%zu)", fsblk, fsblk + 1, extent.nblocks);
			XAL_DEBUG("INFO:   dblk(%zu/%zu)", (fsblk / fsblk_per_dblk) + 1,
				  extent.nblocks / fsblk_per_dblk);

			err = process_dinode_dir_extents_dblock(xal, fsbno, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_dir_extents_block():err(%d)",
					  err);
				return err;
			}
		}

		nbytes -= extent.nblocks * xal->sb.blocksize;
	}

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

	XAL_DEBUG("ENTER");

	err = dinodes_get(xal, ino, (void **)&dinode);
	if (err) {
		XAL_DEBUG("FAILED: dinodes_get(); err(%d)", err);
		return err;
	}

	if (!self->ftype) {
		uint16_t mode = be16toh(dinode->di_mode);

		if (S_ISDIR(mode)) {
			self->ftype = XAL_ODF_DIR3_FT_DIR;
		} else if (S_ISREG(mode)) {
			self->ftype = XAL_ODF_DIR3_FT_REG_FILE;
		} else {
			XAL_DEBUG("FAILED: unsupported ftype");
			return -EINVAL;
		}
	}

	self->size = be64toh(dinode->size);
	self->ino = be64toh(dinode->ino);

	XAL_DEBUG("INFO: ino(0x%" PRIx64 ") @ ofz(%" PRIu64 "), name(%.*s)[%" PRIu8 "]", ino,
		  xal_ino_decode_absolute_offset(xal, ino), self->namelen, self->name,
		  self->namelen);
	XAL_DEBUG("INFO: format(0x%" PRIu8 ")", dinode->di_format);

	switch (dinode->di_format) {
	case XAL_DINODE_FMT_BTREE:
		switch (self->ftype) {
		case XAL_ODF_DIR3_FT_DIR:
			err = process_dinode_dir_btree(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_dir_btree():err(%d)", err);
				return err;
			}
			break;

		case XAL_ODF_DIR3_FT_REG_FILE:
			err = process_dinode_file_btree_root(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_file_btree_root():err(%d)", err);
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
			err = process_dinode_dir_extents(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_dir_extents()");
				return err;
			}
			break;

		case XAL_ODF_DIR3_FT_REG_FILE:
			err = process_dinode_file_extents(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_file_extents()");
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
			err = process_dinode_dir_local(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_dir_local()");
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

	XAL_DEBUG("EXIT");

	return 0;
}

/**
 * Retrieve the IAB3 block 'blkno' in 'ag' via 'xal->buf' into 'buf' and convert endianess
 */
static int
read_iab3_block(struct xal *xal, struct xal_ag *ag, uint64_t blkno, void *buf)
{
	uint64_t ofz = xal_agbno_absolute_offset(xal, ag->seqno, blkno);
	struct xal_odf_btree_sfmt *block = (void *)buf;
	int err;

	XAL_DEBUG("ENTER: blkno(0x%" PRIx64 ", %" PRIu64 ") @ ofz(%" PRIu64 ")", blkno, blkno, ofz);

	err = dev_read_into(xal->dev, xal->buf, xal->sb.blocksize, ofz, buf);
	if (err) {
		XAL_DEBUG("FAILED: dev_read_into(); err(%d)", err);
		return err;
	}

	if (XAL_ODF_IBT_CRC_MAGIC != be32toh(block->magic.num)) {
		XAL_DEBUG("FAILED: expected magic(IAB3) got magic('%.4s', 0x%" PRIx32 "); ",
			  block->magic.text, block->magic.num);
		return -EINVAL;
	}

	block->pos.level = be16toh(block->pos.level);
	block->pos.numrecs = be16toh(block->pos.numrecs);
	block->siblings.left = be32toh(block->siblings.left);
	block->siblings.right = be32toh(block->siblings.right);
	block->blkno = be64toh(block->blkno);

	XAL_DEBUG("INFO:    seqno(%" PRIu32 ")", ag->seqno);
	XAL_DEBUG("INFO:    magic(%.4s, 0x%" PRIx32 ")", block->magic.text, block->magic.num);
	XAL_DEBUG("INFO:    level(%" PRIu16 ")", block->pos.level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", block->pos.numrecs);
	XAL_DEBUG("INFO:  leftsib(0x%08" PRIx32 " @ %" PRIu64 ")", block->siblings.left,
		  xal_agbno_absolute_offset(xal, ag->seqno, block->siblings.left));
	XAL_DEBUG("INFO:      bno(0x%08" PRIx64 " @ %" PRIu64 ")", blkno,
		  xal_agbno_absolute_offset(xal, ag->seqno, blkno));
	XAL_DEBUG("INFO: rightsib(0x%08" PRIx32 " @ %" PRIu64 ")", block->siblings.right,
		  xal_agbno_absolute_offset(xal, ag->seqno, block->siblings.right));

	XAL_DEBUG("EXIT");

	return 0;
}

static int
decode_iab3_leaf_records(struct xal *xal, struct xal_ag *ag, void *buf, uint64_t *index)
{
	struct xal_odf_btree_sfmt *root = (void *)buf;
	int err;

	XAL_DEBUG("ENTER");

	for (uint16_t reci = 0; reci < root->pos.numrecs; ++reci) {
		uint8_t inodechunk[BUF_NBYTES] = {0};
		struct xal_odf_inobt_rec *rec;
		uint32_t agbno, agbino;

		rec = (void *)(((uint8_t *)buf) + sizeof(*root) + reci * sizeof(*rec));
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

			memset(xal->buf, 0, chunk_nbytes);

			err = dev_read(xal->dev, xal->buf, chunk_nbytes, chunk_offset);
			if (err) {
				XAL_DEBUG("FAILED: dev_read(chunk)");
				return err;
			}
			memcpy(inodechunk, xal->buf, chunk_nbytes);
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

	XAL_DEBUG("EXIT");

	return err;
}

int
retrieve_dinodes_via_iab3(struct xal *xal, struct xal_ag *ag, uint64_t blkno, uint64_t *index);

/**
 * Decodes the node and invokes retrieve_dinodes_via_iab3() for each decoded record.
 */
static int
decode_iab3_node_records(struct xal *xal, struct xal_ag *ag, void *buf, uint64_t *index)
{
	uint32_t pointers[ODF_BLOCK_FS_BYTES_MAX / sizeof(uint32_t)] = {0};
	struct xal_odf_btree_sfmt *node = (void *)buf;
	uint8_t *cursor = buf;
	size_t pointers_ofz;
	int err;

	XAL_DEBUG("ENTER");

	btree_block_sfmt_meta(xal, NULL, NULL, &pointers_ofz);

	memcpy(&pointers, cursor + pointers_ofz, xal->sb.blocksize - pointers_ofz);

	XAL_DEBUG("#### Processing Pointers ###");
	for (uint16_t rec = 0; rec < node->pos.numrecs; ++rec) {
		uint32_t blkno = be32toh(pointers[rec]);

		XAL_DEBUG("INFO: ptr[%" PRIu16 "] = 0x%" PRIx32, rec, blkno);
		err = retrieve_dinodes_via_iab3(xal, ag, blkno, index);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_dinodes_via_iab3() : err(%d)", err);
			return err;
		}
	}

	XAL_DEBUG("EXIT");

	return 0;
}

/**
 * Retrieve all the allocated inodes stored within the given allocation group
 *
 * It is assumed that the inode-allocation-b+tree is rooted at the given 'blkno'
 */
int
retrieve_dinodes_via_iab3(struct xal *xal, struct xal_ag *ag, uint64_t blkno, uint64_t *index)
{
	uint8_t block[ODF_BLOCK_FS_BYTES_MAX] = {0};
	struct xal_odf_btree_sfmt *node = (void *)block;
	int err;

	XAL_DEBUG("ENTER");
	XAL_DEBUG("INFO: seqno(%" PRIu32 "), blkno(0x%" PRIx64 ")", ag->seqno, blkno);

	err = read_iab3_block(xal, ag, blkno, block);
	if (err) {
		XAL_DEBUG("FAILED: read_iab3_block(); err(%d)", err);
		return err;
	}

	switch (node->pos.level) {
	case 1:
		err = decode_iab3_node_records(xal, ag, block, index);
		if (err) {
			XAL_DEBUG("FAILED: decode_iab3_node(); err(%d)", err);
			return err;
		}
		break;

	case 0:
		err = decode_iab3_leaf_records(xal, ag, block, index);
		if (err) {
			XAL_DEBUG("FAILED: decode_iab3_leaf(); err(%d)", err);
			return err;
		}
		break;

	default:
		XAL_DEBUG("FAILED: iab3->level(%" PRIu16 ")?", node->pos.level);
		return -EINVAL;
	}

	XAL_DEBUG("EXIT");

	return 0;
}

int
xal_dinodes_retrieve(struct xal *xal)
{
	uint64_t index = 0;

	XAL_DEBUG("ENTER");

	xal->dinodes = calloc(1, xal->sb.nallocated * xal->sb.inodesize);
	if (!xal->dinodes) {
		XAL_DEBUG("FAILED: calloc()");
		return -errno;
	}

	for (uint32_t seqno = 0; seqno < xal->sb.agcount; ++seqno) {
		struct xal_ag *ag = &xal->ags[seqno];
		int err;

		XAL_DEBUG("INFO: seqno: %" PRIu32 "", seqno);

		err = retrieve_dinodes_via_iab3(xal, ag, ag->agi_root, &index);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_dinodes_via_iab3(); err(%d)", err);
			free(xal->dinodes);
			xal->dinodes = NULL;
			return err;
		}
	}

	XAL_DEBUG("EXIT");

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
	xal->root->namelen = 0;
	xal->root->content.extents.count = 0;
	xal->root->content.dentries.count = 0;

	return process_ino(xal, xal->root->ino, xal->root);
}

int
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