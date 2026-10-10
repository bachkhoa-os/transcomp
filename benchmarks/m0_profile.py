"""M0-B2 smoke lifecycle, not a timing or compression benchmark.

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
cache controls, collectors, baselines or performance claims are implemented.
Exit 0 means the requested action succeeded, 1 means error/invalid trial,
and argparse uses 2 for invalid syntax. JSON includes retained artifact paths.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
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
        try:
            child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, close_fds=True)
        except BaseException:
            stream.close()
            raise
        entry = {"identity": None, "pid": child.pid, "identity_unavailable_reason": "identity capture pending",
                 "command": command, "exit_status": None,
                 "exit_status_reason": "exit not yet observed", "launch_monotonic": self.clock.now(),
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
            if anchor.get("schema") != SCHEMA or not re.fullmatch(r"[0-9a-f]{32}", anchor.get("trial_id", "")):
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
                or self.state.get("schema") != SCHEMA or self.state.get("roots") != expected_roots
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
            path = strict_path(self.artifacts / name)
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

    def mount(self):
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
        deadline = self.deadline("readiness")
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
                   str(self.artifacts), str(self.root), str(result_path)]
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
    args = parser.parse_args(argv)
    trial = None
    try:
        if args.command in ("prepare", "self-test"):
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
            if args.command == "run":
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
