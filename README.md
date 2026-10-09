# myfs — Transparent Compression Filesystem

## Giới thiệu

myfs là một FUSE-based filesystem hỗ trợ **transparent compression** — ứng dụng gọi `open()`, `read()`, `write()` hoàn toàn bình thường như với ext4 hay NTFS, nhưng bên dưới filesystem tự động nén dữ liệu trước khi lưu xuống disk và giải nén khi đọc ra. Người dùng và ứng dụng không biết dữ liệu đang được nén.

### Điểm khác biệt với zip/RAR

| | zip / RAR | myfs |
|---|---|---|
| Tầng hoạt động | Application | Filesystem |
| Ứng dụng có biết không | Có | Không |
| `cat file` ra gì | Rác binary | Nội dung đúng |
| `grep` trong file | Không được | Được |
| Người dùng phải làm gì | Nén/giải nén thủ công | Không làm gì |
| Random access | O(n) — giải nén từ đầu | O(1) — chỉ decompress chunk cần đọc |
| Partial overwrite | Không có | Có (Read-Modify-Write) |

---

## Kiến trúc hệ thống

```
Application (cat, cp, grep, ...)
        │  open() / read() / write()
        ▼
    Kernel VFS
        │
        ▼
    FUSE kernel module
        │
        ▼
┌─────────────────────────────────┐
│           myfs (userspace)      │
│                                 │
│  fuse_ops/file.c                │
│    myfs_read()  ─► decompress   │
│    myfs_write() ─► compress     │
│    repack per-file windows (RMW)│
│                                 │
│  core/                          │
│    compress.c   Zstd + TLS reuse│
│    chunkio.c    window I/O      │
│    metadata.c   map + journal   │
│    compact.c    GC + adaptation │
│    path.c       path mapping    │
└─────────────────────────────────┘
        │
        ▼
  backing/ (directory trên ext4)
    file.txt.data      ← compressed blobs
    file.txt.meta      ← chunk map binary
    file.txt.current   ← symlink → generation active (xuất hiện sau compaction)
    file.txt.g.<hex>/  ← generation directory (data + meta)
```

Sau lần compaction đầu tiên, storage của file chuyển sang **generation model**: mỗi lần compact tạo một generation directory mới và publish bằng atomic symlink rename (`.current`). `.data`/`.meta` là hard-link alias tương thích, không quyết định generation active; alias được cập nhật khi không còn handle legacy pin storage cũ. Generation cũ chỉ được GC thu hồi sau publication durable và khi không còn reference nào pin chính generation đó.

### Thiết kế chunk-based (cửa sổ thích nghi theo file)

Không gian logic của mỗi file được chia thành các cửa sổ cố định cho generation đó. File mới bắt đầu ở **64 KiB**; compaction có thể đổi từng bước ×2 trong khoảng **16 KiB–1 MiB** dựa trên tỷ lệ full-window write và partial-RMW. Mỗi cửa sổ chứa tối đa một chunk, luôn head-aligned theo `window_size` của file. Cửa sổ trống không có chunk — file sparse giữ nguyên, hole đọc ra byte 0. Mỗi chunk nén riêng bằng Zstd thành một blob append-only trên `.data`; `.meta` ánh xạ `logical_offset → physical_offset + raw_size + stored_size + codec + CRC32`.

**Read path:** file packed → binary search theo window base của file; file legacy chưa packed → linear scan (tầng tolerant, giữ vĩnh viễn) → `pread` blob → verify CRC32 → decompress → trả đúng offset/size.

**Write path:** handle giữ cache chunk map + storage generation; read/write không reload `.meta` khi epoch không đổi. Append, overwrite và ghi vào hole đi chung một đường: xác định các cửa sổ bị chạm → repack từng cửa sổ → Zstd nếu tiết kiệm ≥ 12.5%, ngược lại raw → append blob → `fdatasync` data → append một metadata delta có CRC + commit marker → `fdatasync` metadata. Checkpoint toàn map chỉ chạy sau 1024 delta hoặc khi journal vượt ngưỡng kích thước.

