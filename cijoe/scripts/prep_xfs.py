#!/usr/bin/env python3
"""
Format block device with XFS
============================

Format the given block device with XFS
"""

import logging as log
from argparse import ArgumentParser, Namespace
from cijoe.core.command import Cijoe
from pathlib import Path


def umount(args: Namespace, cijoe: Cijoe) -> int:
    """Unmount the file-system at args.mountpoint"""

    err, state = cijoe.run(f"sudo umount {args.mountpoint}")
    if err:
        log.error(f"mountpoint({args.mountpoint}); failed un-mounting")
        return err

    return 0


def mount(args: Namespace, cijoe: Cijoe) -> int:
    """
    Does the following:

    * Creates directory at 'args.mountpoint' to use as mountpoint; can exist
    * Mounts 'args.mountpoint' to it
    * Changes ownership of it any anything inside to $USER

    Returns error in case any of the above fails, or if mountpoint is already mounted.
    """

    err, state = cijoe.run(f"mountpoint {args.mountpoint}")
    if not err:
        log.error(f"mountpoint({args.mountpoint}); already mounted")
        return err

    err, state = cijoe.run(f"sudo mkdir -p {args.mountpoint}")
    if err:
        log.error(f"mountpoint({args.mountpoint}); failed creating")
        return err

    err, state = cijoe.run(f"sudo mount {args.dev_path} {args.mountpoint}")
    if err:
        log.error(f"mountpoint({args.mountpoint}); failed mounting")
        return err

    err, state = cijoe.run(f"sudo chown -R $USER:$USER {args.mountpoint}")
    if err:
        log.error(f"chown; failed")
        return err

    return 0


def add_args(parser: ArgumentParser):
    """Optional function for defining command-line arguments for this script"""

    parser.add_argument(
        "--dev-path",
        type=str,
        help="Path to the block device",
    )
    parser.add_argument(
        "--mountpoint",
        type=Path,
        help="Path to mountpoint",
    )


def main(args: Namespace, cijoe: Cijoe):
    """Entry-point of the cijoe-script"""

    err, state = cijoe.run(
        f"sudo mkfs.xfs -b size=4096 -n size=8192 -f {args.dev_path}"
    )
    if err:
        log.error("Failed creating filesystem")
        return err

    return 0


if __name__ == "__main__":
    main()
