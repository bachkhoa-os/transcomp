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

Sau lần compaction đầu tiên, storage của file chuyển sang **generation model**: mỗi lần compact tạo một generation directory mới, publish bằng atomic symlink rename (`.current`), còn `.data`/`.meta` được giữ dưới dạng hard-link alias để debug/benchmark vẫn quan sát được file vật lý đang active. Generation cũ được GC thu hồi khi không còn handle nào mở.

### Thiết kế chunk-based (cửa sổ thích nghi theo file)

Không gian logic của mỗi file được chia thành các cửa sổ cố định cho generation đó. File mới bắt đầu ở **64 KiB**; compaction có thể đổi từng bước ×2 trong khoảng **16 KiB–1 MiB** dựa trên tỷ lệ full-window write và partial-RMW. Mỗi cửa sổ chứa tối đa một chunk, luôn head-aligned theo `window_size` của file. Cửa sổ trống không có chunk — file sparse giữ nguyên, hole đọc ra byte 0. Mỗi chunk nén riêng bằng Zstd thành một blob append-only trên `.data`; `.meta` ánh xạ `logical_offset → physical_offset + raw_size + stored_size + codec + CRC32`.

**Read path:** file packed → binary search theo window base của file; file legacy chưa packed → linear scan (tầng tolerant, giữ vĩnh viễn) → `pread` blob → verify CRC32 → decompress → trả đúng offset/size.

**Write path:** handle giữ cache chunk map + storage generation; read/write không reload `.meta` khi epoch không đổi. Append, overwrite và ghi vào hole đi chung một đường: xác định các cửa sổ bị chạm → repack từng cửa sổ → Zstd nếu tiết kiệm ≥ 12.5%, ngược lại raw → append blob → `fdatasync` data → append một metadata delta có CRC + commit marker → `fdatasync` metadata. Checkpoint toàn map chỉ chạy sau 1024 delta hoặc khi journal vượt ngưỡng kích thước.

**Migration:** metadata v0 không tag và v1 được đọc với cửa sổ 64 KiB; metadata v2 lưu `window_size` trong header little-endian 64 byte. File có chunk layout cũ vẫn đọc được qua tầng linear scan; compaction repack sang generation v2 mà không trộn window size trong cùng một generation.

**Zstd context reuse:** mỗi FUSE worker thread tạo lười một `ZSTD_CCtx` và một `ZSTD_DCtx`, tái sử dụng chúng qua thread-local storage và tự giải phóng bằng destructor của `pthread_key_t` khi worker kết thúc. Context không bao giờ được chia sẻ giữa các thread, vì vậy đường nén/giải nén không thêm global lock và không tạo quan hệ lock-order mới với `cache_lock` hay per-path file lock. Compression giữ nguyên `ZSTD_CLEVEL_DEFAULT` và output byte-identical với API one-shot; decompression reset cả session lẫn sticky parameters trước mỗi frame. Lỗi Zstd loại context tương ứng để lần gọi sau tạo context sạch, còn lỗi cấp phát TLS tự động fallback về API one-shot với cùng semantics.

**Path-lock sharding:** bảng mutex theo path dùng 64 shard × 64 bucket, với FNV-1a 64-bit và fmix64 để phân bố path. Mỗi shard có mutex riêng; refcount pin cả owner, waiter và unlocker, entry được unlink khi refcount về 0 rồi mới destroy/free ngoài shard mutex. Code luôn nhả shard mutex trước khi chờ path mutex. Nếu một thao tác tương lai cần nhiều path lock, thứ tự chuẩn là tăng dần `(shard index, strcmp(path))`, deduplicate path trùng và release theo thứ tự ngược lại.

---

## Cấu trúc thư mục

