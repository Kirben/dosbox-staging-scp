#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test immutable synthetic fixture caching, including concurrent processes."""

from contextlib import ExitStack
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
import unittest
from unittest.mock import patch

from floppy_fixture_cache import ENVIRONMENT, cached_fixture


@cached_fixture
def concurrent_producer(record, payload):
    """Record actual computation so concurrent cache hits can be counted."""
    with Path(record).open("a", encoding="ascii") as output:
        output.write("computed\n")
    time.sleep(0.1)
    return bytearray(payload)


class FixtureCacheTest(unittest.TestCase):
    """Validate identity, freshness, invalidation and corrupt-record recovery."""

    def test_cache_returns_independent_mutable_buffers(self):
        """Equivalent calls share an encoding, but never share mutable results."""
        calls = []

        @cached_fixture
        def producer(payload, marker=3):
            calls.append(marker)
            return bytearray(payload) + bytes([marker])

        with TemporaryDirectory() as temporary, patch.dict(os.environ, {ENVIRONMENT: temporary}):
            self.assertEqual(producer(b"a"), bytearray(b"a\3"))
            result = producer(payload=b"a", marker=3)
            result[0] = 0
            self.assertEqual(producer(b"a"), bytearray(b"a\3"))
            self.assertEqual(calls, [3])
            self.assertEqual(producer(b"a", 4), bytearray(b"a\4"))
            self.assertEqual(calls, [3, 4])
            with patch("floppy_fixture_cache._recipe_digest", return_value="new-recipe"):
                producer(b"a")
            self.assertEqual(calls, [3, 4, 3])

    def test_corruption_is_rebuilt(self):
        """Truncated and hash-invalid records cannot silently become fixtures."""
        with TemporaryDirectory() as temporary, patch.dict(os.environ, {ENVIRONMENT: temporary}):
            folder = Path(temporary)
            record = folder / "computations.txt"
            self.assertEqual(concurrent_producer(str(record), b"payload"), b"payload")
            cache = next(folder.glob("*.fixture"))
            cache.write_bytes(b"truncated")
            self.assertEqual(concurrent_producer(str(record), b"payload"), b"payload")
            data = bytearray(cache.read_bytes())
            data[-1] ^= 1
            cache.write_bytes(data)
            self.assertEqual(concurrent_producer(str(record), b"payload"), b"payload")
            self.assertEqual(record.read_text().splitlines(), ["computed"] * 3)

    def test_cache_can_be_disabled(self):
        """The opt-out executes the encoder directly on every call."""
        with TemporaryDirectory() as temporary, patch.dict(os.environ, {ENVIRONMENT: ""}):
            record = Path(temporary) / "computations.txt"
            concurrent_producer(str(record), b"a")
            concurrent_producer(str(record), b"a")
            self.assertEqual(record.read_text().splitlines(), ["computed", "computed"])

    @unittest.skipUnless(os.name == "posix", "Linux producer-lock test")
    def test_concurrent_writers_compute_once(self):
        """Four Linux workers observe one intact, freshly decoded fixture."""
        with TemporaryDirectory() as temporary:
            folder = Path(temporary)
            record = folder / "computations.txt"
            environment = dict(os.environ, **{ENVIRONMENT: str(folder)})
            with ExitStack() as stack:
                children = [stack.enter_context(subprocess.Popen(
                    [sys.executable, __file__, "--child", str(record)], env=environment,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                )) for _ in range(4)]
                for child in children:
                    stdout, stderr = child.communicate(timeout=20)
                    self.assertEqual(child.returncode, 0, stderr)
                    self.assertEqual(stdout.strip(), "payload")
            self.assertEqual(record.read_text().splitlines(), ["computed"])


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        print(concurrent_producer(sys.argv[2], b"payload").decode())
    else:
        unittest.main()
