#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise floppy controller commands from a DOS guest."""

import argparse
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


# pylint: disable-next=R0913,R0917,R0914,R0912,R0915
def run_probe(dosbox, nasm, directory, extension, unit, size_kb, sectors, encode=None,
              write_mode=None, partial_format=False, format_reset=None,
              format_motor=None, bios_format=False, format_switch=False, weak_bits=None):
    """Check a raw or encoded image with the selected controller scenario."""
    image = directory / f"disk.{extension}"
    data = bytearray(size_kb * 1024)
    for sector in (3, 4):
        offset = ((2 * 2 + 1) * sectors + sector - 1) * 512
        data[offset:offset + 512] = bytes([0x50 + sector]) * 512
    if write_mode in ("duplicate-write", "duplicate-readback"):
        for sector in (5, 6):
            offset = (5 * sectors + sector - 1) * 512
            data[offset:offset + 512] = bytes([0x60 + sector]) * 512
    image_bytes = encode(data) if encode else data
    if write_mode not in ("readback", "format-readback", "duplicate-readback"):
        image.write_bytes(image_bytes)
    if write_mode in ("format", "format-readback"):
        offset = (2 * 2 + 1) * sectors * 512
        data[offset:offset + sectors * 512] = bytes([0xe5]) * (sectors * 512)
    if write_mode in ("write", "readback"):
        for sector in (5, 6):
            offset = ((2 * 2 + 1) * sectors + sector - 1) * 512
            data[offset:offset + 512] = bytes([0x4e + sector]) * 512
    expected_image = image_bytes if encode else data

    source = Path(__file__).with_name("raw_floppy_read_probe.asm")
    options = ["-DREORDERED"] if encode else []
    if extension == "scp" and not write_mode:
        options.append("-DSCP_REVOLUTIONS")
    elif extension == "86f" and write_mode != "format-readback":
        options.append("-DFIRST_ID_AFTER_INDEX=2")
    elif extension == "tc":
        options.extend(("-DFIRST_ID_AFTER_INDEX=9", "-DTC_STEPPED"))
        if weak_bits is not None:
            options.append(f"-DTC_WEAK_BITS={int(weak_bits)}")
    if write_mode in ("write", "protected"):
        options.append("-DWRITE_TEST")
    if write_mode == "protected":
        options.append("-DWRITE_PROTECTED")
    if write_mode == "readback":
        options.append("-DREADBACK")
    if write_mode in ("duplicate-write", "duplicate-readback"):
        options.append("-DDUPLICATE_TEST")
    if write_mode == "duplicate-write":
        options.append("-DDUPLICATE_WRITE")
    if write_mode in ("format", "format-invalid", "format-protected"):
        options.append("-DFORMAT_TEST")
        if bios_format:
            options.append("-DBIOS_FORMAT")
    if write_mode in ("format-invalid", "format-protected", "format-readback"):
        options.append("-D" + write_mode.upper().replace("-", "_"))
    if encode and write_mode and write_mode.startswith("format"):
        options.append("-DSCP_FORMAT")
        if partial_format:
            options.append("-DFORMAT_PARTIAL")
            if not (format_reset or format_motor or format_switch):
                options.append("-DFORMAT_EARLY_TC")
        if format_reset:
            options.append(f"-DFORMAT_RESET={format_reset}")
        if format_motor:
            options.append(f"-DFORMAT_MOTOR={format_motor}")
        if format_switch:
            options.append("-DFORMAT_SWITCH")
        if write_mode == "format-readback":
            options.append("-DSCP_FORMAT_READBACK")
    (directory / "FDC.OUT").unlink(missing_ok=True)
    writable = ("write", "format", "format-invalid", "duplicate-write")
    access = "" if write_mode in writable else " -ro"
    subprocess.run([nasm, *options, "-f", "bin", f"-DUNIT={unit}",
                    f"-DSECTORS={sectors}", source,
                    "-o", directory / "PROBE.COM"], check=True)
    other_mount = ""
    if format_switch:
        other_image = directory / f"other.{extension}"
        other_image.write_bytes(image_bytes)
        other_mount = f'mount {1 - unit} "{other_image}" -t floppy -fs none -ro\n'
    config = directory / "probe.conf"
    config.write_text(
        "[sdl]\noutput=texture\n[mixer]\nnosound=true\n[autoexec]\n"
        f'mount c "{directory}"\n'
        f'mount {unit} "{image}" -t floppy -fs none{access}\n'
        f"{other_mount}c:\nprobe.com\nexit\n",
        encoding="ascii",
    )
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    run = subprocess.run(
        [dosbox, "--noprimaryconf", "--nolocalconf", "-conf", config],
        cwd=directory, env=env, timeout=30, capture_output=True, text=True,
        check=True,
    )
    report = directory / "FDC.OUT"
    if not report.exists() or report.read_bytes() != b"\0":
        stage = report.read_bytes().hex() if report.exists() else "missing"
        raise RuntimeError(f".{extension} {size_kb} KB drive {unit} failed at stage {stage}\n"
                           f"{run.stdout}{run.stderr}")
    if image.read_bytes() != expected_image:
        raise RuntimeError(f".{extension} has unexpected on-disk bytes")
    if write_mode:
        mode = f"partial-{write_mode}" if partial_format else write_mode
        if format_reset:
            phase = {1: "before", 2: "during", 3: "after"}[format_reset]
            mode = f"{mode}, reset {phase}"
        if format_motor:
            phase = {1: "before", 2: "during", 3: "after"}[format_motor]
            mode = f"{mode}, motor off {phase}"
        result = ("INT 13h status passed" if bios_format and write_mode != "format-readback"
                  else "DMA and IRQ6 passed")
        print(f".{extension} {size_kb} KB drive {'AB'[unit]} {mode}: {result}")
        return
    print(f".{extension} {size_kb} KB drive {'AB'[unit]}: seek, Read ID, DMA and IRQ6 passed")


def main():
    """Run the requested raw controller scenarios for both drive slots."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--format", action="store_true")
    parser.add_argument("--bios-format", action="store_true")
    args = parser.parse_args()
    for extension in ("img", "ima"):
        for size_kb, sectors in ((360, 9), (1200, 15)):
            for unit in (0, 1):
                with TemporaryDirectory() as temporary:
                    run_probe(args.dosbox.resolve(), args.nasm, Path(temporary),
                              extension, unit, size_kb, sectors)
                    if args.write:
                        for mode in ("protected", "write", "readback"):
                            run_probe(args.dosbox.resolve(), args.nasm, Path(temporary),
                                      extension, unit, size_kb, sectors, write_mode=mode)
                    if args.format or args.bios_format:
                        for mode in ("format-protected", "format-invalid",
                                     "format", "format-readback"):
                            run_probe(args.dosbox.resolve(), args.nasm, Path(temporary),
                                      extension, unit, size_kb, sectors, write_mode=mode,
                                      bios_format=args.bios_format)


if __name__ == "__main__":
    main()
