#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Incremental Linux floppy tests and a full-suite pre-push validation gate."""

import argparse
from datetime import datetime, timezone
from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


INPUT_DIRECTORIES = ("src", "include", "tests", "cmake", "scripts", "resources", ".github")
STAMP = ".floppy-full-validation.json"
GROUPS = {
    "focused": r"^Guest\.FloppyBios(FormatMachine|Machine)$",
    "smoke": (r"^SectorMedia\.|^FloppyFixtureCache$|^FloppyWorkflow$|"
              r"^Guest\.(FloppyReadDeletedSmoke|FloppySectorContract|FloppyFatCache)$"),
}


def digest_file(path):
    """Hash complete file contents without relying on timestamps."""
    digest = sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def input_files(source):
    """List build/test inputs, excluding generated work and Git metadata."""
    for path in sorted(source.iterdir()):
        if (path.is_file() and not path.is_symlink() and path.name not in (STAMP, ".git")
                and path.suffix not in (".log", ".tmp")):
            yield path
    for name in INPUT_DIRECTORIES:
        directory = source / name
        if not directory.is_dir():
            continue
        for current, folders, files in os.walk(directory):
            folders[:] = sorted(folder for folder in folders
                                if folder not in (".git", "__pycache__")
                                and not folder.endswith("_mount_test_files"))
            for name in sorted(files):
                path = Path(current) / name
                if path.suffix not in (".pyc", ".pyo") and not path.is_symlink():
                    yield path


def input_manifest(source):
    """Return a portable content manifest for the exact tested source tree."""
    return {path.relative_to(source).as_posix(): digest_file(path)
            for path in input_files(source)}


def manifest_digest(manifest):
    """Bind the full source manifest to one stable digest."""
    return sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()


def synchronize(source, target, manifest, build):
    """Copy changed files to an owned native snapshot; prune prior managed files."""
    if source == target:
        return
    state = build / "floppy-source-sync.json"
    previous = json.loads(state.read_text()) if state.exists() else {}
    copied = 0
    for name, digest in manifest.items():
        destination = target / name
        if not destination.resolve().is_relative_to(target.resolve()):
            raise ValueError("Source snapshot contains a path outside its root")
        if not destination.is_file() or digest_file(destination) != digest:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, destination)
            copied += 1
        if digest_file(destination) != digest:
            raise ValueError("Source changed while synchronizing the native snapshot")
    for name in previous.keys() - manifest.keys():
        destination = (target / name).resolve()
        if not destination.is_relative_to(target.resolve()):
            raise ValueError("Unsafe path in source-sync manifest")
        destination.unlink(missing_ok=True)
    state.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
    print(f"Native source sync: {copied} changed files", flush=True)


def cmake_source(build):
    """Get the source tree used by an existing configured build."""
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        raise ValueError("Configure a Linux build first; see docs/build-linux.md")
    for line in cache.read_text().splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
            return Path(line.split("=", 1)[1]).resolve()
    raise ValueError("CMake source directory is missing")


def run_logged(command, folder, log, environment):
    """Keep a complete log while showing incremental test progress."""
    print(" ".join(map(str, command)), flush=True)
    with log.open("w", encoding="utf-8") as output:
        with subprocess.Popen(command, cwd=folder, env=environment, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True) as process:
            for line in process.stdout:
                print(line, end="", flush=True)
                output.write(line)
                output.flush()
            status = process.wait()
    if status:
        raise RuntimeError(f"Command failed ({status}); see {log}")


def artifact_path(path, distro):
    """Let a Windows Git hook verify the native WSL artifacts too."""
    if os.name == "nt" and path.startswith("/"):
        if not distro or any(character in distro for character in "/\\:"):
            raise ValueError("A WSL distribution name is required to verify Linux artifacts")
        return Path("//wsl.localhost") / distro / path.lstrip("/")
    return Path(path)


def validate_stamp(source, stamp):
    """Reject partial tests, changed inputs, or replaced build artifacts."""
    if stamp.get("schema") != 1 or stamp.get("group") != "full" or not stamp.get("passed"):
        raise ValueError("A successful full CTest run is required before pushing")
    if stamp.get("source_digest") != manifest_digest(input_manifest(source)):
        raise ValueError("Build/test inputs changed since the full run; rerun full tests")
    for name, digest in stamp.get("artifacts", {}).items():
        path = artifact_path(name, stamp.get("distro"))
        if not path.is_file() or digest_file(path) != digest:
            raise ValueError(f"A tested build artifact changed or is unavailable: {path}")
    if len(stamp.get("artifacts", {})) != 3:
        raise ValueError("Full-test artifact manifest is incomplete")


