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

Building
========

Dependencies:

* ``xnvme`` >= 0.7.0 — must be installed and visible to ``pkg-config``
* ``librt``

The default ``make`` target runs clean, configure, build, and install in one
shot::

   make

For a debug build::

   BUILD_TYPE=debug make

Individual targets are also available::

   make configure
   make build
   make install
   make test


Limits
======

Unlike ``xfs_bmap``, **xal** stores only file extents, not directory extents.
This is intentional, as **xal** provides a data structure containing the parsed
contents of directory extents via ``xal_index()``.

Instead of reading directory blocks from disk, one can use the, in-memory,
decoded file system tree rooted at ``xal->root``.

Tooling
=======

To inspect and gain familarity with the on-disk format, then there are excellent
tools to do so. These include ``xfs_db`` and ``xfs_metadump``, which understand
the XFS on-disk format.

In addition, ``dd``` for reading/write, along with ``hexdump`` / ``bless`` for
inspecting the data, either by accesing a storage device directly, or dumping
it first.

API Usage
=========

The typical call sequence is:

1. Open a device handle with ``xnvme_dev_open()``.
2. Call ``xal_open()`` to read the superblock and AG headers into ``struct xal``.
3. Call ``xal_dinodes_retrieve()`` to read all inodes from disk.
4. Call ``xal_index()`` to build the in-memory directory tree rooted at ``xal->root``.
5. Use ``xal_get_root()``, ``xal_walk()``, ``xal_get_inode()``, ``xal_get_extents()``, etc.
6. Call ``xal_close()`` and ``xnvme_dev_close()`` when done.

Example::

   struct xnvme_opts xnvme_opts = {0};
   struct xal_opts opts = { .be = XAL_BACKEND_XFS };
   struct xnvme_dev *dev;
   struct xal *xal;
   int err;

   xnvme_opts_set_defaults(&xnvme_opts);
   dev = xnvme_dev_open("/dev/nvme0n1", &xnvme_opts);

   err = xal_open(dev, &xal, &opts);
   err = xal_dinodes_retrieve(xal);
   err = xal_index(xal);

   xal_walk(xal, xal_get_root(xal), my_callback, NULL);

   xal_close(xal);
   xnvme_dev_close(dev);

If ``opts.be`` is left as 0, ``xal_open()`` auto-selects the backend: if the
device URI is found in ``/proc/mounts`` the ``FIEMAP`` backend is chosen,
otherwise ``XFS`` is used.

Backends
========

XFS
---

The XFS backend reads directly from the raw block device by parsing the XFS
on-disk format. The filesystem does **not** need to be mounted — this is the
primary use-case for the library, enabling access from user-space drivers
(e.g., SPDK/NVMe) or peripheral devices where the OS has no control over the
storage.

FIEMAP
------

The FIEMAP backend uses the kernel's ``FS_IOC_FIEMAP`` ioctl to read file
extent information, and standard ``opendir``/``readdir`` to walk the directory
tree. The filesystem **must** be mounted. This backend provides a simpler
integration and supports path-based inode and extent lookup via ``xal_get_inode()``/
``xal_get_extents()``.

Inotify watching
~~~~~~~~~~~~~~~~

When opened with a ``watch_mode`` other than ``XAL_WATCHMODE_NONE``, an
inotify watch is registered for every directory during ``xal_index()``.
A background thread started with ``xal_watch_filesystem()`` then processes
events. The watched event mask per directory is: ``IN_CREATE``,
``IN_DELETE``, ``IN_MOVE``, ``IN_MODIFY``, ``IN_ATTRIB``,
``IN_CLOSE_WRITE``, and ``IN_UNMOUNT``.

Watch modes
~~~~~~~~~~~

``XAL_WATCHMODE_NONE``
  No inotify setup. The xal struct will never be marked dirty automatically.

``XAL_WATCHMODE_DIRTY_DETECTION``
  Any filesystem event marks the xal struct as dirty. The caller detects
  this with ``xal_is_dirty()`` and must re-call ``xal_index()`` to rebuild
  the tree.

``XAL_WATCHMODE_EXTENT_UPDATE``
  File-modification events (``IN_MODIFY``, ``IN_CLOSE_WRITE``) trigger an
  automatic in-place extent refresh for the affected file via a new FIEMAP
  call, coordinated with ``seq_lock`` so concurrent readers remain safe.
  Structural changes (``IN_CREATE``, ``IN_DELETE``, ``IN_MOVE``) still mark
  the struct dirty, as they require a full re-index.

File lookup modes
~~~~~~~~~~~~~~~~~

The path-based inode and extent lookup implementation depends on which
*lookup mode* is selected.

``XAL_FILE_LOOKUPMODE_TRAVERSE``
  Default. Searches the in-memory tree from ``xal->root`` using binary
  search at each directory level. Entries are sorted alphabetically at
  index time to make this possible.

``XAL_FILE_LOOKUPMODE_HASHMAP``
  At index time every inode is inserted into a hash map keyed by its
  absolute path. ``xal_get_inode()`` then resolves in O(1). Trade-off:
  higher memory usage proportional to the number of inodes.

Nomenclature
============

These come in handy when learning about the on-disk format. Note; meta-data
is stored in big-endian format, thus on x86 systems these need conversion to
little-endian when reading them

Superblock (SB)
  The primary superblock occupies the first sector of the first AG. It records
  the filesystem geometry: block size, sector size, inode size, number of AGs,
  and the root inode number. Every subsequent AG begins with a copy of the
  superblock for redundancy; these copies are updated but the one at offset 0
  is considered authoritative.

Sector
  A unit which typically match the minimal I/O size; equivalent to an **lba** of
  an NVMe device.

Inode
  The on-disk metadata record for a file or directory. Stores the file type,
  size in bytes, and one of three data-fork formats: ``FMT_LOCAL`` (data fits
  inline within the inode itself), ``FMT_EXTENT`` (a flat array of extents),
  or ``FMT_BTREE`` (root node of an extent B+Tree, used when the file is
  heavily fragmented). Inode numbers encode the AG number, block within that
  AG, and position of the inode within that block.

Allocation Group (AG)
  A self-contained sub-filesystem. The storage is divided into a fixed number
  of equal-sized AGs at mkfs time. Each AG manages its own block and inode
  allocation independently, which enables parallel allocation and makes the
  filesystem scale on wide storage. Each AG starts with its own copy of the
  superblock followed by three AG-specific headers: AGF, AGI, and AGFL.

  Allocaton Group Free Block (AGF)

    Tracks free-block accounting for the AG. Contains the roots of two
    free-space B+Trees: one ordered by block number, the other by run length.

  Allocaton Group Inode (AGI)

    Tracks inode allocation for the AG. Contains ``agi_count`` (the number
    of currently allocated inodes) and ``agi_root``, the block address of the
    root of the inode B+Tree (magic ``IAB3``).

  Allocaton Group Free List (AGFL)

    A small circular array of pre-allocated spare blocks reserved exclusively
    for B+Tree splits within the AG. Having these blocks set aside ensures
    that space-management operations always have somewhere to write even when
    the AG is nearly full.

Block

  The fundamental allocation unit of the filesystem, sized at mkfs time
  (commonly 4096 bytes). All data and metadata are read and written in whole
  blocks. The block size is always a power of two and must be at least as
  large as the sector size.

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

Testing
=======

In addition to the synthetic generated files and folders generated with
``setup_files.py``, then this::

  oi_download_images \
    --base_dir ./openimages \
    --labels Airplane Horse Fish Pizza Television
    --limit 50000

Was attempted as a potential dataset, however, it fetches files individually
which is very slow. Thus, currently exploring the use of
``Places365-Standard Dataset`` by downloading via::

  wget http://data.csail.mit.edu/places/places365/places365standard_easyformat.tar

To reproduce things, then using this seems simpler, as it would simple to
checksum the archive.

Missing: Inode Allocation B+Tree
--------------------------------

The inode-allocation B+Tree is currently only tested in a scenario where
the root-node is level 0, e.g. a leaf-node. This must be expanded to include
``level=1`` and ``level=2``. To ensure that the decoding is capable of
processing root-node, internal-node with leaf children, internal-node with other
internal nodes as children.