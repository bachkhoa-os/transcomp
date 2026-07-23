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
│    repack 64KB windows (RMW)    │
│                                 │
│  core/                          │
│    compress.c   Zstd + heuristic│
│    chunkio.c    64KB window I/O │
│    metadata.c   chunk map I/O   │
│    compact.c    garbage collect │
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

### Thiết kế chunk-based (cửa sổ 64 KB)

Không gian logic của file chia thành các **cửa sổ 64 KB**; mỗi cửa sổ chứa tối đa một chunk, luôn **head-aligned**: `logical_offset = k×64K`, `stored_size ≤ 64K`, mảng chunk sắp xếp tăng nghiêm ngặt (⇒ không chồng lấn, không vắt qua cửa sổ). Cửa sổ trống không có chunk — file sparse giữ nguyên, hole đọc ra byte 0. Mỗi chunk nén riêng bằng Zstd thành một blob append-only trên `.data`; `.meta` ánh xạ `logical_offset → physical_offset + raw_size + stored_size + codec + CRC32`.

**Read path:** file packed → binary search theo `WINDOW_BASE(offset)`; file legacy chưa packed → linear scan (tầng tolerant, giữ vĩnh viễn) → `pread` blob → verify CRC32 → decompress → trả đúng offset/size.

**Write path:** append, overwrite và ghi vào hole đi chung một đường: xác định các cửa sổ bị chạm → repack từng cửa sổ (merge dữ liệu cũ + patch; cửa sổ được patch phủ trọn thì nén thẳng từ buffer người dùng) → Zstd nếu tiết kiệm ≥ 12.5%, ngược lại raw → append blob → **một** `fdatasync` cho cả batch → save chunk map (atomic rename). Append vào chunk đuôi chưa đầy merge vào chunk đó thay vì tạo chunk mới — nhiều write nhỏ không còn làm nở chunk map.

**Migration:** file từ phiên bản cũ (chunk kích thước tuỳ ý) vẫn đọc được qua tầng linear scan; lần compaction kế tiếp (trong `release()`) repack toàn bộ về bất biến cửa sổ. Tính packed được **derive khi load**, không persist — flag stale sau crash tự lành.

---

## Cấu trúc thư mục

```
transcomp/
├── Makefile
├── README.md
├── benchmark.sh          ← Đo throughput, compression ratio, RMW latency
├── benchmark_results.txt ← Kết quả benchmark lần chạy gần nhất
├── test_suite.sh         ← Regression test suite (49 test cases)
├── src/
│   ├── myfs.h            ← Structs, constants, prototypes, LOG macro, CRC32 helper
│   ├── main.c            ← Entry point, FUSE init/destroy, fuse_operations table
│   ├── core/
│   │   ├── path.c        ← build_path(), build_data_path(), build_meta_path()
│   │   ├── metadata.c    ← load_chunk_map(), save_chunk_map() với atomic write
│   │   ├── compress.c    ← zstd_compress(), zstd_decompress(), is_incompressible()
│   │   ├── chunkio.c     ← Engine chung: payload load, blob append, repack cửa sổ 64KB
│   │   └── compact.c     ← Compaction sang generation mới + GC registry + migration repack
│   ├── fuse_ops/
│   │   ├── file.c        ← myfs_read, myfs_write, write_rmw, myfs_truncate,
│   │   │                    myfs_create, myfs_open, myfs_release
│   │   └── dir.c         ← myfs_getattr, myfs_readdir, myfs_mkdir,
│   │                        myfs_rmdir, myfs_unlink, myfs_utimens
│   └── guards/
│       ├── guards.h
│       └── guards.c      ← Validation functions: chunk metadata, bounds, pread result
├── backing/              ← Backing store (.data/.meta + generation dirs sau compaction)
└── mountpoint/           ← Mount point (giao diện logic cho user)
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
```

