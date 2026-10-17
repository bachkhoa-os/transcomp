# M0 profiling evidence and proposed acceptance decision

**Status: PROPOSED — AWAITING USER REVIEW**

Checkpoint: M0-D9b. Evidence review date: 2026-10-17.
Repository reviewed: `feat/m0-d1a-single-trial-measurement` at
`6217edf82b59737c5b048d07538612fd20491b67`, initially clean.
This document records a proposed milestone decision, not approval to close M0
or authorization to start M1 implementation.

## 1. Purpose, authority and recommendation

The product direction is a directly readable and writable compressed directory
for raw Linux data. Existing applications use ordinary file paths without
changing their code or data formats. The synthetic experiment below is evidence
about one workflow, not a general product capability or customer-data benchmark.

The supplied English roadmap, updated 2026-10-11, states this exact M0 exit
criterion in its milestone table:

> Reproducible baseline, raw evidence, correct output, and separate lists of compatibility gaps and measured bottlenecks.

Source: [English roadmap](../myfs-roadmap-2026-10-10-en.md), section 4, line 37
at the reviewed checkout. The roadmap is a local, ignored document; it may not
be available in a remote clone. Its historical M0-D statement that no timing
baseline or profiling has run is superseded by the verified evidence indexed
below. The roadmap itself has not been changed.

The roadmap requires profiling before optimization; it does not explicitly
require a nonempty list of proven bottlenecks. Its measurement principles require
separate populations, repeated/alternating controls, raw evidence, correctness,
honest cache/durability qualifications and non-duplicated storage accounting.
M1 concerns explicit visibility, sync, error and recovery contracts and tests;
M2 concerns workflow compatibility; M3 permits evidence-driven optimization.

**Recommendation:** accept the scoped M0 evidence package after explicit user
review of the criterion assessment in section 11. Defer additional callback
instrumentation. Recommend a separately authorized M1 contract/test audit after
that decision; no M1 work is started here.

**No causally established native filesystem bottleneck has been identified by the M0 diagnostic campaign.**

This does not mean that no bottleneck exists. It means the collected populations
do not establish a native cause of the historical ext4/myfs timing difference.
The compatibility register and measured-cost/bottleneck disposition are separate
deliverables below. Acceptance of that honest disposition against the roadmap's
wording is an explicit reviewer decision, not an assumption made by this commit.

Evidence terms used throughout:

- **OBSERVED:** recorded historical execution, process or validation evidence.
- **RECALCULATED:** arithmetic or hashes independently checked from retained raw files.
- **SOURCE-INSPECTED:** behavior present in the pinned source; not a newly executed test.
- **INFERRED:** a conclusion supported by observations but not directly measured.
- **UNTESTED / UNKNOWN:** a contract, mechanism or effect not established by this evidence.
- **PROPOSED / NOT EXECUTED:** future work, requiring its own authorization.

## 2. Reproducible workflow and measurement contract

### 2.1 Workload identities

| Item | Established configuration |
|---|---|
| Generator | `myfs-m0-csv-v1`, [m0_fixture.py](m0_fixture.py) |
| Input / manifest schema | `myfs-m0-input-v1` / `myfs-m0-manifest-v1` |
| Independent oracle | `myfs-m0-oracle-v1`; `expected_lines` and `validate_output` in the fixture module |
| Seed / records | `20261010` / `1,048,576` data records |
| Input | `134,217,797` bytes, including header; each data record is 128 ASCII bytes |
| Expected output | `39,079,244` bytes / `346,323` data rows |
| Application | Unchanged [m0_csv_workflow.py](m0_csv_workflow.py), ordinary paths `ROOT/input.csv` and exclusively created `ROOT/output.csv` |
| Interpreter | Retained execution identity `/usr/bin/python3.13`; per-campaign source records pin its device/inode and hash |
| Buffering / encoding | 1 MiB file buffers, ASCII, CSV minimal quoting, LF output |
| Transform | Keep `status == "ok"` and integer `quantity >= 50`; output integer `quantity * price_cents`; preserve accepted row order |
| Output columns | `event_id,sensor_id,region,total_cents,note` |

Input SHA-256:

```text
7a93f9ff097504b34adebed19a756d9ab01b45218dd1ac3c6752979d06eefa55
```

Expected-output SHA-256:

```text
be92af52dc45bb5cf808c52a1fff912518bbd68faa826cf3e8651924ee1ff44c
```

The oracle uses independent `DictReader` parsing and its own output escaping; it
does not import or call the application's transform. Exact stream comparison,
hashes, byte counts and completed data-row counts are retained. Application and
oracle still share the Python runtime and standard-library CSV facilities; this
is an independent transformation implementation, not independent hardware.

Application source SHA-256:
`29b9e4092932b9dbc8a284655cf8e30c6c484c83738824115bf286ffe5e02bf1`.
Generator/oracle module SHA-256:
`efe07160db818ccb042e0bcc1537e9206188889bf67c981c8d991dbcb6328149`.
Both match the current source and retained campaigns.

**Output reduction from filtering and schema reduction is not compression.**
The synthetic structured fixture is compressible; measured backing allocation
does not establish a production compression ratio for arbitrary raw data.

### 2.2 Backend, lifecycle and shared references

The direct backend runs the application in an ordinary directory on verified
ext4. The myfs backend exposes an application mount with backing on the same
verified ext4 device, historically `/dev/nvme0n1p7` (`259:7`). Only the application
root changes in the baseline comparison. Mount source tokens, executable and
owned-process identities are checked; there is no ordinary-directory fallback
for a requested myfs trial.

The normal myfs lifecycle is preparation mount, staging and exact input
validation, normal unmount/daemon exit, fresh application mount, non-mutating
readiness, unchanged application execution, owned-child completion, normal
application-daemon shutdown, fresh verification mount, exact oracle/remount
verification, final normal unmount/exit, post-exit accounting and guarded cleanup.
Preparation, shutdown, validation and accounting are outside primary wall time.
Readiness does not pre-read the staged CSV.

Shared canonical schema `myfs-m0-shared-canonical-v1` pins campaign ownership,
generator/schema/seed/count, directory and file identities, hashes and sizes.
One bundle serves independent trials within a campaign. Its private tmpfs data
and persistent campaign receipt are outside disposable application/backing
workspaces. Integrity is checked at the implemented preparation/run/verification/
cleanup gates. Trial cleanup does not delete shared references. Reuse is not
silently regenerated when identities or contents fail validation. Hash/ownership
checks are not a security boundary against a malicious process with the same UID.

