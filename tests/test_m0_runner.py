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


if __name__ == "__main__":
    unittest.main()
