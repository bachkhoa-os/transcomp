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

Opt-in single-trial measurement (default 32768, maximum 1048576 records;
no repetitions). Smoke and both OFFLINE self-tests remain <=32768 records:
  measure-prepare --parent DIR --canonical-parent DIR --backend ext4|myfs
                  --expected-device MAJOR:MINOR [--records 32768 --seed 20261010]
                  [--daemon FILE --build-provenance OWNED_JSON_FILE]
                  [--application-cpus LIST --daemon-cpus LIST] [--materialize]
                  [--staging-seconds S --verification-seconds S]
                  [--overall-seconds S]
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

Measurement deadlines remain positive and <=300 seconds per operation.
Staging/verification default to --io-seconds (30); io also bounds canonical
generation and accounting. The overall active prepare+run budget defaults
to 900 seconds, maximum 900; idle time between CLI invocations is excluded.
Recommended full-size settings: readiness 15, application/staging/verification/
shutdown/io 120, overall 900. They are opt-in, never larger smoke defaults.
Full-size input is 134217797 bytes including header, with the existing seed
and generator unchanged. No full-size workload is implied by preparation help.

Capacity reservations are estimates, not allocation caps: backing requires
1 GiB retained headroom + 64 MiB evidence + 8*input bytes; canonical storage
requires 1 GiB retained headroom + 64 MiB allowance + 2*input bytes (input
plus conservative output).
If both share a device in OFFLINE mode, combine their data reservations.
MemAvailable requires 2 GiB, plus 2*input bytes for canonical tmpfs storage.
Checkpoints enforce a separate 64 MiB artifact budget using no-follow,
unique-inode apparent/allocated observations; child logs remain <=1 MiB.
No automatic canonical reuse, archiving or historical deletion. By default one
new canonical bundle per trial is retained; repeated full-size use requires
explicit reuse and capacity planning. Guards are observations, not hard limits.

Opt-in campaign-owned shared references:
  canonical-prepare --canonical-parent DIR --evidence-parent DIR
                    [--campaign-id HEX_UUID --records 32768 --seed 20261010]
                    [--io-seconds 30]
  measure-prepare --parent DIR --canonical-bundle BUNDLE --campaign-id HEX_UUID
                  --backend ext4|myfs --expected-device MAJOR:MINOR ...
  measure-self-test --parent DIR --canonical-bundle BUNDLE --campaign-id HEX_UUID
                    [--records 100 --seed 20261010]

Reuse replaces --canonical-parent, never silently creates missing references.
Size/seed must match; defaults remain unchanged. Creation atomically claims
m0-shared-canonical-UUID and m0-canonical-evidence-UUID private directories.
The second directory holds a versioned creation/oracle receipt and hashes,
not another CSV copy. Use nonvolatile evidence storage for real measurements;
tmpfs receipt storage is permitted only for offline checks. Trial anchors pin
the receipt and canonical identities.
Cleanup never deletes either campaign-owned directory; no canonical-cleanup
command exists. Tmpfs CSVs vanish across reboot: explicitly reconstruct and
verify a new bundle, then prepare new trials; stale trials cannot adopt it.
For shared reuse, canonical new-allocation estimate is zero, canonical free
headroom remains 1 GiB + 64 MiB, and memory free headroom remains 2 GiB.
Existing shared allocation is recorded separately and is already reflected
in observed free space/MemAvailable. Backing headroom/model is unchanged.
No automatic archive, reconstruction, repetitions or statistical reporting.

Application CPU diagnostics (measurement preparation only):
  measure-prepare ... --diagnostic python-cpu
  measure-run ARTIFACT_DIRECTORY

Default commands and smoke/OFFLINE self-test CLIs remain unprofiled. An explicit
diagnostic is pinned in ownership metadata and excluded from baseline populations.
The same owned Python child uses cProfile(timer=time.process_time) around
runpy.run_path of the unchanged application, with its original argv, script
directory at sys.path[0], __main__ behavior and unchanged cwd. Launcher imports
are already loaded and runpy has its own loader frames: this is an instrumented
invocation, not identical interpreter startup. CPU seconds combine user/system
execution, not blocking time; C extensions expose call boundaries, not native
internal stacks. Whole-child wait4 also includes launcher/startup/finalization.
Serialization follows profiler disable, outside the profile but inside complete
child wall time. Instrumented wall samples are labeled
instrumented_application_completion. OFFLINE wall values remain unavailable.
Raw pstats-compatible marshal data and a JSON receipt use exclusive, no-follow,
private trial-specific files in persistent artifacts. Writes are strictly bounded
to 16 MiB for raw data and 2 MiB for the receipt, within the 64 MiB trial budget.
Serialization errors retain partial files and invalidate the trial. Identity/hash
checks detect accidental changes, not malicious rewriting of all same-user state.

Isolated daemon sync diagnostics (measurement-only, myfs-only):
  measure-prepare ... --backend myfs --daemon-diagnostic sync-calls
                  --daemon DIAGNOSTIC_ELF --build-provenance REVIEWED_JSON
Only an independently built/reviewed executable is accepted: unchanged production
objects plus m0_daemon_sync_diagnostic.c and --wrap=fsync,--wrap=fdatasync. No automatic
build. Its provenance extends m0-smoke-build-provenance-v1 with daemon_sync schema
myfs-m0-sync-build-v1, wrapper hash, exact nine-source-site relocation/disassembly
coverage, release/debug flags, linked addresses and ELF Build ID. The receipt pins
that evidence; it is not cryptographic attestation against a malicious same user.
Python CPU profiling cannot be combined with daemon sync profiling. Ordinary
measurements and smoke defaults remain unchanged. Instrumented child timing uses
the same launch-to-wait4 boundary but is baseline-ineligible and labeled
 daemon_sync_instrumented_application_completion.

The same daemon executable is a directly owned child in all three mount cycles.
Only application-mount launch inherits the exclusive private JSONL artifact FD,
using pass_fds through taskset's exec; the constructor sets CLOEXEC, clears the
handoff environment and closes/disables the writer in fork descendants. Preparation
and verification are NOT COLLECTED. No tracer, extra waiter, or process adoption.
Actual direct-daemon executable identity is checked after mount readiness.

The recorder uses 512 KiB static event memory, at most 8192 calls and a strict
4 MiB emitted-byte limit. Mutex bookkeeping is outside the real sync operation.
It records CLOCK_MONOTONIC nanoseconds, TID, raw return PC, FD, rc and saved errno.
No per-call artifact I/O: normal-exit finalization writes header/events/footer.
Missing footer, overflow, write/clock errors, live threads, inconsistent identities,
loss or partial output invalidate the trial and retain all evidence. A separately
bounded 1 MiB controller receipt is independently derived after normal exit and
rechecked on verify/cleanup. More than 256 distinct thread or caller groups refuses
analysis while retaining the raw trace. Caller attribution requires later checked
symbolization using the recorded image base and pinned binary/Build ID; FD numbers
alone do not identify files. No production atexit/destructor sync is currently
present; later wrapped destructor calls would require re-review of finalization.

