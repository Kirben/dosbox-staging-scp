# Testing floppy-controller changes

Use a persistent, configured Linux build directory. The helper builds
incrementally, copies only changed source files when using a native snapshot,
and runs up to four test workers by default.

```sh
python3 scripts/tools/test_floppy.py focused --build /path/to/build
python3 scripts/tools/test_floppy.py smoke --build /path/to/build
python3 scripts/tools/test_floppy.py full --build /path/to/build
```

`focused` runs SPECIFY timing, seek status, and PCjr/Tandy BIOS transfer and format regressions. `smoke` exercises
all six image formats, sector contracts, FAT coherence, representative
controller reads, and the cache/workflow checks. These groups are for iteration.
`full` runs **every registered CTest test**, including the unit and floppy suites.
A fast result never authorizes a push.

For an existing native Linux source snapshot of a Windows checkout, run the
helper from the checkout and add `--sync-source`. It discovers the snapshot
from `CMakeCache.txt`, copies changed inputs, and removes only obsolete files
recorded by its previous synchronization manifest. It does not check out or
change any Git branch. Configure the native build first using the normal
Linux build instructions; the helper reuses its compiler, dependencies and
build options. Use `--jobs 2` on a machine with fewer available resources.

## Synthetic fixture cache

The helper sets `DOSBOX_FLOPPY_FIXTURE_CACHE` to a directory inside the build.
The shared MFM, SCP, 86F, and TransCopy encoders cache deterministic image bytes
there. Keys include complete input data and encoder-recipe hashes. Cache entries
have integrity hashes; corrupt entries are regenerated. Concurrent Linux
producers use a lock and atomic replacement.

Each cache hit returns fresh mutable bytes when appropriate. Tests still create
private mount copies and private overlay files. Original game captures and test
results are never cached. Every selected test still executes against the current
emulator. Use `--no-cache` to check the uncached path or diagnose an encoder.
Linux CI uses the same optional cache; correctness does not depend on a cache hit.

## Full validation before a push

Install the local pre-push gate once:

```sh
python3 scripts/tools/test_floppy.py --install-hook
```

The installer preserves unrelated existing hooks and custom hook configurations.
It installs into this local repository's Git hooks directory, which can be shared
by its worktrees. Branches without this workflow cannot pass that installed gate.
It does not push, commit, or modify any remote configuration.

Run `full` before pushing. Only a completely passing full run creates the ignored
`.floppy-full-validation.json` receipt. The gate checks the exact build/test input
contents and hashes of the tested emulator, test executable and CMake configuration.
It also requires a clean worktree and a push of the current tested branch/HEAD.
Committing already-tested, unchanged content is permitted; changing build/test
inputs requires another full run. On Windows, the gate reads WSL artifacts through
`\\wsl.localhost\<distribution>`. WSL and those artifacts must remain available.

The gate consumes its receipt when allowing a push. A subsequent push needs another
full run. Focused/smoke runs invalidate an earlier receipt, and failures or changes
during testing never create one. Do not bypass this check with `--no-verify`.

Cache integrity/concurrency and the push-gate rejection paths have their own
standalone Python regressions. A local hook is an accidental-push safeguard,
not a substitute for CI or repository branch protection.

## PCjr and Tandy BIOS routing

Track-image INT 13h reads, writes and Verify execute through the shared FDC.
BIOS Format Track executes through the FDC for both raw and track images.
PCjr BIOS data transfers service its non-DMA FIFO at the controller byte cadence,
retain 16-bit buffer-offset wrapping, and preserve the keyboard interrupt vector.
The BIOS restores the guest-selected transfer mode after its command.
Tandy uses DMA channel 2 and IRQ6, including physical 64 KiB boundary checks.
Verify leaves the guest buffer and DMA channel untouched on both machines.
Raw IMG/IMA BIOS reads, writes and Verify keep their sector path. DOS FAT
keeps its logical sector view.
PCjr Format Track uses the same wrapped FIFO transfer for its ID table;
Tandy supplies that table through DMA, including terminal count and IRQ6.

Guest.FloppyBiosMachine checks controller results, CRC/deleted/missing-sector
status, partial counts, DMA/FIFO behavior, buffers, keyboard/PIC state, duplicate
IDs, and overlay replay across SCP, MFM, 86F and TransCopy.

`Guest.FloppyBiosFormatMachine` formats A/B images, checks controller results,
ID order, FIFO/DMA state, IRQ6 and table guards, then reopens the image for
readback. It covers all six formats, variable-size track sectors, write protection,
invalid table sizes, and ID tables ending at or crossing a 64 KiB boundary.

The format probe also checks busy-command rejection and recovery using BIOS
Reset on PCjr and Tandy, preserving keyboard interrupt handling.

`Guest.FloppySpecify` measures SRT across all four data rates and verifies head-load delays, head-unload expiry, and maximum-delay zero codes in standard-PC and PCjr modes. The PCjr watchdog probe also verifies BIOS Reset cancels delayed head loading, the watchdog, and an incomplete command without changing keyboard IRQ handling. BIOS Format machine tests also check recovery from an unfinished command using BIOS Reset. Timer values follow Intel 82077AA Table 5-10 (500 kbit/s base, scaled by data rate).

## Image probing and standalone parsers

`DiskImageFactory.*` and `DiskImageFactoryPathTest.*` check content signatures,
file-position preservation, raw fallback, malformed-container rejection, and
explicit hard-disk selection. `Guest.FloppyImageProbe` boots renamed MFM, SCP,
86F, and TransCopy captures through host paths, DOS paths, and mounted slots.

`BitstreamCapture.*` tests container parsing without starting an emulator:
TransCopy native step tables and index skew, HxC track extents, 86F bit counts,
index and physical-hole write protection, and SCP flux entries and atomic
failure handling. The bitstream parsers produce capture records; sector
recognition and persistent overlays remain in the media adapter.

Shared FAT-cache, timing, disk-change, and BIOS/CMOS reporting tests include
IMG, IMA, SCP, MFM, 86F, and TransCopy.

## Command-time track decoding

The FDC uses `FloppyMedia::SampleTrack()` separately from the logical sector
view. SCP retains its original transition stream and runs the existing data
separator on the command path; writes switch subsequent reads to the modified
raw cells. Bitstream captures sample weak cells before recognizing address marks,
so CHRN bytes, ID CRCs, and ID/data sync visibility can change between turns.
The FDC searches samples from the current and following capture cycle within
its physical-index deadline. Read Track also refreshes its sample after a wrap.
Selected fields retain physical slot identity for overlay writes.

`Guest.FloppyWeakId` compares normal, weak-ID, weak-sync, initially hidden-sync,
and weak-data-sync 86F tracks. It checks Read ID results and CRCs, Read Data
matching, and untouched buffers when a sampled field is unavailable. A native
sampling regression checks repeatability within a turn and preservation of
unmarked cells. Existing multi-revolution, duplicate-write, weak-write, overlap,
and persistence tests cover interaction with other media operations.

This is command-time digital decoding, not an analogue drive simulation. The
separator/noise model remains simplified, and programmed-rate mismatch behavior
and cycle-accurate DMA/FIFO service remain separate work. The stable logical
sector view used by DOS FAT does not acquire per-turn weak-field randomness.