**Migration:** metadata v0 không tag và v1 được đọc với cửa sổ 64 KiB; metadata v2 lưu `window_size` trong header little-endian 64 byte. File có chunk layout cũ vẫn đọc được qua tầng linear scan; compaction repack sang generation v2 mà không trộn window size trong cùng một generation.

**Zstd context reuse:** mỗi FUSE worker thread tạo lười một `ZSTD_CCtx` và một `ZSTD_DCtx`, tái sử dụng chúng qua thread-local storage và tự giải phóng bằng destructor của `pthread_key_t` khi worker kết thúc. Context không bao giờ được chia sẻ giữa các thread, vì vậy đường nén/giải nén không thêm global lock và không tạo quan hệ lock-order mới với `cache_lock` hay per-path file lock. Compression giữ nguyên `ZSTD_CLEVEL_DEFAULT` và output byte-identical với API one-shot; decompression reset cả session lẫn sticky parameters trước mỗi frame. Lỗi Zstd loại context tương ứng để lần gọi sau tạo context sạch, còn lỗi cấp phát TLS tự động fallback về API one-shot với cùng semantics.

**Chunk-I/O scratch reuse:** mỗi thread có bốn slot TLS tách theo vai trò `RAW`, `COMP`, `WINDOW` và `CACHED`, nên các buffer đang sống đồng thời trong repack không alias nhau. Slot tăng theo high-water mark của thread, không shrink giữa các operation, và được destructor của một `pthread_key_t` riêng giải phóng khi thread kết thúc. `RAW`/`WINDOW`/`CACHED` giữ tối đa 1 MiB mỗi slot; `COMP` giữ tối đa `ZSTD_COMPRESSBOUND(1 MiB)` = 1,052,672 byte, nên trần retained thông thường là **4,198,400 byte/thread** đã chạm đủ bốn role. Request vượt cap, lỗi khởi tạo TLS, lỗi grow, hoặc acquisition cùng role khi slot còn bận đều dùng allocation tạm thời rồi free khi release; retained buffer cũ vẫn nguyên vẹn. `entries` trả về cho caller, plaintext buffer ở FUSE, copy buffer của compaction và prefix scratch của decompressor không thuộc pool này.

**Path-lock sharding:** bảng mutex theo path dùng 64 shard × 64 bucket, với FNV-1a 64-bit và fmix64 để phân bố path. Mỗi shard có mutex riêng; refcount pin cả owner, waiter và unlocker, entry được unlink khi refcount về 0 rồi mới destroy/free ngoài shard mutex. Code luôn nhả shard mutex trước khi chờ path mutex. Nếu một thao tác tương lai cần nhiều path lock, thứ tự chuẩn là tăng dần `(shard index, strcmp(path))`, deduplicate path trùng và release theo thứ tự ngược lại.

**Generation-registry sharding và GC hai pha:** registry là bảng 64 shard × 64 bucket độc lập với path-lock table, hash bằng FNV-1a + fmix64 chỉ trên `logical_path`; vì vậy generation cũ/mới của cùng path luôn ở chung một lock domain cho live handoff. GC chuyển `NONE → PENDING → CLAIMED`: claim copy identity, refcount snapshot, alias intent và claim ID dưới registry shard; resolve/alias/remove chạy ngoài shard (và dưới path lock trong luồng bình thường); finalize lấy lại đúng shard rồi revalidate record, state và claim ID trước khi retire hoặc đưa về `PENDING` để retry. Thứ tự nested lock là path lock → `cache_lock` → registry shard; read fast path dùng `cache_lock` → shard. `compact_queue_mu` luôn được nhả trước khi worker hoặc fallback chờ path lock. `release()` chạy GC đủ điều kiện đồng bộ dưới path lock + cache write lock, rồi mới schedule compaction sau khi nhả hai lock đó.

