# Transparent Compression FUSE

Project môn Hệ điều hành - Adding Transparent Compression Support to the File System - Nhóm 25228

## Giới thiệu
Đây là prototype File System sử dụng FUSE 3 hỗ trợ transparent compression (nén/giải nén trong suốt).  
Ứng dụng vẫn gọi read/write bình thường; file system tự động nén/giải nén theo chunk 64 KB (Zstd), hỗ trợ ghi đè từng phần và lưu metadata bền vững sau remount.

## Giai đoạn hiện tại (Tuần 6 - Sprint 6)
Sprint 6: Heuristic skip compression + test corner cases (đã hoàn thành)

Các tính năng đã triển khai
- Thiết kế và định nghĩa cấu trúc `myfs_chunk_t`, `myfs_chunk_map_t`, `myfs_inode_t` (chunk 64 KB)
- Cơ chế lưu trữ: mỗi file logic tương ứng với `filename.data` (dữ liệu) và `filename.meta` (chunk map nhị phân)
- Read/Write theo chunk, nén Zstd one-shot; tự động fallback sang raw nếu dữ liệu không nén hiệu quả
- Fast-path incompressible detection bằng magic bytes (JPEG, PNG, ZIP, GIF, MP4/MOV, gzip, Zstd...) để bỏ qua compression không cần thiết
- Partial overwrite (RMW): gom các chunk bị chồng lấn, patch dữ liệu, ghi blob mới
- `load_chunk_map()` và `save_chunk_map()` với atomic write (file tạm + rename + fsync)
- `open(O_TRUNC)` và `truncate()` đồng bộ đầy đủ `chunk_map`, `logical_size` và metadata
- `myfs_read()` hỗ trợ linear scan chunk map thay vì giả định chunk cố định 64 KB, đọc đúng cả các chunk có kích thước thực khác nhau
- Guards kiểm tra metadata/offset/codec khi đọc để tránh lỗi dữ liệu
- `readdir` ẩn `.data` và `.meta`, chỉ hiển thị tên file logic
- Bổ sung `make test` và `test_suite.sh` để regression test toàn bộ filesystem

Kết quả kiểm tra
- Tạo/ghi/đọc file → tự động sinh `.data` và `.meta`, nội dung trả về đúng
- Partial overwrite → dữ liệu ghép đúng tại mọi vị trí, kể cả overwrite qua ranh giới chunk 64 KB
- `open(O_TRUNC)` và `truncate -s 0` → reset đúng chunk map và logical size
- File lớn >64 KB → multi-chunk read/write chính xác, verify bit-by-bit thành công
- File binary/random và định dạng incompressible → tự động lưu raw, dữ liệu đọc lại khớp hoàn toàn
- Compression hoạt động hiệu quả với dữ liệu text lặp lại, kích thước `.data` nhỏ hơn logical size
- Append nhiều lần liên tiếp → logical size và nội dung luôn nhất quán
- `rm` xóa đồng thời `.data` và `.meta`, `readdir` không lộ file nội bộ
- Persistence ổn định: đọc nhiều lần liên tiếp và remount không làm thay đổi metadata hoặc logical size
- Toàn bộ regression test trong `test_suite.sh` PASS

## Cấu trúc thư mục

```markdown
transcomp/
├── Makefile
├── README.md
├── test_suite.sh          ← regression test suite
├── src/
│   ├── main.c
│   ├── myfs.h
│   ├── helpers.c
│   ├── operations.c
│   └── guards/
│       ├── guards.h
│       └── guards.c       ← metadata validation guards
├── backing/               ← thư mục lưu file thật (.data + .meta)
├── mountpoint/            ← thư mục mount
└── myfs                   ← file thực thi
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

### Terminal 2 - Chạy regression test

```bash
make test
```

## Debug

Log có timestamp `[HH:MM:SS.mmm]` in ra stderr trên terminal chạy FUSE:
```
[13:05:01.234] [DEBUG] write: /test.txt offset=0 size=12
[13:05:01.235] [DEBUG] write: incompressible, storing raw
[13:05:01.236] [DEBUG] write OK: chunks=1 logical_size=12 codec=0
```

## Kế hoạch tiếp theo (Sprint 7)

- Đo benchmark hiệu năng đọc/ghi và tỉ lệ nén
- Tối ưu luồng compression và quản lý chunk
- Hoàn thiện cơ chế garbage collection cho blob/chunk mồ côi trong `.data`
- Kiểm thử hồi quy và kiểm thử độ ổn định toàn hệ thống
- Hoàn thiện báo cáo draft và tổng hợp kết quả đánh giá