Canonical reads and staging influence preparation/cache history. The retained
cohort is **first-access-after-staging**, not a verified cold-cache condition.
Ext4 input can remain warm after exact validation; a fresh FUSE connection has a
different history. `kernel_cache=1` is set in [main.c](../src/main.c), line 11.
Equal cache residency, equal internal synchronization and isolated FUSE overhead
have not been established. No global cache dropping or direct-I/O experiment was
used.

### 2.3 Placement, timing and safety

Historical placement: application CPU 0, daemon/worker CPUs 2,4,6, execution
controller CPU 8. Preparation uses a broad permitted process mask so inherited
affinity validation can accept the intended children. Execution pins the
controller to CPU 8. Placement evidence consists of recorded process/thread
snapshots, supplemented by event-time affinity for D8d; it is not a blanket
full-lifetime observation of every thread. D7d's later-created thread remains a
separate historical affinity-evidence gap.

Primary uninstrumented wall time is monotonic time immediately before `Popen`
through verified owned-child `wait4` completion. It includes interpreter startup
and completion observation. It excludes fixture generation, staging, validation,
daemon shutdown, materialization and storage accounting. The application performs
no explicit fsync: the completion contract is buffered application-level
completion, not crash durability or equivalent durability across backends.

`AccountingProcesses` in [m0_profile.py](m0_profile.py), lines 1370–1455 at the
reviewed revision, uses pidfd readiness, `waitid(WNOWAIT)` and one `wait4`. The
actual daemon remains the directly supervised child through `taskset`'s exec.
No independent tracer/reaper is introduced. CPU/RSS/switches from wait4 cover
whole owned-process lifetimes, not only application function execution. Daemon
lifecycle CPU includes worker threads once. `/proc` CPU snapshots have tick
resolution and broader phase boundaries; I/O includes FUSE transport/logging,
and status context switches are leader-only observations.

Full-size trials used bounded readiness, application, staging, verification,
shutdown and accounting deadlines; D8d used 15 seconds readiness, 120 seconds
for each other phase and 900 seconds overall active preparation/execution.
Per-child logs are bounded to 1 MiB and per-trial evidence to 64 MiB. The
conservative backing reservation is 1 GiB retained headroom + 64 MiB evidence +
eight times input bytes. New canonical allocation is estimated at twice input
bytes; shared reuse reserves no second copy because existing allocation already
affects available space/RAM. Memory headroom is 2 GiB, plus estimated new tmpfs
data when generated. These are checked estimates, not hard or instantaneous
resource bounds; host cgroup/RLIMIT capacity requires separate inspection.

Persistent evidence is private, separate from disposable trees and protected
against accidental overwrite/path substitution. Cleanup requires matching owned
directories, no relevant mounts, confirmed child exits and safe traversal.
Failure evidence is retained. Successful orderly cleanup is not proof of recovery
after power loss, disk exhaustion or an unrelated hostile process.

## 3. D5c repeated uninstrumented baseline

Evidence set **E5** below contains the predeclared schedule and all 18 original
trial states. `A` means direct ext4; `B` means myfs. Two correctness warmups, `A B`,
are excluded from all measured statistics. The measured schedule was:

```text
AA | BB | AB | BA | AB | BA | BB | AA
```

That is eight measured trials/backend, four cross-backend pairs, two A/A controls
and two B/B controls. No measured sample was excluded. D9b recalculation from the
original states agrees with the retained completed observations and statistics.

| Backend | Individual measured wall times in chronological backend order, seconds |
|---|---|
| ext4 | 6.974844549, 6.842760250, 6.806048810, 7.070479714, 7.263597366, 6.902476514, 6.915415884, 6.925767130 |
| myfs | 7.906302091, 8.161678285, 7.418766587, 7.734059028, 8.176835127, 7.730601167, 8.003255859, 7.592501460 |

| Measurement | ext4, n=8 | myfs, n=8 |
|---|---:|---:|
| Wall median, seconds | 6.920591507 | 7.820180560 |
| Wall minimum–maximum, seconds | 6.806048810–7.263597366 | 7.418766587–8.176835127 |
| Unscaled wall MAD, seconds | 0.066042150 | 0.205377200 |
| Application user CPU median, seconds | 6.836114 | 6.965386 |
| User CPU minimum–maximum, seconds | 6.741700–7.191197 | 6.778703–7.222527 |
| Application system CPU median, seconds | 0.040974 | 0.094175 |
| System CPU minimum–maximum, seconds | 0.031090–0.052917 | 0.069365–0.113660 |
| wait4 peak RSS median, bytes | 30,474,240 | 30,705,664 |
| Voluntary context switches median (range) | 1 (1–1) | 90 (89–91) |
| Involuntary switches median (range) | 199 (188–224) | 202.5 (191–222) |

Pairs preserve chronological order. Cross-backend arithmetic is B minus A and
B/A regardless of order; control arithmetic is second minus first and second/first.

| Schedule block / measured indices | Type | Difference, seconds | Ratio |
|---|---|---:|---:|
| 1 / 1–2 | AA control | -0.132084299 | 0.981062761 |
| 2 / 3–4 | BB control | +0.255376194 | 1.032300333 |
| 3 / 5–6 | AB comparison | +0.612717777 | 1.090025475 |
| 4 / 7–8 | BA comparison | +0.663579314 | 1.093852092 |
| 5 / 9–10 | AB comparison | +0.913237761 | 1.125728026 |
| 6 / 11–12 | BA comparison | +0.828124653 | 1.119975005 |
| 7 / 13–14 | BB control | -0.410754399 | 0.948676588 |
| 8 / 15–16 | AA control | +0.010351246 | 1.001496836 |

Application-mount daemon wait4 CPU medians were 0.559489 user seconds
(0.537111–0.567829) and 0.1065225 system seconds (0.100635–0.124344).
Application-phase `/proc` delta medians were 0.555/0.100 seconds, with different,
broader snapshot boundaries. Application-daemon shutdown median was 0.159432689
seconds; myfs staging ranged 3.611877450–3.905978159 seconds and verification
2.758294654–2.788974985 seconds. Those excluded phases are not application wall.

**OBSERVED:** all four cross-backend differences were positive and larger than
the absolute differences in these four same-backend control pairs. All 18 trials
passed their correctness checks. **LIMITATION:** four paired comparisons on one
synthetic workflow do not provide statistical certainty, a production regression
claim or causal attribution. Host frequency/load observations, external metadata
activity and unknown cache residency remain confounders. Context switches do not
measure waiting duration; wall minus CPU is not an I/O-wait measurement.