---

## Cấu trúc thư mục

```
transcomp/
├── Makefile
├── README.md
├── benchmark.sh                  ← Đo throughput, compression ratio, RMW latency
├── benchmark_results.txt         ← Kết quả benchmark lần chạy gần nhất
├── benchmarks/
│   ├── meta_inspect.c            ← Đọc metadata bằng parser production
│   ├── zstd_context_bench.c      ← So sánh one-shot với TLS context reuse
│   ├── chunkio_scratch_bench.c   ← malloc/free vs TLS scratch, không FUSE/I/O/Zstd
│   ├── lock_table_bench.c        ← So sánh lock table linear với sharded
│   └── fixtures/
│       └── lock_table_linear.c   ← Baseline frozen từ commit add0290
├── test_suite.sh                 ← FUSE regression suite (84 checks)
├── tests/                        ← Unit, concurrency, metadata/tooling tests
├── src/
│   ├── myfs.h                    ← Structs, constants, prototypes, LOG macro, CRC32 helper
│   ├── main.c                    ← Entry point, FUSE init/destroy, fuse_operations table
│   ├── core/
│   │   ├── path.c                ← build_path(), build_data_path(), build_meta_path()
│   │   ├── metadata.c            ← v0/v1/v2 reader, checkpoint + delta journal
│   │   ├── compress.c            ← zstd_compress(), zstd_decompress(), is_incompressible()
│   │   ├── chunkio.c             ← Engine chung: payload load, blob append, repack cửa sổ
│   │   ├── chunkio_scratch.c/.h  ← Bốn role scratch TLS có cap + fallback tạm thời
│   │   ├── lock.c                ← Per-file lock table (mutex theo path, refcount)
│   │   └── compact.c             ← Generation/GC + adaptive resize + live-handle handoff
│   ├── fuse_ops/
│   │   ├── file.c                ← myfs_read, myfs_write, myfs_truncate,
│   │   │                          myfs_create, myfs_open, myfs_release
│   │   └── dir.c                 ← myfs_getattr, myfs_readdir, myfs_mkdir,
│   │                              myfs_rmdir, myfs_unlink, myfs_utimens
│   └── guards/
│       ├── guards.h
│       └── guards.c              ← Validation functions: chunk metadata, bounds, pread result
├── benchmark-results/            ← Report cục bộ (git-ignored)
├── backing/                      ← Backing store (.data/.meta + generation dirs sau compaction)
└── mountpoint/                   ← Mount point (giao diện logic cho user)
```

---

## Cách build và chạy

### Yêu cầu

```bash
sudo apt install libfuse3-dev libzstd-dev zlib1g-dev pkg-config gcc
```

### Build

```bash
make          # compile
make release  # build -O2 -DNDEBUG
make clean    # xóa binary + backing store (chỉ khi đã unmount)
```

### Chạy

```bash
# Terminal 1 — mount filesystem
make run
# hoặc: ./myfs -f mountpoint ./backing

# Terminal 2 — sử dụng bình thường
echo "Hello World" > mountpoint/test.txt
cat mountpoint/test.txt
ls -la mountpoint/

# Unmount
make umount
```

### Regression test

```bash
# Cần FUSE đang chạy ở terminal khác
make test

# Không cần mount; chạy metadata/cache/resize/concurrency tests trực tiếp
make test-unit
```

84 FUSE regression checks cover: basic read/write, O\_TRUNC, partial overwrite (RMW), multi-chunk file (>64KB), compression/incompressible detection, magic byte heuristic, truncate (kể cả cắt giữa chunk nén), unlink, append, cross-boundary overwrite, persistence sau remount, garbage collection, sparse hole (đọc zero + write chồng lấn), thư mục >2048 entry, durability ordering, chunk packing, migration file legacy, ghi song song per-file locking và background compaction. `make test-unit` bổ sung metadata v0/v1/v2 + journal, cache/generation/concurrency, Zstd byte parity/error recovery/TLS isolation/destructor cleanup/sticky-parameter reset, chunk-I/O scratch reuse/grow/cap/oversize/nested-acquisition/error cleanup/thread isolation, path-lock refcount/reclamation/concurrency, metadata inspector và format output của cả ba microbenchmark.

