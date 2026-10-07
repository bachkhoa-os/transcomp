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
