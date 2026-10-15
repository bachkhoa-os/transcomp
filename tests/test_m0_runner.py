"""Offline M0-B2 safety checks. All mounted evidence below is mocked."""

import importlib.util
import contextlib
import cProfile
import hashlib
import io
import json
import marshal
import os
from pathlib import Path
import pstats
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


class MeasurementHost:
    """Deterministic host evidence; never an actual ext4/FUSE observation."""
    evidence = "mocked"

    def __init__(self, canonical_parent):
        self.canonical_parent = canonical_parent
        self.device_number = "259:7"
        self.free = 4 * 1024**3
        self.memory = 8 * 1024**3

    def device(self, path):
        canonical = path == self.canonical_parent or self.canonical_parent in path.parents
        return {"device": "0:99" if canonical else self.device_number,
                "fstype": "tmpfs" if canonical else "ext4", "source": "MOCKED",
                "mount_id": 9 if canonical else 7, "evidence": "mocked"}

    def resources(self, path):
        return {"available_disk_bytes": self.free, "available_memory_bytes": self.memory,
                "source": "MOCKED statvfs and MemAvailable", "evidence": "mocked"}

    def metadata(self):
        return {"kernel": "MOCKED", "affinity": [0, 1], "evidence": "mocked"}

    def affinity(self, identity, requested):
        return {"value": requested, "evidence": "mocked", "reason": None}


class MeasurementProcesses(FakeProcesses):
    def __init__(self, clock):
        super().__init__(clock)
        self.missing_endpoint = False
        self.reaps = {}

    def launch(self, command, log, limit):
        identity = super().launch(command, log, limit)
        self.entries[-1].update(launch_monotonic=self.clock.now(), exit_monotonic=None,
                                log=str(log), log_truncated=False)
        return identity

    def counters(self, identity):
        if self.missing_endpoint:
            return {"value": None, "reason": "mocked missed endpoint", "evidence": "mocked"}
        return {"value": {"read_bytes": 0, "write_bytes": 0}, "source": "MOCKED /proc",
                "monotonic": self.clock.now(), "evidence": "mocked", "reason": None}

    def wait(self, identity, deadline):
        entry = next(x for x in self.entries if x["identity"] == identity)
        if entry["exit_status"] is not None:
            return entry["exit_status"]
        if str(application.__file__) in entry["command"]:
            if self.application_timeout:
                self.clock.value = deadline
                raise runner.TrialError("application timeout")
            if not self.application_error:
                application.run(Path(entry["command"][-1]))
                if self.corrupt_output:
                    (Path(entry["command"][-1]) / "output.csv").write_bytes(b"wrong\n")
            self.clock.value += 5.0
            code = 7 if self.application_error else 0
            entry["exit_status"] = code
        else:
            self.clock.value += 0.4
            code = super().wait(identity, deadline)
        entry.update(exit_monotonic=self.clock.now(),
                     resources=runner.resource_metrics(None, self.evidence, identity,
                                                       entry["launch_monotonic"], self.clock.now()))
        self.reaps[identity["pid"]] = self.reaps.get(identity["pid"], 0) + 1
        self.on_change()
        return code

    def run(self, command, log, limit, deadline):
        if "generate_csv" in command[3]:
            root, records, seed = command[-3:]
            # A hard test-side tripwire: no test may allocate the full fixture.
            if int(records) > 32768:
                raise AssertionError("offline full-size fixture generation forbidden")
            root = Path(root)
            fixture.generate_csv(root / "input.csv", root / "input.manifest.json", int(records), int(seed))
            fixture.write_expected(root / "input.csv", root / "expected.csv")
            self.clock.value += .5
            Path(log).write_text("MOCKED bounded fixture helper\n")
            return {"exit_status": 0, "evidence": "mocked"}
        if "account_owned_tree" in command[3]:
            root, identity, result, materialize = command[-4:]
            value = runner.account_owned_tree(Path(root), json.loads(identity),
                                             self.clock.now() + 10, self.clock,
                                             materialize == "1")
            Path(result).write_text(json.dumps(value), encoding="ascii")
            Path(log).write_text("MOCKED accounting helper\n")
            return {"exit_status": 0, "evidence": "mocked"}
        self.clock.value += 2.0
        return super().run(command, log, limit, deadline)


class MeasurementMounts(FakeMounts):
    def probe(self, root, processes, deadline, log, limit):
        self.events.append("stat-probe")
        before = {p.name: p.stat().st_size for p in root.iterdir()}
        self.last_probe_before = before
        self.asserted_directory = root.is_dir()
        Path(log).write_text("MOCKED non-mutating root stat\n")
        return {"exit_status": 0, "evidence": "mocked", "kind": "root-stat"}


class MeasurementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="m0-d1a-tests-")
        self.parent = Path(self.temp.name)
        self.canonical_parent = self.parent / "references"
        self.canonical_parent.mkdir()
        self.clock = FakeClock()
        self.processes = MeasurementProcesses(self.clock)
        self.mounts = MeasurementMounts()
        self.host = MeasurementHost(self.canonical_parent)

    def tearDown(self):
        self.temp.cleanup()  # All trees are ordinary directories; mounts are fake.

    def prepare(self, backend="ext4", **kwargs):
        return runner.MeasurementTrial.prepare(
            self.parent, canonical_parent=self.canonical_parent, backend=backend,
            records=32, expected_device="259:7", processes=self.processes,
            mounts=self.mounts, host=self.host, clock=self.clock,
            daemon=Path(sys.executable).resolve(), build_provenance={"mocked": True}, **kwargs)

    def failed(self, trial, message):
        self.assertFalse(trial.execute())
        self.assertIn(message, trial.state["error"])
        self.assertTrue(trial.workspace.exists())
        self.assertTrue(trial.canonical_directory.exists())
        self.assertEqual(trial.state["retention"]["artifacts"], "retained")

    def test_ext4_phase_order_and_exact_application_boundaries(self):
        trial = self.prepare()
        self.assertTrue(trial.execute(), trial.state.get("error"))
        self.assertEqual([p["name"] for p in trial.state["phases"]], [
            "preflight", "canonical_preparation", "preparation_readiness",
            "input_staging_validation", "preparation_shutdown", "application_readiness",
            "application_execution", "application_completion_accounting", "daemon_shutdown",
            "verification_readiness", "correctness_verification", "verification_shutdown",
            "post_exit_storage_accounting"])
        wall = trial.state["measurements"]["application"]["wall_seconds"]
        self.assertEqual(wall["value"], 5.0)
        self.assertEqual(wall["evidence"], "mocked")
        entry = next(e for e in self.processes.entries if str(application.__file__) in e["command"])
        self.assertEqual(wall["interval"], [entry["launch_monotonic"], entry["exit_monotonic"]])
        self.assertEqual(self.processes.reaps[entry["identity"]["pid"]], 1)
        for name in ("preparation_shutdown", "daemon_shutdown", "verification_shutdown"):
            phase = next(p for p in trial.state["phases"] if p["name"] == name)
            self.assertEqual(phase["outcome"], "not_applicable")
            self.assertIsNone(phase["duration_seconds"])
        self.assertIsNone(trial.state["measurements"]["daemon"]["user_seconds"]["value"])
        self.assertFalse(trial.state["cache_history"]["equivalent_residency_proven"])

    def test_myfs_order_nonmutating_readiness_and_canonical_remount(self):
        trial = self.prepare("myfs")
        before = trial.canonical_snapshot()
        self.assertTrue(trial.execute(), trial.state.get("error"))
        self.assertEqual(self.mounts.events, ["mount", "stat-probe", "unmount"] * 3)
        self.assertEqual(before, trial.canonical_snapshot())
        self.assertTrue(trial.state["cache_history"]["fresh_fuse_connection"])
        self.assertEqual(trial.state["validations"][-1]["output"]["expected"],
                         runner.summary(trial.canonical_directory / "expected.csv"))
        self.assertEqual(len(trial.state["measurements"]["daemon_lifecycles"]), 3)

    def test_missing_endpoints_are_null_with_reason(self):
        self.processes.missing_endpoint = True
        trial = self.prepare("myfs")
        self.assertTrue(trial.execute(), trial.state.get("error"))
        endpoint = trial.state["counter_observations"][0]["counters"]
        self.assertIsNone(endpoint["value"])
        self.assertIn("missed", endpoint["reason"])

    def test_device_change_refused(self):
        trial = self.prepare()
        self.host.device_number = "259:8"
        self.failed(trial, "device")

    def test_source_change_refused(self):
        trial = self.prepare()
        with mock.patch.object(runner, "measurement_source", return_value={"revision": "changed"}):
            self.failed(trial, "source")

    def test_executable_identity_change_refused(self):
        trial = self.prepare("myfs")
        trial.state["daemon"]["sha256"] = "0" * 64
        self.failed(trial, "executable")

    def test_insufficient_disk_and_memory_refused(self):
        self.host.free = 1
        trial = self.prepare()
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("disk", trial.state["error"])
        self.host.free = 4 * 1024**3
        self.host.memory = 1
        trial = self.prepare()
        self.assertIn("memory", trial.state["error"])

    def test_canonical_corruption_refused(self):
        trial = self.prepare()
        (trial.canonical_directory / "input.csv").write_bytes(b"corrupt\n")
        self.failed(trial, "canonical")

    def test_staged_corruption_with_self_consistent_output_rejected(self):
        trial = self.prepare()
        original = trial.io
        def corrupt(kind):
            result = original(kind)
            if kind == "stage":
                with (trial.root / "input.csv").open("a") as stream:
                    records = fixture._csv_records(101, fixture.DEFAULT_SEED)
                    next(records)
                    stream.write(list(records)[-1].decode("ascii"))
            return result
        trial.io = corrupt
        self.failed(trial, "canonical validation")
        self.assertTrue((trial.root / "output.csv").exists())

    def test_application_error_timeout_and_validation_error(self):
        for option, message in (("application_error", "application exit"),
                                ("application_timeout", "timeout"),
                                ("corrupt_output", "canonical validation")):
            with self.subTest(option=option):
                setattr(self.processes, option, True)
                self.failed(self.prepare(), message)
                setattr(self.processes, option, False)

    def test_readiness_timeout_and_shutdown_failure_retain_evidence(self):
        self.mounts.visible = False
        self.failed(self.prepare("myfs", readiness_seconds=.1), "readiness timeout")
        self.mounts.visible = True
        self.mounts.current = []
        self.processes.shutdown_timeout = True
        self.failed(self.prepare("myfs"), "shutdown timeout")

    def test_unexplained_warning_invalidates_trial_verbatim(self):
        trial = self.prepare("myfs")
        original = self.processes.wait
        def warning(identity, deadline):
            value = original(identity, deadline)
            entry = next(e for e in self.processes.entries if e["identity"] == identity)
            if str(application.__file__) not in entry["command"]:
                with Path(entry["log"]).open("a") as stream:
                    stream.write("[WARN] unexplained background warning\n")
            return value
        self.processes.wait = warning
        self.failed(trial, "unexplained daemon warning")
        self.assertEqual(trial.state["warnings"][0]["line"], "[WARN] unexplained background warning\n")
        self.assertEqual(trial.state["warnings"][0]["trial_id"], trial.anchor["trial_id"])

    def test_post_exit_accounting_failure_retains_evidence(self):
        trial = self.prepare()
        with mock.patch.object(runner, "account_owned_tree", side_effect=runner.TrialError("accounting failure")):
            self.failed(trial, "accounting failure")
        self.assertIsNone(trial.state["storage"]["allocated_bytes"])

    def test_cleanup_refuses_nested_mount_and_unconfirmed_child(self):
        trial = self.prepare()
        self.assertTrue(trial.execute())
        self.mounts.current = [{"target": str(trial.root / "nested")}]
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.mounts.current = []
        trial.state["children"][0]["exit_status"] = None
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.assertTrue(trial.canonical_directory.exists())

    def test_offline_check_never_claims_measurement_backend(self):
        trial = runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.canonical_parent,
                                                backend="offline-check", records=10)
        self.assertTrue(trial.execute(), trial.state.get("error"))
        self.assertIn("OFFLINE", trial.state["label"])
        self.assertIsNone(trial.state["measurements"]["application"]["wall_seconds"]["value"])
        self.assertIn("offline", trial.state["measurements"]["application"]["wall_seconds"]["reason"])
        loaded = runner.Trial.load(trial.artifacts)
        self.assertTrue(loaded.verify())
        loaded.cleanup()
        self.assertTrue(trial.canonical_directory.exists())

    def test_hardlink_symlink_directory_accounting(self):
        root = self.parent / "owned-accounting"
        root.mkdir()
        (root / "a.data").write_bytes(b"varying data" * 100)
        (root / "nested").mkdir()
        (root / "nested" / "alias.data").hardlink_to(root / "a.data")
        (root / "outside").symlink_to("/etc/passwd")
        result = runner.account_owned_tree(root, runner.directory_identity(root), 10, self.clock, False)
        self.assertEqual(result["regular_apparent_bytes"], 1200)
        self.assertEqual(result["unique_regular_files"], 1)
        self.assertEqual(result["hardlink_aliases"], 1)
        self.assertEqual(result["symlinks"], 1)
        self.assertEqual(result["directories"], 2)
        expected = sum(p.lstat().st_blocks * 512 for p in (root, root / "a.data", root / "nested", root / "outside"))
        self.assertEqual(result["allocated_bytes"], expected)

    def test_accounting_identity_and_deadline_refusal(self):
        root = self.parent / "accounting"
        root.mkdir()
        identity = runner.directory_identity(root)
        with self.assertRaises(runner.TrialError):
            runner.account_owned_tree(root, dict(identity, inode=-1), 10, self.clock, False)
        with self.assertRaises(runner.TrialError):
            runner.account_owned_tree(root, identity, 0, self.clock, False)

    def test_actual_child_has_one_reaper_and_cached_exit(self):
        processes = runner.AccountingProcesses(runner.Clock())
        identity = processes.launch([sys.executable, "-B", "-c", "print('OFFLINE helper')"],
                                    self.parent / "accounted-helper.log", 4096)
        original = runner.os.wait4
        with mock.patch.object(runner.subprocess.Popen, "poll", side_effect=AssertionError("competing reaper")), \
                mock.patch.object(runner.os, "wait4", wraps=original) as reap:
            self.assertEqual(processes.wait(identity, processes.clock.now() + 5), 0)
            self.assertEqual(processes.poll(identity), 0)
            self.assertEqual(processes.wait(identity, processes.clock.now() + 5), 0)
            self.assertEqual(reap.call_count, 1)
        entry = processes.entries[0]
        self.assertGreaterEqual(entry["resources"]["user_seconds"]["value"], 0)
        self.assertEqual(entry["resources"]["peak_rss_bytes"]["source"], "wait4.ru_maxrss Linux KiB * 1024")

    def test_completion_at_deadline_is_invalid_even_with_zero_exit(self):
        trial = self.prepare(application_seconds=5)
        self.failed(trial, "timeout")

    def test_actual_counter_cpu_and_missing_endpoint_semantics(self):
        processes = runner.AccountingProcesses(runner.Clock())
        identity = processes.launch([sys.executable, "-B", "-c", "import time;time.sleep(.05)"],
                                    self.parent / "counter-helper.log", 4096)
        counters = processes.counters(identity)
        processes.wait(identity, processes.clock.now() + 5)
        self.assertIn("user_seconds", counters["value"])
        self.assertEqual(counters["value"]["user_seconds"]["unit"], "seconds")
        missed = processes.counters(identity)
        self.assertIsNone(missed["value"])
        self.assertIn("reaped", missed["reason"])

    def test_warning_is_retained_on_shutdown_timeout(self):
        trial = self.prepare("myfs")
        original = self.processes.launch
        def warning(*args):
            identity = original(*args)
            with Path(self.processes.entries[-1]["log"]).open("a") as stream:
                stream.write("[WARN] retained on failure\n")
            return identity
        self.processes.launch = warning
        self.processes.shutdown_timeout = True
        self.failed(trial, "shutdown timeout")
        self.assertEqual(trial.state["warnings"][0]["line"], "[WARN] retained on failure\n")

    def test_accounting_rejects_nested_mount_descriptor(self):
        root = self.parent / "nested-accounting"
        root.mkdir()
        (root / "child").mkdir()
        with mock.patch.object(runner, "fd_mount_id", side_effect=[1, 2]):
            with self.assertRaises(runner.TrialError):
                runner.account_owned_tree(root, runner.directory_identity(root), 10, self.clock, False)

    def test_canonical_symlink_substitution_refuses_cleanup(self):
        trial = self.prepare()
        self.assertTrue(trial.execute())
        saved = trial.canonical_directory.with_name(trial.canonical_directory.name + "-saved")
        trial.canonical_directory.rename(saved)
        trial.canonical_directory.symlink_to(saved, target_is_directory=True)
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.assertTrue(saved.exists())
        self.assertTrue(trial.workspace.exists())

    def test_unavailable_daemon_totals_have_reason(self):
        trial = self.prepare("myfs")
        self.assertTrue(trial.execute())
        value = trial.state["measurements"]["daemon"]["user_seconds"]
        self.assertIsNone(value["value"])
        self.assertTrue(value["reason"])

    def test_wait_services_other_owned_log_pipes(self):
        processes = runner.AccountingProcesses(runner.Clock())
        marker = self.parent / "helper-finished-writing"
        producer = processes.launch([sys.executable, "-B", "-c",
            "import sys,time;from pathlib import Path;sys.stdout.write('x'*200000);sys.stdout.flush();"
            "Path(sys.argv[1]).write_text('done');time.sleep(.05)", str(marker)],
            self.parent / "producer.log", 400000)
        consumer = processes.launch([sys.executable, "-B", "-c",
            "import sys,time;from pathlib import Path;p=Path(sys.argv[1]);end=time.monotonic()+.5;"
            "\nwhile not p.exists() and time.monotonic()<end: time.sleep(.005)"
            "\nsys.exit(0 if p.exists() else 9)", str(marker)], self.parent / "consumer.log", 4096)
        try:
            status = processes.wait(consumer, processes.clock.now() + 2)
        finally:
            if processes.poll(producer) is None:
                processes.signal_owned(producer, 15)
            processes.wait(producer, processes.clock.now() + 2)
        self.assertEqual(status, 0, "another owned PIPE must not block the workflow")

    def test_post_reap_bookkeeping_does_not_move_completion_endpoint(self):
        processes = runner.AccountingProcesses(self.clock)
        identity = processes.launch([sys.executable, "-B", "-c", "pass"], self.parent / "endpoint.log", 4096)
        def bookkeeping():
            if processes.entries[0]["exit_status"] is not None:
                self.clock.value = 10
        processes.on_change = bookkeeping
        self.assertEqual(processes.wait(identity, 1), 0)
        self.assertLess(processes.entries[0]["exit_monotonic"], 1)

    def test_storage_metrics_have_individual_units(self):
        trial = self.prepare()
        self.assertTrue(trial.execute())
        metrics = trial.state["storage"]["metrics"]
        self.assertEqual(metrics["allocated_bytes"]["unit"], "bytes")
        self.assertEqual(metrics["unique_regular_files"]["unit"], "count")
        self.assertEqual(metrics["logical_input_bytes"]["value"], 69 + 32 * 128)

    def test_application_affinity_uses_application_deadline_and_owned_signal(self):
        trial = self.prepare(application_affinity=[0], application_seconds=.02)
        observed = []
        def mismatch(identity, requested):
            observed.append(self.clock.now())
            return {"value": [1], "matches": False, "reason": None, "evidence": "mocked"}
        self.host.affinity = mismatch
        original = self.processes.signal_owned
        def terminate(identity, number):
            original(identity, number)
            entry = next(e for e in self.processes.entries if e["identity"] == identity)
            entry.update(exit_status=-15, exit_monotonic=self.clock.now())
        self.processes.signal_owned = terminate
        self.failed(trial, "affinity")
        self.assertTrue(self.processes.signals)
        entry = next(e for e in self.processes.entries if str(application.__file__) in e["command"])
        self.assertLessEqual(max(observed), entry["launch_monotonic"] + .02)

    def test_actual_measurement_stat_probe_does_not_read_or_mutate_input(self):
        root = self.parent / "probe-root"
        root.mkdir()
        (root / "input.csv").write_bytes(b"not even CSV; probe must not read this")
        before = (root / "input.csv").stat()
        processes = runner.AccountingProcesses(runner.Clock())
        result = runner.MeasurementMounts().probe(root, processes, processes.clock.now() + 5,
                                                  self.parent / "root-stat.log", 4096)
        after = (root / "input.csv").stat()
        self.assertEqual(result["exit_status"], 0)
        self.assertEqual([p.name for p in root.iterdir()], ["input.csv"])
        self.assertEqual((before.st_atime_ns, before.st_mtime_ns, before.st_size),
                         (after.st_atime_ns, after.st_mtime_ns, after.st_size))

    def test_build_provenance_mismatch_and_non_ext4_refused(self):
        with mock.patch.object(self.host, "device", side_effect=lambda path: {
                "device": "259:7", "fstype": "tmpfs", "evidence": "mocked"}):
            trial = self.prepare()
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("ext4", trial.state["error"])
        trial = runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.canonical_parent,
            backend="myfs", expected_device="259:7", records=1, host=self.host,
            processes=self.processes, mounts=self.mounts, clock=self.clock,
            daemon=Path(sys.executable).resolve(), build_provenance={"schema": "bad"})
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("provenance", trial.state["error"])

    def test_cleanup_does_not_trust_mocked_flag_in_state(self):
        trial = self.prepare()
        self.assertTrue(trial.execute())
        trial.processes.evidence = "actual"
        child = trial.state["children"][0]["identity"]
        with mock.patch.object(runner, "process_identity", return_value=child):
            with self.assertRaises(runner.TrialError):
                trial.cleanup()
        self.assertTrue(trial.workspace.exists())

    def test_daemon_affinity_shares_readiness_deadline(self):
        trial = self.prepare("myfs", readiness_seconds=.1, daemon_affinity=[0])
        original = self.mounts.probe
        def slow_probe(*args):
            self.clock.value += .08
            return original(*args)
        self.mounts.probe = slow_probe
        times = []
        def mismatch(identity, requested):
            times.append(self.clock.now())
            return {"value": [1], "matches": False, "evidence": "mocked", "reason": None}
        self.host.affinity = mismatch
        self.failed(trial, "affinity")
        launch = self.processes.entries[0]["launch_monotonic"]
        self.assertLessEqual(max(times), launch + .1)

    def test_materialization_refuses_hardlinks_outside_owned_tree(self):
        root = self.parent / "materialization"
        root.mkdir()
        outside = self.parent / "outside.data"
        outside.write_bytes(b"external user data")
        (root / "alias.data").hardlink_to(outside)
        with mock.patch.object(runner.os, "fsync") as flush:
            with self.assertRaises(runner.TrialError):
                runner.account_owned_tree(root, runner.directory_identity(root), 10, self.clock, True)
        flush.assert_not_called()

    def test_materialization_of_internal_hardlinks_is_deduplicated(self):
        root = self.parent / "materialization-local"
        root.mkdir()
        (root / "data").write_bytes(b"owned data")
        (root / "alias").hardlink_to(root / "data")
        flushed = []
        original = runner.os.fsync
        def flush(fd):
            flushed.append(runner.os.fstat(fd).st_ino)
            original(fd)
        with mock.patch.object(runner.os, "fsync", side_effect=flush):
            result = runner.account_owned_tree(root, runner.directory_identity(root), 10, self.clock, True)
        self.assertEqual(len(flushed), 2)
        self.assertEqual(result["unique_regular_files"], 1)

    def test_namespace_local_mount_ids_do_not_impersonate_device_changes(self):
        original = self.host.device
        namespace, mount_offset = ["mock-namespace-A"], [0]
        def device(path):
            value = original(path)
            return dict(value, mount_namespace=namespace[0], mount_id=value["mount_id"] + mount_offset[0])
        self.host.device = device
        trial = self.prepare()
        namespace[0], mount_offset[0] = "mock-namespace-B", 100
        self.assertTrue(trial.execute(), trial.state.get("error"))
        self.assertTrue(trial.state["device_observations"])
        # A different ID in the SAME namespace remains an identity failure.
        another = self.prepare()
        mount_offset[0] += 1
        self.failed(another, "identity")

    def test_namespace_change_does_not_allow_device_change(self):
        original = self.host.device
        namespace = ["mock-namespace-A"]
        self.host.device = lambda path: dict(original(path), mount_namespace=namespace[0])
        trial = self.prepare()
        namespace[0] = "mock-namespace-B"
        self.host.device_number = "259:8"
        self.failed(trial, "identity")

    def test_mount_id_change_in_an_already_observed_namespace_refused(self):
        original = self.host.device
        namespace, offset = ["A"], [0]
        self.host.device = lambda path: dict(original(path), mount_namespace=namespace[0],
                                             mount_id=original(path)["mount_id"] + offset[0])
        trial = self.prepare()
        namespace[0], offset[0] = "B", 100
        trial.check_device()
        offset[0] = 101
        with self.assertRaises(runner.TrialError):
            trial.check_device()

    def test_recycled_namespace_inode_across_observer_contexts(self):
        original = self.host.device
        context, offset = ["observer-A"], [0]
        self.host.device = lambda path: dict(original(path), mount_namespace="recycled-inode",
            observation_context=context[0], mount_id=original(path)["mount_id"] + offset[0])
        trial = self.prepare()
        context[0], offset[0] = "observer-B", 100
        trial.check_device()
        offset[0] = 101
        with self.assertRaises(runner.TrialError):
            trial.check_device()

    def test_measurement_fuse_unavailable_has_no_fallback(self):
        self.mounts.available = False
        trial = self.prepare("myfs")
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("unavailable", trial.state["error"])
        self.assertEqual(trial.state["backend"], "myfs")
        self.assertFalse(self.processes.entries)

    def test_stale_owned_daemon_cannot_authorize_cleanup_or_signal(self):
        trial = self.prepare("myfs")
        self.processes.stale = True
        self.failed(trial, "identity")
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.assertFalse(self.processes.signals)

    def test_unowned_artifact_path_and_parent_refused(self):
        with self.assertRaises(runner.TrialError):
            runner.Trial.load(self.parent)
        before = list(self.parent.iterdir())
        with mock.patch.object(runner, "directory_identity", side_effect=runner.TrialError("unowned parent")):
            with self.assertRaises(runner.TrialError):
                self.prepare()
        self.assertEqual(before, list(self.parent.iterdir()))

    def test_accounting_api_unavailable_refuses_before_launch(self):
        processes = runner.AccountingProcesses(runner.Clock())
        with mock.patch.object(runner.os, "wait4", new=None), \
                mock.patch.object(runner.subprocess, "Popen") as launch:
            # Simulate a missing OS attribute, not an operating-system change.
            with mock.patch.object(runner, "hasattr", create=True, side_effect=lambda obj, name: name != "wait4"):
                with self.assertRaises(runner.TrialError):
                    processes.launch(["MOCKED"], self.parent / "never-launched.log", 4096)
        launch.assert_not_called()