### Benchmark

```bash
make bench
# Kết quả lưu vào benchmark_results.txt

make bench-zstd-context
# Microbenchmark one-shot vs TLS reuse: p50/p99, ops/s và 8-thread throughput

make bench-chunkio-scratch
# Standalone malloc/free vs scratch TLS; không gọi FUSE, I/O hay Zstd.
# 7 workload × 1/8 thread × 7 paired repetitions, thứ tự chạy xen kẽ.
# CSV chứa retained bytes/role, timed growth và temporary acquisition.

make bench-lock-table
# Linear frozen baseline vs 64×64 sharded table.
# Mặc định: worker 1/4/8/16 × resident path 0/64/256, 7 paired repetitions.
# Chạy gate chính nhanh hơn:
./benchmarks/lock_table_bench --workers 8 --resident-paths 256
# Control single-thread không có resident path:
./benchmarks/lock_table_bench --workers 1 --resident-paths 0

make benchmarks/meta_inspect
./benchmarks/meta_inspect backing/test.txt.meta
# chunks=N<TAB>logical_size=N<TAB>raw_chunks=N
```

10 benchmark sections: sequential write/read throughput, compression ratio theo workload, RMW latency, so sánh với ext4 baseline, heuristic skip throughput, FUSE overhead vs Zstd overhead breakdown, append pattern analysis. BM07 và BM10 gọi `meta_inspect`, nên dùng chung parser/validation/journal replay của `core/metadata.c` thay vì hard-code offset của format trên disk.

---

## Các quyết định kỹ thuật quan trọng

| Quyết định | Lý do |
|---|---|
| FUSE thay vì kernel module | Debug nhanh, không kernel panic, đủ để học semantics |
| Per-file window 16 KiB–1 MiB | Thu nhỏ cho RMW, tăng cho full-window sequential writes |
| Zstd thay vì zlib/LZ4 | Ratio cao nhất trong nhóm fast codec, decompress 1550 MB/s |
| Directory + `.data`/`.meta` | Dễ debug (hexdump trực tiếp), dễ implement atomic write |
| Append-only blob | Tránh in-place rewrite, đơn giản, atomic với rename |
| Delta journal + checkpoint | Metadata write O(changed windows), torn tail tự phục hồi |
| Path/cache/registry lock hierarchy | Mutation theo path lock → cache rwlock → registry shard; read fast path dùng cache → shard |
| Background compaction thread | `release()` chạy GC đủ điều kiện đồng bộ rồi schedule compaction; queue dedupe, drain khi unmount, fallback đồng bộ khi worker không chạy |
| Writer-preferring cache rwlock | Handoff không starvation; read fast path không gọi metadata syscall |
| Zstd context theo worker thread | Bỏ allocation mỗi call mà không share context, global lock hay serialization |
| Scratch TLS tách 4 role, có cap | Cho phép RAW/COMP/WINDOW/CACHED sống đồng thời; giới hạn retained memory và fallback an toàn khi nested/oversize |
| Path-lock table 64 × 64 | Giới hạn lookup theo bucket và contention theo shard, vẫn eager reclaim |
| Generation registry 64 × 64 + GC hai pha | Gom mọi generation cùng logical path vào một shard; filesystem I/O nằm ngoài shard lock |

---

## Kết quả benchmark (tóm tắt)

