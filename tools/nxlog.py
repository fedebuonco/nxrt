"""The generator's logger: the same shape as nxrt's C logger.

    [2026-10-26T01:45:12.345Z] [INFO] [gen] [run=<id>] message

NXRT_LOG selects the level (error / warn / info / debug / trace; `gen=debug` also works), NXRT_LOG_FILE appends to a
file too, and NXRT_RUN_ID adds the manager's run id. The progress lines from report.py keep their own format: the start
screen parses them.
"""
import logging
import os
import re
import sys
import time

_LEVELS = {
    "error": logging.ERROR,
    "warn": logging.WARNING,
    "warning": logging.WARNING,
    "info": logging.INFO,
    "debug": logging.DEBUG,
    "trace": logging.DEBUG,
}
_run = ""
_configured = False


class _Formatter(logging.Formatter):
    converter = time.gmtime

    def formatTime(self, record, datefmt=None):
        ct = self.converter(record.created)
        return time.strftime("%Y-%m-%dT%H:%M:%S", ct) + f".{int(record.msecs):03d}Z"

    def format(self, record):
        if record.levelno == logging.WARNING:
            record.levelname = "WARN"
        return super().format(record)


class _RunFilter(logging.Filter):
    def filter(self, record):
        record.run = _run
        return True


def _level_from_env():
    level = "info"
    for token in os.environ.get("NXRT_LOG", "").split(","):
        token = token.strip()
        if not token:
            continue
        if "=" in token:
            cat, val = token.split("=", 1)
            if cat.strip().lower() == "gen":
                level = val.strip()
        else:
            level = token
    return _LEVELS.get(level.lower(), logging.INFO)


def setup():
    """Configure the generator's logger once, from the environment."""
    global _run, _configured
    if _configured:
        return
    raw = re.sub(r"[^A-Za-z0-9._-]", "", os.environ.get("NXRT_RUN_ID", ""))[:63]
    _run = raw
    logger = logging.getLogger("nxrt.gen")
    logger.setLevel(_level_from_env())
    logger.propagate = False
    logger.addHandler(logging.NullHandler())
    fmt = "[%(asctime)s] [%(levelname)s] [gen]" + (" [run=%(run)s]" if _run else "") + " %(message)s"
    formatter = _Formatter(fmt)
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(formatter)
    handler.addFilter(_RunFilter())
    logger.addHandler(handler)
    path = os.environ.get("NXRT_LOG_FILE")
    if path:
        try:
            fh = logging.FileHandler(path, mode="a")
        except OSError:
            fh = None
        if fh:
            fh.setFormatter(formatter)
            fh.addFilter(_RunFilter())
            logger.addHandler(fh)
    _configured = True


log = logging.getLogger("nxrt.gen")
