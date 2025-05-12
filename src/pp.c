#define _GNU_SOURCE
#include <endian.h>
#include <inttypes.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <xal_odf.h>

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
	wrtn += printf("  offset: %" PRIiMAX "\n", (intmax_t)ag->offset);
	wrtn += printf("  agf_length: %" PRIu32 "\n", ag->agf_length);
	wrtn += printf("  agi_count: %" PRIu32 "\n", ag->agi_count);
	wrtn += printf("  agi_root: %" PRIu32 "\n", ag->agi_root);
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
	wrtn += printf("  sb.blocksize: %" PRIu32 "\n", xal->sb.blocksize);
	wrtn += printf("  sb.sectsize: %" PRIu16 "\n", xal->sb.sectsize);
	wrtn += printf("  sb.inodesize: %" PRIu16 "\n", xal->sb.inodesize);
	wrtn += printf("  sb.inopblock: %" PRIu16 "\n", xal->sb.inopblock);
	wrtn += printf("  sb.inopblog: %" PRIu8 "\n", xal->sb.inopblog);
	wrtn += printf("  sb.icount: %" PRIu64 "\n", xal->sb.icount);
	wrtn += printf("  sb.nallocated: %" PRIu64 "\n", xal->sb.nallocated);
	wrtn += printf("  sb.rootino: %" PRIu64 "\n", xal->sb.rootino);
	wrtn += printf("  sb.agblocks: %" PRIu32 "\n", xal->sb.agblocks);
	wrtn += printf("  sb.agblklog: %" PRIu8 "\n", xal->sb.agblklog);
	wrtn += printf("  sb.agcount: %" PRIu32 "\n", xal->sb.agcount);

	for (uint32_t i = 0; i < xal->sb.agcount; ++i) {
		wrtn += xal_ag_pp(&xal->ags[i]);
	}

	return wrtn;
}

int
xal_odf_sb_pp(void *buf)
{
	struct xal_odf_sb *sb = buf;
	int wrtn = 0;

	wrtn += printf("xal_sb:\n");
	wrtn += printf("  magicnum: 0x%" PRIx32 "\n", be32toh(sb->magicnum));
	wrtn += printf("  blocksize: 0x%" PRIx32 "x\n", be32toh(sb->blocksize));
	wrtn += printf("  rootino: %" PRIx64 "zu\n", be64toh(sb->rootino));
	wrtn += printf("  agblocks: %" PRIx32 "d\n", be32toh(sb->agblocks));
	wrtn += printf("  agcount: %" PRIx32 "d\n", be32toh(sb->agcount));
	wrtn += printf("  sectsize: %" PRIx16 "u\n", be16toh(sb->sectsize));
	wrtn += printf("  inodesize: %" PRIx16 "u\n", be16toh(sb->inodesize));
	wrtn += printf("  fname: '%.*s'\n", XAL_ODF_LABEL_MAX, sb->fname);

	return wrtn;
}

int
xal_odf_agf_pp(void *buf)
{
	struct xal_odf_agf *agf = buf;
	int wrtn = 0;

	wrtn += printf("xal_odf_agf:\n");
	wrtn += printf("  magicnum: 0x%x\n", be32toh(agf->magicnum));
	wrtn += printf("  seqno: 0x%x\n", be32toh(agf->seqno));
	wrtn += printf("  length: 0x%x\n", be32toh(agf->length));

	return wrtn;
}

int
xal_odf_agi_pp(void *buf)
{
	struct xal_odf_agi *agi = buf;
	int wrtn = 0;

	wrtn += printf("xal_agi:\n");
	wrtn += printf("  magicnum: 0x%x\n", be32toh(agi->magicnum));
	wrtn += printf("  seqno: 0x%x\n", be32toh(agi->seqno));
	wrtn += printf("  length: 0x%x\n", be32toh(agi->length));

	return wrtn;
}

