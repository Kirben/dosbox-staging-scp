#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify BIOS floppy reads, DMA page boundaries, and IRQ6 in a DOS guest."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_hxc_mfm_probe import encode, launch
from run_scp_attachment_probe import encode_scp


# pylint: disable-next=R0913,R0917,R0914
def run_probe(dosbox, nasm, directory, image, unit, options, read_only=True,
              machine=None, tandy_standard=False):
    """Assemble and run one BIOS guest probe."""
    source = Path(__file__).with_name("bios_floppy_read_probe.asm")
    (directory / "BIOS.OUT").unlink(missing_ok=True)
    subprocess.run(
        [nasm, "-f", "bin", f"-DUNIT={unit}", *options, source,
         "-o", directory / "PROBE.COM"],
        check=True,
    )
    letter = "ab"[unit]
    access = " -ro" if read_only else ""
    commands = [f'mount c "{directory}"',
                f'mount {letter} "{image}" -t floppy -fs none{access}', "c:"]
    if "-DKEYBOARD_TEST" in options:
        commands.append("autotype -w 2 k")
    commands.extend(("probe.com", "exit"))
    command, env = launch(dosbox, directory, "bios", commands)
    if machine:
        config = command[-1]
        settings = f"[dosbox]\nmachine={machine}\n"
        if tandy_standard:
            settings += "[dos]\ntandy_memory_config=standard\n"
        config.write_text(settings + config.read_text(), encoding="ascii")
    run = subprocess.run(command, cwd=directory, env=env, timeout=30,
                         capture_output=True, text=True, check=True)
    report = directory / "BIOS.OUT"
    stage = report.read_bytes() if report.exists() else b""
    if stage != b"\0":
        raise RuntimeError(f"{image.suffix} {letter} {options}: stage {stage.hex()}\n"
                           f"{run.stdout}{run.stderr}")


def check_scp_write(dosbox, nasm, disk):
    """Verify SCP BIOS writes and overlay persistence."""
    capture = encode_scp(disk)
    for unit, letter in enumerate("AB"):
        with TemporaryDirectory() as temp:
            directory = Path(temp)
            image = directory / "disk.scp"
            image.write_bytes(capture)
            run_probe(dosbox, nasm, directory, image, unit,
                      ["-DTRACK_IMAGE", "-DWRITE_TEST"], read_only=False)
            overlay = image.with_suffix(".scp.overlay")
            saved = overlay.read_bytes()
            run_probe(dosbox, nasm, directory, image, unit,
                      ["-DTRACK_IMAGE", "-DWRITE_READBACK"])
            if overlay.read_bytes() != saved or image.read_bytes() != capture:
                raise RuntimeError(f"SCP drive {letter} source or overlay changed on readback")
            print(f".scp drive {letter}: DMA write, IRQ6, page boundary and restart passed")


def check_read_track(dosbox, nasm, disk):
    """Verify DMA Read Track on PCjr and Tandy."""
    start = 4 * 9 * 512
    disk[start:start + 512] = bytes(range(256)) * 2
    disk[start + 511] = 0xf7
    capture = encode(disk, malformed_id=True)
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        image = directory / "readtrack.mfm"
        image.write_bytes(capture)
        for machine in ("pcjr", "tandy"):
            options = ["-DREAD_TRACK_TEST"]
            if machine == "pcjr":
                options.append("-DPCJR_IRQ_TEST")
            run_probe(dosbox, nasm, directory, image, 0, options,
                      machine=machine, tandy_standard=machine == "tandy")
            print(f"{machine}: malformed ID, Read Track DMA payload, "
                  "terminal count and status passed")
            if machine == "tandy":
                print("tandy: 96 KB BIOS memory report and four video RAM aliases passed")


# pylint: disable-next=R0914,R0912,R0915
def main():
    """Select BIOS scenarios and run them across the applicable formats."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", type=Path, required=True)
    parser.add_argument("--nasm", default="nasm")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--scp-write", action="store_true")
    modes.add_argument("--verify", action="store_true")
    modes.add_argument("--cross-track", action="store_true")
    modes.add_argument("--irq-reset", action="store_true")
    modes.add_argument("--read-track", action="store_true")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()

    disk = bytearray(360 * 1024)
    for sector in (3, 4):
        offset = (5 * 9 + sector - 1) * 512
        disk[offset:offset + 512] = bytes([0x50 + sector]) * 512

    if args.read_track:
        check_read_track(dosbox, args.nasm, disk)
        return

    if args.cross_track:
        for track, sector, value in ((2, 9, 0x19), (3, 1, 0x21),
                                     (3, 9, 0x29), (4, 1, 0x31),
                                     (5, 9, 0x39), (6, 1, 0x41)):
            offset = (track * 9 + sector - 1) * 512
            disk[offset:offset + 512] = bytes([value]) * 512

    if args.scp_write:
        check_scp_write(dosbox, args.nasm, disk)
        return

    formats = {"img": None, "ima": None, "scp": encode_scp,
               "mfm": encode, "86f": encode_86f}
    for extension, encoder in formats.items():
        if (args.verify or args.cross_track) and encoder is None:
            continue
        image_bytes = (
            encoder(disk, verify_errors=args.verify or args.cross_track) if encoder else disk
        )
        for unit, letter in enumerate("ab"):
            with TemporaryDirectory() as temp:
                directory = Path(temp)
                image = directory / f"disk.{extension}"
                image.write_bytes(image_bytes)
                options = ["-DTRACK_IMAGE"] if encoder else []
                if args.irq_reset:
                    options.append("-DIRQ_RESET_TEST")
                    run_probe(dosbox, args.nasm, directory, image, unit, options)
                    print(f".{extension} drive {letter.upper()}: BIOS reset and PIC IRQ6 passed")
                    if extension == "mfm" and unit == 0:
                        for machine in ("pcjr", "tandy"):
                            machine_options = options + ["-DKEYBOARD_TEST"]
                            if machine == "pcjr":
                                machine_options.append("-DPCJR_IRQ_TEST")
                            run_probe(dosbox, args.nasm, directory, image, unit,
                                      machine_options, machine=machine)
                            print(f"{machine}: BIOS reset, MFM read and keyboard passed")
                    continue
                if args.cross_track:
                    options.append("-DCROSS_TRACK")
                    run_probe(dosbox, args.nasm, directory, image, unit, options)
                    if extension == "scp":
                        run_probe(dosbox, args.nasm, directory, image, unit,
                                  options + ["-DCROSS_WRITE"], read_only=False)
                        run_probe(dosbox, args.nasm, directory, image, unit,
                                  options + ["-DCROSS_READBACK"])
                        if image.read_bytes() != image_bytes:
                            raise RuntimeError("SCP capture changed after writes")
                    print(f".{extension} drive {letter.upper()}: "
                          "track crossing, partial counts and IRQ6 passed")
                    continue
                if args.verify:
                    options.append("-DVERIFY_TEST")
                run_probe(dosbox, args.nasm, directory, image, unit, options)
                if args.verify:
                    print(f".{extension} drive {letter.upper()}: "
                          "Verify status, count, IRQ6 and buffer passed")
                    continue
                mode = "DMA boundary and IRQ6" if encoder else "legacy sector path"
                print(f".{extension} drive {letter.upper()}: data and {mode} passed")


if __name__ == "__main__":
    main()
