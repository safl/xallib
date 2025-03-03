/**
 * Internal definitions for the Xfs Access Library (XAL)
 *
 * These should be compatible with the Linux Kernel XFS definitions of the equivalent structures
 * for superblock, allocation-group headers, and magic-values. They are defined in this manner to
 * be to able to embed / vendor this in various ways.
 *
 * The definitions include:
 *
 * - Superblock
 *   - xal_sb
 *
 * - Allocation Group Headers
 *   - xal_agf
 *   - xal_agi
 *   - xal_agfl
 *
 * - Inode Information
 *   - todo
 */

/**
 * These are used by xfs_types.h but expected to be defined in a linux header,
 * thus dropping a pseudo-rep for theme here.
 */
#include <sys/types.h>
typedef uint64_t xfs_ino_t;
typedef uint32_t xfs_nlink_t; // NOTE: This is defined as '__u32'

#include <xfs/xfs_types.h>

/**
 * We do not want to depend on a library providing uuid_t since we are not using
 * it for anything but basic printing and byte-wise comparison. Thus, this
 * simple representation here.
 */
typedef struct {
	uint8_t uuid[16];
} uuid_t;

/// The following are hand-picked from xfs/xfs_format.h

/**
 * Maximum size of the filesystem label, no terminating NULL
 */
#define XAL_ODF_LABEL_MAX 12

#define XAL_ODF_AGF_MAGIC 0x58414746  /* 'XAGF' */
#define XAL_ODF_AGI_MAGIC 0x58414749  /* 'XAGI' */
#define XAL_ODF_AGFL_MAGIC 0x5841464c /* 'XAFL' */

#define XAL_ODF_DIR3_FT_REG_FILE 1
#define XAL_ODF_DIR3_FT_DIR 2

/**
 * The XFS Superblock on-disk representation in v5 format
 */
struct xal_odf_sb {
	uint32_t magicnum;  /* magic number == XAL_SB_MAGIC */
	uint32_t blocksize; /* logical block size, bytes */

	xfs_rfsblock_t sb_dblocks; /* number of data blocks */
	xfs_rfsblock_t sb_rblocks; /* number of realtime blocks */
	xfs_rtbxlen_t sb_rextents; /* number of realtime extents */
	uuid_t sb_uuid;		   /* user-visible file system unique id */
	xfs_fsblock_t sb_logstart; /* starting block of log if internal */

	xfs_ino_t rootino; /* root inode number */

	xfs_ino_t sb_rbmino;	   /* bitmap inode for realtime extents */
	xfs_ino_t sb_rsumino;	   /* summary inode for rt bitmap */
	xfs_agblock_t sb_rextsize; /* realtime extent size, blocks */

	xfs_agblock_t agblocks; /* size of an allocation group */
	xfs_agnumber_t agcount; /* number of allocation groups */

	xfs_extlen_t sb_rbmblocks; /* number of rt bitmap blocks */
	xfs_extlen_t sb_logblocks; /* number of log blocks */
	uint16_t sb_versionnum;	   /* header version == XAL_SB_VERSION */

	uint16_t sectsize;  /* volume sector size, bytes */
	uint16_t inodesize; /* inode size, bytes */

	uint16_t sb_inopblock; /* inodes per block */

	char sb_fname[XAL_ODF_LABEL_MAX]; /* file system name */

