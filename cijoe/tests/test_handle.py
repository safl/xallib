import logging as log
from pprint import pprint

def test_open_close(cijoe):

    pprint(cijoe.config)
    log.error(cijoe.config)

    dev_path = cijoe.getconf("xal.dev_path", None)
    
    err, state = cijoe.run(f"xal -v {dev_path}")
    assert not err
