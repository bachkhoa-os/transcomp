# Transparent Compression FUSE

Project môn Hệ điều hành - Adding Transparent Compression Support to the File System - Nhóm 25228

## Giới thiệu
Đây là prototype File System sử dụng FUSE hỗ trợ transparent compression (nén/giải nén trong suốt).  
Ứng dụng vẫn đọc/ghi file bình thường, file system tự động nén dữ liệu theo chunk trước khi lưu xuống backing store và tự động giải nén khi đọc.

Giai đoạn hiện tại (Tuần 2):  
- Skeleton FUSE hoàn chỉnh  
- Backing store là thư mục thật  
- Hỗ trợ create, read, write, readdir, mkdir, rmdir (dữ liệu raw - chưa nén)

## Cấu trúc thư mục
transcomp/
├── Makefile
├── README.md
├── src/
│   ├── main.c
│   ├── myfs.h
│   ├── helpers.c
│   └── operations.c
├── backing/          ← thư mục lưu file thật (tự động tạo)
├── mountpoint/       ← thư mục mount (tự động tạo khi chạy)
└── myfs              ← file thực thi (sau khi make)

## Cách build
```bash
make
```

## Cách chạy
Terminal 1 - chạy FUSE (foreground):
```bash
./myfs -f mountpoint ./backing
```

Terminal 2 - test:
```bash
touch mountpoint/test.txt
echo "XIN CHAO THE GIOIIIII" > mountpoint/test.txt
cat mountpoint/test.txt
ls -la mountpoint
ls -la backing
```

Để unmount:
```bash
fusermount -u mountpoint
```

## Debug
Mọi hàm đều in [DEBUG] ra terminal đang chạy FUSE.  
Nếu gặp lỗi, xem ngay terminal chạy ./myfs để thấy thông báo [ERROR].

## Kế hoạch tiếp theo (Sprint 3)
Triển khai chunk map + metadata persistence (remount OK)

## Lưu ý
- Hiện tại chỉ hỗ trợ dữ liệu raw (chưa nén).
- Backing directory phải tồn tại trước khi chạy.
- Project dùng FUSE 3.