## 4. Diagnostic findings: separate populations

Each diagnostic was opt-in and baseline-ineligible. Later ordinary controls are
contemporary single observations, not additions to D5c's predeclared population.
No cumulative function times or independently collected traces are added into a
wall-time decomposition.

### 4.1 D6c Python application CPU profiling (E6)

Sequence: ext4 unprofiled, myfs unprofiled, ext4 Python-profiled, myfs
Python-profiled; one observation per condition. `cProfile.Profile` uses
`time.process_time` in the owned application process. Profiling covers the
explicit script/runpy invocation interval; it is disabled before serialization.
Whole-child wall and wait4 CPU also include interpreter/wrapper startup and
finalization/serialization.

| Condition | Whole-child wall s | User CPU s | System CPU s | Profile self-time total s |
|---|---:|---:|---:|---:|
| U-A | 8.303894578 | 8.196357 | 0.060839 | Not collected |
| U-B | 8.897528450 | 8.013613 | 0.110814 | Not collected |
| P-A | 102.936000893 | 68.881737 | 33.599290 | 80.869442216 |
| P-B | 85.152447713 | 49.865246 | 34.340482 | 65.854873604 |

Each raw pstats file contains 432 function entries, 54,910,367 calls and
46,521,691 primitive calls. The profiler intervals were 102.812493449 and
85.033031131 elapsed seconds; serialization was 0.003919299 and 0.003929057
seconds. Raw profiles and receipt hashes were rechecked in D9b.

Large self-CPU categories included `builtins.any` (40.503069947 / 31.825282734
seconds), the application's validation generator at line 52 (23.130051674 /
19.018523017), `transform` (7.207337715 / 6.664891113), ASCII/decimal validation
and CSV writing. Values are ext4/myfs under instrumentation, not unprofiled
function costs. The large perturbation makes these wall times unsuitable for
filesystem comparisons or a stable overhead percentage. C-extension work is
attributed at Python/C-call boundaries, not native internal stacks. The profiles
do not measure myfs, Zstd, FUSE blocking or off-CPU time; self totals need not
equal complete wait4 CPU and cumulative times overlap.

### 4.2 D7d synchronization-call timing (E7)

One ordinary myfs control preceded one isolated sync-wrapped myfs diagnostic.
GNU linker wrappers time eligible production-object `fsync`/`fdatasync` calls
immediately around the real operation, before recording serialization. Only the
application-mount daemon records; preparation and verification are NOT COLLECTED.
Daemon startup and shutdown are within recorder lifetime and classified against
the application interval, rather than silently pooled into it.

| Operation | Calls | Minimum / median / maximum ns | Summed ns | Union ns |
|---|---:|---|---:|---:|
| fsync | 7 | 318,520 / 1,225,956 / 3,911,287 | 10,064,034 | 9,855,899 |
| fdatasync | 150 | 987,887 / 1,395,265.5 / 5,575,326 | 312,263,221 | 312,263,221 |
| Total | 157 | Separate operation populations above | 322,327,255 | 322,119,120 |

All 157 calls were complete and wholly inside that diagnostic application's
interval. There were no failed calls, drops or completeness errors. Caller
classification includes 75 write-path data-sync calls and 75 metadata-delta-sync
calls. A later-created TID, 87160, lacked a direct affinity sample in D7d; D8d's
later observations do not retroactively fill that gap.

Control/diagnostic application wall times were 9.516123997 / 7.594464945 seconds;
user/system CPU was 8.784811/0.109610 and 6.917695/0.072692 seconds. The diagnostic
being shorter in this one pair does not demonstrate negative overhead or a
speedup. The elapsed sync intervals mix possible kernel execution, blocking and
scheduling. They do not establish physical disk-service time, application waiting,
crash durability or the historical ext4/myfs gap's cause.

### 4.3 D8d backing-I/O timing (E8)

One ordinary myfs control preceded one backing-I/O diagnostic. A separately
linked build wraps eligible `pread`, `pwrite`, `read`, `write` and `__pread_chk`
references at eight reviewed production sites. Shared-library-internal calls,
direct syscalls and other interfaces are excluded. Individual libc attempts,
not complete retry-loop transactions, are recorded. Only the application-mount
daemon receives the private recording FD. Limits remain 8,192 events, 4 MiB
complete trace and 64 MiB total trial evidence.

| Measurement | U-B control | IO-B diagnostic |
|---|---:|---:|
| Application wall s | 7.945146831 | 8.693695711 |
| Application user/system CPU s | 7.267569 / 0.085829 | 7.727995 / 0.114643 |
| Application wait4 peak RSS bytes | 35,950,592 | 35,930,112 |
| Voluntary/involuntary switches | 90 / 217 | 91 / 207 |
| Application-daemon user/system CPU s | 0.603906 / 0.115624 | 0.585667 / 0.132975 |
| Application-daemon shutdown s | 0.158399222 | 0.516044798 |

The 0.748548880-second wall difference is arithmetic on one observation per
condition, not stable instrumentation overhead or a production regression.

| Symbol | Calls | Requested bytes | Transferred bytes | Short calls | Minimum / median / maximum ns | Sum = union ns |
|---|---:|---:|---:|---:|---|---:|
| pread | 3,331 | 57,532,331 | 57,532,331 | 0 | 442 / 5,552 / 31,168 | 17,967,423 |
| pwrite | 668 | 8,951,838 | 8,951,838 | 0 | 1,018 / 8,445.5 / 50,712 | 7,502,212 |
| read | 6 | 318 | 312 | 6 | 1,465 / 2,109.5 / 3,415 | 14,087 |
| write | 76 | 25,640 | 25,640 | 0 | 5,592 / 15,953 / 34,326 | 1,239,176 |
| Total | 4,081 | 66,510,127 | 66,510,121 | 6 | Operation distributions above | 26,722,898 |

There were zero failed and zero zero-byte calls; `__pread_chk` was not exercised.
All sequences 1–4,081 are unique and contiguous. Observed equals written; dropped,
inflight, canceled, clock/recording/affinity errors are zero, overflow is false,
and the footer is complete. Normal owned-daemon exit is separately recorded.

The six short reads request 53 bytes and return the expected 52-byte generation
marker at [path.c](../src/core/path.c), lines 129–158. This follows the source's
`sizeof(prefix)` buffer versus `strlen(prefix) + 32 + 1` accepted length. A
successful return may retain nonzero errno; 3,930 successful events retained
errno 2 and 151 retained zero. Neither case is a failed call.

