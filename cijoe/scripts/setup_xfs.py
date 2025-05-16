"""
Format block device with XFS
============================

Format the given block device with XFS
"""

import logging as log
from argparse import ArgumentParser, Namespace
from cijoe.core.command import Cijoe


def add_args(parser: ArgumentParser):
    """Optional function for defining command-line arguments for this script"""

    parser.add_argument(
        "--dev-path",
        type=str,
        help="Path to the block device",
    )


def main(args: Namespace, cijoe: Cijoe):
    """Entry-point of the cijoe-script"""

#    err, state = cijoe.run(f"sudo mkfs.xfs -f {args.dev_path}")
    err, state = cijoe.run(f"sudo mkfs.xfs -b size=4096 -n size=16384 -f {args.dev_path}")
    if err:
        log.error("Failed creating filesystem")
        return err

    return 0

if __name__ == "__main__":
    main()
