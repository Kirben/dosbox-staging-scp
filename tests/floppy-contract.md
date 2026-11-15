# Shared floppy hardware contract

`Guest.FloppyHardwareContract` runs the same command cases and expectations
against IMG, IMA, SCP, HxC MFM, 86F and TransCopy. It covers:

- Cyclic Read ID order and a 200 ms revolution, allowing guest polling error.
- Read Data, data-rate rejection/recovery, Wrong Cylinder and missing IDs.
- DMA counts and terminal count, exactly one IRQ6, returned CHRN and buffer guards.
- Write Data followed by a fresh emulator process: the written sector changes,
  neighbouring sectors and another cylinder remain intact.
- Read-only write rejection, unchanged capture files and unchanged overlays
  during readback. Raw images change only the selected payload bytes.

The guest records observations in fixed-size binary records. Python checks
explicit expected status/data values; the fixture encoder is not the oracle.
Physical seeks are separate from the command's advertised cylinder.

`Guest.FloppyWeakIdWriteContract` covers capabilities raw sector files cannot
represent: physical duplicate IDs, data CRC errors and multi-revolution SCP
writes with a damaged ID CRC on another turn. It checks both logical and
physical overlay addressing, preserves ID errors and the unselected duplicate,
and reopens the image before checking the data. All-good-ID captures are controls.

The status and command expectations follow Intel 82077AA sections 5.1.1,
5.1.4 and its status-register descriptions. Wrong Cylinder compares the ID
against the command; it does not move the head. Write Data changes the selected
data arc, not the ID CRC. Raw files intentionally cannot model weak cells or
damaged CRCs; those cases have an explicit capture-only test profile.

Run the focused pair with:

```sh
ctest --test-dir build -R '^Guest.Floppy(HardwareContract|WeakIdWriteContract)$' --output-on-failure
```

Keep expectations independent of the implementation. New failures must be
investigated against the hardware contract; do not update the expected values
solely to match current emulator output. Use fixed fixtures and virtual guest
PIT measurements, and run the full Linux suite before pushing changes.
