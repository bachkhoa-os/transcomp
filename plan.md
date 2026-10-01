# Per-file Adaptive Window Sizing Plan

## Phase 1 — Cache Chunk Maps and Resolved Storage in Open Handles

### Files and functions touched

- `src/myfs.h`: `myfs_file_handle_t`
- `src/fuse_ops/file.c`: handle attach, `myfs_read()`, `myfs_write()`, `myfs_truncate()`, `myfs_release()`
- `src/fuse_ops/dir.c`: `myfs_getattr()`
- `src/core/compact.c`: generation registry and metadata epochs

### Change description

Each handle owns a cached `myfs_inode_t`, pinned data/meta descriptors, resolved `myfs_storage_t`, `seen_metadata_epoch`, and a `pthread_rwlock_t`. The lock is initialized with `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`; failure is fatal to handle attachment. Reads hold it in read mode for the complete lookup, `pread`, decompression, and final metadata use. Mutations and generation handoff hold it in write mode.

Generation records own an in-memory metadata epoch and superseded bit. A normal read checks the epoch without a backing metadata syscall. An epoch mismatch refreshes the complete cached bundle under the file lock and cache write lock. Superseded read-only handles remain pinned to their old generation; mutations return `-ESTALE`.

### Acceptance criterion

- After open, 100 unchanged reads through one handle perform no additional `.meta` load or backing metadata syscall.
- A read racing a live handoff returns fully old- or fully new-generation data, never a mixed bundle or invalid pointer.
- With eight overlapping 100 ms reads sustained on one handle, the handoff write lock is acquired and the handoff completes within two seconds under ThreadSanitizer.

### Dependencies

No prior phase. This phase can ship independently and is required before live adaptive handoff.

## Phase 2 — Replace Per-write Full-map Rewrites with a Delta Journal

### Files and functions touched

- `src/core/metadata.c`: versioned reader/writer, delta append/replay, checkpoint writer
- `src/myfs.h`: metadata state in `myfs_inode_t`
- `src/fuse_ops/file.c`: write/truncate commit ordering and checkpoint trigger

### Change description

Retain v0 legacy decoding. Add a fixed 64-byte little-endian header, fixed 32-byte wire entries, CRC-protected base map, and append-only delta records containing sequence, splice range, new logical size, entries, CRC, and commit marker. Normal writes perform `fdatasync(data)`, append one delta, then `fdatasync(meta)` before updating the handle cache. An incomplete tail is ignored; a complete corrupt record fails loading.

Checkpoint after 1024 committed records or when journal bytes exceed `max(1 MiB, 2 × live checkpoint bytes)`. Checkpoint replacement bumps the generation epoch so other handles reopen the new metadata inode. A v0 file is converted once before its first journal append.

### Acceptance criterion

- Updating one chunk in a 4096-entry map grows `.meta` by exactly one 88-byte delta, independent of total chunk count.
- Reopening replays committed deltas; an incomplete trailing record leaves the last committed state intact.
- Data is durable before a committed delta can reference it.

### Dependencies

Depends on Phase 1 for safe descriptor replacement and cross-handle invalidation. It could ship without adaptive sizing. Skipping it makes smaller adaptive windows amplify metadata rewrite cost.

## Phase 3 — Persist a Per-inode Window Size

### Files and functions touched

- `src/myfs.h`: `myfs_inode_t.window_size`, bounds, checked window helpers
- `src/core/metadata.c`: v1/v2 decoding and v2 encoding
- `src/core/chunkio.c`: size-aware packing and repacking
- `src/fuse_ops/file.c`, `src/core/compact.c`: replace fixed-size arithmetic

### Change description

Metadata v2 stores a power-of-two `window_size` in the header. Valid values are 16 KiB through 1 MiB. New files default to 64 KiB. Untagged v0 and versioned v1 metadata derive 64 KiB, so existing files remain readable. Every packed-generation lookup and repack uses the inode value; a generation never mixes sizes.

### Acceptance criterion

- v0 and v1 fixtures load as 64 KiB.
- A v2 16 KiB file round-trips its size and a 32 KiB write produces exactly two head-aligned chunks.
- Invalid, non-power-of-two, or out-of-range v2 sizes are rejected.

### Dependencies

Depends on Phases 1 and 2. It must not ship adaptive decisions before Phase 2 because smaller windows otherwise increase the full-map rewrite penalty.

## Phase 4 — Collect Full-window and Partial-RMW Counters

### Files and functions touched

- `src/myfs.h`: `myfs_window_stats_t`, `myfs_adaptive_ticket_t`
- `src/fuse_ops/file.c`: successful write classification and post-lock scheduling
- `src/core/compact.c`: generation counters, evaluation state, handle list

### Change description