`chunkio_scratch_bench` là instrument chính cho thay đổi buffer: dùng trực tiếp production acquire/release, touch cache line đầu/cuối, warm-up trước timed phase và đo baseline/TLS theo cặp xen kẽ. Lần chạy mặc định 7 repetition ngày 2026-10-04 cho kết quả median sau (số tuyệt đối phụ thuộc host, không dùng speed threshold trong CI):

| Workload | 1 thread | 8 thread | Retained/thread | Timed growth/temp |
|---|---:|---:|---:|---:|
| RAW 64 KiB | 1.70× | 1.85× | 65,536 B | 0 / 0 |
| COMP bound 64 KiB | 1.73× | 1.70× | 65,824 B | 0 / 0 |
| Partial RMW 64 KiB | 2.23× | 2.17× | 196,896 B | 0 / 0 |
| Adaptive shrink 1 MiB → 512 KiB | 34.47× | 76.49× | 3,147,776 B | 0 / 0 |
| Mixed 16 KiB–1 MiB | 2.37× | 2.21× | 3,149,824 B | 0 / 0 |
| Max retained | 43.51× | 77.67× | 4,198,400 B | 0 / 0 |
| Oversize fallback | 1.03× | 0.99× | 0 B | 0 / temporary-only |

Gate output kiểm tra metric hữu hạn/dương, đủ mọi workload/mode/thread count, không grow sau warm-up, không temporary allocation ở workload thông thường, từng role không vượt cap, tổng retained không vượt 4,198,400 byte/thread, và oversize chỉ dùng allocation tạm mà không tăng retained capacity.

Microbenchmark chuyên biệt cho allocation context cho kết quả lặp lại trên hai lần đo độc lập:

| Workload | TLS reuse so với one-shot |
|---|---:|
| Single-thread, buffer 1–64 KiB | **1.27×–2.50×** nhanh hơn |
| 8 worker thread, buffer 4 KiB | **1.41×–1.67×** throughput |

Không thấy serialization hay lock contention. Đây là instrument chính cho thay đổi này vì nó tách đúng chi phí tạo/hủy `ZSTD_CCtx`/`ZSTD_DCtx` khỏi FUSE, I/O và page cache.

`benchmark.sh` end-to-end được chạy baseline/candidate năm lần xen kẽ với cold-cache đã xác minh. Mọi chênh lệch median đều nhỏ hơn spread min–max ngay trong cùng một phía; cả control ext4 không liên quan cũng dao động mạnh (ví dụ ext4 write 211–461 MB/s). Vì noise floor của host lớn hơn ngưỡng 5%, gate end-to-end được báo cáo **inconclusive**, không phải pass/fail. Hai control thuần Zstd vẫn ổn định qua cả hai lần so sánh: compress −0.04%/−0.67%, decompress +0.16%/+0.35%, không cho thấy regression tính toán.

### Generation registry / GC microbenchmark (pooled metrics)

`generation_registry_bench` gọi public API thật, không mount FUSE và không dùng `benchmark.sh` hay test hooks. Bốn workload: **control** chỉ chạy vòng tính toán CPU, không gọi registry/GC trong measured loop; **scalability** register → snapshot → bump epoch → unregister trên path riêng của mỗi thread; **hot-path** chạy cùng transaction trên một path chung; **gc-interference** snapshot/bump epoch trên path không liên quan trong khi background thread liên tục tạo/thu hồi generation thật. Các registry workload vẫn gồm path/cache locks của caller, không phải phép đo riêng thời gian registry mutex. Mode `real` dùng filesystem I/O thật; `synthetic-delay` vẫn dùng I/O thật nhưng thêm delay yêu cầu tại các syscall được wrap bên trong GC. Tải GC liên tục này không đại diện cho tần suất GC production thông thường.

