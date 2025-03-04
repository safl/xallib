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


Appendix
========

The implementation of the XFS Access Library (xal) is done by reading the
material below, dumping meta-data and data from XFS formated disks for
inspection (hexdump), and experimental data-access scripts implemented in
Python. This approach was taken as I want the library to be BSD-3 and do not
want to be GPL-v2 infected.

* https://www.usenix.org/system/files/login/articles/140-hellwig.pdf

* https://www.kernel.org/pub/linux/utils/fs/xfs/docs/xfs_filesystem_structure.pdf
