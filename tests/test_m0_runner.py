"""Offline M0-B2 safety checks. All mounted evidence below is mocked."""

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


BENCHMARKS = Path(__file__).resolve().parents[1] / "benchmarks"
sys.path.insert(0, str(BENCHMARKS))
import m0_fixture as fixture
import m0_csv_workflow as application

spec = importlib.util.spec_from_file_location("m0_profile", BENCHMARKS / "m0_profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class FakeClock:
    def __init__(self):
        self.value = 0.0

    def now(self):
        return self.value

    def sleep(self, seconds):
        self.value += seconds


class FakeProcesses:
    evidence = "mocked"

    def __init__(self, clock):
        self.clock = clock
        self.entries = []
        self.events = []
        self.on_change = lambda: None
        self.application_error = False
        self.application_timeout = False
        self.shutdown_timeout = False
        self.abnormal_exit = False
        self.stale = False
        self.corrupt_output = False
        self.signals = []

    def launch(self, command, log, limit):
        identity = {"pid": 1000 + len(self.entries), "start_ticks": len(self.entries),
                    "boot_id": "mock-boot", "evidence": "mocked"}
        entry = {"identity": identity, "command": command, "exit_status": None}
        self.entries.append(entry)
        self.events.append("launch")
        Path(log).write_text("MOCKED daemon log\n", encoding="ascii")
        self.on_change()
        return identity

    def owned(self, identity):
        return not self.stale and any(x["identity"] == identity for x in self.entries)

    def poll(self, identity):
        if not self.owned(identity):
            raise runner.TrialError("process identity cannot be established")
        return next(x["exit_status"] for x in self.entries if x["identity"] == identity)

    def wait(self, identity, deadline):
        if self.shutdown_timeout:
            self.clock.value = deadline
            raise runner.TrialError("shutdown timeout")
        code = 9 if self.abnormal_exit else 0
        next(x for x in self.entries if x["identity"] == identity)["exit_status"] = code
        self.events.append("exit")
        self.on_change()
        return code

    def signal_owned(self, identity, signum):
        if not self.owned(identity):
            raise runner.TrialError("stale process identity; refusing signal")
        self.signals.append((identity, signum))

    def run(self, command, log, limit, deadline):
        Path(log).write_text("MOCKED child log\n", encoding="ascii")
        if command[2] == "-c":
            kind, artifacts, root, result = command[-4:]
            value = (runner.stage_files if kind == "stage" else runner.verify_files)(
                Path(artifacts), Path(root))
            Path(result).write_text(json.dumps(value), encoding="ascii")
            return {"exit_status": 0, "evidence": "mocked"}
        self.events.append("application")
        if self.application_timeout:
            self.clock.value = deadline
            raise runner.TrialError("application timeout")
        if self.application_error:
            return {"exit_status": 7, "evidence": "mocked"}
        application.run(Path(command[-1]))
        if self.corrupt_output:
            (Path(command[-1]) / "output.csv").write_bytes(b"wrong\n")
        return {"exit_status": 0, "evidence": "mocked"}


class FakeMounts:
    evidence = "mocked"

    def __init__(self):
        self.available = True
        self.visible = True
        self.unmount_error = False
        self.current = []
        self.events = []
        self.mounted = None
        self.sequence = 0

    def preflight(self):
        if not self.available:
            raise runner.TrialError("mount access unavailable (mocked)")

    def snapshot(self):
        return list(self.current)

    def command(self, daemon, root, backing, source):
        self.sequence += 1
        self.mounted = {"target": str(root), "source": source, "fstype": "fuse.myfs",
                        "root": "/", "mount_id": self.sequence, "evidence": "mocked"}
        self.current = [self.mounted] if self.visible else []
        self.events.append("mount")
        return [str(daemon), "-f", "-o", f"fsname={source},subtype=myfs", str(root), str(backing)]

    def probe(self, root, processes, deadline, log, limit):
        self.events.append("probe")
        Path(log).write_text("MOCKED functional probe\n", encoding="ascii")
        return {"exit_status": 0, "evidence": "mocked"}

    def unmount(self, root, processes, deadline, log, limit):
        self.events.append("unmount")
        if self.unmount_error:
            raise runner.TrialError("unmount failed (mocked)")
        self.current = []
        Path(log).write_text("MOCKED orderly unmount\n", encoding="ascii")
        return {"exit_status": 0, "evidence": "mocked"}


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="m0-b2-tests-")
        self.parent = Path(self.temp.name)
        self.clock = FakeClock()
        self.processes = FakeProcesses(self.clock)
        self.mounts = FakeMounts()

    def tearDown(self):
        # Only owned ordinary temporary directories: no real mounts exist.
        self.temp.cleanup()

    def prepare(self, backend="myfs", **kwargs):
        return runner.Trial.prepare(
            self.parent, backend=backend, records=32, daemon=Path(sys.executable).resolve(),
            build_provenance="TEST ONLY: mocked lifecycle, not a myfs executable",
            processes=self.processes, mounts=self.mounts, clock=self.clock, **kwargs)

    def failed(self, trial, reason):
        self.assertFalse(trial.execute())
        state = json.loads((trial.artifacts / "state.json").read_text())
        self.assertEqual(state["outcome"], "failed")
        self.assertIn(reason, state["error"])
        self.assertTrue(trial.workspace.exists())
        self.assertTrue((trial.artifacts / "input.csv").exists())
        self.assertTrue((trial.artifacts / "verification.jsonl").exists())
        return state

    def test_successful_mocked_lifecycle_and_canonical_preservation(self):
        trial = self.prepare()
        before = trial.canonical_snapshot()
        self.assertTrue(trial.execute())
        self.assertEqual(before, trial.canonical_snapshot())
        self.assertEqual(self.mounts.events, ["mount", "probe", "unmount"] * 3)
        self.assertEqual(self.processes.events,
                         ["launch", "exit", "launch", "application", "exit", "launch", "exit"])
        self.assertEqual([p["name"] for p in trial.state["phases"]], [
            "preflight", "prepare", "preparation_mount", "stage", "preparation_unmount",
            "application_mount", "application", "verify", "application_unmount",
            "verification_mount", "remount_verify", "final_unmount"])
        self.assertEqual(trial.state["evidence"], "mocked")
        self.assertTrue(all(p["outcome"] == "passed" for p in trial.state["phases"]))

    def test_requested_myfs_without_mount_access_fails_without_substitution(self):
        self.mounts.available = False
        trial = self.prepare()
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertEqual(self.processes.events, [])
        self.assertFalse(trial.execute())
        self.assertEqual(trial.state["backend"], "myfs")

    def test_ordinary_directory_does_not_satisfy_mount_readiness(self):
        self.mounts.visible = False
        trial = self.prepare(readiness_seconds=0.2)
        self.failed(trial, "readiness timeout")
        self.assertNotIn("probe", self.mounts.events)

    def test_application_failure(self):
        trial = self.prepare()
        self.processes.application_error = True
        self.failed(trial, "application exit")

    def test_output_validation_failure(self):
        trial = self.prepare()
        self.processes.corrupt_output = True
        state = self.failed(trial, "canonical validation")
        self.assertFalse(state["validations"][-1]["output"]["passed"])

    def test_corrupt_staged_input_and_self_consistent_output_rejected(self):
        trial = self.prepare()
        original = self.processes.run

        def corrupt_then_run(command, *args):
            if command[2] == "-c":
                return original(command, *args)
            path = Path(command[-1]) / "input.csv"
            path.write_bytes(path.read_bytes().replace(b",ok,", b",er,"))
            return original(command, *args)

        self.processes.run = corrupt_then_run
        state = self.failed(trial, "canonical validation")
        self.assertFalse(state["validations"][-1]["input"]["passed"])
        self.assertTrue(fixture.validate_output(trial.root / "input.csv", trial.root / "output.csv")["passed"])

    def test_application_timeout(self):
        trial = self.prepare(application_seconds=0.2)
        self.processes.application_timeout = True
        self.failed(trial, "application timeout")

    def test_shutdown_timeout_invalidates_trial_and_refuses_cleanup(self):
        trial = self.prepare(shutdown_seconds=0.2)
        self.processes.shutdown_timeout = True
        self.failed(trial, "shutdown timeout")
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.assertEqual(self.processes.signals, [])

    def test_unmount_failure_retains_evidence_and_data(self):
        trial = self.prepare()
        self.mounts.unmount_error = True
        self.failed(trial, "unmount failed")
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.assertTrue((trial.root / "input.csv").exists())

    def test_abnormal_daemon_exit_invalidates_trial(self):
        trial = self.prepare()
        self.processes.abnormal_exit = True
        self.failed(trial, "daemon exit")

    def test_stale_identity_is_never_signaled(self):
        trial = self.prepare()
        self.processes.stale = True
        self.failed(trial, "identity")
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.assertEqual(self.processes.signals, [])

    def test_cleanup_preserves_artifacts(self):
        trial = self.prepare(backend="offline-directory")
        self.assertTrue(trial.execute())
        before = trial.canonical_snapshot()
        trial.cleanup()
        self.assertFalse(trial.workspace.exists())
        self.assertTrue(trial.artifacts.exists())
        self.assertEqual(before, trial.canonical_snapshot())
        self.assertEqual(trial.state["outcome"], "cleaned")

    def test_cleanup_refuses_unowned_path(self):
        with self.assertRaises(runner.TrialError):
            runner.Trial.load(self.parent, processes=self.processes, mounts=self.mounts, clock=self.clock)

    def test_cleanup_refuses_symlink_substitution(self):
        trial = self.prepare(backend="offline-directory")
        self.assertTrue(trial.execute())
        moved = trial.workspace.with_name("saved-workspace")
        trial.workspace.rename(moved)
        trial.workspace.symlink_to(moved, target_is_directory=True)
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.assertTrue(moved.exists())

    def test_cleanup_refuses_changed_directory_identity(self):
        trial = self.prepare(backend="offline-directory")
        self.assertTrue(trial.execute())
        trial.root.rename(trial.workspace / "old-root")
        trial.root.mkdir()
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_cleanup_refuses_state_path_escape(self):
        trial = self.prepare(backend="offline-directory")
        self.assertTrue(trial.execute())
        trial.state["roots"]["workspace"] = str(self.parent)
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.assertTrue(trial.workspace.exists())

    def test_cleanup_refuses_unexpected_nested_mount(self):
        trial = self.prepare(backend="offline-directory")
        self.assertTrue(trial.execute())
        self.mounts.current = [{"target": str(trial.root / "nested"), "source": "unrelated"}]
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_canonical_corruption_invalidates_verification(self):
        trial = self.prepare(backend="offline-directory")
        (trial.artifacts / "expected.csv").write_bytes(b"corrupt\n")
        self.failed(trial, "canonical artifact changed")

    def test_unavailable_evidence_is_null_with_reason(self):
        trial = self.prepare(backend="offline-directory")
        self.assertTrue(trial.execute())
        for value in trial.state["unavailable"].values():
            self.assertIsNone(value["value"])
            self.assertTrue(value["reason"])
        self.assertIn("offline correctness", trial.state["label"])

    def test_reloaded_verification_preserves_child_exit_evidence(self):
        trial = runner.Trial.prepare(self.parent, backend="offline-directory", records=8)
        self.assertTrue(trial.execute())
        children = json.loads(json.dumps(trial.state["children"]))
        self.assertGreater(len(children), 0)
        loaded = runner.Trial.load(trial.artifacts)
        self.assertTrue(loaded.verify())
        for child in children:
            self.assertIn(child, loaded.state["children"])

    def test_canonical_symlink_substitution_refused(self):
        trial = self.prepare(backend="offline-directory")
        expected = trial.artifacts / "expected.csv"
        moved = self.parent / "saved-expected.csv"
        expected.rename(moved)
        expected.symlink_to(moved)
        self.failed(trial, "symlink")

    def test_cleanup_refuses_unconfirmed_retained_child(self):
        trial = self.prepare(backend="offline-directory")
        self.assertTrue(trial.execute())
        trial.state["children"].append({"identity": {"pid": 999999}, "exit_status": None})
        trial.save()
        loaded = runner.Trial.load(trial.artifacts, processes=self.processes,
                                   mounts=self.mounts, clock=self.clock)
        with self.assertRaises(runner.TrialError):
            loaded.cleanup()
        self.assertEqual(self.processes.signals, [])

    def test_cleanup_refuses_replacement_at_deletion_boundary(self):
        trial = self.prepare(backend="offline-directory")
        self.assertTrue(trial.execute())
        remove = runner.remove_owned_tree
        saved = self.parent / "saved-at-boundary"

        def substitute(*args, **kwargs):
            trial.workspace.rename(saved)
            trial.workspace.mkdir()
            (trial.workspace / "unrelated.txt").write_text("preserve me")
            return remove(*args, **kwargs)

        with mock.patch.object(runner, "remove_owned_tree", side_effect=substitute):
            with self.assertRaises(runner.TrialError):
                trial.cleanup()
        self.assertEqual((trial.workspace / "unrelated.txt").read_text(), "preserve me")
        self.assertTrue(saved.exists())

    def test_late_successful_application_is_a_timeout(self):
        trial = self.prepare(application_seconds=0.1)
        original = self.processes.run

        def late(command, log, limit, deadline):
            value = original(command, log, limit, deadline)
            if command[2] != "-c":
                self.clock.value = deadline + 0.1
            return value

        self.processes.run = late
        self.failed(trial, "application timeout")

    def test_unexpected_mount_identity_never_probed_or_unmounted(self):
        trial = self.prepare()
        original = self.mounts.command

        def wrong_identity(*args):
            command = original(*args)
            self.mounts.current[0]["source"] = "unrelated"
            return command

        self.mounts.command = wrong_identity
        self.failed(trial, "unexpected mount identity")
        self.assertEqual(self.mounts.events, ["mount"])

    def test_failed_trial_cannot_pass_later_verification(self):
        trial = self.prepare(backend="offline-directory")
        self.processes.application_error = True
        self.failed(trial, "application exit")
        self.assertFalse(trial.verify())
        self.assertEqual(trial.state["outcome"], "failed")

    def test_identity_capture_failure_records_launch_without_signaling(self):
        processes = runner.Processes(runner.Clock())
        # A fake Popen object: this failure test launches no real process.
        child = mock.Mock(pid=424242)
        with mock.patch.object(runner.subprocess, "Popen", return_value=child), \
                mock.patch.object(runner.os, "pidfd_open", side_effect=OSError("mocked pidfd refusal")):
            with self.assertRaises(runner.TrialError):
                processes.launch(["MOCKED executable"], self.parent / "identity-failure.log", 4096)
        self.assertEqual(len(processes.entries), 1)
        entry = processes.entries[0]
        self.assertIsNone(entry["identity"])
        self.assertIsNone(entry["exit_status"])
        self.assertIn("mocked pidfd refusal", entry["identity_unavailable_reason"])
        self.assertFalse(processes.owned({"pid": 424242}))
        child.send_signal.assert_not_called()
        processes.handles[424242]["log"].close()

    def test_size_limit_and_fresh_output(self):
        with self.assertRaises(runner.TrialError):
            runner.Trial.prepare(self.parent, backend="offline-directory", records=32769)
        trial = self.prepare(backend="offline-directory")
        (trial.root / "output.csv").write_bytes(b"existing")
        self.failed(trial, "existing output")
        self.assertEqual((trial.root / "output.csv").read_bytes(), b"existing")

    def test_actual_offline_directory_execution_and_reload_cleanup(self):
        trial = runner.Trial.prepare(self.parent, backend="offline-directory", records=64)
        self.assertTrue(trial.execute(), trial.state.get("error"))
        self.assertEqual(trial.state["evidence"], "actual offline execution")
        self.assertEqual(trial.state["logical"]["input_rows"], 64)
        loaded = runner.Trial.load(trial.artifacts)
        self.assertTrue(loaded.verify())
        loaded.cleanup()
        self.assertFalse(trial.workspace.exists())

    def test_actual_owned_helper_timeout_and_stale_identity(self):
        clock = runner.Clock()
        processes = runner.Processes(clock)
        identity = processes.launch([sys.executable, "-B", "-c", "import time; time.sleep(30)"],
                                    self.parent / "helper.log", 4096)
        stale = dict(identity, start_ticks=-1)
        with self.assertRaises(runner.TrialError):
            processes.signal_owned(stale, 15)
        self.assertIsNone(processes.poll(identity))
        with self.assertRaises(runner.TrialError):
            processes.wait(identity, clock.now() + 0.03)
        processes.signal_owned(identity, 15)
        self.assertIsNotNone(processes.wait(identity, clock.now() + 2))


if __name__ == "__main__":
    unittest.main()
