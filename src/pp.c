#define _GNU_SOURCE
#include <endian.h>
#include <inttypes.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdio.h>
#include <xal.h>

int
xal_ag_pp(struct xal_ag *ag)
{
	int wrtn = 0;

	if (!ag) {
		wrtn += printf("xal_ag: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_ag:\n");
	wrtn += printf("  seqno: %" PRIu32 "\n", ag->seqno);
	wrtn += printf("  agf_length: %" PRIu32 "\n", ag->agf_length);
	wrtn += printf("  agi_count: %" PRIu32 "\n", ag->agi_count);
	wrtn += printf("  agi_root: 0x%016" PRIx32 "\n", ag->agi_root);
	wrtn += printf("  agi_level: %" PRIu32 "\n", ag->agi_level);

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
	wrtn += printf("  blocksize: %" PRIu32 "\n", xal->blocksize);
	wrtn += printf("  sectsize: %" PRIu16 "\n", xal->sectsize);
	wrtn += printf("  inodesize: %" PRIu16 "\n", xal->inodesize);
	wrtn += printf("  inopblock: %" PRIu16 "\n", xal->inopblock);
	wrtn += printf("  inopblog: %" PRIu8 "\n", xal->inopblog);
	wrtn += printf("  rootino: %" PRIu64 "\n", xal->rootino);
	wrtn += printf("  agblocks: %" PRIu32 "\n", xal->agblocks);
	wrtn += printf("  agblklog: %" PRIu8 "\n", xal->agblklog);
	wrtn += printf("  agcount: %" PRIu32 "\n", xal->agcount);

	for (uint32_t i = 0; i < xal->agcount; ++i) {
		wrtn += xal_ag_pp(&xal->ags[i]);
	}

	return wrtn;
}

int
xal_sb_pp(void *buf)
{
	struct xal_sb *sb = buf;
	int wrtn = 0;

	wrtn += printf("xal_sb:\n");
	wrtn += printf("  magicnum: 0x%" PRIx32 "\n", be32toh(sb->magicnum));
	wrtn += printf("  blocksize: 0x%" PRIx32 "x\n", be32toh(sb->blocksize));
	wrtn += printf("  rootino: %" PRIx64 "zu\n", be64toh(sb->rootino));
	wrtn += printf("  agblocks: %" PRIx32 "d\n", be32toh(sb->agblocks));
	wrtn += printf("  agcount: %" PRIx32 "d\n", be32toh(sb->agcount));
	wrtn += printf("  sectsize: %" PRIx16 "u\n", be16toh(sb->sectsize));
	wrtn += printf("  inodesize: %" PRIx16 "u\n", be16toh(sb->inodesize));
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

int
xal_inode_pp(struct xal_inode *inode)
{
	int wrtn = 0;

	if (!inode) {
		wrtn += printf("xal_inode: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_inode:\n");
	wrtn += printf("  ino: 0x%08" PRIX64 "\n", inode->ino);
	wrtn += printf("  ftype: %" PRIu8 "\n", inode->ftype);
	wrtn += printf("  namelen: %" PRIu8 "\n", inode->namelen);
	wrtn += printf("  name: '%.256s'\n", inode->name);
	wrtn += printf("  nchildren: %u\n", inode->count);

	for (uint8_t i = 0; i < inode->count; ++i) {
		xal_inode_pp(inode->children[i]);
	}

	return wrtn;
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

int
xal_extent_pp(struct xal_extent *extent)
{
	int wrtn = 0;

	if (!extent) {
		wrtn += printf("xal_extent: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_extent:\n");
	wrtn += printf("  start_offset: %" PRIu64 "\n", extent->start_offset);
	wrtn += printf("  start_block: %" PRIu64 "\n", extent->start_block);
	wrtn += printf("  nblocks: %" PRIu64 "\n", extent->nblocks);

	return wrtn;
}