class SharedCanonicalTests(unittest.TestCase):
    """Real tiny canonical/oracle files; mount and trial measurements are fake."""
    setUp = MeasurementTests.setUp
    tearDown = MeasurementTests.tearDown

    def bundle(self, campaign_id="1" * 32, **kwargs):
        return runner.CanonicalBundle.prepare(self.canonical_parent,
            evidence_parent=self.parent, campaign_id=campaign_id, records=32,
            host=self.host, **kwargs)

    def trial(self, bundle, backend="ext4", **kwargs):
        return runner.MeasurementTrial.prepare(self.parent,
            canonical_bundle=bundle.root, campaign_id=bundle.state["campaign_id"],
            records=32, backend=backend, expected_device="259:7",
            daemon=Path(sys.executable).resolve(), build_provenance={"mocked": True},
            processes=MeasurementProcesses(self.clock), mounts=MeasurementMounts(),
            host=self.host, clock=self.clock, **kwargs)

    def test_two_independent_trials_reuse_one_bundle_without_generation(self):
        bundle = self.bundle()
        self.assertEqual(bundle.state["outcome"], "ready", bundle.state.get("error"))
        before = {p.name: p.read_bytes() for p in bundle.root.iterdir()}
        with mock.patch.object(fixture, "generate_csv", side_effect=AssertionError("reuse generated a fixture")):
            for backend in ("ext4", "myfs"):
                trial = self.trial(bundle, backend)
                self.assertEqual(trial.state["outcome"], "prepared", trial.state.get("error"))
                self.assertTrue(trial.execute(), trial.state.get("error"))
                self.assertEqual(trial.state["canonical"], bundle.state["canonical"])
                self.assertEqual(trial.state["validations"][-1]["output"]["actual"],
                                 trial.state["validations"][-1]["output"]["expected"])
                self.assertEqual(trial.state["canonical_reference"]["ownership"], "campaign")
                self.assertEqual(trial.state["evidence"], "mocked")
                self.assertTrue(trial.state["canonical_checks"])
                trial.cleanup()
                self.assertFalse(trial.workspace.exists())
                self.assertEqual(before, {p.name: p.read_bytes() for p in bundle.root.iterdir()})

    def test_seed_record_and_generator_mismatch_refused(self):
        bundle = self.bundle()
        for options in ({"seed": 1}, {"records": 33}):
            with self.subTest(options=options), self.assertRaises(runner.TrialError):
                runner.CanonicalBundle.load(bundle.root, campaign_id="1" * 32,
                    **dict({"seed": 20261010, "records": 32}, **options))
        with mock.patch.object(fixture, "CSV_GENERATOR", "foreign-generator"), self.assertRaises(runner.TrialError):
            runner.CanonicalBundle.load(bundle.root, campaign_id="1" * 32, records=32, seed=20261010)

    def test_corrupt_truncated_and_invalid_references_refused_without_repair(self):
        for index, name in enumerate(("input.csv", "expected.csv", "input.manifest.json")):
            with self.subTest(name=name):
                bundle = self.bundle(f"{index + 2:032x}")
                path = bundle.root / name
                path.write_bytes(b"corrupt\n")
                with self.assertRaises(runner.TrialError):
                    runner.CanonicalBundle.load(bundle.root, campaign_id=bundle.state["campaign_id"],
                                                records=32, seed=20261010)
                self.assertEqual(path.read_bytes(), b"corrupt\n")

    def test_symlink_substitution_refused(self):
        bundle = self.bundle()
        original = bundle.root / "expected.csv"
        target = self.parent / "unrelated.csv"
        target.write_bytes(original.read_bytes())
        original.unlink()
        original.symlink_to(target)
        with self.assertRaises(runner.TrialError):
            self.trial(bundle)
        self.assertTrue(target.exists())

    def test_file_identity_replacement_with_identical_bytes_refused(self):
        bundle = self.bundle()
        path = bundle.root / "input.csv"
        replacement = bundle.root / "replacement"
        replacement.write_bytes(path.read_bytes())
        replacement.replace(path)
        with self.assertRaises(runner.TrialError):
            self.trial(bundle)

    def test_directory_substitution_refused(self):
        bundle = self.bundle()
        trial = self.trial(bundle)
        moved = bundle.root.with_name("retained-original")
        bundle.root.rename(moved)
        bundle.root.mkdir(mode=0o700)
        self.assertFalse(trial.execute())
        self.assertTrue(trial.workspace.exists())
        self.assertTrue(moved.exists())
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_missing_tmpfs_bundle_requires_explicit_reconstruction(self):
        bundle = self.bundle()
        trial = self.trial(bundle)
        (bundle.root / "input.csv").unlink()
        self.assertFalse(trial.execute())
        self.assertFalse((bundle.root / "input.csv").exists())
        self.assertTrue(trial.artifacts.exists())

    def test_foreign_campaign_and_path_escape_refused(self):
        bundle = self.bundle()
        for root, campaign in ((bundle.root, "2" * 32),
                               (bundle.root, "../escape")):
            with self.subTest(campaign=campaign), self.assertRaises(runner.TrialError):
                runner.CanonicalBundle.load(root, campaign_id=campaign, records=32, seed=20261010)
        alias = self.parent / "bundle-alias"
        alias.symlink_to(bundle.root, target_is_directory=True)
        with self.assertRaises(runner.TrialError):
            runner.CanonicalBundle.load(alias, campaign_id="1" * 32, records=32, seed=20261010)

    def test_duplicate_and_in_progress_campaign_creation_preserves_existing_data(self):
        bundle = self.bundle()
        before = {p.name: p.read_bytes() for p in bundle.root.iterdir()}
        with self.assertRaises((runner.TrialError, FileExistsError)):
            self.bundle()
        self.assertEqual(before, {p.name: p.read_bytes() for p in bundle.root.iterdir()})
        pending = self.canonical_parent / ("m0-shared-canonical-" + "3" * 32)
        pending.mkdir(mode=0o700)
        sentinel = pending / "pending"
        sentinel.write_bytes(b"another preparer owns this")
        with self.assertRaises((runner.TrialError, FileExistsError)):
            self.bundle("3" * 32)
        self.assertEqual(sentinel.read_bytes(), b"another preparer owns this")

    def test_stale_receipt_or_foreign_state_refused(self):
        bundle = self.bundle()
        trial = self.trial(bundle)
        path = bundle.artifacts / "state.json"
        record = json.loads(path.read_text())
        record["campaign_id"] = "2" * 32
        path.write_text(json.dumps(record))
        self.assertFalse(trial.execute())
        self.assertTrue(trial.workspace.exists())
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_canonical_corruption_after_staging_invalidates_trial(self):
        bundle = self.bundle()
        trial = self.trial(bundle)
        original = trial.io
        def stage_then_corrupt(kind):
            value = original(kind)
            if kind == "stage":
                (bundle.root / "expected.csv").write_bytes(b"incorrect\n")
            return value
        trial.io = stage_then_corrupt
        self.assertFalse(trial.execute())
        self.assertTrue(trial.workspace.exists())
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_failed_application_keeps_shared_bundle_and_trial_evidence(self):
        bundle = self.bundle()
        trial = self.trial(bundle)
        trial.processes.application_error = True
        self.assertFalse(trial.execute())
        self.assertTrue(trial.artifacts.exists())
        self.assertTrue(bundle.root.exists())
        self.assertTrue(bundle.artifacts.exists())
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_reuse_reserves_no_new_canonical_copy_and_preserves_backing_headroom(self):
        bundle = self.bundle()
        self.host.free = 1024**3 + 64 * 1024**2 + 8 * (69 + 32 * 128)
        trial = self.trial(bundle)
        self.assertEqual(trial.state["outcome"], "prepared", trial.state.get("error"))
        plan = trial.state["capacity_plan"]
        self.assertEqual(plan["retained_headroom_bytes"], 1073741824)
        self.assertEqual(plan["canonical_required_bytes"], 1140850688)
        self.assertEqual(plan["canonical_new_allocation_estimate_bytes"], 0)
        self.assertGreater(plan["shared_canonical_allocated_bytes"], 0)
        self.assertEqual(plan["memory_required_bytes"], 2147483648)
        self.host.free = 1
        self.assertFalse(trial.execute())
        self.assertTrue(bundle.root.exists())

    def test_shared_reference_cannot_be_inside_trial_parent(self):
        bundle = self.bundle()
        with self.assertRaises(runner.TrialError):
            runner.MeasurementTrial.prepare(bundle.root, canonical_bundle=bundle.root,
                campaign_id="1" * 32, records=32, backend="ext4", expected_device="259:7")

    def test_bundle_receipt_retains_metadata_not_large_csv_copies(self):
        bundle = self.bundle()
        record = json.loads((bundle.artifacts / "state.json").read_text())
        self.assertEqual(record["outcome"], "ready")
        self.assertTrue(record["oracle_validation"]["passed"])
        self.assertEqual(record["fixture_manifest"]["generator_version"], "myfs-m0-csv-v1")
        self.assertFalse(list(bundle.artifacts.glob("*.csv")))
        self.assertIn("reboot", record["persistence"]["limitation"])

    def test_canonical_cli_and_shared_offline_cli_are_explicit(self):
        from contextlib import redirect_stdout
        from io import StringIO
        output = StringIO()
        with mock.patch.object(runner, "MeasurementHost", return_value=self.host), redirect_stdout(output):
            code = runner.main(["canonical-prepare", "--canonical-parent", str(self.canonical_parent),
                "--evidence-parent", str(self.parent), "--campaign-id", "1" * 32,
                "--records", "32", "--seed", "20261010"])
            self.assertEqual(code, 0)
            prepared = json.loads(output.getvalue())
            self.assertTrue(prepared["passed"])
            for _ in range(2):
                output.seek(0); output.truncate()
                self.assertEqual(runner.main(["measure-self-test", "--parent", str(self.parent),
                    "--canonical-bundle", prepared["bundle"], "--campaign-id", "1" * 32,
                    "--records", "32", "--seed", "20261010"]), 0)
                trial_record = json.loads(output.getvalue())
                self.assertIn("OFFLINE", trial_record["label"])
                state = json.loads((Path(trial_record["artifacts"]) / "state.json").read_text())
                self.assertIsNone(state["measurements"]["application"]["wall_seconds"]["value"])
                self.assertEqual(state["canonical_reference"]["ownership"], "campaign")
                self.assertEqual(runner.main(["cleanup", trial_record["artifacts"]]), 0)
            self.assertTrue(Path(prepared["bundle"], "expected.csv").exists())

    def test_corrupt_staged_input_with_consistent_output_cannot_redefine_reference(self):
        bundle = self.bundle()
        trial = self.trial(bundle)
        original = trial.processes.wait
        def corrupt_then_wait(identity, deadline):
            entry = next(c for c in trial.processes.entries if c["identity"] == identity)
            if str(application.__file__) in entry["command"]:
                path = trial.root / "input.csv"
                path.write_bytes(path.read_bytes().replace(b",ok,", b",er,"))
            return original(identity, deadline)
        trial.processes.wait = corrupt_then_wait
        self.assertFalse(trial.execute())
        self.assertFalse(trial.state["validations"][-1]["input"]["passed"])
        self.assertTrue(fixture.validate_output(trial.root / "input.csv", trial.root / "output.csv")["passed"])
        self.assertTrue(bundle.root.exists())

    def test_canonical_creation_capacity_failure_retains_unvalidated_evidence(self):
        self.host.memory = 1
        bundle = self.bundle()
        self.assertEqual(bundle.state["outcome"], "failed")
        self.assertFalse((bundle.root / "input.csv").exists())
        self.assertTrue((bundle.artifacts / "state.json").exists())
        with self.assertRaises(runner.TrialError):
            self.trial(bundle)

    def test_missing_capacity_remains_unavailable(self):
        self.host.resources = lambda path: {"available_disk_bytes": None,
            "available_memory_bytes": None, "evidence": "mocked", "reason": "test unavailable"}
        bundle = self.bundle()
        self.assertEqual(bundle.state["outcome"], "failed")
        self.assertIsNone(bundle.state["resource_observations"][0]["available_disk_bytes"])

    def test_full_size_capacity_model_reuse_does_not_allocate_in_test(self):
        with mock.patch.object(fixture, "generate_csv", side_effect=AssertionError("full generation forbidden")):
            plan = runner.measurement_capacity(134217797, canonical_tmpfs=True, same_device=False, shared=True)
            self.assertEqual(plan["backing_required_bytes"], 2214593064)
            self.assertEqual(plan["canonical_required_bytes"], 1140850688)
            self.assertEqual(plan["memory_required_bytes"], 2147483648)
            for count in (-1, 1048577, 2**200, True):
                with self.subTest(count=count), self.assertRaises(runner.TrialError):
                    runner.CanonicalBundle.prepare(self.canonical_parent, evidence_parent=self.parent, records=count)

    def test_stale_trial_cannot_adopt_explicit_reconstructed_campaign(self):
        bundle = self.bundle()
        trial = self.trial(bundle)
        bundle.root.rename(bundle.root.with_name("old-bundle"))
        bundle.artifacts.rename(bundle.artifacts.with_name("old-evidence"))
        replacement = self.bundle()
        self.assertEqual(replacement.state["outcome"], "ready")
        self.assertEqual(bundle.state["canonical"]["input.csv"]["sha256"],
                         replacement.state["canonical"]["input.csv"]["sha256"])
        self.assertFalse(trial.execute())
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_ancestor_and_receipt_symlinks_are_refused(self):
        bundle = self.bundle()
        trial = self.trial(bundle)
        receipt = bundle.artifacts / "state.json"
        original = self.parent / "retained-receipt.json"
        receipt.rename(original)
        receipt.symlink_to(original)
        self.assertFalse(trial.execute())
        self.assertTrue(original.exists())

    def test_canonical_deadline_failure_retains_partial_files(self):
        bundle = self.bundle(io_seconds=0.001)
        self.assertEqual(bundle.state["outcome"], "failed")
        self.assertTrue(bundle.root.exists())
        self.assertTrue((bundle.artifacts / "state.json").exists())

    def test_non_object_ownership_and_receipt_json_fail_closed(self):
        for index, name in enumerate(("owner.json", "state.json")):
            with self.subTest(name=name):
                bundle = self.bundle(f"{index + 4:032x}")
                path = (bundle.root if name == "owner.json" else bundle.artifacts) / name
                path.write_text("[]")
                with self.assertRaises(runner.TrialError):
                    runner.CanonicalBundle.load(bundle.root, campaign_id=bundle.state["campaign_id"],
                                                records=32, seed=20261010)

    def test_pinned_owner_mismatch_refused_before_following_foreign_receipt_path(self):
        bundle = self.bundle()
        reference = bundle.reference()
        owner = json.loads((bundle.root / "owner.json").read_text())
        owner["evidence_parent"] = str(self.parent / "foreign")
        (bundle.root / "owner.json").write_text(json.dumps(owner))
        with mock.patch.object(runner, "strict_path", wraps=runner.strict_path) as inspected:
            with self.assertRaises(runner.TrialError):
                runner.CanonicalBundle.load(bundle.root, campaign_id="1" * 32,
                    records=32, seed=20261010, expected=reference)
        self.assertFalse(any("foreign" in str(c.args[0]) for c in inspected.call_args_list))

    def test_unexpected_mount_over_shared_references_refused(self):
        bundle = self.bundle()
        mounts = mock.Mock()
        mounts.snapshot.return_value = [{"target": str(bundle.root / "nested")}]
        with self.assertRaises(runner.TrialError):
            runner.CanonicalBundle.load(bundle.root, campaign_id="1" * 32,
                                        records=32, seed=20261010, mounts=mounts)

    def test_shared_hard_link_alias_refused(self):
        import os
        bundle = self.bundle()
        os.link(bundle.root / "input.csv", self.parent / "alias.csv")
        with self.assertRaises(runner.TrialError):
            self.trial(bundle)

    def test_resource_read_error_retains_null_observation_and_reason(self):
        self.host.resources = mock.Mock(side_effect=OSError("test resource endpoint unavailable"))
        bundle = self.bundle()
        self.assertEqual(bundle.state["outcome"], "failed")
        observation = bundle.state["resource_observations"][0]
        self.assertIsNone(observation["available_disk_bytes"])
        self.assertIsNone(observation["available_memory_bytes"])
        self.assertIn("endpoint unavailable", observation["reason"])

    def test_boolean_seed_cannot_alias_integer_seed(self):
        bundle = self.bundle(seed=0)
        with self.assertRaises((runner.TrialError, ValueError)):
            runner.CanonicalBundle.load(bundle.root, campaign_id="1" * 32, records=32, seed=False)

    def test_real_measurement_requires_nonvolatile_campaign_receipt(self):
        bundle = self.bundle()
        self.assertEqual(bundle.state["persistence"]["small_receipt_filesystem"], "ext4")
        device = self.host.device
        self.host.device = lambda path: dict(device(path), fstype="tmpfs") if path == bundle.artifacts else device(path)
        trial = self.trial(bundle)
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("volatile", trial.state["error"])
        self.assertTrue(bundle.root.exists())
        self.assertTrue(trial.artifacts.exists())