Campaign pooled-metrics ngày 2026-10-09 đo candidate `743c2b467041994cc64dbcc2714a8952fe406f1e` với snapshot `origin/main` tại `7f8f335b7a00fc4d928475fa9d947d3a3187b00a`. Host: Intel Core i5-12450H, ext4. Set A pin foreground `0,2,4,6`, GC `8`; set B pin foreground `1,3,5,7`, GC `9`. Cặp `0/1`, `2/3`, `4/5`, `6/7` là SMT siblings: A/B không phải hai nhóm core vật lý độc lập; GC không dùng SMT sibling của foreground. Mỗi bên chạy thread count `1,2,4`, bảy primary repetitions, warmup 128 operation/thread; basic workload 200000 iteration/thread. GC foreground chạy ít nhất 100000 operation/thread **và** ít nhất 20 completed GC cycles/repetition, nên count thực tế có thể lớn hơn. Threshold strict `> 1000 µs` (`> 1000000 ns`), synthetic delay 250 µs/call, seed 25228. Normal runs và A/A runs riêng được lưu cho cả hai tree; các commit test/docs sau campaign không phải lần đo performance mới.

Nguồn: các raw artifact cục bộ `/tmp/grb-pooled-set{A,B}-{current,main,current-aa,main-aa}.out` của campaign này, không được version-control cùng README.

Median throughput, đơn vị **operation/s tổng**; một operation là transaction/loop nêu trên. Tỷ số là current / baseline: >1 nghĩa là throughput đo được cao hơn, <1 là thấp hơn.

| Workload | Set / threads | Baseline | Current | Current / baseline |
|---|---|---:|---:|---:|
| Distinct paths | A / 2 | 517,975 | 1,452,712 | 2.805× |
| Distinct paths | A / 4 | 433,616 | 2,620,246 | 6.043× |
| Distinct paths | B / 2 | 507,407 | 1,446,853 | 2.851× |
| Distinct paths | B / 4 | 446,372 | 2,595,749 | 5.815× |
| Same-path hot-path | A / 4 | 470,971 | 447,765 | 0.951× |
| Same-path hot-path | B / 4 | 465,890 | 452,790 | 0.972× |

Distinct-path scaling tăng trong các dòng trên, nhưng same-path hot-path có regression throughput quan sát được. Dòng hot-path B / 4 không vượt A/A spread của chính dòng đó; không kết luận mọi chênh lệch nhỏ là regression đã xác lập.

GC-interference, bốn foreground threads: **p99.9 (µs)**, baseline → current; đây là median của percentile từng repetition, không phải percentile pooled.

| Set | I/O mode | Baseline p99.9 | Current p99.9 |
|---|---|---:|---:|
| A | Real ext4 | 8.732 | 3.663 |
| A | Synthetic-delay trên ext4 | 8.951 | 3.696 |
| B | Real ext4 | 11.274 | 3.236 |
| B | Synthetic-delay trên ext4 | 11.901 | 3.030 |

Các case GC đại diện, bốn foreground threads, pooled qua bảy primary repetitions. Fraction dưới đây là tỷ lệ 0–1, không phải phần trăm; denominator là operation count thực tế, không phải `iterations × threads × repetitions`.

| Case | Tree | delayed_total / pooled_operations | pooled_delayed_fraction | delayed_per_gc_cycle |
|---|---|---:|---:|---:|
| A / real | Baseline | 623 / 3,824,159 | 0.000162912 | 3.643274854 |
| A / real | Current | 16 / 7,683,635 | 0.000002082 | 0.108843537 |
| B / synthetic-delay | Baseline | 662 / 2,908,335 | 0.000227622 | 3.343434343 |
| B / synthetic-delay | Current | 1 / 14,037,798 | 0.000000071 | 0.006802721 |

Định nghĩa population trong CSV (mọi measured operation có một latency observation; warmup bị loại, A/A companion không vào primary pooled totals):

