# Transparent Compression FUSE

Project môn Hệ điều hành - Adding Transparent Compression Support to the File System - Nhóm 25228

## Giới thiệu
Đây là prototype File System sử dụng FUSE 3 hỗ trợ transparent compression (nén/giải nén trong suốt).  
Ứng dụng vẫn gọi read/write bình thường; file system tự động nén/giải nén theo chunk 64 KB (Zstd), hỗ trợ ghi đè từng phần và lưu metadata bền vững sau remount.

## Giai đoạn hiện tại (Tuần 5 - Sprint 5)
Sprint 5: Write path + partial overwrite + fsync/checkpoint (đã hoàn thành)

Các tính năng đã triển khai:
- Thiết kế và định nghĩa cấu trúc `myfs_chunk_t`, `myfs_chunk_map_t`, `myfs_inode_t` (chunk 64 KB)
- Cơ chế lưu trữ: mỗi file logic tương ứng với `filename.data` (dữ liệu) và `filename.meta` (chunk map nhị phân)
- Read/Write theo chunk, nén Zstd one-shot; bỏ nén nếu không hiệu quả
- Partial overwrite (RMW): gom các chunk bị chồng lấn, patch dữ liệu, ghi blob mới
- `load_chunk_map()`/`save_chunk_map()` với atomic write (file tạm + rename + fsync)
- `open(O_TRUNC)` và `truncate` đồng bộ lại chunk map và logical size
- Guards kiểm tra metadata/offset/codec khi đọc để tránh lỗi dữ liệu
- `readdir` ẩn `.data` và `.meta`, chỉ hiển thị tên file logic

Kết quả kiểm tra:
- Tạo/ghi/đọc file → tự động sinh `.data` và `.meta`, nội dung trả về đúng
- Partial overwrite → dữ liệu ghép đúng sau ghi đè giữa file
- `rm` xoá đồng thời `.data` và `.meta`, `readdir` không lộ file nội bộ
- Unmount → remount → file vẫn tồn tại, metadata và kích thước logic không thay đổi

## Cấu trúc thư mục
```markdown
transcomp/
├── Makefile
├── README.md
├── src/
│   ├── main.c
│   ├── myfs.h
│   ├── helpers.c
│   ├── operations.c
│   └── guards/
│       ├── guards.h
│       └── guards.c    ← metadata validation guards
├── backing/          ← thư mục lưu file thật (.data + .meta)
├── mountpoint/       ← thư mục mount
└── myfs              ← file thực thi
```

## Cách build
```bash
# 1. Write và read basic
echo "HELLO WORLD" > mountpoint/test.txt
cat mountpoint/test.txt                             # HELLO WORLD

# 2. Partial overwrite (RMW)
printf 'FUSE!' | dd of=mountpoint/test.txt bs=1 seek=6 conv=notrunc
cat mountpoint/test.txt                             # HELLO FUSE!

# 3. Incompressible data (random binary)
dd if=/dev/urandom bs=1K count=1 > mountpoint/rand.bin 2>/dev/null
# log: write: incompressible, storing raw

# 4. Xem metadata binary
hexdump -C backing/test.txt.meta

# 5. Xóa file
rm mountpoint/test.txt
ls mountpoint/                                      # trống
ls backing/                                         # chỉ còn . và ..

# 6. Persistence (unmount → remount)
make umount && make run
cat mountpoint/test.txt                             # vẫn đúng nội dung
```

## Debug

Log có timestamp `[HH:MM:SS.mmm]` in ra stderr trên terminal chạy FUSE:
```
[13:05:01.234] [DEBUG] write: /test.txt offset=0 size=12
[13:05:01.235] [DEBUG] write: incompressible, storing raw
[13:05:01.236] [DEBUG] write OK: chunks=1 logical_size=12 codec=0
```

## Kế hoạch tiếp theo (Sprint 6)

- Heuristic skip compression nâng cao (detect magic bytes)
- Corner case testing toàn diện (file rỗng, truncate nhiều lần, append lớn)
- Garbage collection: thu hồi orphan blob trong `.data`