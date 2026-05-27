# Transparent Compression FUSE

Project môn Hệ điều hành - Adding Transparent Compression Support to the File System - Nhóm 25228

## Giới thiệu
Đây là sản phẩm hoàn chỉnh của File System sử dụng FUSE 3 hỗ trợ transparent compression (nén/giải nén trong suốt).  
Hệ thống cho phép các ứng dụng gọi read/write bình thường; bên dưới file system tự động nén/giải nén theo chunk 64 KB (sử dụng thuật toán Zstd), xử lý trơn tru ghi đè từng phần (Read-Modify-Write) và đảm bảo lưu metadata bền vững (persistence) kể cả sau khi remount.

## Giai đoạn hiện tại (Tuần 8 - Sprint 8: Hoàn thiện)
**Trạng thái:** Dự án đã đáp ứng đầy đủ 100% các yêu cầu chức năng và phi chức năng. Tập trung hiện tại là đóng gói codebase, chuẩn bị slide báo cáo, viết final report và script demo.

**Thành quả kỹ thuật cốt lõi đạt được:**
- **Kiến trúc Module hóa (Refactored):** Codebase đã được tách nhỏ từ các file khổng lồ thành mô hình module rõ ràng (`core/`, `fuse_ops/`, `guards/`), rất dễ bảo trì và mở rộng.
- **Tính toàn vẹn (Data Correctness):** Test suite pass 100% các corner cases (file > 64KB, partial overwrite cắt ngang ranh giới, truncate, append). Metadata đảm bảo checksum khớp 100% sau khi unmount/remount.
- **Tối ưu hóa (Heuristics & GC):** Hệ thống có khả năng nhận diện định dạng không thể nén (JPEG, ZIP...) để bỏ qua bước nén giúp tiết kiệm CPU. Tích hợp cơ chế Garbage Collection (Compaction) khi đóng tệp (`release`) để dọn dẹp các orphan blobs sinh ra do RMW.
- **Hiệu năng kiểm chứng (Benchmarked):** Hoạt động đọc/ghi dữ liệu nén đạt hiệu suất cao, xác định rõ overhead phân bổ giữa tiến trình userspace (FUSE) và Zstd. 

## Cấu trúc thư mục (Đã Refactor)

```markdown
transcomp/
├── Makefile               ← Script biên dịch dự án
├── README.md
├── benchmark.sh           ← Đo throughput, compression ratio, latency (RMW)
├── test_suite.sh          ← Chạy regression test (Corner cases, RMW, Data Integrity)
├── src/
│   ├── main.c             ← Entry point & quản lý vòng đời FUSE (init, destroy)
│   ├── myfs.h             ← Cấu trúc dữ liệu, constants và header chung
│   ├── core/              ← Nhóm logic nghiệp vụ lõi (Filesystem Internals)
│   │   ├── path.c         ← Xử lý ánh xạ đường dẫn logic -> physical
│   │   ├── metadata.c     ← Load/Save metadata (chunk map) an toàn
│   │   ├── compress.c     ← Tích hợp nén Zstd và Heuristic nhận diện file ZIP/JPEG
│   │   └── compact.c      ← Garbage Collection thu hồi dung lượng orphan blobs
│   ├── fuse_ops/          ← Nhóm xử lý FUSE Callbacks (Giao tiếp Kernel VFS)
│   │   ├── file.c         ← Thao tác nội dung tệp (read, write, write_rmw, truncate, release)
│   │   └── dir.c          ← Thao tác namespace (getattr, readdir, mkdir, unlink...)
│   └── guards/            ← Nhóm validate dữ liệu & memory safety
│       ├── guards.h
│       └── guards.c       
├── backing/               ← Thư mục backing store lưu dữ liệu thật (.data và .meta)
└── mountpoint/            ← Thư mục mount (Giao diện logic cho người dùng)
```

Script `test_suite.sh` sẽ tự động kiểm tra:

- Basic read/write
- Partial overwrite (RMW)
- Multi-chunk file (>64 KB)
- Compression / incompressible detection
- Truncate / unlink / append
- Cross-boundary overwrite
- Persistence và metadata consistency

## Cách build

### Terminal 1 — Build và mount filesystem lên `mountpoint/`

```bash
make clean && make && make run
```
Filesystem sẽ chạy foreground và mount tại thư mục mountpoint/.

### Terminal 2 — Chạy regression test

```bash
make test
```

### Terminal 3 — Chạy benchmark + tuning

```bash
make bench
```
Kết quả benchmark sẽ được ghi vào `benchmark_results.txt`.

## Debug

Log có timestamp `[HH:MM:SS.mmm]` in ra stderr trên terminal chạy FUSE:
```
[13:05:01.234] [DEBUG] write: /test.txt offset=0 size=12
[13:05:01.235] [DEBUG] write: incompressible, storing raw
[13:05:01.236] [DEBUG] write OK: chunks=1 logical_size=12 codec=0
```