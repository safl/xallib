===========================================
 Mapping-layer based on XFS on-disk format
===========================================

The goal is to build a data-access library capable of reading data from XFS file
systems without requiring the file system to be mounted or the storage device to
be managed by the operating system.

This is necessary in scenarios such as:

* The storage device driver is detached from the OS:

  - Operating in user space (e.g., SPDK/NVMe).
  - Operating on a peripheral device (e.g., BaM).

The library's minimal requirements include:

* Traversing the file system

  - Reading directory contents.
  - Accessing inode properties.
  - Reading regular file contents.

The focus is solely on an efficient data-access library, with no interest in
forensics, recovery, or repair.

Tooling
=======

To inspect and gain familarity with the on-disk format, then there are excellent
tools to do so. These include ``xfs_db`` and ``xfs_metadump``, which understand
the XFS on-disk format.

In addition, ``dd``` for reading/write, along with ``hexdump`` / ``bless`` for
inspecting the data, either by accesing a storage device directly, or dumping
it first.

Nomenclature
============

These come in handy when learning about the on-disk format. Note; meta-data
is stored in big-endian format, thus on x86 systems these need conversion to
little-endian when reading them

Superblock (SB)
  ...

Sector
  A unit which typically match the minimal I/O size; equivalent to an **lba** of
  an NVMe device.

Inode
  ...

Allocation Group (AG)
  ...

  Allocaton Group Free Block (AGF)

    ...

  Allocaton Group Inode (AGI)

    ...

  Allocaton Group Free List (AGFL)

    ...

Block

  ...

Data Structures
===============

* Superblock

* AGI

* Inode

Visualized
----------

Here is a visualization of an Allocation Group, the headers including the
Superblock followed by Allocation Group Headers.

+----+-----+-----+------+------------------------------------------+
| sb | agf | agi | agfl |          ... blocks ...                  |
+----+-----+-----+------+------------------------------------------+
| First Allocation Group (AG) starting with the primary Superblock |
+------------------------------------------------------------------+

+----+-----+-----+------+------------------------------------------+
| sb | agf | agi | agfl |          ... blocks ...                  |
+----+-----+-----+------+------------------------------------------+
| Second Allocation Group (AG) starting with a Superblock copy     |
+------------------------------------------------------------------+

...

+----+-----+-----+------+------------------------------------------+
| sb | agf | agi | agfl |          ... blocks ...                  |
+----+-----+-----+------+------------------------------------------+
| Nth Allocation Group starting a Superblock copy                  |
+------------------------------------------------------------------+

This ordering is why the Allocation Group is regarded as its own little
file-system, because it is. Each allocation group manage a collection of blocks
with all that entails.

Traversing the file system
==========================

1) Retrieve the **primary** SB

This is done to obtain basic addressing values and information such as
sector-size, block-size and number of AGs. As you can see above then each AG has
a superblock. These are all copies of the superblock from the first AG.

Hence, the notion of the "primary" SB.

2) Retrieve meta-data for all Allocation Groups

Read all AG meta-data from disk, this is done ahead of time before doing any
further processing.

3) Read all inodes and associate them with Allocation Groups

Inodes are stored in inode chunks, each containing 64 inodes within an
allocation group. To reduce disk access when traversing the file system, we read
all inodes into memory within the allocation group structure.
Ideally, we store them in an contigous array, allowing lookup using the decoded
inode_number.

To achieve this, we need to walk the einode B+tree of pointed to by the
Allocation Group Inode meta-header. So, we start here:

* ``agi->agi_root``


3) Traverse Inodes

Inode numbers in XFS are not just consequitive integers, they are encoding in
format describing their location relative. 

-- directories on "local" / short-form

When i8count is set, then the first i8count number of inode numbers are 64bit.

- Does this mean that they have the AG encoded?
- When "normalizing" it, should it encode the AG then?

Inode Allocation
================

The superblock keeps a initial count (sb.icount) of allocated inodes. However,
as inodes are allocated and freed during the lifetime of the file-system, then
it seems like this on-disk value is not updated.
However, it seems like the counter in each allocation group get updated. Thus,
to determine the number of currently allocated iondes, then one can do:

  icount = sum((ag.agi_count for ag in sb.ags))

This would be a sensible value to use for the inode-memory pool.

Inode Numbers (Encoding)
========================

The inode numbers in XFS are not simply increasing continously rising integer
values. Rather they are encodings of location information and come in the form
of absolute an relative location information.

* **Absolute** Inode Numbers (64bit)

  - Consists of: "AG | Block in AG | Inode in Block"
  - This representation is used in the superblock
  - This representation is used in directory entries

* **Relative** Inode Numbers (32bit)

  - Consists of: "Block in AG | Inode in Block"
  - This representation is found in AG inode structures ? Give examples here...

To decode the above, then one can use the superblock data of:

sb.agblklog
  The size of an allocation group in unit of **blocks** represented as a log2
  value rounded up.

sb.inoblog
  The number of inodes per block, equivalent to blocksize / inodesize,
  represented as a log2 value rounded up.

Thus encoded on the form::

      ┌────────────────────────────────┐    
      │ Inode Number                   │    
      ┌────────────────────────────────┐    
  MSB │ AG# │ sb.agblklog │ sb.inoblog │ LSB
      └────────────────────────────────┘    
            │ Always fits in 32 bit    │    
      ┌─────┴──────────────────────────┤    
      │ May fit in 32 bit              │    
      └────────────────────────────────┘    

Directories
===========

An interesting experiment: On a nearly full mount point, repeatedly copy
directories with large files. You'll see minimal space usage since the data
isn't duplicated. The filesystem stores more data than physically fits, but no
extra space is consumed because the data blocks are shared.

However, in a directory listing, the same inode may appear multiple times. These
listings generally take the form of tuples:

  (name, ino)

This behavior is due to the data-blocks being shared, the terms here are
copy-on-write (cow) strategy and reflinking.

Files
=====

In XFS, a logical block offset in a file doesn’t directly map to a physical
disk block due to COW and reflinking. When data blocks are shared, extending
or modifying a file creates a copy to preserve shared content, breaking the
1:1 mapping.

B-Trees
=======

The storage format of an inode can be one of inode which can be one of:
``FMT_LOCAL``, ``FMT_EXTENT``, and ``FMT_BTREE``. The latter form is a B+Tree.
This storage format is used for large directories and for fragmented files.

Additionally, then within an allocation-group then inodes are stored in B+Trees,
one for allocated inodes another for free inodes. There are probably other
usages, but these are mentioned as they are uses which the library currently
supports decoding. Specifically:

* Inode B+Tree

  - Magic number ``IAB3``
  - Allocated inodes
  
* Directory B+tree

  - Directory contents: tree of directory blocks

* File B+tree ``BMA3``

  - File-contents: collection of extents / mapping file to disk-blocks

  - Return error if level == 0, the root-node should not be a leaf, since it
    would not need to be in FMT_BTREE.
  
  - Decode records encoded in the "dinode" as filesystem block numbers
  
    - Call leaf-decoder on rightsub if level == 1, that means all the children
      are leafs, the leaf-decoder run recursively for siblings
      
    - Call node-decoder if level > 1, the node-decode will run recursively for
      siblings

  - Follow the filesystem block pointers, now assuming magic-number BMA3

  - Read the block; and call the appriate decoder

Observations
============

These are notes on the on-disk-format that I did not find described in the XFS
spec.

B+Tree File Extent Lists
------------------------

Although the root-node is an internal node similar to other nodes in the B+Tree,
then it differs from other internal nodes on at least two essential areas.

1) The root-node does **not** have fields: magic, leftsib, rightsib, that you
otherwise find in a B+Tree internal node and even in leafs. Instead of the
magic-value then the dinode.format combined with the ftype determine the content
of the data-fork.
Leftsib and rightsib are not there which is logically sound, since the root
would not be a root node if it had siblings.

2) There is less space as the node is embedded within the inode, inodes are
usually 256 or 512 bytes, whereas an internal node is blocksize amount of bytes
which usually is at least 512 bytes and commonly 4096. Consequently, then an
internal node can contain significantly more records than a root node. This is
good to be aware of, since the offsets to keys and pointers begin at variable
offsets.

In addition, then, starting with the root node, then children of the root in the
three are pointed to by **records**. However, internal-node and leafs also have
leftsib and rightsib pointers. However, to traverse the three starting at the
root-node, then you only need to use the pointers within the records as these
point to all immediate children of the root node.

Thus, given any child, then one kan move left/right until reaching 0xFFF...F
which indicates that there are no more sibling in the given direction.
However, using the sibling pointers it superflous when starting at the root and
processing filesystem block numbers from the records.

Appendix
========

The implementation of the XFS Access Library (xal) is done by reading the
material below, dumping meta-data and data from XFS formated disks for
inspection (hexdump), and experimental data-access scripts implemented in
Python. This approach was taken as I want the library to be BSD-3 and do not
want to be GPL-v2 infected.

* https://www.usenix.org/system/files/login/articles/140-hellwig.pdf

* https://www.kernel.org/pub/linux/utils/fs/xfs/docs/xfs_filesystem_structure.pdf