80 test cases cover: basic read/write, O\_TRUNC, partial overwrite (RMW), multi-chunk file (>64KB), compression/incompressible detection, magic byte heuristic, truncate (kể cả cắt giữa chunk nén), unlink, append, cross-boundary overwrite, persistence sau remount, garbage collection, sparse hole (đọc zero + write chồng lấn), thư mục >2048 entry, durability ordering, chunk packing 64KB, migration file legacy.

### Benchmark

```bash
make bench
# Kết quả lưu vào benchmark_results.txt
```

10 benchmark sections: sequential write/read throughput, compression ratio theo workload, RMW latency, so sánh với ext4 baseline, heuristic skip throughput, FUSE overhead vs Zstd overhead breakdown, append pattern analysis.

---

## Các quyết định kỹ thuật quan trọng

| Quyết định | Lý do |
|---|---|
| FUSE thay vì kernel module | Debug nhanh, không kernel panic, đủ để học semantics |
| Chunk size 64 KB | Cân bằng compression ratio vs chi phí partial overwrite |
| Zstd thay vì zlib/LZ4 | Ratio cao nhất trong nhóm fast codec, decompress 1550 MB/s |
| Directory + `.data`/`.meta` | Dễ debug (hexdump trực tiếp), dễ implement atomic write |
| Append-only blob | Tránh in-place rewrite, đơn giản, atomic với rename |
| Checkpoint + atomic rename | Tránh journaling phức tạp, đủ cho prototype 8 tuần |

---

## Kết quả benchmark (tóm tắt)

| Metric | myfs | ext4 baseline |
|---|---|---|
| Write text 100MB | ~162 MB/s | ~450 MB/s |
| Write random 100MB | ~139 MB/s | ~198 MB/s |
| Read text 50MB | ~410 MB/s | ~3704 MB/s |
| Read random 50MB | ~1786 MB/s | ~3846 MB/s |
| Compression (text lặp lại) | ~3277x | 1.00x |
| Compression (source code) | 1.27x | 1.00x |
| Compression (random binary) | 1.00x | 1.00x |
| RMW avg latency | ~81 ms | N/A |
| Append 1KB ×1024 (packing) | 17 chunk, disk 1KB (~860x) | — |
| Zstd compress (in-memory) | ~4400 MB/s | — |

> **Ghi chú:** Số đo sau khi thêm `fdatasync` blob mỗi write (giá của crash-safe ordering) và 64KB window packing. Baseline ext4 giờ đo trên thư mục disk-backed thay vì tmpfs nên thấp hơn số cũ — tỷ lệ myfs/ext4 mới phản ánh đúng hơn. Overhead chính vẫn là FUSE round-trip + device flush, không phải Zstd (~4400 MB/s in-memory).

---

## Known limitations

- **GC/compaction synchronous:** generation GC và compaction chạy trong `release()` dưới global mutex, không có background thread. *(kế hoạch: upscale phase)*
- **Global mutex:** mọi thao tác mutate serialize qua một mutex toàn cục (per-file locking là bước cải tiến sau, không đổi on-disk format). *(kế hoạch: upscale phase)*
- **Chunk legacy cực lớn decompress nguyên khối:** file từ format cũ có chunk đã merge rất lớn sẽ được giải nén nguyên khối vào RAM ở lần chạm đầu (migration); streaming Zstd là follow-up nếu thành vấn đề.

Các hạng mục đã hoàn thành (regression test TC19–TC27): sparse-hole write chồng lấn → stale read; hole đọc ra EOF thay vì byte 0; truncate vào giữa chunk nén làm hỏng chunk; `readdir` mất entry sau 2048 file; `fdatasync` blob trước khi publish metadata (crash-safe ordering); **chunk packing 64 KB thực sự** — bất biến cửa sổ head-aligned, append merge vào chunk đuôi, read lookup theo cửa sổ, migration tự động cho file legacy qua compaction.

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

Xem backing store trực tiếp:

```bash
ls -la backing/          # thấy .data và .meta cho mỗi file
hexdump -C backing/test.txt.meta   # xem chunk map binary
```