Callers comprise six marker reads, 1,209 metadata reads, 76 metadata writes,
2,122 chunk-payload reads and 668 blob appends. All addresses resolve against the
pinned executable. Marker creation and two compaction I/O sites were unexercised.
FD integers alone do not establish file/object role; these categories use source
callers. TID event counts were 1,801 / 1,155 / 719 / 406 for 27772 / 27773 / 27774 /
27784. Every event-time affinity sample matched CPUs 2,4,6; TID 27784 was absent
from readiness and supplied 406 later samples. This is event-time evidence only.

Integer-nanosecond interval recalculation finds **zero overlaps**, including
cross-thread overlaps, so sum and union are exactly 26,722,898 ns. This is not a
rounding coincidence or proof of globally serial daemon execution. Of the events,
247 precede application launch (128,861 ns); 3,834 are wholly inside
(26,594,037 ns); none straddle, follow completion or overlap shutdown. Gaps between
recorded calls have no CPU/wait/lock attribution.

Both persisted diagnostic labels are
`daemon_backing_io_instrumented_application_completion`, and
`baseline_eligible=false`. The whole-child timing definition is unchanged.
The 26.722898 ms trace union does not identify device-service time, off-CPU time,
FUSE latency, compression CPU or application blocking. **D7d and D8d intervals
come from separate executions and must not be added together.**

## 5. Correctness, lifecycle, provenance and resource evidence

D5c's 18 original states contain 153 owned-child records, all exit 0 with exactly
one reap. D6c, D7d and D8d retained states and final audits report passed staged
input, exact output and fresh-remount checks for the myfs trials. D8d contains
13 children per trial and six normally unmounted daemon lifecycles across the
pair. Original logs, validation records and cleanup audits remain available.
This establishes the tested workflow after normal shutdown, not a general
visibility, failure-recovery or crash-durability contract.

Diagnostic data are recorded outside measured roots, with trial/process token,
file identity, bounded output, hashes and independent decoder receipts. Normal
exit without complete diagnostic evidence is invalid. Preparation and verification
are explicitly excluded from daemon trace populations. Historical warning/error
logs remain original; D8d logs contain no unexplained warning/error. Preparation
repacking is observed separately; application-mount logs show compaction skips,
not established application-stage compaction execution. Metadata lookups and root
enumerations do not reveal requester identity or causal performance impact.

### 5.1 Storage and memory

Post-exit accounting traverses verified owned trees without following symlink
targets, deduplicates device/inode hard links, and sums `st_blocks * 512`.
Materialization, where enabled, is outside primary timing. D5c allocated-byte
medians were 173,314,048 ext4 and 37,384,192 myfs. Both D8d trials recorded:

| D8d backing category | Bytes | Relationship to total |
|---|---:|---|
| Unique-inode allocated total | 37,384,192 | Total checkpoint allocation |
| Apparent backing bytes | 37,347,532 | Different metric; do not add to allocation |
| Directory allocation | 8,192 | Included |
| Identified metadata allocation | 81,920 | Included |
| Payload allocation | 37,294,080 | Included |
| Generation allocation | 28,389,376 | Subset already included; do not add again |

Two hard-link aliases were deduplicated; one symlink's target was excluded.
Generation allocation is not automatically unreclaimed garbage; active/retired
status requires separate evidence. These are post-exit checkpoints, **not storage
peaks** or a general compression-ratio guarantee. Host free-space changes are not
automatically attributable to myfs.

Wait4 peak RSS is per-process lifetime peak, not total host or whole-campaign
memory. Daemon worker CPU is already included in daemon totals. Missing optional
endpoints remain null with reasons: D8d terminal `/proc/PID/io` reads were denied,
and some terminal memory fields were absent; wait4 measurements remain available.
Resource guards and sampled headroom demonstrate bounded trial eligibility at
observation points, not proven maximum allocation or continuous minimum headroom.

### 5.2 Build isolation and test evidence

The ordinary optimized binary is retained at
`/home/quynhcolleen/m0-d2-preliminary-8q5lux9l/build/myfs-optimized`, SHA-256
`ab27e8173b536cf5553b40fcca7a02cf44d8eda2a23d8421ed39a5808413523e`.
Flags include `-O2 -DNDEBUG -g -fno-omit-frame-pointer`. Build provenance,
production/header/Makefile hashes and runtime dependencies are retained separately
from later runner revisions. D9b verified its current identity and hash against
the D8d record; it was not executed.

The persistent backing-I/O binary is
`/home/quynhcolleen/m0-d8b-build-08p4k0w9/build-r1/myfs-backing-io-diagnostic`,
SHA-256 `95b557f92b5a45854cfc001173df163bf716240659a84c7faee348ec82d94842`,
build ID `a12c5ecef430b411fd210c2485851bead4a46f28`. Its matching
`backing-io-build-provenance.json` records production hashes, diagnostic-source
hash, compiler/linker identities, runtime libraries, all eight wrapped sites and
serialization's bypass of wrapped `write`. D9b checked retained identity/hash and
current production fingerprints. No rebuild or new binary execution occurred.

The original D7d sync executable under `/tmp` is **UNAVAILABLE** after reboot.
Its provenance, original hash, raw trace and receipt remain available; it cannot
be re-executed from that missing path. This limits replay, not recalculation of
the retained events. No substitute binary is adopted.

Historical offline command:
`python3 -B -m unittest discover -s tests -p 'test_m0_*.py'`.

| Retained run | Historical result | D9b status |
|---|---|---|
| D5c | 163 tests, 16.578 s, OK, exit 0 | Log/result read; not rerun |
| D6c | 191 tests, 43.558 s, OK, exit 0 | Log/result read; not rerun |
| D7d | 221 tests, 63.762 s, OK, exit 0 | Log/result read; not rerun |
| D8d | 264 tests, 97.510 s, OK, exit 0 | Log/result read; not rerun |

Current source inventory contains 31 M0 fixture/oracle and 233 runner test-method
definitions. Inventory is not a fresh test execution. Passing these tests does
not establish full POSIX compatibility, crash durability or correctness of every
production path.

## 6. Compatibility and contract-gap register

This register concerns behavior/contracts and verification scope, not performance
bottlenecks. Source locations refer to the reviewed commit. Potential consequences
are conditional risks, not reported incidents. Existing tests must be audited
before proposing missing implementation.