class FullMeasurementTests(unittest.TestCase):
    """Full-size boundaries are mocked; actual fixture generation stays tiny."""
    setUp = MeasurementTests.setUp
    tearDown = MeasurementTests.tearDown
    prepare = MeasurementTests.prepare
    failed = MeasurementTests.failed

    def test_measurement_size_boundaries_without_generation(self):
        for records, size, profile in ((0, 69, "smoke-sized"),
                                       (32768, 4194373, "smoke-sized"),
                                       (1048576, 134217797, "larger-measurement")):
            with self.subTest(records=records), mock.patch.object(
                    fixture, "generate_csv", side_effect=AssertionError("no boundary fixture generation")):
                config = runner.measurement_limits(records)
                self.assertEqual(config["input_bytes"], size)
                self.assertEqual(config["size_class"], profile)

    def test_bad_counts_refused_before_creating_or_generating(self):
        before = list(self.parent.iterdir())
        for records in (-1, 1048577, 2**200, True, 1.5, "1048576", None):
            with self.subTest(records=records), mock.patch.object(
                    fixture, "generate_csv", side_effect=AssertionError("expensive generation reached")):
                with self.assertRaises(runner.TrialError):
                    runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.canonical_parent,
                        backend="ext4", expected_device="259:7", records=records)
        self.assertEqual(list(self.parent.iterdir()), before)

    def test_full_size_not_enabled_for_offline_self_test_or_smoke(self):
        with mock.patch.object(fixture, "generate_csv", side_effect=AssertionError("full generation reached")):
            for cls, backend, extra in ((runner.Trial, "offline-directory", {}),
                    (runner.MeasurementTrial, "offline-check", {"canonical_parent": self.canonical_parent})):
                with self.subTest(backend=backend), self.assertRaises(runner.TrialError):
                    cls.prepare(self.parent, backend=backend, records=1048576, **extra)

    def test_separate_full_size_deadlines_and_bounded_values(self):
        config = runner.measurement_limits(1048576, readiness_seconds=15, application_seconds=120,
            staging_seconds=120, verification_seconds=120, shutdown_seconds=120,
            io_seconds=120, overall_seconds=900)
        self.assertEqual(config["deadlines_seconds"], {"readiness": 15, "application": 120,
            "staging": 120, "verification": 120, "shutdown": 120, "io": 120})
        self.assertEqual(config["overall_seconds"], 900)
        for field in ("readiness", "application", "staging", "verification", "shutdown", "io", "overall"):
            for value in (0, -1, float("nan"), float("inf"), 901, True):
                with self.subTest(field=field, value=value), self.assertRaises(runner.TrialError):
                    runner.measurement_limits(1048576, **{field + "_seconds": value})

    def test_io_deadline_fallback_preserves_existing_measurement_contract(self):
        trial = self.prepare(io_seconds=18)
        self.assertEqual(trial.state["limits"]["deadlines_seconds"], {
            "readiness": 10, "application": 30, "shutdown": 30,
            "io": 18, "staging": 18, "verification": 18})
        self.assertTrue(trial.execute(), trial.state.get("error"))

    def test_full_size_capacity_model_has_separate_reservations(self):
        plan = runner.measurement_capacity(134217797, canonical_tmpfs=True, same_device=False)
        self.assertEqual(plan["backing_required_bytes"], 2214593064)
        self.assertEqual(plan["canonical_required_bytes"], 1409286282)
        self.assertEqual(plan["memory_required_bytes"], 2415919242)
        self.assertEqual(plan["retained_headroom_bytes"], 1073741824)
        self.assertIn("estimate", plan["limitation"])

    def test_insufficient_canonical_capacity_before_generation(self):
        original = self.host.resources
        def resources(path):
            result = original(path)
            if self.canonical_parent in path.parents:
                result["available_disk_bytes"] = 1
            return result
        self.host.resources = resources
        with mock.patch.object(fixture, "generate_csv", side_effect=AssertionError("allocation reached")):
            trial = self.prepare()
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("canonical", trial.state["error"])
        self.assertTrue(trial.artifacts.exists())

    def test_capacity_loss_between_preparation_and_execution(self):
        trial = self.prepare()
        self.host.free = 1
        self.failed(trial, "disk")
        self.assertFalse((trial.root / "input.csv").exists())
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_capacity_loss_after_staging_prevents_application_launch(self):
        trial = self.prepare()
        original = trial.io
        def stage_then_lose_capacity(kind):
            value = original(kind)
            if kind == "stage":
                self.host.free = 1
            return value
        trial.io = stage_then_lose_capacity
        self.failed(trial, "disk")
        self.assertFalse((trial.root / "output.csv").exists())

    def test_missing_capacity_observation_retained_as_null(self):
        trial = self.prepare()
        self.host.resources = lambda path: {"available_disk_bytes": None,
            "available_memory_bytes": None, "reason": "MOCKED endpoint unavailable", "evidence": "mocked"}
        self.failed(trial, "unavailable")
        observation = trial.state["resource_observations"][-1]
        self.assertIsNone(observation["available_disk_bytes"])
        self.assertIn("unavailable", observation["reason"])

    def test_evidence_budget_exhaustion_retains_sparse_evidence(self):
        trial = self.prepare()
        path = trial.artifacts / "oversized-evidence"
        with path.open("xb") as stream:
            stream.truncate(64 * 1024**2 + 1)  # sparse; no large data allocation
        self.failed(trial, "evidence budget")
        self.assertTrue(path.exists())
        self.assertGreater(trial.state["evidence_usage"]["value"], 64 * 1024**2)
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_overall_budget_expiry_retains_evidence(self):
        trial = self.prepare(overall_seconds=2)
        self.failed(trial, "timeout")
        self.assertFalse((trial.root / "output.csv").exists())
        self.assertTrue(trial.state["budget"]["events"])
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_prepare_and_run_share_budget_but_idle_time_is_excluded(self):
        trial = self.prepare(overall_seconds=30)
        consumed = trial.state["budget"]["consumed_seconds"]
        self.clock.value += 10000  # user review between CLI invocations is not active trial time
        self.assertTrue(trial.execute(), trial.state.get("error"))
        self.assertGreater(trial.state["budget"]["consumed_seconds"], consumed)
        self.assertLess(trial.state["budget"]["consumed_seconds"], 30)

    def test_separate_staging_and_verification_deadlines(self):
        for field, phase in (("staging", "input_staging_validation"),
                             ("verification", "correctness_verification")):
            with self.subTest(field=field):
                trial = self.prepare(**{field + "_seconds": 1})
                self.failed(trial, "timeout")
                self.assertEqual(trial.state["phases"][-1]["name"], phase)

    def test_full_prepare_validation_uses_owned_helper_without_large_generation(self):
        # Model only the size boundary. Stop deliberately before generating any data.
        def decline_generation(command, log, limit, deadline):
            self.assertIn("generate_csv", command[3])
            self.assertEqual(command[-2:], ["1048576", "20261010"])
            Path(log).write_text("MOCKED generation declined\n")
            raise runner.TrialError("MOCKED full-size generation deliberately not executed")
        self.processes.run = decline_generation
        with mock.patch.object(fixture, "generate_csv", side_effect=AssertionError("full allocation reached")):
            trial = runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.canonical_parent,
                backend="ext4", records=1048576, expected_device="259:7", host=self.host,
                processes=self.processes, mounts=self.mounts, clock=self.clock)
        self.assertEqual(trial.state["limits"]["records"], 1048576)
        self.assertIn("deliberately not executed", trial.state["error"])
        self.assertFalse((trial.canonical_directory / "input.csv").exists())

    def test_io_budget_includes_canonical_checks(self):
        trial = self.prepare(staging_seconds=1)
        original = trial.check_canonical
        def slow_reference_check():
            original()
            self.clock.value += 1.1
        trial.check_canonical = slow_reference_check
        # No staged data if reference validation already consumed the I/O budget.
        trial.phase("test_staging", lambda: None)
        with self.assertRaises(runner.TrialError):
            trial.io("stage")
        self.assertFalse((trial.root / "input.csv").exists())

    def test_tampered_large_count_refused_at_load(self):
        trial = self.prepare()
        trial.state["limits"]["records"] = 2**200
        trial.save()
        with self.assertRaises(runner.TrialError):
            runner.Trial.load(trial.artifacts, processes=self.processes,
                mounts=self.mounts, clock=self.clock, host=self.host)

    def test_invalid_persisted_budget_cannot_disable_overall_deadline(self):
        for consumed in (-1, float("nan"), float("inf")):
            with self.subTest(consumed=consumed):
                trial = self.prepare()
                trial.state["budget"]["consumed_seconds"] = consumed
                trial.save()
                with self.assertRaises(runner.TrialError):
                    runner.Trial.load(trial.artifacts, processes=self.processes,
                        mounts=self.mounts, clock=self.clock, host=self.host)

    def test_preparation_time_includes_source_capture_in_overall_budget(self):
        original = runner.measurement_source
        def slow_source():
            self.clock.value += 2
            return original()
        with mock.patch.object(runner, "measurement_source", side_effect=slow_source):
            trial = self.prepare(overall_seconds=1)
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("overall", trial.state["error"])
        self.assertFalse((trial.canonical_directory / "input.csv").exists())

    def test_exhausted_preparation_budget_survives_cli_reload(self):
        trial = self.prepare(overall_seconds=1)
        # Simulate a confirmed completed preparation consuming the entire budget.
        trial.state["budget"]["consumed_seconds"] = 1
        trial.save()
        loaded = runner.Trial.load(trial.artifacts, processes=self.processes,
            mounts=self.mounts, clock=self.clock, host=self.host)
        self.failed(loaded, "overall")
        self.assertFalse((loaded.root / "input.csv").exists())

    def test_final_bookkeeping_cannot_report_pass_after_overall_deadline(self):
        trial = self.prepare(overall_seconds=30)
        original = trial.save
        def late_final_save():
            original()
            if trial.state["outcome"] == "passed":
                self.clock.value += 30
        trial.save = late_final_save
        self.failed(trial, "overall")

    def test_deadline_maxima_are_inclusive_and_next_value_is_rejected(self):
        runner.measurement_limits(1048576, readiness_seconds=300, application_seconds=300,
            staging_seconds=300, verification_seconds=300, shutdown_seconds=300,
            io_seconds=300, overall_seconds=900)
        for field in ("readiness", "application", "staging", "verification", "shutdown", "io"):
            with self.subTest(field=field), self.assertRaises(runner.TrialError):
                runner.measurement_limits(1048576, **{field + "_seconds": 301})

    def test_evidence_scan_failure_remains_unavailable_and_preserves_artifacts(self):
        trial = self.prepare()
        with mock.patch.object(runner, "account_owned_tree", side_effect=runner.TrialError("MOCKED scan unavailable")):
            self.failed(trial, "scan unavailable")
        self.assertIsNone(trial.state["evidence_usage"]["value"])
        self.assertIn("unavailable", trial.state["evidence_usage"]["reason"])
        self.assertTrue(trial.artifacts.exists())

    def test_32768_records_still_prepare_and_verify_offline(self):
        trial = runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.canonical_parent,
            backend="ext4", records=32768, expected_device="259:7", host=self.host,
            processes=self.processes, mounts=self.mounts, clock=self.clock)
        self.assertEqual(trial.state["outcome"], "prepared", trial.state.get("error"))
        self.assertEqual(trial.state["logical"]["input_bytes"], 4194373)
        self.assertTrue(trial.execute(), trial.state.get("error"))
        self.assertEqual(trial.state["validations"][-1]["output"]["actual"]["row_count"], 10773)
        self.assertEqual(trial.state["validations"][-1]["output"]["actual"]["sha256"],
            "c804dfef2dd1c1d5ed64a9f4768bfd816e843372c8ec3801b044249c7a038734")

    def test_full_size_memory_guard_before_generation(self):
        self.host.memory = 2 * 1024**3
        with mock.patch.object(fixture, "generate_csv", side_effect=AssertionError("full allocation reached")):
            trial = runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.canonical_parent,
                backend="ext4", records=1048576, expected_device="259:7", host=self.host,
                processes=self.processes, mounts=self.mounts, clock=self.clock)
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("memory", trial.state["error"])

    def test_full_size_disk_guard_before_generation(self):
        self.host.free = 2214593063  # full backing reservation minus one byte
        with mock.patch.object(fixture, "generate_csv", side_effect=AssertionError("full allocation reached")):
            trial = runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.canonical_parent,
                backend="ext4", records=1048576, expected_device="259:7", host=self.host,
                processes=self.processes, mounts=self.mounts, clock=self.clock)
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("backing", trial.state["error"])

    def test_separate_device_canonical_reservation_and_same_device_offline_combination(self):
        # Unlike backing, canonical storage need not reserve eight copies.
        original = self.host.resources
        def resources(path):
            values = original(path)
            if self.canonical_parent in path.parents:
                values["available_disk_bytes"] = 1140859018  # 1 GiB + 64 MiB + 2*(69+32*128)
            return values
        self.host.resources = resources
        trial = self.prepare()
        self.assertEqual(trial.state["outcome"], "prepared", trial.state.get("error"))
        plan = runner.measurement_capacity(134217797, canonical_tmpfs=False, same_device=True)
        self.assertEqual(plan["backing_required_bytes"], 2483028658)

    def test_readiness_and_application_defaults_unchanged(self):
        trial = self.prepare()
        self.assertEqual(trial.state["limits"]["deadlines_seconds"]["application"], 30)
        self.assertEqual(trial.state["limits"]["deadlines_seconds"]["readiness"], 10)
        self.assertEqual(runner.measurement_limits(32768)["input_bytes"], 4194373)

    def test_preparation_helper_deadline_retains_partial_canonical(self):
        original = self.processes.run
        def late_generation(command, log, limit, deadline):
            value = original(command, log, limit, deadline)
            self.clock.value = deadline
            return value
        self.processes.run = late_generation
        trial = self.prepare(io_seconds=1)
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertIn("timeout", trial.state["error"])
        self.assertTrue((trial.canonical_directory / "input.csv").exists())
        self.assertEqual(trial.state["canonical"], {})

    def test_deadline_cli_options_only_on_measurement(self):
        import contextlib
        import io
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            runner.main(["self-test", "--parent", str(self.parent), "--staging-seconds", "120"])
        self.assertEqual(error.exception.code, 2)
        with contextlib.redirect_stdout(io.StringIO()):
            code = runner.main(["measure-self-test", "--parent", str(self.parent),
                "--canonical-parent", str(self.canonical_parent), "--records", "2",
                "--staging-seconds", "120", "--verification-seconds", "120", "--overall-seconds", "900"])
        self.assertEqual(code, 0)


