#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check guest BIOS, CMOS, FDC, and IDE reporting across floppy slot changes."""

import argparse
import os
from pathlib import Path
from struct import pack, pack_into, unpack_from
import subprocess
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_fat_floppy_cache_probe import fat12
from run_hxc_mfm_probe import encode
from run_scp_attachment_probe import encode_scp


def make_fixtures(folder, marker, odd_geometry):
    """Create matching reporting fixtures in all supported formats."""
    folder.mkdir(exist_ok=True)
    disk = fat12()
    disk[3072:3584] = bytes([marker]) * 512
    if odd_geometry:
        # Keep all 80 physical tracks but expose a single-sided 8-sector FAT volume.
        pack_into("<H", disk, 19, 320)
        pack_into("<HH", disk, 24, 8, 1)
    for extension, encoder in (
        ("img", bytes), ("ima", bytes), ("scp", encode_scp),
        ("mfm", encode), ("86f", encode_86f),
    ):
        (folder / f"disk.{extension}").write_bytes(encoder(disk))


def make_iso(path):
    # A minimal ISO 9660 volume with an empty root directory.
    """Create a minimal disc image for the ATAPI control."""
    sector = 2048
    iso = bytearray(19 * sector)
    root = pack("<BBI", 34, 0, 18) + pack(">I", 18)
    root += pack("<I", sector) + pack(">I", sector) + bytes(7)
    root += b"\x02\x00\x00\x01\x00\x00\x01\x01\x00"
    start = 16 * sector
    iso[start:start + 7] = b"\x01CD001\x01"
    iso[start + 8:start + 72] = b" " * 64
    iso[start + 40:start + 47] = b"IDETEST"
    iso[start + 80:start + 88] = pack("<I", 19) + pack(">I", 19)
    for offset, value in ((120, 1), (124, 1), (128, sector)):
        iso[start + offset:start + offset + 4] = pack("<H", value) + pack(">H", value)
    iso[start + 156:start + 190] = root
    iso[start + 881] = 1
    iso[17 * sector:17 * sector + 7] = b"\xffCD001\x01"
    iso[18 * sector:18 * sector + 68] = root + root[:-1] + b"\x01"
    path.write_bytes(iso)


# pylint: disable-next=R0914
def verify_report(path, slots):
    """Compare guest BIOS, CMOS, FDC, and ATAPI data to the expected state."""
    data = path.read_bytes()
    assert len(data) == 40, (path, data.hex())
    equipment, cmos_type, cmos_equipment, ide_id = unpack_from("<HBBH", data)
    count = sum(marker != 0 for marker, _ in slots)
    equipment_bits = 0 if not count else 1 | ((count - 1) << 6)
    # Marker identifies the expected cylinder/head/sector and drive type.
    types = {0x31: (40, 2, 9, 1, 1), 0x62: (40, 2, 9, 1, 1),
             0x18: (40, 1, 9, 0, 1), 0xed: (80, 2, 36, 6, 5)}
    a, b = (types[marker][-1] if marker else 0 for marker, _ in slots)
    expected_types = (a << 4) | b
    assert (equipment & 0xc1, cmos_equipment & 0xc1) == (equipment_bits,) * 2, (
        path, equipment, cmos_equipment)
    assert cmos_type == expected_types, (path, cmos_type, expected_types)
    assert ide_id & 0xff00 == 0x8500, (path, hex(ide_id))
    for unit, (marker, changed) in enumerate(slots):
        carry, status, bios_type, chs, heads_count, change_carry, change, address, bda, \
            actual, after_carry, after, after_address, after_bda, read_error = unpack_from(
                "<BBBHH10B", data, 6 + unit * 17)
        assert (address & 0x7f) == 0x3e, (path, unit, address)
        assert bool(address & 0x80) == changed, (path, unit, address)
        expected_change = (6 if changed else 0) if marker else 0x80
        assert (change_carry, change, bda) == (int(bool(expected_change)), expected_change,
                                               expected_change), (path, unit, change, bda)
        if marker:
            cylinders, heads, sectors, drive_type, _ = types[marker]
            assert (carry, status, bios_type, chs, heads_count) == (
                0, 0, drive_type, ((cylinders - 1) << 8) | sectors,
                (heads - 1) * 256 + count), (path, unit, data.hex())
            assert (actual, read_error) == (marker, 0), (path, unit, actual, read_error)
            assert (after_carry, after, after_address, after_bda) == (0, 0, 0x3e, 0), (
                path, unit, after, after_address, after_bda)
        else:
            assert (carry, status) == (1, 7), (path, unit, carry, status)


