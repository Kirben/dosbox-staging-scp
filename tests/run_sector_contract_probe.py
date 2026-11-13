#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check sector-result adapters through guest BIOS access."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_86f_attachment_probe import encode_86f
from run_fat_floppy_cache_probe import fat12
from run_hxc_mfm_probe import encode
from run_scp_attachment_probe import encode_scp
from run_transcopy_attachment_probe import encode_transcopy
from run_transcopy_layout_probe import run_guest


CHECKS = """
    mov ax,0201h
    mov bx,buffer
    mov cx,7
    xor dx,dx
    int 13h
    jc done
    cmp ah,0
    jne done
    cmp byte [buffer],37h
    jne done
    mov byte [stage],2
    mov ax,0201h
    mov cx,2801h
    xor dx,dx
    int 13h
    jnc done
    cmp ah,4
    jne done
    cmp byte [buffer],37h
    jne done
    mov byte [stage],3
    mov ax,0301h
    mov cx,EXPECTED_WRITE_CX
    xor dx,dx
    int 13h
    jnc done
    cmp ah,EXPECTED_WRITE_STATUS
    jne done
    mov byte [stage],4
    mov ax,0201h
    mov cx,7
    xor dx,dx
    int 13h
    jc done
    cmp ah,0
    jne done
    cmp byte [buffer],37h
    jne done
"""


def main():
    """Check BIOS result mapping for each format in PC and Tandy modes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    with TemporaryDirectory() as temporary:
        directory = Path(temporary)
        disk = fat12()
        disk[6 * 512:7 * 512] = b"\x37" * 512
        for extension, encoder in (
            ("img", bytes), ("ima", bytes), ("scp", encode_scp),
            ("mfm", encode), ("86f", encode_86f), ("tc", encode_transcopy),
        ):
            image = directory / f"contract.{extension}"
            original = encoder(disk)
            image.write_bytes(original)
            for machine in ("svga_s3", "tandy"):
                for read_only in (False, True):
                    expected = 3 if read_only else 4
                    checks = CHECKS.replace("EXPECTED_WRITE_STATUS", str(expected))
                    checks = checks.replace("EXPECTED_WRITE_CX", "7" if read_only else "2801h")
                    run_guest(args.dosbox.resolve(), args.nasm, directory, image, checks,
                              machine=machine, read_only=read_only)
                    assert image.read_bytes() == original
                    assert not Path(str(image) + ".overlay").exists()
                    print(f"{extension} {machine} read_only={read_only}: data, "
                          "missing-sector and write status passed", flush=True)


if __name__ == "__main__":
    main()