After a successful metadata commit, classify every touched window. A write covering the complete window increments `full_windows`; a partial write increments `partial_rmw` only when it overlaps live data and therefore performs RMW. Counters live on `myfs_generation_record` and reset on daemon restart.

When both total sample and newly classified count reach 128 in `IDLE`, atomically move the complete counters into a uniquely numbered ticket, clear the record counters, and set `QUEUED`. Scheduling happens after releasing the file lock. Queue/in-flight state deduplicates the same generation/evaluation. A scheduling failure restores the snapshot. Final-writer release uses the same claim path and cannot double-consume a ticket already claimed in-band.

### Acceptance criterion

- A handle that never closes queues evaluation after its 128th classified partial RMW.
- A concurrent release cannot create a second ticket or consume the same counters twice.
- Scheduling failure leaves the user write successful and restores the statistics.

### Dependencies

Depends on Phases 1–3. Counters can ship dormant before Phase 5, but have no user-visible benefit alone.

## Phase 5 — Resize in the Existing Compaction Worker

### Files and functions touched

- `src/core/compact.c`: adaptive queue request, policy, repack, cooldown, atomic publish, live handoff
- `src/fuse_ops/file.c`: in-band request scheduling
- `src/myfs.h`: policy and scheduling interfaces

### Change description

Reuse the existing queue, compaction worker, generation directories, atomic `.current` publish, and GC registry—no timer or second worker. For a sample of at least 128, shrink one ×2 step when partial RMW is at least 60%; grow one ×2 step when full-window writes are at least 90%. Clamp to 16 KiB–1 MiB.

The ratio is necessary but not sufficient. Let `live_bytes` be the sum of live `stored_size`, `step_bytes` the absolute window-size change, and `signal_events` the direction’s event count. Resize only when the saturating product `signal_events × step_bytes >= live_bytes`. This scales required evidence with full-repack cost and avoids a fixed size ceiling.

After resize, record a `CLOCK_MONOTONIC` one-hour cooldown on the new generation. During cooldown, counters accumulate and state becomes `COOLDOWN_DEFERRED`, but no request is queued. On the first write or final release after expiry, halve both accumulated counters once, return to `IDLE`, then evaluate. Decay preserves sustained evidence while damping burst-driven oscillation.

For live writers, fully prepare and sync the new generation before taking cache locks. Sort writable handles by pointer and acquire every cache write lock. Publish atomically, rebind each writer’s descriptors/storage/cache/generation record, close and free old writer bundles while locks are held, then unlock in reverse. Read-only handles keep the old generation until release. Ambiguous publish resolution either completes the new handoff, restores the old active generation, or marks writers stale; it never exposes a mixed bundle.

### Acceptance criterion

- 128 qualifying partial RMWs resize a live 64 KiB handle to 32 KiB without requiring `release()`.
- A cost-rejected large generation is not repacked and needs 128 new events before reevaluation.
- A qualifying opposite signal during the one-hour cooldown does not resize; its counters are carried and later decayed.
- The live handoff remains bounded to two seconds under continuous overlapping reads and passes ThreadSanitizer without a torn bundle or use-after-free.

### Dependencies

Depends on Phases 1–4. The cost gate and periodic trigger are independent and separately tested. Resizing before Phase 2 is explicitly prohibited because more chunks would worsen full-map persistence cost.

## Phase 6 — Migration and Mixed-generation Compatibility

### Files and functions touched

- `src/core/metadata.c`: v0/v1/v2 compatibility
- `src/core/chunkio.c`: tolerant non-packed source handling
- `src/core/compact.c`, `src/core/path.c`: existing generation publish and GC paths
- `test_suite.sh`, `tests/meta_inspect.py`: compatibility inspection

### Change description

Reuse the existing tolerant linear-scan path for old or unpacked source layouts. Repack the complete logical content into one window size in a new v2 generation, sync it, and publish through the existing atomic symlink mechanism. Handles may temporarily refer to different generations—and therefore different window sizes—but each handle’s descriptor/map/window bundle remains internally consistent. Old storage is reclaimed only after its last pinned handle closes.

### Acceptance criterion

- Existing v0 files read correctly before migration and become valid v2 packed generations after compaction without content change.
- A pre-publish read-only handle and a post-publish handle can read their respective generations concurrently.
- No generation contains entries aligned to more than one window size, and restart recovery removes only inactive generations.

### Dependencies

Depends on all prior phases. It reuses generation, atomic publish, GC, and tolerant read machinery rather than adding a migration subsystem.

## Open questions

The implemented policy uses the accepted constants (128 samples, 60% shrink, 90% grow, 16 KiB–1 MiB, one-hour cooldown). Operational benchmarking may justify tuning them later; changing them does not require another on-disk format revision.
