"""M0 smoke lifecycle and opt-in single-trial measurement.

CLI (Python standard library; no builds):
  prepare --parent DIR --backend offline-directory|myfs [--records 32768]
          [--seed 20261010] [--daemon ABSOLUTE_FILE --build-provenance TEXT]
  run ARTIFACT_DIRECTORY
  verify ARTIFACT_DIRECTORY
  cleanup ARTIFACT_DIRECTORY
  self-test --parent DIR [--records 32768]

prepare creates two private, uniquely named sibling directories under an
existing, caller-selected parent. Artifacts contain the canonical input,
manifest, independent expected output, ownership anchor, state and logs.
Only the workspace sibling (application root and backing) is deletable.
run consumes a prepared trial exactly once. self-test explicitly selects an
ordinary directory and is an OFFLINE CORRECTNESS CHECK, never an ext4 or FUSE
benchmark. verify requires a passed trial and checks canonical artifacts and,
for an offline trial,
its application files. Mounted verification is part of run's remount phase;
standalone verify does not mount. cleanup requires a successful terminal
trial, unchanged ownership/identities, no mounts and confirmed child exits.
Failures retain both directories. No automatic deletion or recovery mounting.

myfs requires an explicitly supplied executable and build provenance. Its
command follows src/main.c: executable -f -o fsname=TOKEN,subtype=myfs ROOT
BACKING. Readiness requires the exact mountinfo target/source/type/root and
an owned, deadline-bounded create/read/unlink probe. Orderly unmount and a
normal observed daemon exit are lifecycle boundaries, NOT crash durability
or reclamation evidence. Process and mount adapters are explicitly injectable;
fake adapters must label their evidence mocked. Persisted PIDs are never
adopted for signaling: without this instance's owned child handle, fail closed.

Limits: 0..32768 records; positive deadlines <=300s; logs <=1MiB per child;
conservative free-space reservation 64MiB + 8 times logical input bytes.
Mounted file I/O runs in deadline-bounded owned helpers. No RLIMIT changes,
cache controls or baselines are used by the smoke path.
Exit 0 means the requested action succeeded, 1 means error/invalid trial,
and argparse uses 2 for invalid syntax. JSON includes retained artifact paths.

Opt-in single-trial measurement (<=32768 records; no repetitions):
  measure-prepare --parent DIR --canonical-parent DIR --backend ext4|myfs
                  --expected-device MAJOR:MINOR [--records 32768 --seed 20261010]
                  [--daemon FILE --build-provenance OWNED_JSON_FILE]
                  [--application-cpus LIST --daemon-cpus LIST] [--materialize]
  measure-run ARTIFACT_DIRECTORY
  measure-self-test --parent DIR --canonical-parent DIR [--records 100]
  verify ARTIFACT_DIRECTORY; cleanup ARTIFACT_DIRECTORY

Real measurements require canonical references on a different filesystem
from the verified ext4 device. Canonical/artifact trees survive cleanup.
measure-self-test is explicitly OFFLINE, never an ext4/myfs measurement;
its primary benchmark wall value is null. Measurement uses root stat probes,
owned wait4 accounting, first-access-after-staging history, and bounded
post-exit allocation accounting. It does not establish equivalent cache
residency, crash durability, isolated FUSE cost or compression savings.
--materialize fsyncs only owned files/directories, separately and untimed.
Without it st_blocks can be affected by delayed allocation. Source and
executable fingerprints must remain unchanged between preparation and run.
Provided build provenance is cross-checked, not cryptographically attested.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import stat
import subprocess
import sys
import time
import uuid

import m0_fixture as fixture


SCHEMA = "myfs-m0-lifecycle-v1"
MAX_RECORDS = 32768
REPOSITORY = Path(__file__).resolve().parents[1]
APPLICATION = Path(__file__).with_name("m0_csv_workflow.py")


class TrialError(Exception):
    """Invalid trial or safety refusal; retain evidence."""


class Clock:
    now = staticmethod(time.monotonic)
    sleep = staticmethod(time.sleep)


def strict_path(path):
    """Canonical absolute path with no symlink in any existing component."""
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        if part.is_symlink():
            raise TrialError(f"symlink path refused: {part}")
    if path.resolve(strict=True) != path:
        raise TrialError(f"noncanonical path: {path}")
    return path


def directory_identity(path):
    info = Path(path).lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise TrialError(f"not an owned directory: {path}")
    return {"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid}


def exclusive_json(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w", encoding="ascii") as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write("\n")


def read_json(path):
    info = Path(path).lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_size > 2 * 1024**2:
        raise TrialError(f"unsafe state/marker file: {path}")
    with Path(path).open("r", encoding="ascii") as stream:
        return json.load(stream)


def summary(path):
    digest, count, lines = hashlib.sha256(), 0, 0
    with Path(path).open("rb", buffering=fixture.BUFFER_BYTES) as stream:
        while chunk := stream.read(fixture.BUFFER_BYTES):
            digest.update(chunk)
            count += len(chunk)
            lines += chunk.count(b"\n")
    return {"sha256": digest.hexdigest(), "byte_count": count, "row_count": max(0, lines - 1)}


def compare(reference, actual):
    """Exact streaming byte comparison against an immutable canonical file."""
    mismatch, offset = None, 0
    with Path(reference).open("rb") as expected, Path(actual).open("rb") as observed:
        while True:
            want, got = expected.read(65536), observed.read(65536)
            if not want and not got:
                break
            if mismatch is None and want != got:
                i = next((i for i, (a, b) in enumerate(zip(want, got)) if a != b),
                         min(len(want), len(got)))
                mismatch = {"offset": offset + i, "expected_hex": want[i:i + 16].hex(),
                            "actual_hex": got[i:i + 16].hex()}
            offset += len(want)
    wanted, observed = summary(reference), summary(actual)
    return {"passed": mismatch is None and wanted == observed, "expected": wanted, "actual": observed,
            "first_mismatch": mismatch}


def stage_files(artifacts, root):
    """Child helper: write through application root, never backing storage."""
    if os.path.lexists(root / "output.csv"):
        raise TrialError("existing output; fresh output required")
    with (artifacts / "input.csv").open("rb") as source, (root / "input.csv").open("xb") as target:
        shutil.copyfileobj(source, target, fixture.BUFFER_BYTES)
    return {"input": compare(artifacts / "input.csv", root / "input.csv")}


def verify_files(artifacts, root):
    return {"input": compare(artifacts / "input.csv", root / "input.csv"),
            "output": compare(artifacts / "expected.csv", root / "output.csv")}


IO_CODE = (
    "import json,sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
    "import m0_profile as m; kind,art,root,result=sys.argv[2:]; "
    "value=(m.stage_files if kind=='stage' else m.verify_files)(Path(art),Path(root)); "
    "m.exclusive_json(Path(result),value)"
)


def process_identity(pid):
    """Linux boot ID + /proc start tick, paired with an owned handle/pidfd.

    /proc/PID/exe disappears for a zombie before waitpid observes its exit;
    boot/start identity remains readable until this Popen reaps the child.
    Daemon executable identity is separately recorded and checked at launch.
    """
    raw = Path(f"/proc/{pid}/stat").read_text()
    ticks = raw[raw.rfind(")") + 2:].split()[19]
    return {"pid": pid, "start_ticks": ticks,
            "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}


class Processes:
    """Owned Popen handles + pidfds; bounded logs/waits; no PID adoption."""
    evidence = "actual"

    def __init__(self, clock):
        self.clock, self.entries, self.handles = clock, [], {}
        self.on_change = lambda: None

    def launch(self, command, log, limit):
        if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
            raise TrialError("pidfd supervision unavailable; refusing launch")
        fd = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        stream = os.fdopen(fd, "wb")
        launch_time = self.clock.now()
        try:
            child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, close_fds=True)
        except BaseException:
            stream.close()
            raise
        entry = {"identity": None, "pid": child.pid, "identity_unavailable_reason": "identity capture pending",
                 "command": command, "exit_status": None,
                 "exit_status_reason": "exit not yet observed", "launch_monotonic": launch_time,
                 "exit_monotonic": None,
                 "log": str(log), "log_limit": limit, "log_bytes": 0,
                 "log_truncated": False, "evidence": self.evidence}
        handle = {"child": child, "pidfd": None, "log": stream, "entry": entry,
                  "closed": False}
        self.handles[child.pid] = handle
        self.entries.append(entry)
        self.on_change()
        try:
            # The child can exit quickly. Popen still owns the unreaped process.
            handle["pidfd"] = os.pidfd_open(child.pid)
            identity = process_identity(child.pid)
            entry.update(identity=identity, identity_unavailable_reason=None)
            os.set_blocking(child.stdout.fileno(), False)
        except OSError as error:
            entry["identity_unavailable_reason"] = str(error)
            self.on_change()
            raise TrialError(f"launched child identity unavailable; exit unconfirmed: {error}") from error
        self.on_change()
        return identity

    def owned(self, identity):
        handle = self.handles.get(identity.get("pid"))
        if handle is None or handle["entry"]["identity"] != identity:
            return False
        if handle["closed"]:
            return True
        try:
            return process_identity(identity["pid"]) == identity
        except OSError:
            return False

    def _drain(self, handle):
        entry = handle["entry"]
        for _ in range(8):
            try:
                chunk = os.read(handle["child"].stdout.fileno(), 65536)
            except BlockingIOError:
                break
            if not chunk:
                handle["pipe_eof"] = True
                break
            room = entry["log_limit"] - entry["log_bytes"]
            handle["log"].write(chunk[:room])
            entry["log_bytes"] += min(room, len(chunk))
            if len(chunk) > room:
                entry["log_truncated"] = True
        handle["log"].flush()

    def poll(self, identity):
        if not self.owned(identity):
            raise TrialError("process identity cannot be established; retaining evidence")
        handle = self.handles[identity["pid"]]
        if handle["closed"]:
            return handle["entry"]["exit_status"]
        self._drain(handle)
        code = handle["child"].poll()
        if code is not None:
            self._drain(handle)
            handle["entry"]["exit_status"] = code
            handle["entry"]["exit_status_reason"] = None
            handle["entry"]["exit_monotonic"] = self.clock.now()
            handle["child"].stdout.close()
            handle["log"].close()
            os.close(handle["pidfd"])
            handle["closed"] = True
            self.on_change()
        if handle["entry"]["log_truncated"]:
            raise TrialError("child log limit exceeded")
        return code

    def wait(self, identity, deadline):
        while True:
            code = self.poll(identity)
            if self.clock.now() >= deadline:
                raise TrialError("child/shutdown timeout; exit unconfirmed")
            if code is not None:
                return code
            self.clock.sleep(min(0.02, deadline - self.clock.now()))

    def signal_owned(self, identity, signum):
        if not self.owned(identity):
            raise TrialError("stale/unowned process identity; refusing signal")
        handle = self.handles[identity["pid"]]
        if handle["closed"]:
            raise TrialError("process already exited; refusing signal")
        signal.pidfd_send_signal(handle["pidfd"], signum)
        handle["entry"].setdefault("signals", []).append({"signal": signum, "monotonic": self.clock.now()})
        self.on_change()

    def run(self, command, log, limit, deadline):
        identity = self.launch(command, log, limit)
        try:
            code = self.wait(identity, deadline)
        except TrialError:
            # Only this launched helper; daemon shutdown never uses this path.
            if self.owned(identity) and not self.handles[identity["pid"]]["closed"]:
                self.signal_owned(identity, signal.SIGTERM)
                try:
                    self.wait(identity, self.clock.now() + 1)
                except TrialError:
                    if self.owned(identity) and not self.handles[identity["pid"]]["closed"]:
                        self.signal_owned(identity, signal.SIGKILL)
                        try:
                            self.wait(identity, self.clock.now() + 1)
                        except TrialError:
                            pass  # Unconfirmed exit remains recorded; cleanup refuses.
            raise
        return {"exit_status": code, "identity": identity, "evidence": self.evidence}


class Mounts:
    """Linux mountinfo inspection; ordinary, never lazy/forced, FUSE unmount."""
    evidence = "actual"

    def preflight(self):
        if not os.access("/dev/fuse", os.R_OK | os.W_OK):
            raise TrialError("/dev/fuse access unavailable in this environment; no fallback")
        if shutil.which("fusermount3") is None:
            raise TrialError("fusermount3 unavailable")

    def snapshot(self):
        def decode(value):
            return re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), value)

        mounts = []
        for line in Path("/proc/self/mountinfo").read_text().splitlines():
            left, right = line.split(" - ", 1)
            fields, filesystem = left.split(), right.split()
            mounts.append({"mount_id": int(fields[0]), "root": decode(fields[3]),
                           "target": decode(fields[4]), "fstype": filesystem[0],
                           "source": decode(filesystem[1]), "evidence": self.evidence})
        return mounts

    def command(self, daemon, root, backing, source):
        return [str(daemon), "-f", "-o", f"fsname={source},subtype=myfs", str(root), str(backing)]

    def probe(self, root, processes, deadline, log, limit):
        code = ("import sys; from pathlib import Path; p=Path(sys.argv[1])/sys.argv[2]; "
                "f=p.open('xb'); f.write(b'm0-readiness-v1'); f.close(); "
                "assert p.read_bytes()==b'm0-readiness-v1'; p.unlink()")
        return processes.run([sys.executable, "-B", "-c", code, str(root),
                              ".m0-probe-" + uuid.uuid4().hex], log, limit, deadline)

    def unmount(self, root, processes, deadline, log, limit):
        return processes.run([shutil.which("fusermount3"), "-u", str(root)], log, limit, deadline)


def source_evidence():
    facts = {"files": {}}
    for name in ("benchmarks/m0_profile.py", "benchmarks/m0_fixture.py",
                 "benchmarks/m0_csv_workflow.py", "tests/test_m0_runner.py", "tests/test_m0_profile.py",
                 "src/main.c", "Makefile"):
        facts["files"][name] = summary(REPOSITORY / name)["sha256"]
    for label, command in (("revision", ["git", "rev-parse", "HEAD"]),
                           ("worktree", ["git", "status", "--short"])):
        try:
            result = subprocess.run(command, cwd=REPOSITORY, capture_output=True, timeout=3,
                                    check=True)
            facts[label] = {"value": result.stdout.decode().strip(), "reason": None}
        except (OSError, subprocess.SubprocessError) as error:
            facts[label] = {"value": None, "reason": str(error)}
    return facts


def fd_mount_id(fd):
    for line in Path(f"/proc/self/fdinfo/{fd}").read_text().splitlines():
        if line.startswith("mnt_id:"):
            return int(line.split()[1])
    raise TrialError("directory mount identity unavailable; deletion refused")


def remove_owned_tree(parent_fd, name, expected, guard):
    """Delete only through a pinned, verified workspace descriptor.

    Each opened directory must remain on the workspace's mount ID, including
    bind mounts on the same device. Never follow symlinks. Check named entries
    against pinned descriptors before mutation; refuse concurrent replacement.
    Private directories exclude other users, not a privileged concurrent actor.
    """
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    root_fd = os.open(name, flags, dir_fd=parent_fd)
    try:
        info = os.fstat(root_fd)
        if {"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid} != expected:
            raise TrialError("cleanup workspace identity changed at deletion boundary")
        mount_id = fd_mount_id(root_fd)

        def same_entry(parent, child_name, child_fd):
            guard()
            named = os.stat(child_name, dir_fd=parent, follow_symlinks=False)
            opened = os.fstat(child_fd)
            if ((named.st_dev, named.st_ino) != (opened.st_dev, opened.st_ino)
                    or opened.st_uid != os.getuid() or fd_mount_id(child_fd) != mount_id):
                raise TrialError("cleanup path/mount/ownership substitution")

        def erase(fd, parent, child_name):
            same_entry(parent, child_name, fd)
            for entry_name in os.listdir(fd):
                same_entry(parent, child_name, fd)
                entry = os.stat(entry_name, dir_fd=fd, follow_symlinks=False)
                if entry.st_uid != os.getuid():
                    raise TrialError("cleanup encountered unrelated ownership")
                if stat.S_ISDIR(entry.st_mode):
                    nested = os.open(entry_name, flags, dir_fd=fd)
                    try:
                        if (entry.st_dev, entry.st_ino) != (os.fstat(nested).st_dev, os.fstat(nested).st_ino):
                            raise TrialError("cleanup directory substituted before open")
                        erase(nested, fd, entry_name)
                        same_entry(fd, entry_name, nested)
                        os.rmdir(entry_name, dir_fd=fd)
                    finally:
                        os.close(nested)
                else:
                    current = os.stat(entry_name, dir_fd=fd, follow_symlinks=False)
                    if (entry.st_dev, entry.st_ino) != (current.st_dev, current.st_ino):
                        raise TrialError("cleanup file substituted")
                    guard()
                    os.unlink(entry_name, dir_fd=fd)
            same_entry(parent, child_name, fd)

        erase(root_fd, parent_fd, name)
        os.rmdir(name, dir_fd=parent_fd)
    finally:
        os.close(root_fd)


class Trial:
    """Persistent phases + two private siblings; no destructive recovery."""
    schema = SCHEMA

    @property
    def canonical_directory(self):
        return self.artifacts

    def __init__(self, artifacts, anchor, state, processes=None, mounts=None, clock=None):
        self.artifacts, self.anchor, self.state = artifacts, anchor, state
        self.workspace = artifacts.parent / ("m0-work-" + anchor["trial_id"])
        self.root, self.backing = self.workspace / "application", self.workspace / "backing"
        self.clock = clock or Clock()
        self.processes, self.mounts = processes or Processes(self.clock), mounts or Mounts()
        self.processes.on_change = self.save
        self.daemon_identity, self.mount_identity = None, None

    @classmethod
    def prepare(cls, parent, *, backend, records=MAX_RECORDS, seed=fixture.DEFAULT_SEED,
                daemon=None, build_provenance=None, processes=None, mounts=None, clock=None,
                readiness_seconds=10, application_seconds=30, shutdown_seconds=30,
                io_seconds=30, log_bytes=1024**2):
        if backend not in ("offline-directory", "myfs") or not 0 <= records <= MAX_RECORDS:
            raise TrialError("invalid backend or record limit (0..32768)")
        deadlines = {"readiness": readiness_seconds, "application": application_seconds,
                     "shutdown": shutdown_seconds, "io": io_seconds}
        if any(not 0 < value <= 300 for value in deadlines.values()) or not 1024 <= log_bytes <= 1024**2:
            raise TrialError("invalid deadlines/log limit")
        if backend == "myfs" and (daemon is None or not build_provenance):
            raise TrialError("myfs requires explicit daemon and supplied build provenance")
        parent = strict_path(parent)
        trial_id = uuid.uuid4().hex
        artifacts, workspace = parent / ("m0-artifacts-" + trial_id), parent / ("m0-work-" + trial_id)
        artifacts.mkdir(mode=0o700)
        workspace.mkdir(mode=0o700)
        (workspace / "application").mkdir(mode=0o700)
        (workspace / "backing").mkdir(mode=0o700)
        identities = {name: directory_identity(path) for name, path in (
            ("artifacts", artifacts), ("workspace", workspace),
            ("application", workspace / "application"), ("backing", workspace / "backing"))}
        anchor = {"schema": SCHEMA, "trial_id": trial_id, "parent": str(parent),
                  "parent_identity": directory_identity(parent), "identities": identities}
        exclusive_json(artifacts / "owner.json", anchor)
        exclusive_json(workspace / "owner.json", anchor)
        state = {"schema": SCHEMA, "trial_id": trial_id, "anchor": anchor, "backend": backend,
                 "label": ("offline correctness check on an ordinary directory" if backend == "offline-directory"
                           else "myfs lifecycle smoke; mounted evidence requires actual execution"),
                 "roots": {"artifacts": str(artifacts), "workspace": str(workspace),
                           "application": str(workspace / "application"), "backing": str(workspace / "backing")},
                 "limits": {"deadlines_seconds": deadlines, "log_bytes_per_child": log_bytes,
                            "records": records, "helper_termination_grace_seconds": 2},
                 "seed": seed, "source": source_evidence(), "daemon": None, "canonical": {},
                 "outcome": "created", "error": None, "phases": [], "validations": [],
                 "children": [], "mounts": [], "commands": [], "logical": None,
                 "unavailable": {name: {"value": None, "reason": reason} for name, reason in (
                     ("effective_resource_limits", "not measured; no RLIMIT changes"),
                     ("compression", "not measured; logical bytes/selectivity are not compression"),
                     ("performance", "no timing benchmark or profiling in M0-B2"),
                     ("durability", "orderly exit is not crash durability evidence"),
                     ("reclamation", "no backing-tree sampling or reclamation measurement"),
                     ("application_cpu", "CPU collection deferred"),
                     ("daemon_cpu", "CPU collection deferred; offline mode has no daemon"),
                     ("waiting_io", "waiting/I/O collection deferred"),
                     ("peak_rss", "memory collection deferred"),
                     ("allocated_backing_usage", "backing usage collection deferred"))}}
        trial = cls(artifacts, anchor, state, processes, mounts, clock)
        state["evidence"] = ("mocked" if "mocked" in (trial.processes.evidence, trial.mounts.evidence)
                             else "actual offline execution" if backend == "offline-directory" else "actual")
        trial.save()
        try:
            def preflight():
                trial.no_mounts()
                trial.check_paths()
                needed = 64 * 1024**2 + 8 * (69 + records * 128)
                free = shutil.disk_usage(parent).free
                state["space_preflight"] = {"free_bytes": free, "required_bytes": needed,
                                            "reason": "conservative reservation; allocation behavior unknown"}
                if free < needed:
                    raise TrialError("insufficient free space for bounded smoke")
                if backend == "myfs":
                    executable = strict_path(daemon)
                    if not executable.is_file() or not os.access(executable, os.X_OK):
                        raise TrialError("daemon is not an executable file")
                    info = executable.stat()
                    state["daemon"] = {"path": str(executable), "sha256": summary(executable)["sha256"],
                                       "device": info.st_dev, "inode": info.st_ino,
                                       "supplied_build_provenance": build_provenance,
                                       "source_match": {"value": None, "reason": "supplied provenance; not independently built/verified"}}
                    trial.mounts.preflight()
            trial.phase("preflight", preflight)
            def generate():
                manifest = fixture.generate_csv(artifacts / "input.csv", artifacts / "input.manifest.json", records, seed)
                expected = fixture.write_expected(artifacts / "input.csv", artifacts / "expected.csv")
                state["canonical"] = trial.canonical_snapshot()
                state["fixture_manifest"] = manifest
                state["logical"] = {"input_rows": records, "input_bytes": manifest["byte_count"],
                                    "output_rows": expected["row_count"], "output_bytes": expected["byte_count"],
                                    "selectivity": expected["row_count"] / records if records else None,
                                    "selectivity_reason": None if records else "zero input rows",
                                    "meaning": "logical sizes and row filtering; not filesystem compression"}
            trial.phase("prepare", generate)
            state["outcome"] = "prepared"
            trial.save()
        except (OSError, ValueError, TrialError) as error:
            trial.fail(error)
        return trial

    @classmethod
    def load(cls, artifacts, **adapters):
        try:
            artifacts = strict_path(artifacts)
            anchor = read_json(artifacts / "owner.json")
            state = read_json(artifacts / "state.json")
            if cls is Trial and anchor.get("schema") == MEASUREMENT_SCHEMA:
                return MeasurementTrial.load(artifacts, **adapters)
            if anchor.get("schema") != cls.schema or not re.fullmatch(r"[0-9a-f]{32}", anchor.get("trial_id", "")):
                raise TrialError("missing/inconsistent ownership")
            trial = cls(artifacts, anchor, state, **adapters)
            trial.no_mounts()
            trial.check_paths()
            return trial
        except (OSError, ValueError, KeyError) as error:
            raise TrialError(f"unowned/inconsistent artifact path: {error}") from error

    def check_paths(self, mounted=False):
        expected_roots = {"artifacts": str(self.artifacts), "workspace": str(self.workspace),
                          "application": str(self.root), "backing": str(self.backing)}
        if (self.state.get("anchor") != self.anchor or self.state.get("trial_id") != self.anchor["trial_id"]
                or self.state.get("schema") != self.schema or self.state.get("roots") != expected_roots
                or self.artifacts.name != "m0-artifacts-" + self.anchor["trial_id"]
                or str(self.artifacts.parent) != self.anchor["parent"]
                or directory_identity(strict_path(self.artifacts.parent)) != self.anchor["parent_identity"]):
            raise TrialError("inconsistent ownership/state or path escape")
        for name, value in expected_roots.items():
            path = strict_path(value)
            if name == "application" and mounted:
                continue  # Underlying mountpoint identity is checked again after unmount.
            if directory_identity(path) != self.anchor["identities"][name]:
                raise TrialError(f"changed directory identity: {name}")
        if (read_json(self.artifacts / "owner.json") != self.anchor
                or read_json(self.workspace / "owner.json") != self.anchor):
            raise TrialError("missing/inconsistent ownership marker")

    def save(self):
        # Reloading must not erase historical or unconfirmed child evidence.
        retained = list(self.state.get("children", []))
        for entry in self.processes.entries:
            existing = next((i for i, old in enumerate(retained)
                             if old.get("identity") == entry.get("identity")
                             and (entry.get("identity") is not None or old.get("pid") == entry.get("pid"))), None)
            if existing is None:
                retained.append(entry)
            else:
                retained[existing] = entry
        self.state["children"] = retained
        temporary = self.artifacts / (".state-" + uuid.uuid4().hex)
        exclusive_json(temporary, self.state)
        os.replace(temporary, self.artifacts / "state.json")

    def phase(self, name, action):
        record = {"name": name, "start_monotonic": self.clock.now(), "end_monotonic": None,
                  "outcome": "running", "error": None, "evidence": self.state["evidence"]}
        self.state["phases"].append(record)
        self.save()
        try:
            result = action()
            record["outcome"] = "passed"
            return result
        except (OSError, ValueError, TrialError) as error:
            record.update(outcome="failed", error=str(error))
            raise
        finally:
            record["end_monotonic"] = self.clock.now()
            self.save()

    def fail(self, error):
        self.state.update(outcome="failed", error=str(error))
        self.save()
        self.verification_log({"error": str(error), "outcome": "failed", "artifacts_retained": True})

    def verification_log(self, value):
        # At most a few small summaries per trial, separate from application/daemon logs.
        with (self.artifacts / "verification.jsonl").open("a", encoding="ascii") as stream:
            stream.write(json.dumps(dict(value, monotonic=self.clock.now(), evidence=self.state["evidence"]),
                                    sort_keys=True) + "\n")

    def canonical_snapshot(self):
        result = {}
        for name in ("input.csv", "input.manifest.json", "expected.csv"):
            path = strict_path(self.canonical_directory / name)
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
                raise TrialError("canonical artifact is not an owned regular file")
            result[name] = dict(summary(path), identity={"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid})
        return result

    def check_canonical(self):
        if self.canonical_snapshot() != self.state["canonical"]:
            raise TrialError("canonical artifact changed; trial invalid")

    def deadline(self, name):
        return self.clock.now() + self.state["limits"]["deadlines_seconds"][name]

    def no_mounts(self):
        for mount in self.mounts.snapshot():
            target = Path(mount["target"])
            if target == self.workspace or self.workspace in target.parents or target == self.artifacts or self.artifacts in target.parents:
                raise TrialError(f"unexpected/nested mount; cleanup refused: {target}")

    def expected_mount(self):
        matches = [m for m in self.mounts.snapshot() if Path(m["target"]) == self.root]
        if len(matches) != 1:
            return None
        found = matches[0]
        if any(found.get(k) != v for k, v in {"source": "m0-" + self.anchor["trial_id"],
                                             "fstype": "fuse.myfs", "root": "/"}.items()):
            raise TrialError("unexpected mount identity")
        for other in self.mounts.snapshot():
            target = Path(other["target"])
            if target != self.root and (self.workspace == target or self.workspace in target.parents):
                raise TrialError("unexpected/nested mount in workspace")
        if self.mount_identity is not None and found != self.mount_identity:
            raise TrialError("mount identity changed")
        return found

    def mount(self, readiness_deadline=None):
        self.no_mounts()
        self.check_paths()
        executable = Path(self.state["daemon"]["path"])
        info = executable.stat()
        if (info.st_dev != self.state["daemon"]["device"] or info.st_ino != self.state["daemon"]["inode"]
                or summary(executable)["sha256"] != self.state["daemon"]["sha256"]):
            raise TrialError("daemon executable identity changed")
        command = self.mounts.command(executable, self.root, self.backing, "m0-" + self.anchor["trial_id"])
        index = len(self.state["mounts"])
        self.state["commands"].append(command)
        self.daemon_identity = self.processes.launch(command, self.artifacts / f"daemon-{index}.log",
                                                     self.state["limits"]["log_bytes_per_child"])
        entry = {"daemon": self.daemon_identity, "mount": None, "unmounted": False,
                 "daemon_exit_status": None, "daemon_exit_reason": "exit not yet observed",
                 "evidence": self.state["evidence"]}
        self.state["mounts"].append(entry)
        self.save()
        deadline = self.deadline("readiness") if readiness_deadline is None else readiness_deadline
        while self.clock.now() < deadline:
            if self.processes.poll(self.daemon_identity) is not None:
                raise TrialError("daemon exit before readiness")
            found = self.expected_mount()
            if found is not None:
                self.mount_identity = found
                entry["mount"] = found
                result = self.mounts.probe(self.root, self.processes, deadline,
                                           self.artifacts / f"readiness-{index}.log",
                                           self.state["limits"]["log_bytes_per_child"])
                if result["exit_status"] != 0 or self.clock.now() >= deadline:
                    raise TrialError("readiness functional probe failed/timeout")
                if self.expected_mount() != found or self.processes.poll(self.daemon_identity) is not None:
                    raise TrialError("mount/daemon changed during readiness")
                entry["probe"] = result
                return
            self.clock.sleep(min(0.02, deadline - self.clock.now()))
        raise TrialError("readiness timeout; directory existence is insufficient")

    def unmount(self):
        if (self.daemon_identity is None or not self.processes.owned(self.daemon_identity)
                or self.mount_identity is None or self.expected_mount() != self.mount_identity):
            raise TrialError("unmount refused: mount/process identity unconfirmed")
        entry = self.state["mounts"][-1]
        deadline = self.deadline("shutdown")
        result = self.mounts.unmount(self.root, self.processes, deadline,
                                     self.artifacts / f"unmount-{len(self.state['mounts']) - 1}.log",
                                     self.state["limits"]["log_bytes_per_child"])
        entry["unmount_result"] = result
        if result["exit_status"] != 0:
            raise TrialError("unmount failed")
        self.no_mounts()
        entry["unmounted"] = True
        code = self.processes.wait(self.daemon_identity, deadline)
        entry["daemon_exit_status"] = code
        entry["daemon_exit_reason"] = None
        if self.clock.now() >= deadline:
            raise TrialError("shutdown timeout")
        if code != 0:
            raise TrialError(f"abnormal daemon exit: {code}")
        self.daemon_identity, self.mount_identity = None, None
        self.check_paths()

    def io(self, kind):
        self.check_canonical()
        index = len(self.state["validations"])
        result_path = self.artifacts / f"validation-{index}.json"
        command = [sys.executable, "-B", "-c", IO_CODE, str(Path(__file__).parent), kind,
                   str(self.canonical_directory), str(self.root), str(result_path)]
        self.state["commands"].append(command)
        deadline = self.deadline("io")
        result = self.processes.run(command, self.artifacts / f"verification-{index}.log",
                                     self.state["limits"]["log_bytes_per_child"], deadline)
        if self.clock.now() >= deadline:
            raise TrialError(f"{kind} timeout")
        if result["exit_status"] != 0:
            raise TrialError(f"{kind} child exit: {result['exit_status']}")
        validation = dict(read_json(result_path), phase=kind, evidence=self.state["evidence"])
        self.state["validations"].append(validation)
        self.verification_log(validation)
        self.check_canonical()
        if not all(v["passed"] for k, v in validation.items() if k in ("input", "output")):
            raise TrialError("canonical validation failed; staged input cannot define the reference")
        return validation

    def application(self):
        command = [sys.executable, "-B", str(APPLICATION), str(self.root)]
        self.state["commands"].append(command)
        deadline = self.deadline("application")
        result = self.processes.run(command, self.artifacts / "application.log",
                                     self.state["limits"]["log_bytes_per_child"], deadline)
        self.state["application_result"] = result
        if self.clock.now() >= deadline:
            raise TrialError("application timeout")
        if result["exit_status"] != 0:
            raise TrialError(f"application exit: {result['exit_status']}; partial output unvalidated")

    def execute(self):
        if self.state["outcome"] != "prepared":
            return False  # Never rerun or silently replace existing output.
        self.state["outcome"] = "running"
        self.save()
        try:
            self.no_mounts()
            self.check_paths()
            self.check_canonical()
            self.state["source_at_run"] = source_evidence()
            if self.state["backend"] == "myfs":
                self.mounts.preflight()
                for name, action in (("preparation_mount", self.mount), ("stage", lambda: self.io("stage")),
                                     ("preparation_unmount", self.unmount), ("application_mount", self.mount),
                                     ("application", self.application), ("verify", lambda: self.io("verify")),
                                     ("application_unmount", self.unmount), ("verification_mount", self.mount),
                                     ("remount_verify", lambda: self.io("verify")), ("final_unmount", self.unmount)):
                    self.phase(name, action)
            else:
                for name, action in (("stage", lambda: self.io("stage")), ("application", self.application),
                                     ("verify", lambda: self.io("verify"))):
                    self.phase(name, action)
            self.check_canonical()
            self.state["outcome"] = "passed"
            self.save()
            return True
        except (OSError, ValueError, TrialError) as error:
            self.fail(error)
            # Failure never automatically unmounts, signals a daemon, or deletes evidence.
            return False

    def verify(self):
        try:
            self.no_mounts()
            self.check_paths()
            self.check_canonical()
            if self.state["outcome"] != "passed":
                raise TrialError("verification refused: trial is not a successful completed run")
            if self.state["backend"] == "offline-directory" and self.state["outcome"] == "passed":
                self.phase("offline_reverify", lambda: self.io("verify"))
            else:
                self.verification_log({"canonical_passed": True, "application": None,
                                       "reason": "standalone command does not mount; application files not reverified"})
            return True
        except (OSError, ValueError, TrialError) as error:
            self.fail(error)
            return False

    def cleanup(self):
        self.no_mounts()
        self.check_paths()
        self.check_canonical()
        if self.state["outcome"] != "passed":
            raise TrialError("cleanup refused: trial not passed; retain failed-run evidence")
        if any(c.get("exit_status") is None for c in self.state["children"]):
            raise TrialError("cleanup refused: child exit unconfirmed; persisted PIDs are not adopted")
        if any(not m["unmounted"] or m["daemon_exit_status"] != 0 for m in self.state["mounts"]):
            raise TrialError("cleanup refused: unmount/normal daemon exit unconfirmed")
        # Only a derived UUID sibling, after mount absence + recorded identities.
        # fd-relative deletion prevents an ancestor path substitution.
        fd = os.open(self.artifacts.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            info = os.fstat(fd)
            if {"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid} != self.anchor["parent_identity"]:
                raise TrialError("cleanup parent identity changed")
            self.check_paths()
            self.no_mounts()
            remove_owned_tree(fd, self.workspace.name, self.anchor["identities"]["workspace"], self.no_mounts)
        finally:
            os.close(fd)
        self.state["outcome"] = "cleaned"
        self.save()


MEASUREMENT_SCHEMA = "myfs-m0-measurement-v1"


def metric(value, unit, source, population, interval, evidence, reason=None):
    """All observations carry units, population, boundaries and evidence kind."""
    return {"value": value, "unit": unit, "source": source, "population": population,
            "interval": interval, "evidence": evidence, "reason": reason}


def resource_metrics(usage, evidence, identity, start, end):
    population = {"owned_child": identity, "includes": "all threads; no worker CPU added again"}
    fields = (("user_seconds", "ru_utime", 1, "seconds"),
              ("system_seconds", "ru_stime", 1, "seconds"),
              ("peak_rss_bytes", "ru_maxrss", 1024, "bytes"),
              ("voluntary_context_switches", "ru_nvcsw", 1, "count"),
              ("involuntary_context_switches", "ru_nivcsw", 1, "count"),
              ("major_faults", "ru_majflt", 1, "count"),
              ("minor_faults", "ru_minflt", 1, "count"),
              ("block_inputs", "ru_inblock", 1, "kernel block operations"),
              ("block_outputs", "ru_oublock", 1, "kernel block operations"))
    return {name: metric(getattr(usage, field) * factor if usage is not None else None,
                         unit, "wait4." + field + (" Linux KiB * 1024" if factor == 1024 else ""),
                         population, [start, end], evidence,
                         None if usage is not None else "resource usage unavailable from adapter")
            for name, field, factor, unit in fields}


class AccountingProcesses(Processes):
    """One reaper: pidfd readiness -> waitid(WNOWAIT) -> exactly one wait4.

    Popen.poll/wait never reap these children. returncode is assigned after our
    wait4 so Popen destruction cannot become a second reaper. /proc snapshots
    are best effort, not instantaneous limits or native hotspot evidence.
    """

    def launch(self, *args):
        if any(not hasattr(os, name) for name in ("wait4", "waitid", "P_PIDFD", "WNOWAIT")):
            raise TrialError("pidfd waitid/wait4 accounting unavailable; refusing launch")
        return super().launch(*args)

    def counters(self, identity):
        at = self.clock.now()
        try:
            if not self.owned(identity) or self.handles[identity["pid"]]["closed"]:
                raise TrialError("counter endpoint missed: owned process already reaped")
            base = Path(f"/proc/{identity['pid']}")
            values = {}
            errors = {}
            try:
                raw_stat = (base / "stat").read_text()
                fields = raw_stat[raw_stat.rfind(")") + 2:].split()
                ticks = os.sysconf("SC_CLK_TCK")
                for name, index in (("user_seconds", 11), ("system_seconds", 12)):
                    values[name] = metric(int(fields[index]) / ticks, "seconds", "/proc/PID/stat ticks / SC_CLK_TCK",
                                          {"owned_process": identity, "scope": "thread-group CPU including workers"},
                                          [at, at], self.evidence)
            except (OSError, ValueError) as error:
                errors["stat"] = str(error)
            for filename, keys, unit in (
                    ("io", ("rchar", "wchar", "read_bytes", "write_bytes", "cancelled_write_bytes"), "bytes"),
                    ("status", ("VmRSS", "VmHWM", "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches"), None)):
                try:
                    raw = dict(line.split(":", 1) for line in (base / filename).read_text().splitlines() if ":" in line)
                    for key in keys:
                        tokens = raw.get(key, "").split()
                        value = int(tokens[0]) if tokens else None
                        memory = key.startswith("Vm")
                        values[key] = metric(value * 1024 if memory and value is not None else value,
                                             "bytes" if memory else unit or "count", f"/proc/PID/{filename}:{key}",
                                             {"owned_process": identity,
                                              "scope": "thread-group memory/I/O; status switches are leader only"},
                                             [at, at], self.evidence, None if tokens else "field absent (possibly exited)")
                except (OSError, ValueError) as error:
                    errors[filename] = str(error)
            if process_identity(identity["pid"]) != identity:
                raise TrialError("identity changed during counter observation")
            return {"value": values or None, "reason": None if values else "all /proc endpoints unavailable",
                    "errors": errors, "monotonic": at, "evidence": self.evidence,
                    "source": "identity-checked /proc; missing fields never zero"}
        except (OSError, TrialError) as error:
            return {"value": None, "reason": str(error), "monotonic": at, "evidence": self.evidence,
                    "source": "identity-checked /proc"}

    def poll(self, identity):
        if not self.owned(identity):
            raise TrialError("process identity cannot be established; retaining evidence")
        handle = self.handles[identity["pid"]]
        entry = handle["entry"]
        if handle["closed"]:
            return entry["exit_status"]
        self._drain(handle)
        observed = os.waitid(os.P_PIDFD, handle["pidfd"], os.WEXITED | os.WNOHANG | os.WNOWAIT)
        if observed is not None:
            endpoint = self.counters(identity)  # Zombie remains ours until wait4.
            pid, status, usage = os.wait4(identity["pid"], 0)
            completed = self.clock.now()  # Before log draining, JSON or verification.
            if pid != identity["pid"]:
                raise TrialError("unexpected child reaped")
            code = os.waitstatus_to_exitcode(status)
            handle["child"].returncode = code
            entry.update(exit_status=code, exit_status_reason=None, exit_monotonic=completed,
                         resources=resource_metrics(usage, self.evidence, identity,
                                                    entry["launch_monotonic"], completed),
                         endpoint_counters=endpoint, reap_count=1)
            self._drain(handle)
            handle["child"].stdout.close()
            handle["log"].close()
            os.close(handle["pidfd"])
            handle["closed"] = True
            self.on_change()
        if entry["log_truncated"]:
            raise TrialError("child log limit exceeded")
        return entry["exit_status"]

    def wait(self, identity, deadline):
        while True:
            code = self.poll(identity)
            if code is not None:
                if self.handles[identity["pid"]]["entry"]["exit_monotonic"] >= deadline:
                    raise TrialError("child/shutdown timeout; completion deadline exceeded")
                return code
            if self.clock.now() >= deadline:
                raise TrialError("child/shutdown timeout; completion deadline exceeded")
            # A live daemon can otherwise block writing to its PIPE while its
            # FUSE client is awaited. Service all owned children with the same
            # single reaper; never supervise an unowned process.
            for handle in list(self.handles.values()):
                if not handle["closed"] and handle["entry"]["identity"] != identity:
                    self.poll(handle["entry"]["identity"])
            with selectors.DefaultSelector() as ready:
                for handle in self.handles.values():
                    if not handle["closed"]:
                        ready.register(handle["pidfd"], selectors.EVENT_READ)
                        if not handle.get("pipe_eof"):
                            ready.register(handle["child"].stdout, selectors.EVENT_READ)
                ready.select(max(0, deadline - self.clock.now()))


class MeasurementMounts(Mounts):
    """Measurement probes stat the root only. Smoke's mutating probe is unchanged."""

    def __init__(self, affinity=None):
        self.affinity = affinity

    def command(self, *args):
        return placed_command(super().command(*args), self.affinity)

    def probe(self, root, processes, deadline, log, limit):
        code = ("import os,stat,sys; mode=os.stat(sys.argv[1]).st_mode; "
                "sys.exit(0 if stat.S_ISDIR(mode) else 1)")
        result = processes.run([sys.executable, "-B", "-c", code, str(root)], log, limit, deadline)
        return dict(result, kind="non-mutating root stat; no input read; no write-path claim")


