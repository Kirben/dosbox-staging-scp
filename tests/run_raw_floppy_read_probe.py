#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise raw floppy controller reads from a DOS guest."""

import argparse
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


def run_probe(dosbox, nasm, directory, extension, unit, size_kb, sectors, encode=None):
    image = directory / f"disk.{extension}"
    data = bytearray(size_kb * 1024)
    for sector in (3, 4):
        offset = ((2 * 2 + 1) * sectors + sector - 1) * 512
        data[offset:offset + 512] = bytes([0x50 + sector]) * 512
    image_bytes = encode(data) if encode else data
    image.write_bytes(image_bytes)

    source = Path(__file__).with_name("raw_floppy_read_probe.asm")
    options = ["-DREORDERED"] if encode else []
    if extension == "scp":
        options.append("-DSCP_REVOLUTIONS")
    elif extension == "86f":
        options.append("-DFIRST_ID_AFTER_INDEX=2")
    subprocess.run([nasm, *options, "-f", "bin", f"-DUNIT={unit}",
                    f"-DSECTORS={sectors}", source,
                    "-o", directory / "PROBE.COM"], check=True)
    config = directory / "probe.conf"
    config.write_text(
        "[sdl]\noutput=texture\n[mixer]\nnosound=true\n[autoexec]\n"
        f'mount c "{directory}"\n'
        f'mount {unit} "{image}" -t floppy -fs none -ro\n'
        "c:\nprobe.com\nexit\n",
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
    if image.read_bytes() != image_bytes:
        raise RuntimeError(f".{extension} changed during a read-only test")
    print(f".{extension} {size_kb} KB drive {'AB'[unit]}: seek, Read ID, DMA and IRQ6 passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    for extension in ("img", "ima"):
        for size_kb, sectors in ((360, 9), (1200, 15)):
            for unit in (0, 1):
                with TemporaryDirectory() as temporary:
                    run_probe(args.dosbox.resolve(), args.nasm, Path(temporary),
                              extension, unit, size_kb, sectors)


if __name__ == "__main__":
    main()
