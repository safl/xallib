#!/usr/bin/env python3
"""
Virtual block-device using loop device
======================================

Creates a file-backed block device using a loop device. The file is stored on disk.
"""

import sys
import logging as log
from argparse import ArgumentParser, Namespace
from cijoe.core.command import Cijoe


def add_args(parser: ArgumentParser):
    parser.add_argument(
        "--size-in-gb",
        type=int,
        default=1,
        help="Size of the block device (in GB)",
    )
    parser.add_argument(
        "--dev-path",
        type=str,
        default="/dev/loop7",
        help="Loop device to use (e.g., /dev/loop7)",
    )
    parser.add_argument(
        "--file-path",
        type=str,
        default="/tmp/loopback.img",
        help="Backing file for loop-device",
    )


def main(args: Namespace, cijoe: Cijoe):
    """Entry-point of the cijoe-script"""

    size_bytes = int(args.size_in_gb << 30)
    dev_path = cijoe.getconf("xal.dev_path", args.dev_path)
    file_path = cijoe.getconf("xal.file_path", args.file_path)

    if "loop" not in dev_path:
        log.info(f"Substr 'loop' not in dev_path({dev_path}); skipping")
        return 0

    # Clean up any previous setup
    cijoe.run("sudo modprobe loop max_loop=8 || true")
    cijoe.run(f"sudo umount {dev_path} || true")
    cijoe.run(f"sudo losetup -d {dev_path} || true")

    # cijoe.run(f"sudo rm -f {file_path}")

    # Create /dev/loop7 if it doesn't exist
    dev_minor = int(dev_path.replace("/dev/loop", ""))
    cijoe.run(f"test -b {dev_path} || sudo mknod {dev_path} b 7 {dev_minor}")
    cijoe.run(f"sudo chown root:disk {dev_path}")
    cijoe.run(f"sudo chmod 660 {dev_path}")

    # Create the backing file on disk
    err, _ = cijoe.run(f"fallocate -l {size_bytes} {file_path}")
    if err:
        log.error("Failed to create backing file.")
        return err

    # Associate the file with the loop device
    err, _ = cijoe.run(f"sudo losetup {dev_path} {file_path}")
    if err:
        log.error("Failed to set up loop device.")
        return err

    # Optional: chown the loop device to the user
    cijoe.run(f"sudo chown $USER:$USER {dev_path}")

    log.info(f"Loop device ready at {dev_path}, backed by {file_path}")
    return 0