def git_output(source, *arguments):
    """Read repository state without changing the index or remote."""
    return subprocess.check_output(["git", "-C", str(source), *arguments], text=True).strip()


def verify_push(source, rows):
    """Allow only the current clean, fully tested tree, consuming its approval."""
    if not rows:
        return
    path = source / STAMP
    if not path.is_file():
        raise ValueError("No full-test validation: run test_floppy.py full before pushing")
    validate_stamp(source, json.loads(path.read_text()))
    if git_output(source, "status", "--porcelain", "--untracked-files=normal"):
        raise ValueError("Commit or stash local changes, then validate the exact tree "
                         "before pushing")
    head = git_output(source, "rev-parse", "HEAD")
    branch = "refs/heads/" + git_output(source, "branch", "--show-current")
    for row in rows:
        local_ref, local_sha, remote_ref, _ = row.split()
        if (local_ref not in (branch, "HEAD") or local_sha != head
                or remote_ref != branch):
            raise ValueError("Push only the checked-out, fully tested branch; "
                             "validate other refs separately")
    path.unlink()
    print("Full CTest validation verified for this push; validation consumed.", flush=True)


def install_hook(source):
    """Install the local gate without replacing unrelated hooks or changing branches."""
    configured = subprocess.run(
        ["git", "-C", str(source), "config", "--get", "core.hooksPath"],
                                capture_output=True, text=True, check=False)
    if configured.stdout.strip():
        raise ValueError("An existing core.hooksPath is configured; integrate the "
                         "provided hook there")
    path = Path(git_output(source, "rev-parse", "--git-path", "hooks/pre-push"))
    if not path.is_absolute():
        path = source / path
    content = (source / "scripts/git-hooks/pre-push").read_bytes().replace(b"\r\n", b"\n")
    if path.exists() and path.read_bytes() != content:
        raise ValueError(f"An existing pre-push hook was preserved: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    path.chmod(path.stat().st_mode | 0o111)
    print(f"Installed local full-test gate: {path}")


def run_tests(args):
    """Build incrementally, run the selected group and stamp full runs only."""
    source, build = args.source.resolve(), args.build.resolve()
    stamp_path = source / STAMP
    stamp_path.unlink(missing_ok=True)
    before = input_manifest(source)
    native_source = cmake_source(build)
    if native_source != source:
        if not args.sync_source:
            raise ValueError("Build source differs; pass --sync-source to update "
                             "the native snapshot")
        synchronize(source, native_source, before, build)
    environment = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    if args.no_cache:
        environment.pop("DOSBOX_FLOPPY_FIXTURE_CACHE", None)
    else:
        environment["DOSBOX_FLOPPY_FIXTURE_CACHE"] = str(build / "floppy-fixture-cache")
    run_logged(["cmake", "--build", str(build), "--parallel", str(args.jobs)], source,
               build / "floppy-test-build.log", environment)
    artifacts = {str(path): digest_file(path) for path in
                 (build / "dosbox", build / "tests/dosbox_tests", build / "CMakeCache.txt")}
    command = ["ctest", "--test-dir", str(build), "--parallel", str(args.jobs),
               "--output-on-failure", "--no-tests=error"]
    if args.group != "full":
        command += ["-R", GROUPS[args.group]]
    run_logged(command, source, build / f"floppy-test-{args.group}.log", environment)
    if input_manifest(source) != before or any(digest_file(Path(path)) != digest
                                               for path, digest in artifacts.items()):
        raise ValueError("Inputs or build artifacts changed during testing; no validation saved")
    if args.group == "full":
        stamp = {"schema": 1, "group": "full", "passed": True,
                 "source_digest": manifest_digest(before), "artifacts": artifacts,
                 "distro": os.environ.get("WSL_DISTRO_NAME"),
                 "completed_utc": datetime.now(timezone.utc).isoformat()}
        stamp_path.write_text(json.dumps(stamp, indent=2), encoding="utf-8")
        print(f"Full-test validation saved: {stamp_path}", flush=True)


def main():
    """Run a focused, smoke or full suite, or verify/install the push gate."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("group", nargs="?", default="smoke",
                        choices=(*GROUPS, "full", "verify-push"))
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build", type=Path)
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--sync-source", action="store_true")
    parser.add_argument("--no-cache", action="store_true")
    parser.add_argument("--install-hook", action="store_true")
    args = parser.parse_args()
    try:
        if args.install_hook:
            install_hook(args.source.resolve())
        elif args.group == "verify-push":
            rows = [line for line in sys.stdin.read().splitlines() if line]
            verify_push(args.source.resolve(), rows)
        else:
            if args.build is None or args.jobs < 1:
                raise ValueError("Specify an existing --build directory and a positive "
                                 "--jobs count")
            run_tests(args)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Floppy tests: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
