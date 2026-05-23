# Transparent Compression FUSE

Project môn Hệ điều hành - Adding Transparent Compression Support to the File System - Nhóm 25228

## Giới thiệu
Đây là prototype File System sử dụng FUSE 3 hỗ trợ transparent compression (nén/giải nén trong suốt).  
Ứng dụng vẫn gọi read/write bình thường; file system tự động nén/giải nén theo chunk 64 KB (Zstd), hỗ trợ ghi đè từng phần và lưu metadata bền vững sau remount.

## Giai đoạn hiện tại (Tuần 7 - Sprint 7)
Sprint 7: Benchmark + tuning (đang hoàn thiện báo cáo và chốt số liệu)

Các nội dung đang tập trung
- Thêm `benchmark.sh` để đo throughput ghi/đọc, compression ratio, latency RMW và so sánh với ext4 baseline
- Ghi `benchmark_results.txt` để lưu kết quả benchmark theo từng lần chạy
- Kiểm tra heuristic skip compression bằng các file giả JPEG/ZIP để xác nhận dữ liệu incompressible đi raw path
- Đo persistence sau remount bằng checksum MD5 và script `verify_remount.sh`
- Phân tích overhead giữa FUSE và Zstd để phục vụ tuning và báo cáo

Kết quả benchmark chính
- Sequential write/read của file text và source-like data cho thấy myfs hoạt động đúng theo chunk 64 KB và compression theo workload
- File random/incompressible được lưu raw, compression ratio xấp xỉ 1.00x như mong đợi
- Partial overwrite vẫn giữ đúng nội dung sau RMW
- Remount verification giữ nguyên checksum, xác nhận metadata persistence ổn định
- Pattern append nhỏ hơn chunk cho thấy chi phí RMW tăng rõ rệt, phù hợp để đưa vào phần tuning báo cáo
- Benchmark cũng giúp xác nhận heuristic skip compression đang giảm overhead với payload không nén hiệu quả

## Cấu trúc thư mục

```markdown
transcomp/
├── Makefile
├── README.md
├── benchmark.sh           ← benchmark hiệu năng + tuning report
├── benchmark_results.txt
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


## Kế hoạch tiếp theo

- Hoàn thiện phần dọn dẹp blob/chunk mồ côi trong `.data`
- Tiếp tục tinh chỉnh hiệu năng cho append nhỏ và partial overwrite
- Tăng độ phủ regression test cho các trường hợp biên còn lại
- Hoàn thiện báo cáo tổng hợp benchmark + tuning và chốt số liệu cuối