Counts, elapsed min/median/max, sums, and interval unions are diagnostic populations.
The application-window union clips call intervals; straddlers are reported. Summed
thread durations overlap, and neither sums nor unions measure disk service, off-CPU
waiting, scheduling delay, FUSE latency or causal application delay. Serialization
is after sync timing but inside daemon shutdown. Raw events survive cleanup; no
real mounted instrumentation is implied by offline helpers/fake lifecycle tests.
"""

import argparse
import cProfile
import hashlib
import io
import json
import math
import marshal
import os
from pathlib import Path
import re
import runpy
import selectors
import shutil
import signal
import stat
import statistics
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


def summary(path, guard=None):
    digest, count, lines = hashlib.sha256(), 0, 0
    with Path(path).open("rb", buffering=fixture.BUFFER_BYTES) as stream:
        while True:
            if guard is not None:
                guard()
            chunk = stream.read(fixture.BUFFER_BYTES)
            if not chunk:
                break
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

    def launch(self, command, log, limit, *, pass_fds=(), env=None):
        if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
            raise TrialError("pidfd supervision unavailable; refusing launch")
        fd = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        stream = os.fdopen(fd, "wb")
        launch_time = self.clock.now()
        try:
            child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, close_fds=True, pass_fds=pass_fds, env=env)
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
            result[name] = dict(self.canonical_summary(path), identity={"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid})
        return result

    canonical_summary = staticmethod(summary)

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

    def launch_daemon(self, command, log, limit, index):
        return self.processes.launch(command, log, limit)

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
        self.daemon_identity = self.launch_daemon(command, self.artifacts / f"daemon-{index}.log",
                                                  self.state["limits"]["log_bytes_per_child"], index)
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
MEASUREMENT_MAX_RECORDS = 1048576
EVIDENCE_BUDGET_BYTES = 64 * 1024**2
PYTHON_PROFILE_SCHEMA = "myfs-m0-python-cpu-v1"
PROFILE_MAX_BYTES = 16 * 1024**2


def diagnostic_configuration(mode):
    if mode is not None and (not isinstance(mode, str) or mode != "python-cpu"):
        raise TrialError("invalid diagnostic; select python-cpu explicitly or leave unprofiled")
    return {"schema": PYTHON_PROFILE_SCHEMA, "mode": mode,
            "timer": "time.process_time" if mode else None,
            "profile_max_bytes": PROFILE_MAX_BYTES if mode else None}


def profile_names(trial_id):
    if not re.fullmatch(r"[0-9a-f]{32}", trial_id):
        raise TrialError("invalid diagnostic trial identity")
    prefix = "python-cpu-" + trial_id
    return prefix + ".prof", prefix + ".json"


def profile_directory(artifacts, anchor):
    artifacts = strict_path(artifacts)
    if (artifacts != Path(anchor["parent"]) / ("m0-artifacts-" + anchor["trial_id"])
            or directory_identity(artifacts.parent) != anchor["parent_identity"]
            or read_json(artifacts / "owner.json") != anchor
            or stat.S_IMODE(artifacts.stat().st_mode) != 0o700):
        raise TrialError("diagnostic artifact ownership/path mismatch")
    fd = os.open(artifacts, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        profile_directory_check(artifacts, fd, anchor)
        return fd
    except BaseException:
        os.close(fd)
        raise


def profile_directory_check(artifacts, fd, anchor):
    info = os.fstat(fd)
    identity = {"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid}
    if identity != anchor["identities"]["artifacts"] or directory_identity(strict_path(artifacts)) != identity:
        raise TrialError("diagnostic artifact directory substituted")


def profile_file_read(fd, name, maximum, guard, *, private=True):
    """Pinned no-follow read; strict size, private owner and no hard-link aliases."""
    child = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
    try:
        before = os.fstat(child)
        if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid()
                or (private and stat.S_IMODE(before.st_mode) != 0o600) or before.st_nlink != 1
                or before.st_size > maximum):
            raise TrialError("unsafe/oversized diagnostic artifact")
        chunks, size = [], 0
        while True:
            guard()
            chunk = os.read(child, min(65536, maximum + 1 - size))
            if not chunk:
                break
            chunks.append(chunk)
            size += len(chunk)
            if size > maximum:
                raise TrialError("diagnostic artifact grew beyond limit")
        after, named = os.fstat(child), os.stat(name, dir_fd=fd, follow_symlinks=False)
        def stable(info):
            return (info.st_dev, info.st_ino, info.st_uid, info.st_mode, info.st_nlink,
                    info.st_size, info.st_mtime_ns, info.st_ctime_ns)
        if stable(before) != stable(after) or stable(after) != stable(named) or size != after.st_size:
            raise TrialError("diagnostic artifact identity/content changed during read")
        data = b"".join(chunks)
        return data, {"identity": {"device": after.st_dev, "inode": after.st_ino, "uid": after.st_uid},
                      "sha256": hashlib.sha256(data).hexdigest(), "byte_count": size,
                      "mtime_ns": after.st_mtime_ns, "ctime_ns": after.st_ctime_ns}
    finally:
        os.close(child)


def profile_application_file(script):
    # Repository source can be group-writable (the current checkout is 0664).
    # Pin its identity/content; do not apply campaign-private permission rules.
    script = strict_path(script)
    fd = os.open(script.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        _, record = profile_file_read(fd, script.name, 2 * 1024**2, lambda: None, private=False)
        return record
    finally:
        os.close(fd)


class BoundedProfileWriter:
    """Write-time byte ceiling; retain partial data on overflow or I/O failure."""

    def __init__(self, fd, parent, name, maximum):
        self.fd, self.parent, self.name, self.maximum = fd, parent, name, maximum
        self.written = 0
        self.identity = os.fstat(fd)

    def write(self, data):
        if len(data) > self.maximum - self.written:
            raise TrialError("diagnostic serialization exceeds write-time size limit")
        current = os.stat(self.name, dir_fd=self.parent, follow_symlinks=False)
        if ((current.st_dev, current.st_ino) != (self.identity.st_dev, self.identity.st_ino)
                or current.st_uid != os.getuid() or current.st_nlink != 1
                or not stat.S_ISREG(current.st_mode) or stat.S_IMODE(current.st_mode) != 0o600):
            raise TrialError("diagnostic output identity substituted")
        offset = 0
        while offset < len(data):
            n = os.write(self.fd, data[offset:])
            if n <= 0:
                raise OSError("diagnostic short write")
            offset += n
            self.written += n
        return offset


def profile_statistics(stats, guard=lambda: None):
    """Validate the raw pstats format before accepting any call/CPU summary."""
    if not isinstance(stats, dict) or not stats or len(stats) > 65536:
        raise TrialError("invalid/empty diagnostic function statistics")
    def function(key):
        return (isinstance(key, tuple) and len(key) == 3 and isinstance(key[0], str)
                and type(key[1]) is int and key[1] >= 0 and isinstance(key[2], str))
    def numbers(values):
        return (len(values) == 4 and all(type(n) is int and n >= 0 for n in values[:2])
                and all(type(n) in (int, float) and math.isfinite(n) and n >= 0 for n in values[2:]))
    for key, value in stats.items():
        guard()
        if (not function(key) or not isinstance(value, tuple) or len(value) != 5
                or not numbers(value[:4]) or value[0] > value[1] or not isinstance(value[4], dict)):
            raise TrialError("corrupt diagnostic function statistics")
        for caller, counters in value[4].items():
            guard()
            if not function(caller) or not isinstance(counters, tuple) or not numbers(counters):
                raise TrialError("corrupt diagnostic caller statistics")
    calls = sum(value[1] for value in stats.values())
    if calls <= 0:
        raise TrialError("diagnostic profile has no calls")
    return {"function_count": len(stats), "total_calls": calls,
            "primitive_calls": sum(value[0] for value in stats.values()),
            "self_cpu_seconds": sum(value[2] for value in stats.values()),
            "self_cpu_reason": "sum of non-cumulative profiler self CPU; not complete wait4 CPU or blocked time"}


def python_cpu_profile(artifacts, anchor, evidence, script, root):
    """Child-only launcher: one process, CPU clock, original script via runpy.

    No waiter or backend logic. Exceptions/SystemExit propagate after disabling
    and publishing diagnostics; serialization failure makes successful scripts
    exit nonzero. No output path is accepted from persisted profile receipts.
    """
    launcher_start = time.monotonic()
    config = diagnostic_configuration("python-cpu")
    if anchor.get("diagnostic") != config:
        raise TrialError("diagnostic launcher configuration mismatch")
    artifacts, script, root = Path(artifacts), strict_path(script), Path(root)
    application_record = {"path": str(script), "file": profile_application_file(script)}
    if (application_record != anchor.get("diagnostic_application")
            or root != Path(anchor["parent"]) / ("m0-work-" + anchor["trial_id"]) / "application"):
        raise TrialError("diagnostic application script/root mismatch")
    directory = profile_directory(artifacts, anchor)
    raw_name, receipt_name = profile_names(anchor["trial_id"])
    raw_fd = receipt_fd = None
    error = serialization_error = None
    report = {"schema": PYTHON_PROFILE_SCHEMA, "trial_id": anchor["trial_id"],
              "mode": "python-cpu", "timer": "time.process_time", "unit": "CPU seconds (user + system)",
              "boundary_clock": "time.monotonic", "boundary_unit": "seconds",
              "evidence": evidence, "process_identity": process_identity(os.getpid()),
              "application": application_record, "argv": [str(script), str(root)], "cwd": os.getcwd(),
              "sys_path_0": str(script.parent), "valid": False, "reason": "profile not serialized",
              "sha256": None, "byte_count": None,
              "script_exit_status": None, "script_error": None, "serialization_error": None,
              "serialization_scope": "profiler finalization, raw marshal write and hash; JSON receipt publication follows",
              "boundaries": {"launcher_start": launcher_start, "profiler_enable": None,
                  "profiler_disable": None, "serialization_start": None, "serialization_end": None}}
    try:
        for name in (raw_name, receipt_name):
            try:
                os.stat(name, dir_fd=directory, follow_symlinks=False)
            except FileNotFoundError:
                continue
            raise TrialError("existing diagnostic artifact; refusing overwrite")
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
        raw_fd = os.open(raw_name, flags, 0o600, dir_fd=directory)
        created_raw = os.fstat(raw_fd)
        receipt_fd = os.open(receipt_name, flags, 0o600, dir_fd=directory)
        old_argv, old_path = sys.argv, sys.path[:]
        profiler = cProfile.Profile(timer=time.process_time)
        try:
            sys.argv = [str(script), str(root)]
            sys.path[0] = str(script.parent)
            report["boundaries"]["profiler_enable"] = time.monotonic()
            profiler.enable()
            try:
                runpy.run_path(str(script), run_name="__main__")
            except BaseException as caught:
                error = caught
            finally:
                profiler.disable()
                report["boundaries"]["profiler_disable"] = time.monotonic()
        finally:
            sys.argv, sys.path[:] = old_argv, old_path
        if isinstance(error, SystemExit):
            code = error.code
            report["script_exit_status"] = 0 if code is None else code % 256 if isinstance(code, int) else 1
        else:
            report["script_exit_status"] = 1 if error else 0
        report["script_error"] = repr(error)[:4096] if error and report["script_exit_status"] != 0 else None
        report["boundaries"]["serialization_start"] = time.monotonic()
        try:
            profiler.create_stats()  # Already disabled; cProfile preserves pstats caller data.
            report.update(profile_statistics(profiler.stats))
            marshal.dump(profiler.stats, BoundedProfileWriter(raw_fd, directory, raw_name, config["profile_max_bytes"]))
            _, raw_record = profile_file_read(directory, raw_name, config["profile_max_bytes"], lambda: None)
            current_raw = os.fstat(raw_fd)
            if (raw_record["identity"] != {"device": created_raw.st_dev, "inode": created_raw.st_ino, "uid": created_raw.st_uid}
                    or current_raw.st_nlink != 1 or raw_record["byte_count"] != current_raw.st_size
                    or raw_record["mtime_ns"] != current_raw.st_mtime_ns
                    or raw_record["ctime_ns"] != current_raw.st_ctime_ns):
                raise TrialError("diagnostic raw output substituted after serialization")
            report.update(raw_record, valid=True, reason=None)
        except Exception as caught:
            serialization_error = caught
            report.update(valid=False, reason="profile serialization failed", serialization_error=str(caught)[:4096])
            try:
                _, raw_record = profile_file_read(directory, raw_name, config["profile_max_bytes"], lambda: None)
                report.update(raw_record)  # Actual partial bytes/hash, never an empty valid fallback.
            except Exception as observation_error:
                report["raw_observation_error"] = str(observation_error)[:4096]
        finally:
            report["boundaries"]["serialization_end"] = time.monotonic()
        profile_directory_check(artifacts, directory, anchor)
        info = os.fstat(receipt_fd)
        report["receipt_identity"] = {"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid}
        payload = (json.dumps(report, sort_keys=True, indent=2) + "\n").encode("ascii")
        BoundedProfileWriter(receipt_fd, directory, receipt_name, 2 * 1024**2).write(payload)
        if serialization_error is not None and report["script_exit_status"] == 0:
            raise TrialError("diagnostic serialization failed") from serialization_error
        if error is not None:
            raise error
    finally:
        for fd in (raw_fd, receipt_fd, directory):
            if fd is not None:
                os.close(fd)


PYTHON_PROFILE_CODE = (
    "import json,sys; from pathlib import Path; sys.path[0]=sys.argv[1]; import m0_profile as m; "
    "m.python_cpu_profile(Path(sys.argv[2]),json.loads(sys.argv[3]),sys.argv[4],Path(sys.argv[5]),Path(sys.argv[6]))"
)


def read_python_profile(artifacts, anchor, evidence, child, guard, previous=None):
    """Require complete matching evidence from this owned child, never empty fallback."""
    directory = profile_directory(artifacts, anchor)
    raw_name, receipt_name = profile_names(anchor["trial_id"])
    try:
        data, receipt = profile_file_read(directory, receipt_name, 2 * 1024**2, guard)
        report = json.loads(data)
        if (not isinstance(report, dict) or report.get("schema") != PYTHON_PROFILE_SCHEMA
                or report.get("trial_id") != anchor["trial_id"] or report.get("mode") != "python-cpu"
                or report.get("timer") != "time.process_time" or report.get("unit") != "CPU seconds (user + system)"
                or report.get("boundary_clock") != "time.monotonic" or report.get("boundary_unit") != "seconds"
                or report.get("evidence") != evidence or report.get("process_identity") != child["identity"]
                or report.get("application") != anchor["diagnostic_application"]
                or report.get("argv") != [anchor["diagnostic_application"]["path"],
                    str(Path(anchor["parent"]) / ("m0-work-" + anchor["trial_id"]) / "application")]
                or report.get("sys_path_0") != str(Path(anchor["diagnostic_application"]["path"]).parent)
                or report.get("cwd") != child.get("diagnostic_cwd")
                or report.get("receipt_identity") != receipt["identity"]):
            raise TrialError("diagnostic receipt configuration/identity mismatch")
        if report.get("valid") is not True:
            raise TrialError("diagnostic serialization invalid: " + str(report.get("serialization_error") or report.get("reason")))
        bounds = report.get("boundaries", {})
        if not isinstance(bounds, dict):
            raise TrialError("diagnostic boundaries must be an object")
        times = [child["launch_monotonic"], *[bounds.get(n) for n in
                 ("launcher_start", "profiler_enable", "profiler_disable", "serialization_start", "serialization_end")],
                 child["exit_monotonic"]]
        if (any(type(t) not in (int, float) or not math.isfinite(t) for t in times)
                or times != sorted(times) or report.get("script_exit_status") != child["exit_status"]):
            raise TrialError("diagnostic timing/exit evidence mismatch")
        blob, raw = profile_file_read(directory, raw_name, anchor["diagnostic"]["profile_max_bytes"], guard)
        if any(report.get(k) != v for k, v in raw.items()):
            raise TrialError("diagnostic profile identity/hash mismatch")
        source = io.BytesIO(blob)
        statistics = profile_statistics(marshal.load(source), guard)
        if source.read(1) or any(report.get(k) != v for k, v in statistics.items()):
            raise TrialError("diagnostic statistics mismatch/trailing bytes")
        value = dict(report, artifact=str(artifacts / raw_name), receipt=str(artifacts / receipt_name),
                     receipt_record=receipt, child_completion_monotonic=child["exit_monotonic"],
                     population="instrumented script invocation including runpy loader; serialization excluded",
                     limitation="CPU clock combines user/system; C-call boundaries are not native stacks; not blocked time or complete wait4 CPU")
        if previous is not None and value != previous:
            raise TrialError("diagnostic evidence changed after publication")
        profile_directory_check(artifacts, directory, anchor)
        guard()
        return value
    except (OSError, ValueError, TypeError, KeyError, EOFError, OverflowError, RecursionError) as error:
        raise TrialError("missing/corrupt diagnostic evidence: " + str(error)) from error
    finally:
        os.close(directory)


def measurement_limits(records, *, readiness_seconds=10, application_seconds=30,
                       shutdown_seconds=30, io_seconds=30, staging_seconds=None,
                       verification_seconds=None, overall_seconds=900):
    """Validate before paths/allocation; integer records never silently truncate."""
    if (not isinstance(records, int) or isinstance(records, bool)
            or not 0 <= records <= MEASUREMENT_MAX_RECORDS):
        raise TrialError("measurement records must be an integer in 0..1048576")
    deadlines = {"readiness": readiness_seconds, "application": application_seconds,
                 "shutdown": shutdown_seconds, "io": io_seconds,
                 "staging": io_seconds if staging_seconds is None else staging_seconds,
                 "verification": io_seconds if verification_seconds is None else verification_seconds}
    for name, value in dict(deadlines, overall=overall_seconds).items():
        maximum = 900 if name == "overall" else 300
        if (not isinstance(value, (int, float)) or isinstance(value, bool)
                or not 0 < value <= maximum or not math.isfinite(value)):
            raise TrialError(f"invalid measurement {name} deadline; require finite 0 < seconds <= {maximum}")
    return {"records": records, "input_bytes": 69 + records * 128,
            "size_class": "smoke-sized" if records <= MAX_RECORDS else "larger-measurement",
            "deadlines_seconds": deadlines, "overall_seconds": overall_seconds}


def measurement_capacity(input_bytes, *, canonical_tmpfs, same_device, shared=False):
    """Streaming workload estimate; generated output is bounded by input size.

    8*input covers logical files, temporary generations/compaction and metadata;
    it is not a proven worst-case physical allocation bound. 64 MiB per-trial
    evidence comfortably exceeds observed M0-D3 runner logs/states, while its
    separate campaign canonical archive is NOT generated by this runner.
    """
    # Existing references already consume observed free space/MemAvailable.
    # Reuse reserves no second copy; the backing headroom is unchanged.
    canonical_data = 0 if shared else 2 * input_bytes
    return {"input_bytes": input_bytes, "output_upper_estimate_bytes": input_bytes,
            "retained_headroom_bytes": 1024**3, "evidence_budget_bytes": EVIDENCE_BUDGET_BYTES,
            "backing_required_bytes": 1024**3 + EVIDENCE_BUDGET_BYTES + 8 * input_bytes
                                      + (canonical_data if same_device else 0),
            "canonical_required_bytes": 1024**3 + EVIDENCE_BUDGET_BYTES + canonical_data,
            "memory_required_bytes": 2 * 1024**3 + (canonical_data if canonical_tmpfs else 0),
            "canonical_new_allocation_estimate_bytes": canonical_data,
            "limitation": "conservative reservation estimate, not a hard allocation or instantaneous resource bound; "
                          "MemAvailable does not establish effective cgroup/RLIMIT capacity"}


GENERATION_CODE = (
    "import sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
    "import m0_fixture as f; root=Path(sys.argv[2]); "
    "f.generate_csv(root/'input.csv',root/'input.manifest.json',int(sys.argv[3]),int(sys.argv[4])); "
    "f.write_expected(root/'input.csv',root/'expected.csv')"
)


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

    def launch(self, *args, **kwargs):
        if any(not hasattr(os, name) for name in ("wait4", "waitid", "P_PIDFD", "WNOWAIT")):
            raise TrialError("pidfd waitid/wait4 accounting unavailable; refusing launch")
        return super().launch(*args, **kwargs)

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


CANONICAL_SCHEMA = "myfs-m0-shared-canonical-v1"
CANONICAL_NAMES = ("input.csv", "input.manifest.json", "expected.csv")


def canonical_file(path, guard=None, *, parse_json=False):
    """Hash a pinned no-follow regular file; refuse changes during the read.

    Markers are bounded to 2 MiB; data to the reviewed full input size. Private
    directories and identity/hash checks detect substitution, not malicious
    rewriting of every same-user ownership receipt. No cryptographic attestation.
    """
    path = strict_path(path)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid()
                or before.st_nlink != 1 or before.st_mode & 0o022
                or before.st_size > (2 * 1024**2 if parse_json else 69 + 128 * MEASUREMENT_MAX_RECORDS)):
            raise TrialError(f"unsafe shared canonical file: {path}")
        digest, count, lines, chunks = hashlib.sha256(), 0, 0, []
        while True:
            if guard:
                guard()
            chunk = stream.read(fixture.BUFFER_BYTES)
            if not chunk:
                break
            digest.update(chunk)
            count += len(chunk)
            lines += chunk.count(b"\n")
            if parse_json:
                chunks.append(chunk)
            if count > before.st_size:
                raise TrialError("shared canonical file grew during observation")
        after, named = os.fstat(stream.fileno()), path.lstat()
        def stable(info):
            return (info.st_dev, info.st_ino, info.st_uid, info.st_mode,
                    info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_nlink)
        if stable(before) != stable(after) or stable(after) != stable(named) or count != after.st_size:
            raise TrialError("shared canonical file changed during observation")
    value = {"sha256": digest.hexdigest(), "byte_count": count, "row_count": max(0, lines - 1),
             "identity": {"device": after.st_dev, "inode": after.st_ino, "uid": after.st_uid},
             "mode": stat.S_IMODE(after.st_mode), "mtime_ns": after.st_mtime_ns, "ctime_ns": after.st_ctime_ns}
    if parse_json:
        record = json.loads(b"".join(chunks).decode("ascii"))
        if not isinstance(record, dict):
            raise TrialError("shared canonical JSON must be an object")
        return value, record
    return value


class CanonicalBundle:
    """Campaign-owned references + a small separately retained creation receipt.

    No trial writes here and no canonical deletion operation exists. A missing
    tmpfs bundle requires explicit new preparation and exact verification; old
    trials keep their original identities and cannot adopt reconstructed data.
    """

    def __init__(self, root, artifacts, owner, state, clock=None):
        self.root, self.artifacts, self.owner, self.state = root, artifacts, owner, state
        self.clock = clock or Clock()

    @staticmethod
    def campaign(value):
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{32}", value):
            raise TrialError("campaign identity must be 32 lowercase hexadecimal characters")
        return value

    def check_paths(self, mounts=None, guard=None):
        owner = self.owner
        for parent_name, path, prefix, identity in (
                ("canonical_parent", self.root, "m0-shared-canonical-", "canonical_identity"),
                ("evidence_parent", self.artifacts, "m0-canonical-evidence-", "evidence_identity")):
            if (path != Path(owner[parent_name]) / (prefix + owner["campaign_id"])
                    or directory_identity(strict_path(path.parent)) != owner[parent_name + "_identity"]
                    or directory_identity(strict_path(path)) != owner[identity]
                    or stat.S_IMODE(path.stat().st_mode) != 0o700):
                raise TrialError("shared canonical directory/parent ownership identity mismatch")
            _, actual = canonical_file(path / "owner.json", guard, parse_json=True)
            if actual != owner:
                raise TrialError("shared canonical ownership marker mismatch")
            if any(Path(m["target"]) == path or path in Path(m["target"]).parents
                   for m in (mounts or Mounts()).snapshot()):
                raise TrialError("unexpected mount over shared canonical/evidence")
        if self.root == self.artifacts or self.root in self.artifacts.parents or self.artifacts in self.root.parents:
            raise TrialError("shared canonical/evidence path overlap")

    def reference(self):
        return {"owner": self.owner, "receipt": canonical_file(self.artifacts / "state.json", parse_json=True)[0]}

    def snapshot(self, guard=None):
        names = set()
        with os.scandir(self.root) as entries:
            for entry in entries:
                names.add(entry.name)
                if len(names) > 4:
                    raise TrialError("unexpected shared canonical entries")
        if names != set((*CANONICAL_NAMES, "owner.json")):
            raise TrialError("unexpected/missing shared canonical entries")
        return {name: canonical_file(self.root / name, guard, parse_json=name.endswith(".json"))[0]
                if name.endswith(".json") else canonical_file(self.root / name, guard)
                for name in CANONICAL_NAMES}

    def allocation(self):
        # A checkpoint observation, not a peak or guaranteed reservation bound.
        return sum(path.lstat().st_blocks * 512 for path in
                   (self.root, self.root / "owner.json", *(self.root / n for n in CANONICAL_NAMES)))

    @classmethod
    def load(cls, root, *, campaign_id, records, seed, expected=None,
             guard=None, mounts=None, content=True):
        cls.campaign(campaign_id)
        config = measurement_limits(records)
        fixture._check_seed(seed)
        if guard is None:
            deadline = time.monotonic() + 30
            def guard():
                if time.monotonic() >= deadline:
                    raise TrialError("shared canonical validation timeout")
        try:
            root = strict_path(root)
            if root.name != "m0-shared-canonical-" + campaign_id:
                raise TrialError("shared canonical campaign/path mismatch")
            _, owner = canonical_file(root / "owner.json", guard, parse_json=True)
            if (owner.get("schema") != CANONICAL_SCHEMA or owner.get("campaign_id") != campaign_id
                    or owner.get("canonical_parent") != str(root.parent)
                    or owner.get("canonical_parent_identity") != directory_identity(root.parent)
                    or owner.get("canonical_identity") != directory_identity(root)
                    or expected is not None and owner != expected["owner"]):
                raise TrialError("shared canonical foreign ownership/campaign")
            artifacts = strict_path(Path(owner["evidence_parent"]) / ("m0-canonical-evidence-" + campaign_id))
            receipt, state = canonical_file(artifacts / "state.json", guard, parse_json=True)
            bundle = cls(root, artifacts, owner, state)
            bundle.check_paths(mounts, guard)
            if (state.get("owner") != owner or state.get("schema") != CANONICAL_SCHEMA
                    or state.get("campaign_id") != campaign_id or state.get("outcome") != "ready"
                    or state.get("records") != records or state.get("seed") != seed
                    or state.get("generator_version") != fixture.CSV_GENERATOR
                    or state.get("fixture_source_sha256") != summary(Path(fixture.__file__))["sha256"]
                    or expected is not None and expected != {"owner": owner, "receipt": receipt}):
                raise TrialError("shared canonical incompatible, stale or unvalidated receipt")
            manifest_snapshot, manifest = canonical_file(root / "input.manifest.json", guard, parse_json=True)
            if (manifest != state["fixture_manifest"] or manifest_snapshot != state["canonical"]["input.manifest.json"]
                    or manifest.get("generator_version") != fixture.CSV_GENERATOR
                    or manifest.get("schema_version") != "myfs-m0-input-v1"
                    or manifest.get("manifest_version") != "myfs-m0-manifest-v1"
                    or manifest.get("columns") != list(fixture.INPUT_COLUMNS)
                    or manifest.get("seed") != seed or manifest.get("record_count") != records
                    or manifest.get("byte_count") != config["input_bytes"]
                    or manifest.get("sha256") != state["canonical"]["input.csv"]["sha256"]
                    or state["oracle_validation"]["passed"] is not True
                    or any(state["oracle_validation"][side] != {
                        k: state["canonical"]["expected.csv"][k] for k in ("sha256", "byte_count", "row_count")}
                        for side in ("expected", "actual"))):
                raise TrialError("shared canonical manifest/schema/validation mismatch")
            if content and bundle.snapshot(guard) != state["canonical"]:
                raise TrialError("shared canonical input/expected identity or hash mismatch")
            if content:
                input_record, output_record = state["canonical"]["input.csv"], state["canonical"]["expected.csv"]
                if (input_record["byte_count"] != config["input_bytes"] or input_record["row_count"] != records
                        or not 0 <= output_record["row_count"] <= records
                        or output_record["byte_count"] > input_record["byte_count"]):
                    raise TrialError("shared canonical byte/row accounting mismatch")
            return bundle
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise TrialError(f"shared canonical missing/invalid; explicit reconstruction required: {error}") from error

    @classmethod
    def prepare(cls, canonical_parent, *, evidence_parent, records=MAX_RECORDS,
                seed=fixture.DEFAULT_SEED, campaign_id=None, io_seconds=30,
                host=None, processes=None, mounts=None, clock=None):
        config = measurement_limits(records, io_seconds=io_seconds)
        fixture._check_seed(seed)
        campaign_id = cls.campaign(uuid.uuid4().hex if campaign_id is None else campaign_id)
        clock, host, mounts = clock or Clock(), host or MeasurementHost(), mounts or Mounts()
        started, deadline = clock.now(), clock.now() + io_seconds
        canonical_parent, evidence_parent = strict_path(canonical_parent), strict_path(evidence_parent)
        parents = {"canonical_parent": directory_identity(canonical_parent), "evidence_parent": directory_identity(evidence_parent)}
        root = canonical_parent / ("m0-shared-canonical-" + campaign_id)
        artifacts = evidence_parent / ("m0-canonical-evidence-" + campaign_id)
        if os.path.lexists(root) or os.path.lexists(artifacts):
            raise TrialError("conflicting canonical campaign path; no overwrite/resume")
        if root == artifacts or root in artifacts.parents or artifacts in root.parents:
            raise TrialError("shared canonical/evidence overlap")
        root.mkdir(mode=0o700)  # Atomic claim: concurrent preparers cannot adopt it.
        artifacts.mkdir(mode=0o700)
        owner = {"schema": CANONICAL_SCHEMA, "campaign_id": campaign_id,
                 "canonical_parent": str(canonical_parent), "evidence_parent": str(evidence_parent),
                 **{k + "_identity": v for k, v in parents.items()},
                 "canonical_identity": directory_identity(root), "evidence_identity": directory_identity(artifacts)}
        for path in (root, artifacts):
            exclusive_json(path / "owner.json", owner)
        state = {"schema": CANONICAL_SCHEMA, "campaign_id": campaign_id, "owner": owner,
                 "outcome": "preparing", "error": None, "records": records, "seed": seed,
                 "generator_version": fixture.CSV_GENERATOR, "fixture_source_sha256": summary(Path(fixture.__file__))["sha256"],
                 "commands": [], "children": [], "resource_observations": [],
                 "creation": {"start_monotonic": started, "deadline_monotonic": deadline, "end_monotonic": None},
                 "ownership_population": "campaign-owned references and retained campaign evidence; independent of trials"}
        bundle = cls(root, artifacts, owner, state, clock)
        processes = processes or AccountingProcesses(clock)
        state["evidence"] = "mocked" if any(x.evidence == "mocked" for x in (host, processes, mounts)) else "actual offline preparation"
        def save():
            state["children"] = processes.entries
            temporary = artifacts / (".state-" + uuid.uuid4().hex)
            exclusive_json(temporary, state)
            os.replace(temporary, artifacts / "state.json")
        processes.on_change = save
        def guard():
            if clock.now() >= deadline:
                raise TrialError("canonical preparation/validation timeout; partial bundle unvalidated")
        def capacity():
            bundle.check_paths(mounts, guard)
            device = host.device(root)
            plan = measurement_capacity(config["input_bytes"], canonical_tmpfs=device["fstype"] == "tmpfs", same_device=False)
            state["capacity_plan"] = plan
            for role, path, required in (("canonical", root, plan["canonical_required_bytes"]),
                                         ("campaign_evidence", artifacts, 1024**3 + EVIDENCE_BUDGET_BYTES)):
                try:
                    values = host.resources(path)
                except (OSError, ValueError) as error:
                    values = {"available_disk_bytes": None, "available_memory_bytes": None,
                              "reason": str(error), "evidence": host.evidence}
                state["resource_observations"].append(dict(values, role=role, required_disk_bytes=required,
                    required_memory_bytes=plan["memory_required_bytes"], monotonic=clock.now()))
                if values.get("available_disk_bytes") is None or values.get("available_memory_bytes") is None:
                    raise TrialError("canonical capacity observation unavailable")
                if values["available_disk_bytes"] < required or values["available_memory_bytes"] < plan["memory_required_bytes"]:
                    raise TrialError("insufficient canonical/evidence disk or memory capacity")
            guard()
        save()
        try:
            capacity()
            state["device"] = host.device(root)
            state["evidence_device"] = host.device(artifacts)
            state["persistence"] = {"filesystem": state["device"]["fstype"],
                "limitation": "tmpfs references are volatile across reboot; missing data requires explicit reconstruction, exact verification and new trial identities",
                "small_receipt": str(artifacts / "state.json"), "automatic_csv_archive": False,
                "small_receipt_filesystem": state["evidence_device"]["fstype"],
                "receipt_limitation": "tmpfs/ramfs receipts are also volatile; real measurements refuse them; nonvolatile storage is not a crash-durability guarantee"}
            command = [sys.executable, "-B", "-c", "import os; os.umask(0o077); " + GENERATION_CODE,
                       str(Path(__file__).parent), str(root), str(records), str(seed)]
            state["commands"].append(command)
            result = processes.run(command, artifacts / "generation.log", 1024**2, deadline)
            if result["exit_status"] != 0:
                raise TrialError("canonical generation failed; partial references unvalidated")
            capacity()
            command = [sys.executable, "-B", str(Path(fixture.__file__)), "validate", str(root / "input.csv"), str(root / "expected.csv")]
            state["commands"].append(command)
            result = processes.run(command, artifacts / "oracle-validation.log", 1024**2, deadline)
            if result["exit_status"] != 0:
                raise TrialError("independent canonical oracle validation failed")
            state["oracle_validation"] = json.loads((artifacts / "oracle-validation.log").read_text())
            state["canonical"] = bundle.snapshot(guard)
            state["fixture_manifest"] = canonical_file(root / "input.manifest.json", guard, parse_json=True)[1]
            state["allocated_bytes_observation"] = {"value": bundle.allocation(), "unit": "bytes",
                "source": "owned no-follow st_blocks*512 checkpoint", "population": str(root),
                "monotonic": clock.now(), "reason": None, "evidence": state["evidence"],
                "limitation": "existing shared allocation, not peak or reservation bound"}
            state["outcome"] = "ready"
            state["creation"]["end_monotonic"] = clock.now()
            guard()
            save()
            cls.load(root, campaign_id=campaign_id, records=records, seed=seed, guard=guard, mounts=mounts)
        except (OSError, ValueError, TrialError) as error:
            state.update(outcome="failed", error=str(error))
            state["creation"]["end_monotonic"] = clock.now()
            save()  # No cleanup, regeneration, signaling of unrelated processes or deletion.
        return bundle


# Isolated daemon diagnostic. No build, ptrace, extra waiter or mounted helper.
SYNC_SCHEMA = "m0-daemon-sync-v1"
SYNC_MAX_BYTES = 4 * 1024**2
SYNC_MAX_EVENTS = 8192
SYNC_WRAPPER = Path(__file__).with_name("m0_daemon_sync_diagnostic.c")
SYNC_ENV = ("M0_SYNC_FD", "M0_SYNC_TRIAL", "M0_SYNC_TOKEN")
SYNC_SITES = {
    ("src/core/path.c", "create_generation_storage", "fsync", 325),
    ("src/core/path.c", "fsync_parent_path", "fsync", 389),
    ("src/core/metadata.c", "save_chunk_map_to_path", "fsync", 456),
    ("src/core/metadata.c", "append_chunk_map_delta_to_fd", "fdatasync", 531),
    ("src/core/compact.c", "compact_data_file_locked", "fsync", 2172),
    ("src/fuse_ops/file.c", "rewrite_truncated_chunk_fd", "fdatasync", 291),
    ("src/fuse_ops/file.c", "truncate_open_handle_locked", "fdatasync", 428),
    ("src/fuse_ops/file.c", "myfs_write_locked", "fdatasync", 1052),
    ("src/fuse_ops/file.c", "myfs_release", "fsync", 1162),
}


def sync_configuration(mode):
    if mode not in (None, "sync-calls"):
        raise TrialError("invalid daemon diagnostic")
    return {"schema": SYNC_SCHEMA, "mode": mode, "clock": "CLOCK_MONOTONIC" if mode else None,
            "max_bytes": SYNC_MAX_BYTES if mode else None,
            "collection": "application-mount lifecycle only" if mode else None}


def sync_build(daemon, provenance):
    """Cross-check an independently produced build/coverage receipt; never build.

    This pins supplied evidence and current source, not a security attestation
    against malicious same-user rewrites. Coverage is deliberately source-specific;
    changes require a new disassembly review and explicit coverage update.
    """
    try:
        binary = executable_identity(daemon)
        receipt = provenance if isinstance(provenance, dict) else read_json(strict_path(provenance))
        production = measurement_source()["production_files"]
        detail = receipt["daemon_sync"]
        sites = detail["coverage"]
        if (receipt.get("schema") != "m0-smoke-build-provenance-v1" or receipt.get("exit_status") != 0
                or receipt.get("source_fingerprints_before") != production
                or receipt.get("source_fingerprints_after") != production or receipt.get("daemon") != binary
                or detail.get("schema") != "myfs-m0-sync-build-v1"
                or detail.get("wrapper_sha256") != summary(SYNC_WRAPPER)["sha256"]
                or detail.get("wrap_symbols") != ["fsync", "fdatasync"]
                or detail.get("coverage_verified") is not True
                or len(sites) != len(SYNC_SITES)
                or {(s["source"], s["function"], s["operation"], s["source_line"]) for s in sites} != SYNC_SITES
                or any(s["target"] != "__wrap_" + s["operation"]
                       or not re.fullmatch(r"(?:0x)?[0-9a-f]+", s["linked_call_address"]) for s in sites)
                or not all(f in detail.get("build_flags", []) for f in ("-O2", "-DNDEBUG", "-g", "-fno-omit-frame-pointer"))
                or not re.fullmatch(r"[0-9a-f]{40,64}", detail.get("module_build_id", ""))):
            raise TrialError("diagnostic executable provenance/coverage mismatch")
        return {"executable": binary, "provenance": receipt}
    except (KeyError, TypeError, OSError, ValueError) as error:
        raise TrialError("diagnostic executable provenance unavailable/invalid: " + str(error)) from error


def sync_names(trial_id):
    if not re.fullmatch(r"[0-9a-f]{32}", trial_id):
        raise TrialError("invalid sync diagnostic trial identity")
    return "daemon-sync-" + trial_id + ".jsonl", "daemon-sync-" + trial_id + ".receipt.json"


def sync_intervals(events, clip=None):
    intervals = []
    for event in events:
        start, end = event["entry_ns"], event["exit_ns"]
        if clip is not None:
            start, end = max(start, clip[0]), min(end, clip[1])
        if start <= end:
            intervals.append((start, end))
    durations = [(b-a)/1e9 for a, b in intervals]
    union, right = 0, None
    for start, end in sorted(intervals):
        union += max(0, end - max(start, right if right is not None else start))
        right = max(end, right if right is not None else end)
    return {"event_count": len(intervals), "failed_count": sum(e["return_value"] == -1 for e in events),
            "minimum_seconds": min(durations) if durations else None,
            "median_seconds": statistics.median(durations) if durations else None,
            "maximum_seconds": max(durations) if durations else None,
            "summed_seconds": sum(durations), "union_seconds": union/1e9,
            "empty_reason": None if durations else "no observed calls in this population"}


def decode_sync_trace(raw, *, trial_id, token, child, application_interval, shutdown_start, evidence, guard=lambda: None):
    """Independent bounded JSONL validation and interval analysis after owned exit.

    Sums can overlap across threads. Unions describe time inside observed calls,
    not disk service, off-CPU, scheduling delay or application/FUSE waiting.
    Caller PCs plus image base/build identity permit later verified symbolization;
    a reused FD number never proves an underlying pathname or data/metadata role.
    """
    def require(condition, message):
        if not condition:
            raise TrialError("invalid sync diagnostic: " + message)
    def number(value, minimum=0):
        return type(value) is int and minimum <= value < 2**64
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key")
            result[key] = value
        return result
    try:
        require(0 < len(raw) <= SYNC_MAX_BYTES and raw.endswith(b"\n"), "empty/oversized/truncated trace")
        lines = raw.splitlines()
        require(2 <= len(lines) <= SYNC_MAX_EVENTS + 2, "invalid record count")
        records = []
        for line in lines:
            guard()
            require(len(line) <= 2048, "oversized record")
            records.append(json.loads(line, object_pairs_hook=unique))
        require(all(isinstance(r, dict) for r in records), "non-object record")
        header, footer = records[0], records[-1]
        events = records[1:-1]
        pid = child["identity"]["pid"]
        require(child["exit_status"] == 0 and child["reap_count"] == 1, "normal exactly-once daemon exit unconfirmed")
        require(header.get("kind") == "header" and header.get("schema") == SYNC_SCHEMA
                and header.get("trial_id") == trial_id and header.get("token") == token
                and header.get("pid") == pid and header.get("clock") == "CLOCK_MONOTONIC"
                and header.get("unit") == "nanoseconds" and header.get("max_bytes") == SYNC_MAX_BYTES
                and header.get("max_events") == SYNC_MAX_EVENTS, "header identity/clock mismatch")
        require(number(header.get("module_base"), 1) and header.get("caller_source") == "__builtin_return_address",
                "missing caller/module attribution")
        require(footer.get("kind") == "footer" and footer.get("trial_id") == trial_id
                and footer.get("token") == token and footer.get("pid") == pid and footer.get("complete") is True
                and footer.get("overflow") is False, "incomplete/overflow finalization")
        for name in ("observed", "written", "dropped", "inflight", "clock_errors", "recording_errors"):
            require(number(footer.get(name)), "invalid completeness counter")
        require(footer["observed"] == footer["written"] == len(events)
                and all(footer[k] == 0 for k in ("dropped", "inflight", "clock_errors", "recording_errors")),
                "lost/unfinished/error events")
        require(type(footer.get("live_threads")) is int and footer["live_threads"] == 1, "daemon threads still live at finalization")
        enabled, finalized = header.get("enabled_monotonic_ns"), footer.get("finalized_monotonic_ns")
        require(number(enabled) and number(finalized) and enabled <= finalized
                and child["launch_monotonic"] <= enabled/1e9 <= finalized/1e9 <= child["exit_monotonic"],
                "recorder outside owned child interval")
        for sequence, event in enumerate(events, 1):
            guard()
            require(event.get("kind") == "event" and type(event.get("sequence")) is int and event["sequence"] == sequence
                    and event.get("pid") == pid and number(event.get("tid"), 1)
                    and event.get("operation") in ("fsync", "fdatasync")
                    and type(event.get("fd")) is int and -(2**31) <= event["fd"] < 2**31
                    and number(event.get("caller"), header["module_base"])
                    and number(event.get("entry_ns")) and number(event.get("exit_ns"))
                    and enabled <= event["entry_ns"] <= event["exit_ns"] <= finalized
                    and type(event.get("return_value")) is int and event["return_value"] in (0, -1)
                    and number(event.get("errno")) and event.get("clock_error") is False,
                    "malformed/unattributed/out-of-order event")
        require(isinstance(application_interval, (list, tuple)) and len(application_interval) == 2
                and all(type(v) in (int, float) and math.isfinite(v) for v in application_interval)
                and child["launch_monotonic"] <= application_interval[0] <= application_interval[1] <= child["exit_monotonic"]
                and type(shutdown_start) in (float, int) and math.isfinite(shutdown_start)
                and application_interval[1] <= shutdown_start <= child["exit_monotonic"], "invalid application/shutdown boundaries")
        lo, hi = (round(v*1e9) for v in application_interval)
        shutdown = round(shutdown_start*1e9)
        groups = {}
        for name, key in (("by_operation", lambda e: e["operation"]),
                          ("by_thread", lambda e: str(e["tid"])),
                          ("by_caller", lambda e: hex(e["caller"] - header["module_base"]))):
            grouped = {}
            for event in events:
                grouped.setdefault(key(event), []).append(event)
            require(len(grouped) <= 256, "analysis group bound exceeded; retain raw trace")
            groups[name] = {k: sync_intervals(v) for k, v in grouped.items()}
        overlap = [e for e in events if e["entry_ns"] < hi and e["exit_ns"] > lo]
        straddlers = [e for e in events if e["entry_ns"] < lo < e["exit_ns"] or e["entry_ns"] < hi < e["exit_ns"]]
        return {"schema": SYNC_SCHEMA, "valid": True, "reason": None, "evidence": evidence,
                "clock": "CLOCK_MONOTONIC", "unit": "seconds", "header": header, "completeness": footer,
                "summary": sync_intervals(events), **groups,
                "failed_errno_counts": {str(n): sum(e["return_value"] == -1 and e["errno"] == n for e in events)
                                        for n in sorted({e["errno"] for e in events if e["return_value"] == -1})},
                "application_interval": list(application_interval), "shutdown_start": shutdown_start,
                "application_overlap": sync_intervals(overlap, (lo, hi)),
                "boundary_straddling_count": len(straddlers),
                "before_application": sync_intervals([e for e in events if e["exit_ns"] <= lo]),
                "after_application": sync_intervals([e for e in events if e["entry_ns"] >= hi]),
                "shutdown_overlap": sync_intervals([e for e in events if e["exit_ns"] > shutdown],
                                                    (shutdown, round(child["exit_monotonic"]*1e9))),
                "limitations": "Elapsed calls include CPU and waiting/scheduling. Sums overlap; unions are not application delay. "
                               "FD numbers are not file identities; caller keys are image-relative return PCs, not verified symbols. "
                               "Preparation/verification not collected. No native-library internal/direct-syscall coverage."}
    except (KeyError, TypeError, ValueError, OverflowError, RecursionError) as error:
        raise TrialError("missing/corrupt sync diagnostic: " + str(error)) from error


class MeasurementTrial(Trial):
    """Opt-in, one-use staged-directory trial; never a multi-run campaign.

    Canonical references reside in a private UUID tree on a different filesystem
    for actual measurements. They and artifact logs survive guarded workspace
    cleanup. First-access-after-staging is a cache history, NOT equivalent
    residency: ext4 staging validation can warm input; myfs uses fresh FUSE.
    """
    schema = MEASUREMENT_SCHEMA

    def __init__(self, artifacts, anchor, state, processes=None, mounts=None, clock=None, host=None):
        self.validate_diagnostic_configuration(anchor, state)
        limits = state["limits"]
        measurement_limits(limits["records"], overall_seconds=limits.get("overall_seconds", 900),
                           **{k + "_seconds": v for k, v in limits["deadlines_seconds"].items()})
        if (state["backend"] == "offline-check" and limits["records"] > MAX_RECORDS
                or limits.get("evidence_bytes", EVIDENCE_BUDGET_BYTES) != EVIDENCE_BUDGET_BYTES
                or not isinstance(limits["log_bytes_per_child"], int)
                or not 1024 <= limits["log_bytes_per_child"] <= 1024**2):
            raise TrialError("invalid persisted measurement limits")
        consumed = state.get("budget", {}).get("consumed_seconds", 0)
        if (not isinstance(consumed, (int, float)) or isinstance(consumed, bool)
                or consumed < 0 or not math.isfinite(consumed)):
            raise TrialError("invalid persisted overall budget consumption")
        clock = clock or Clock()
        super().__init__(artifacts, anchor, state, processes or AccountingProcesses(clock),
                         mounts or MeasurementMounts(state.get("affinities", {}).get("daemon")), clock)
        self.host = host or MeasurementHost()
        self._budget_start = self._budget_end = None
        self._io_kind = None
        self._io_end = None

    @staticmethod
    def validate_diagnostic_configuration(anchor, state):
        # Legacy states without either field remain unprofiled; no migration writes.
        if "diagnostic" not in anchor:
            if "diagnostic" in state or "daemon_diagnostic" in anchor or "daemon_diagnostic" in state:
                raise TrialError("diagnostic configuration missing from ownership anchor")
            return
        config = anchor["diagnostic"]
        if not isinstance(config, dict):
            raise TrialError("invalid diagnostic configuration")
        sync = anchor.get("daemon_diagnostic")
        if ("daemon_diagnostic" in anchor or "daemon_diagnostic" in state):
            if (not isinstance(sync, dict) or sync != sync_configuration("sync-calls")
                    or state["backend"] != "myfs" or config.get("mode") is not None
                    or not isinstance(state.get("daemon_diagnostic"), dict)
                    or state["daemon_diagnostic"].get("configuration") != sync
                    or not isinstance(state["daemon_diagnostic"].get("trace"), dict)
                    or not re.fullmatch(r"[0-9a-f]{32}", anchor.get("daemon_sync_token", ""))
                    or not isinstance(anchor.get("daemon_sync_build"), dict)):
                raise TrialError("changed/invalid daemon diagnostic ownership configuration")
        population = ("daemon_sync_instrumented_application_completion" if sync else
                      "instrumented_application_completion" if config.get("mode") else "buffered_application_completion")
        if (not isinstance(config, dict) or config != diagnostic_configuration(config.get("mode"))
                or not isinstance(state.get("diagnostic"), dict)
                or state["diagnostic"].get("configuration") != config
                or not isinstance(state["diagnostic"].get("profile"), dict)
                or state.get("measurement_population") != population
                or state.get("baseline_eligible") is not (not config["mode"] and not sync and state["backend"] != "offline-check")):
            raise TrialError("inconsistent/changed diagnostic configuration or population")

    @property
    def sync_diagnostic(self):
        return self.anchor.get("daemon_diagnostic", {}).get("mode") == "sync-calls"

    def launch_daemon(self, command, log, limit, index):
        if not self.sync_diagnostic:
            return super().launch_daemon(command, log, limit, index)
        # taskset execs the daemon in place: only that direct child inherits this
        # FD. No tracer or new waiter. Recorder marks it CLOEXEC in its constructor.
        env = {key: value for key, value in os.environ.items() if key not in SYNC_ENV}
        if index != 1:
            if index not in (0, 2):
                raise TrialError("unexpected diagnostic daemon lifecycle")
            return self.processes.launch(command, log, limit, env=env)
        if self.state["phases"][-1]["name"] != "application_readiness":
            raise TrialError("sync recorder only allowed for application mount")
        directory = profile_directory(self.artifacts, self.anchor)
        raw_name, receipt_name = sync_names(self.anchor["trial_id"])
        fd = None
        try:
            for name in (raw_name, receipt_name):
                if os.path.lexists(self.artifacts / name):
                    raise TrialError("existing sync diagnostic artifact; refusing overwrite")
            fd = os.open(raw_name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                         0o600, dir_fd=directory)
            info = os.fstat(fd)
            profile_directory_check(self.artifacts, directory, self.anchor)
            record = {"artifact": str(self.artifacts / raw_name),
                      "identity": {"device": info.st_dev, "inode": info.st_ino, "uid": info.st_uid},
                      "valid": None, "reason": "owned daemon has not finalized trace", "daemon": None}
            self.state["daemon_diagnostic"]["trace"] = record
            self.save()
            env.update(M0_SYNC_FD=str(fd), M0_SYNC_TRIAL=self.anchor["trial_id"],
                       M0_SYNC_TOKEN=self.anchor["daemon_sync_token"])
            identity = self.processes.launch(command, log, limit, pass_fds=(fd,), env=env)
            record["daemon"] = identity
            self.save()
            return identity
        finally:
            if fd is not None:
                os.close(fd)
            os.close(directory)

    def check_sync_trace(self, *, publish=False):
        if not self.sync_diagnostic:
            return
        previous = self.state["daemon_diagnostic"]["trace"]
        directory = None
        try:
            self.no_mounts()
            child = next((c for c in self.state["children"] if c.get("identity") == previous.get("daemon")), None)
            if child is None or child.get("exit_status") != 0 or child.get("reap_count") != 1:
                raise TrialError("sync diagnostic normal exactly-once daemon exit unconfirmed")
            if len(self.state["mounts"]) < 2 or self.state["mounts"][1]["daemon"] != child["identity"]:
                raise TrialError("sync diagnostic daemon lifecycle identity mismatch")
            if not self.state["mounts"][1]["unmounted"] or self.state["mounts"][1]["daemon_exit_status"] != 0:
                raise TrialError("sync diagnostic unmount/exit unconfirmed")
            self.confirm_exits()
            directory = profile_directory(self.artifacts, self.anchor)
            deadline = self.deadline("io")
            def guard():
                if self.clock.now() >= deadline:
                    raise TrialError("sync diagnostic validation timeout")
                self.check_budget()
            raw_name, receipt_name = sync_names(self.anchor["trial_id"])
            raw, file = profile_file_read(directory, raw_name, SYNC_MAX_BYTES, guard)
            if file["identity"] != previous["identity"]:
                raise TrialError("sync artifact identity substituted")
            interval = self.state["measurements"]["application"]["wall_seconds"]["interval"]
            shutdown = next(p["start_monotonic"] for p in self.state["phases"] if p["name"] == "daemon_shutdown")
            report = decode_sync_trace(raw, trial_id=self.anchor["trial_id"], token=self.anchor["daemon_sync_token"],
                child=child, application_interval=interval, shutdown_start=shutdown,
                evidence=self.state["evidence"], guard=guard)
            report.update(artifact=str(self.artifacts / raw_name), file=file, daemon=child["identity"],
                          executable=self.anchor["daemon_sync_build"]["executable"],
                          module_build_id=self.anchor["daemon_sync_build"]["provenance"]["daemon_sync"]["module_build_id"])
            if publish:
                fd = os.open(receipt_name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=directory)
                try:
                    BoundedProfileWriter(fd, directory, receipt_name, 1024**2).write(
                        (json.dumps(report, sort_keys=True, allow_nan=False) + "\n").encode("ascii"))
                finally:
                    os.close(fd)
            receipt_raw, receipt = profile_file_read(directory, receipt_name, 1024**2, guard)
            if json.loads(receipt_raw) != report:
                raise TrialError("sync diagnostic receipt does not match independently decoded events")
            result = {"artifact": str(self.artifacts / raw_name), "identity": file["identity"],
                      "valid": True, "reason": None, "daemon": child["identity"],
                      "file": file, "receipt": receipt, "report": report}
            if not publish and result != previous:
                raise TrialError("sync diagnostic evidence changed after publication")
            profile_directory_check(self.artifacts, directory, self.anchor)
            self.state["daemon_diagnostic"]["trace"] = result
            self.save()
        except (OSError, ValueError, KeyError, TypeError, StopIteration, TrialError) as error:
            self.state["daemon_diagnostic"]["trace"] = dict(previous, valid=False, reason=str(error))
            self.save()
            raise TrialError("sync diagnostic invalid: " + str(error)) from error
        finally:
            if directory is not None:
                os.close(directory)

    @property
    def python_diagnostic(self):
        return self.anchor.get("diagnostic", {}).get("mode") == "python-cpu"

    def collect_python_profile(self, child):
        if not self.python_diagnostic:
            return
        previous = self.state["diagnostic"]["profile"]
        deadline = self.deadline("io")
        def guard():
            self.check_budget()
            if self.clock.now() >= deadline:
                raise TrialError("diagnostic evidence validation timeout")
        try:
            value = read_python_profile(self.artifacts, self.anchor, self.state["evidence"], child, guard,
                                        previous if previous.get("valid") is True else None)
            self.state["diagnostic"]["profile"] = value
        except TrialError as error:
            self.state["diagnostic"]["profile"] = dict(previous, valid=False, reason=str(error))
            self.save()
            raise

    def check_python_profile(self):
        if self.python_diagnostic:
            child = next((c for c in self.state["children"] if c.get("identity") == self.state.get("application_identity")), None)
            if child is None or child.get("exit_status") != 0:
                raise TrialError("diagnostic application normal exit unconfirmed")
            self.collect_python_profile(child)

    @property
    def canonical_directory(self):
        shared = self.anchor.get("shared_canonical")
        if shared is not None:
            campaign_id = CanonicalBundle.campaign(shared["owner"]["campaign_id"])
            return Path(self.anchor["canonical_parent"]) / ("m0-shared-canonical-" + campaign_id)
        return Path(self.anchor["canonical_parent"]) / ("m0-canonical-" + self.anchor["trial_id"])

    @classmethod
    def prepare(cls, parent, *, canonical_parent=None, canonical_bundle=None, campaign_id=None,
                backend, records=MAX_RECORDS,
                seed=fixture.DEFAULT_SEED, expected_device=None, daemon=None, build_provenance=None,
                application_affinity=None, daemon_affinity=None, materialize=False,
                processes=None, mounts=None, host=None, clock=None,
                readiness_seconds=10, application_seconds=30, shutdown_seconds=30,
                io_seconds=30, staging_seconds=None, verification_seconds=None,
                overall_seconds=900, log_bytes=1024**2, diagnostic=None, daemon_diagnostic=None):
        diagnostic_config = diagnostic_configuration(diagnostic)
        sync_config = sync_configuration(daemon_diagnostic)
        if daemon_diagnostic and (backend != "myfs" or diagnostic is not None):
            raise TrialError("daemon diagnostic requires myfs and cannot combine with Python profiling")
        # Pin reviewed executable/provenance before any expensive allocation.
        sync_binary = sync_build(daemon, build_provenance) if daemon_diagnostic else None
        config = measurement_limits(records, readiness_seconds=readiness_seconds,
            application_seconds=application_seconds, shutdown_seconds=shutdown_seconds,
            io_seconds=io_seconds, staging_seconds=staging_seconds,
            verification_seconds=verification_seconds, overall_seconds=overall_seconds)
        deadlines = config["deadlines_seconds"]
        if backend not in ("ext4", "myfs", "offline-check") or (backend == "offline-check" and records > MAX_RECORDS):
            raise TrialError("invalid backend; OFFLINE self-test record limit remains 0..32768")
        if not isinstance(log_bytes, int) or isinstance(log_bytes, bool) or not 1024 <= log_bytes <= 1024**2:
            raise TrialError("invalid measurement deadline/log limit")
        clock = clock or Clock()
        preparation_start = clock.now()
        if backend != "offline-check" and not expected_device:
            raise TrialError("measurement requires explicit expected backing device major:minor")
        if backend == "myfs" and (daemon is None or build_provenance is None):
            raise TrialError("myfs measurement requires daemon and build provenance JSON")
        if (canonical_parent is None) == (canonical_bundle is None):
            raise TrialError("select exactly one canonical-parent or canonical-bundle")
        if canonical_bundle is None and campaign_id is not None:
            raise TrialError("campaign-id requires a shared canonical-bundle")
        shared = None
        if canonical_bundle is not None:
            validation_end = preparation_start + min(io_seconds, overall_seconds)
            def reference_guard():
                if clock.now() >= validation_end:
                    raise TrialError("shared canonical preparation validation timeout")
            shared = CanonicalBundle.load(canonical_bundle, campaign_id=campaign_id,
                records=records, seed=seed, guard=reference_guard, mounts=mounts)
            canonical_parent = shared.root.parent
        parent, canonical_parent = strict_path(parent), strict_path(canonical_parent)
        if shared and any(path == parent or path in parent.parents
                          for path in (shared.root, shared.artifacts)):
            raise TrialError("trial parent overlaps campaign-owned canonical/evidence")
        # Ownership is checked before creating any directory, unlike adopting existing roots.
        parent_identity, canonical_parent_identity = directory_identity(parent), directory_identity(canonical_parent)
        trial_id = uuid.uuid4().hex
        artifacts, workspace = parent / ("m0-artifacts-" + trial_id), parent / ("m0-work-" + trial_id)
        canonical = shared.root if shared else canonical_parent / ("m0-canonical-" + trial_id)
        for path in (artifacts, workspace, workspace / "application", workspace / "backing",
                     *((canonical,) if shared is None else ())):
            path.mkdir(mode=0o700)
        anchor = {"schema": cls.schema, "trial_id": trial_id, "parent": str(parent),
                  "parent_identity": parent_identity, "canonical_parent": str(canonical_parent),
                  "canonical_parent_identity": canonical_parent_identity,
                  "canonical_identity": directory_identity(canonical),
                  "identities": {name: directory_identity(path) for name, path in (
                      ("artifacts", artifacts), ("workspace", workspace),
                      ("application", workspace / "application"), ("backing", workspace / "backing"))}}
        if shared:
            anchor["shared_canonical"] = shared.reference()
        anchor["diagnostic"] = diagnostic_config
        if daemon_diagnostic:
            anchor["daemon_diagnostic"] = sync_config
            anchor["daemon_sync_build"] = sync_binary
            anchor["daemon_sync_token"] = uuid.uuid4().hex
        if diagnostic:
            anchor["diagnostic_application"] = {"path": str(APPLICATION), "file": profile_application_file(APPLICATION)}
        for path in (artifacts, workspace, *((canonical,) if shared is None else ())):
            exclusive_json(path / "owner.json", anchor)
        state = {"schema": cls.schema, "trial_id": trial_id, "anchor": anchor, "backend": backend,
                 "label": "OFFLINE correctness and artifact check" if backend == "offline-check" else "single staged-directory measurement",
                 "roots": {"artifacts": str(artifacts), "workspace": str(workspace),
                           "application": str(workspace / "application"), "backing": str(workspace / "backing")},
                 "canonical_root": str(canonical), "expected_device": expected_device,
                 "limits": {"records": records, "deadlines_seconds": deadlines, "log_bytes_per_child": log_bytes,
                            "overall_seconds": overall_seconds, "evidence_bytes": EVIDENCE_BUDGET_BYTES,
                            "helper_termination_grace_seconds": 2, "accounting_entries": 4096},
                 "fixture_size": {k: config[k] for k in ("records", "input_bytes", "size_class")},
                 "budget": {"consumed_seconds": 0, "events": [],
                            "scope": "active preparation plus execution; excludes idle between invocations and guarded cleanup"},
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
        state["diagnostic"] = {"configuration": dict(diagnostic_config),
            "profile": {"valid": None, "reason": "application not executed" if diagnostic else "diagnostic not selected",
                        "artifact": str(artifacts / profile_names(trial_id)[0]) if diagnostic else None,
                        "sha256": None, "byte_count": None, "function_count": None, "total_calls": None}}
        state["measurement_population"] = "instrumented_application_completion" if diagnostic else "buffered_application_completion"
        state["baseline_eligible"] = not diagnostic and backend != "offline-check"
        if diagnostic:
            state["label"] = ("OFFLINE CPU diagnostic and correctness check" if backend == "offline-check"
                              else "instrumented Python CPU diagnostic; excluded from baseline populations")
        if daemon_diagnostic:
            state["daemon_diagnostic"] = {"configuration": dict(sync_config),
                "trace": {"valid": None, "reason": "application daemon not executed", "artifact": None},
                "preparation": {"value": None, "reason": "NOT COLLECTED: recorder disabled for preparation mount"},
                "verification": {"value": None, "reason": "NOT COLLECTED: recorder disabled for verification mount"}}
            state["measurement_population"] = "daemon_sync_instrumented_application_completion"
            state["baseline_eligible"] = False
            state["label"] = "sync-call daemon diagnostic; excluded from baseline populations"
        state["canonical_reference"] = {"ownership": "campaign" if shared else "trial-retained",
            "campaign_id": campaign_id if shared else None,
            "receipt": str(shared.artifacts / "state.json") if shared else None,
            "new_bundle_generated": shared is None,
            "limitation": "shared reference reads may influence preparation/cache history; no equivalent residency claim"}
        state["canonical_checks"] = []
        state["cache_history"]["shared_canonical_reuse"] = shared is not None
        trial = cls(artifacts, anchor, state, processes, mounts, clock, host)
        state["evidence"] = "mocked" if any(a.evidence == "mocked" for a in (trial.processes, trial.mounts, trial.host)) else (
            "actual offline execution" if backend == "offline-check" else "actual")
        trial.save()
        try:
            trial.begin_budget("prepare", preparation_start)
            def preflight():
                trial.no_mounts()
                trial.check_paths()
                state["host"] = trial.host.metadata()
                state["device"] = trial.host.device(trial.workspace)
                state["canonical_device"] = trial.host.device(trial.canonical_directory)
                if shared:
                    state["campaign_receipt_device"] = trial.host.device(shared.artifacts)
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
                deadline = trial.deadline("io")
                if shared is None:
                    command = [sys.executable, "-B", "-c", GENERATION_CODE, str(Path(__file__).parent),
                               str(canonical), str(records), str(seed)]
                    state["commands"].append(command)
                    result = trial.processes.run(command, artifacts / "canonical-preparation.log", log_bytes, deadline)
                    if result["exit_status"] != 0 or trial.clock.now() >= deadline:
                        raise TrialError("canonical preparation failed/timeout; partial fixture unvalidated")
                else:
                    trial.shared_bundle(content=True)
                    exclusive_json(artifacts / "canonical-preparation.json", {
                        "operation": "validated shared reuse; no generation", "reference": anchor["shared_canonical"],
                        "evidence": state["evidence"], "monotonic": trial.clock.now()})
                manifest = read_json(canonical / "input.manifest.json")
                expected = summary(canonical / "expected.csv")
                if manifest["byte_count"] != config["input_bytes"] or manifest["record_count"] != records:
                    raise TrialError("canonical generator size mismatch")
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
        finally:
            trial.finish_budget()
        return trial

    def check_paths(self, mounted=False):
        self.validate_diagnostic_configuration(self.anchor, self.state)
        super().check_paths(mounted)
        if self.sync_diagnostic:
            pinned = self.anchor["daemon_sync_build"]
            if sync_build(pinned["executable"]["path"], pinned["provenance"]) != pinned:
                raise TrialError("diagnostic source/executable provenance changed")
            if self.state.get("daemon") is not None and any(
                    self.state["daemon"].get(k) != v for k, v in pinned["executable"].items()):
                raise TrialError("diagnostic executable state differs from ownership anchor")
        parent = strict_path(self.anchor["canonical_parent"])
        if (directory_identity(parent) != self.anchor["canonical_parent_identity"]
                or str(self.canonical_directory) != self.state["canonical_root"]
                or directory_identity(strict_path(self.canonical_directory)) != self.anchor["canonical_identity"]
                or (not self.anchor.get("shared_canonical")
                    and read_json(self.canonical_directory / "owner.json") != self.anchor)
                or self.workspace == self.canonical_directory or self.workspace in self.canonical_directory.parents):
            raise TrialError("canonical ownership/path identity mismatch")
        if self.anchor.get("shared_canonical"):
            self.shared_bundle(content=False)

    def shared_bundle(self, *, content):
        reference = self.anchor["shared_canonical"]
        owner = reference["owner"]
        if (owner["canonical_parent"] != self.anchor["canonical_parent"]
                or owner["canonical_identity"] != self.anchor["canonical_identity"]
                or self.state.get("canonical_reference", {}).get("campaign_id") != owner["campaign_id"]
                or self.state["canonical_reference"].get("ownership") != "campaign"):
            raise TrialError("shared canonical trial anchor/state mismatch")
        deadline = self.deadline("io")
        def guard():
            self.check_budget()
            if self.clock.now() >= deadline:
                raise TrialError("shared canonical validation timeout")
        return CanonicalBundle.load(self.canonical_directory, campaign_id=owner["campaign_id"],
            records=self.state["limits"]["records"], seed=self.state["seed"], expected=reference,
            guard=guard, mounts=self.mounts, content=content)

    def canonical_snapshot(self):
        if self.anchor.get("shared_canonical"):
            return self.shared_bundle(content=True).state["canonical"]
        return super().canonical_snapshot()

    def check_canonical(self):
        super().check_canonical()
        if self.anchor.get("shared_canonical"):
            self.state["canonical_checks"].append({"monotonic": self.clock.now(),
                "outcome": "passed", "population": "campaign-owned canonical input/manifest/expected",
                "evidence": self.state["evidence"]})

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
        devices = [("device", current), ("canonical_device", canonical)]
        receipt = None
        if self.anchor.get("shared_canonical"):
            receipt = self.host.device(self.shared_bundle(content=False).artifacts)
            devices.append(("campaign_receipt_device", receipt))
        for field, observed in devices:
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
            if receipt and (not receipt.get("fstype") or receipt["fstype"] in ("tmpfs", "ramfs")):
                raise TrialError("campaign receipt storage volatile or unknown; select nonvolatile evidence storage")

    def resource_guard(self):
        bundle = None
        plan = measurement_capacity(69 + self.state["limits"]["records"] * 128,
            canonical_tmpfs=self.state["canonical_device"]["fstype"] == "tmpfs",
            same_device=self.state["device"]["device"] == self.state["canonical_device"]["device"],
            shared=bool(self.anchor.get("shared_canonical")))
        if self.anchor.get("shared_canonical"):
            bundle = self.shared_bundle(content=False)
            plan["shared_canonical_allocated_bytes"] = bundle.allocation()
            plan["shared_canonical_logical_bytes"] = sum(bundle.state["canonical"][n]["byte_count"]
                                                        for n in ("input.csv", "expected.csv"))
            plan["shared_allocation_limitation"] = "existing checkpoint allocation; already reflected in free space/RAM, not a new per-trial copy or peak"
        self.state["capacity_plan"] = plan
        populations = [("backing", self.workspace, plan["backing_required_bytes"]),
                       ("canonical", self.canonical_directory, plan["canonical_required_bytes"])]
        if bundle:
            populations.append(("campaign_evidence", bundle.artifacts, 1024**3 + EVIDENCE_BUDGET_BYTES))
        for role, path, required in populations:
            try:
                values = self.host.resources(path)
            except (OSError, ValueError) as error:
                values = {"available_disk_bytes": None, "available_memory_bytes": None,
                          "reason": str(error), "evidence": self.host.evidence}
            observation = dict(values, path=str(path), role=role, monotonic=self.clock.now(),
                               required_disk_bytes=required, required_memory_bytes=plan["memory_required_bytes"])
            self.state["resource_observations"].append(observation)
            if any(observation.get(k) is None for k in ("available_disk_bytes", "available_memory_bytes")):
                observation.setdefault("reason", "required capacity observation unavailable")
                raise TrialError(f"{role} capacity observation unavailable")
            if observation["available_disk_bytes"] < required:
                raise TrialError(f"insufficient {role} user-accessible disk space")
            if observation["available_memory_bytes"] < plan["memory_required_bytes"]:
                raise TrialError("insufficient available memory")
        self.check_budget()

    def begin_budget(self, action, start=None):
        # Persist accumulated active time, not an absolute monotonic deadline
        # which would incorrectly consume time while the user reviews preparation.
        budget = self.state.setdefault("budget", {"consumed_seconds": 0, "events": [],
            "scope": "legacy preparation not budgeted; active execution only"})
        self._budget_start = self.clock.now() if start is None else start
        self._budget_end = self._budget_start + self.state["limits"].get("overall_seconds", 900) - budget["consumed_seconds"]
        budget["events"].append({"action": action, "start_monotonic": self._budget_start,
                                 "deadline_monotonic": self._budget_end, "end_monotonic": None})
        self.check_budget()

    def finish_budget(self):
        if self._budget_start is not None:
            end = self.clock.now()
            budget = self.state["budget"]
            budget["consumed_seconds"] += end - self._budget_start
            budget["events"][-1]["end_monotonic"] = end
            self._budget_start = self._budget_end = None
            if (budget["consumed_seconds"] >= self.state["limits"].get("overall_seconds", 900)
                    and self.state["outcome"] in ("prepared", "passed")):
                self.fail(TrialError("overall active trial timeout during final bookkeeping; evidence retained"))
            self.save()

    def check_budget(self):
        if self._budget_end is not None and self.clock.now() >= self._budget_end:
            raise TrialError("overall active trial timeout; evidence retained")
        if self._io_end is not None and self.clock.now() >= self._io_end:
            raise TrialError(f"{self._io_kind} timeout; includes canonical integrity checks")

    def canonical_summary(self, path):
        return summary(path, self.check_budget)

    def deadline(self, name):
        self.check_budget()
        if name == "io" and self._io_kind is not None:
            name = "staging" if self._io_kind == "stage" else "verification"
        deadlines = self.state["limits"]["deadlines_seconds"]
        end = self.clock.now() + deadlines.get(name, deadlines["io"])
        return min([end, *[v for v in (self._budget_end, self._io_end) if v is not None]])

    def check_evidence_budget(self):
        # Only the separate artifact tree, never backing storage while the app
        # runs. This checkpoint scan uses the same no-follow/inode safeguards.
        self.check_paths(mounted=self.mount_identity is not None)
        start = self.clock.now()
        try:
            observed = account_owned_tree(self.artifacts, self.anchor["identities"]["artifacts"],
                                          self.deadline("io"), self.clock)
            value = max(observed["regular_apparent_bytes"], observed["allocated_bytes"])
            self.state["evidence_usage"] = metric(value, "bytes", "no-follow artifact inventory; max(apparent, allocated)",
                str(self.artifacts), [start, self.clock.now()], self.state["evidence"])
        except (OSError, TrialError) as error:
            self.state["evidence_usage"] = metric(None, "bytes", "no-follow artifact inventory",
                str(self.artifacts), None, self.state["evidence"], str(error))
            raise
        if value > self.state["limits"].get("evidence_bytes", EVIDENCE_BUDGET_BYTES):
            raise TrialError("trial evidence budget exhausted; retain all artifacts")

    def io(self, kind):
        self._io_kind = kind
        try:
            self._io_end = self.deadline("io")
            self.check_device()
            self.resource_guard()
            result = super().io(kind)
            self.check_budget()
            return result
        finally:
            self._io_kind = None
            self._io_end = None

    def phase(self, name, action):
        self.observe_daemon(name + ":start")
        def guarded():
            self.check_budget()
            self.check_evidence_budget()
            value = action()
            self.check_budget()
            self.check_evidence_budget()
            return value
        try:
            return super().phase(name, guarded)
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
        if self.sync_diagnostic and self.processes.evidence != "mocked":
            observed = os.stat(f"/proc/{self.daemon_identity['pid']}/exe")
            pinned = self.anchor["daemon_sync_build"]["executable"]
            if (not self.processes.owned(self.daemon_identity)
                    or (observed.st_dev, observed.st_ino) != (pinned["device"], pinned["inode"])):
                raise TrialError("diagnostic child is not the verified direct daemon executable")
        if self.clock.now() >= deadline:
            raise TrialError("readiness/daemon affinity timeout")

    def unmount(self):
        identity = self.daemon_identity
        super().unmount()
        entry = next(e for e in self.processes.entries if e.get("identity") == identity)
        resources = entry.get("resources") or resource_metrics(None, self.state["evidence"], identity, None, None)
        self.state["measurements"]["daemon_lifecycles"].append(resources)
        self.record_daemon_warnings()
        if self.sync_diagnostic and len(self.state["mounts"]) == 2:
            self.check_sync_trace(publish=True)
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
        command = [sys.executable, "-B", str(APPLICATION), str(self.root)]
        if self.python_diagnostic:
            command = [sys.executable, "-B", "-c", PYTHON_PROFILE_CODE, str(Path(__file__).parent),
                       str(self.artifacts), json.dumps(self.anchor), self.state["evidence"],
                       str(APPLICATION), str(self.root)]
        command = placed_command(command, self.state["affinities"]["application"])
        self.state["commands"].append(command)
        deadline = self.deadline("application")
        identity = self.processes.launch(command, self.artifacts / "application.log",
                                         self.state["limits"]["log_bytes_per_child"])
        self.state["application_identity"] = identity
        if self.python_diagnostic:
            next(e for e in self.processes.entries if e["identity"] == identity)["diagnostic_cwd"] = os.getcwd()
        try:
            self.state["application_affinity_observation"] = self.check_affinity(identity, "application", deadline)
            self.state["application_start_counters"] = self.processes.counters(identity)
            code = self.processes.wait(identity, deadline)
        except TrialError as failure:
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
                try:
                    self.complete_application()
                except TrialError as profile_error:
                    raise TrialError(f"{failure}; diagnostic invalid: {profile_error}") from profile_error
            raise
        self.state["application_result"] = {"exit_status": code, "identity": identity, "evidence": self.state["evidence"]}
        entry = next(e for e in self.processes.entries if e.get("identity") == identity)
        if entry["exit_monotonic"] >= deadline:
            raise TrialError("application timeout")
        if code != 0:
            try:
                self.complete_application()
            except TrialError as error:
                raise TrialError(f"application exit: {code}; diagnostic invalid: {error}") from error
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
        if self.sync_diagnostic:
            values["wall_seconds"]["population"]["completion"] = "daemon_sync_instrumented_application_completion"
            values["wall_seconds"]["limitation"] = "daemon instrumentation active; diagnostic only, excluded from baseline"
        if self.python_diagnostic:
            values["wall_seconds"]["population"]["completion"] = "instrumented_application_completion"
            values["wall_seconds"]["limitation"] = "whole child includes startup, launcher and profile serialization; excluded from baseline"
            self.collect_python_profile(entry)

    def storage_accounting(self):
        self.no_mounts()
        self.check_paths()
        self.check_device()
        self.resource_guard()
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
            self.begin_budget("run")
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
            self.check_budget()
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
        except (OSError, ValueError, TrialError) as error:
            try:
                self.record_daemon_warnings()
            except (OSError, TrialError) as warning_error:
                self.state["warning_collection_error"] = str(warning_error)
            self.state.setdefault("errors", []).append(str(error))
            if self.state["phases"][-1]["name"] == "post_exit_storage_accounting":
                self.state["storage"] = {"allocated_bytes": None, "regular_apparent_bytes": None,
                                         "reason": "accounting incomplete: " + str(error), "errors": [str(error)]}
            if self.sync_diagnostic:
                self.state["daemon_diagnostic"]["trace"].update(valid=False, reason=str(error))
            self.fail(error)
        finally:
            self.finish_budget()
        return self.state["outcome"] == "passed"

    def verify(self):
        if self.sync_diagnostic:
            try:
                self.check_sync_trace()
            except (OSError, TrialError) as error:
                self.fail(error)
                return False
        if self.python_diagnostic:
            try:
                self.check_python_profile()
            except TrialError as error:
                self.fail(error)
                return False
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
        self.check_python_profile()
        self.check_sync_trace()
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
    canonical = commands.add_parser("canonical-prepare")
    canonical.add_argument("--canonical-parent", type=Path, required=True)
    canonical.add_argument("--evidence-parent", type=Path, required=True)
    canonical.add_argument("--campaign-id")
    canonical.add_argument("--records", type=int, default=MAX_RECORDS)
    canonical.add_argument("--seed", type=int, default=fixture.DEFAULT_SEED)
    canonical.add_argument("--io-seconds", type=float, default=30)
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
        references = command.add_mutually_exclusive_group(required=True)
        references.add_argument("--canonical-parent", type=Path)
        references.add_argument("--canonical-bundle", type=Path)
        command.add_argument("--campaign-id", help="required with a shared bundle; 32 lowercase hex UUID")
        command.add_argument("--records", type=int, default=100 if name == "measure-self-test" else MAX_RECORDS,
                             help="0..1048576 for measurement (default 32768); OFFLINE self-test 0..32768 (default 100)")
        command.add_argument("--seed", type=int, default=fixture.DEFAULT_SEED)
        command.add_argument("--materialize", action="store_true")
        if name == "measure-prepare":
            command.add_argument("--backend", choices=("ext4", "myfs"), required=True)
            command.add_argument("--expected-device", required=True)
            command.add_argument("--daemon", type=Path)
            command.add_argument("--build-provenance", type=Path)
            command.add_argument("--application-cpus", type=cpu_list)
            command.add_argument("--daemon-cpus", type=cpu_list)
            command.add_argument("--daemon-diagnostic", choices=("sync-calls",), default=None,
                                 help="myfs only; isolated verified sync wrapper build; excludes baseline")
            command.add_argument("--diagnostic", choices=("python-cpu",), default=None,
                                 help="opt-in application CPU profile; instrumented completion is excluded from baseline")
        for limit, default in (("readiness", 10), ("application", 30), ("shutdown", 30), ("io", 30)):
            command.add_argument(f"--{limit}-seconds", type=float, default=default)
        for name in ("staging", "verification"):
            command.add_argument(f"--{name}-seconds", type=float, default=None,
                                 help="measurement only; defaults to --io-seconds; finite 0 < seconds <=300")
        command.add_argument("--overall-seconds", type=float, default=900,
                             help="active prepare+run budget; finite 0 < seconds <=900; excludes review idle")
    args = parser.parse_args(argv)
    trial = None
    try:
        if args.command == "canonical-prepare":
            bundle = CanonicalBundle.prepare(args.canonical_parent, evidence_parent=args.evidence_parent,
                campaign_id=args.campaign_id, records=args.records, seed=args.seed, io_seconds=args.io_seconds)
            passed = bundle.state["outcome"] == "ready"
            print(json.dumps({"passed": passed, "outcome": bundle.state["outcome"],
                "label": "canonical preparation only; not a filesystem measurement",
                "bundle": str(bundle.root), "artifacts": str(bundle.artifacts),
                "campaign_id": bundle.state["campaign_id"], "canonical": bundle.state.get("canonical"),
                "error": bundle.state["error"]}, sort_keys=True))
            return 0 if passed else 1
        elif args.command in ("measure-prepare", "measure-self-test"):
            trial = MeasurementTrial.prepare(args.parent, canonical_parent=args.canonical_parent,
                canonical_bundle=args.canonical_bundle, campaign_id=args.campaign_id,
                backend="offline-check" if args.command == "measure-self-test" else args.backend,
                records=args.records, seed=args.seed, expected_device=getattr(args, "expected_device", None),
                daemon=getattr(args, "daemon", None), build_provenance=getattr(args, "build_provenance", None),
                application_affinity=getattr(args, "application_cpus", None), daemon_affinity=getattr(args, "daemon_cpus", None),
                materialize=args.materialize, diagnostic=getattr(args, "diagnostic", None),
                daemon_diagnostic=getattr(args, "daemon_diagnostic", None),
                **{name + "_seconds": getattr(args, name + "_seconds")
                                               for name in ("readiness", "application", "shutdown", "io",
                                                            "staging", "verification", "overall")})
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
