#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Keep ordinary repeated writes compact without changing their replay data."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from run_floppy_zero_sector_probe import encode_tracks
from run_hxc_mfm_probe import encode_track
from run_transcopy_layout_probe import run_guest


def writes(repetitions):
    """Rewrite two independent sectors in alternating order."""
    return f"""
    mov bp,{repetitions}
write_pair:
    mov di,buffer
    mov cx,512
    mov al,0a5h
    rep stosb
    mov ax,0301h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    push bp
    int 13h
    pop bp
    jc done
    cmp ax,1
    jne done
    mov di,buffer
    mov cx,512
    mov al,0b6h
    rep stosb
    mov ax,0301h
    mov bx,buffer
    mov cx,2
    xor dx,dx
    push bp
    int 13h
    pop bp
    jc done
    cmp ax,1
    jne done
    dec bp
    jnz write_pair
"""


READBACK = """
    mov ax,0202h
    mov bx,buffer
    mov cx,1
    xor dx,dx
    int 13h
    jc done
    cmp ax,2
    jne done
    mov si,buffer
    mov cx,512
check_first:
    lodsb
    cmp al,0a5h
    jne done
    loop check_first
    mov cx,512
check_second:
    lodsb
    cmp al,0b6h
    jne done
    loop check_second
"""


def main():
    """Check bounded overlays and read-only replay for each writable backend."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox", required=True, type=Path)
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    dosbox = args.dosbox.resolve()
    cells = encode_track(bytes(360 * 1024), 0)
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        for extension in ("tc", "mfm", "scp", "86f"):
            image = directory / f"compact.{extension}"
            original = encode_tracks(extension, [cells])
            image.write_bytes(original)
            run_guest(dosbox, args.nasm, directory, image,
                      writes(1) + READBACK, read_only=False)
            overlay = image.with_suffix(f".{extension}.overlay")
            saved = overlay.read_bytes()
            assert saved[6] == 4
            run_guest(dosbox, args.nasm, directory, image,
                      writes(50) + READBACK, read_only=False)
            assert overlay.read_bytes() == saved
            run_guest(dosbox, args.nasm, directory, image, READBACK)
            assert overlay.read_bytes() == saved
            assert image.read_bytes() == original
            print(f".{extension}: repeated writes stay at {len(saved)} bytes and replay",
                  flush=True)


if __name__ == "__main__":
    main()
