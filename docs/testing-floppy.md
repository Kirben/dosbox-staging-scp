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
raw cells. Long intervals without transitions are preserved as recorded; they
do not establish a weak-bit mask. Captured revolutions supply SCP variability. Bitstream captures sample weak cells before recognizing address marks,
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
separator/noise model remains simplified, and cycle-accurate DMA bus arbitration
remains separate work. The stable logical
sector view used by DOS FAT does not acquire per-turn weak-field randomness.

### Programmed read data rate

`Guest.FloppyReadRate` checks Read ID, Read Data, Read Track and Verify with
250-kbps SCP, HxC MFM, 86F and TransCopy tracks. It switches between 250, 300,
500 and 1,000 kbps without resetting the controller, checking missing-address-mark
status, IRQ6, DMA terminal count, untouched buffers on failure and recovery at
the correct rate. Mixed-rate tracks contain separate 250- and 500-kbps IDs;
changing the selected rate must reveal the matching ID and payload on the same
track. `Guest.FloppyFormatRate` also checks reads after formatting
and restarting at every supported rate.

The controller passes its selected rate to command-time decoding. SCP flux
uses that rate as the separator's nominal clock, rather than the clock chosen
when mounting the logical sector view. Bitstream captures retain their recovered
cells within the separator's existing 10% clock range; outside that range their
transitions are decoded again at the selected rate. This remains a digital
separator approximation, rather than an analogue PLL simulation. DOS FAT's
logical sector view is unchanged.

### Separator frequency holdover

The digital separator continues at its acquired frequency when an interval
contains more than four channel cells. Such an interval supplies no reliable
short-spacing frequency measurement. It no longer pulls the oscillator back
to nominal merely because transitions are sparse. Short intervals retain the
existing phase and frequency correction, and the clock remains bounded to
90–110% of nominal. No missing transitions, weak masks or corrected CRCs are
invented. Recorded index periods and transition positions remain unchanged.

`FloppyTrackData.HoldSeparatorFrequencyAcrossLongGaps` checks four nominal
channel clocks, fast/slow streams, alternating edge displacement, repeated
twelve-cell intervals, a transition-free tail and timing metadata. Its
normalized control has the same channel cells at the nominal clock.
`Guest.FloppySeparator` embeds sparse transitions in a valid-CRC data field
and tests 1,825-, 2,000- and 2,175-ns captures through Read ID, Read Data,
Read Track and Verify, including DMA terminal count, IRQ6 and buffer guards.
Both regressions fail with the earlier nominal-pullback behavior.

