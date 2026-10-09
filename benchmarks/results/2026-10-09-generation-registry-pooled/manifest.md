# Generation registry / GC pooled-metrics campaign — 2026-10-09

These eight files are byte-for-byte copies of the original completed campaign
outputs `/tmp/grb-pooled-set{A,B}-{current,main,current-aa,main-aa}.out`.
No measurements were regenerated, edited, normalized, or redacted for archival.
`SHA256SUMS` identifies the original output bytes; verify with:

```bash
cd benchmarks/results/2026-10-09-generation-registry-pooled
sha256sum --check SHA256SUMS
```

## Source identities

- `current` and `current-aa`: `743c2b467041994cc64dbcc2714a8952fe406f1e`.
- `main` and `main-aa`: `7f8f335b7a00fc4d928475fa9d947d3a3187b00a`, the
  campaign's `origin/main` baseline (global registry mutex / GC I/O under it).
- Both binaries use the same pooled-reporting benchmark source from `743c2b4`,
  linked against the respective tree's real public registry/GC API. These are
  not algorithm models and do not use test hooks. The `TREE` field identifies
  the core tree linked into each binary, not a later documentation/test commit.
- Each set has `current`, `main`, `current-aa`, and `main-aa` outputs. Normal
  runs have `aa=false`; separate A/A runs have `aa=true` and pair repetitions
  of the same binary with alternating primary/companion execution order.

## Host and configuration

- Intel Core i5-12450H, x86_64, Linux `6.17.0-41-generic`, hostname `Colleen`.
- Real I/O work directory was under the repository on ext4. Raw output calls
  the ext filesystem family `ext` with magic `0xef53`; that magic alone does
  not distinguish ext2/ext3/ext4. The campaign path and `findmnt` establish ext4.
- Set A: foreground CPUs `0,2,4,6`, GC CPU `8`.
- Set B: foreground CPUs `1,3,5,7`, GC CPU `9`.
- `0/1`, `2/3`, `4/5`, `6/7` are SMT sibling pairs. Sets A/B therefore are not
  independent sets of physical cores. GC CPUs are not foreground SMT siblings.
- Thread counts `1,2,4`; one GC worker; seven primary repetitions per row;
  warmup `128` operations per foreground thread, excluded from measurements.
- Basic iterations `200000` per thread. GC foreground minimum `100000`
  operations per thread and minimum `20` completed GC cycles per repetition;
  both conditions must be met, so actual foreground counts vary.
- Strict delayed-operation threshold `>1000` microseconds (`>1000000` ns).
- Both real-I/O and synthetic-delay modes run on ext4. Synthetic mode adds a
  requested `250` microseconds at wrapped I/O calls inside GC, not instead of I/O.
- Deterministic seed `25228`. Parameters and exact per-thread affinity appear
  in each original file's `CONFIG`, `AFFINITY`, `TOPOLOGY`, and `ASSIGNMENT` lines.

## Validation and interpretation

All eight originals were checked before copying: source identities, host,
affinity and configuration match; each contains 15 unique workload summary CSV
rows with seven primary repetitions and passing self-checks (120 rows total).
Framing, column structure, finite numeric values, latency ordering, strict
threshold/max consistency and pooled `delayed_total / pooled_operations`
arithmetic were checked. README throughput ratios and pooled fractions were
recalculated from these rows; percentile conversions and cycle-normalized
values were checked against the original columns. `cmp` and SHA-256 checks
verify every archived output against its original.

The CSV summarizes repetitions, not 120 individual latency observations.
Primary pooled counts exclude warmup and A/A companions. `p50_ns`, `p99_ns`,
`p999_ns` and `median_rep_delayed_fraction` are medians of per-repetition
metrics; `max_ns` is the maximum over all primary repetitions. GC cycle totals
include completion of a final cycle beyond the foreground interval, so
`delayed_per_gc_cycle` has imperfectly aligned foreground/GC windows. The raw
summary does not export individual repetition samples or summed GC cycle counts;
do not reconstruct those measurements from `gc_min_cycles`.

Continuous GC is a synchronization stress workload, not typical production GC
frequency. Registry workloads also include caller path/cache locking. Control
has no registry/GC calls in its measured loop. OS preemption, A/A variability
and between-run drift remain limitations; normal-run `noise_floor_pct=0` and
`win_gate=unavailable` do not establish zero noise. The same-path throughput
regression and current-tree operations above 1 ms remain visible in these files.
No individual spike is attributed to GC; neither GC-interference throughput
ratios nor these microbenchmarks establish end-to-end FUSE speedups. Registry
lock duty cycle is not exposed by the public API.

Raw fields and summary text were inspected for credentials/private material;
no secrets or credentials were found. Hostname, kernel, CPU and affinity are
retained as benchmark context. Nothing was silently redacted.
