"""
Virtual block-device using zram
===============================

Crete a single block device of a given size in GB.

"""

import os
import sys
import subprocess
import errno
import logging as log
from pathlib import Path
from argparse import ArgumentParser, Namespace
from cijoe.core.command import Cijoe


def add_args(parser: ArgumentParser):
    """Optional function for defining command-line arguments for this script"""

    parser.add_argument(
        "--size-in-gb",
        type=int,
        default=1,
        help="Size of the zram device, in bytes",
    )
    parser.add_argument(
        "--dev-path",
        type=str,
        default=Path("/dev/zram0"),
        help="Path to the zram device",
    )


def main(args: Namespace, cijoe: Cijoe):
    """Entry-point of the cijoe-script"""

    size_bytes = int(args.size_in_gb << 30)

    cijoe.run(f'sudo swapoff {args.dev_path} || echo "Above error is OK."')
    cijoe.run(f'sudo zramctl --reset {args.dev_path} || echo "Above is OK."')
    cijoe.run(f'sudo modprobe -r zram || echo "Above is OK."')

    err, state = cijoe.run(f"sudo modprobe zram num_devices=1")
    if err:
        log.error("Failed loading zram module.")
        return err

    err, state = cijoe.run(f"sudo zramctl --size {size_bytes} {args.dev_path}")
    if err:
        log.error("Failing resizing device")
        return err

    err, state = cijoe.run(f"sudo chown $USER:$USER /dev/zram0")
    if err:
        log.error("...")
        return err

    return 0


if __name__ == "__main__":
    main()
