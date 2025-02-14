#!/usr/bin/env python3
import struct
import uuid

from pathlib import Path
import ctypes


def decode_inode_no(ino):
    """
    Inodes in XFS are 64bit values encoding:

    * allocation group (AG)
    * block within the AG
    * and inode offset within the block

    This function decodes the format and returns the individual parts
    """

    ag = (ino >> 32) & 0xFFFFFFFF  # Extract AG (bits 63-32)
    block_number = (ino >> 9) & 0x7FFFFFFF  # Extract block number (bits 31-9)
    inode_offset = ino & 0x1FF  # Extract inode offset (bits 8-0)

    return ag, block_number, inode_offset


def decode_uuid(uuid_field):
    """Convert a raw uuid field into a readable UUID string."""

    return str(uuid.UUID(bytes=bytes(uuid_field)))


def magic_as_text(val):
    text = []
    for _ in range(4):
        text.append(chr(val & 0xFF))
        val = val >> 8

    return "".join(text[::-1])


class Superblock(ctypes.BigEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("sb_magicnum", ctypes.c_uint32),  # __be32
        ("sb_blocksize", ctypes.c_uint32),  # __be32
        ("sb_dblocks", ctypes.c_uint64),  # __be64
        ("sb_rblocks", ctypes.c_uint64),  # __be64
        ("sb_rextents", ctypes.c_uint64),  # __be64
        ("sb_uuid", ctypes.c_ubyte * 16),  # uuid_t
        ("sb_logstart", ctypes.c_uint64),  # __be64
        ("sb_rootino", ctypes.c_uint64),  # Root inode number for the filesystem
        ("sb_rbmino", ctypes.c_uint64),  # __be64
        ("sb_rsumino", ctypes.c_uint64),  # __be64
        ("sb_rextsize", ctypes.c_uint32),  # __be32
        ("sb_agblocks", ctypes.c_uint32),  # __be32
        ("sb_agcount", ctypes.c_uint32),  # __be32
        ("sb_rbmblocks", ctypes.c_uint32),  # __be32
        ("sb_logblocks", ctypes.c_uint32),  # __be32
        ("sb_versionnum", ctypes.c_uint16),  # __be16
        ("sb_sectsize", ctypes.c_uint16),  # __be16
        ("sb_inodesize", ctypes.c_uint16),  # __be16
        ("sb_inopblock", ctypes.c_uint16),  # __be16
        ("sb_fname", ctypes.c_char * 12),  # char[XFSLABEL_MAX]
        ("sb_blocklog", ctypes.c_uint8),  # __u8
        ("sb_sectlog", ctypes.c_uint8),  # __u8
        ("sb_inodelog", ctypes.c_uint8),  # __u8
        ("sb_inopblog", ctypes.c_uint8),  # __u8
        ("sb_agblklog", ctypes.c_uint8),  # __u8
        ("sb_rextslog", ctypes.c_uint8),  # __u8
        ("sb_inprogress", ctypes.c_uint8),  # __u8
        ("sb_imax_pct", ctypes.c_uint8),  # __u8
        ("sb_icount", ctypes.c_uint64),  # __be64
        ("sb_ifree", ctypes.c_uint64),  # __be64
        ("sb_fdblocks", ctypes.c_uint64),  # __be64
        ("sb_frextents", ctypes.c_uint64),  # __be64
        ("sb_uquotino", ctypes.c_uint64),  # __be64
        ("sb_gquotino", ctypes.c_uint64),  # __be64
        ("sb_qflags", ctypes.c_uint16),  # __be16
        ("sb_flags", ctypes.c_uint8),  # __u8
        ("sb_shared_vn", ctypes.c_uint8),  # __u8
        ("sb_inoalignmt", ctypes.c_uint32),  # __be32
        ("sb_unit", ctypes.c_uint32),  # __be32
        ("sb_width", ctypes.c_uint32),  # __be32
        ("sb_dirblklog", ctypes.c_uint8),  # __u8
        ("sb_logsectlog", ctypes.c_uint8),  # __u8
        ("sb_logsectsize", ctypes.c_uint16),  # __be16
        ("sb_logsunit", ctypes.c_uint32),  # __be32
        ("sb_features2", ctypes.c_uint32),  # __be32
        ("sb_bad_features2", ctypes.c_uint32),  # __be32
        ("sb_features_compat", ctypes.c_uint32),  # __be32
        ("sb_features_ro_compat", ctypes.c_uint32),  # __be32
        ("sb_features_incompat", ctypes.c_uint32),  # __be32
        ("sb_features_log_incompat", ctypes.c_uint32),  # __be32
        ("sb_crc", ctypes.c_uint32),  # __le32
        ("sb_spino_align", ctypes.c_uint32),  # __be32
        ("sb_pquotino", ctypes.c_uint64),  # __be64
        ("sb_lsn", ctypes.c_uint64),  # __be64
        ("sb_meta_uuid", ctypes.c_ubyte * 16),  # uuid_t
        ("sb_metadirino", ctypes.c_uint64),  # __be64
        ("sb_rgcount", ctypes.c_uint32),  # __be32
        ("sb_rgextents", ctypes.c_uint32),  # __be32
        ("sb_rgblklog", ctypes.c_uint8),  # __u8
        ("sb_pad", ctypes.c_uint8 * 7),  # __u8[7]
    ]

    def __str__(self):

        sb_magicnum_text = magic_as_text(self.sb_magicnum)

        return (
            f"XFS Superblock:\n"
            f"  Magic Number: {hex(self.sb_magicnum)} '{sb_magicnum_text}'\n"
            f"  Sector Size: {self.sb_sectsize}\n"
            f"  AllocationGroupCount: {self.sb_agcount}\n"
            f"  Block Size: {self.sb_blocksize}\n"
            f"  Data Blocks: {self.sb_dblocks}\n"
            f"  Root Inode: {self.sb_rootino} | {decode_inode_no(self.sb_rootino)}\n"
            f"  UUID: {decode_uuid(self.sb_uuid)}\n"
            f"  File System Name: {self.sb_fname.decode('utf-8').rstrip()}\n"
            f"  Free Data Blocks: {self.sb_fdblocks}\n"
            f"  Free Inodes: {self.sb_ifree}\n"
            f"  CRC: {hex(self.sb_crc)}\n"
        )


