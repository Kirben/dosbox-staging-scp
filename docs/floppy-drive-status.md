# Sense Drive Status

The shared floppy controller implements the two-byte `04h` command and its
single-byte ST3 result. It enters the result phase without a data transfer or a
new interrupt. Pending Seek/Recalibrate status remains available through Sense
Interrupt Status.

The register layout follows Intel's
[82077AA datasheet, sections 5.2.5 and 6.4](https://mark-ogden.uk/files/intel/publications/290166-007%2082077AA%20CHMOS%20Single-Chip%20Floppy%20Disk%20Controller-May94.pdf):

| Bit | Value |
| --- | --- |
| 7 | Zero |
| 6 | Write protection of the active floppy image in the requested slot |
| 5 | One |
| 4 | Physical head is at cylinder zero |
| 3 | One |
| 2 | Requested head |
| 1–0 | Requested unit |

Bits 5 and 3 are fixed on this controller; they are not media-ready and
two-sided-drive indications. Motor shutdown and media ejection do not clear
those bits. Ejecting a disk clears the virtual media's write-protect indication
without moving the head. Units 2 and 3 have no attached floppy drive; their
status never borrows write protection from BIOS hard-disk slots 2 and 3.

TRACK0 uses the existing physical head-position model, not the PCN register or
sector IDs. A failed 79-pulse recalibration can clear PCN while leaving TRACK0
inactive. This change does not add per-step motion: the existing Seek model
updates physical position at completion. The same emulated controller register
layout is exposed through the PCjr port aliases.

`Guest.FloppyDriveStatus` exercises IMG, IMA, SCP, HxC MFM, 86F and TransCopy in
standard PC, PCjr and Tandy configurations, with both virtual drives. It checks
command/result framing, unit/head bits, motor-off status, writable/read-only
swaps, ejection/reinsertion, reset, physical TRACK0 versus PCN, pending Seek
status, IRQ6, DMA requests and disk-change-latch preservation. It uses synthetic
fixtures and does not modify their contents.