	uint8_t sb_blocklog;   /* log2 of sb_blocksize */
	uint8_t sb_sectlog;    /* log2 of sb_sectsize */
	uint8_t sb_inodelog;   /* log2 of sb_inodesize */
	uint8_t sb_inopblog;   /* log2 of sb_inopblock */
	uint8_t sb_agblklog;   /* log2 of sb_agblocks (rounded up) */
	uint8_t sb_rextslog;   /* log2 of sb_rextents */
	uint8_t sb_inprogress; /* mkfs is in progress, don't mount */
	uint8_t sb_imax_pct;   /* max % of fs for inode space */
	/* statistics */
	/*
	 * These fields must remain contiguous.  If you really
	 * want to change their layout, make sure you fix the
	 * code in xfs_trans_apply_sb_deltas().
	 */
	uint64_t sb_icount;    /* allocated inodes */
	uint64_t sb_ifree;     /* free inodes */
	uint64_t sb_fdblocks;  /* free data blocks */
	uint64_t sb_frextents; /* free realtime extents */
	/*
	 * End contiguous fields.
	 */
	xfs_ino_t sb_uquotino;	    /* user quota inode */
	xfs_ino_t sb_gquotino;	    /* group quota inode */
	uint16_t sb_qflags;	    /* quota flags */
	uint8_t sb_flags;	    /* misc. flags */
	uint8_t sb_shared_vn;	    /* shared version number */
	xfs_extlen_t sb_inoalignmt; /* inode chunk alignment, fsblocks */
	uint32_t sb_unit;	    /* stripe or raid unit */
	uint32_t sb_width;	    /* stripe or raid width */
	uint8_t sb_dirblklog;	    /* log2 of dir block size (fsbs) */
	uint8_t sb_logsectlog;	    /* log2 of the log sector size */
	uint16_t sb_logsectsize;    /* sector size for the log, bytes */
	uint32_t sb_logsunit;	    /* stripe unit size for the log */
	uint32_t sb_features2;	    /* additional feature bits */

	/*
	 * bad features2 field as a result of failing to pad the sb structure to
	 * 64 bits. Some machines will be using this field for features2 bits.
	 * Easiest just to mark it bad and not use it for anything else.
	 *
	 * This is not kept up to date in memory; it is always overwritten by
	 * the value in sb_features2 when formatting the incore superblock to
	 * the disk buffer.
	 */
	uint32_t sb_bad_features2;

	/* version 5 superblock fields start here */

	/* feature masks */
	uint32_t sb_features_compat;
	uint32_t sb_features_ro_compat;
	uint32_t sb_features_incompat;
	uint32_t sb_features_log_incompat;

	uint32_t sb_crc;	     /* superblock crc */
	xfs_extlen_t sb_spino_align; /* sparse inode chunk alignment */

	xfs_ino_t sb_pquotino; /* project quota inode */
	xfs_lsn_t sb_lsn;      /* last write sequence */
	uuid_t sb_meta_uuid;   /* metadata file system unique id */

	/* must be padded to 64 bit alignment */
};

struct xal_odf_agf {
	/*
	 * Common allocation group header information
	 */
	uint32_t magicnum;	 /* magic number == XAL_ODF_AGF_MAGIC */
	uint32_t agf_versionnum; /* header version == XAL_ODF_AGF_VERSION */
	uint32_t seqno;		 /* sequence # starting from 0 */
	uint32_t length;	 /* size in blocks of a.g. */
	/*
	 * Freespace and rmap information
	 */
	uint32_t agf_bno_root;	/* bnobt root block */
	uint32_t agf_cnt_root;	/* cntbt root block */
	uint32_t agf_rmap_root; /* rmapbt root block */

	uint32_t agf_bno_level;	 /* bnobt btree levels */
	uint32_t agf_cnt_level;	 /* cntbt btree levels */
	uint32_t agf_rmap_level; /* rmapbt btree levels */

	uint32_t agf_flfirst;  /* first freelist block's index */
	uint32_t agf_fllast;   /* last freelist block's index */
	uint32_t agf_flcount;  /* count of blocks in freelist */
	uint32_t agf_freeblks; /* total free blocks */

	uint32_t agf_longest;	/* longest free space */
	uint32_t agf_btreeblks; /* # of blocks held in AGF btrees */
	uuid_t agf_uuid;	/* uuid of filesystem */

	uint32_t agf_rmap_blocks;     /* rmapbt blocks used */
	uint32_t agf_refcount_blocks; /* refcountbt blocks used */

	uint32_t agf_refcount_root;  /* refcount tree root block */
	uint32_t agf_refcount_level; /* refcount btree levels */

	/*
	 * reserve some contiguous space for future logged fields before we add
	 * the unlogged fields. This makes the range logging via flags and
	 * structure offsets much simpler.
	 */
	uint64_t agf_spare64[14];

	/* unlogged fields, written during buffer writeback. */
	uint64_t agf_lsn; /* last write sequence */
	uint32_t agf_crc; /* crc of agf sector */
	uint32_t agf_spare2;

	/* structure must be padded to 64 bit alignment */
};

/*
 * Size of the unlinked inode hash table in the agi.
 */
#define XAL_AGI_UNLINKED_BUCKETS 64