int
xal_odf_agfl_pp(void *buf)
{
	struct xal_odf_agfl *agfl = buf;
	int wrtn = 0;

	wrtn += printf("xal_odf_agfl:\n");
	wrtn += printf("  magicnum: 0x%x\n", agfl->magicnum);
	wrtn += printf("  seqno: 0x%x\n", agfl->seqno);

	return wrtn;
}

const char *
xal_odf_dinode_format_str(int val)
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
	wrtn += printf("  namelen: %" PRIu8 "\n", inode->namelen);
	wrtn += printf("  name: '%.256s'\n", inode->name);
	wrtn += printf("  ftype: %" PRIu8 "\n", inode->ftype);

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR:
		wrtn += printf("  dentries.count: %u\n", inode->content.dentries.count);
		break;

	case XAL_ODF_DIR3_FT_REG_FILE:
		wrtn += printf("  extents.count: %u\n", inode->content.extents.count);
		break;
	}

	for (uint8_t i = 0; i < inode->content.dentries.count; ++i) {
		struct xal_inode *children = inode->content.dentries.inodes;

		xal_inode_pp(&children[i]);
	}

	return wrtn;
}

const char *
mode_to_type_str(uint32_t mode)
{
	if (S_ISDIR(mode)) {
		return "directory";
	}
	if (S_ISREG(mode)) {
		return "file";
	}

	return "UNEXPECTED";
}

int
xal_odf_dinode_pp(void *buf)
{
	struct xal_odf_dinode *dinode = buf;
	int wrtn = 0;

	wrtn += printf("xal_dinode:\n");
	wrtn += printf("  magic: 0x%x | 0x%x\n", be16toh(dinode->di_magic), XAL_DINODE_MAGIC);
	wrtn += printf("  mode: 0x%" PRIx16 " | '%s'\n", be16toh(dinode->di_mode),
		       mode_to_type_str(be16toh(dinode->di_mode)));
	wrtn += printf("  format: 0x%" PRIu8 " | '%s'\n", dinode->di_format,
		       xal_odf_dinode_format_str(dinode->di_format));

	wrtn += printf("  ino: %" PRIu64 "\n", be64toh(dinode->ino));

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
	wrtn += printf("  start_block: 0x%" PRIx64 "\n", extent->start_block);
	wrtn += printf("  nblocks: %" PRIu64 "\n", extent->nblocks);
	wrtn += printf("  flag: %" PRIu8 "\n", extent->flag);

	return wrtn;
}

int
xal_odf_btree_iab3_sfmt_pp(struct xal_odf_btree_sfmt *iab3)
{
	int wrtn = 0;

	if (!iab3) {
		wrtn += printf("xal_ofd_btree_iab3: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_ofd_btree_iab3:\n");
	wrtn += printf("  magic: 0x%08" PRIX32 " / '%.4s'\n", iab3->magic.num, iab3->magic.text);
	wrtn += printf("  level: %" PRIu16 "\n", iab3->level);
	wrtn += printf("  numrecs: %" PRIu16 "\n", iab3->numrecs);
	wrtn += printf("  leftsib: 0x%08" PRIX32 "\n", iab3->leftsib);
	wrtn += printf("  rightsib: 0x%08" PRIX32 "\n", iab3->rightsib);

	wrtn += printf("  blkno: %" PRIu64 "\n", iab3->blkno / 8);

	return wrtn;
}

int
xal_odf_inobt_rec_pp(struct xal_odf_inobt_rec *rec)
{
	int wrtn = 0;

	if (!rec) {
		wrtn += printf("xal_ofd_inobt_rec: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_ofd_inobt_rec:\n");
	wrtn += printf("  startino: %" PRIu32 "\n", rec->startino);
	wrtn += printf("  holemask: %" PRIu16 "\n", rec->holemask);
	wrtn += printf("  count: %" PRIu8 "\n", rec->count);
	wrtn += printf("  freecount: %" PRIu8 "\n", rec->freecount);
	wrtn += printf("  free: %" PRIu64 "\n", rec->free);

	return wrtn;
}