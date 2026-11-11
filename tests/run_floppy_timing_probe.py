#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Measure DOS FAT, BIOS, and direct FDC reads using guest PIT timestamps."""

import argparse
import os
from pathlib import Path
from struct import iter_unpack, pack_into
import subprocess
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_fat_floppy_cache_probe import fat12
from run_hxc_mfm_probe import encode
from run_scp_attachment_probe import encode_scp


def timing_fixture():
    """Create a FAT12 disk with distinct data at a track boundary."""
    disk = fat12()
    for offset in (512, 1536):
        disk[offset + 3:offset + 9] = bytes.fromhex("03 40 00 05 f0 ff")
    pack_into("<I", disk, 2560 + 28, 4096)
    disk[6 * 512:14 * 512] = b"\x31" * 4096
    for lba, marker in ((8, 0x69), (9, 0x71)):
        disk[lba * 512:(lba + 1) * 512] = bytes([marker]) * 512
    return disk


def measure(dosbox, folder, program, image, speed):
    """Run the guest probe and convert its PIT counts to milliseconds."""
    folder.mkdir()
    config = folder / "timing.conf"
    config.write_text(f"""[sdl]
output=texture
[dosbox]
machine=svga_s3
floppy_disk_speed={speed}
[cpu]
core=normal
cpu_cycles=20000
[mixer]
nosound=true
[autoexec]
mount c "{folder}"
mount d "{program.parent}" -ro
mount a "{image}" -t floppy -ro
c:
d:\\{program.stem}
exit
""", encoding="ascii")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    run = subprocess.run(
        [dosbox, "--noprimaryconf", "--nolocalconf", "-conf", config],
        cwd=folder, env=env, timeout=30, capture_output=True, text=True, check=True,
    )
    report = folder / "TIMES.BIN"
    failure = folder / "FAIL.BIN"
    if not report.is_file() or failure.exists():
        stage = len(failure.read_bytes()) // 12 if failure.exists() else "missing"
        raise RuntimeError(f"{image.suffix} {speed} failed at sample {stage}\n"
                           f"{run.stdout}{run.stderr}")
    return [(65536 * (t1 - t0) + p0 - p1) * 1000 / 1193182
            for t0, p0, t1, p1 in iter_unpack("<IHIH", report.read_bytes())]


def check_timings(extension, maximum, slow):
    """Check the configured FAT delay and controller timing."""
    assert len(maximum) == len(slow) == 5, (extension, maximum, slow)
    print(f".{extension} maximum {maximum}, slow {slow}", flush=True)
    # 4096 bytes at 30 KiB/s adds 133.33 ms to the DOS FAT read.
    assert 131 < slow[0] - maximum[0] < 136, (extension, maximum, slow)
    for index in range(1, 5):
        assert abs(slow[index] - maximum[index]) < 3, (extension, index)
    _, bios_single, bios_cross, fdc_single, fdc_cross = maximum
    assert 16 < fdc_single < 202, (extension, fdc_single)
    assert 32 < fdc_cross < 403, (extension, fdc_cross)
    if extension in ("img", "ima"):
        # Raw images use the BIOS sector path with no rotational wait.
        assert max(bios_single, bios_cross) < 3, (extension, maximum)
    else:
        assert abs(bios_single - fdc_single) < 5, (extension, maximum)
        assert abs(bios_cross - fdc_cross) < 5, (extension, maximum)


def main():
    """Measure each floppy format at maximum and slow speeds."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    source = Path(__file__).with_name("floppy_timing_probe.asm")
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        program = directory / "TIMING.COM"
        subprocess.run([args.nasm, "-f", "bin", source, "-o", program], check=True)
        disk = timing_fixture()
        for extension, encoder in (
            ("img", bytes), ("ima", bytes), ("scp", encode_scp),
            ("mfm", encode), ("86f", encode_86f),
        ):
            image = directory / f"disk.{extension}"
            original = encoder(disk)
            image.write_bytes(original)
            maximum, slow = (
                measure(dosbox, directory / f"{extension}-{speed}", program, image, speed)
                for speed in ("maximum", "slow")
            )
            check_timings(extension, maximum, slow)
            assert image.read_bytes() == original, image


if __name__ == "__main__":
    main()
