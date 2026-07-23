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
│    write_rmw()  ─► RMW          │
│                                 │
│  core/                          │
│    compress.c   Zstd + heuristic│
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

### Thiết kế chunk-based

Dữ liệu được chia thành các **chunk 64 KB độc lập**. Mỗi chunk được nén riêng bằng Zstd và lưu thành một blob trên `.data`. Metadata ánh xạ `logical_offset → physical_offset + raw_size + codec_type + CRC32` được lưu trong `.meta`.

**Read path:** tra chunk map → `pread` blob → verify CRC32 → decompress → trả buffer đúng offset/size.

**Write path:** thử Zstd compress → nếu tiết kiệm ≥ 12.5% thì lưu compressed (codec=1), ngược lại lưu raw (codec=0) → append blob vào cuối `.data` → cập nhật chunk map.

**Partial overwrite (RMW):** gom tất cả chunk bị overlap → decompress vào working buffer → patch → recompress → append blob mới → merge chunk array → save chunk map.

---

## Cấu trúc thư mục

```
transcomp/
├── Makefile
├── README.md
├── benchmark.sh          ← Đo throughput, compression ratio, RMW latency
├── benchmark_results.txt ← Kết quả benchmark lần chạy gần nhất
├── demo.sh               ← Script demo bảo vệ (6 bước, auto mount/unmount)
├── test_suite.sh         ← Regression test suite (49 test cases)
├── src/
│   ├── myfs.h            ← Structs, constants, prototypes, LOG macro, CRC32 helper
│   ├── main.c            ← Entry point, FUSE init/destroy, fuse_operations table
│   ├── core/
│   │   ├── path.c        ← build_path(), build_data_path(), build_meta_path()
│   │   ├── metadata.c    ← load_chunk_map(), save_chunk_map() với atomic write
│   │   ├── compress.c    ← zstd_compress(), zstd_decompress(), is_incompressible()
│   │   └── compact.c     ← Compaction sang generation mới + generation GC registry
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

49 test cases cover: basic read/write, O\_TRUNC, partial overwrite (RMW), multi-chunk file (>64KB), compression/incompressible detection, magic byte heuristic, truncate, unlink, append, cross-boundary overwrite, persistence sau remount, garbage collection.

### Benchmark

```bash
make bench
# Kết quả lưu vào benchmark_results.txt
```

10 benchmark sections: sequential write/read throughput, compression ratio theo workload, RMW latency, so sánh với ext4 baseline, heuristic skip throughput, FUSE overhead vs Zstd overhead breakdown, append pattern analysis.

### Demo bảo vệ

```bash
make demo
# Tự động: build → mount → 6 bước demo → unmount
```

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
| Write text 100MB | ~166–298 MB/s | ~1220 MB/s |
| Write random 100MB | ~120–246 MB/s | ~1190 MB/s |
| Read text 50MB | ~109–248 MB/s | ~1205 MB/s |
| Compression (text lặp lại) | 9000–20000x | 1.00x |
| Compression (source code) | 1.27x | 1.00x |
| Compression (random binary) | 1.00x | 1.00x |
| RMW avg latency | ~64–110 ms | N/A |
| Zstd compress (in-memory) | ~4600 MB/s | — |

> **Ghi chú:** Overhead chính là FUSE context switch (~7x so với ext4), không phải Zstd (~30x nhanh hơn throughput của myfs khi đo in-memory).

---

## Known limitations

- **Chunk packing chưa thực sự 64 KB:** mỗi `write()` call tạo 1 chunk riêng. File nhỏ nhiều write call sẽ có nhiều chunk nhỏ → compression ratio thấp hơn lý thuyết. `CHUNK_SIZE` hiện chỉ dùng làm ngưỡng cho compaction.
- **GC/compaction synchronous:** generation GC và compaction chạy trong `release()` dưới global mutex, không có background thread.
- **Global mutex:** mọi thao tác mutate serialize qua một mutex toàn cục (per-file locking là bước cải tiến sau, không đổi on-disk format).
- **Crash recovery một phần:** metadata và compaction đã crash-safe (generation + owner marker + fsync parent dir + lazy recovery khi open/getattr), nhưng data blob chỉ được fsync khi `release()` — crash trước đó có thể để `.meta` trỏ tới blob chưa persist. CRC32 phát hiện trường hợp này và trả `-EIO` thay vì đọc sai dữ liệu.
- **Truncate vào giữa chunk nén làm hỏng chunk:** truncate về size nằm giữa một chunk `codec_type=1` chỉ giảm `stored_size` mà không ghi lại blob → lần đọc sau decompress vào buffer nhỏ hơn frame thật → `-EIO`.
- **Write vào sparse hole chồng lên chunk sau → stale read:** write bắt đầu trong hole nhưng kéo dài qua chunk hiện có sẽ append chunk mới chồng lấn dải logic; read scan tuyến tính gặp chunk cũ trước nên vùng chồng lấn đọc ra dữ liệu cũ.
- **`readdir` giới hạn 2048 entry:** bảng dedup cố định trên stack; entry logic vượt quá 2048 không được emit (biến mất khỏi `ls`).

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
