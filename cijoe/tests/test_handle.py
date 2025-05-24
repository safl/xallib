import logging as log
from pprint import pprint
from pathlib import Path


def test_open_close(cijoe):

    log.error(cijoe.config)

    dev_path = cijoe.getconf("xal.dev_path", None)

    err, state = cijoe.run(f"xal {dev_path}")
    assert not err