struct xal_odf_agi {
	/*
	 * Common allocation group header information
	 */
	uint32_t magicnum;   /* magic number == XAL_AGI_MAGIC */
	uint32_t versionnum; /* header version == XAL_AGI_VERSION */
	uint32_t seqno;	     /* sequence # starting from 0 */
	uint32_t length;     /* size in blocks of a.g. */
	/*
	 * Inode information
	 * Inodes are mapped by interpreting the inode number, so no
	 * mapping data is needed here.
	 */
	uint32_t agi_count;	/* count of allocated inodes */
	uint32_t agi_root;	/* root of inode btree */
	uint32_t agi_level;	/* levels in inode btree */
	uint32_t agi_freecount; /* number of free inodes */

	uint32_t agi_newino; /* new inode just allocated */
	uint32_t agi_dirino; /* last directory inode chunk */
	/*
	 * Hash table of inodes which have been unlinked but are
	 * still being referenced.
	 */
	uint32_t agi_unlinked[XAL_AGI_UNLINKED_BUCKETS];
	/*
	 * This marks the end of logging region 1 and start of logging region 2.
	 */
	uuid_t agi_uuid;  /* uuid of filesystem */
	uint32_t agi_crc; /* crc of agi sector */
	uint32_t agi_pad32;
	uint64_t agi_lsn; /* last write sequence */

	uint32_t agi_free_root;	 /* root of the free inode btree */
	uint32_t agi_free_level; /* levels in free inode btree */

	uint32_t agi_iblocks; /* inobt blocks used */
	uint32_t agi_fblocks; /* finobt blocks used */

	/* structure must be padded to 64 bit alignment */
};

struct xal_odf_agfl {
	uint32_t magicnum;
	uint32_t seqno;
	uuid_t agfl_uuid;
	uint64_t agfl_lsn;
	uint32_t agfl_crc;
} __attribute__((packed));

/**
 * These are in big-endian format
 */
typedef uint64_t xfs_timestamp_t;

#define XAL_DINODE_MAGIC 0x494e /* 'IN' */

/*
 * Values for di_format
 *
 * This enum is used in string mapping in xfs_trace.h; please keep the
 * TRACE_DEFINE_ENUMs for it up to date.
 */
enum xal_odf_dinode_fmt {
	XAL_DINODE_FMT_DEV,	/* xfs_dev_t */
	XAL_DINODE_FMT_LOCAL,	/* bulk data */
	XAL_DINODE_FMT_EXTENTS, /* struct xfs_bmbt_rec */
	XAL_DINODE_FMT_BTREE,	/* struct xfs_bmdr_block */
	XAL_DINODE_FMT_UUID	/* added long ago, but never used */
};

#define XAL_DIFLAG2_NREXT64_BIT 4 /* large extent counters */
#define XAL_DIFLAG2_NREXT64 (1 << XAL_DIFLAG2_NREXT64_BIT)

struct xal_odf_dinode {
	uint16_t di_magic;     /* inode magic # = XFS_DINODE_MAGIC */
	uint16_t di_mode;      /* mode and type of file; see stat.h */
	uint8_t di_version;    /* inode version; one of these [1,2,3]*/
	uint8_t di_format;     /* format of di_c data */
	uint16_t di_onlink;    /* old number of links to file */
	uint32_t di_uid;       /* owner's user id */
	uint32_t di_gid;       /* owner's group id */
	uint32_t di_nlink;     /* number of links to file */
	uint16_t di_projid_lo; /* lower part of owner's project id */
	uint16_t di_projid_hi; /* higher part owner's project id */
	union {
		/* Number of data fork extents if NREXT64 is set */
		uint64_t di_big_nextents;

		/* Padding for V3 inodes without NREXT64 set. */
		uint64_t di_v3_pad;

		/* Padding and inode flush counter for V2 inodes. */
		struct {
			uint8_t di_v2_pad[6];
			uint16_t di_flushiter;
		};
	};
	xfs_timestamp_t di_atime; /* time last accessed */
	xfs_timestamp_t di_mtime; /* time last modified */
	xfs_timestamp_t di_ctime; /* time created/inode modified */
	uint64_t di_size;	  /* number of bytes in file */
	uint64_t di_nblocks;	  /* # of direct & btree blocks used */
	uint32_t di_extsize;	  /* basic/minimum extent size for file */
	union {
		/*
		 * For V2 inodes and V3 inodes without NREXT64 set, this
		 * is the number of data and attr fork extents.
		 */
		struct {
			uint32_t di_nextents;
			uint16_t di_anextents;
		} __attribute__((packed));