# Define constants for the size of arrays used in the structure
XFS_AGI_UNLINKED_BUCKETS = 64  # Replace with the actual number of buckets if known


class XfsAgi(ctypes.BigEndianStructure):
    """
    Allocation Group Inode Information

    All fields are in BigEndian, however, I am not sure what that means for the UUID
    """

    _pack_ = 1  # Ensure tight packing
    _fields_ = [
        ("agi_magicnum", ctypes.c_uint32),
        ("agi_versionnum", ctypes.c_uint32),
        ("agi_seqno", ctypes.c_uint32),
        ("agi_length", ctypes.c_uint32),
        (
            "agi_count",
            ctypes.c_uint32,
        ),  # Specifies the number of inodes allocated for the AG
        (
            "agi_root",
            ctypes.c_uint32,
        ),  # Specifies the block number in the AG containing the root of the inode B+tree
        (
            "agi_level",
            ctypes.c_uint32,
        ),  # Specifies the number of levels in the inode B+tree
        (
            "agi_freecount",
            ctypes.c_uint32,
        ),  # Specifies the number of free inodes in the AG
        (
            "agi_newino",
            ctypes.c_uint32,
        ),  # Specifies AG-relative inode number of the most recently allocated chunk.
        (
            "agi_dirino",
            ctypes.c_uint32,
        ),  # Deprecated and not used, this is always set to NULL (-1)
        (
            "agi_unlinked",
            ctypes.c_uint32 * XFS_AGI_UNLINKED_BUCKETS,
        ),  # Hash table of unlinked (deleted) inodes that are still being reference
        ("agi_uuid", ctypes.c_ubyte * 16),  # uuid_t agi_uuid (16 bytes)
        ("agi_crc", ctypes.c_uint32),  # __be32 agi_crc
        ("agi_pad32", ctypes.c_uint32),  # __be32 agi_pad32
        ("agi_lsn", ctypes.c_uint64),  # __be64 agi_lsn
        ("agi_free_root", ctypes.c_uint32),  # __be32 agi_free_root
        ("agi_free_level", ctypes.c_uint32),  # __be32 agi_free_level
        ("agi_iblocks", ctypes.c_uint32),  # __be32 agi_iblocks
        ("agi_fblocks", ctypes.c_uint32),  # __be32 agi_fblocks
        # Padding to ensure 64-bit alignment (if needed)
        ("_pad", ctypes.c_ubyte * 4),
    ]

    def __str__(self):

        magic_text = magic_as_text(self.agi_magicnum)

        return (
            f"XFS AG inode:\n"
            f"  Magic Number: {hex(self.agi_magicnum)} '{magic_text}'\n"
            f"  UUID: {decode_uuid(self.agi_uuid)}\n"
            f"  seqno: {self.agi_seqno}\n"
            f"  length: {self.agi_length}\n"
            f"  count: {self.agi_count}\n"
            f"  root: {self.agi_root}\n"
        )


def retrieve_agi(dev, sb: Superblock, agno: int):
    """Return the Allocation Group Inode (AGI) for the given Allocation Group number"""

    sb_ofz_nbytes = agno * sb.sb_agblocks * sb.sb_blocksize

    # Seeking to the start of the Allocation Group and skipping past the superblock copy
    # and the AGF, since all we want is to index the file-system and read data out
    dev.seek(sb_ofz_nbytes + 2 * sb.sb_sectsize)

    return XfsAgi.from_buffer_copy(dev.read(sb.sb_sectsize))


def main():

    dev_path = Path("/dev/nvme1n1")

    with dev_path.open("rb") as dev:
        # Retrieve the primary Superblock
        sb = Superblock.from_buffer_copy(dev.read(4096))
        print(sb)

        allocation_group = []

        # Grab the XFSAgi header for each AG
        for agno in range(0, sb.sb_agcount):
            agi = retrieve_agi(dev, sb, agno)
            print(f"agno({agno})", agi)

            allocation_group.append(agi)


if __name__ == "__main__":
    main()