| ID | Area | Exact issue or untested contract; evidence | Classification | Potential user consequence | Proposed milestone | Priority rationale | Required verification |
|---|---|---|---|---|---|---|---|
| C01 | Application fsync/flush | `myfs_oper` in [main.c](../src/main.c), lines 37–52, registers neither `.fsync` nor `.flush`; the CSV app performs neither explicit operation. No application-visible fsync result was measured by M0. | CONFIRMED SOURCE BEHAVIOR; UNTESTED CONTRACT | Applications requesting persistence may receive behavior not yet covered by an explicit supported contract. | M1 | Persistence/error promises must precede user reliance. | Audit installed libfuse/kernel behavior, specify file/directory sync and close semantics, then test application-visible results and failures. Do not infer a particular errno from registration alone. |
| C02 | Release sync errors | [file.c](../src/fuse_ops/file.c), line 1162, calls `fsync(handle->data_fd)` without checking its result; release later returns 0. Installed FUSE headers say release's return is ignored. | CONFIRMED SOURCE BEHAVIOR | A sync failure at that boundary may lack an application-visible reporting route. No such failure or data loss was observed here. | M1 | Audit error propagation before claiming durability. | Inject a real boundary failure in a separately authorized test; trace propagation and define the correct application-facing sync/error contract. |
| C03 | Crash consistency/recovery | M0 validates normal shutdown/remount. [metadata.c](../src/core/metadata.c) checks publication/sync errors and torn-tail handling; [compact.c](../src/core/compact.c) contains generation recovery. Existing tests exercise portions of these mechanisms. | UNTESTED CONTRACT for campaign-level crash durability | Interrupted publication or power loss could expose states whose supported recovery outcome needs specification. | M1 | Data correctness has priority over optimization. | Inventory existing fault tests; specify data/meta/current/alias/generation invariants; distinguish process crash from lost non-persisted writes and exercise acknowledged-data recovery. |
| C04 | Concurrent visibility | Cached handles and generation epochs exist in [file.c](../src/fuse_ops/file.c); direct-call handoff/reader-writer tests exist. The CSV campaigns are not a mounted multi-process visibility test. | UNTESTED CONTRACT | A long-lived reader's visibility after another writer or generation handoff is not established by this workload. | M1, then M2 | Central to multiple applications sharing ordinary paths. | Audit current tests and define cross-process read-after-write visibility, then test mounted old/new handles and kernel caching. Do not presume a registry rewrite. |
| C05 | Rename and operation coverage | The registered table lacks rename, statfs, chmod/chown, symlink/link and xattr callbacks. Other file/directory operations are registered. Absence does not establish every application's observed fallback/error behavior. | CONFIRMED SOURCE BEHAVIOR; UNKNOWN application compatibility | Write-temp-and-replace and metadata-sensitive tools may fall outside the supported scope. | M2 | Required operations depend on the selected workflow. | Specify support/error contracts and run a bounded application-level compatibility suite; inspect libfuse defaults rather than inventing results. |
| C06 | Timestamps | [dir.c](../src/fuse_ops/dir.c), lines 230–238, implements registered `myfs_utimens` as a no-op returning 0. This was inspected, not newly exercised. | CONFIRMED SOURCE BEHAVIOR | Timestamp-dependent applications may not observe the update they requested. | M2 | A concrete source behavior requiring an explicit compatibility disposition. | Test requested/observed timestamps and define supported semantics before choosing a correction. |
| C07 | I/O/resource failure propagation | Write-path `fdatasync` and metadata publication/delta errors are checked; helper retry and injected-error tests already exist. M0 successful runs do not establish the complete ENOSPC/EIO/ENOMEM/FD-limit contract. | UNTESTED CONTRACT | Partial or failed work may need explicit caller error and recovery guarantees. | M1; capacity policy later M3 | Avoid false success and protect acknowledged contents. | Inventory current error tests, then cover end-to-end faults and retry/short-I/O boundaries under defined guarantees. |
| C08 | Broader application patterns | M0 exercised one sequential ASCII CSV filter with exclusive output creation, not arbitrary database, mmap, concurrent append, replace, import/export or real customer workloads. | OBSERVED TEST GAP in these campaigns | Compatibility outside the measured workflow remains unknown. | M2 | Prevent generalizing one successful pipeline into POSIX/product coverage. | Choose an actual target application's required patterns and verify them, reusing relevant existing tests. |

No entry asserts an observed application-visible fsync failure, corruption or
data loss. Source-confirmed behavior and untested contracts are not interchangeable
with a demonstrated user-visible failure. Recovery, error handling and concurrency
are not absent merely because M0 did not establish their full contracts.

Representative existing coverage, inspected but not executed in D9b, includes
`test_v2_round_trip_and_delta_recovery` and
`test_uncommitted_full_length_tail_is_ignored` in
[test_metadata.c](../tests/test_metadata.c); publication-failure, live-writer
handoff, reader-bundle, unlink and worker/fallback tests in
[test_file_ops.c](../tests/test_file_ops.c); GC retries and worker-shutdown tests
in [test_generation_registry.c](../tests/test_generation_registry.c); and payload
error tests in [test_chunkio_scratch_integration.c](../tests/test_chunkio_scratch_integration.c).
The existing [test_suite.sh](../test_suite.sh) also covers non-CSV patterns, while
its crash section labels actual kill/restart execution as manual. M1 should audit
these existing contracts and tests rather than recreate them. On-disk format and
parser-version coverage likewise exists; complete corruption/fuzz/upgrade support
is an M1 contract still to assess, not an assertion that parsers lack validation.

## 7. Measured-cost and bottleneck disposition

**No causally established native filesystem bottleneck has been identified by the M0 diagnostic campaign.**

This is the separate disposition required for review; it is not merged into the
compatibility register. Directly measured cost is not necessarily a bottleneck,
and frequency alone is not elapsed-time importance.

