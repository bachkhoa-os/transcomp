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


if __name__ == "__main__":
    unittest.main()