This is a bounded digital holdover model, not a reconstruction of Intel's
analog loop filter or a measured match to its acquisition/jitter curves.
Sections 3.2–3.2.3 of the
[Intel 82077AA datasheet](https://www.ardent-tool.com/datasheets/Intel_82077AA.pdf)
describe frequency tracking, phase-jitter response and sync-field acquisition.
The model's gains and 10% clock bounds are approximations, not datasheet
guarantees. Noise during an erased area, pulse qualification and command-time
analog acquisition remain unmodeled.

### Separator acquisition and index continuity

SCP recovery now feeds all captured revolutions through one separator. Index
marks partition the resulting cells without restarting clock or phase, or
rounding the two halves of a crossing transition interval independently.
A cell is assigned to the revolution containing its endpoint. An unfinished
cell at the end of the capture is not rounded into an extra trailing cell.
This applies both when mounting the logical sector view and when sampling
tracks at the FDC's programmed rate.

Before the first visible index, the decoder processes up to 128 nominal
channel cells of the final recorded turn's tail. This eight-byte history
interval supplies actual input for digital acquisition. Its output is then
discarded, so it adds no guest-visible cells or extra revolution. A blank or
damaged tail is retained as such; no sync pulses or successful lock are
fabricated. Each decode starts fresh for its selected clock and capture;
state is not reused between unrelated tracks or data rates.

The history length is motivated by the 64-data-bit sync-field acquisition
interval in Intel's section 3.2.2. This is not a measured model of the analog
lock detector, VCO-enable gating or acquisition/jitter curves. It represents
recorded-history acquisition and continuous digital phase recovery; analog
noise and electrical pulse qualification remain separate work.

Native regressions check a transition 100 ns after index, fractional-cell
index positions, unchanged cells when index markers are inserted, blank
history, malformed inputs and the existing per-revolution allocation limit.
`Guest.FloppyScpAcquisition` uses off-speed, two-revolution fields crossing
index, with 100-ns phase displacement. BIOS reads and direct FDC Read Track
must retain valid CRCs and distinct recorded payloads; writes must survive a
restart without changing the capture. Existing nominal/exact-index controls
continue to run separately. Bitstream containers retain their existing
recovered-cell path, and overlay record layouts are unchanged.

### Seek completion and bounded recalibration

`Guest.FloppySeekStatus` sends head-1 Seek and Recalibrate commands on A and B
across all six formats. Sense Interrupt Status must return H=0, while Read ID
continues to report the actual selected head through the existing head probes.

`Guest.FloppyRecalibrateLimit` checks starting cylinders 79, 80 and 81 in PC,
PCjr and Tandy modes on A and B across all six formats. It checks the SPECIFY
step interval, busy state, IRQ6 (absent on PCjr), SE/EC/IC status, cleared PCN,
remaining physical cylinder using Read ID, and recovery on a second recalibration.
A Seek to PCN zero after a failed recalibration must leave the physical head
where it is; recalibration then continues toward the physical TRACK0 position.
The images are mounted read-only and checked for changes.

Intel's 82077AA manual section 5.2.2 specifies failure if TRACK0 is still inactive
after 79 pulses. Section 8.3 describes a maximum of 80 pulses and says a head
farther than cylinder 79 returns an error. The implementation follows the
79-pulse termination rule in the detailed command section: cylinder 79 succeeds,
while cylinders 80 and 81 fail with equipment check and leave one or two physical
steps for the next recalibration. Motion interrupted by reset or motor loss
still uses the controller's existing completion-time position model.

### Byte-level DMA and FIFO timing

`Guest.FloppyFifo` checks live channel-2 counter progress during Read Data,
Read Track and Format Track. It checks threshold-dependent service latency,
mask/unmask recovery, overruns, early terminal count before IRQ6, zero-filled
write tails and restart persistence across IMG, IMA, SCP, MFM, 86F and TransCopy.
The Read Track cases use captured-track formats. Enhanced non-DMA tests check
RQM bursts, delayed CPU service, payloads and persistent writes. The probe also
checks explicit FIFO disable and DOR reset returning to compatibility mode.

DMA transfers service one byte per scheduled acknowledgement. The media side
advances at its byte cadence, with completion held through the sector's CRC or
format slot. The 16-byte FIFO is enabled with CONFIGURE (13h); EFIFO disables
it and FIFOTHR selects its request threshold. Disabled mode has a one-byte
buffer. The existing default PCjr non-DMA path retains its compatibility
behavior; explicitly enabled FIFO mode also supports non-DMA transfers.

A masked DMA channel can recover while the FIFO still has space or data.
Overruns stop reads. Write underruns stop requests but clock a zero-filled
remainder and valid CRC before committing the sector. Early terminal count
retains its distinct completion status. Reset and motor loss cancel pending
byte/service/completion events. DOR reset preserves the written portion of an
active sector, as does loss of the selected drive's motor-enable signal.
Format Track consumes its ID bytes progressively and persists completed slots.

This is an event-driven digital timing model. Its fixed 1.5-microsecond DMA
service interval provides finite acknowledgements but does not model full 8237
bus arbitration, CPU wait states or ISA bus cycles. CONFIGURE currently handles
FIFO enable and threshold only; implied seek, polling and precompensation are
not added by this change. LOCK, VERSION and other enhanced-controller commands
remain separate work.

### Preserving recorded SCP intervals

`Guest.FloppyScpNoFlux` reads a bad-CRC sector containing a long interval without
transitions 32 times. Single and repeated captures must return their recorded
payload unchanged; a capture with two different turns must return exactly those
two payloads. CRC status and source-image integrity are checked as well.

The earlier reader inferred weakness from a long gap and injected random cells.
That altered the protection data in Lemmings' original capture. The decoder now
preserves the captured transition stream, including gaps, and cycles through
actual recorded turns. Annotated 86F weak cells and the TransCopy weak-cell
model are unchanged. A single-turn SCP remains deterministic in this digital
model; physical noise cannot be established merely from the duration of a gap.

### FM read decoding

`Guest.FloppyFm` checks the command's FM/MFM bit independently of the programmed
rate. It covers FM Read ID, Read Data, Read Deleted Data, Read Track and Verify
on SCP, HxC MFM, 86F and TransCopy captures, including 128-, 256- and 512-byte
sectors, duplicate IDs, deleted marks, ID/data CRC errors and DMA buffer guards.
A mixed-encoding track checks selection, wrong-rate rejection and recovery.
The live DMA counter and PIT check that 512 FM bytes take approximately 32.8 ms
at the 250-kbps selector. `BitstreamCapture.F86FmUsesHalfTheMfmDataRate` checks
86F's FM rate interpretation and rejection of unsupported M2FM/GCR encodings.

FM uses the C7 clock pattern for FE/FB/F8 address marks and a CRC initialized
to FFFF at the mark. At the same rate selector its byte period is twice MFM's.
Track sampling caches include encoding as well as rate and revolution, allowing
a guest to select different encodings within the same physical track.

FM Write Data, Write Deleted Data and Format Track use the persistent overlay
on SCP, HxC MFM, 86F and TransCopy. Writes support 128-, 256-, 512- and
1,024-byte sectors, C7 normal/deleted marks and FM CRCs. Raw IMG/IMA media
expose a synthesized MFM layout and reject FM writes and formats. Standard
BIOS requests still select MFM; a guest must issue FM controller commands to
access FM fields. Sector-view writes also continue to reject FM records.

`Guest.FloppyFmWrite` checks DMA and PCjr non-DMA writes, duplicate-ID
selection, deleted marks, CRCs, terminal count, IRQ6, buffer guards, read-only
rejection and replay in a fresh emulator. Mixed tracks sharing CHRN values
must retain the other encoding's fields. `Guest.FloppyFmFormat` checks DMA
and PCjr non-DMA formatting at the 250-, 300- and 500-kbps selectors,
restart persistence and wrong-encoding rejection. Interrupted formats retain
the original MFM suffix; subsequent writes and another interrupted format
must preserve it as well. Malformed overlay records must fail mounting.

FM operations use overlay version 10. A sector record's flags byte retains
bit 0 for the deleted mark and adds bit 1 for FM. A format record adds one
encoding byte after its little-endian data rate: 0 for MFM and 1 for FM.
A 128-byte write record requires the FM flag. Versions 1 through 9 remain
readable, and MFM-only overlays continue to use their earlier representation.
The source capture stays unchanged; saved writes require its `.overlay`
sidecar. Sampled cell endpoints retain the angular extent of writes between
different clock grids, preserving adjacent faster fields on mixed tracks.

The automated fixtures are synthetic. No original PC game whose protection
requires FM decoding has yet been validated. FM duplication metadata on a
protected disk must not be confused with an FM-dependent protection check.

FM is a 765-compatible feature of this combined controller model, rather than
an assertion of exact 82077AA behavior. NEC's uPD765A/B documentation describes
FM and MFM; Intel's final 82077AA datasheet explicitly removes FM support.
The branch also offers selected later-controller features such as the FIFO.

### GPL-aware Format Track

New controller formats on SCP, HxC MFM, 86F and TransCopy use the IBM
3740/System 34 index gap, IAM, post-index gap, ID and data sync fields,
marks and CRCs. The command's GPL byte controls Gap 3. The command's N
controls payload length, independently of the ID's N byte. Sectors are
written sequentially at the selected rate rather than spread evenly using
SC; the remaining Gap 4b runs to the next index. A format never wraps its
output over the first field. DMA and PCjr non-DMA ID requests follow the
same field cadence, and normal completion waits for index.

`Guest.FloppyFormatGap` measures duplicate and nonsequential Read ID results
at GPL 0, 84 and 240, in FM and MFM, through DMA and PCjr non-DMA formatting
on all four track backends. It checks absolute first-ID position, successive
ID spacing, index wrap, terminal count, source integrity and read-only replay
after restart. A 512-byte MFM sector at the 250-kbps selector occupies
`(62 + 512 + GPL) * 32` microseconds per sector; a 128-byte FM sector occupies
`(33 + 128 + GPL) * 64` microseconds. FM Read ID completion includes the
seven-byte FE/CHRN/CRC field; MFM also includes its three A1 sync bytes.

The layout follows Figure 5 of the
[NEC uPD765A datasheet](https://hxc2001.com/download/datasheet/floppy/thirdparty/FDC/NEC/upd765A.pdf).
This remains a digital controller model; it does not add analog write
precompensation or analog write-splice noise.

Overlay version 11 stores GPL after the format encoding byte. Encoding bit
0 still selects FM; bit 1 indicates that GPL is present. A format without
GPL writes zero in that byte and retains its historical layout. Versions
1 through 10 remain readable. The probe explicitly verifies version-10
FM/MFM formats retain their old evenly spaced positions. Sector write
records are unchanged. IMG and IMA remain logical sector files: formatting
updates their sectors, but these containers cannot preserve physical gaps.

### Partial writes interrupted by DOR reset

`Guest.FloppyResetWrite` resets Write Data before transfer, during payload,
during CRC emission and after completion. It checks FM/MFM, DMA and native
PCjr non-DMA transfers, FIFO-prefetched bytes, duplicate IDs and replay after
restart across SCP, HxC MFM, 86F and TransCopy. The selected occurrence changes;
the other duplicate retains its data and valid CRC. Long Read Track exposes
the old CRC or its partly rewritten prefix, rather than merely checking an
error flag. A successful earlier write must remain the source of the untouched
suffix. The probe also rejects malformed partial-write overlays and checks
IMG/IMA payload persistence. `FloppyTrackData.StopWriteAtResetCell` verifies
exact cell boundaries and that only written weak cells become stable.

The controller tracks media progress separately from host DMA/FIFO progress.
On DOR reset it records the emitted cells, including a partially emitted byte
or CRC, and cancels the remaining events. The old suffix and unrecorded CRC
cells remain unchanged. CRC status comes from decoding the resulting cells;
reset does not append a fresh valid CRC or force an artificial CRC error.
Reset before the data transfer leaves the field unchanged, and reset after
normal completion retains the completed write. The DAM is treated as emitted
at payload start; sub-byte write-gate timing in the preceding sync field and
analog splice effects are not modeled. Reset IRQ/status handling is retained.
The behavior follows the reset/write-corruption warning in section 2.2 of the
[Intel 82077AA datasheet](https://www.osdever.net/documents/82077AA_FloppyControllerDatasheet.pdf).

Overlay version 12 adds sector-flag bit 2 for a partial write. Such records
store a four-byte little-endian cell count after the flags and before the
payload. The count starts at the DAM and includes clock and data cells.
It must be positive and cannot exceed DAM, payload and CRC. Earlier versions
1 through 11 remain readable, and ordinary writes keep their existing layout
unless partial-write history requires version 12. Partial records retain write
order because their suffix depends on earlier writes. Source captures remain
unchanged. IMG/IMA preserve the changed payload bits directly but cannot store
damaged CRCs or physical clock cells.

### Partial writes interrupted by motor loss

`Guest.FloppyMotorWrite` checks the same written-cell preservation when the
selected motor is disabled without resetting the controller. It covers both
standard-PC drives, native PCjr non-DMA transfers, FM/MFM, FIFO prefetch,
payload and CRC interruptions, earlier write history and restart persistence
across SCP, HxC MFM, 86F and TransCopy. Five-revolution SCP and IMG/IMA controls
are included. Stopping B's motor during an A write must leave that write alone.
Re-enabling the selected motor must allow reads without a controller reset.
The interrupted command returns its existing drive-unavailable status and
standard-PC IRQ6 behavior; PCjr retains its polled completion path.

This uses the model's existing digital drive-unavailable boundary. It does not
simulate mechanical coast-down or a universal physical write-gate cutoff.
The [TEAC FD-55BV specification](https://deramp.com/downloads/floppy_drives/teac/Teac%20FD-55BV.pdf)
specifies READY deassertion within 0.3 ms of MOTOR ON becoming false, but that
is not a spindle-stop time. Its write-mode expression differs from the READY
gating described in the older
[FD-55E/F specification](https://nfggames.com/X68000/Documentation/Floppy%20Drives/Teac/1981%20Teac%20FD55-E%20FD55-F%20Drive%20Specification.pdf).
Drive-specific write gating and changing rotational speed remain separate work.
Partial writes use the existing version-12 overlay records, preserving the
original capture and the untouched duplicate's cells and CRC.

### PC-compatible TransCopy types

The parser accepts disk types 02h (MFM high density), 03h (MFM DD captured in a
360-RPM drive), 05h (FM single density), 07h (MFM DD), 0Ch (Atari FM), and FFh
(unknown). This covers PC-compatible IBM FM/MFM fields; it does not add Apple II
GCR, Commodore GCR or Amiga sector decoding. Those types remain rejected.

Track type bits in the flags table at 0705h override the disk type. The separate
bit-15 capture flag is masked off. Missing/unknown track types and the unused
4444h table sentinel fall back to the disk type. FM records shorter than 8,000
bytes use the single-density sampling clock; longer records retain the
higher sampling clock. Unknown records retain the existing DD/HD length
heuristic. These are sampling clocks, not instructions to force the guest FDC
to use one data rate. Mount probing also recognizes fields recorded at another
supported data rate, including 300-kbps data sampled at a higher clock.

`Guest.FloppyTransCopyTypes` checks Read ID, Read Data, Read Track and Verify,
DMA terminal count, IRQ6 and buffer guards for all six PC-compatible disk types.
Its 03h fixture samples 300-kbps MFM at a 1-MHz cell clock. Mixed disk/track
headers are checked through BOOT and DOS FAT. Parser tests check per-track
overrides, capture flags, sentinels and rejection of non-PC/undefined types.

The type list and flags layout follow NewRisingSun's 2017-05-31 description:
https://www.robcraig.com/download/transcopy-5-x-file-format/
PCE's independent PRI reader also distinguishes short single-density records
from longer double-density records. No source code was copied from it.

The supplementary address-mark timing table at 0905h is still not applied.
Read ID timing comes from bit positions, sampling clock and index skew. TC's
weak-cell model is inferred from the bitstream, since the container has no
explicit weak-bit mask. Capture flags that control TC.EXE's physical write-back
and the alignment padding outside each declared track extent are not modeled
as separate read-time behavior. These are limits of conformance, not claims of
complete implementation of every TC.EXE write-back option.


`Guest.FloppyFormatFinalGap` interrupts formatting after the last requested data
field, while gap 4b is still being written. Reset and motor-loss cases must retain
both the new field and the unwritten old suffix after a fresh mount. Read ID,
Read Data and a two-field DMA Read Track verify the result for SCP, MFM, 86F and
TransCopy; normal completion erases the old suffix, and early terminal count is
an additional partial-format control. Source capture bytes remain unchanged.

Overlay version 13 appends a four-byte little-endian cell cutoff after each
format record's GPL byte (zero means no explicit cutoff). A nonzero cutoff uses
the partial-format marker even when all requested ID tuples have arrived. The
cutoff counts encoding cells from index at the format command's clock, including
cells written in gap 4b. Older overlays retain their prior interpretation; new
overlays continue using an older version unless a cutoff is required.
