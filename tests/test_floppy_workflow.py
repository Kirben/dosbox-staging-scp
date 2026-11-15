#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Ensure fast test runs and stale inputs cannot authorize a Git push."""

import argparse
from importlib.util import module_from_spec, spec_from_file_location
import json
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch


SPEC = spec_from_file_location("floppy_workflow", Path(__file__).parents[1]
                               / "scripts/tools/test_floppy.py")
WORKFLOW = module_from_spec(SPEC)
SPEC.loader.exec_module(WORKFLOW)


@unittest.skipUnless(shutil.which("git"), "Git is required for the local gate tests")
class WorkflowTest(unittest.TestCase):
    """Use an isolated repository; never contact any remote."""

    def setUp(self):
        # unittest owns this resource across the setup and test methods.
        # pylint: disable-next=consider-using-with
        self.temporary = TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "src").mkdir()
        (self.source / "src/input.cpp").write_text("tested source\n")
        (self.source / "tests").mkdir()
        (self.source / "tests/CMakeLists.txt").write_text(
            "add_floppy_guest_test(Probe probe.py)\n")
        (self.source / ".gitignore").write_text(WORKFLOW.STAMP + "\n")
        self.build = self.root / "build"
        (self.build / "tests").mkdir(parents=True)
        (self.build / "dosbox").write_bytes(b"emulator")
        (self.build / "tests/dosbox_tests").write_bytes(b"tests")
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.source}\n")
        self.git("init", "-q", "-b", "test-branch")
        self.git("add", ".")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-qm", "Test fixture")

    def git(self, *args):
        """Run Git only in the disposable repository."""
        return subprocess.check_output(["git", "-C", str(self.source), *args], text=True).strip()

    def stamp(self):
        """Create a simulated successful full run for gate unit testing."""
        return {"schema": 2, "group": "full", "passed": True,
                "floppy_tests": WORKFLOW.required_floppy_tests(self.source),
                "source_digest": WORKFLOW.manifest_digest(WORKFLOW.input_manifest(self.source)),
                "artifacts": {str(path): WORKFLOW.digest_file(path) for path in
                              (self.build / "dosbox", self.build / "tests/dosbox_tests",
                               self.build / "CMakeCache.txt")}}

    def save_stamp(self, stamp):
        """Persist a unit-test receipt in the ignored local file."""
        (self.source / WORKFLOW.STAMP).write_text(json.dumps(stamp))

    def test_partial_and_stale_results_are_rejected(self):
        """Only full tests of the current code and artifacts are accepted."""
        stamp = self.stamp()
        WORKFLOW.validate_stamp(self.source, stamp)
        stamp["group"] = "smoke"
        with self.assertRaises(ValueError):
            WORKFLOW.validate_stamp(self.source, stamp)
        stamp = self.stamp()
        (self.source / "src/input.cpp").write_text("changed source\n")
        with self.assertRaises(ValueError):
            WORKFLOW.validate_stamp(self.source, stamp)
        stamp = self.stamp()
        (self.build / "dosbox").write_bytes(b"replaced emulator")
        with self.assertRaises(ValueError):
            WORKFLOW.validate_stamp(self.source, stamp)

    def test_fast_and_failed_runs_cannot_leave_an_approval(self):
        """A fast success or a full failure invalidates an older receipt."""
        args = argparse.Namespace(source=self.source, build=self.build, sync_source=False,
                                  no_cache=True, jobs=1, group="focused")
        self.save_stamp(self.stamp())
        with patch.object(WORKFLOW, "run_logged"):
            WORKFLOW.run_tests(args)
        self.assertFalse((self.source / WORKFLOW.STAMP).exists())
        self.save_stamp(self.stamp())
        args.group = "full"
        with patch.object(WORKFLOW, "run_logged", side_effect=RuntimeError("failure")):
            with self.assertRaises(RuntimeError):
                WORKFLOW.run_tests(args)
        self.assertFalse((self.source / WORKFLOW.STAMP).exists())

    def test_push_requires_clean_current_ref_and_consumes_receipt(self):
        """An accepted push uses its one full-run receipt exactly once."""
        head = self.git("rev-parse", "HEAD")
        row = f"refs/heads/test-branch {head} refs/heads/test-branch {'0' * 40}"
        with self.assertRaises(ValueError):
            WORKFLOW.verify_push(self.source, [row])
        self.save_stamp(self.stamp())
        with self.assertRaises(ValueError):
            WORKFLOW.verify_push(self.source, [row.replace(head, "1" * 40)])
        (self.source / "website").mkdir()
        draft = self.source / "website/draft.txt"
        draft.write_text("uncommitted documentation")
        with self.assertRaises(ValueError):
            WORKFLOW.verify_push(self.source, [row])
        draft.unlink()
        WORKFLOW.verify_push(self.source, [row])
        self.assertFalse((self.source / WORKFLOW.STAMP).exists())
        with self.assertRaises(ValueError):
            WORKFLOW.verify_push(self.source, [row])

    def test_sync_changes_only_changed_files_and_prunes_owned_paths(self):
        """An unchanged mirror keeps timestamps; removed managed files disappear."""
        target = self.root / "mirror"
        manifest = WORKFLOW.input_manifest(self.source)
        WORKFLOW.synchronize(self.source, target, manifest, self.build)
        first_time = (target / "src/input.cpp").stat().st_mtime_ns
        WORKFLOW.synchronize(self.source, target, manifest, self.build)
        self.assertEqual(first_time, (target / "src/input.cpp").stat().st_mtime_ns)
        (self.source / "src/input.cpp").unlink()
        WORKFLOW.synchronize(self.source, target, WORKFLOW.input_manifest(self.source), self.build)
        self.assertFalse((target / "src/input.cpp").exists())

    def test_missing_or_disabled_guest_registration_is_rejected(self):
        """Ordinary native tests cannot substitute for omitted floppy probes."""
        names = WORKFLOW.required_floppy_tests(self.source)
        tests = [{"name": name, "properties": []} for name in names]
        with patch.object(WORKFLOW.subprocess, "check_output",
                          return_value=json.dumps({"tests": tests[:-1]})):
            with self.assertRaises(ValueError):
                WORKFLOW.verify_full_registration(self.source, self.build, {})
        tests[-1]["properties"] = [{"name": "DISABLED", "value": True}]
        with patch.object(WORKFLOW.subprocess, "check_output",
                          return_value=json.dumps({"tests": tests})):
            with self.assertRaises(ValueError):
                WORKFLOW.verify_full_registration(self.source, self.build, {})
        tests[-1]["properties"] = []
        with patch.object(WORKFLOW.subprocess, "check_output",
                          return_value=json.dumps({"tests": tests})):
            self.assertEqual(names, WORKFLOW.verify_full_registration(
                self.source, self.build, {}))

    def test_full_results_require_executed_probes(self):
        """A successful CTest exit cannot authorize skipped or unexecuted probes."""
        names = WORKFLOW.required_floppy_tests(self.source)
        report = self.build / "results.xml"
        cases = [f'<testcase name="{name}" status="run"/>' for name in names]
        report.write_text("<testsuite>" + "".join(cases[:-1]) + "</testsuite>")
        with self.assertRaises(ValueError):
            WORKFLOW.verify_full_results(report, names)
        cases[-1] = f'<testcase name="{names[-1]}" status="notrun"><skipped/></testcase>'
        report.write_text("<testsuite>" + "".join(cases) + "</testsuite>")
        with self.assertRaises(ValueError):
            WORKFLOW.verify_full_results(report, names)
        cases[-1] = f'<testcase name="{names[-1]}" status="run"/>'
        report.write_text("<testsuite>" + "".join(cases) + "</testsuite>")
        WORKFLOW.verify_full_results(report, names)
        stamp = self.stamp()
        stamp.pop("floppy_tests")
        with self.assertRaises(ValueError):
            WORKFLOW.validate_stamp(self.source, stamp)


if __name__ == "__main__":
    unittest.main()
