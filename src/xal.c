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

#define BUF_NBYTES 4096

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

int
xal_ag_pp(struct xal_ag *ag)
{
	int wrtn = 0;

	if (!ag) {
		wrtn += printf("xal_ag: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_ag:\n");
	wrtn += printf("  seqno: %u\n", ag->seqno);
	wrtn += printf("  agf_length: %u\n", ag->agf_length);
	wrtn += printf("  agi_count: %u\n", ag->agi_count);
	wrtn += printf("  agi_root: %u\n", ag->agi_root);
	wrtn += printf("  agi_level: %u\n", ag->agi_level);

	return wrtn;
}

int
xal_pp(struct xal *xal)
{
	int wrtn = 0;

	if (!xal) {
		wrtn += printf("xal: ~\n");
		return wrtn;
	}

	wrtn += printf("xal:\n");
	wrtn += printf("  blocksize: %u\n", xal->blocksize);
	wrtn += printf("  sectsize: %u\n", xal->sectsize);
	wrtn += printf("  inodesize: %u\n", xal->inodesize);
	wrtn += printf("  inopblock: %u\n", xal->inopblock);
	wrtn += printf("  inopblog: %u\n", xal->inopblog);
	wrtn += printf("  rootino: %zu\n", xal->rootino);
	wrtn += printf("  agblocks: %d\n", xal->agblocks);
	wrtn += printf("  agblklog: %d\n", xal->agblklog);
	wrtn += printf("  agcount: %d\n", xal->agcount);

	for (uint32_t i = 0; i < xal->agcount; ++i) {
		wrtn += xal_ag_pp(&xal->ags[i]);
	}

	return wrtn;
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

	// TODO: add verification that agcount has a reasonable size

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
xal_sb_pp(void *buf)
{
	struct xal_sb *sb = buf;
	int wrtn = 0;

	wrtn += printf("xal_sb:\n");
	wrtn += printf("  magicnum: 0x%x\n", be32toh(sb->magicnum));
	wrtn += printf("  blocksize: 0x%x\n", be32toh(sb->blocksize));
	wrtn += printf("  rootino: %zu\n", be64toh(sb->rootino));
	wrtn += printf("  agblocks: %d\n", be32toh(sb->agblocks));
	wrtn += printf("  agcount: %d\n", be32toh(sb->agcount));
	wrtn += printf("  sectsize: %u\n", be16toh(sb->sectsize));
	wrtn += printf("  inodesize: %u\n", be16toh(sb->inodesize));
	wrtn += printf("  fname: '%.*s'\n", XALLABEL_MAX, sb->sb_fname);

	return wrtn;
}

int
xal_agf_pp(void *buf)
{
	struct xal_agf *agf = buf;
	int wrtn = 0;

	wrtn += printf("xal_agf:\n");
	wrtn += printf("  magicnum: 0x%x\n", be32toh(agf->magicnum));
	wrtn += printf("  seqno: 0x%x\n", be32toh(agf->seqno));
	wrtn += printf("  length: 0x%x\n", be32toh(agf->length));

	return wrtn;
}

int
xal_agi_pp(void *buf)
{
	struct xal_agi *agi = buf;
	int wrtn = 0;

	wrtn += printf("xal_agi:\n");
	wrtn += printf("  magicnum: 0x%x\n", be32toh(agi->magicnum));
	wrtn += printf("  seqno: 0x%x\n", be32toh(agi->seqno));
	wrtn += printf("  length: 0x%x\n", be32toh(agi->length));

	return wrtn;
}

int
xal_agfl_pp(void *buf)
{
	struct xal_agfl *agfl = buf;
	int wrtn = 0;

	wrtn += printf("xal_agfl:\n");
	wrtn += printf("  magicnum: 0x%x\n", agfl->magicnum);
	wrtn += printf("  seqno: 0x%x\n", agfl->seqno);

	return wrtn;
}

const char *
xal_dinode_format_str(int val)
{
	switch (val) {
	case XAL_DINODE_FMT_BTREE:
		return "btree";
	case XAL_DINODE_FMT_DEV:
		return "dev";
	case XAL_DINODE_FMT_EXTENTS:
		return "extents";
	case XAL_DINODE_FMT_LOCAL:
		return "local";
	case XAL_DINODE_FMT_UUID:
		return "uuid";
	};

	return "INODE_FORMAT_UNKNOWN";
}

struct xal_dir2_sf_hdr {
	uint8_t count;	 ///< Number of inodes
	uint8_t i8count; ///< ??
	uint64_t parent; ///< Parent directory inode number
} __attribute__((packed));

struct xal_dir2_sf_entry {
	uint8_t namelen;
} __attribute__((packed));

void
dump_bytes(void *buf, int nbytes)
{
	for (int i = 0; i < nbytes; ++i) {
		uint8_t val = ((uint8_t *)buf)[i];

		if ((val > 31) && (val < 127)) {
			printf("%03d: '%c'\n", i, val);
		} else {
			printf("%03d: %u\n", i, val);
		}
	}
}

int
xal_dir_pp(struct xal_dir *dir)
{
	int wrtn = 0;

	if (!dir) {
		wrtn += printf("xal_dir: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_dir:\n");
	wrtn += printf("  count: %u\n", dir->count);

	for (uint8_t i = 0; i < dir->count; ++i) {
		wrtn += printf("xal_dir_entry:\n");
		wrtn += printf("  namelen: %u\n", dir->entries[i].namelen);
		wrtn += printf("  name: '%s'\n", dir->entries[i].name);
		wrtn += printf("  ino: 0x%08X\n", dir->entries[i].ino);
		wrtn += printf("  ftype: %d\n", dir->entries[i].ftype);
	}
}

int
xal_dir_from_shortform(struct xal *xal, void *buf, struct xal_dir **dir)
{
	struct xal_xfs_dir2_sf_hdr *odf = (void *)(((char *)buf) + 176);
	char *cursor = (void *)(((char *)buf) + 182);
	struct xal_dir *cand;

	printf("count; %u, i8count: %u\n", odf->count, odf->i8count);
	dump_bytes(buf + 176, 30);

	cand = calloc(1, odf->count * sizeof(*cand->entries) + sizeof(*cand));
	if (!cand) {
		return -errno;
	}
	cand->count = odf->count;

	for (int i = 0; i < odf->count; ++i) {
		struct xal_dir_entry *entry = &cand->entries[i];

		entry->namelen = *cursor;
		cursor += 1 + 2; ///< Advance past 'namelen' and 'offset[2]'

		memcpy(entry->name, cursor, entry->namelen);
		cursor += entry->namelen; ///< Advance past 'name'

		entry->ftype = *((uint8_t *)(cursor));
		cursor += 1; ///< Advance past file-type

		///< TODO: add support handling of i8count here
		entry->ino = be32toh(*(uint32_t *)cursor);
		cursor += 4; ///< Advance past inode number
	}
	xal_dir_pp(cand);

	*dir = cand;

	return 0;
}

int
xal_dinode_pp(void *buf)
{
	struct xal_dinode *dinode = buf;
	int wrtn = 0;

	wrtn += printf("xal_dinode:\n");
	wrtn += printf("  magic: 0x%x | 0x%x\n", dinode->di_magic, XAL_DINODE_MAGIC);
	wrtn += printf("  format: 0x%x\n", dinode->di_format);
	wrtn += printf("  format_str: '%s'\n", xal_dinode_format_str(dinode->di_format));

	return wrtn;
}
