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
 * These are not defined in xfs_types.h; thus, providing them here.
 */
typedef uint8_t __u8;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint32_t __le32;
typedef uint64_t __be64;

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
#define XALLABEL_MAX 12

#define XAL_AGF_MAGIC 0x58414746  /* 'XAGF' */
#define XAL_AGI_MAGIC 0x58414749  /* 'XAGI' */
#define XAL_AGFL_MAGIC 0x5841464c /* 'XAFL' */

/**
 * The XFS Superblock on-disk representation in v5 format
 */
struct xal_sb {
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

	char sb_fname[XALLABEL_MAX]; /* file system name */

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

struct xal_agf {
	/*
	 * Common allocation group header information
	 */
	__be32 magicnum;       /* magic number == XAL_AGF_MAGIC */
	__be32 agf_versionnum; /* header version == XAL_AGF_VERSION */
	__be32 seqno;	       /* sequence # starting from 0 */
	__be32 length;	       /* size in blocks of a.g. */
	/*
	 * Freespace and rmap information
	 */
	__be32 agf_bno_root;  /* bnobt root block */
	__be32 agf_cnt_root;  /* cntbt root block */
	__be32 agf_rmap_root; /* rmapbt root block */

	__be32 agf_bno_level;  /* bnobt btree levels */
	__be32 agf_cnt_level;  /* cntbt btree levels */
	__be32 agf_rmap_level; /* rmapbt btree levels */

	__be32 agf_flfirst;  /* first freelist block's index */
	__be32 agf_fllast;   /* last freelist block's index */
	__be32 agf_flcount;  /* count of blocks in freelist */
	__be32 agf_freeblks; /* total free blocks */

	__be32 agf_longest;   /* longest free space */
	__be32 agf_btreeblks; /* # of blocks held in AGF btrees */
	uuid_t agf_uuid;      /* uuid of filesystem */

	__be32 agf_rmap_blocks;	    /* rmapbt blocks used */
	__be32 agf_refcount_blocks; /* refcountbt blocks used */

	__be32 agf_refcount_root;  /* refcount tree root block */
	__be32 agf_refcount_level; /* refcount btree levels */

	/*
	 * reserve some contiguous space for future logged fields before we add
	 * the unlogged fields. This makes the range logging via flags and
	 * structure offsets much simpler.
	 */
	__be64 agf_spare64[14];

	/* unlogged fields, written during buffer writeback. */
	__be64 agf_lsn; /* last write sequence */
	__be32 agf_crc; /* crc of agf sector */
	__be32 agf_spare2;

	/* structure must be padded to 64 bit alignment */
};

/*
 * Size of the unlinked inode hash table in the agi.
 */
#define XAL_AGI_UNLINKED_BUCKETS 64

struct xal_agi {
	/*
	 * Common allocation group header information
	 */
	__be32 magicnum;   /* magic number == XAL_AGI_MAGIC */
	__be32 versionnum; /* header version == XAL_AGI_VERSION */
	__be32 seqno;	   /* sequence # starting from 0 */
	__be32 length;	   /* size in blocks of a.g. */
	/*
	 * Inode information
	 * Inodes are mapped by interpreting the inode number, so no
	 * mapping data is needed here.
	 */
	__be32 agi_count;     /* count of allocated inodes */
	__be32 agi_root;      /* root of inode btree */
	__be32 agi_level;     /* levels in inode btree */
	__be32 agi_freecount; /* number of free inodes */

	__be32 agi_newino; /* new inode just allocated */
	__be32 agi_dirino; /* last directory inode chunk */
	/*
	 * Hash table of inodes which have been unlinked but are
	 * still being referenced.
	 */
	__be32 agi_unlinked[XAL_AGI_UNLINKED_BUCKETS];
	/*
	 * This marks the end of logging region 1 and start of logging region 2.
	 */
	uuid_t agi_uuid; /* uuid of filesystem */
	__be32 agi_crc;	 /* crc of agi sector */
	__be32 agi_pad32;
	__be64 agi_lsn; /* last write sequence */

	__be32 agi_free_root;  /* root of the free inode btree */
	__be32 agi_free_level; /* levels in free inode btree */

	__be32 agi_iblocks; /* inobt blocks used */
	__be32 agi_fblocks; /* finobt blocks used */

