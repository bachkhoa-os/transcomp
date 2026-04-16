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
│   └── operations.c
├── backing/          ← thư mục lưu file thật (.data + .meta)
├── mountpoint/       ← thư mục mount
└── myfs              ← file thực thi
```

## Cách build
```bash
make clean && make
```

## Cách chạy (2 terminal)

**Terminal 1** (chạy FUSE):
```bash
./myfs -f mountpoint ./backing
```

**Terminal 2** (test):
```bash
touch mountpoint/test.txt
echo "XIN CHAO THE GIOIIIII" > mountpoint/test.txt
cat mountpoint/test.txt
ls -la mountpoint
ls -la backing          # kiểm tra .data và .meta
fusermount -u mountpoint

# Remount và kiểm tra persistence
./myfs -f mountpoint ./backing
cat mountpoint/test.txt
ls -la mountpoint
```

## Debug
Mọi hàm đều in log [DEBUG] rõ ràng trên terminal chạy FUSE.  
Xem file .meta bằng lệnh:
```bash
hexdump -C backing/test.txt.meta
```

## Kế hoạch tiếp theo (Sprint 4)
- Compression engine (Zstd) + read path hoàn chỉnh
- Tích hợp giải nén tự động khi đọc dữ liệu từ chunk

## Lưu ý
- Hiện tại vẫn hoạt động trên dữ liệu raw (chưa nén)
- Backing directory phải tồn tại trước khi chạy
- Project dùng FUSE 3, chunk size 64 KB
