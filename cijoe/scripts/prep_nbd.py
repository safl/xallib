#!/usr/bin/env python3
"""
Virtual block-device using nbd + nbdkit
=======================================

Create a single block device of a given size in GB, backed by RAM.

This uses `nbdkit memory` as a drop-in replacement for zram in environments
like CI where zram is not available.

"""

import os
import sys
import subprocess
import logging as log
from pathlib import Path
from argparse import ArgumentParser, Namespace
from cijoe.core.command import Cijoe


def add_args(parser: ArgumentParser):
    """Define command-line arguments for this script"""

    parser.add_argument(
        "--size-in-gb",
        type=int,
        default=1,
        help="Size of the memory-backed device, in GB",
    )
    parser.add_argument(
        "--dev-path",
        type=str,
        default="/dev/nbd0",
        help="Path to the nbd device",
    )
    parser.add_argument(
        "--socket-path",
        type=str,
        default="/tmp/nbd.sock",
        help="UNIX socket path for nbdkit",
    )


def main(args: Namespace, cijoe: Cijoe):
    """Entry-point of the cijoe-script"""

    size_bytes = args.size_in_gb << 30

    # Unload previous client if active
    cijoe.run(f"sudo umount {args.dev_path} || true")
    cijoe.run(f"sudo nbd-client -d {args.dev_path} || true")
    cijoe.run("sudo killall -q nbdkit || true")
    # cijoe.run(f"sudo rm -f {args.socket_path} || true")

    # Load nbd module if not loaded
    err, _ = cijoe.run("sudo modprobe nbd max_part=8 nbds_max=1")
    if err:
        log.error("Failed to load nbd module")
        return err

    # Start nbdkit with a memory-backed device
    # " --exit-with-parent"
    start_cmd = (
        f"sudo nbdkit"
        " --exportname=xal"
        f" -U {args.socket_path}"
        f" memory size={size_bytes}"
        " &"
    )
    err, _ = cijoe.run(start_cmd)
    if err:
        log.error("Failed to start nbdkit with memory backend")
        return err

    # Give it time to create the socket
    for _ in range(20):
        err, _ = cijoe.run(f"test -S {args.socket_path}")
        if err == 0:
            break
        time.sleep(0.1)
    else:
        log.error(f"Socket {args.socket_path} not created in time")
        return 1

    # Connect the NBD device
    connect_cmd = f"sudo nbd-client -name=xal -unix {args.socket_path} {args.dev_path}"
    err, _ = cijoe.run(connect_cmd)
    if err:
        log.error(f"Failed to connect nbd device at {args.dev_path}")
        return err

    err, _ = cijoe.run(f"sudo chown $USER:$USER {args.dev_path}")
    if err:
        log.warning("Could not change ownership of device")

    log.info(f"Memory-backed block device available at {args.dev_path}")
    return 0
