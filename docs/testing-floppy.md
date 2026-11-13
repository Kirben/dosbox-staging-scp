# Testing floppy-controller changes

Use a persistent, configured Linux build directory. The helper builds
incrementally, copies only changed source files when using a native snapshot,
and runs up to four test workers by default.

```sh
python3 scripts/tools/test_floppy.py focused --build /path/to/build
python3 scripts/tools/test_floppy.py smoke --build /path/to/build
python3 scripts/tools/test_floppy.py full --build /path/to/build
```

`focused` runs format-rate, motor-reporting and sector-contract regressions. `smoke` exercises
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
