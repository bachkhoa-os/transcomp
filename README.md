# Transparent Compression FUSE

Project môn Hệ điều hành - Adding Transparent Compression Support to the File System - Nhóm 25228

## Giới thiệu
Đây là prototype File System sử dụng FUSE 3 hỗ trợ transparent compression (nén/giải nén trong suốt).  
Ứng dụng vẫn gọi read/write bình thường, file system tự động nén/giải nén dữ liệu theo chunk 64 KB và lưu metadata bền vững sau remount.

## Giai đoạn hiện tại (Tuần 3 - Sprint 3)
Hoàn thành: Chunk map + metadata persistence (remount OK)

Các tính năng đã triển khai:
- Thiết kế và định nghĩa cấu trúc myfs_chunk_t, myfs_chunk_map_t, myfs_inode_t
- Xây dựng cơ chế lưu trữ: mỗi file logic tương ứng với filename.data (dữ liệu) và filename.meta (chunk map dạng binary)
- Implement load_chunk_map() và save_chunk_map() với cơ chế atomic write (file tạm + rename + fsync)
- Cập nhật myfs_create, myfs_open, myfs_getattr, myfs_truncate và myfs_utimens
- readdir tự động ẩn .data và .meta, chỉ hiển thị tên file logic
- Logical size được quản lý và trả về đúng qua getattr

Kết quả kiểm tra:
- Tạo/ghi file → tự động sinh .data và .meta
- Unmount → remount → file vẫn tồn tại, nội dung và kích thước logic không thay đổi

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