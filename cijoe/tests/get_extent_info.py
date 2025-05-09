#!/usr/bin/env python3
import os
import sys
import json
import argparse
import subprocess
from pathlib import Path
import re

COLUMNS = ["startoffset", "endoffset", "startblock", "endblock"]


def get_extents(path: Path):
    """The given path is expected to be absolute"""

    regex = (
        r"^(?P<ident>\d+):\s+"
        r"\[(?P<startoffset>\d+)\.\.(?P<endoffset>\d+)\]:\s+"
        r"(?P<startblock>\d+)\.\.(?P<endblock>\d+)$"
    )

    cmd = ["xfs_bmap", path]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    if proc.returncode:
        print("Failed running: %s" % " ".join(cmd))
        return proc.returncode

    extents = []
    for line in proc.stdout.splitlines():
        match = re.match(regex, line.strip())
        if match:
            extent = {key: int(val) for key, val in match.groupdict().items()}
            extents.append([extent.get(col) for col in COLUMNS])

    return extents


def main(args):
    result = {}

    for root, _, files in os.walk(args.mountpoint):
        for name in files:
            path = Path(root) / name

            result[str(path)] = (os.stat(path).st_ino, get_extents(path))

    args.output.write_text(json.dumps(result))

    return 0


def parse_args():
    parser = argparse.ArgumentParser(
        description="Extract file extent-info from mounted file-system"
    )

    parser.add_argument(
        "--mountpoint", type=Path, required=True, help="Path to the mountpoint."
    )
    parser.add_argument(
        "--output",
        default=Path.cwd() / "bmap.json",
        type=Path,
        help="Path to the output file or directory.",
    )

    return parser.parse_args()


if __name__ == "__main__":
    sys.exit(main(parse_args()))