class PythonProfileTests(unittest.TestCase):
    """Real tiny offline profiles; no mounts, large fixtures or timing comparisons."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="m0-d6b-tests-")
        self.parent = Path(self.temp.name)
        self.references = self.parent / "references"
        self.references.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def prepare(self, diagnostic="python-cpu", **kwargs):
        return runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.references,
            backend="offline-check", records=12, diagnostic=diagnostic, **kwargs)

    def profile_paths(self, trial):
        prefix = "python-cpu-" + trial.anchor["trial_id"]
        return trial.artifacts / (prefix + ".prof"), trial.artifacts / (prefix + ".json")

    def passed(self, trial):
        self.assertTrue(trial.execute(), trial.state["error"])
        value = trial.state["diagnostic"]["profile"]
        self.assertTrue(value["valid"], value.get("reason"))
        self.assertGreater(value["function_count"], 0)
        self.assertGreater(value["total_calls"], 0)
        return value

    def test_default_application_command_and_offline_wall_unchanged(self):
        trial = self.prepare(diagnostic=None)
        self.assertTrue(trial.execute(), trial.state["error"])
        child = next(c for c in trial.state["children"] if str(application.__file__) in c["command"])
        self.assertEqual(child["command"], [sys.executable, "-B", str(application.__file__), str(trial.root)])
        self.assertIsNone(trial.state["measurements"]["application"]["wall_seconds"]["value"])
        self.assertEqual(list(trial.artifacts.glob("python-cpu-*")), [])

    def test_real_profile_is_cpu_timed_readable_and_exact_output(self):
        trial = self.prepare()
        value = self.passed(trial)
        raw, receipt = self.profile_paths(trial)
        stats = pstats.Stats(str(raw))
        self.assertEqual(value["total_calls"], stats.total_calls)
        self.assertEqual(value["primitive_calls"], stats.prim_calls)
        self.assertEqual(value["function_count"], len(stats.stats))
        self.assertEqual(value["sha256"], hashlib.sha256(raw.read_bytes()).hexdigest())
        self.assertEqual(value["byte_count"], raw.stat().st_size)
        self.assertEqual(value["timer"], "time.process_time")
        self.assertEqual(value["unit"], "CPU seconds (user + system)")
        self.assertEqual(value["boundary_clock"], "time.monotonic")
        self.assertEqual(value["boundary_unit"], "seconds")
        self.assertTrue(any(k[0] == str(application.__file__) and k[2] == "transform" for k in stats.stats))
        self.assertEqual((trial.root / "output.csv").read_bytes(),
                         (trial.canonical_directory / "expected.csv").read_bytes())
        self.assertEqual(value["evidence"], "actual offline execution")
        self.assertIsNone(value["script_error"])
        self.assertEqual(receipt.parent, trial.artifacts)
        self.assertEqual(trial.state["measurement_population"], "instrumented_application_completion")
        self.assertFalse(trial.state["baseline_eligible"])
        self.assertIsNone(trial.state["measurements"]["application"]["wall_seconds"]["value"])

    def test_boundaries_and_owned_child_accounting(self):
        trial = self.prepare()
        value = self.passed(trial)
        child = next(c for c in trial.state["children"] if c["identity"] == trial.state["application_identity"])
        boundaries = value["boundaries"]
        ordered = [child["launch_monotonic"], *[boundaries[k] for k in
                   ("launcher_start", "profiler_enable", "profiler_disable", "serialization_start", "serialization_end")],
                   child["exit_monotonic"]]
        self.assertEqual(ordered, sorted(ordered))
        self.assertEqual(value["child_completion_monotonic"], child["exit_monotonic"])
        self.assertEqual(child["reap_count"], 1)
        self.assertEqual(child["exit_status"], 0)
        self.assertEqual(value["process_identity"], child["identity"])
        resources = trial.state["measurements"]["application"]
        self.assertEqual(resources["user_seconds"], child["resources"]["user_seconds"])
        self.assertEqual(resources["wall_seconds"]["population"]["completion"],
                         "instrumented_application_completion")

    def test_diagnostic_configuration_immutable_across_reload_and_runtime(self):
        trial = self.prepare()
        loaded = runner.Trial.load(trial.artifacts)
        self.assertEqual(loaded.state["diagnostic"]["configuration"]["mode"], "python-cpu")
        loaded.state["diagnostic"]["configuration"]["mode"] = None
        self.assertFalse(loaded.execute())
        self.assertIn("diagnostic", loaded.state["error"])
        trial = self.prepare()
        state = json.loads((trial.artifacts / "state.json").read_text())
        state["diagnostic"]["configuration"]["profile_max_bytes"] += 1
        (trial.artifacts / "state.json").write_text(json.dumps(state))
        with self.assertRaises(runner.TrialError):
            runner.Trial.load(trial.artifacts)

    def test_invalid_modes_rejected_before_workspace_allocation(self):
        before = set(self.parent.iterdir())
        for mode in ("wall", "perf", "", True, {}, 1):
            with self.subTest(mode=mode), self.assertRaises(runner.TrialError):
                self.prepare(diagnostic=mode)
        self.assertEqual(set(self.parent.iterdir()), before)

    def test_cli_opt_in_only_on_measurement_preparation(self):
        with contextlib.redirect_stderr(io.StringIO()):
            for command in ("prepare", "self-test", "measure-self-test"):
                with self.subTest(command=command), self.assertRaises(SystemExit) as e:
                    runner.main([command, "--parent", str(self.parent), "--diagnostic", "python-cpu"])
                self.assertEqual(e.exception.code, 2)
        clock = FakeClock()
        processes, mounts, host = MeasurementProcesses(clock), MeasurementMounts(), MeasurementHost(self.references)
        with mock.patch.object(runner, "AccountingProcesses", return_value=processes), \
                mock.patch.object(runner, "MeasurementMounts", return_value=mounts), \
                mock.patch.object(runner, "MeasurementHost", return_value=host), \
                mock.patch.object(runner, "Clock", return_value=clock), contextlib.redirect_stdout(io.StringIO()) as out:
            result = runner.main(["measure-prepare", "--parent", str(self.parent),
                "--canonical-parent", str(self.references), "--backend", "ext4", "--expected-device", "259:7",
                "--records", "2", "--diagnostic", "python-cpu"])
        self.assertEqual(result, 0)
        state = json.loads((Path(json.loads(out.getvalue())["artifacts"]) / "state.json").read_text())
        self.assertEqual(state["diagnostic"]["configuration"]["mode"], "python-cpu")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as e:
            runner.main(["measure-prepare", "--parent", str(self.parent), "--canonical-parent", str(self.references),
                         "--backend", "ext4", "--expected-device", "259:7", "--diagnostic", "wall"])
        self.assertEqual(e.exception.code, 2)

    def test_profile_overwrite_and_symlink_refused_without_touching_target(self):
        for which in (0, 1):
            for symlink in (False, True):
                with self.subTest(which=which, symlink=symlink):
                    trial = self.prepare()
                    target = trial.artifacts / "unrelated-preserve"
                    target.write_bytes(b"preserve")
                    path = self.profile_paths(trial)[which]
                    if symlink:
                        path.symlink_to(target)
                    else:
                        path.write_bytes(b"preserve")
                    self.assertFalse(trial.execute())
                    self.assertEqual(target.read_bytes(), b"preserve")
                    self.assertEqual(path.read_bytes(), b"preserve")
                    self.assertFalse((trial.root / "output.csv").exists())
                    self.assertTrue(trial.workspace.exists())
                    self.assertFalse(trial.state["diagnostic"]["profile"]["valid"])

    def test_missing_corrupt_partial_oversized_and_identity_substitution_refused(self):
        mutations = ("missing", "corrupt", "partial", "oversized", "same_bytes_new_inode", "hardlink")
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                trial = self.prepare()
                self.passed(trial)
                raw, _ = self.profile_paths(trial)
                if mutation == "missing":
                    raw.unlink()
                elif mutation == "corrupt":
                    raw.write_bytes(b"not a marshal profile")
                elif mutation == "partial":
                    raw.write_bytes(raw.read_bytes()[:12])
                elif mutation == "oversized":
                    with raw.open("ab") as f:
                        f.truncate(16 * 1024**2 + 1)  # Sparse file; no large allocation.
                elif mutation == "same_bytes_new_inode":
                    old = raw.with_suffix(".saved")
                    raw.rename(old)
                    raw.write_bytes(old.read_bytes())
                else:
                    raw.with_suffix(".alias").hardlink_to(raw)
                self.assertFalse(trial.verify())
                self.assertFalse(trial.state["diagnostic"]["profile"]["valid"])
                with self.assertRaises(runner.TrialError):
                    trial.cleanup()
                self.assertTrue(trial.workspace.exists())

    def test_invalid_receipt_timer_identity_and_boundaries_refused(self):
        for field in ("timer", "process_identity", "boundaries", "argv", "sys_path_0", "cwd"):
            with self.subTest(field=field):
                trial = self.prepare()
                original = trial.complete_application
                def tamper():
                    _, receipt = self.profile_paths(trial)
                    value = json.loads(receipt.read_text())
                    if field == "timer":
                        value[field] = "time.perf_counter"
                    elif field == "process_identity":
                        value[field]["start_ticks"] = "stale"
                    elif field == "boundaries":
                        value[field]["profiler_disable"] = value[field]["profiler_enable"] - 1
                    elif field == "argv":
                        value[field][-1] = "/unrelated-root"
                    else:
                        value[field] = "/unrelated-modules"
                    receipt.write_text(json.dumps(value))
                    return original()
                trial.complete_application = tamper
                self.assertFalse(trial.execute())
                self.assertFalse(trial.state["diagnostic"]["profile"]["valid"])

    def test_successful_cleanup_retains_profile_and_canonical(self):
        trial = self.prepare()
        value = self.passed(trial)
        before = trial.canonical_snapshot()
        raw, receipt = self.profile_paths(trial)
        runner.Trial.load(trial.artifacts).cleanup()
        self.assertFalse(trial.workspace.exists())
        self.assertTrue(raw.is_file() and receipt.is_file())
        self.assertEqual(hashlib.sha256(raw.read_bytes()).hexdigest(), value["sha256"])
        self.assertEqual(trial.canonical_snapshot(), before)

    def test_existing_output_failure_and_nonzero_exit_retain_profile(self):
        trial = self.prepare()
        original = trial.io
        def existing(kind):
            value = original(kind)
            if kind == "stage":
                (trial.root / "output.csv").write_bytes(b"preserve existing output")
            return value
        trial.io = existing
        self.assertFalse(trial.execute())
        self.assertIn("application exit", trial.state["error"])
        self.assertEqual((trial.root / "output.csv").read_bytes(), b"preserve existing output")
        self.assertTrue(self.profile_paths(trial)[0].is_file())
        self.assertEqual(trial.state["measurements"]["application"]["exit_status"]["value"], 1)
        self.assertEqual(trial.state["diagnostic"]["profile"]["script_exit_status"], 1)

    def launcher(self, text):
        """Owned tiny __main__ fixture for launcher contracts, never an app replacement in a trial."""
        trial = self.prepare()
        script = self.parent / ("launcher-case-" + trial.anchor["trial_id"] + ".py")
        script.write_text(text, encoding="ascii")
        anchor = json.loads(json.dumps(trial.anchor))
        anchor["diagnostic_application"] = {"path": str(script), "file": runner.profile_application_file(script)}
        (trial.artifacts / "owner.json").write_text(json.dumps(anchor))
        return trial, anchor, script

    def invoke_launcher(self, trial, anchor, script):
        return runner.python_cpu_profile(trial.artifacts, anchor, "actual offline execution", script, trial.root)

    def test_launcher_preserves_main_argv_file_cwd_and_sibling_import(self):
        (self.parent / "sibling.py").write_text("VALUE = 17\n")
        trial, anchor, script = self.launcher(
            "import json,sys,os,sibling\n"
            "print(json.dumps([__name__,__file__,sys.argv,os.getcwd(),sys.path[0],sibling.VALUE]))\n")
        argv, path, cwd = sys.argv, sys.path[:], os.getcwd()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.invoke_launcher(trial, anchor, script)
        self.assertEqual(json.loads(out.getvalue()), ["__main__", str(script), [str(script), str(trial.root)],
                                                    cwd, str(script.parent), 17])
        self.assertIs(sys.argv, argv)
        self.assertEqual(sys.path, path)
        self.assertEqual(os.getcwd(), cwd)

    def test_cpu_timer_and_disabled_serialization_are_real(self):
        trial, anchor, script = self.launcher("sum(range(500))\n")
        constructor, dump = runner.cProfile.Profile, runner.marshal.dump
        timers, enabled_at_write = [], []
        def construct(*args, **kwargs):
            timers.append(kwargs.get("timer"))
            return constructor(*args, **kwargs)
        def serialize(*args):
            enabled_at_write.append(sys.getprofile())
            return dump(*args)
        with mock.patch.object(runner.cProfile, "Profile", side_effect=construct), \
                mock.patch.object(runner.marshal, "dump", side_effect=serialize):
            self.invoke_launcher(trial, anchor, script)
        self.assertEqual(timers, [runner.time.process_time])
        self.assertEqual(enabled_at_write, [None])
        self.assertGreater(pstats.Stats(str(self.profile_paths(trial)[0])).total_calls, 0)

    def test_exception_and_systemexit_preserve_profile_and_exit(self):
        for text, exception, code in (("raise RuntimeError('literal failure')\n", RuntimeError, 1),
                                      ("raise SystemExit(7)\n", SystemExit, 7),
                                      ("raise SystemExit(0)\n", SystemExit, 0)):
            with self.subTest(code=code):
                trial, anchor, script = self.launcher(text)
                with self.assertRaises(exception) as e:
                    self.invoke_launcher(trial, anchor, script)
                if exception is SystemExit:
                    self.assertEqual(e.exception.code, code)
                raw, receipt = self.profile_paths(trial)
                value = json.loads(receipt.read_text())
                self.assertTrue(value["valid"])
                self.assertEqual(value["script_exit_status"], code)
                self.assertGreater(pstats.Stats(str(raw)).total_calls, 0)
                self.assertLessEqual(value["boundaries"]["profiler_disable"], value["boundaries"]["serialization_start"])

    def test_serialization_error_invalidates_trial_with_correct_output(self):
        trial = self.prepare()
        code = runner.PYTHON_PROFILE_CODE.replace("m.python_cpu_profile(",
            "m.marshal.dump=lambda *a: (_ for _ in ()).throw(OSError('injected serialization failure')); m.python_cpu_profile(")
        with mock.patch.object(runner, "PYTHON_PROFILE_CODE", code):
            self.assertFalse(trial.execute())
        self.assertEqual((trial.root / "output.csv").read_bytes(), (trial.canonical_directory / "expected.csv").read_bytes())
        raw, receipt = self.profile_paths(trial)
        value = json.loads(receipt.read_text())
        self.assertFalse(value["valid"])
        self.assertIn("injected serialization", value["serialization_error"])
        self.assertEqual(value["byte_count"], raw.stat().st_size)
        self.assertEqual(value["sha256"], hashlib.sha256(raw.read_bytes()).hexdigest())
        self.assertTrue(raw.exists())
        self.assertFalse(trial.state["diagnostic"]["profile"]["valid"])
        self.assertEqual(trial.state["measurements"]["application"]["exit_status"]["value"], 1)
        with self.assertRaises(runner.TrialError):
            trial.cleanup()

    def test_write_time_bound_retains_partial_and_does_not_exceed_limit(self):
        path = self.parent / "bounded.prof"
        parent = os.open(self.parent, os.O_RDONLY | os.O_DIRECTORY)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            writer = runner.BoundedProfileWriter(fd, parent, path.name, 8)
            writer.write(b"12345")
            with self.assertRaises(runner.TrialError):
                writer.write(b"6789")
            self.assertEqual(path.read_bytes(), b"12345")
            self.assertLessEqual(path.stat().st_size, 8)
        finally:
            os.close(fd)
            os.close(parent)

    def test_missing_profile_cannot_pass_even_with_exact_output(self):
        for which in (0, 1):
            with self.subTest(which=which):
                trial = self.prepare()
                original = trial.complete_application
                def missing():
                    self.profile_paths(trial)[which].unlink()
                    return original()
                trial.complete_application = missing
                self.assertFalse(trial.execute())
                self.assertFalse(trial.state["diagnostic"]["profile"]["valid"])
                self.assertEqual((trial.root / "output.csv").read_bytes(), (trial.canonical_directory / "expected.csv").read_bytes())

    def test_nonobject_boundary_receipts_fail_closed_and_retain_evidence(self):
        for malformed in (None, [], "invalid"):
            for after_pass in (False, True):
                with self.subTest(malformed=malformed, after_pass=after_pass):
                    trial = self.prepare()
                    def corrupt():
                        _, receipt = self.profile_paths(trial)
                        value = json.loads(receipt.read_text())
                        value["boundaries"] = malformed
                        receipt.write_text(json.dumps(value))
                    if after_pass:
                        self.passed(trial)
                        corrupt()
                        result = trial.verify()
                    else:
                        original = trial.complete_application
                        def corrupted_completion():
                            corrupt()
                            return original()
                        trial.complete_application = corrupted_completion
                        result = trial.execute()
                    self.assertFalse(result)
                    self.assertEqual(trial.state["outcome"], "failed")
                    self.assertFalse(trial.state["diagnostic"]["profile"]["valid"])
                    self.assertIn("diagnostic", trial.state["error"])
                    self.assertTrue(trial.artifacts.exists() and trial.workspace.exists())
                    with self.assertRaises(runner.TrialError):
                        trial.cleanup()

    def test_recursive_json_receipt_failure_invalidates_trial(self):
        trial = self.prepare()
        original = trial.complete_application
        def recursive_receipt():
            raw_loads = runner.json.loads
            def loads(value, *args, **kwargs):
                # Leave owner/state/canonical decoding real; inject only the receipt failure.
                if isinstance(value, bytes) and b'"profiler_enable"' in value:
                    raise RecursionError("injected recursive diagnostic JSON")
                return raw_loads(value, *args, **kwargs)
            with mock.patch.object(runner.json, "loads", side_effect=loads):
                return original()
        trial.complete_application = recursive_receipt
        self.assertFalse(trial.execute())
        self.assertEqual(trial.state["outcome"], "failed")
        self.assertFalse(trial.state["diagnostic"]["profile"]["valid"])
        self.assertIn("recursive diagnostic JSON", trial.state["error"])
        self.assertTrue(trial.workspace.exists())

    def test_timeout_uses_single_reaper_retains_evidence_and_blocks_cleanup(self):
        trial = self.prepare(application_seconds=.3)
        code = runner.PYTHON_PROFILE_CODE.replace("m.python_cpu_profile(",
                                                "import time; time.sleep(10); m.python_cpu_profile(")
        with mock.patch.object(runner, "PYTHON_PROFILE_CODE", code):
            self.assertFalse(trial.execute())
        self.assertIn("timeout", trial.state["error"])
        child = next(c for c in trial.state["children"] if c["identity"] == trial.state["application_identity"])
        self.assertEqual(child["reap_count"], 1)
        self.assertTrue(child["signals"])
        self.assertEqual(child["resources"]["user_seconds"]["population"]["owned_child"], child["identity"])
        with self.assertRaises(runner.TrialError):
            trial.cleanup()
        self.assertTrue(trial.workspace.exists() and trial.artifacts.exists())

    def test_artifact_directory_substitution_and_source_identity_refused(self):
        trial = self.prepare()
        anchor = json.loads(json.dumps(trial.anchor))
        anchor["identities"]["artifacts"]["inode"] += 1
        with self.assertRaises(runner.TrialError):
            self.invoke_launcher(trial, anchor, Path(application.__file__))
        anchor = json.loads(json.dumps(trial.anchor))
        anchor["diagnostic_application"]["file"]["sha256"] = "0" * 64
        with self.assertRaises(runner.TrialError):
            self.invoke_launcher(trial, anchor, Path(application.__file__))
        self.assertEqual(list(trial.artifacts.glob("python-cpu-*")), [])

    def test_legacy_unprofiled_state_loads_without_diagnostic_migration(self):
        trial = self.prepare(diagnostic=None)
        trial.anchor.pop("diagnostic")
        trial.state.pop("diagnostic")
        trial.state.pop("measurement_population")
        trial.state.pop("baseline_eligible")
        for p in (trial.artifacts, trial.workspace, trial.canonical_directory):
            (p / "owner.json").write_text(json.dumps(trial.anchor))
        trial.save()
        loaded = runner.Trial.load(trial.artifacts)
        self.assertTrue(loaded.execute(), loaded.state["error"])
        self.assertNotIn("diagnostic", loaded.state)

    def test_population_and_profile_state_corruption_refused_on_reload(self):
        for field in ("population", "profile"):
            with self.subTest(field=field):
                trial = self.prepare(diagnostic=None)
                state = json.loads((trial.artifacts / "state.json").read_text())
                if field == "population":
                    state["measurement_population"] = "arbitrary population"
                else:
                    state["diagnostic"]["profile"] = None
                (trial.artifacts / "state.json").write_text(json.dumps(state))
                with self.assertRaises(runner.TrialError):
                    runner.Trial.load(trial.artifacts)

    def test_explicit_null_diagnostic_anchor_is_not_legacy_absence(self):
        trial = self.prepare(diagnostic=None)
        trial.anchor["diagnostic"] = None
        trial.state.pop("diagnostic")
        for root in (trial.artifacts, trial.workspace, trial.canonical_directory):
            (root / "owner.json").write_text(json.dumps(trial.anchor))
        trial.save()
        with self.assertRaises(runner.TrialError):
            runner.Trial.load(trial.artifacts)

    def test_post_serialization_inode_substitution_invalidates_profile(self):
        trial, anchor, script = self.launcher("sum(range(100))\n")
        dump = runner.marshal.dump
        raw, receipt = self.profile_paths(trial)
        def substitute(*args):
            dump(*args)
            saved = raw.with_suffix(".saved")
            raw.rename(saved)
            fd = os.open(raw, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                os.write(fd, saved.read_bytes())
            finally:
                os.close(fd)
        with mock.patch.object(runner.marshal, "dump", side_effect=substitute):
            with self.assertRaises(runner.TrialError):
                self.invoke_launcher(trial, anchor, script)
        self.assertFalse(json.loads(receipt.read_text())["valid"])
        self.assertTrue(raw.exists() and raw.with_suffix(".saved").exists())

    def test_profiled_shared_trial_cleanup_preserves_campaign_references(self):
        bundle = runner.CanonicalBundle.prepare(self.references, evidence_parent=self.parent,
                                                records=12, seed=20261010)
        self.assertEqual(bundle.state["outcome"], "ready", bundle.state["error"])
        before = bundle.snapshot()
        trial = runner.MeasurementTrial.prepare(self.parent, canonical_bundle=bundle.root,
            campaign_id=bundle.owner["campaign_id"], backend="offline-check", records=12, diagnostic="python-cpu")
        self.passed(trial)
        runner.Trial.load(trial.artifacts).cleanup()
        self.assertEqual(bundle.snapshot(), before)
        self.assertTrue(bundle.artifacts.exists())
        self.assertTrue(self.profile_paths(trial)[0].exists())
        self.assertEqual(len(list(self.references.glob("m0-shared-canonical-*"))), 1)

    def test_profiled_malformed_input_propagates_nonzero_application_exit(self):
        trial = self.prepare()
        original = trial.io
        def corrupt(kind):
            result = original(kind)
            if kind == "stage":
                (trial.root / "input.csv").write_bytes(b"incorrect header\n")
            return result
        trial.io = corrupt
        self.assertFalse(trial.execute())
        self.assertEqual(trial.state["measurements"]["application"]["exit_status"]["value"], 1)
        self.assertEqual(trial.state["diagnostic"]["profile"]["script_exit_status"], 1)
        self.assertEqual((trial.root / "output.csv").read_bytes(), b"")
        self.assertTrue(trial.workspace.exists())

    def test_mocked_ext4_and_myfs_profiles_keep_lifecycle_and_evidence_populations(self):
        class ProfileProcesses(MeasurementProcesses):
            def wait(self, identity, deadline):
                entry = next(e for e in self.entries if e["identity"] == identity)
                if runner.PYTHON_PROFILE_CODE not in entry["command"]:
                    return super().wait(identity, deadline)
                artifacts, anchor_json, evidence, script, root = entry["command"][-5:]
                anchor = json.loads(anchor_json)
                with contextlib.redirect_stdout(io.StringIO()):
                    try:
                        runner.python_cpu_profile(Path(artifacts), anchor, evidence, Path(script), Path(root))
                    except SystemExit as e:
                        if e.code != 0:
                            raise
                receipt = Path(artifacts) / runner.profile_names(anchor["trial_id"])[1]
                value = json.loads(receipt.read_text())
                start = entry["launch_monotonic"]
                value.update(process_identity=identity, boundaries=dict(zip(
                    ("launcher_start", "profiler_enable", "profiler_disable", "serialization_start", "serialization_end"),
                    (start+.1, start+.2, start+4, start+4.1, start+4.5))))
                receipt.write_text(json.dumps(value))
                self.clock.value += 5
                entry.update(exit_status=0, exit_monotonic=self.clock.now(),
                    resources=runner.resource_metrics(None, "mocked", identity, start, self.clock.now()))
                self.reaps[identity["pid"]] = 1
                self.on_change()
                return 0
        for backend in ("ext4", "myfs"):
            with self.subTest(backend=backend):
                clock = FakeClock()
                processes, mounts = ProfileProcesses(clock), MeasurementMounts()
                trial = runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.references,
                    backend=backend, records=8, diagnostic="python-cpu", expected_device="259:7",
                    daemon=Path(sys.executable).resolve(), build_provenance={"mocked": True},
                    processes=processes, mounts=mounts, host=MeasurementHost(self.references), clock=clock)
                self.passed(trial)
                self.assertEqual(trial.state["diagnostic"]["profile"]["evidence"], "mocked")
                wall = trial.state["measurements"]["application"]["wall_seconds"]
                self.assertEqual(wall["value"], 5)
                self.assertEqual(wall["evidence"], "mocked")
                self.assertFalse(trial.state["baseline_eligible"])
                self.assertEqual(mounts.events, ["mount", "stat-probe", "unmount"] * 3 if backend == "myfs" else [])
                self.assertEqual(len(trial.state["measurements"]["daemon_lifecycles"]), 3 if backend == "myfs" else 0)
                trial.cleanup()
                self.assertTrue(self.profile_paths(trial)[0].exists())


class DaemonSyncTests(unittest.TestCase):
    """No real mount: lifecycle adapters are mocked; artifact validation is real."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="m0-sync-runner-")
        self.parent = Path(self.temp.name)
        self.refs = self.parent / "refs"
        self.refs.mkdir()
        self.clock = FakeClock()
        self.host = MeasurementHost(self.refs)
        self.processes = MeasurementProcesses(self.clock)
        self.mounts = MeasurementMounts()

    def tearDown(self):
        self.temp.cleanup()

    def prepare(self, backend="myfs", **kwargs):
        return runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.refs,
            backend=backend, records=8, expected_device="259:7", processes=self.processes,
            mounts=self.mounts, host=self.host, clock=self.clock,
            daemon=Path(sys.executable).resolve(), build_provenance={"mocked": True}, **kwargs)

    def test_bad_or_combined_modes_refused_before_allocation(self):
        before = list(self.parent.iterdir())
        for backend, mode, python in (("ext4", "sync-calls", None),
                ("offline-check", "sync-calls", None), ("myfs", "invalid", None),
                ("myfs", "sync-calls", "python-cpu")):
            with self.subTest(backend=backend, mode=mode, python=python):
                with self.assertRaises(runner.TrialError):
                    self.prepare(backend, daemon_diagnostic=mode, diagnostic=python)
        self.assertEqual(list(self.parent.iterdir()), before)

    def test_unverified_diagnostic_binary_refused(self):
        with self.assertRaisesRegex(runner.TrialError, "provenance"):
            self.prepare(daemon_diagnostic="sync-calls")

    def test_default_keeps_baseline_and_old_child_command(self):
        trial = self.prepare("ext4")
        self.assertTrue(trial.execute(), trial.state["error"])
        self.assertTrue(trial.state["baseline_eligible"])
        self.assertEqual(trial.state["measurement_population"], "buffered_application_completion")
        self.assertIn([sys.executable, "-B", str(application.__file__), str(trial.root)], trial.state["commands"])

    def test_fd_handoff_retains_direct_child_and_one_reaper(self):
        import subprocess
        clock = runner.Clock()
        processes = runner.AccountingProcesses(clock)
        path = self.parent / "child-artifact"
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            code = "import os; os.write(int(os.environ['M0_TEST_FD']), str(os.getpid()).encode())"
            env = dict(os.environ, M0_TEST_FD=str(fd))
            identity = processes.launch([sys.executable, "-B", "-c", code],
                self.parent / "child.log", 1024, pass_fds=(fd,), env=env)
        finally:
            os.close(fd)
        with mock.patch.object(subprocess.Popen, "poll", side_effect=AssertionError("alternate reaper")), \
                mock.patch.object(subprocess.Popen, "wait", side_effect=AssertionError("alternate reaper")):
            self.assertEqual(processes.wait(identity, clock.now() + 5), 0)
            self.assertEqual(processes.poll(identity), 0)
        self.assertEqual(path.read_text(), str(identity["pid"]))
        self.assertEqual(processes.entries[0]["reap_count"], 1)

    @staticmethod
    def trace(events=None):
        header = {"kind": "header", "schema": "m0-daemon-sync-v1", "trial_id": "a"*32,
            "token": "b"*32, "pid": 123, "clock": "CLOCK_MONOTONIC", "unit": "nanoseconds",
            "module_base": 4096, "caller_source": "__builtin_return_address",
            "max_bytes": 4194304, "max_events": 8192, "enabled_monotonic_ns": 1000000000}
        if events is None:
            events = [dict(kind="event", sequence=1, pid=123, tid=123, operation="fsync", fd=5,
                caller=8192, entry_ns=2000000000, exit_ns=4000000000, return_value=0,
                errno=0, clock_error=False),
                dict(kind="event", sequence=2, pid=123, tid=124, operation="fdatasync", fd=6,
                caller=8200, entry_ns=3000000000, exit_ns=5000000000, return_value=-1,
                errno=9, clock_error=False)]
        footer = {"kind": "footer", "observed": len(events), "written": len(events), "dropped": 0,
            "inflight": 0, "clock_errors": 0, "recording_errors": 0, "overflow": False,
            "complete": True, "finalized_monotonic_ns": 6000000000, "live_threads": 1,
            "trial_id": "a"*32, "token": "b"*32, "pid": 123}
        return [header, *events, footer]

    def decode(self, rows):
        raw = ("\n".join(json.dumps(r) for r in rows)+"\n").encode("ascii")
        return runner.decode_sync_trace(raw, trial_id="a"*32, token="b"*32,
            child={"identity": {"pid":123}, "launch_monotonic": .5, "exit_monotonic":7.,
                "exit_status":0, "reap_count":1}, application_interval=[2.5,4.5],
            shutdown_start=5.5, evidence="mocked")

    def test_independent_elapsed_union_and_straddling_populations(self):
        report = self.decode(self.trace())
        self.assertEqual(report["summary"]["event_count"], 2)
        self.assertEqual(report["summary"]["failed_count"], 1)
        self.assertEqual(report["summary"]["summed_seconds"], 4.)
        self.assertEqual(report["summary"]["union_seconds"], 3.)
        self.assertEqual(report["application_overlap"]["union_seconds"], 2.)
        self.assertEqual(report["boundary_straddling_count"], 2)
        self.assertEqual(report["evidence"], "mocked")

    def test_corrupt_partial_foreign_and_incomplete_events_rejected(self):
        import copy
        changes = [lambda r: r.pop(), lambda r: r[-1].update(complete=False),
            lambda r: r[-1].update(dropped=1), lambda r: r[-1].update(written=1),
            lambda r: r[-1].update(overflow=True), lambda r: r[-1].update(inflight=1),
            lambda r: r[0].update(pid=999), lambda r: r[0].update(token="c"*32),
            lambda r: r[1].update(exit_ns=100), lambda r: r[1].update(tid=0),
            lambda r: r[2].update(sequence=1), lambda r: r[1].update(clock_error=True),
            lambda r: r[1].update(operation="write"), lambda r: r[-1].update(pid=999),
            lambda r: r[-1].pop("live_threads"), lambda r: r[0].update(module_base=0),
            lambda r: r[1].update(sequence=True), lambda r: r[1].update(caller=1)]
        for change in changes:
            rows = copy.deepcopy(self.trace())
            change(rows)
            with self.subTest(rows=rows), self.assertRaises(runner.TrialError):
                self.decode(rows)