	/* structure must be padded to 64 bit alignment */
};

struct xal_agfl {
	__be32 magicnum;
	__be32 seqno;
	uuid_t agfl_uuid;
	__be64 agfl_lsn;
	__be32 agfl_crc;
} __attribute__((packed));

typedef __be64 xfs_timestamp_t;

#define XAL_DINODE_MAGIC 0x494e /* 'IN' */

/*
 * Values for di_format
 *
 * This enum is used in string mapping in xfs_trace.h; please keep the
 * TRACE_DEFINE_ENUMs for it up to date.
 */
enum xal_dinode_fmt {
	XAL_DINODE_FMT_DEV,	/* xfs_dev_t */
	XAL_DINODE_FMT_LOCAL,	/* bulk data */
	XAL_DINODE_FMT_EXTENTS, /* struct xfs_bmbt_rec */
	XAL_DINODE_FMT_BTREE,	/* struct xfs_bmdr_block */
	XAL_DINODE_FMT_UUID	/* added long ago, but never used */
};

struct xal_dinode {
	__be16 di_magic;     /* inode magic # = XFS_DINODE_MAGIC */
	__be16 di_mode;	     /* mode and type of file */
	__u8 di_version;     /* inode version */
	__u8 di_format;	     /* format of di_c data */
	__be16 di_onlink;    /* old number of links to file */
	__be32 di_uid;	     /* owner's user id */
	__be32 di_gid;	     /* owner's group id */
	__be32 di_nlink;     /* number of links to file */
	__be16 di_projid_lo; /* lower part of owner's project id */
	__be16 di_projid_hi; /* higher part owner's project id */
	union {
		/* Number of data fork extents if NREXT64 is set */
		__be64 di_big_nextents;

		/* Padding for V3 inodes without NREXT64 set. */
		__be64 di_v3_pad;

		/* Padding and inode flush counter for V2 inodes. */
		struct {
			__u8 di_v2_pad[6];
			__be16 di_flushiter;
		};
	};
	xfs_timestamp_t di_atime; /* time last accessed */
	xfs_timestamp_t di_mtime; /* time last modified */
	xfs_timestamp_t di_ctime; /* time created/inode modified */
	__be64 di_size;		  /* number of bytes in file */
	__be64 di_nblocks;	  /* # of direct & btree blocks used */
	__be32 di_extsize;	  /* basic/minimum extent size for file */
	union {
		/*
		 * For V2 inodes and V3 inodes without NREXT64 set, this
		 * is the number of data and attr fork extents.
		 */
		struct {
			__be32 di_nextents;
			__be16 di_anextents;
		} __attribute__((packed));

		/* Number of attr fork extents if NREXT64 is set. */
		struct {
			__be32 di_big_anextents;
			__be16 di_nrext64_pad;
		} __attribute__((packed));
	} __attribute__((packed));
	__u8 di_forkoff;     /* attr fork offs, <<3 for 64b align */
	u_int8_t di_aformat; /* format of attr fork's data */
	__be32 di_dmevmask;  /* DMIG event mask */
	__be16 di_dmstate;   /* DMIG state info */
	__be16 di_flags;     /* random flags, XFS_DIFLAG_... */
	__be32 di_gen;	     /* generation number */

	/* di_next_unlinked is the only non-core field in the old dinode */
	__be32 di_next_unlinked; /* agi unlinked list ptr */

	/* start of the extended dinode, writable fields */
	__le32 di_crc;	       /* CRC of the inode */
	__be64 di_changecount; /* number of attribute changes */
	__be64 di_lsn;	       /* flush sequence */
	__be64 di_flags2;      /* more random flags */
	__be32 di_cowextsize;  /* basic cow extent size for file */
	__u8 di_pad2[12];      /* more padding for future expansion */

	/* fields only written to during inode creation */
	xfs_timestamp_t di_crtime; /* time created */
	__be64 di_ino;		   /* inode number */
	uuid_t di_uuid;		   /* UUID of the filesystem */

	/* structure must be padded to 64 bit alignment */
};

struct xal_xfs_dir2_sf_hdr {
	uint8_t count;	   /* Number of directory entries */
	uint8_t i8count;   /* Count of 8-byte inode numbers (vs 4-byte) */
	uint8_t parent[8]; /* Parent directory inode number */
} __attribute__((packed));