| Metric | Định nghĩa |
|---|---|
| `median_rep_delayed_fraction` | Median của fraction từng primary repetition, chỉ đếm latency strict > threshold; có thể bằng 0 dù pooled có delayed operation |
| `delayed_total` | Tổng operation strict > threshold qua các primary repetitions |
| `pooled_operations` | Tổng operation count thực đo qua các primary repetitions |
| `pooled_delayed_fraction` | `delayed_total / pooled_operations`; denominator 0 trả về 0 |
| `delayed_per_gc_cycle` | `delayed_total / tổng completed primary GC cycles`; không có GC trả về 0 theo quy ước |
| `max_ns` | Latency lớn nhất qua tất cả primary repetitions, đơn vị ns; informational |
| `p50_ns`, `p99_ns`, `p999_ns` | Median của p50/p99/p99.9 từng primary repetition, đơn vị ns, không phải pooled percentile |

Median-per-repetition và pooled metrics mô tả population khác nhau. Completed GC cycles dùng cửa sổ toàn run: GC bắt đầu trước foreground measured interval và cycle cuối có thể hoàn tất sau foreground kết thúc; vì vậy denominator của `delayed_per_gc_cycle` **không khớp hoàn toàn cửa sổ foreground**, và metric này không chứng minh từng delay do GC gây ra. Public API không cung cấp lock-hold intervals để đo registry lock duty cycle.

**Tái lập** (Bash, từ repo root trên ext4; cần các build dependencies ở trên). Build cùng benchmark source với current core và baseline core thật trong worktree ngoài repo; giữ hash baseline để `origin/main` thay đổi không làm đổi phép so sánh. `TREE,hash=` trong output ghi tree thực sự được build. Các lệnh dưới đây tái lập campaign, không phải đã chạy lại khi viết tài liệu:

```bash
registry_artifacts=$(mktemp -d /tmp/myfs-registry-results.XXXXXX)
git worktree add --detach "$registry_artifacts/baseline-tree" \
    7f8f335b7a00fc4d928475fa9d947d3a3187b00a
make build-generation-registry-bench
make build-generation-registry-baseline \
    BASELINE_TREE="$registry_artifacts/baseline-tree" \
    BASELINE_OUTPUT="$registry_artifacts/baseline-bench"
./benchmarks/generation_registry_bench --help

registry_workdir=$(mktemp -d "$PWD/.registry-bench-io.XXXXXX")
findmnt -T "$registry_workdir" -o TARGET,SOURCE,FSTYPE
# Xác nhận ext4; không dùng /tmp nếu filesystem đó là tmpfs.
registry_args=(--workdir "$registry_workdir" --threads 1,2,4
    --iterations 200000 --gc-iterations 100000 --min-gc-cycles 20
    --repetitions 7 --warmup 128 --gc-threads 1 --threshold-us 1000
    --synthetic-delay-us 250 --gc-mode both --seed 25228)
for registry_set in A B; do
    if [[ $registry_set == A ]]; then
        registry_cpus=0,2,4,6; registry_gc_cpu=8
    else
        registry_cpus=1,3,5,7; registry_gc_cpu=9
    fi
    for registry_role in current baseline current-aa baseline-aa; do
        case $registry_role in
            current*) registry_binary=./benchmarks/generation_registry_bench ;;
            baseline*) registry_binary="$registry_artifacts/baseline-bench" ;;
        esac
        registry_aa=()
        case $registry_role in *-aa) registry_aa=(--aa) ;; esac
        timeout 300 "$registry_binary" "${registry_args[@]}" \
            --cpus "$registry_cpus" --gc-cpu "$registry_gc_cpu" \
            "${registry_aa[@]}" \
            >"$registry_artifacts/$registry_set-$registry_role.out" \
            2>"$registry_artifacts/$registry_set-$registry_role.stderr"
    done
    ./benchmarks/compare_generation_registry_results.sh \
        "$registry_artifacts/$registry_set-current.out" \
        "$registry_artifacts/$registry_set-baseline.out" \
        "$registry_artifacts/$registry_set-current-aa.out" \
        "$registry_artifacts/$registry_set-baseline-aa.out" \
        >"$registry_artifacts/$registry_set-comparison.csv"
done
```