# pylint: disable-next=R0914
def run_probe(exe, nasm, out):
    """Check reporting across the mount and ejection sequence."""
    make_fixtures(out / "first", 0x31, False)
    make_fixtures(out / "second", 0x62, True)
    make_iso(out / "ide.iso")
    virtual = out / "virtual"
    virtual.mkdir(exist_ok=True)
    (virtual / "CHECK.DAT").write_bytes(b"DOS only")
    source = Path(__file__).with_name("floppy_reporting_probe.asm")
    subprocess.run([nasm, "-f", "bin", source, "-o", out / "REPORT.COM"], check=True)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    for extension in ("img", "ima", "scp", "mfm", "86f"):
        folder = out / extension
        folder.mkdir(exist_ok=True)
        first = out / "first" / f"disk.{extension}"
        second = out / "second" / f"disk.{extension}"
        snapshots = [
            ("EMPTY", (), ((0, False), (0, False))),
            ("VIRTUAL", (f'mount a "{virtual}" -t floppy', r"copy A:\CHECK.DAT C:\VIRTUAL.DAT"),
             ((0, False), (0, False))),
            ("AONLY", ("mount -u a", f'mount a "{first}" -t floppy -ro',
                       r"copy A:\CHECK.DAT C:\FIRST.DAT"),
             ((0x31, True), (0, False))),
            ("TWO", (f'mount 1 "{first}" -t floppy -fs none -ro',),
             ((0x31, False), (0x31, True))),
            ("SWAPA", ("mount -u a", f'mount a "{second}" -t floppy -ro',
                       r"copy A:\CHECK.DAT C:\SECOND.DAT"),
             ((0x62, True), (0x31, False))),
            ("SWAPB", (f'mount 1 "{second}" -t floppy -fs none -ro',),
             ((0x62, False), (0x62, True))),
            ("BONLY", ("mount -u a",), ((0, True), (0x62, False))),
            ("EJECTED", ("mount -u 1",), ((0, True), (0, True))),
        ]
        if extension == "img":
            legacy = folder / "legacy.img"
            ed = folder / "ed.img"
            legacy.write_bytes(b"\x18" * (180 * 1024))
            ed.write_bytes(b"\xed" * (2880 * 1024))
            snapshots.append(("TYPES", (f'mount 0 "{legacy}" -t floppy -fs none -ro',
                                        f'mount 1 "{ed}" -t floppy -fs none -ro'),
                              ((0x18, True), (0xed, True))))
        commands = [f'mount c "{folder}"', f'mount d "{out}" -ro',
                    f'mount e "{out / "ide.iso"}" -t cdrom -ide', "c:"]
        for name, changes, _ in snapshots:
            commands.extend((*changes, r"d:\report", f"ren REPORT.BIN {name}.BIN"))
        config = folder / "test.conf"
        config.write_text("[sdl]\noutput=texture\n[cpu]\ncpu_cycles=20000\n"
                          "[mixer]\nnosound=true\n[autoexec]\n" +
                          "\n".join(commands) + "\nexit\n")
        with (folder / "host.log").open("wb") as log:
            subprocess.run([exe, "--noprimaryconf", "--nolocalconf", "-conf", config],
                           cwd=folder, env=env, stdout=log, stderr=log, timeout=45, check=True)
        assert (folder / "VIRTUAL.DAT").read_bytes() == b"DOS only"
        assert (folder / "FIRST.DAT").read_bytes() == b"\x31" * 512
        assert (folder / "SECOND.DAT").read_bytes() == b"\x62" * 512
        for name, _, slots in snapshots:
            verify_report(folder / f"{name}.BIN", slots)
        print(f".{extension}: {len(snapshots)} reporting states and IDE passed", flush=True)


def main():
    """Run reporting checks in an isolated workspace."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        run_probe(args.dosbox.resolve(), args.nasm, Path(temporary))


if __name__ == "__main__":
    main()