class SyncWrapperOfflineTests(unittest.TestCase):
    """Actual tiny C helpers; never execute or mount myfs."""

    @classmethod
    def setUpClass(cls):
        import shutil
        import subprocess
        cls.compiler = shutil.which("gcc")
        if not cls.compiler:
            raise unittest.SkipTest("gcc unavailable: offline wrapper build not verified")
        cls.temp = tempfile.TemporaryDirectory(prefix="m0-sync-wrapper-tests-")
        cls.directory = Path(cls.temp.name)
        cls.component = BENCHMARKS / "m0_daemon_sync_diagnostic.c"
        if not cls.component.exists():
            raise AssertionError("sync diagnostic component has not been implemented")
        cls.helper_source = cls.directory / "helper.c"
        cls.helper_source.write_text(r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static void *worker(void *value) {
    long count = (long)value;
    for (long i=0; i<count; ++i) {
        errno=0; if (fsync(-1)!=-1 || errno!=EBADF) abort();
        errno=0; if (fdatasync(-1)!=-1 || errno!=EBADF) abort();
    }
    return NULL;
}
static void *unfinished_worker(void *value) { (void)value; pause(); return NULL; }
int main(int argc, char **argv) {
    const char *mode=argc>1 ? argv[1] : "regular";
    if (!strcmp(mode,"threads") || !strcmp(mode,"overflow")) {
        pthread_t threads[8]; long count=!strcmp(mode,"overflow") ? 1024 : 64;
        for (int i=0;i<8;++i) if(pthread_create(&threads[i],NULL,worker,(void*)count)) abort();
        for (int i=0;i<8;++i) if(pthread_join(threads[i],NULL)) abort();
    } else {
        FILE *data=tmpfile(); if(!data) return 2;
        errno=EAGAIN; int a=fsync(fileno(data)); int ae=errno;
        errno=EINTR; int b=fdatasync(fileno(data)); int be=errno;
        errno=0; int c=fsync(-1); int ce=errno;
        errno=0; int d=fdatasync(-1); int de=errno;
        printf("%d %d %d %d %d %d %d %d\n",a,ae,b,be,c,ce,d,de);
        fclose(data);
        if (!strcmp(mode,"fork")) {
            pid_t child=fork(); if(child<0)return 5;
            if(!child) {
                errno=0;
                if(fcntl(atoi(argv[2]),F_GETFD)!=-1 || errno!=EBADF) _exit(6);
                if(fsync(-1)!=-1 || errno!=EBADF) _exit(7);
                _exit(0);
            }
            int status=0; if(waitpid(child,&status,0)!=child || status) return 8;
            if(fsync(-1)!=-1 || errno!=EBADF) return 9;
        }
        if (!strcmp(mode,"handoff")) {
            if(getenv("M0_SYNC_FD") || getenv("M0_SYNC_TRIAL") || getenv("M0_SYNC_TOKEN"))return 10;
            if(!(fcntl(atoi(argv[2]),F_GETFD)&FD_CLOEXEC))return 11;
        }
        if (!strcmp(mode,"live-thread")) {
            pthread_t thread;
            if(pthread_create(&thread,NULL,unfinished_worker,NULL))return 12;
        }
        if (!strcmp(mode,"incomplete")) _exit(0);
        if (!strcmp(mode,"write-failure")) {
            int replacement=open(argv[3],O_RDONLY); if(replacement<0)return 3;
            if(dup2(replacement,atoi(argv[2]))<0)return 4;
            close(replacement);
        }
    }
    return 0;
}
''')
        cls.helper = cls.directory / "helper"
        flags = [cls.compiler, "-O2", "-g", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-pthread"]
        result = subprocess.run(flags + [str(cls.helper_source), str(cls.component), "-Wl,--wrap=fsync,--wrap=fdatasync", "-o", str(cls.helper)], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(f"offline helper compile exit {result.returncode}: {result.stderr}")
        cls.stub_source = cls.directory / "stub.c"
        cls.stub_source.write_text(r'''
#include <errno.h>
int fsync_count, fdatasync_count;
int fsync(int fd) { ++fsync_count; errno=EAGAIN; return fd; }
int fdatasync(int fd) { ++fdatasync_count; errno=EBADF; return -fd; }
''')
        cls.count_source = cls.directory / "count.c"
        cls.count_source.write_text(r'''
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
extern int fsync_count,fdatasync_count;
int main(void) {
 errno=EINTR; int a=fsync(17), ae=errno;
 errno=EINTR; int b=fdatasync(23), be=errno;
 printf("%d %d %d %d %d %d\n",a,ae,b,be,fsync_count,fdatasync_count);
 return 0;
}
''')
        cls.count_helper = cls.directory / "count"
        result = subprocess.run(flags + [str(cls.count_source), str(cls.stub_source), str(cls.component), "-Wl,--wrap=fsync,--wrap=fdatasync", "-o", str(cls.count_helper)], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(f"stub helper compile exit {result.returncode}: {result.stderr}")
        cls.readdir_source = cls.directory / "readdir_failure.c"
        cls.readdir_source.write_text(r'''
#include <dirent.h>
#include <errno.h>
#include <string.h>
struct dirent *readdir(DIR *directory) {
    (void)directory;
    static struct dirent entry;
    static int calls;
    if(calls++) { errno=EIO; return 0; }
    strcpy(entry.d_name,"123"); return &entry;
}
''')
        cls.readdir_helper = cls.directory / "readdir-failure"
        result = subprocess.run(flags + [str(cls.helper_source), str(cls.readdir_source), str(cls.component),
            "-Wl,--wrap=fsync,--wrap=fdatasync", "-o", str(cls.readdir_helper)],
            capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(f"readdir failure helper compile exit {result.returncode}: {result.stderr}")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self, mode="regular", *, enabled=True, count=False, changes=None,
               file_mode=None, prefix=b"", hardlink=False, binary=None):
        import subprocess
        import uuid
        root = self.directory / uuid.uuid4().hex
        root.mkdir(mode=0o700)
        path = root / "events.jsonl"
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        if file_mode is not None:
            os.fchmod(fd, file_mode)
        if prefix:
            os.write(fd, prefix)
            os.lseek(fd, 0, os.SEEK_SET)
        if hardlink:
            os.link(path, root / "alias.jsonl")
        env = dict(os.environ)
        for key in ("M0_SYNC_FD", "M0_SYNC_TRIAL", "M0_SYNC_TOKEN"):
            env.pop(key, None)
        if enabled:
            env.update(M0_SYNC_FD=str(fd), M0_SYNC_TRIAL="1"*32, M0_SYNC_TOKEN="2"*32)
        if changes:
            env.update(changes)
        try:
            result = subprocess.run([str(self.count_helper)] if count else [str(binary or self.helper), mode, str(fd), str(path)],
                env=env, pass_fds=(fd,), capture_output=True, text=True, timeout=10)
        finally:
            os.close(fd)
        self.assertEqual(result.returncode, 0, result.stderr)
        data = path.read_bytes()
        rows = [json.loads(row) for row in data.splitlines()]
        return result, path, rows

    def test_wrapper_calls_real_operations_once_and_preserves_return_and_errno(self):
        import errno
        result, _, rows = self.invoke(count=True)
        self.assertEqual(result.stdout.strip(), f"17 {errno.EAGAIN} -23 {errno.EBADF} 1 1")
        self.assertEqual([r["return_value"] for r in rows[1:-1]], [17, -23])
        self.assertEqual([r["errno"] for r in rows[1:-1]], [errno.EAGAIN, errno.EBADF])

    def test_real_linker_wrapped_success_failure_and_monotonic_caller_evidence(self):
        import errno
        result, path, rows = self.invoke()
        self.assertEqual(result.stdout.strip(), f"0 {errno.EAGAIN} 0 {errno.EINTR} -1 {errno.EBADF} -1 {errno.EBADF}")
        header, footer = rows[0], rows[-1]
        self.assertEqual(header["schema"], "m0-daemon-sync-v1")
        self.assertEqual(header["trial_id"], "1"*32)
        self.assertEqual(header["token"], "2"*32)
        self.assertEqual(header["clock"], "CLOCK_MONOTONIC")
        self.assertGreater(header["module_base"], 0)
        self.assertTrue(footer["complete"])
        self.assertEqual(footer["observed"], 4)
        self.assertEqual(footer["written"], 4)
        self.assertEqual(footer["dropped"], 0)
        self.assertEqual(footer["inflight"], 0)
        self.assertLessEqual(path.stat().st_size, 4*1024*1024)
        for event in rows[1:-1]:
            self.assertEqual(event["pid"], header["pid"])
            self.assertGreater(event["tid"], 0)
            self.assertGreaterEqual(event["entry_ns"], header["enabled_monotonic_ns"])
            self.assertGreaterEqual(event["exit_ns"], event["entry_ns"])
            self.assertLessEqual(event["exit_ns"], footer["finalized_monotonic_ns"])
            self.assertGreater(event["caller"], header["module_base"])

    def test_disabled_instrumentation_emits_nothing_and_preserves_real_result(self):
        import errno
        result, _, rows = self.invoke(enabled=False, count=True)
        self.assertEqual(result.stdout.strip(), f"17 {errno.EAGAIN} -23 {errno.EBADF} 1 1")
        self.assertEqual(rows, [])

    def test_threaded_recorder_has_no_lost_or_duplicate_events(self):
        _, _, rows = self.invoke("threads")
        events = rows[1:-1]
        self.assertEqual(len(events), 1024)
        self.assertEqual(sorted(event["sequence"] for event in events), list(range(1, 1025)))
        self.assertEqual(len(set(event["tid"] for event in events)), 8)
        self.assertTrue(rows[-1]["complete"])
        self.assertEqual(rows[-1]["observed"], 1024)
        self.assertEqual(rows[-1]["clock_errors"], 0)
        self.assertEqual(rows[-1]["recording_errors"], 0)

    def test_overflow_is_bounded_and_explicitly_incomplete(self):
        _, path, rows = self.invoke("overflow")
        self.assertLessEqual(path.stat().st_size, 4*1024*1024)
        self.assertEqual(rows[-1]["observed"], 16384)
        self.assertEqual(rows[-1]["written"], 8192)
        self.assertEqual(rows[-1]["dropped"], 8192)
        self.assertTrue(rows[-1]["overflow"])
        self.assertFalse(rows[-1]["complete"])

    def test_abrupt_exit_has_no_complete_footer(self):
        _, _, rows = self.invoke("incomplete")
        self.assertEqual(rows, [])

    def test_failed_serialization_cannot_be_presented_as_complete(self):
        _, _, rows = self.invoke("write-failure")
        self.assertFalse(rows and rows[-1].get("complete"))

    def test_invalid_enabled_configuration_does_not_change_sync_results(self):
        import errno
        result, _, rows = self.invoke(count=True, changes={"M0_SYNC_TRIAL": "invalid"})
        self.assertEqual(result.stdout.strip(), f"17 {errno.EAGAIN} -23 {errno.EBADF} 1 1")
        self.assertEqual(rows, [])

    def test_forked_helper_cannot_inherit_the_diagnostic_writer(self):
        _, _, rows = self.invoke("fork")
        self.assertTrue(rows[-1]["complete"])
        self.assertEqual(rows[-1]["written"], 5)
        self.assertEqual(len(set(row["pid"] for row in rows)), 1)

    def test_constructor_clears_handoff_environment_and_sets_close_on_exec(self):
        _, _, rows = self.invoke("handoff")
        self.assertTrue(rows[-1]["complete"])

    def test_nonprivate_or_aliased_artifact_descriptor_is_refused(self):
        for options in ({"file_mode": 0o644}, {"hardlink": True}):
            with self.subTest(options=options):
                _, _, rows = self.invoke(**options)
                self.assertEqual(rows, [])

    def test_nonempty_artifact_is_not_overwritten(self):
        before = b'{"existing":"preserve"}\n'
        _, path, rows = self.invoke(prefix=before)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(rows, [{"existing": "preserve"}])

    def test_surviving_threads_invalidate_normal_exit_completeness(self):
        _, _, rows = self.invoke("live-thread")
        self.assertGreater(rows[-1]["live_threads"], 1)
        self.assertGreater(rows[-1]["recording_errors"], 0)
        self.assertFalse(rows[-1]["complete"])

    def test_incomplete_thread_enumeration_cannot_publish_complete_evidence(self):
        _, _, rows = self.invoke(binary=self.readdir_helper)
        self.assertEqual(rows[-1]["live_threads"], -1)
        self.assertGreater(rows[-1]["recording_errors"], 0)
        self.assertFalse(rows[-1]["complete"])

    def test_invalid_token_still_contains_a_verified_private_handoff_descriptor(self):
        for mode in ("handoff", "fork"):
            with self.subTest(mode=mode):
                _, _, rows = self.invoke(mode, changes={"M0_SYNC_TOKEN": "invalid"})
                self.assertEqual(rows, [])


    def test_real_taskset_fd_handoff_single_reaper_and_independent_parser(self):
        import shutil
        import subprocess
        taskset = shutil.which("taskset")
        self.assertIsNotNone(taskset, "taskset required to verify actual launch chain")
        with tempfile.TemporaryDirectory(prefix="m0-sync-owned-helper-") as directory:
            root = Path(directory)
            raw = root / "events.jsonl"
            fd = os.open(raw, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            clock = runner.Clock()
            processes = runner.AccountingProcesses(clock)
            try:
                env = dict(os.environ, M0_SYNC_FD=str(fd), M0_SYNC_TRIAL="a"*32, M0_SYNC_TOKEN="b"*32)
                command = [taskset, "-c", str(min(os.sched_getaffinity(0))), str(self.helper), "handoff", str(fd)]
                identity = processes.launch(command, root/"helper.log", 4096, pass_fds=(fd,), env=env)
            finally:
                os.close(fd)
            with mock.patch.object(subprocess.Popen, "poll", side_effect=AssertionError("second reaper")), \
                    mock.patch.object(subprocess.Popen, "wait", side_effect=AssertionError("second reaper")):
                self.assertEqual(processes.wait(identity,clock.now()+5),0)
                self.assertEqual(processes.poll(identity),0)
            child = processes.entries[0]
            records = [json.loads(line) for line in raw.read_bytes().splitlines()]
            self.assertEqual(records[0]["pid"],identity["pid"])
            self.assertEqual(child["reap_count"],1)
            interval=[records[0]["enabled_monotonic_ns"]/1e9,records[-1]["finalized_monotonic_ns"]/1e9]
            decoded=runner.decode_sync_trace(raw.read_bytes(),trial_id="a"*32,token="b"*32,
                child=child,application_interval=interval,shutdown_start=interval[-1],evidence="actual offline helper")
            self.assertTrue(decoded["valid"])
            self.assertEqual(decoded["summary"]["event_count"],4)
            self.assertEqual(decoded["summary"]["failed_count"],2)
            self.assertEqual(decoded["failed_errno_counts"],{"9":2})


class SyncMeasurementProcesses(MeasurementProcesses):
    """Explicitly mocked mount daemon; real descriptor handoff and trace bytes."""
    def __init__(self, clock):
        super().__init__(clock)
        self.launch_options = []
        self.trace_fd = None
        self.trace_environment = None
        self.trace_identity = None
        self.trace_error = None

    def launch(self, command, log, limit, *, pass_fds=(), env=None):
        identity = super().launch(command, log, limit)
        if "-f" in command and any(str(arg).startswith("fsname=m0-") for arg in command):
            self.launch_options.append({"pass_fds": pass_fds, "env": dict(env or {}), "identity": identity})
        if pass_fds:
            self.trace_fd = os.dup(pass_fds[0])
            self.trace_environment = dict(env)
            self.trace_identity = identity
        return identity

    def wait(self, identity, deadline):
        code = super().wait(identity, deadline)
        entry = next(e for e in self.entries if e["identity"] == identity)
        entry["reap_count"] = self.reaps[identity["pid"]]
        if identity == self.trace_identity and self.trace_fd is not None:
            try:
                app = next(e for e in self.entries if str(application.__file__) in e["command"])
                rows = DaemonSyncTests.trace()
                rows[0].update(trial_id=self.trace_environment["M0_SYNC_TRIAL"], token=self.trace_environment["M0_SYNC_TOKEN"],
                    pid=identity["pid"], enabled_monotonic_ns=round((entry["launch_monotonic"]+.01)*1e9))
                for index,event in enumerate(rows[1:-1]):
                    event.update(pid=identity["pid"], tid=identity["pid"]+index,
                        entry_ns=round((app["launch_monotonic"]+1+index)*1e9),
                        exit_ns=round((app["launch_monotonic"]+2+index)*1e9))
                rows[-1].update(finalized_monotonic_ns=round((entry["exit_monotonic"]-.01)*1e9),
                    live_threads=1, trial_id=rows[0]["trial_id"], token=rows[0]["token"], pid=identity["pid"])
                if self.trace_error == "incomplete":
                    rows.pop()
                elif self.trace_error == "foreign":
                    rows[0]["pid"] = 99999
                if self.trace_error != "missing":
                    os.write(self.trace_fd, ("\n".join(json.dumps(r) for r in rows)+"\n").encode("ascii"))
            finally:
                os.close(self.trace_fd)
                self.trace_fd = None
        return code


class DaemonSyncLifecycleTests(unittest.TestCase):
    setUp = DaemonSyncTests.setUp
    tearDown = DaemonSyncTests.tearDown

    def provenance(self):
        # Intentionally mocked linked-image evidence; no real daemon is executed.
        exe = runner.executable_identity(Path(sys.executable).resolve())
        production = runner.measurement_source()["production_files"]
        return {"schema":"m0-smoke-build-provenance-v1", "exit_status":0,
            "source_fingerprints_before":production, "source_fingerprints_after":production,
            "daemon":exe, "evidence":"mocked", "daemon_sync":{
                "schema":"myfs-m0-sync-build-v1", "wrapper_sha256":runner.summary(runner.SYNC_WRAPPER)["sha256"],
                "wrap_symbols":["fsync","fdatasync"], "coverage_verified":True,
                "build_flags":["-O2","-DNDEBUG","-g","-fno-omit-frame-pointer"], "module_build_id":"d"*40,
                "coverage":[dict(source=s,function=f,operation=o,source_line=n,
                    linked_call_address="abc",target="__wrap_"+o) for s,f,o,n in runner.SYNC_SITES]}}

    def prepare(self, **kwargs):
        self.processes = SyncMeasurementProcesses(self.clock)
        return runner.MeasurementTrial.prepare(self.parent, canonical_parent=self.refs,
            backend="myfs", records=8, expected_device="259:7", processes=self.processes,
            mounts=self.mounts, host=self.host, clock=self.clock,
            daemon=Path(sys.executable).resolve(), build_provenance=self.provenance(),
            daemon_diagnostic="sync-calls", **kwargs)

    def test_full_fake_lifecycle_excludes_other_mounts_and_preserves_timing(self):
        trial = self.prepare()
        self.assertEqual(trial.state["outcome"], "prepared", trial.state["error"])
        before = trial.canonical_snapshot()
        self.assertTrue(trial.execute(), trial.state["error"])
        self.assertFalse(trial.state["baseline_eligible"])
        self.assertEqual(trial.state["measurement_population"], "daemon_sync_instrumented_application_completion")
        self.assertEqual(trial.state["measurements"]["application"]["wall_seconds"]["value"],5.)
        options = self.processes.launch_options
        self.assertEqual([len(v["pass_fds"]) for v in options], [0,1,0])
        self.assertFalse(any(k in options[0]["env"] or k in options[2]["env"] for k in runner.SYNC_ENV))
        self.assertEqual(options[1]["env"]["M0_SYNC_TRIAL"], trial.anchor["trial_id"])
        trace = trial.state["daemon_diagnostic"]["trace"]
        self.assertTrue(trace["valid"])
        self.assertEqual(trace["report"]["evidence"], "mocked")
        self.assertEqual(trace["report"]["summary"]["event_count"],2)
        self.assertEqual(trace["daemon"], options[1]["identity"])
        self.assertEqual(self.mounts.events, ["mount","stat-probe","unmount"]*3)
        self.assertEqual(before, trial.canonical_snapshot())
        self.assertTrue(all(n == 1 for n in self.processes.reaps.values()))
        self.assertIn("NOT COLLECTED",trial.state["daemon_diagnostic"]["preparation"]["reason"])
        artifact = Path(trace["artifact"])
        trial.cleanup()
        self.assertTrue(artifact.exists())
        self.assertTrue(trial.canonical_directory.exists())
        self.assertFalse(trial.workspace.exists())

    def test_changed_diagnostic_state_cannot_become_baseline(self):
        trial = self.prepare()
        trial.state["baseline_eligible"] = True
        with self.assertRaises(runner.TrialError):
            trial.check_paths()
        trial.state["baseline_eligible"] = False
        trial.state["daemon_diagnostic"]["configuration"]["mode"] = None
        with self.assertRaises(runner.TrialError):
            trial.check_paths()

    def test_missing_incomplete_foreign_trace_fails_and_retains_workspace(self):
        for mode in ("missing", "incomplete", "foreign"):
            with self.subTest(mode=mode):
                trial = self.prepare()
                self.processes.trace_error = mode
                self.assertFalse(trial.execute())
                self.assertIn("sync diagnostic",trial.state["error"])
                self.assertFalse(trial.state["daemon_diagnostic"]["trace"]["valid"])
                self.assertTrue(trial.workspace.exists())
                self.assertTrue(trial.artifacts.exists())
                with self.assertRaises(runner.TrialError):
                    trial.cleanup()

    def test_existing_artifact_and_symlink_refused(self):
        for symlink in (False,True):
            with self.subTest(symlink=symlink):
                trial=self.prepare()
                raw=trial.artifacts/runner.sync_names(trial.anchor["trial_id"])[0]
                foreign=self.parent/("foreign-"+trial.anchor["trial_id"])
                foreign.write_text("untouched")
                if symlink:
                    raw.symlink_to(foreign)
                else:
                    raw.write_text("existing")
                self.assertFalse(trial.execute())
                self.assertIn("overwrite",trial.state["error"])
                self.assertEqual(foreign.read_text(),"untouched")
                self.assertTrue(trial.workspace.exists())

    def test_trace_or_receipt_change_prevents_cleanup(self):
        for kind in ("raw", "receipt", "identity", "oversized"):
            with self.subTest(kind=kind):
                trial=self.prepare()
                self.assertTrue(trial.execute(),trial.state["error"])
                raw,receipt=(trial.artifacts/n for n in runner.sync_names(trial.anchor["trial_id"]))
                if kind == "raw":
                    raw.write_bytes(raw.read_bytes()[:-2])
                elif kind == "receipt":
                    receipt.write_text("{}")
                elif kind == "oversized":
                    with raw.open("ab") as stream: stream.truncate(runner.SYNC_MAX_BYTES+1)
                else:
                    blob=raw.read_bytes()
                    raw.rename(raw.with_suffix(".saved"))
                    raw.write_bytes(blob)
                    raw.chmod(0o600)
                with self.assertRaises(runner.TrialError): trial.cleanup()
                self.assertTrue(trial.workspace.exists())
                self.assertFalse(trial.state["daemon_diagnostic"]["trace"]["valid"])

    def test_timeout_abnormal_exit_and_warning_cannot_validate_trace(self):
        for mode in ("shutdown", "application", "abnormal"):
            trial=self.prepare()
            if mode == "shutdown": self.processes.shutdown_timeout=True
            elif mode == "application": self.processes.application_timeout=True
            else: self.processes.abnormal_exit=True
            self.assertFalse(trial.execute(),mode)
            self.assertTrue(trial.workspace.exists())
            with self.assertRaises(runner.TrialError): trial.cleanup()
            if self.processes.trace_fd is not None:
                os.close(self.processes.trace_fd)
                self.processes.trace_fd=None

    def test_provenance_coverage_and_source_mismatch_refused(self):
        for key,value in (("wrapper_sha256","0"*64),("coverage",[]),("wrap_symbols",["fsync"]),
                ("module_build_id",""),("coverage_verified",False)):
            receipt=self.provenance()
            receipt["daemon_sync"][key]=value
            with self.subTest(key=key),self.assertRaises(runner.TrialError):
                runner.sync_build(receipt["daemon"]["path"],receipt)

    def test_cli_is_measurement_only_and_combination_rejected(self):
        import subprocess
        base=[sys.executable,"-B",str(BENCHMARKS/"m0_profile.py")]
        for command in ("prepare","self-test","measure-self-test"):
            result=subprocess.run(base+[command,"--help"],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0)
            self.assertNotIn("--daemon-diagnostic",result.stdout)
        result=subprocess.run(base+["measure-prepare","--help"],capture_output=True,text=True,timeout=5)
        self.assertIn("--daemon-diagnostic {sync-calls}",result.stdout)


if __name__ == "__main__":
    unittest.main()
