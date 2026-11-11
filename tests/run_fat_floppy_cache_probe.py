#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check DOS FAT handles across BIOS and controller writes to floppy images."""

import argparse
from functools import partial
from pathlib import Path
from struct import pack_into
import subprocess
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_hxc_mfm_probe import encode, launch
from run_scp_attachment_probe import encode_scp


def fat12():
    """Build a minimal FAT12 disk with a marker file."""
    disk = bytearray(360 * 1024)
    disk[:11] = b"\xeb\x3c\x90MSDOS5.0"
    pack_into("<HBHBHHBHHH", disk, 11, 512, 2, 1, 2, 16, 720, 0xfd, 2, 9, 2)
    disk[510:512] = b"\x55\xaa"
    for start in (512, 1536):
        disk[start:start + 6] = b"\xfd\xff\xff\xff\x0f\x00"
    disk[2560:2572] = b"CHECK   DAT\x20"
    pack_into("<HI", disk, 2560 + 26, 2, 512)
    disk[3072:3584] = b"\x31" * 512
    return disk


def fat_cache_fixture():
    """Add files and cluster chains used by the cache checks."""
    disk = fat12()
    # Two sectors per cluster. CHAIN.DAT starts in cluster 3, and its last
    # two bytes initially live in cluster 4. Clusters 5 and 6 are replacements.
    for start in (512, 1536):
        disk[start + 4:start + 12] = b"\x4f\x00\xff\xff\xff\xff\xff\xff"
    for index, name, cluster, size in (
        (1, b"CHAIN   DAT", 3, 1026), (2, b"BEFORE  DAT", 7, 512),
    ):
        offset = 2560 + index * 32
        disk[offset:offset + 12] = name + b"\x20"
        pack_into("<HI", disk, offset + 26, cluster, size)
    for sector, marker in ((10, 0x44), (12, 0x55), (14, 0x66)):
        disk[sector * 512:(sector + 1) * 512] = bytes([marker]) * 512
    return disk



# pylint: disable-next=R0913,R0917,R0914
def run_probe(dosbox, nasm, directory, extension, encoder, unit, writable):
    """Check one image for cache coherence and persistence."""
    image = directory / f"disk.{extension}"
    capture = encoder(fat_cache_fixture())
    image.write_bytes(capture)
    source = Path(__file__).with_name("fat_floppy_cache_probe.asm")
    options = [] if writable else ["-DFAT_CACHE_READ_ONLY"]
    subprocess.run([nasm, "-f", "bin", f"-DFAT_UNIT={unit}", *options,
                    source, "-o", directory / "PROBE.COM"], check=True)
    letter = "ab"[unit]
    commands = [f'mount c "{directory}"', f'mount {letter} "{image}"', "c:",
                "probe.com", "exit"]
    args, env = launch(dosbox, directory, "cache", commands)
    run = subprocess.run(args, cwd=directory, env=env, timeout=30,
                         capture_output=True, text=True, check=True)
    report = directory / "FAT.OUT"
    stage = report.read_bytes() if report.exists() else b""
    if stage != b"\0":
        raise RuntimeError(f".{extension} cache probe failed at stage {stage.hex()}\n"
                           f"{run.stdout}{run.stderr}")
    if not writable:
        assert image.read_bytes() == capture
        assert not image.with_suffix(f".{extension}.overlay").exists()
        print(f".{extension}: BIOS and FDC writes rejected, DOS file data and metadata intact")
        return

    commands = [f'mount c "{directory}"', f'mount {letter} "{image}" -ro', "c:",
                f"copy {letter}:\\check.dat check.out",
                f"copy {letter}:\\chain.dat chain.out",
                f"copy {letter}:\\yefore.dat name.out", "exit"]
    args, env = launch(dosbox, directory, "restart", commands)
    subprocess.run(args, cwd=directory, env=env, timeout=30, check=True,
                   capture_output=True)
    files = {entry.name.lower(): entry for entry in directory.iterdir()}
    assert files["check.out"].read_bytes() == b"r" * 512
    chain = files["chain.out"].read_bytes()
    assert len(chain) == 1028 and chain[1024:] == b"vfff"
    assert files["name.out"].stat().st_size == 512
    if extension in ("scp", "mfm", "86f"):
        assert image.read_bytes() == capture
        assert image.with_suffix(f".{extension}.overlay").is_file()
    print(f".{extension}: open file data, directory and FAT chain refreshed, restart passed")


def main():
    """Run the FAT cache probe for each supported format."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    for extension, encoder, unit, writable in (
        ("img", bytes, 0, True), ("ima", bytes, 1, True),
        ("scp", encode_scp, 0, True), ("mfm", encode, 0, True),
        ("86f", encode_86f, 1, True),
        ("86f", partial(encode_86f, write_protected=True), 0, False),
        ("86f", partial(encode_86f, physical_hole=True), 0, False),
    ):
        with TemporaryDirectory() as directory:
            run_probe(args.dosbox.resolve(), args.nasm, Path(directory),
                      extension, encoder, unit, writable)


if __name__ == "__main__":
    main()
