#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise overlay writer exclusion and interrupted saves."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_fat_floppy_cache_probe import fat12
from run_hxc_mfm_probe import encode, launch


def run_guest(dosbox, folder, name, commands):
    """Run a DOS command sequence with a host directory for test results."""
    args, env = launch(dosbox, folder, name,
                       [f'mount c "{folder}"', "c:", *commands, "exit"])
    subprocess.run(args, env=env, cwd=folder, check=True, capture_output=True, timeout=30)


def check_overlay(dosbox, folder):
    """Verify that a second writer cannot replace saved data, even after a crash."""
    original = encode(fat12())
    image = folder / "disk.mfm"
    image.write_bytes(original)
    (folder / "ONE.TXT").write_bytes(b"first")
    (folder / "TWO.TXT").write_bytes(b"second")
    mount_a = f'mount a "{image}"'
    mount_b = f'mount b "{image}"'
    run_guest(dosbox, folder, "writers", [
        mount_a, mount_b, r"copy one.txt a:\first.txt",
        r"copy two.txt b:\second.txt", r"copy b:\second.txt rejected.txt",
        mount_b + " -ro", r"copy b:\first.txt shared.txt", "mount -u b",
        "mount -u a", mount_b, r"copy b:\first.txt kept.txt",
    ])
    assert not (folder / "REJECTED.TXT").exists()
    assert (folder / "SHARED.TXT").read_bytes() == b"first"
    assert (folder / "KEPT.TXT").read_bytes() == b"first"

    temporary = folder / "disk.mfm.overlay.tmp"
    temporary.write_bytes(b"incomplete overlay from an interrupted save")
    run_guest(dosbox, folder, "recovery", [
        mount_b, r"copy two.txt b:\second.txt",
    ])
    run_guest(dosbox, folder, "replay", [
        mount_a + " -ro", r"copy a:\first.txt first.out",
        r"copy a:\second.txt second.out",
    ])
    assert (folder / "FIRST.OUT").read_bytes() == b"first"
    assert (folder / "SECOND.OUT").read_bytes() == b"second"
    assert not temporary.exists()
    assert image.read_bytes() == original


def main():
    """Run the write safety regressions against disposable floppy images."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", help="Accepted by the CTest guest probe runner")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        folder = Path(temporary)
        check_overlay(args.dosbox.resolve(), folder)


if __name__ == "__main__":
    main()