`--aa` đo cặp cùng binary, thứ tự primary/companion xen kẽ; spread là `100 × (high − low) / low`. Comparison script lấy throughput normal runs rồi so absolute difference với A/A gate của chính workload/mode/thread-count và global gate. Max paired A/A spread qua mọi dòng là **29.07% (A)** và **53.58% (B)**; chỉ dùng global max sẽ che khác biệt giữa các dòng. `noise_floor_pct=0` với `win_gate=unavailable` ở normal run không phải bằng chứng noise bằng 0. A/A riêng cho từng tree cho thấy variability; giữa các lần chạy vẫn có drift, nên chênh lệch nhỏ có thể inconclusive và throughput gate không phải kiểm định latency percentile.

**Giới hạn:** OS scheduling/preemption ảnh hưởng latency maxima. Control đo noise của host nhưng không cho phép gán chính xác từng stall cho GC: riêng control baseline A / 4 đã có 9 / 5,600,000 operation vượt 1 ms, còn `delayed_per_gc_cycle=0` chỉ là quy ước vì không chạy GC. Current tree vẫn có operation vượt 1 ms như bảng pooled ở trên. Các số là quan sát trong workload/pinning này, không tự chứng minh nguyên nhân; tỷ số GC-interference throughput **không phải production speedup**, và kết quả **không xác lập performance FUSE end-to-end**.

---

## Known limitations

- **Chunk legacy cực lớn decompress nguyên khối:** file từ format cũ có chunk đã merge rất lớn sẽ được giải nén nguyên khối vào RAM ở lần chạm đầu (migration); streaming Zstd là follow-up nếu thành vấn đề.
- **Một worker compaction duy nhất:** queue FIFO một thread — đủ cho tải hiện tại; nhiều worker là bước mở rộng sau.

Các hạng mục đã hoàn thành (regression test TC19–TC29): sparse-hole write chồng lấn → stale read; hole đọc ra EOF thay vì byte 0; truncate vào giữa chunk nén làm hỏng chunk; `readdir` mất entry sau 2048 file; `fdatasync` blob trước khi publish metadata (crash-safe ordering); **chunk packing theo generation** — cửa sổ head-aligned theo `window_size`, mặc định 64 KiB và thích nghi trong khoảng 16 KiB–1 MiB, append merge vào chunk đuôi, read lookup theo cửa sổ, migration tự động cho file legacy; **locking phân tầng** — thao tác trên file khác nhau chạy song song, mutation theo path lock → cache rwlock → registry shard, fast read theo cache → shard, queue mutex không được giữ khi chờ path lock; **background compaction + synchronous release GC** — `release()` unregister handle và chạy GC đủ điều kiện đồng bộ dưới path/cache locks, rồi mới schedule compaction; worker compact dưới path lock và drain sạch queue khi unmount, còn khi worker không chạy thì scheduling fallback đồng bộ.

---

## Debug

Log in ra stderr với timestamp millisecond:

```
[13:05:01.234] [DEBUG] write: /test.txt offset=0 size=12
[13:05:01.235] [DEBUG] write: compressed 12 → 8 bytes (66.7%)
[13:05:01.236] [DEBUG] write OK: chunks=1 logical_size=12 codec=1
[13:05:01.240] [DEBUG] myfs_read: /test.txt offset=0 size=4096
[13:05:01.241] [DEBUG] myfs_read success: read 12 bytes
```

Xem backing store trực tiếp. Dùng inspector cho thông tin metadata có ngữ nghĩa; tool này gọi parser production nên hỗ trợ cùng v0/v1/v2 và journal replay như filesystem:

```bash
ls -la backing/                          # .data/.meta của mỗi file
make benchmarks/meta_inspect
./benchmarks/meta_inspect backing/test.txt.meta
hexdump -C backing/test.txt.meta         # chỉ dùng khi cần xem wire bytes
```