		/* Number of attr fork extents if NREXT64 is set. */
		struct {
			uint32_t di_big_anextents;
			uint16_t di_nrext64_pad;
		} __attribute__((packed));
	} __attribute__((packed));
	uint8_t di_forkoff;   /* attr fork offs, <<3 for 64b align */
	uint8_t di_aformat;   /* format of attr fork's data */
	uint32_t di_dmevmask; /* DMIG event mask */
	uint16_t di_dmstate;  /* DMIG state info */
	uint16_t di_flags;    /* random flags, XFS_DIFLAG_... */
	uint32_t di_gen;      /* generation number */

	/* di_next_unlinked is the only non-core field in the old dinode */
	uint32_t di_next_unlinked; /* agi unlinked list ptr */

	/* start of the extended dinode, writable fields */
	uint32_t di_crc;	 /* LITTLE-ENDIAN: CRC of the inode */
	uint64_t di_changecount; /* number of attribute changes */
	uint64_t di_lsn;	 /* flush sequence */
	uint64_t di_flags2;	 /* more random flags */
	uint32_t di_cowextsize;	 /* basic cow extent size for file */
	uint8_t di_pad2[12];	 /* more padding for future expansion */

	/* fields only written to during inode creation */
	xfs_timestamp_t di_crtime; /* time created */
	uint64_t di_ino;	   /* inode number */
	uuid_t di_uuid;		   /* UUID of the filesystem */

	/* structure must be padded to 64 bit alignment */
};

struct xal_odf_dir2_sf_hdr {
	uint8_t count;	   /* Number of directory entries */
	uint8_t i8count;   /* Count of 8-byte inode numbers (vs 4-byte) */
	uint8_t parent[8]; /* Parent directory inode number */
} __attribute__((packed));

union xal_odf_btree_iab3_magic {
	uint32_t num;
	char text[4];
};

struct xal_odf_btree_iab3_sfmt {
	union xal_odf_btree_iab3_magic magic; // 'IAB3' for inode B+Tree
	uint16_t level;			      // Tree level (0 = leaf, >0 = interior)
	uint16_t numrecs;		      // Number of records in this node
	uint32_t leftsib;		      // Left sibling block (AG-relative)
	uint32_t rightsib;		      // Right sibling block (AG-relative)

	uint64_t blkno; ///< blkno; seems like this is only filled when mkfs use-crc; also reported
			///< in unit of SECTORS!
	uint64_t bb_lsn;
	uuid_t bb_uuid;
	uint32_t bb_owner;
	uint32_t bb_crc; ///< In little-endian
};

struct xal_odf_btree_iab3_lfmt {
	union xal_odf_btree_iab3_magic magic; // 'IAB3' for inode B+Tree
	uint16_t level;			      // Tree level (0 = leaf, >0 = interior)
	uint16_t numrecs;		      // Number of records in this node
	uint64_t leftsib;		      // Left sibling block (AG-relative)
	uint64_t rightsib;		      // Right sibling block (AG-relative)

	uint64_t blkno; ///< blkno; seems like this is only filled when mkfs use-crc
	uint64_t bb_lsn;
	uuid_t bb_uuid;
	uint32_t bb_owner;
	uint32_t bb_crc; ///< In little-endian
};

struct xal_odf_inobt_rec {
	uint32_t startino; ///< The lowest-numbered inode in this chunk, rounded down to the nearest
			   ///< multiple of 64, even if the start of this chunk is sparse.
	uint16_t holemask; ///< A 16 element bitmap showing which parts of the chunk are not
			   ///< allocated to inodes. Each bit represents four inodes; if a bit is
			   ///< marked here, the corresponding bits in ir_free must also be marked.
	uint8_t count;	   ///< Number of inodes allocated to this chunk
	uint8_t freecount; ///< Number of free inodes in this chunk
	uint64_t free; ///< A 64 element bitmap showing which inodes in this chunk are not available
		       ///< for allocation
};