def placed_command(command, affinity):
    if affinity is None:
        return command
    executable = shutil.which("taskset")
    if not executable:
        raise TrialError("configured affinity requires installed taskset")
    return [executable, "-c", ",".join(map(str, affinity)), *command]


class MeasurementHost:
    evidence = "actual"

    def __init__(self):
        # Namespace inode numbers and mount IDs can both be recycled after
        # namespace destruction. Pin comparisons to this observer lifetime.
        self.observation_context = uuid.uuid4().hex

    def device(self, path):
        path = strict_path(path)
        candidates = [m for m in Mounts().snapshot()
                      if Path(m["target"]) == path or Path(m["target"]) in path.parents]
        if not candidates:
            raise TrialError("backing filesystem identity unavailable")
        found = max(candidates, key=lambda m: len(Path(m["target"]).parts))
        device = path.stat().st_dev
        return dict(found, device=f"{os.major(device)}:{os.minor(device)}",
                    mount_namespace=str(Path("/proc/self/ns/mnt").readlink()),
                    observation_context=self.observation_context)

    def resources(self, path):
        fs = os.statvfs(path)
        memory = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())
        return {"available_disk_bytes": fs.f_bavail * fs.f_frsize,
                "available_memory_bytes": int(memory["MemAvailable"].split()[0]) * 1024,
                "source": "statvfs.f_bavail*f_frsize; /proc/meminfo:MemAvailable",
                "evidence": self.evidence}

    def metadata(self):
        return {"kernel": os.uname().release, "python": sys.version,
                "affinity": sorted(os.sched_getaffinity(0)), "evidence": self.evidence,
                "observer": process_identity(os.getpid()), "observation_context": self.observation_context}

    def affinity(self, identity, requested):
        try:
            if process_identity(identity["pid"]) != identity:
                raise TrialError("affinity process identity mismatch")
            masks = {p.name: sorted(os.sched_getaffinity(int(p.name)))
                     for p in Path(f"/proc/{identity['pid']}/task").iterdir()}
            if process_identity(identity["pid"]) != identity:
                raise TrialError("affinity endpoint missed")
            return {"value": masks, "reason": None, "evidence": self.evidence,
                    "matches": all(v == requested for v in masks.values())}
        except (OSError, TrialError) as error:
            return {"value": None, "reason": str(error), "evidence": self.evidence, "matches": False}