| Candidate / cost category | Supporting observation | Direct cost measurement? | Contribution to historical ext4/myfs gap established? | Remaining uncertainty | Specific trigger for future investigation |
|---|---|---|---|---|---|
| Python application processing | D6c validation, parsing and transform functions dominate instrumented profile categories. | Yes, heavily perturbed process-CPU profile. | No | Unprofiled function costs and custom-timer overhead differ. | A target application's CPU budget requires application-side attribution. |
| FUSE interaction/wakeups | D5c myfs voluntary switches 89–91 versus ext4 1; real callback log entries exist. | Counts, not request/wakeup duration. | No | Kernel queueing, reply delivery, callback service and scheduling are not separated. | A concrete M3 decision needs read/write/release service-category discrimination. |
| Daemon native CPU | Daemon wait4 lifecycle totals and `/proc` phase deltas. | Yes, process totals including workers; no native stacks. | No | Compression, copying, metadata, locking and logging CPU attribution. | A measured workload CPU/resource limit justifies permitted native sampling. |
| Synchronization | D7d 157 complete calls, 322.327255 ms sum / 322.119120 ms union. | Yes, eligible real-call elapsed intervals. | No | CPU/blocking/scheduling/device contributions and dependency on application requests. | An explicit sync/durability policy or M3 optimization choice requires this population. |
| Backing reads/writes | D8d 4,081 calls, 66,510,121 transferred bytes, 26.722898 ms union. | Yes, eligible libc-call intervals and bytes. | No | Excluded interfaces, surrounding userspace work and end-to-end request dependencies. | A target I/O budget or changed workload makes eligible backing-call behavior material. |
| Compression/decompression | Source calls and payload caller families; compressible synthetic allocation. | No native compression CPU timing. | No | Zstd CPU cost and raw-fallback behavior on other data. | A measured M3 CPU/storage tradeoff or high-entropy workload requirement. |
| Metadata/chunk operations | Source paths, call counts and metadata I/O/sync intervals. | Only selected subcalls, not complete operations. | No | Repacking/lookup/copy/lock cost outside wrappers. | A concrete operation budget identifies a need for targeted attribution. |
| Worker/compaction activity | Separate daemon CPU; preparation repacking and application compaction-skip logs. | Lifecycle CPU, not isolated worker transactions. | No | Background cost/dependencies on broader workloads; active versus retired storage. | M3 background-work or headroom requirements are exceeded in a measured workflow. |
| Host scheduling/noise | Frequency/load/pressure observations, switches, external-looking metadata requests. | Snapshots/counts, not causal delay. | No | Requester identities and runnable/off-CPU durations. | A reproducibility problem or application latency requirement needs controlled scheduling evidence. |

The current evidence supports preserving the baseline, documenting limitations
and avoiding premature production optimization. It does not support ranking
these candidates as proven causes of the wall-time difference.

## 8. Deferred research and stop/go rationale

All items below are **PROPOSED / NOT EXECUTED as new work by this checkpoint**.
They are conditional investigations, not an indefinite extension of M0.

| Investigation | Current status and roadmap relationship | Resume only when |
|---|---|---|
| Selected read/write/release callback timing | D9a source/object review found unresolved registration relocations that could support isolated GNU wrapping. No callback recorder or mounted callback profile exists. M3 attribution option, not a stated M0 prerequisite. | A concrete optimization decision depends on callback categories. Entry-to-return would still mix CPU, I/O, locking and scheduling and exclude kernel queue/reply delay. |
| Off-CPU waiting and request latency | Not measured by CPU, switches, sync or backing-I/O intervals. Consistent with roadmap's requirement to distinguish observable waiting populations. | An application latency budget requires those exact populations and a safe permitted method is available. |
| Native stack profiling | Prior design reviews reported perf permission denial; no standalone probe report was located for independent D9b re-verification. No native stack result is claimed. | A specific native CPU question justifies sampling under legitimately suitable permissions, without bypassing controls. |
| FUSE passthrough / null compression | Explicitly later options in roadmap M0-D, not performed here. | A controlled causal comparison is needed for a defined M3 decision; matched synchronization/cache semantics must be reviewed first. |
| High-entropy / incompressible data | `myfs-m0-shake256-v1` is an existing labeled high-entropy candidate; compression behavior is unmeasured. Roadmap M3 includes raw fallback. | The chosen data scope or storage/resource requirement makes this behavior relevant. Do not call the candidate empirically incompressible before measurement. |
| Broader/real workloads | Roadmap starts with one reproducible pipeline; no customer corpus has been validated. M2 workflow selection and later M3/M5 evaluation remain distinct. | An actual application and permitted representative data are selected. |

Callback timing is feasible in principle but not needed for a presently approved
M1 correctness/recovery decision. It would not resolve durability, concurrent
visibility or compatibility contracts. Its implementation, ABI/recorder tests,
mounted smoke and instrumented execution would add cost and perturbation without
closing an essential missing M0 measurement identified by this review.

## 9. Evidence index and current availability

Paths below are host-local persistent evidence locations, not files copied into
this repository or guaranteed accessible from another machine. D9b checked the
main raw files and receipts used for its claims. Historical final audits describe
their execution-time cleanup; they are not substituted for new experiments.

### 9.1 Main campaigns

| ID / checkpoint | Evidence root and source identity | Original artifacts and measurement population | Completeness / D9b availability / limitations |
|---|---|---|---|
| E5 / D5c | [/home/quynhcolleen/m0-d5c-campaign-ulhg8154](/home/quynhcolleen/m0-d5c-campaign-ulhg8154); source `170594107042a9669ef95e0bc2d042357683dc2b` | `schedule.json`, `schedule-identity.json`, all 18 `m0-artifacts-*/state.json`, `observations-complete.csv`, `calculation-inputs.json`, `statistics.json`, `daemon-phase-observations.csv`, original application/daemon logs, per-trial canonical checks, `build-provenance.json`, `final-audit.json` | AVAILABLE: raw states and retained statistics reconcile; 16 measured + 2 excluded warmups. Persistent canonical receipts available; original tmpfs CSVs UNAVAILABLE. |
| E6 / D6c | [/home/quynhcolleen/m0-d6c-authorized-zlzjlecu](/home/quynhcolleen/m0-d6c-authorized-zlzjlecu); source `7a3068858607a74f5771520b29fd71c9dafde97c` | `report.md`, `summary.json`, `schedule.json`, four `m0-artifacts-*/state.json`, two `python-cpu-*.prof` and adjacent `.json` receipts, original logs, `artifact-manifest.json`, `final-audit.json` | AVAILABLE: four conditions, raw profiles readable and hashes match state/receipts. Severe profiling perturbation; one observation/condition. Original tmpfs CSVs UNAVAILABLE. |
| E7 / D7d | [/home/quynhcolleen/m0-d7d-sync-pair-9qw1cb9n](/home/quynhcolleen/m0-d7d-sync-pair-9qw1cb9n); source `41127b11e24d3f4950cf541c4603609c70aa8e69` | `summary.json`, `sync-analysis.json`, both `m0-artifacts-*/state.json`, `daemon-sync-4e26dd45da7e40348c0172b73c132064.jsonl` and `.receipt.json` inside that trial's artifact directory, original logs, `retained-build-provenance.json`, `final-audit.json` | AVAILABLE: complete 157-event trace and receipts. Late-thread affinity gap retained. Original tmpfs CSVs and historical `/tmp` executable UNAVAILABLE; build provenance remains. |
| E8 / D8d | [/home/quynhcolleen/m0-d8d-campaign-9mrlomv8](/home/quynhcolleen/m0-d8d-campaign-9mrlomv8); source `6217edf82b59737c5b048d07538612fd20491b67` | `report.md`, `plan.json`, `commands.txt`, `U-B-run-state-after.json`, `IO-B-run-state-after.json`, original artifact states/logs, raw trace and receipt, `independent-decoder.json`, `independent-analysis.json`, `events-expanded.json`, `caller-symbolization.json`, `binary-verification.json`, `evidence-manifest.json`, `final-audit.json` | AVAILABLE: both trials passed, complete 4,081-event trace; all 132 manifest-listed files match hashes/sizes/device/inode. No numerical/metadata discrepancy. Original tmpfs CSVs UNAVAILABLE. |

