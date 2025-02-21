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
#include <xal_pp.h>

#define BUF_NBYTES 4096

/**
struct pool {
	size_t nentries;
	size_t current;
	struct xal_dir_entry entries[];
};

int
pool_alloc(size_t count, struct pool **pool)
{
	struct pool *cand;

	cand = calloc(count, sizeof(**pool));
	if (!cand) {
		return -errno;
	}
	cand->current = 0;
	cand->nentries = count;

	*pool = cand;

	return 0;
}

int
pool_pop(struct pool *pool, struct xal_dir_entry **entry)
{
	// TODO: do re-allocate here

	*entry = &pool->entries[pool->current];
	pool->current += 1;

	return 0;
}

void
pool_free(struct pool *pool)
{
	free(pool);
}
*/

uint64_t
xal_get_inode_offset(struct xal *xal, uint64_t ino)
{
	uint64_t agno, agbno, agbino, offset;

	// Allocation Group Number
	agno = ino >> xal->agblklog;

	// Block Number relative to Allocation Group
	agbno = (ino & ((1ULL << xal->agblklog) - 1)) >> xal->inopblog;

	// Inode number relative to Block in Allocation Group
	agbino = ino & ((1ULL << xal->inopblog) - 1);

	// Absolute Inode offset in bytes
	offset = (agno * xal->agblocks + agbno) * xal->blocksize;

	return offset + (agbino * xal->inodesize);
}

void
xal_close(struct xal *mp)
{
	if (!mp) {
		return;
	}

	close(mp->handle.fd);
}

int
xal_open(const char *path, struct xal **xal)
{
	struct xal base = {0};
	char buf[BUF_NBYTES] = {0};
	const struct xal_sb *psb = (void *)buf;
	struct xal *cand;
	ssize_t nbytes;

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
	base.blocksize = be32toh(psb->blocksize);
	base.sectsize = be16toh(psb->sectsize);
	base.inodesize = be16toh(psb->inodesize);
	base.inopblock = be16toh(psb->sb_inopblock);
	base.inopblog = psb->sb_inopblog;
	base.rootino = be64toh(psb->rootino);
	base.agblocks = be32toh(psb->agblocks);
	base.agblklog = psb->sb_agblklog;
	base.agcount = be32toh(psb->agcount);

	cand = calloc(1, sizeof(*cand) + sizeof(*(cand->ags)) * base.agcount);
	if (!cand) {
		perror("Failed allocating Reading Primary Superblock failed.");
		return -errno;
	}
	*cand = base;

	// Retrieve allocation-group meta, convert it, and store it.
	for (uint32_t agno = 0; agno < cand->agcount; ++agno) {
		struct xal_agf *agf = (void *)(buf + cand->sectsize);
		struct xal_agi *agi = (void *)(buf + cand->sectsize * 2);
		off_t offset;

		memset(buf, 0, BUF_NBYTES);

		offset = (off_t)agno * (off_t)cand->agblocks * (off_t)cand->blocksize;
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

	// All is good; promote the candidate
	*xal = cand;

	return 0;
}



int
xal_dir_from_shortform(void *inode, struct xal_inode **dir)
{
	uint8_t *cursor = inode;
	uint8_t count, i8count;
	struct xal_inode *cand;

	cursor += sizeof(struct xal_dinode); ///< Advance past inode data

	count = *cursor;
	cursor += 1; ///< Advance past count

	i8count = *cursor;
	cursor += 1; ///< Advance past i8count

	cursor += i8count ? 8 : 4; ///< Advance past parent inode number

	cand = calloc(1, count * sizeof(*cand->children) + sizeof(*cand));
	if (!cand) {
		return -errno;
	}
	cand->count = count;

	/** DECODE: namelen[1], offset[2], name[namelen], ftype[1], ino[4] | ino[8] */
	for (int i = 0; i < count; ++i) {
		struct xal_inode *entry = cand->children[i];

		entry->namelen = *cursor;
		cursor += 1 + 2; ///< Advance past 'namelen' and 'offset[2]'

		memcpy(entry->name, cursor, entry->namelen);
		cursor += entry->namelen; ///< Advance past 'name'

		entry->ftype = *cursor;
		cursor += 1; ///< Advance past 'ftype'

		if (i8count) {
			i8count--;
			entry->ino = be64toh(*(uint64_t *)cursor);
			cursor += 8; ///< Advance past 64-bit inode number
		} else {
			entry->ino = be32toh(*(uint32_t *)cursor);
			cursor += 4; ///< Advance past 32-bit inode number
		}
	}

	*dir = cand; ///< Promote the candidate

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
xal_decode_extents(void *buf)
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

	printf("nextents: %" PRIu64 "\n", nextents);
	printf("dinode->magic: 0x%04" PRIX32 "\n", dinode->di_magic);
	printf("dinode->di_size: %" PRIu64 "\n", be64toh(dinode->di_size));

	cursor += sizeof(struct xal_dinode); ///< Advance past inode data

	for (uint64_t i = 0; i < nextents; ++i) {
		uint64_t l0, l1;

		l0 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		l1 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		decode_xfs_extent(l0, l1, NULL);
	}

	return 0;
}

/**
 * Internal helper recursively traversing the on-disk-format to build an index of the file-system
 */
int
process_inode(struct xal *xal, uint64_t ino, struct xal_inode *index)
{
	uint8_t buf[BUF_NBYTES] = {0};
	struct xal_dinode *dinode;
	ssize_t nbytes;

	printf("\n## ino(0x%08" PRIX64 ")\n", ino);

	///< Read the on-disk inode data
	nbytes = pread(xal->handle.fd, buf, xal->sectsize, xal_get_inode_offset(xal, ino));
	if (nbytes != xal->sectsize) {
		return -EIO;
	}

	dinode = (void *)buf;
	xal_dinode_pp(buf);

	switch (dinode->di_format) {
	case XAL_DINODE_FMT_DEV: ///< What is this?
		break;

	case XAL_DINODE_FMT_BTREE: ///< Recursively walk the btree
		break;

	case XAL_DINODE_FMT_EXTENTS: ///< Decode extent in inode
		xal_decode_extents(buf);
		break;

	case XAL_DINODE_FMT_LOCAL: ///< Decode directory listing in inode
	{
		struct xal_inode *dir;

		xal_dir_from_shortform(buf, &dir);
		xal_inode_pp(dir);
		for (uint8_t i = 0; i < dir->count; ++i) {
			struct xal_inode *child = dir->children[i];
			process_inode(xal, child->ino, index);
		}
	} break;

	case XAL_DINODE_FMT_UUID:
		break;
	}

	return 0;
}

int
xal_get_index(struct xal *xal, struct xal_inode **index)
{
	printf("xal_get_index(): not implemented. xal(%p), index(%p)\n", (void *)xal,
	       (void *)index);
	return 0;
}

int
xal_dir_walk(struct xal_inode *dir, void *cb_func, void *cb_data)
{
	printf("xal_dir_walk(): not implemented; dir(%p), func(%p), data(%p)\n", (void *)dir,
	       (void *)cb_func, (void *)cb_data);

	return -ENOSYS;
}