def executable_identity(path):
    path = strict_path(path)
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or not os.access(path, os.X_OK):
        raise TrialError("not an executable regular file")
    return {"path": str(path), "device": info.st_dev, "inode": info.st_ino,
            "sha256": summary(path)["sha256"]}


def measurement_source():
    evidence = source_evidence()
    evidence["production_files"] = {str(p.relative_to(REPOSITORY)): summary(p)["sha256"]
                                    for p in sorted((REPOSITORY / "src").rglob("*"))
                                    if p.suffix in (".c", ".h")}
    evidence["production_files"]["Makefile"] = summary(REPOSITORY / "Makefile")["sha256"]
    evidence["interpreter"] = executable_identity(Path(sys.executable).resolve())
    return evidence


def account_owned_tree(root, expected, deadline, clock=None, materialize=False):
    """Bounded FD-relative no-follow post-exit accounting, unique (dev, inode).

    Includes directory/symlink blocks, all generation files and unknown files.
    Generation bytes are a subset, never added to total again. A maximum of
    4096 named entries and the caller's deadline bound traversal. Optional
    fsync applies only to pinned, owned regular files/directories in this tree;
    it is untimed materialization, not a claim of application crash durability.
    """
    clock = clock or Clock()
    root = strict_path(root)
    if directory_identity(root) != expected:
        raise TrialError("accounting directory identity changed")
    objects, entries, aliases, named = {}, 0, 0, {}
    flushed, materialization_names = set(), set()
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
    root_fd = os.open(root, flags | os.O_DIRECTORY)
    root_mount = fd_mount_id(root_fd)
    def check():
        if clock.now() >= deadline:
            raise TrialError("accounting timeout")
        if entries > 4096:
            raise TrialError("accounting entry limit exceeded")
    def identity(info):
        return info.st_dev, info.st_ino, info.st_uid, stat.S_IFMT(info.st_mode)
    def remember(info, name):
        nonlocal aliases
        key = (info.st_dev, info.st_ino)
        generation = bool(re.search(r"\.g\.[0-9a-f]{32}(?:/|\.owner$|$)", name))
        if key in objects:
            aliases += 1
            objects[key]["generation"] |= generation
            objects[key]["seen_names"] += 1
            return False
        regular = stat.S_ISREG(info.st_mode)
        category = ("metadata" if name.endswith((".meta", ".owner")) or name.endswith("/meta")
                    else "payload" if name.endswith(".data") or name.endswith("/data")
                    else "logical_csv" if name in ("input.csv", "output.csv")
                    else "directories" if stat.S_ISDIR(info.st_mode)
                    else "symlinks" if stat.S_ISLNK(info.st_mode) else "unclassified")
        objects[key] = {"allocated": info.st_blocks * 512, "apparent": info.st_size if regular else 0,
                        "regular": regular, "directory": stat.S_ISDIR(info.st_mode),
                        "symlink": stat.S_ISLNK(info.st_mode), "category": category, "generation": generation,
                        "links": info.st_nlink, "seen_names": 1}
        return True
    def walk(fd, relative, perform=False):
        nonlocal entries
        check()
        for name in os.listdir(fd):
            entries += 1
            check()
            before = os.stat(name, dir_fd=fd, follow_symlinks=False)
            if before.st_dev != expected["device"] or before.st_uid != os.getuid():
                raise TrialError("accounting unowned/cross-device object")
            label = relative + name
            key = before.st_dev, before.st_ino
            if perform:
                if named.get(label) != identity(before) or key not in objects:
                    raise TrialError("accounting tree changed before materialization")
                if stat.S_ISREG(before.st_mode) and before.st_nlink != objects[key]["seen_names"]:
                    raise TrialError("materialization hardlink outside owned tree")
                materialization_names.add(label)
            else:
                remember(before, label)
                named[label] = identity(before)
            if stat.S_ISLNK(before.st_mode):
                continue  # Count the link itself, never its target.
            if not stat.S_ISREG(before.st_mode) and not stat.S_ISDIR(before.st_mode):
                raise TrialError("accounting unsupported object type")
            child = os.open(name, flags | (os.O_DIRECTORY if stat.S_ISDIR(before.st_mode) else 0), dir_fd=fd)
            try:
                if identity(before) != identity(os.fstat(child)) or fd_mount_id(child) != root_mount:
                    raise TrialError("accounting substituted object or nested mount")
                if stat.S_ISDIR(before.st_mode):
                    walk(child, label + "/", perform)
                if perform and key not in flushed:
                    check()
                    os.fsync(child)
                    flushed.add(key)
                current = os.fstat(child)
                if identity(current) != identity(os.stat(name, dir_fd=fd, follow_symlinks=False)):
                    raise TrialError("accounting path substituted")
                objects[(current.st_dev, current.st_ino)]["allocated"] = current.st_blocks * 512
            finally:
                os.close(child)
    try:
        info = os.fstat(root_fd)
        if {"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid} != expected:
            raise TrialError("accounting root substituted")
        remember(info, "")
        walk(root_fd, "")
        if materialize:
            if any(v["regular"] and v["links"] != v["seen_names"] for v in objects.values()):
                raise TrialError("materialization hardlink outside owned tree")
            walk(root_fd, "", True)
            if materialization_names != set(named):
                raise TrialError("accounting tree changed during materialization")
            check()
            os.fsync(root_fd)
        objects[(info.st_dev, info.st_ino)]["allocated"] = os.fstat(root_fd).st_blocks * 512
        if directory_identity(strict_path(root)) != expected:
            raise TrialError("accounting root path changed")
        check()
    finally:
        os.close(root_fd)
    values = list(objects.values())
    return {"regular_apparent_bytes": sum(v["apparent"] for v in values),
            "allocated_bytes": sum(v["allocated"] for v in values),
            "unique_regular_files": sum(v["regular"] for v in values),
            "directories": sum(v["directory"] for v in values), "symlinks": sum(v["symlink"] for v in values),
            "hardlink_aliases": aliases,
            "outside_tree_hardlink_objects": sum(v["regular"] and v["links"] != v["seen_names"] for v in values),
            "category_allocated_bytes": {c: sum(v["allocated"] for v in values if v["category"] == c)
                                         for c in sorted({v["category"] for v in values})},
            "generation_allocated_bytes_subset": sum(v["allocated"] for v in values if v["generation"]),
            "errors": [], "exclusions": ["symlink targets", "filesystem-wide journal/superblock allocation"],
            "materialized_owned_files": materialize,
            "limitation": "st_blocks snapshot, not a storage peak; delayed allocation may remain" if not materialize
                          else "owned-file fsync is separate from application completion and does not prove crash durability"}


ACCOUNT_CODE = ("import json,sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
                "import m0_profile as m; root,identity,result,materialize=sys.argv[2:]; "
                "value=m.account_owned_tree(Path(root),json.loads(identity),m.time.monotonic()+float(sys.argv[0]),"
                "materialize=materialize=='1'); m.exclusive_json(Path(result),value)")


class MeasurementTrial(Trial):
    """Opt-in, one-use staged-directory trial; never a multi-run campaign.

    Canonical references reside in a private UUID tree on a different filesystem
    for actual measurements. They and artifact logs survive guarded workspace
    cleanup. First-access-after-staging is a cache history, NOT equivalent
    residency: ext4 staging validation can warm input; myfs uses fresh FUSE.
    """
    schema = MEASUREMENT_SCHEMA

    def __init__(self, artifacts, anchor, state, processes=None, mounts=None, clock=None, host=None):
        clock = clock or Clock()
        super().__init__(artifacts, anchor, state, processes or AccountingProcesses(clock),
                         mounts or MeasurementMounts(state.get("affinities", {}).get("daemon")), clock)
        self.host = host or MeasurementHost()

    @property
    def canonical_directory(self):
        return Path(self.anchor["canonical_parent"]) / ("m0-canonical-" + self.anchor["trial_id"])

    @classmethod
    def prepare(cls, parent, *, canonical_parent, backend, records=MAX_RECORDS,
                seed=fixture.DEFAULT_SEED, expected_device=None, daemon=None, build_provenance=None,
                application_affinity=None, daemon_affinity=None, materialize=False,
                processes=None, mounts=None, host=None, clock=None,
                readiness_seconds=10, application_seconds=30, shutdown_seconds=30,
                io_seconds=30, log_bytes=1024**2):
        deadlines = {"readiness": readiness_seconds, "application": application_seconds,
                     "shutdown": shutdown_seconds, "io": io_seconds}
        if backend not in ("ext4", "myfs", "offline-check") or not 0 <= records <= MAX_RECORDS:
            raise TrialError("invalid measurement backend or record limit (0..32768)")
        if any(not 0 < v <= 300 for v in deadlines.values()) or not 1024 <= log_bytes <= 1024**2:
            raise TrialError("invalid measurement deadline/log limit")
        if backend != "offline-check" and not expected_device:
            raise TrialError("measurement requires explicit expected backing device major:minor")
        if backend == "myfs" and (daemon is None or build_provenance is None):
            raise TrialError("myfs measurement requires daemon and build provenance JSON")
        parent, canonical_parent = strict_path(parent), strict_path(canonical_parent)
        # Ownership is checked before creating any directory, unlike adopting existing roots.
        parent_identity, canonical_parent_identity = directory_identity(parent), directory_identity(canonical_parent)
        trial_id = uuid.uuid4().hex
        artifacts, workspace = parent / ("m0-artifacts-" + trial_id), parent / ("m0-work-" + trial_id)
        canonical = canonical_parent / ("m0-canonical-" + trial_id)
        for path in (artifacts, workspace, workspace / "application", workspace / "backing", canonical):
            path.mkdir(mode=0o700)
        anchor = {"schema": cls.schema, "trial_id": trial_id, "parent": str(parent),
                  "parent_identity": parent_identity, "canonical_parent": str(canonical_parent),
                  "canonical_parent_identity": canonical_parent_identity,
                  "canonical_identity": directory_identity(canonical),
                  "identities": {name: directory_identity(path) for name, path in (
                      ("artifacts", artifacts), ("workspace", workspace),
                      ("application", workspace / "application"), ("backing", workspace / "backing"))}}
        for path in (artifacts, workspace, canonical):
            exclusive_json(path / "owner.json", anchor)
        state = {"schema": cls.schema, "trial_id": trial_id, "anchor": anchor, "backend": backend,
                 "label": "OFFLINE correctness and artifact check" if backend == "offline-check" else "single staged-directory measurement",
                 "roots": {"artifacts": str(artifacts), "workspace": str(workspace),
                           "application": str(workspace / "application"), "backing": str(workspace / "backing")},
                 "canonical_root": str(canonical), "expected_device": expected_device,
                 "limits": {"records": records, "deadlines_seconds": deadlines, "log_bytes_per_child": log_bytes,
                            "helper_termination_grace_seconds": 2, "accounting_entries": 4096},
                 "seed": seed, "source": measurement_source(), "daemon": None, "canonical": {},
                 "phases": [], "validations": [], "children": [], "mounts": [], "commands": [],
                 "logical": None, "outcome": "created", "error": None,
                 "affinities": {"application": application_affinity, "daemon": daemon_affinity},
                 "materialize": materialize, "measurements": {"application": {}, "daemon_lifecycles": []},
                 "counter_observations": [], "warnings": [], "resource_observations": [],
                 "storage": {"allocated_bytes": None, "reason": "post-exit accounting not completed"},
                 "retention": {"artifacts": "retained", "canonical": "retained", "workspace": "retained"},
                 "completion_contract": "buffered application-level child completion; no durability equivalence or crash-durability proof",
                 "cache_history": {"cohort": "first-access-after-staging", "staging": "copy then exact canonical comparison",
                     "fresh_fuse_connection": backend == "myfs", "input_residency": {"value": None, "reason": "not measured"},
                     "known_myfs_settings": "-f; fsname/subtype; source sets kernel_cache=1; no cache controls",
                     "equivalent_residency_proven": False,
                     "limitation": "ext4 input may remain warm after validation; fresh myfs FUSE has different history; not isolated FUSE/decompression overhead"},
                 "unavailable": {name: {"value": None, "reason": reason} for name, reason in (
                     ("effective_resource_limits", "not measured; no RLIMIT changes"),
                     ("native_hotspots", "no native profiling; counters cannot locate hotspots"),
                     ("compression_savings", "not calculated; filtering/schema changes are not compression"),
                     ("storage_peak", "no continuous accounting"), ("crash_durability", "not tested"))}}
        trial = cls(artifacts, anchor, state, processes, mounts, clock, host)
        state["evidence"] = "mocked" if any(a.evidence == "mocked" for a in (trial.processes, trial.mounts, trial.host)) else (
            "actual offline execution" if backend == "offline-check" else "actual")
        trial.save()
        try:
            def preflight():
                trial.no_mounts()
                trial.check_paths()
                state["host"] = trial.host.metadata()
                state["device"] = trial.host.device(trial.workspace)
                state["canonical_device"] = trial.host.device(trial.canonical_directory)
                trial.check_device()
                trial.resource_guard()
                for role, mask in state["affinities"].items():
                    if mask is not None and (not mask or len(set(mask)) != len(mask)
                                             or not set(mask) <= set(state["host"]["affinity"])):
                        raise TrialError(f"invalid/unavailable {role} affinity")
                if backend == "myfs":
                    state["daemon"] = executable_identity(daemon)
                    provenance = build_provenance if isinstance(build_provenance, dict) else read_json(strict_path(build_provenance))
                    if all(a.evidence == "mocked" for a in (trial.processes, trial.mounts, trial.host)) and provenance == {"mocked": True}:
                        state["daemon"]["build_provenance"] = provenance
                    else:
                        before, after = provenance.get("source_fingerprints_before"), provenance.get("source_fingerprints_after")
                        actual = state["source"]["production_files"]
                        if (provenance.get("schema") != "m0-smoke-build-provenance-v1" or provenance.get("exit_status") != 0
                                or before != actual or after != actual
                                or any(provenance.get("daemon", {}).get(k) != v for k, v in state["daemon"].items())):
                            raise TrialError("executable build provenance/source mismatch")
                        state["daemon"]["build_provenance"] = provenance
                    trial.mounts.preflight()
            trial.phase("preflight", preflight)
            def generate():
                trial.resource_guard()
                manifest = fixture.generate_csv(canonical / "input.csv", canonical / "input.manifest.json", records, seed)
                expected = fixture.write_expected(canonical / "input.csv", canonical / "expected.csv")
                state["canonical"] = trial.canonical_snapshot()
                state["fixture_manifest"] = manifest
                state["logical"] = {"input_rows": records, "input_bytes": manifest["byte_count"],
                                    "output_rows": expected["row_count"], "output_bytes": expected["byte_count"],
                                    "selectivity": expected["row_count"] / records if records else None,
                                    "selectivity_reason": None if records else "zero input rows",
                                    "meaning": "row filtering and schema reduction, not filesystem compression"}
                state["workload"] = {"generator": manifest, "application": str(APPLICATION),
                                     "buffer_bytes": fixture.BUFFER_BYTES, "arguments": ["ROOT"],
                                     "interpreter": state["source"]["interpreter"]}
            trial.phase("canonical_preparation", generate)
            state["outcome"] = "prepared"
            trial.save()
        except (OSError, ValueError, TrialError) as error:
            trial.fail(error)
        return trial

    def check_paths(self, mounted=False):
        super().check_paths(mounted)
        parent = strict_path(self.anchor["canonical_parent"])
        if (directory_identity(parent) != self.anchor["canonical_parent_identity"]
                or str(self.canonical_directory) != self.state["canonical_root"]
                or directory_identity(strict_path(self.canonical_directory)) != self.anchor["canonical_identity"]
                or read_json(self.canonical_directory / "owner.json") != self.anchor
                or self.workspace == self.canonical_directory or self.workspace in self.canonical_directory.parents):
            raise TrialError("canonical ownership/path identity mismatch")

    def no_mounts(self):
        super().no_mounts()
        for mount in self.mounts.snapshot():
            target = Path(mount["target"])
            if target == self.canonical_directory or self.canonical_directory in target.parents:
                raise TrialError("unexpected mount over canonical references")

    def check_device(self):
        self.check_paths(mounted=self.mount_identity is not None)
        if self.mount_identity is not None and self.expected_mount() != self.mount_identity:
            raise TrialError("mounted filesystem identity changed")
        current = self.host.device(self.workspace)
        canonical = self.host.device(self.canonical_directory)
        for field, observed in (("device", current), ("canonical_device", canonical)):
            recorded = self.state[field]
            namespace_changed = recorded.get("mount_namespace") != observed.get("mount_namespace")
            context_changed = recorded.get("observation_context") != observed.get("observation_context")
            # Mount IDs are namespace/observer-local. Across observer contexts require
            # the same filesystem/device mapping AND the pinned path identities
            # (check_paths), while still checking all current unexpected mounts.
            # FUSE connection IDs/source tokens remain exact and invocation-local.
            stable = ("device", "fstype", "source", "root", "target")
            previous = next((x["observed"] for x in self.state.get("device_observations", [])
                             if x["field"] == field
                             and x["observed"].get("mount_namespace") == observed.get("mount_namespace")
                             and x["observed"].get("observation_context") == observed.get("observation_context")), None)
            if (any(recorded.get(k) != observed.get(k) for k in stable)
                    or (not namespace_changed and not context_changed and recorded.get("mount_id") != observed.get("mount_id"))
                    or (previous is not None and previous.get("mount_id") != observed.get("mount_id"))
                    or (context_changed and (not recorded.get("observation_context") or not observed.get("observation_context")))
                    or (namespace_changed and (not recorded.get("mount_namespace") or not observed.get("mount_namespace")))):
                raise TrialError("device/filesystem identity changed")
            if (namespace_changed or context_changed) and previous is None:
                self.state.setdefault("device_observations", []).append({"field": field, "observed": observed,
                    "monotonic": self.clock.now(), "reason": "observer-local mount ID; stable device mapping and owned path identities checked"})
        if self.state["backend"] != "offline-check":
            if current["fstype"] != "ext4" or current["device"] != self.state["expected_device"]:
                raise TrialError("expected ext4 backing device mismatch")
            if canonical["device"] == current["device"]:
                raise TrialError("canonical references must be outside the tested filesystem/device")

    def resource_guard(self):
        required_disk = 1024**3 + 64 * 1024**2 + 8 * (69 + self.state["limits"]["records"] * 128)
        for path in (self.workspace, self.canonical_directory):
            observation = dict(self.host.resources(path), path=str(path), monotonic=self.clock.now(),
                               required_disk_bytes=required_disk, required_memory_bytes=2 * 1024**3)
            self.state["resource_observations"].append(observation)
            if observation["available_disk_bytes"] < required_disk:
                raise TrialError("insufficient user-accessible disk space")
            if observation["available_memory_bytes"] < 2 * 1024**3:
                raise TrialError("insufficient available memory")

    def phase(self, name, action):
        self.observe_daemon(name + ":start")
        try:
            return super().phase(name, action)
        finally:
            phase = self.state["phases"][-1]
            phase["duration_seconds"] = phase["end_monotonic"] - phase["start_monotonic"]
            self.observe_daemon(name + ":end")
            self.save()

    def not_applicable(self, name, reason):
        self.state["phases"].append({"name": name, "outcome": "not_applicable", "start_monotonic": None,
                                     "end_monotonic": None, "duration_seconds": None, "reason": reason,
                                     "error": None, "evidence": self.state["evidence"]})
        self.save()

    def observe_daemon(self, phase):
        if self.daemon_identity is not None:
            self.state["counter_observations"].append({"phase": phase, "identity": self.daemon_identity,
                                                      "counters": self.processes.counters(self.daemon_identity)})

    def check_affinity(self, identity, role, deadline=None):
        mask = self.state["affinities"][role]
        if mask is None:
            return {"value": None, "reason": "affinity not configured", "evidence": self.state["evidence"]}
        deadline = min(deadline, self.deadline("readiness")) if deadline is not None else self.deadline("readiness")
        while self.clock.now() < deadline:
            observation = self.host.affinity(identity, mask)
            if observation.get("value") is not None and observation.get("matches", True):
                return observation
            if self.processes.poll(identity) is not None:
                raise TrialError("configured affinity endpoint missed before child exit")
            self.clock.sleep(min(.005, deadline - self.clock.now()))
        raise TrialError("configured affinity not verified before deadline")

    def mount(self):
        deadline = self.deadline("readiness")
        self.check_device()
        self.resource_guard()
        super().mount(readiness_deadline=deadline)
        self.state["mounts"][-1]["affinity"] = self.check_affinity(self.daemon_identity, "daemon", deadline)
        if self.clock.now() >= deadline:
            raise TrialError("readiness/daemon affinity timeout")

    def unmount(self):
        identity = self.daemon_identity
        super().unmount()
        entry = next(e for e in self.processes.entries if e.get("identity") == identity)
        resources = entry.get("resources") or resource_metrics(None, self.state["evidence"], identity, None, None)
        self.state["measurements"]["daemon_lifecycles"].append(resources)
        self.record_daemon_warnings()
        if self.state["warnings"]:
            raise TrialError("unexplained daemon warning/error; trial invalid")

    def record_daemon_warnings(self):
        for index, mount in enumerate(self.state["mounts"]):
            # Derived paths only; never read a log path supplied by state JSON.
            log = self.artifacts / f"daemon-{index}.log"
            if not log.exists():
                continue
            strict_path(log)
            for number, line in enumerate(log.read_text(encoding="utf-8", errors="backslashreplace").splitlines(keepends=True)):
                if (not re.search(r"\[(?:WARN|ERROR)\]", line)
                        or any(w["log"] == str(log) and w["line_number"] == number for w in self.state["warnings"])):
                    continue
                self.state["warnings"].append({"line": line, "line_number": number, "log": str(log), "trial_id": self.anchor["trial_id"],
                    "daemon": mount["daemon"], "observed_monotonic": self.clock.now(),
                    "classification": "unexplained", "evidence": self.state["evidence"],
                    "timestamp_limitation": "monotonic observation timestamp; original LOG event timestamp retained verbatim"})

    def application(self):
        self.check_canonical()
        self.check_device()
        self.resource_guard()
        command = placed_command([sys.executable, "-B", str(APPLICATION), str(self.root)],
                                 self.state["affinities"]["application"])
        self.state["commands"].append(command)
        deadline = self.deadline("application")
        identity = self.processes.launch(command, self.artifacts / "application.log",
                                         self.state["limits"]["log_bytes_per_child"])
        self.state["application_identity"] = identity
        try:
            self.state["application_affinity_observation"] = self.check_affinity(identity, "application", deadline)
            self.state["application_start_counters"] = self.processes.counters(identity)
            code = self.processes.wait(identity, deadline)
        except TrialError:
            # A timed-out application may be terminated only through its owned
            # handle, never a persisted PID; the failed trial retains evidence.
            if self.processes.owned(identity) and self.processes.poll(identity) is None:
                self.processes.signal_owned(identity, signal.SIGTERM)
                try:
                    self.processes.wait(identity, self.clock.now() + 1)
                except TrialError:
                    pass  # No broad kill/recovery; unconfirmed child blocks cleanup.
            entry = next(e for e in self.processes.entries if e.get("identity") == identity)
            if entry.get("exit_monotonic") is not None:
                self.complete_application()
            raise
        self.state["application_result"] = {"exit_status": code, "identity": identity, "evidence": self.state["evidence"]}
        entry = next(e for e in self.processes.entries if e.get("identity") == identity)
        if entry["exit_monotonic"] >= deadline:
            raise TrialError("application timeout")
        if code != 0:
            self.complete_application()
            raise TrialError(f"application exit: {code}; partial output unvalidated")
        if self.daemon_identity is not None and (self.processes.poll(self.daemon_identity) is not None
                                                or self.expected_mount() != self.mount_identity):
            raise TrialError("daemon/mount changed during application execution")

    def complete_application(self):
        identity = self.state["application_identity"]
        entry = next(e for e in self.processes.entries if e.get("identity") == identity)
        start, end = entry["launch_monotonic"], entry["exit_monotonic"]
        values = entry.get("resources") or resource_metrics(None, self.state["evidence"], identity, start, end)
        offline = self.state["backend"] == "offline-check"
        values["wall_seconds"] = metric(None if offline else end - start, "seconds", "monotonic before Popen through owned wait4 completion",
                                         {"application_child": identity}, [start, end], self.state["evidence"],
                                         "offline correctness check, not a filesystem measurement" if offline else None)
        values["exit_status"] = metric(entry["exit_status"], "exit code", "owned child wait", identity, [start, end], self.state["evidence"])
        self.state["measurements"]["application"] = values
        self.state["application_end_counters"] = entry.get("endpoint_counters", {
            "value": None, "reason": "adapter did not capture pre-reap endpoint", "evidence": self.state["evidence"]})

    def storage_accounting(self):
        self.no_mounts()
        self.check_paths()
        self.check_device()
        if any(c.get("exit_status") is None for c in self.state["children"]):
            raise TrialError("post-exit accounting refused: child exit unconfirmed")
        if any(not m["unmounted"] or m["daemon_exit_status"] != 0 for m in self.state["mounts"]):
            raise TrialError("post-exit accounting refused: daemon/mount still active")
        self.confirm_exits()
        root = self.backing if self.state["backend"] == "myfs" else self.root
        result = self.artifacts / "storage.json"
        seconds = self.state["limits"]["deadlines_seconds"]["io"]
        # -c sets argv[0] to -c; pass a real relative deadline as an explicit argument.
        code = ACCOUNT_CODE.replace("float(sys.argv[0])", repr(seconds))
        command = [sys.executable, "-B", "-c", code, str(Path(__file__).parent), str(root),
                   json.dumps(self.anchor["identities"]["backing" if self.state["backend"] == "myfs" else "application"]),
                   str(result), "1" if self.state["materialize"] else "0"]
        self.state["commands"].append(command)
        deadline = self.deadline("io")
        observed = self.processes.run(command, self.artifacts / "storage.log",
                                       self.state["limits"]["log_bytes_per_child"], deadline)
        if observed["exit_status"] != 0 or self.clock.now() >= deadline:
            raise TrialError("post-exit accounting failed/timeout")
        data = read_json(result)
        self.state["storage"] = dict(data, evidence=self.state["evidence"], source="no-follow unique (device,inode) st_blocks*512",
                                      population=str(root), interval=[self.state["phases"][-1]["start_monotonic"], self.clock.now()],
                                      logical_input_bytes=self.state["logical"]["input_bytes"],
                                      logical_output_bytes=self.state["logical"]["output_bytes"])
        storage = self.state["storage"]
        storage["metrics"] = {name: metric(storage[name], "bytes" if "bytes" in name else "count",
                                            storage["source"], str(root), storage["interval"], self.state["evidence"])
                              for name in ("allocated_bytes", "regular_apparent_bytes", "unique_regular_files", "directories",
                                           "symlinks", "hardlink_aliases", "outside_tree_hardlink_objects", "generation_allocated_bytes_subset")}
        preparation = next(p for p in self.state["phases"] if p["name"] == "canonical_preparation")
        for name in ("logical_input_bytes", "logical_output_bytes"):
            storage["metrics"][name] = metric(storage[name], "bytes", "canonical manifest/independent oracle", "canonical CSV data",
                                               [preparation["start_monotonic"], preparation["end_monotonic"]], self.state["evidence"])
        for name, value in storage["category_allocated_bytes"].items():
            storage["metrics"][name + "_allocated_bytes"] = metric(value, "bytes", storage["source"], str(root),
                                                                     storage["interval"], self.state["evidence"])
        storage["metrics"]["filesystem_shared_metadata_bytes"] = metric(None, "bytes", "not collected", "filesystem journal/superblock",
            storage["interval"], self.state["evidence"], "cannot attribute shared filesystem metadata to this trial")

    def execute(self):
        if self.state["outcome"] != "prepared":
            return False
        self.state["outcome"] = "running"
        self.save()
        try:
            self.no_mounts()
            self.check_paths()
            self.check_canonical()
            self.check_device()
            current = measurement_source()
            self.state["source_at_run"] = current
            if any(current.get(k) != self.state["source"].get(k) for k in ("revision", "files", "production_files", "interpreter")):
                raise TrialError("source/interpreter fingerprint changed since preparation")
            self.resource_guard()
            mounted = self.state["backend"] == "myfs"
            if mounted:
                self.mounts.preflight()
                self.phase("preparation_readiness", self.mount)
            else:
                self.not_applicable("preparation_readiness", "no FUSE preparation mount")
            self.phase("input_staging_validation", lambda: self.io("stage"))
            if mounted:
                self.phase("preparation_shutdown", self.unmount)
            else:
                self.not_applicable("preparation_shutdown", "no daemon")
            self.phase("application_readiness", self.mount if mounted else self.check_paths)
            self.phase("application_execution", self.application)
            self.phase("application_completion_accounting", self.complete_application)
            if mounted:
                self.phase("daemon_shutdown", self.unmount)
                self.phase("verification_readiness", self.mount)
            else:
                self.not_applicable("daemon_shutdown", "no daemon")
                self.not_applicable("verification_readiness", "no remount required")
            self.phase("correctness_verification", lambda: self.io("verify"))
            if mounted:
                self.phase("verification_shutdown", self.unmount)
            else:
                self.not_applicable("verification_shutdown", "no daemon")
            self.phase("post_exit_storage_accounting", self.storage_accounting)
            self.check_canonical()
            daemon = self.state["measurements"]["daemon_lifecycles"]
            self.state["measurements"]["daemon"] = {
                name: metric(sum(d[name]["value"] for d in daemon) if daemon and all(d[name]["value"] is not None for d in daemon) else None,
                             "seconds", "sum of non-overlapping daemon lifecycle wait4 usage",
                             "all trial daemons including worker threads, staging and verification; not added to app wall",
                             [d[name]["interval"] for d in daemon] if daemon else None,
                             self.state["evidence"], ("not applicable: no daemon" if not daemon else
                                 "one or more daemon resource endpoints unavailable" if any(d[name]["value"] is None for d in daemon) else None))
                for name in ("user_seconds", "system_seconds")}
            self.state["outcome"] = "passed"
            self.save()
            return True
        except (OSError, ValueError, TrialError) as error:
            try:
                self.record_daemon_warnings()
            except (OSError, TrialError) as warning_error:
                self.state["warning_collection_error"] = str(warning_error)
            self.state.setdefault("errors", []).append(str(error))
            if self.state["phases"][-1]["name"] == "post_exit_storage_accounting":
                self.state["storage"] = {"allocated_bytes": None, "regular_apparent_bytes": None,
                                         "reason": "accounting incomplete: " + str(error), "errors": [str(error)]}
            self.fail(error)
            return False

    def verify(self):
        if self.state["backend"] not in ("ext4", "offline-check"):
            return super().verify()
        try:
            self.no_mounts()
            self.check_paths()
            self.check_canonical()
            self.check_device()
            if self.state["outcome"] != "passed":
                raise TrialError("verification refused: trial not passed")
            self.phase("standalone_reverify", lambda: self.io("verify"))
            return True
        except (OSError, ValueError, TrialError) as error:
            self.fail(error)
            return False

    def confirm_exits(self):
        # Persisted process identities are evidence only. Even recorded exits
        # cannot authorize deletion if that exact process still exists.
        if self.processes.evidence != "mocked":
            for child in self.state["children"]:
                identity = child.get("identity")
                if identity is None:
                    raise TrialError("cleanup refused: missing child identity")
                try:
                    if process_identity(identity["pid"]) == identity:
                        raise TrialError("cleanup refused: recorded child still exists")
                except FileNotFoundError:
                    pass

    def cleanup(self):
        self.check_device()
        self.confirm_exits()
        super().cleanup()
        self.state["retention"]["workspace"] = "guardedly removed"
        self.save()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("prepare", "self-test"):
        command = commands.add_parser(name)
        command.add_argument("--parent", type=Path, required=True)
        command.add_argument("--records", type=int, default=MAX_RECORDS)
        command.add_argument("--seed", type=int, default=fixture.DEFAULT_SEED)
        if name == "prepare":
            command.add_argument("--backend", choices=("offline-directory", "myfs"), required=True)
            command.add_argument("--daemon", type=Path)
            command.add_argument("--build-provenance")
        for limit, default in (("readiness", 10), ("application", 30), ("shutdown", 30), ("io", 30)):
            command.add_argument(f"--{limit}-seconds", type=float, default=default)
    for name in ("run", "verify", "cleanup"):
        commands.add_parser(name).add_argument("artifacts", type=Path)
    commands.add_parser("measure-run").add_argument("artifacts", type=Path)
    def cpu_list(value):
        try:
            numbers = [int(x) for x in value.split(",")]
            if any(x < 0 for x in numbers):
                raise ValueError()
            return sorted(numbers)
        except ValueError as error:
            raise argparse.ArgumentTypeError("CPUs must be comma-separated nonnegative integers") from error
    for name in ("measure-prepare", "measure-self-test"):
        command = commands.add_parser(name)
        command.add_argument("--parent", type=Path, required=True)
        command.add_argument("--canonical-parent", type=Path, required=True)
        command.add_argument("--records", type=int, default=100 if name == "measure-self-test" else MAX_RECORDS)
        command.add_argument("--seed", type=int, default=fixture.DEFAULT_SEED)
        command.add_argument("--materialize", action="store_true")
        if name == "measure-prepare":
            command.add_argument("--backend", choices=("ext4", "myfs"), required=True)
            command.add_argument("--expected-device", required=True)
            command.add_argument("--daemon", type=Path)
            command.add_argument("--build-provenance", type=Path)
            command.add_argument("--application-cpus", type=cpu_list)
            command.add_argument("--daemon-cpus", type=cpu_list)
        for limit, default in (("readiness", 10), ("application", 30), ("shutdown", 30), ("io", 30)):
            command.add_argument(f"--{limit}-seconds", type=float, default=default)
    args = parser.parse_args(argv)
    trial = None
    try:
        if args.command in ("measure-prepare", "measure-self-test"):
            trial = MeasurementTrial.prepare(args.parent, canonical_parent=args.canonical_parent,
                backend="offline-check" if args.command == "measure-self-test" else args.backend,
                records=args.records, seed=args.seed, expected_device=getattr(args, "expected_device", None),
                daemon=getattr(args, "daemon", None), build_provenance=getattr(args, "build_provenance", None),
                application_affinity=getattr(args, "application_cpus", None), daemon_affinity=getattr(args, "daemon_cpus", None),
                materialize=args.materialize, **{name + "_seconds": getattr(args, name + "_seconds")
                                               for name in ("readiness", "application", "shutdown", "io")})
            passed = trial.state["outcome"] == "prepared"
            if args.command == "measure-self-test" and passed:
                passed = trial.execute()
        elif args.command in ("prepare", "self-test"):
            trial = Trial.prepare(args.parent, backend="offline-directory" if args.command == "self-test" else args.backend,
                                  records=args.records, seed=args.seed, daemon=getattr(args, "daemon", None),
                                  build_provenance=getattr(args, "build_provenance", None),
                                  **{name + "_seconds": getattr(args, name + "_seconds")
                                     for name in ("readiness", "application", "shutdown", "io")})
            passed = trial.state["outcome"] == "prepared"
            if args.command == "self-test" and passed:
                passed = trial.execute()
        else:
            trial = Trial.load(args.artifacts)
            if args.command == "measure-run":
                if not isinstance(trial, MeasurementTrial):
                    raise TrialError("measure-run requires a prepared measurement trial")
                passed = trial.execute()
            elif args.command == "run":
                if isinstance(trial, MeasurementTrial):
                    raise TrialError("measurement trial requires explicit measure-run")
                passed = trial.execute()
            elif args.command == "verify":
                passed = trial.verify()
            else:
                trial.cleanup()
                passed = True
        print(json.dumps({"passed": passed, "outcome": trial.state["outcome"], "label": trial.state["label"],
                          "artifacts": str(trial.artifacts), "workspace": str(trial.workspace),
                          "canonical": trial.state["canonical"], "logical": trial.state["logical"],
                          "error": trial.state["error"]}, sort_keys=True))
        return 0 if passed else 1
    except (OSError, ValueError, TrialError) as error:
        print(json.dumps({"passed": False, "error": str(error),
                          "artifacts": str(trial.artifacts) if trial else None}, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