The E8 raw trace is
`m0-artifacts-a0f998b95dce4fe5903d6dacf36c4f74/daemon-io-a0f998b95dce4fe5903d6dacf36c4f74.jsonl`:
1,513,523 bytes, SHA-256
`474f81a245bb137a8e6d9a96322cb4270abd3c35205c4cdbea35bcd084a27996`.
Its adjacent `.receipt.json` is 9,390 bytes, SHA-256
`e00270cc52dfb3f769e72d9327c1e668e956d985df7720002eab88df75797efe`.

The E7 raw trace is 33,191 bytes, SHA-256
`8238993e3d790a5b8f849433185729ce888102ef6b7d372c7855818cb7e683a5`;
its receipt SHA-256 is
`472decac596db4b0cb6a75005b806a8c0afae371344be413bb8bf3e1ee28b3db`.
Both match the original state. E5's current `schedule.json` SHA-256 is
`47d1d49c9ca7d8062d205138129614a8c4d2782430ba61972ffe62b458cfe146`;
its retained `schedule-identity.json` provides the original schedule identity.

E6 raw profile SHA-256 values, verified against the receipts, are:

- P-A: `d25ad779dba343b8738d387607f0a87ccd26f02a2d546ae03a3574dfca13908a`.
- P-B: `2d1ed30d32abae9eebf700153a3db2203f93cd3c4fa9bdd9ab34f4dc21ece36d`.

Each profile is 57,200 bytes. Exact paths relative to E6 are
`m0-artifacts-d2540c9ae7d44b0a9142e29472e50579/python-cpu-d2540c9ae7d44b0a9142e29472e50579.prof`
and
`m0-artifacts-6be29c0e90a34b5cba3adebb6148a7d2/python-cpu-6be29c0e90a34b5cba3adebb6148a7d2.prof`;
the corresponding JSON receipts use the same basenames with `.json`.

### 9.2 Supporting implementation, smoke and failure evidence

| Checkpoint | Persistent evidence / provenance | Availability and interpretation |
|---|---|---|
| Original mounted M0-C | [/home/quynhcolleen/m0-mounted-smoke-z1840jcx/m0-artifacts-ccf85da872de49f28bb3436c0116b019](/home/quynhcolleen/m0-mounted-smoke-z1840jcx/m0-artifacts-ccf85da872de49f28bb3436c0116b019) | Historical roadmap reference; availability checked separately from the later raw recalculations. No new mount or claim of independently rerunning it. |
| Ordinary optimized build | [/home/quynhcolleen/m0-d2-preliminary-8q5lux9l/build](/home/quynhcolleen/m0-d2-preliminary-8q5lux9l/build), `provenance.json` | AVAILABLE; executable identity/hash match E8. Build revision predates later runner changes; production fingerprints, not revision equality alone, establish compatibility. |
| D7c sync mounted smoke | [/home/quynhcolleen/m0-d7c-smoke-i5t2xc27](/home/quynhcolleen/m0-d7c-smoke-i5t2xc27), `summary.json`, `state.json` within trial artifacts, `final-audit.json` | Persistent evidence AVAILABLE; historical small sync integration test, not a performance population. |
| D8b isolated backing-I/O build | [/home/quynhcolleen/m0-d8b-build-08p4k0w9/build-r1](/home/quynhcolleen/m0-d8b-build-08p4k0w9/build-r1), `backing-io-build-provenance.json`, retained objects/disassembly and binary | AVAILABLE; persistent binary checked. Offline linkage evidence does not replace mounted evidence E8. |
| Original blocked D8c | [/home/quynhcolleen/m0-d8c-smoke-r3wkvxwo](/home/quynhcolleen/m0-d8c-smoke-r3wkvxwo), `final-audit.json`, `evidence-manifest.json` | AVAILABLE; blocked before canonical/mounted execution by population-label inconsistency, not a completed trial. |
| D8c metadata fix | [/home/quynhcolleen/m0-d8c-fix-ks2yrwwa](/home/quynhcolleen/m0-d8c-fix-ks2yrwwa), `initial-audit.json`, `preservation-audit.json`, offline results | AVAILABLE; separate runner-metadata correction; no binary rebuild or historical artifact rewrite. |
| D8c recovery smoke | [/home/quynhcolleen/m0-d8c-recovery-frjoreue](/home/quynhcolleen/m0-d8c-recovery-frjoreue), `report.md`, `summary.json`, raw trace/receipt, `final-audit.json` | AVAILABLE; small 100-record backing-I/O integration evidence. Kept separate from full-size E8. |
| D6c blocked/recovery attempts | `/home/quynhcolleen/m0-d6c-blocked-preflight-a4h86094`, `/home/quynhcolleen/m0-d6c-diagnostic-l6pmq4rm`, `/home/quynhcolleen/m0-d6c-recovery-blocked-1uin2nb4` | Persistent summaries/final audits AVAILABLE; failures retained and not reclassified as successful samples. E6 is the distinct successful authorization. |
| D6a / D7a / D8a / D9a design reviews | No standalone report located in the inspected checkpoint-named evidence roots | Standalone files UNAVAILABLE. Prior conversational decisions are context, not substitutes for raw evidence. D9a's callback feasibility/stop-go reasoning is restated here from inspected source, retained objects and reconciled measurements. |

### 9.3 Volatile canonical data and reproducibility limits

Fresh actual-host availability inspection during D9b found boot ID
`6b4e2b9e-4755-4c9d-8b7d-02f0546a504e`. This differs from D8d/D9a's inspected boot
`58023e5a-6849-441a-938f-ef13c0761f51`.
All four historical canonical directories below are currently **UNAVAILABLE**:

