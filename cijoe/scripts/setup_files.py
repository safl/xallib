"""
Populate a mountpoint with files and dictories of different lengths
===================================================================

Format the given block device with XFS
"""

import logging as log
from argparse import ArgumentParser, Namespace
from pathlib import Path
from cijoe.core.command import Cijoe


def populate(args: Namespace, cijoe: Cijoe):

    directories = [
        ("few", "1", 3),
        ("few", "512", 2),
        ("few", "1G", 1),
        ("few", "100K", 3),
        ("many", "41K", 100),
        ("many", "154K", 100),
        ("many", "500K", 100),
    ]

    for name, size, count in directories:
        dir = args.mountpoint / f"{name}"

        cijoe.run(f"mkdir -p {dir}")

        for cur in range(count):
            filepath = dir / f"{size}-{cur}-{count}.bin"
            err, state = cijoe.run(
                f"dd if=/dev/urandom of={filepath} bs={size} count=1"
            )
            if err:
                return err

    return 0


def add_args(parser: ArgumentParser):
    """Optional function for defining command-line arguments for this script"""

    parser.add_argument(
        "--dev-path",
        type=Path,
        help="Path to the block device",
    )
    parser.add_argument(
        "--mountpoint",
        type=Path,
        help="Path to mountpoint",
    )


def main(args: Namespace, cijoe: Cijoe):
    """Entry-point of the cijoe-script"""

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

    # Create a bunch of files
    if err := populate(args, cijoe):
        return err

    err, state = cijoe.run(f"sudo umount {args.mountpoint}")
    if err:
        log.error(f"mountpoint({args.mountpoint}); failed un-mounting")
        return err

    return 0


if __name__ == "__main__":
    main()