```
transcomp/
├── Makefile
├── README.md
├── benchmark.sh                 ← Đo throughput, compression ratio, RMW latency
├── benchmark_results.txt        ← Kết quả benchmark lần chạy gần nhất
├── benchmarks/
│   ├── meta_inspect.c           ← Đọc metadata bằng parser production
│   ├── zstd_context_bench.c     ← So sánh one-shot với TLS context reuse
│   ├── lock_table_bench.c       ← So sánh lock table linear với sharded
│   └── fixtures/
│       └── lock_table_linear.c  ← Baseline frozen từ commit add0290
├── test_suite.sh                ← FUSE regression suite (84 checks)
├── tests/                       ← Unit, concurrency, metadata/tooling tests
├── src/
│   ├── myfs.h                   ← Structs, constants, prototypes, LOG macro, CRC32 helper
│   ├── main.c                   ← Entry point, FUSE init/destroy, fuse_operations table
│   ├── core/
│   │   ├── path.c               ← build_path(), build_data_path(), build_meta_path()
│   │   ├── metadata.c           ← v0/v1/v2 reader, checkpoint + delta journal
│   │   ├── compress.c           ← zstd_compress(), zstd_decompress(), is_incompressible()
│   │   ├── chunkio.c            ← Engine chung: payload load, blob append, repack cửa sổ
│   │   ├── lock.c              ← Per-file lock table (mutex theo path, refcount)
│   │   └── compact.c            ← Generation/GC + adaptive resize + live-handle handoff
│   ├── fuse_ops/
│   │   ├── file.c               ← myfs_read, myfs_write, write_rmw, myfs_truncate,
│   │   │                         myfs_create, myfs_open, myfs_release
│   │   └── dir.c                ← myfs_getattr, myfs_readdir, myfs_mkdir,
│   │                             myfs_rmdir, myfs_unlink, myfs_utimens
│   └── guards/
│       ├── guards.h
│       └── guards.c             ← Validation functions: chunk metadata, bounds, pread result
├── benchmark-results/           ← Report cục bộ (git-ignored)
├── backing/                     ← Backing store (.data/.meta + generation dirs sau compaction)
└── mountpoint/                  ← Mount point (giao diện logic cho user)
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

84 FUSE regression checks cover: basic read/write, O\_TRUNC, partial overwrite (RMW), multi-chunk file (>64KB), compression/incompressible detection, magic byte heuristic, truncate (kể cả cắt giữa chunk nén), unlink, append, cross-boundary overwrite, persistence sau remount, garbage collection, sparse hole (đọc zero + write chồng lấn), thư mục >2048 entry, durability ordering, chunk packing, migration file legacy, ghi song song per-file locking và background compaction. `make test-unit` bổ sung metadata v0/v1/v2 + journal, cache/generation/concurrency, Zstd byte parity/error recovery/TLS isolation/destructor cleanup/sticky-parameter reset, path-lock refcount/reclamation/concurrency, metadata inspector và format output của cả hai microbenchmark.

### Benchmark

```bash
make bench
# Kết quả lưu vào benchmark_results.txt

make bench-zstd-context
# Microbenchmark one-shot vs TLS reuse: p50/p99, ops/s và 8-thread throughput

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
| Per-file lock + leaf mutex | File khác nhau chạy song song; thứ tự lock cố định → không deadlock |
| Background compaction thread | `release()` không trả tiền GC/compact; queue dedupe, drain khi unmount |
| Writer-preferring cache rwlock | Handoff không starvation; read fast path không gọi metadata syscall |
| Zstd context theo worker thread | Bỏ allocation mỗi call mà không share context, global lock hay serialization |
| Path-lock table 64 × 64 | Giới hạn lookup theo bucket và contention theo shard, vẫn eager reclaim |

---

## Kết quả benchmark (tóm tắt)

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

Các hạng mục đã hoàn thành (regression test TC19–TC29): sparse-hole write chồng lấn → stale read; hole đọc ra EOF thay vì byte 0; truncate vào giữa chunk nén làm hỏng chunk; `readdir` mất entry sau 2048 file; `fdatasync` blob trước khi publish metadata (crash-safe ordering); **chunk packing 64 KB thực sự** — bất biến cửa sổ head-aligned, append merge vào chunk đuôi, read lookup theo cửa sổ, migration tự động cho file legacy; **per-file locking** — thao tác trên các file khác nhau chạy song song, registry/queue dùng leaf mutex riêng (thứ tự lock cố định: file lock → leaf, không thể deadlock); **background compaction** — `release()` chỉ enqueue, worker thread compact dưới file lock của path, drain sạch queue khi unmount.

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