| Campaign | Historical volatile path |
|---|---|
| D5c | `/dev/shm/m0-d5c-canonical-68m7_agy/m0-shared-canonical-d94e14c9a7b4429cb360adc7ce7a64a9` |
| D6c | `/dev/shm/m0-d6c-authorized-canonical-c1mvv1hb/m0-shared-canonical-0889af67be2444ffab8584ce7055b3af` |
| D7d | `/dev/shm/m0-d7d-canonical-cz2f9nk4/m0-shared-canonical-d40acbebcbea4e85a8d8b4c5dffae8c2` |
| D8d | `/dev/shm/m0-d8d-canonical-ts8w8bew/m0-shared-canonical-91a9c1675f464620a471d49068fea418` |

Historical receipts, manifests, sizes, hashes and oracle results remain available.
Their past validation is not a current validation of absent files. Future execution
would need explicitly authorized deterministic reconstruction, new campaign/boot
identities and fresh independent validation. No stale ownership identity may be
adopted. This checkpoint regenerated nothing and created no large canonical archive.

The missing D7d executable was
`/tmp/m0-d7b-build-piilx7_4/revision3/myfs-sync-diagnostic`, recorded SHA-256
`e551438b3c5bd1635a3e30e6f84d9e0259245fd78632e9b5d5ebf6ddcc979986`.
That hash is historical, not a fresh hash of an available file. Reconstruction
or replacement would require separate approval and provenance validation.

## 10. D9b verification and preservation record

The review used read-only Python recalculation, `pstats`, SHA-256, source/header
inspection and Git/document checks. No test suite, daemon, benchmark, fixture
generator, build, mount or profiler workload was executed. Historical passing
test logs are labeled historical in section 5.2.

Checks performed:

1. Read the actual roadmap and compared its exact M0 wording with the proposed
   interpretation; no material discrepancy was found.
2. Recalculated D5c medians, unscaled MAD, ranges and pair/control arithmetic from
   original trial states; checked all 18 states against retained observations.
3. Loaded both D6c raw pstats files and verified counts, CPU totals and receipt
   hashes; checked timer and serialization boundaries.
4. Recalculated D7d and D8d event sequence, byte/error populations and integer
   interval sums/unions from original traces; checked retained receipts/hashes.
5. Checked source references and existing test inventory for the gap register.
   No claim of missing implementation is based merely on an old handoff.
6. Verified all 132 E8 manifest-listed file hashes, sizes and device/inode
   identities; checked 22 current source fingerprints and both persistent
   executable identities/hashes against E8.
7. Checked named persistent paths and separately inspected actual-host volatile
   paths. Unavailable canonical files, old sync executable and standalone design
   reports are explicitly identified; no missing raw data was reconstructed.
8. Reviewed links, causal language, population separation, storage subsets,
   pending milestone status and the sole-file diff. Ran `git diff --check` and
   inspected the final staged diff; only this new Markdown file is in scope.

For preservation checking, aggregate SHA-256 digests were taken over each sorted
relative file name, NUL separator and SHA-256 file digest, excluding symlinks:

| Evidence root | Regular files | Aggregate preservation digest |
|---|---:|---|
| E5 | 558 | `f0ff3f1a4872589d674a53c4a1c831dbc39b5282cb4712277be2c98a5940a956` |
| E6 | 139 | `da4c4232a70df36925b5b9d1006dde23cefbfccb993d4183f64fec485cd9030d` |
| E7 | 87 | `0aaca9434cc4ba1ac2a679590ebcec953ec81a5039f1c90fce9590169da24b03` |
| E8 | 133 | `2c9ab2fbbafb6d11ac69ee3dfb0059e4a54637bfab75926dc01c52887f317e70` |

These digests detect changes to the reviewed retained files during this checkpoint;
they are not cryptographic signatures proving historical authenticity. Original
failures, logs, source, binaries and provenance are preserved. Only this document
is created in the repository; roadmap, production, runner, diagnostic C, fixture,
application and tests remain unchanged.

## 11. Proposed criterion-by-criterion acceptance

The following assessments concern M0's scoped evidence deliverables, not overall
filesystem readiness. Status labels do not constitute user acceptance.

| Roadmap deliverable | Proposed status | Evidence | Important limitations | Remaining action |
|---|---|---|---|---|
| Reproducible baseline | SATISFIED | Deterministic versioned workload/oracle, hashes, identities, fixed D5c schedule, eight measurements/backend and paired controls; sections 2–3, E5 | One synthetic workflow/machine/cache history; fixture reconstruction now required for replay | Reviewer accepts this scope; any future execution rechecks host/provenance and creates valid current identities |
| Raw evidence | SATISFIED | Original trial states/logs, pstats, complete sync/I/O traces, receipts, provenance and calculations; E5–E8 | Host-local evidence is not in the repository; volatile CSVs/old sync binary absent; standalone design reports unavailable | Preserve persistent evidence and these availability qualifications; do not infer full historical replay capability |
| Correct output | SATISFIED | Exact independent input/output comparisons, successful application exits and fresh-remount verification for the measured workflows | Normal orderly completion only; not crash durability, concurrent visibility or full POSIX correctness | Reviewer accepts the scoped claim; M1/M2 contracts require separate verification |
| Separate compatibility/contract-gap disposition | SATISFIED | Section 6, source references and existing-test qualifications | Register is a proposed audit scope, not proof every candidate is broken or every gap is found | Review priorities and supported scope; no implementation authorized |
| Separate measured-bottleneck disposition | NEEDS USER DECISION | Section 7 separately records directly measured costs, unknown causes and explicit absence of a causally established native bottleneck | Roadmap does not explicitly demand at least one proven bottleneck, but acceptance of an honest unestablished/empty proven list must not be assumed | Accept this disposition, or specify a concrete unmet criterion before authorizing further work; do not automatically start another experiment |

The documentary separation missing from scattered campaign reports is supplied
here. No measured result justifies production optimization at this point. The
remaining decision is whether the scoped evidence and explicit unknowns meet
M0's intended exit, particularly its measured-bottleneck wording.

**Proposed acceptance recommendation:** approve the M0 evidence package with its
stated limitations and defer further profiling until a concrete engineering
decision warrants it. If the user accepts the criterion interpretation, a later
separately authorized checkpoint may audit M1 visibility, sync, error and recovery
contracts against existing code/tests. This document neither changes roadmap
status nor starts or completes M1.

**Status remains: PROPOSED — AWAITING USER REVIEW.**
