#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Cache deterministic synthetic image bytes, never mutable test mounts."""

from contextlib import contextmanager
from functools import lru_cache, wraps
from hashlib import sha256
from inspect import signature
import json
import os
from pathlib import Path
from struct import pack, unpack
from tempfile import NamedTemporaryFile


ENVIRONMENT = "DOSBOX_FLOPPY_FIXTURE_CACHE"
MAGIC = b"FDCFIX01"
RECIPES = (
    "floppy_fixture_cache.py", "run_hxc_mfm_probe.py", "run_scp_attachment_probe.py",
    "run_86f_attachment_probe.py", "run_transcopy_attachment_probe.py",
    "run_floppy_zero_sector_probe.py",
)


@lru_cache(maxsize=1)
def _recipe_digest():
    """Invalidate cached encodings when any shared encoding recipe changes."""
    digest = sha256()
    for name in RECIPES:
        digest.update(name.encode())
        digest.update(Path(__file__).with_name(name).read_bytes())
    return digest.hexdigest()


def _argument(value):
    """Hash large buffers without serializing their contents into JSON."""
    if isinstance(value, (bytes, bytearray, memoryview)):
        return ["bytes", len(value), sha256(value).hexdigest()]
    if isinstance(value, (tuple, list)):
        return [_argument(item) for item in value]
    if isinstance(value, dict):
        return {key: _argument(item) for key, item in sorted(value.items())}
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    raise TypeError(f"Unsupported fixture argument: {type(value)}")


@contextmanager
def _locked(path):
    """Serialize Linux producers; atomic replacement protects other platforms."""
    with path.open("a+b") as lock:
        if os.name == "posix":
            import fcntl  # pylint: disable=import-outside-toplevel
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        yield


def _load(path, key):
    """Reject truncated, corrupt, or misplaced cache records."""
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return None
    if len(data) < 81 or data[:8] != MAGIC or data[8:40] != key:
        return None
    size, = unpack("<Q", data[73:81])
    payload = data[81:]
    if size != len(payload) or sha256(payload).digest() != data[40:72]:
        return None
    if data[72:73] == b"B":
        return bytearray(payload)
    return payload if data[72:73] == b"b" else None


def cached_fixture(function):
    """Return fresh bytearrays and immutable bytes for repeatable fixture inputs."""
    parameters = signature(function)

    @wraps(function)
    def wrapper(*args, **kwargs):
        location = os.environ.get(ENVIRONMENT)
        if not location:
            return function(*args, **kwargs)
        bound = parameters.bind(*args, **kwargs)
        bound.apply_defaults()
        arguments = [_recipe_digest(), Path(function.__code__.co_filename).name,
                     function.__qualname__, _argument(bound.arguments)]
        key = sha256(json.dumps(arguments, sort_keys=True).encode()).digest()
        folder = Path(location)
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / (key.hex() + ".fixture")
        with _locked(path.with_suffix(".lock")):
            result = _load(path, key)
            if result is not None:
                return result
            result = function(*args, **kwargs)
            if not isinstance(result, (bytes, bytearray)):
                raise TypeError("Cached fixture functions must return bytes or bytearray")
            record = (MAGIC + key + sha256(result).digest()
                      + (b"B" if isinstance(result, bytearray) else b"b")
                      + pack("<Q", len(result)) + result)
            temporary = None
            try:
                with NamedTemporaryFile(dir=folder, delete=False) as stream:
                    temporary = Path(stream.name)
                    stream.write(record)
                    stream.flush()
                    os.fsync(stream.fileno())
                temporary.replace(path)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
            return result
    return wrapper
