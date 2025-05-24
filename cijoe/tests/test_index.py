import logging as log
from pprint import pprint
from pathlib import Path


def test_compare_to_find(cijoe):

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    paths = {
        "find": Path(cijoe.getconf("xal.artifacts.path")) / "find.output",
        "xal": Path(cijoe.getconf("xal.artifacts.path")) / "xal_find.output",
    }
    indexes = {
        "find": [],
        "xal": [],
    }

    # Have 'xal' produce the 'find-like' index
    err, state = cijoe.run(f"xal --find {dev_path} > {paths['xal']}")
    assert not err

    for key, path in paths.items():
        for line in sorted(paths[key].read_text().splitlines()):
            indexes[key].append(
                line.replace(dev_path if key == "xal" else mountpoint, "")
            )

    diffs = []
    for expected, got in zip(indexes["find"], indexes["xal"]):
        if expected == got:
            continue

        diffs.append({"expected": expected, "got": got})

    assert not diffs
