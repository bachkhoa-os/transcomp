# myfs — Transparent Compression Filesystem

myfs là filesystem chạy trong không gian người dùng qua FUSE trên Linux, lưu dữ liệu bằng Zstd hoặc dạng không nén trong một thư mục nền. Hướng phát triển là một thư mục nén có thể đọc và ghi trực tiếp cho dữ liệu thô: ứng dụng dùng đường dẫn thông thường, giữ nguyên mã ứng dụng và định dạng dữ liệu. Hiện dự án có các thao tác tệp cơ bản, cơ chế quản lý phiên bản lưu trữ và bộ công cụ đo M0; phạm vi tương thích và độ bền dữ liệu còn cần được xác lập.

**Status:** nguyên mẫu nghiên cứu; chưa sẵn sàng cho môi trường sản xuất (*research prototype; not production-ready*). M0 đã được chấp thuận trong phạm vi thí nghiệm được ghi nhận; M1 và các mốc sau chưa bắt đầu.

## Table of Contents

Tài liệu đi từ cách sử dụng đến định dạng lưu trữ, kiểm thử và bằng chứng đo đạc. Các liên kết dưới đây dẫn đến từng phần để người đọc chọn mức chi tiết cần thiết.

1. [Audience guide](#audience-guide)
2. [What myfs is and is not](#what-myfs-is-and-is-not)
3. [Project status and roadmap](#project-status-and-roadmap)
4. [Quick start](#quick-start)
5. [Compatibility and limitations](#compatibility-and-limitations)
6. [Architecture overview](#architecture-overview)
7. [Storage format](#storage-format)
8. [Read and write paths](#read-and-write-paths)
9. [Concurrency](#concurrency)
10. [Performance mechanisms](#performance-mechanisms)
11. [Testing](#testing)
12. [Benchmarks](#benchmarks)
13. [M0 profiling tooling](#m0-profiling-tooling)
14. [Repository layout](#repository-layout)
15. [Debugging](#debugging)
16. [Design decisions](#design-decisions)
17. [Glossary](#glossary)
18. [License](#license)

## Audience guide

Phần này chỉ ra lộ trình đọc cho từng mục đích. Người thử sử dụng có thể bắt đầu bằng các lệnh ngắn, còn người đánh giá cần xem cả giới hạn và nguồn bằng chứng.

| Bạn muốn… | Đọc phần… |
|---|---|
| Dùng thử việc đọc và ghi tệp | [Quick start](#quick-start), rồi [Compatibility and limitations](#compatibility-and-limitations) |
| Đóng góp mã nguồn | [Architecture overview](#architecture-overview), [Storage format](#storage-format), [Concurrency](#concurrency), [Testing](#testing) |
| Hiểu hoặc tái lập số đo | [Benchmarks](#benchmarks), [M0 profiling tooling](#m0-profiling-tooling) |
| Review phạm vi và trạng thái dự án | [Project status and roadmap](#project-status-and-roadmap), [Compatibility and limitations](#compatibility-and-limitations), [Design decisions](#design-decisions) |

## What myfs is and is not

Mục tiêu sản phẩm là một thư mục nén cho dữ liệu thô trên Linux mà ứng dụng có thể đọc và ghi qua đường dẫn thông thường. Phần này phân biệt mục tiêu đó với những khả năng đã có, để tránh coi một lần chạy ứng dụng thành công là bằng chứng tương thích đầy đủ.

Theo [tài liệu M0, §1](benchmarks/m0_evidence_finalization.md#1-purpose-authority-and-decision), ứng dụng giữ nguyên mã và định dạng dữ liệu; ứng dụng vẫn cần trỏ đến đường dẫn trong mount. myfs hiện **chưa phải filesystem POSIX đa dụng**.

| Khía cạnh | myfs hiện tại |
|---|---|
| Giao diện | Các callback FUSE đã đăng ký phục vụ đọc, ghi và quản lý tệp/thư mục cơ bản |
| Cách dùng dữ liệu | `cat`, ghi nối đuôi và ghi đè một phần qua mount; không có bước giải nén thủ công trước mỗi lần đọc |
| Lưu trữ | Dữ liệu nền ở dạng các khối nén hoặc không nén, kèm bản đồ metadata |
| So với tệp lưu trữ zip/RAR | Mount cung cấp nội dung logic qua đường dẫn; không phải công cụ tạo/giải nén một archive |
| Phạm vi đã đo trong M0 | Một quy trình lọc CSV tổng hợp, tuần tự, tạo riêng tệp đầu ra |
| Chưa có bảo đảm | POSIX đầy đủ, mọi ứng dụng Linux, độ bền sau mất điện, khả năng quan sát thay đổi giữa nhiều tiến trình, hiệu năng hoặc tỷ lệ nén cho dữ liệu bất kỳ |

## Project status and roadmap

Phần này ghi trạng thái được chấp thuận và các mốc tiếp theo. Số đo M0 được giữ cùng giới hạn của thí nghiệm để người đọc biết chúng trả lời được câu hỏi nào.

Công việc quản lý phiên bản lưu trữ/GC và M0 đã được hợp nhất vào `main`. Ngày **2026-10-17**, chủ dự án chấp thuận M0 theo [§11 của tài liệu M0](benchmarks/m0_evidence_finalization.md#11-approved-criterion-by-criterion-acceptance): baseline tái lập được, bằng chứng gốc, đầu ra đúng trong quy trình đã thử, danh sách khoảng trống tương thích và kết luận riêng về điểm nghẽn.

Bảng mốc sau lấy phạm vi từ §4 của roadmap đầy đủ được giữ **cục bộ, ngoài Git**. Trạng thái M0 “đang thực hiện” trong bản roadmap cập nhật ngày 2026-10-11 đã được quyết định chấp thuận mới hơn thay thế; M1 và các mốc sau chưa bắt đầu.

| Mốc | Trọng tâm và điều kiện hoàn tất | Trạng thái |
|---|---|---|
| M0 | Rà soát có mục tiêu, profiling toàn filesystem; baseline, bằng chứng gốc, đầu ra đúng, tách khoảng trống tương thích khỏi kết luận về điểm nghẽn | Đã chấp thuận trong phạm vi thí nghiệm |
| M1 | Hợp đồng về tính đúng và khôi phục: khả năng thấy dữ liệu sau đọc/ghi, sync, báo lỗi, phục hồi và các kiểm thử tương ứng | Chưa bắt đầu; kiểm kê mã/kiểm thử hiện có trước |
| M2 | Tương thích quy trình ứng dụng: thay tên, handle, thuộc tính và thư mục trong phạm vi đã chọn | Chưa bắt đầu |
| M3 | Giới hạn vận hành: hết dung lượng, RAM/FD, công việc nền; tối ưu dựa trên điểm nghẽn đo được | Chưa bắt đầu |
| M4 | Công cụ sử dụng: vòng đời mount, thống kê, chuyển đổi, kiểm tra ngoại tuyến, xuất và phục hồi | Chưa bắt đầu |
| M5 | Thử nghiệm với người dùng: đóng gói tối thiểu, điều kiện phát hành và tài liệu | Chưa bắt đầu |

### M0 headline results

Nguồn: [§2–3](benchmarks/m0_evidence_finalization.md#3-d5c-repeated-uninstrumented-baseline), [§5.1](benchmarks/m0_evidence_finalization.md#51-storage-and-memory) và [§7](benchmarks/m0_evidence_finalization.md#7-measured-cost-and-bottleneck-disposition). Đây là số đo lịch sử, không phải lần chạy benchmark mới khi sửa README.

| Thành phần | Phạm vi hoặc số đo |
|---|---|
| Dữ liệu | CSV tổng hợp có thể nén; seed `20261010`; `1,048,576` bản ghi dữ liệu, mỗi bản ghi 128 byte ASCII |
| Đầu vào/đầu ra | `134,217,797` byte đầu vào; `39,079,244` byte và `346,323` dòng dữ liệu đầu ra |
| Phép biến đổi | Chọn `status == "ok"` và `quantity >= 50`, tính `quantity * price_cents`, giữ thứ tự dòng |
| Số lần thử D5c | 18 lần: 2 warmup không tính vào thống kê, 8 lần đo/backend; 4 cặp khác backend, 2 cặp A/A và 2 cặp B/B |
| Median wall time ứng dụng | ext4: `6.920591507` s; myfs: `7.820180560` s |
| Chênh lệch myfs − ext4 của 4 cặp | `+0.612717777`, `+0.663579314`, `+0.913237761`, `+0.828124653` s |
| Tỷ số myfs/ext4 của 4 cặp | `1.090025475`, `1.093852092`, `1.125728026`, `1.119975005` |
| Tính đúng trong chiến dịch | Cả 18 lần qua kiểm tra; đối chiếu độc lập nội dung và xác minh sau remount có trật tự |

Cả bốn chênh lệch đều dương và lớn hơn trị tuyệt đối của chênh lệch trong bốn cặp control cùng backend. Tuy nhiên, một máy, một quy trình tổng hợp và bốn cặp so sánh không xác lập độ chắc chắn thống kê, hồi quy sản phẩm hay nguyên nhân. Tần số/tải host, hoạt động metadata bên ngoài và trạng thái cư trú cache chưa biết vẫn gây nhiễu; các giai đoạn chuẩn bị, xác minh và tắt daemon nằm ngoài wall time ứng dụng.

**M0 chưa xác định được điểm nghẽn native của filesystem có quan hệ nhân quả đã được chứng minh.** Điều này không có nghĩa là không tồn tại điểm nghẽn. Tổng CPU daemon, profile Python bị tác động bởi instrumentation, số lần chuyển ngữ cảnh và khoảng thời gian của các lời gọi sync/I/O là những tập đo khác nhau; không cộng hoặc trừ chúng để suy ra thời gian ứng dụng chờ hay nguyên nhân chênh lệch ext4/myfs.

### Storage allocation in M0

**Các số sau đến từ fixture tổng hợp có thể nén**, không phải dữ liệu khách hàng hay cam kết tỷ lệ nén. Việc lọc bớt dòng và cột của ứng dụng cũng không phải nén filesystem.

| Phép đo | Byte | Cách hiểu |
|---|---:|---|
| D5c median dung lượng cấp phát ext4 | 173,314,048 | Sau tiến trình kết thúc |
| D5c median dung lượng cấp phát myfs | 37,384,192 | Sau tiến trình kết thúc |
| D8d tổng cấp phát theo inode duy nhất | 37,384,192 | Tổng tại thời điểm kiểm tra |
| D8d kích thước biểu kiến của dữ liệu nền | 37,347,532 | Đại lượng khác; không cộng vào tổng cấp phát |
| D8d cấp phát thư mục | 8,192 | Đã nằm trong tổng |
| D8d cấp phát metadata đã nhận diện | 81,920 | Đã nằm trong tổng |
| D8d cấp phát payload | 37,294,080 | Đã nằm trong tổng |
| D8d cấp phát trong các thư mục phiên bản lưu trữ | 28,389,376 | Tập con đã nằm trong tổng; không đồng nghĩa toàn bộ là rác |

Cách tính cộng `st_blocks * 512`, khử trùng lặp theo device/inode, loại việc đi theo đích symlink; hai hard link đã được khử trùng lặp. Đây không phải mức sử dụng đĩa đỉnh. RSS từ `wait4` là đỉnh theo vòng đời từng tiến trình, không phải tổng RAM toàn chiến dịch; xem giới hạn tài nguyên và endpoint thiếu trong §5.1.

## Quick start

Phần này hướng dẫn build, mount và thử đọc/ghi từ thư mục gốc repository. Dùng dữ liệu thử trong các thư mục riêng vì bộ kiểm thử tạo, thay đổi và dọn các tệp của nó.

### Prerequisites

| Thành phần | Cần cho |
|---|---|
| Linux có thiết bị `/dev/fuse` và quyền mount FUSE | Chạy filesystem và bộ mounted test |
| GCC, GNU Make, `pkg-config`, pthread/glibc | Build; khởi tạo cache rwlock yêu cầu loại ưu tiên writer của glibc |
| Header/thư viện FUSE 3, Zstd, zlib | `fuse3`, `-lzstd`, `-lz` trong [Makefile](Makefile) |
| `fusermount3`, Bash, Python 3, tiện ích GNU và `mountpoint` | Unmount, kiểm thử shell/Python |
| `hexdump` | Tùy chọn để xem byte metadata khi debug |

Ví dụ cài gói Debian/Ubuntu — **not executed here**: các phụ thuộc đã có sẵn, không thay đổi gói hệ thống trong lần sửa tài liệu này.

```bash
sudo apt install libfuse3-dev libzstd-dev zlib1g-dev pkg-config gcc make fuse3 python3
```

### Build and mount

Các lệnh sau đã được chạy thành công khi kiểm tra README. `make` dùng cờ debug `-g`; `make release` biên dịch lại với `-O2 -DNDEBUG`; `make run` dùng binary hiện có và chạy ở foreground.

```bash
make
make release
mkdir -p backing mountpoint
make run
```

Giữ terminal đó chạy; dùng terminal khác tại cùng thư mục repository:

```bash
echo "Hello World" > mountpoint/readme-example.txt
cat mountpoint/readme-example.txt
ls -la mountpoint/
make test
```

Bộ mounted test trả về **84 PASS / 0 FAIL / 84 total** trong lần kiểm tra README. Sau khi thử xong:

```bash
make umount
make test-unit
python3 -B -m unittest discover -s tests -p 'test_m0_*.py'
```

`make test-unit` không cần mount và đã đạt; bộ Python M0 đạt **264 tests / OK**. Các con số là kết quả của checkout được kiểm tra, không phải số lượng được bảo đảm cho các phiên bản sau.

`make clean` còn xóa `backing/*`, không chỉ binary; chỉ dùng khi đã unmount và chấp nhận bỏ dữ liệu thử. Target này **not executed here**, để giữ dữ liệu kiểm chứng; xem [Makefile](Makefile).

## Compatibility and limitations

Phần này ghi rõ thao tác nào được đăng ký và hợp đồng nào còn chưa được kiểm chứng. Một callback có mặt không đồng nghĩa với toàn bộ ngữ nghĩa POSIX của thao tác đó đã được xác nhận.

### FUSE operation coverage

Bảng được trích từ `myfs_oper` trong [src/main.c](src/main.c), đối chiếu tên callback với header FUSE 3 trên máy kiểm tra. “Supported” ở đây có nghĩa có callback được đăng ký; “Not registered” không tự quyết định errno hay cách ứng dụng/kernel/libfuse xử lý.

| Operation | Registration | Callback / ghi chú |
|---|---|---|
| `getattr` | Supported | `myfs_getattr` |
| `readdir` | Supported | `myfs_readdir` |
| `mknod` | Supported | `myfs_mknod`; xem phạm vi tệp trong implementation |
| `create` | Supported | `myfs_create` |
| `open` | Supported | `myfs_open` |
| `read` | Supported | `myfs_read` |
| `write` | Supported | `myfs_write` |
| `release` | Supported | `myfs_release`; giới hạn báo lỗi C02 bên dưới |
| `mkdir` | Supported | `myfs_mkdir` |
| `rmdir` | Supported | `myfs_rmdir` |
| `unlink` | Supported | `myfs_unlink` |
| `init` | Supported | `myfs_init`; bật `kernel_cache` |
| `destroy` | Supported | `myfs_destroy`; chờ worker xử lý hết hàng đợi |
| `truncate` | Supported | `myfs_truncate` |
| `utimens` | Supported | `myfs_utimens`; no-op trả về 0 |
| `fsync`, `flush`, `fsyncdir` | Not registered | Chưa có callback |
| `rename`, `statfs`, `chmod`, `chown` | Not registered | Chưa có callback |
| `readlink`, `symlink`, `link` | Not registered | Chưa có callback |
| `setxattr`, `getxattr`, `listxattr`, `removexattr` | Not registered | Chưa có callback |
| `access`, `opendir`, `releasedir` | Not registered | Chưa có callback riêng |
| `lock`, `flock`, `bmap`, `ioctl`, `poll` | Not registered | Chưa có callback |
| `read_buf`, `write_buf`, `fallocate`, `copy_file_range`, `lseek` | Not registered | Chưa có callback |

### M0 compatibility register

Phân loại dưới đây giữ nguyên nhãn của [M0 §6](benchmarks/m0_evidence_finalization.md#6-compatibility-and-contract-gap-register). Đây là hành vi xác nhận từ nguồn hoặc giới hạn kiểm chứng, **không phải danh sách lỗi quan sát được trong chiến dịch**; riêng C08 được tài liệu nguồn gọi là `OBSERVED TEST GAP`.

| ID | Phân loại của nguồn | Điều đã biết và giới hạn |
|---|---|---|
| C01 | CONFIRMED SOURCE BEHAVIOR; UNTESTED CONTRACT | Không đăng ký `fsync`/`flush`; ứng dụng CSV không gọi chúng tường minh. M0 chưa đo kết quả sync mà ứng dụng nhìn thấy; không suy ra errno chỉ từ bảng callback. |
| C02 | CONFIRMED SOURCE BEHAVIOR | `release()` gọi `fsync(handle->data_fd)` nhưng không kiểm tra kết quả, rồi tiếp tục trả 0 trên đường này; FUSE bỏ qua giá trị trả về của `release`. M0 không quan sát lỗi sync hay mất dữ liệu tại đây. |
| C03 | UNTESTED CONTRACT for campaign-level crash durability | M0 xác minh tắt/mount lại bình thường. Mã có kiểm tra sync/publication, xử lý phần metadata ghi dở và phục hồi; hợp đồng sau crash/mất điện chưa được chiến dịch xác lập. |
| C04 | UNTESTED CONTRACT | Có cache handle, số phiên bản metadata và kiểm thử gọi trực tiếp reader/writer. M0 không kiểm chứng khả năng thấy thay đổi giữa nhiều tiến trình qua mount và kernel cache. |
| C05 | CONFIRMED SOURCE BEHAVIOR; UNKNOWN application compatibility | Thiếu callback `rename`, `statfs`, `chmod`/`chown`, `symlink`/`link`, xattr. Quy trình ghi tệp tạm rồi thay tên và công cụ phụ thuộc thuộc tính cần kiểm chứng riêng. |
| C06 | CONFIRMED SOURCE BEHAVIOR | `utimens` bỏ qua yêu cầu đổi timestamp và trả 0; xem [dir.c](src/fuse_ops/dir.c). Đây là kết quả đọc mã, không phải lỗi được phát hiện từ M0. |
| C07 | UNTESTED CONTRACT | Đường ghi có kiểm tra `fdatasync`, lỗi metadata và một số kiểm thử lỗi/retry. Hợp đồng đầy đủ cho `ENOSPC`, `EIO`, `ENOMEM`, giới hạn FD và phục hồi chưa được M0 xác lập. |
| C08 | OBSERVED TEST GAP in these campaigns | Chỉ thử một ứng dụng lọc CSV tuần tự, tạo độc quyền đầu ra. Database, `mmap`, append đồng thời, thay thế tệp, import/export và dữ liệu khách hàng chưa được chiến dịch bao phủ. |

### Implementation limits

**Chunk** là một đoạn dữ liệu có một mục metadata riêng và được lưu thành một blob nén hoặc không nén.

- Chunk legacy rất lớn vẫn có thể phải giải nén nguyên khối vào RAM khi đọc hoặc chuyển đổi; cả frame dài hơn phần dữ liệu còn hợp lệ cũng được giải nén đầy đủ rồi lấy phần đầu. Chưa có đường giải nén streaming cho trường hợp này ([compress.c](src/core/compress.c), [chunkio.c](src/core/chunkio.c)).
- Compaction hiện có một worker và hàng đợi FIFO. Chưa có bằng chứng ở đây để kết luận một worker đủ cho mọi tải ([compact.c](src/core/compact.c)).
- Cơ chế sync nội bộ và các kiểm thử lỗi hiện có không thay thế hợp đồng độ bền/hiển thị dữ liệu còn thiếu ở C01–C07.

## Architecture overview

Phần này nối đường dẫn ứng dụng với những tệp thật trong thư mục nền. Hiểu sự phân tách này giúp đọc metadata và thu hồi dữ liệu cũ mà không nhầm tên tương thích với dữ liệu đang hoạt động.

**Generation** là một phiên bản lưu trữ gồm cặp dữ liệu và metadata mà một handle có thể giữ tham chiếu tới. **Alias** là tên bổ sung trỏ đến cùng tệp lưu trữ để giữ tương thích với cách đặt tên trước đây.

```text
Application: open / read / write
                 |
             Kernel VFS
                 |
          FUSE kernel module
                 |
       src/main.c: FUSE callbacks
                 |
   src/fuse_ops/file.c + dir.c
                 |
   src/core/ + src/guards/
   | compress.c       Zstd
   | chunkio.c        doc / ghi / repack
   | metadata.c       ban do luu tru
   | compact.c        generation / GC
   | lock.c, path.c   khoa / duong dan
                 |
       backing directory tren Linux
```

| Lớp | Trách nhiệm |
|---|---|
| `main.c` | Tham số mount, cấu hình, callback và vòng đời worker |
| `fuse_ops/` | Thao tác tệp/thư mục, handle, đọc/ghi và cache metadata |
| `core/` | Nén, I/O chunk, metadata, khóa, đường dẫn, compaction và thu hồi |
| `guards/` | Kiểm tra metadata, vùng dữ liệu và kết quả I/O |

### Backing-store layout

Ví dụ dưới đây mô tả tên trên đĩa, không phải lệnh chạy:

```text
backing/
  file.txt.data              # cap legacy hoac hard-link alias
  file.txt.meta              # cap legacy hoac hard-link alias
  file.txt.current           # symlink -> file.txt.g.<hex>
  file.txt.g.<hex>.owner     # dau nhan quyen so huu generation
  file.txt.g.<hex>/
    data
    meta
```

Tệp mới bắt đầu bằng cặp `.data`/`.meta`. Sau compaction đầu tiên, `.current` chọn generation đang hoạt động; publication dùng đổi tên symlink nguyên tử rồi sync thư mục cha. `.data`/`.meta` là hard-link alias tương thích và được cập nhật khi không còn handle legacy giữ cặp cũ; chúng không quyết định generation hoạt động.

Trong compaction bình thường, generation cũ chỉ đủ điều kiện thu hồi sau khi publication được xác nhận bền vững và không còn tham chiếu giữ chính generation đó. Phục hồi cũng có thể đánh dấu generation không hoạt động thuộc sở hữu myfs để thu hồi. Xem [path.c](src/core/path.c) và [compact.c](src/core/compact.c); thứ tự này là cơ chế trong mã, không phải hợp đồng crash durability đã được chứng minh.

## Storage format

Phần này mô tả đơn vị bố trí dữ liệu và các phiên bản metadata được đọc. Nó giải thích vì sao tệp cũ vẫn đọc được và vì sao việc đổi kích thước bố trí cần tạo generation mới.

**Window** là một khoảng địa chỉ logic cố định dùng để gom dữ liệu trước khi ghi trong một generation. **RMW** (*read-modify-write*) là việc đọc dữ liệu cũ, sửa phần được yêu cầu rồi ghi lại phần dữ liệu liên quan. **Journal** là phần nhật ký nối thêm các thay đổi metadata sau bản đồ gốc. **Checkpoint** là việc ghi lại toàn bộ bản đồ metadata hiện hành để bắt đầu một chu kỳ nhật ký mới.

### Windows and chunks

| Thuộc tính | Giá trị hoặc hành vi |
|---|---|
| Window của tệp mới | 64 KiB |
| Miền hợp lệ | Lũy thừa của hai từ 16 KiB đến 1 MiB |
| Khi thích nghi | Tăng hoặc giảm một bước ×2, trong compaction |
| Bố trí packed | Tối đa một chunk/window; chunk bắt đầu ở đầu window |
| Khoảng trống | Window không có dữ liệu không tạo chunk; đọc hole trả byte 0 tới EOF logic |
| Blob | Nén riêng bằng Zstd hoặc lưu raw; nối vào cuối tệp `data` |
| Bản đồ | `logical_offset`, `physical_offset`, `raw_size`, `stored_size`, `codec_type`, `flags`, `checksum` |

Tên trường dễ gây nhầm: **`raw_size` là số byte blob trên đĩa**, còn **`stored_size` là số byte nội dung logic hợp lệ sau giải nén**. `codec_type` bằng 0 cho raw và 1 cho Zstd; CRC32 kiểm tra blob trên đĩa ([myfs.h](src/myfs.h), [chunkio.c](src/core/chunkio.c)).

### Metadata versions and journal

| Phiên bản | Đọc và ghi |
|---|---|
| v0 | Metadata legacy không tag; suy ra window 64 KiB; chuyển sang định dạng versioned trước lần append journal đầu tiên |
| v1 | Header versioned, suy ra window 64 KiB |
| v2 | Lưu `window_size`; writer hiện tại ghi v2 |

v1/v2 dùng header little-endian 64 byte và mục chunk trên đĩa 32 byte. Journal chứa sequence, vùng thay thế trong map, logical size mới, các mục thêm, CRC và commit marker; loader replay các delta đã commit. Phần đuôi thiếu dữ liệu/marker được bỏ qua, còn record đầy đủ nhưng sai CRC hoặc sequence bị từ chối ([metadata.c](src/core/metadata.c)).

| Điều kiện checkpoint ở build thông thường | Ngưỡng |
|---|---|
| Số delta đã commit | Từ 1024 trở lên |
| Byte journal | Lớn hơn `max(1 MiB, 2 × (64 + 32 × num_chunks))` |
| Build có `MYFS_TEST_FAILPOINTS` | Ngưỡng số delta là 8 để kiểm thử |

Checkpoint ghi tệp tạm, sync, đổi tên và sync thư mục cha; handle phải mở lại metadata khi cần. Có đường tolerant đọc chunk layout cũ bằng quét tuyến tính; compaction repack toàn nội dung sang một generation v2 với một window size duy nhất. Các handle có thể đồng thời giữ những generation khác window size mà không trộn descriptor/map của chúng.

## Read and write paths

Phần này mô tả đường đi của dữ liệu trong các thao tác thường gặp. Nó cũng chỉ ra chỗ cache được làm mới và chỗ dữ liệu được đồng bộ trước khi metadata tham chiếu tới nó.

**Epoch** là bộ đếm trong bộ nhớ dùng để nhận biết metadata của generation đã thay đổi so với cache trong handle.

### Read

1. Handle giữ descriptor dữ liệu/metadata, thông tin generation và chunk map; so epoch khi đọc. Nếu cache còn hợp lệ và epoch không đổi, không reload `.meta` hoặc gọi syscall metadata trên backing store.
2. Nếu cần làm mới, đi qua path lock và cache write lock; đường đọc cache hợp lệ giữ cache read lock suốt lookup, `pread`, giải nén và sao chép.
3. Với map packed, tìm chunk bằng tìm kiếm nhị phân theo window base. Map legacy/unpacked dùng quét tuyến tính; khi ở hole, mã còn quét để tìm chunk tiếp theo.
4. Đọc blob, kiểm tra CRC32, giải nén hoặc sao chép raw, rồi trả đúng vùng được yêu cầu. Không có bảo đảm random access `O(1)`.

Nguồn: [file.c](src/fuse_ops/file.c), [chunkio.c](src/core/chunkio.c), [guards.c](src/guards/guards.c).

### Write and truncate

1. Giữ path lock rồi cache write lock; làm mới cache và từ chối mutation trên handle đã bị đánh dấu stale bằng `-ESTALE`.
2. Append, overwrite và ghi vào hole cùng xác định các window bị chạm rồi repack. Ghi một phần giữ dữ liệu còn lại bằng RMW; append nhỏ có thể gộp vào chunk cuối.
3. Heuristic magic bytes có thể bỏ qua nén. Nếu thử Zstd, mã chỉ nhận kết quả khi `compressed_size < src_size - src_size / 8`; đây là ngưỡng khoảng 12.5% có chia nguyên và so sánh nghiêm ngặt, không phải điều kiện `>= 12.5%` chính xác cho mọi kích thước.
4. Append blob, kiểm tra `fdatasync(data)`, append delta có CRC/commit marker rồi kiểm tra `fdatasync(meta)`.
5. Cập nhật cache/epoch sau commit, kiểm tra checkpoint và thống kê thích nghi. Yêu cầu compaction được gửi sau khi nhả khóa; lỗi scheduling được log và khôi phục thống kê, không đảo kết quả write đã thành công.

`truncate` xử lý map và phần chunk còn lại; đường chunk-I/O chung hỗ trợ cắt giữa chunk nén. Các lỗi sync của đường ghi được kiểm tra; riêng sync trong `release()` và hợp đồng báo lỗi toàn tuyến có giới hạn ở C02/C07. Xem [file.c](src/fuse_ops/file.c), [compress.c](src/core/compress.c).

## Concurrency

Phần này nêu thứ tự khóa và cách phối hợp giữa handle, thu hồi và compaction. Người thay đổi mã cần giữ các quy tắc này để không đưa vào thứ tự chờ khóa ngược chiều.

**Claim** là bước giữ quyền xử lý một công việc bằng định danh để lần hoàn tất kiểm tra đúng công việc đã nhận. **Handoff** là việc chuyển một handle đang mở sang bộ descriptor, metadata và tham chiếu generation mới trong cùng một lần chuyển được bảo vệ bằng khóa.

### Lock hierarchy

| Trường hợp | Thứ tự |
|---|---|
| Mutation | path lock → `cache_lock` → registry shard |
| Read dùng cache hợp lệ | `cache_lock` → registry shard; không lấy path lock |
| Chỉ thao tác registry | Có thể lấy shard riêng; không chờ path/cache lock khi đang giữ shard |
| Hàng đợi compaction | Nhả `compact_queue_mu` trước khi worker hoặc fallback chờ path lock |
| Nhiều path trong thao tác tương lai | Loại path trùng; lấy theo `(shard index, strcmp(path))` tăng dần, nhả ngược thứ tự |

Cache rwlock dùng `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`; lỗi khởi tạo khiến việc gắn handle thất bại. Cấu hình ưu tiên writer và các kiểm thử handoff không tạo bảo đảm thời gian chờ cho mọi tải ([lock.c](src/core/lock.c), [file.c](src/fuse_ops/file.c)).

### Path locks and generation registry

- Bảng path lock có **64 shard × 64 bucket**, hash FNV-1a 64-bit rồi fmix64. Mỗi shard có mutex; refcount giữ entry cho owner, waiter và unlocker. Khi refcount về 0, unlink entry dưới shard mutex rồi destroy/free bên ngoài; luôn nhả shard trước khi chờ path mutex.
- Generation registry là bảng **64 shard × 64 bucket riêng**, cũng hash FNV-1a + fmix64 chỉ theo `logical_path`. Generation cũ và mới của cùng path vì vậy thuộc cùng miền khóa phục vụ handoff.
- GC dùng trạng thái `NONE → PENDING → CLAIMED`: chụp identity, refcount, ý định cập nhật alias và claim ID dưới shard; resolve/alias/remove chạy ngoài shard, dưới path lock trong đường thông thường; finalize lấy lại shard và kiểm tra record/state/claim ID trước khi retire hoặc đưa về `PENDING` để thử lại.

Nguồn: [lock.c](src/core/lock.c), [compact.c](src/core/compact.c).

### Live handoff and worker lifecycle

Compaction chuẩn bị và sync generation mới trước khi chuyển writer. Các writable handle được sắp theo địa chỉ con trỏ rồi lấy cache write lock; publication, chuyển descriptor/map/tham chiếu, đóng bộ cũ diễn ra trong vùng được bảo vệ; nhả khóa theo thứ tự ngược. Read-only handle giữ generation cũ tới release; đường xử lý publication không rõ kết quả có thể hoàn tất chuyển, phục hồi con trỏ cũ hoặc đánh dấu writer stale.

`release()` unregister handle, sync/đóng descriptor, rồi **chạy GC đủ điều kiện đồng bộ dưới path lock và cache write lock**. Sau khi nhả cả hai khóa mới schedule compaction. Worker duy nhất lấy việc từ queue FIFO có khử trùng lặp; `destroy()` chờ drain queue trước khi hủy registry/lock table. Khi worker không chạy hoặc đang dừng, scheduling fallback sang compaction đồng bộ dưới path lock.

## Performance mechanisms

Phần này giải thích các cơ chế tránh làm lại công việc và điều chỉnh bố trí dữ liệu. Các tỷ số đo được và giới hạn suy luận được trình bày riêng trong [Benchmarks](#benchmarks).

### Zstd context reuse

Mỗi worker tạo lười một `ZSTD_CCtx` và một `ZSTD_DCtx` qua TLS; destructor của `pthread_key_t` giải phóng khi thread kết thúc. Không chia sẻ context giữa các thread và không thêm global lock vào đường nén/giải nén. Mã giữ `ZSTD_CLEVEL_DEFAULT`; kiểm thử đối chiếu byte với API one-shot.

Giải nén reset cả session và tham số sticky trước mỗi frame. Lỗi Zstd loại context tương ứng để lần sau tạo lại; lỗi cấp phát TLS/context chuyển sang API one-shot ([compress.c](src/core/compress.c), [test_zstd_context.c](tests/test_zstd_context.c)).

### Chunk-I/O scratch buffers

Bốn slot TLS tách vai trò để các buffer sống đồng thời trong repack không dùng chung vùng nhớ. Slot tăng theo mức lớn nhất đã dùng, không thu nhỏ giữa các thao tác; destructor của khóa TLS riêng giải phóng khi thread kết thúc ([chunkio_scratch.c](src/core/chunkio_scratch.c)).

| Role | Giới hạn bộ nhớ giữ lại mỗi thread |
|---|---:|
| `RAW` | 1 MiB |
| `WINDOW` | 1 MiB |
| `CACHED` | 1 MiB |
| `COMP` | `ZSTD_COMPRESSBOUND(1 MiB)` = 1,052,672 byte |
| Tổng khi đã chạm đủ bốn role | **4,198,400 byte/thread** |

Yêu cầu vượt cap, TLS không khả dụng, grow thất bại hoặc cùng role còn bận dùng allocation tạm rồi free khi release; buffer giữ lại trước đó không bị thay thế khi grow thất bại. Cap không bao gồm allocation tạm, mảng `entries` trả caller, buffer plaintext của FUSE, buffer copy của compaction hay scratch lấy phần đầu frame trong decompressor.

### Adaptive windows

| Quy tắc trong [compact.c](src/core/compact.c) | Điều kiện |
|---|---|
| Mẫu tối thiểu | 128 sự kiện đủ điều kiện |
| Thu nhỏ | Partial RMW chiếm ít nhất 60%; giảm một bước ×2 |
| Tăng | Full-window write chiếm ít nhất 90%; tăng một bước ×2 |
| Điều kiện chi phí bổ sung | `signal_events × step_bytes >= live_bytes`, nhân bão hòa; `live_bytes` là tổng `stored_size` còn sống |
| Khoảng nghỉ sau resize | 3600 giây theo đồng hồ monotonic |
| Trong khoảng nghỉ | Tích lũy thống kê; lần write/release đủ điều kiện sau hết hạn giảm đôi các bộ đếm một lần rồi đánh giá |

Các bộ đếm thuộc generation trong bộ nhớ và được đặt lại khi daemon khởi động lại. Thay đổi window chỉ diễn ra khi repack sang generation mới trong miền 16 KiB–1 MiB; không trộn nhiều window size trong cùng generation.

## Testing

Phần này chỉ ra bộ kiểm thử phù hợp với từng lớp mã và yêu cầu mount của chúng. Kết quả kiểm thử gọi trực tiếp không thay thế kiểm chứng hợp đồng qua FUSE, còn kiểm thử hoàn tất bình thường không chứng minh khả năng chịu mất điện.

### Make targets

Các target lấy từ [Makefile](Makefile). Những target sanitizer dưới đây được liệt kê để sử dụng khi cần, **not executed here**; lần sửa README chạy bộ unit, mounted suite và Python M0, không chạy lại toàn bộ ma trận sanitizer.

| Target | Nội dung | Cần mount FUSE? | Đã chạy trong lần kiểm tra này? |
|---|---|---|---|
| `make test` | `test_suite.sh`: đọc/ghi, `O_TRUNC`, append, unlink, tệp nhiều chunk (>64 KiB), nén/raw và heuristic magic bytes, ghi đè qua biên, sparse hole, truncate chunk nén, thư mục lớn, compaction, migration, ghi song song | Có, mount sẵn | Có: 84 PASS / 0 FAIL |
| `make test-unit` | Metadata, file/handle/cache, registry/GC, Zstd, scratch, khóa, inspector; guard shell và định dạng đầu ra của cả bốn microbenchmark | Không | Có: exit 0 |
| `make test-generation-registry` | Registry, claim/finalize GC, tham chiếu và các tình huống đồng thời | Không | Binary được chạy bởi `test-unit` |
| `make test-chunkio-scratch` | Scratch độc lập và tích hợp chunk-I/O | Không | Hai binary được chạy bởi `test-unit` |
| `make test-lock` | Refcount, thu hồi và đồng thời của bảng path lock | Không | Binary được chạy bởi `test-unit` |
| `make tsan-file-ops` | File/handle/handoff dưới ThreadSanitizer | Không | Không |
| `make tsan-generation-registry` | Registry/GC dưới ThreadSanitizer | Không | Không |
| `make test-chunkio-scratch-tsan` | Cả hai bộ scratch dưới ThreadSanitizer | Không | Không |
| `make test-lock-tsan` | Path lock dưới ThreadSanitizer | Không | Không |
| `make tsan-generation-registry-bench` | Kiểm tra ngắn đầu ra benchmark registry/GC dưới ThreadSanitizer | Không | Không |
| `make help`, `make test-help` | In danh sách target | Không | Có |

Một số phạm vi cụ thể của các bộ kiểm thử:

- Metadata v0/v1/v2, replay journal, phần đuôi ghi dở, checkpoint, resize và lỗi publication.
- Zstd: đối chiếu byte, phục hồi sau lỗi, cô lập TLS, destructor và reset tham số sticky.
- Scratch: reuse/grow/cap, yêu cầu vượt cap, acquire lồng nhau, dọn sau lỗi và cô lập thread.
- Path lock/registry: đồng thời, thu hồi refcount, GC hai pha và chuyển writer đang mở.
- Mounted TC19–TC29: hole ghi chồng lấn, đọc zero, cắt chunk nén, `readdir` hơn 2048 entry (fixture 2200 tệp), thứ tự ghi/sync qua kiểm tra không crash, packing, migration và compaction nền.

**Giới hạn của mounted suite:** TC15 chỉ đọc lặp lại, không unmount/remount hoặc ép cold cache. TC22 kiểm tra nội dung và thời gian append, không thực hiện crash test; ghi chú thủ công trong script cũng không xác lập hợp đồng mất điện. Xác minh remount của M0 là bằng chứng riêng trong [tài liệu M0](benchmarks/m0_evidence_finalization.md).

### Test-only hooks and offline M0 tests

`MYFS_TEST_FAILPOINTS` bật hook phục vụ tiêm lỗi, quan sát trạng thái và kiểm soát lịch chạy trong test; các hook đó không được biên dịch vào binary `myfs` của `make release`. Makefile bật macro cho một số test và microbenchmark scratch; đó không phải tùy chọn runtime cho người dùng. Nguồn: [compact_test.h](src/core/compact_test.h), [lock_test.h](src/core/lock_test.h), [chunkio_scratch.h](src/core/chunkio_scratch.h), [Makefile](Makefile).

Lệnh Python đã chạy thành công, độc lập với `make test-unit`:

```bash
python3 -B -m unittest discover -s tests -p 'test_m0_*.py'
```

Kết quả là **264 tests / OK**. Các test M0 ngoại tuyến kiểm tra fixture/oracle, runner, guard, phép tính và diagnostic; adapter giả lập không phải bằng chứng FUSE thật. Xem [M0 profiling tooling](#m0-profiling-tooling) cho self-test thư mục thường.

## Benchmarks

Phần này phân biệt phép đo cơ chế riêng với phép đo ứng dụng qua mount. Các số lịch sử được giữ nguyên, kèm nguồn và giới hạn; không có benchmark dài nào được chạy lại để sửa README.

### Benchmark index

| Nguồn | Make target | Phạm vi | Cần mount? |
|---|---|---|---|
| [zstd_context_bench.c](benchmarks/zstd_context_bench.c) | `make bench-zstd-context` | API one-shot so với TLS context; p50/p99, ops/s, throughput nhiều thread | Không |
| [chunkio_scratch_bench.c](benchmarks/chunkio_scratch_bench.c) | `make bench-chunkio-scratch` | `malloc/free` so với scratch TLS; gọi acquire/release thật, không FUSE/I/O/Zstd | Không |
| [lock_table_bench.c](benchmarks/lock_table_bench.c) | `make bench-lock-table` | Bảng linear frozen từ `add0290` so với bảng sharded | Không |
| [generation_registry_bench.c](benchmarks/generation_registry_bench.c) | `make bench-generation-registry` | Public API registry/GC thật, gồm khóa path/cache của caller và I/O thật | Không |
| [benchmark.sh](benchmark.sh) | `make bench` | 10 phần BM01–BM10 qua mount và control | Có |
| [m0_profile.py](benchmarks/m0_profile.py) | Chưa có Make target | Vòng đời, phép đo ứng dụng và diagnostic M0 qua CLI Python | Tùy backend; `myfs` cần FUSE |

Các target chạy benchmark trong bảng **not executed here** vì yêu cầu không chạy benchmark dài. `make test-unit` chỉ chạy các kiểm tra ngắn về output/self-check của microbenchmark, không tạo số đo performance mới để thay các bảng dưới.

| Công cụ phụ trợ | Mục đích |
|---|---|
| `make build-generation-registry-bench` | Build benchmark với core hiện tại |
| `make build-generation-registry-baseline` | Build cùng nguồn benchmark với core ở `BASELINE_TREE`; bắt buộc `BASELINE_OUTPUT` |
| `make compare-generation-registry-bench` | Đọc bốn kết quả normal/A/A; cần các biến `GENERATION_REGISTRY_*_RESULT` trong Makefile |
| `make benchmarks/meta_inspect` | Build inspector dùng parser metadata production |

Scratch mặc định có 7 workload × 1/8 thread × 7 lần lặp theo cặp, đổi thứ tự chạy; CSV ghi byte giữ lại/role, grow và allocation tạm trong vùng đo. Lock-table mặc định dùng 1/4/8/16 worker × 0/64/256 resident path, 7 lần lặp theo cặp; có thể chọn riêng `--workers 8 --resident-paths 256` hoặc control `--workers 1 --resident-paths 0`.

`benchmark.sh` ghi `benchmark_results.txt` cục bộ. Các phần bao gồm ghi/đọc tuần tự, tỷ lệ nén theo dữ liệu, độ trễ RMW, baseline ext4, bảng so sánh, heuristic bỏ nén, chuẩn bị kiểm tra metadata/remount, ước tính overhead FUSE/Zstd và kiểu append. BM07/BM10 gọi `meta_inspect`, dùng parser/validation/journal replay production thay vì đoán offset trên đĩa.

BM08 tạo checksum và script xác minh để người chạy tự remount; nó không tự thực hiện remount. Các kết luận “bottleneck” được BM09 in từ so sánh throughput là suy luận của script, chưa phải phép đo nhân quả; xem kết luận thận trọng hơn của [M0 §7](benchmarks/m0_evidence_finalization.md#7-measured-cost-and-bottleneck-disposition).

### Methodology and interpretation

- Ghi revision, compiler/cờ build, CPU/kernel, filesystem thật, affinity, dữ liệu và trạng thái cache. Pin CPU của foreground và worker; xét cả SMT siblings, không chỉ số CPU.
- Giữ control, chạy baseline/candidate xen kẽ và A/A cho cùng binary; so chênh lệch với mức nhiễu của đúng workload/mode/thread count trước khi kết luận.
- Host không pin có thể nhiễu lớn; pinning vẫn không loại hết preemption, thay đổi tải/tần số hoặc drift giữa lần chạy. Số đo lịch sử bên dưới cho thấy control dao động và A/A spread đáng kể.
- Ghi rõ vùng tính giờ và trạng thái cache thực sự được xác nhận. Không gọi một lượt “cold-cache” nếu thao tác thả cache thất bại; helper ở [benchmark_helpers.sh](benchmark_helpers.sh) có đường báo lỗi.
- GC liên tục là tải stress đồng bộ hóa, không đại diện tần suất GC thường ngày. Microbenchmark không xác lập hiệu năng ứng dụng FUSE toàn tuyến; throughput gate cũng không kiểm định percentile latency.
- Dùng filesystem được xác nhận cho I/O thật. `ext`/magic `0xef53` không tự phân biệt ext2/ext3/ext4; baseline của campaign bên dưới được xác nhận là ext4.

### Historical scratch and Zstd results

**Trạng thái bằng chứng:** các số trong phần này được chép nguyên từ README trước khi viết lại (`5aa1e2f:README.md`, phần kết quả benchmark). Repository không lưu raw output tương ứng để kiểm tra lại số đo; chúng **chưa được xác minh độc lập trong lần sửa này**, và không phải cam kết hiệu năng. Thiết kế instrument và kiểm tra output vẫn kiểm chứng được trong mã/test.

README cũ ghi lần chạy scratch mặc định 7 repetition ngày 2026-10-04: gọi trực tiếp acquire/release production, chạm cache line đầu/cuối, warmup trước đo và đổi thứ tự baseline/TLS. Bảng giữ nguyên median được báo cáo; tỷ số là TLS so với baseline.

| Workload | 1 thread | 8 thread | Retained/thread | Timed growth/temp |
|---|---:|---:|---:|---:|
| RAW 64 KiB | 1.70× | 1.85× | 65,536 B | 0 / 0 |
| COMP bound 64 KiB | 1.73× | 1.70× | 65,824 B | 0 / 0 |
| Partial RMW 64 KiB | 2.23× | 2.17× | 196,896 B | 0 / 0 |
| Adaptive shrink 1 MiB → 512 KiB | 34.47× | 76.49× | 3,147,776 B | 0 / 0 |
| Mixed 16 KiB–1 MiB | 2.37× | 2.21× | 3,149,824 B | 0 / 0 |
| Max retained | 43.51× | 77.67× | 4,198,400 B | 0 / 0 |
| Oversize fallback | 1.03× | 0.99× | 0 B | 0 / temporary-only |

[Test output scratch](tests/test_chunkio_scratch_benchmark_output.sh) kiểm tra metric hữu hạn/dương, đủ workload/mode/thread count, không grow sau warmup và không allocation tạm ở workload thường. Nó kiểm tra cap từng role/tổng 4,198,400 byte/thread và oversize chỉ dùng allocation tạm, không tăng retained capacity; không dùng ngưỡng tốc độ trong CI.

README cũ báo cáo Zstd context qua hai lần đo độc lập:

| Workload | TLS reuse so với one-shot |
|---|---:|
| Single-thread, buffer 1–64 KiB | **1.27×–2.50×** |
| 8 worker thread, buffer 4 KiB | **1.41×–1.67×** throughput |

Nhận xét cũ “không thấy serialization hay lock contention” chưa thể tái kiểm chứng từ raw output trong repository. Mã dùng context riêng từng thread; instrument tách việc tạo/hủy `ZSTD_CCtx`/`ZSTD_DCtx` khỏi FUSE, I/O và page cache.

README cũ còn ghi `benchmark.sh` chạy baseline/candidate năm lần xen kẽ với cold-cache được xác minh. Mọi chênh lệch median nhỏ hơn spread min–max trong từng phía; control ext4 write là **211–461 MB/s**, noise floor lớn hơn ngưỡng **5%**, nên kết luận là **inconclusive**. Hai lần so sánh control Zstd ghi compress **−0.04%/−0.67%**, decompress **+0.16%/+0.35%**, với nhận xét không thấy hồi quy tính toán; toàn bộ kết quả và điều kiện lịch sử này có cùng trạng thái chưa xác minh nêu trên.

Số **1550 MB/s** cho giải nén từng xuất hiện trong bảng quyết định cũ nhưng không kèm phép đo hay nguồn tái lập. Giữ ở đây như số lịch sử **chưa xác minh**, không dùng để biện minh chọn codec hoặc so sánh Zstd với zlib/LZ4.

### Generation registry and GC campaign

Nguồn có thể kiểm tra trong Git: [raw outputs](benchmarks/results/2026-10-09-generation-registry-pooled/), [manifest](benchmarks/results/2026-10-09-generation-registry-pooled/manifest.md), [SHA256SUMS](benchmarks/results/2026-10-09-generation-registry-pooled/SHA256SUMS). Bản lưu gồm tám file của campaign ngày 2026-10-09, mỗi file 15 dòng summary, tổng 120 dòng; đây là summary các lần lặp, không phải 120 latency observation riêng lẻ.

| Workload | Nội dung đo |
|---|---|
| `control` | Vòng tính toán CPU; không gọi registry/GC trong vùng đo |
| `scalability` | Register → snapshot → bump epoch → unregister, path riêng mỗi thread |
| `hot-path` | Cùng transaction trên một path chung |
| `gc-interference` | Snapshot/bump epoch trên path không liên quan trong khi một thread tạo/thu hồi generation thật liên tục |

Các workload registry gồm cả path/cache locks của caller, không phải phép đo riêng mutex registry. Mode `real` dùng I/O thật; `synthetic-delay` vẫn dùng I/O thật và thêm delay yêu cầu tại syscall được wrap bên trong GC. Instrument không mount FUSE, không dùng `benchmark.sh` hoặc test hooks.

| Cấu hình campaign | Giá trị giữ nguyên |
|---|---|
| Candidate | `743c2b467041994cc64dbcc2714a8952fe406f1e` |
| Baseline `origin/main` tại lúc đo | `7f8f335b7a00fc4d928475fa9d947d3a3187b00a`; mutex registry toàn cục, GC I/O dưới mutex đó |
| Nguồn benchmark | Cùng nguồn pooled-reporting từ `743c2b4`, link với core thật của từng tree |
| Host | Intel Core i5-12450H, x86_64, Linux `6.17.0-41-generic`, ext4 |
| Set A | Foreground CPU `0,2,4,6`; GC CPU `8` |
| Set B | Foreground CPU `1,3,5,7`; GC CPU `9` |
| Quan hệ SMT | `0/1`, `2/3`, `4/5`, `6/7` là siblings; A/B không phải hai nhóm core vật lý độc lập; GC không dùng sibling của foreground |
| Thread và lần lặp | `1,2,4` foreground thread; một GC worker; bảy primary repetitions/dòng |
| Warmup | 128 operation/thread, loại khỏi vùng đo |
| Basic workload | 200000 iteration/thread |
| GC foreground | Ít nhất 100000 operation/thread **và** ít nhất 20 completed GC cycles/repetition; phải đạt cả hai |
| Ngưỡng delayed | Strict `> 1000 µs` (`> 1000000 ns`) |
| Synthetic delay | 250 µs/call, vẫn chạy trên ext4 |
| Seed | 25228 |
| Controls | Normal run và A/A riêng cho cả hai tree; `TREE` ghi core được link, không phải commit tài liệu sau đó |

Median throughput dưới đây có đơn vị **operation/s tổng**; operation là transaction/loop của bảng workload. Tỷ số giữ nguyên là current/baseline, lớn hơn 1 nghĩa là throughput đo được cao hơn.

| Workload | Set / threads | Baseline | Current | Current / baseline |
|---|---|---:|---:|---:|
| Distinct paths | A / 2 | 517,975 | 1,452,712 | 2.805× |
| Distinct paths | A / 4 | 433,616 | 2,620,246 | 6.043× |
| Distinct paths | B / 2 | 507,407 | 1,446,853 | 2.851× |
| Distinct paths | B / 4 | 446,372 | 2,595,749 | 5.815× |
| Same-path hot-path | A / 4 | 470,971 | 447,765 | 0.951× |
| Same-path hot-path | B / 4 | 465,890 | 452,790 | 0.972× |

Distinct-path scaling tăng ở các dòng trên; same-path hot-path giảm throughput quan sát được. Riêng B / 4 không vượt A/A spread của chính dòng đó, nên không coi mọi chênh lệch nhỏ là hồi quy đã xác lập.

GC-interference với bốn foreground thread, **p99.9 (µs)**; đây là median của percentile từng repetition, không phải percentile gộp:

| Set | I/O mode | Baseline p99.9 | Current p99.9 |
|---|---|---:|---:|
| A | Real ext4 | 8.732 | 3.663 |
| A | Synthetic-delay trên ext4 | 8.951 | 3.696 |
| B | Real ext4 | 11.274 | 3.236 |
| B | Synthetic-delay trên ext4 | 11.901 | 3.030 |

Các case đại diện sau gộp bảy primary repetitions, bốn foreground thread. Fraction có miền 0–1, không phải phần trăm; mẫu số là operation count thực đo.

| Case | Tree | delayed_total / pooled_operations | pooled_delayed_fraction | delayed_per_gc_cycle |
|---|---|---:|---:|---:|
| A / real | Baseline | 623 / 3,824,159 | 0.000162912 | 3.643274854 |
| A / real | Current | 16 / 7,683,635 | 0.000002082 | 0.108843537 |
| B / synthetic-delay | Baseline | 662 / 2,908,335 | 0.000227622 | 3.343434343 |
| B / synthetic-delay | Current | 1 / 14,037,798 | 0.000000071 | 0.006802721 |

| Metric | Định nghĩa |
|---|---|
| `median_rep_delayed_fraction` | Median fraction từng primary repetition với latency strict > threshold; có thể 0 dù tổng gộp có delayed operation |
| `delayed_total` | Tổng operation strict > threshold qua primary repetitions |
| `pooled_operations` | Tổng operation count thực đo qua primary repetitions |
| `pooled_delayed_fraction` | `delayed_total / pooled_operations`; mẫu số 0 trả 0 |
| `delayed_per_gc_cycle` | `delayed_total / tổng completed primary GC cycles`; không có GC trả 0 theo quy ước |
| `max_ns` | Latency lớn nhất qua tất cả primary repetitions, ns; chỉ để tham khảo |
| `p50_ns`, `p99_ns`, `p999_ns` | Median p50/p99/p99.9 của từng primary repetition, ns; không phải pooled percentile |

Mỗi measured operation có một latency observation; warmup và A/A companion không vào primary pooled totals. GC bắt đầu trước foreground và cycle cuối có thể hoàn tất sau nó, nên mẫu số `delayed_per_gc_cycle` không khớp hoàn toàn vùng đo foreground. Raw summary không xuất mẫu từng repetition hay tổng GC cycle; không tái dựng chúng từ `gc_min_cycles`.

`--aa` đo cặp cùng binary, đổi thứ tự primary/companion; spread là `100 × (high − low) / low`. Script so sánh dùng throughput normal run, A/A gate đúng workload/mode/thread count và global gate. Max paired A/A spread là **29.07% (A)** và **53.58% (B)**; global max có thể che khác biệt giữa các dòng.

`noise_floor_pct=0` với `win_gate=unavailable` trong normal run không chứng minh nhiễu bằng 0. A/A cho thấy variability, còn drift giữa lần chạy vẫn có thể khiến chênh lệch nhỏ inconclusive.

Control baseline A / 4 có **9 / 5,600,000** operation vượt **1 ms** dù không chạy GC; `delayed_per_gc_cycle=0` ở đó chỉ là quy ước. Current vẫn có operation vượt 1 ms như bảng gộp. Public API không xuất lock-hold intervals để đo registry lock duty cycle; không quy từng spike cho GC hoặc diễn giải tỷ số throughput thành mức tăng tốc FUSE toàn tuyến.

### Reproduction

Kiểm tra checksum bản lưu là thao tác đọc ngoại tuyến đã chạy thành công:

```bash
(cd benchmarks/results/2026-10-09-generation-registry-pooled && sha256sum --check SHA256SUMS)
```

Đo lại cần chọn CPU hợp lệ trên host và xác nhận filesystem. Script dưới giữ baseline cố định, build candidate từ checkout hiện tại: nếu checkout khác `743c2b4`, đó là phép so sánh mới, không phải khôi phục đúng binary campaign cũ.

<details>
<summary>Script so sánh registry/GC với baseline cố định</summary>

**not executed here**: script chạy benchmark dài và tạo worktree; không thuộc lần kiểm tra tài liệu này. CPU trong script là cấu hình host của campaign cũ, phải điều chỉnh theo topology và quyền affinity của máy thực hiện.

```bash
registry_artifacts=$(mktemp -d /tmp/myfs-registry-results.XXXXXX)
git worktree add --detach "$registry_artifacts/baseline-tree" \
    7f8f335b7a00fc4d928475fa9d947d3a3187b00a
make build-generation-registry-bench
make build-generation-registry-baseline \
    BASELINE_TREE="$registry_artifacts/baseline-tree" \
    BASELINE_OUTPUT="$registry_artifacts/baseline-bench"
./benchmarks/generation_registry_bench --help

registry_workdir=$(mktemp -d "$PWD/.registry-bench-io.XXXXXX")
findmnt -T "$registry_workdir" -o TARGET,SOURCE,FSTYPE
# Xac nhan ext4 truoc khi tiep tuc; khong dung tmpfs.
registry_args=(--workdir "$registry_workdir" --threads 1,2,4
    --iterations 200000 --gc-iterations 100000 --min-gc-cycles 20
    --repetitions 7 --warmup 128 --gc-threads 1 --threshold-us 1000
    --synthetic-delay-us 250 --gc-mode both --seed 25228)
for registry_set in A B; do
    if [[ $registry_set == A ]]; then
        registry_cpus=0,2,4,6; registry_gc_cpu=8
    else
        registry_cpus=1,3,5,7; registry_gc_cpu=9
    fi
    for registry_role in current baseline current-aa baseline-aa; do
        case $registry_role in
            current*) registry_binary=./benchmarks/generation_registry_bench ;;
            baseline*) registry_binary="$registry_artifacts/baseline-bench" ;;
        esac
        registry_aa=()
        case $registry_role in *-aa) registry_aa=(--aa) ;; esac
        timeout 300 "$registry_binary" "${registry_args[@]}" \
            --cpus "$registry_cpus" --gc-cpu "$registry_gc_cpu" \
            "${registry_aa[@]}" \
            >"$registry_artifacts/$registry_set-$registry_role.out" \
            2>"$registry_artifacts/$registry_set-$registry_role.stderr"
    done
    ./benchmarks/compare_generation_registry_results.sh \
        "$registry_artifacts/$registry_set-current.out" \
        "$registry_artifacts/$registry_set-baseline.out" \
        "$registry_artifacts/$registry_set-current-aa.out" \
        "$registry_artifacts/$registry_set-baseline-aa.out" \
        >"$registry_artifacts/$registry_set-comparison.csv"
done
```

</details>

## M0 profiling tooling

Phần này giới thiệu công cụ tạo dữ liệu, chạy ứng dụng và giữ bằng chứng M0. Self-test ngoại tuyến cho phép kiểm tra đường đi của runner mà không mount hoặc tạo lại chiến dịch performance lịch sử.

| Tệp | Vai trò |
|---|---|
| [m0_fixture.py](benchmarks/m0_fixture.py) | Fixture CSV xác định được, manifest/hash và oracle kiểm tra nội dung độc lập với mã biến đổi của ứng dụng; có ứng viên dữ liệu entropy cao SHAKE-256 chưa được đo khả năng nén |
| [m0_csv_workflow.py](benchmarks/m0_csv_workflow.py) | Ứng dụng CSV dùng thư viện chuẩn Python, đọc/ghi qua đường dẫn, từ chối đầu ra đã tồn tại |
| [m0_profile.py](benchmarks/m0_profile.py) | CLI `prepare`, `run`, `verify`, `cleanup`, `self-test`; mở rộng `canonical-prepare`, `measure-prepare`, `measure-run`, `measure-self-test`; quản lý identity, deadline, log, provenance và xác minh |
| [m0_daemon_sync_diagnostic.c](benchmarks/m0_daemon_sync_diagnostic.c) | Diagnostic link riêng: sync-call hoặc backing-I/O; wrapper gọi hàm thật, ghi khoảng thời gian và xuất trace có giới hạn |
| [test_m0_profile.py](tests/test_m0_profile.py) | Test fixture/oracle, tính xác định và trường hợp nội dung sai |
| [test_m0_runner.py](tests/test_m0_runner.py) | Test runner, ownership/cleanup, phép đo, diagnostic và các tình huống lỗi ngoại tuyến |

Diagnostic C không nằm trong build `myfs` thông thường. Hai tập diagnostic sync/backing-I/O được link riêng, không bật đồng thời; trace không tự đại diện thời gian đĩa phục vụ hoặc ứng dụng chờ. Profile Python có instrumentation và được tách khỏi baseline không instrumentation.

Self-test sau đã chạy thành công; nó dùng thư mục cha mới thuộc người chạy, không dùng trực tiếp thư mục cha chung như `/tmp`:

```bash
m0_check_dir=$(mktemp -d /tmp/myfs-readme-self-test.XXXXXX)
python3 -B benchmarks/m0_profile.py self-test --parent "$m0_check_dir" --records 100
```

Kết quả có `passed: true` và nhãn `offline correctness check on an ordinary directory`. Đây không phải benchmark ext4/FUSE; runner lưu artifact và bằng chứng, còn cleanup chỉ dọn workspace khi đạt điều kiện ownership/identity/trạng thái. Backend `myfs` cần executable và provenance được cung cấp rõ ràng, không tự chuyển sang thư mục thường khi mount lỗi.

[Tài liệu chấp thuận M0](benchmarks/m0_evidence_finalization.md) là nguồn về phạm vi, chiến dịch và quyết định. **Bằng chứng gốc M0 được giữ trên host, không nằm trong repository**; CSV tạm của các chiến dịch và một executable diagnostic cũ không còn sẵn. Receipts/hash lịch sử không phải xác minh mới các tệp đã vắng mặt; lần tái lập cần tạo dữ liệu và identity mới theo quy trình của runner.

## Repository layout

Phần này chỉ ra các nguồn được Git theo dõi và nơi lưu dữ liệu cục bộ. Danh sách được suy ra từ `git ls-files`, để một clone mới không bị hiểu là có sẵn binary hoặc kết quả đo ngoài Git.

| Đường dẫn được theo dõi | Nội dung |
|---|---|
| `README.md`, `Makefile`, `.gitignore` | Tài liệu, target build/test và quy tắc bỏ qua |
| `plan.md` | Kế hoạch kỹ thuật về cache, journal và window thích nghi; không phải roadmap M0–M5 |
| `src/main.c`, `src/myfs.h` | Điểm vào, cấu hình, kiểu dữ liệu và giao diện chung |
| `src/fuse_ops/file.c`, `src/fuse_ops/dir.c` | Callback tệp và thư mục |
| `src/core/path.c`, `metadata.c`, `compress.c`, `chunkio.c` | Đường dẫn, định dạng và đường dữ liệu |
| `src/core/chunkio_scratch.c`, `chunkio_scratch.h`, `lock.c`, `compact.c` | Buffer TLS, khóa, registry, GC và compaction |
| `src/core/compact_test.h`, `src/core/lock_test.h` | Giao diện hook test |
| `src/guards/guards.c`, `src/guards/guards.h` | Kiểm tra điều kiện dữ liệu |
| `test_suite.sh`, `tests/` | Mounted suite, test C/shell/Python và `meta_inspect.py` |
| `benchmark.sh`, `benchmark_helpers.sh` | Benchmark qua mount và helper |
| `benchmarks/` | Bốn microbenchmark, `meta_inspect.c`, script build/compare registry và công cụ M0 |
| `benchmarks/fixtures/lock_table_linear.c` | Baseline linear frozen |
| `benchmarks/results/2026-10-09-generation-registry-pooled/` | Tám raw output, manifest và checksum đã theo dõi |
| `benchmarks/m0_evidence_finalization.md` | Phạm vi bằng chứng và chấp thuận M0 |

**Đường dẫn cục bộ, không phải tệp nguồn được theo dõi:** `backing/`, `mountpoint/`, `benchmark-results/` chứa dữ liệu được ignore; binary `myfs`, binary test/microbenchmark và `benchmark_results.txt` được tạo khi chạy. Roadmap đầy đủ cũng được giữ cục bộ và bị ignore; README không liên kết đến tệp đó.

## Debugging

Phần này hướng dẫn xem log và metadata để phân biệt nội dung logic với dữ liệu nền. Dùng parser production trước khi diễn giải byte trực tiếp giúp tránh nhầm phiên bản metadata hoặc bỏ sót journal.

`LOG` trong [myfs.h](src/myfs.h) ghi stderr với timestamp dạng `[HH:MM:SS.mmm]`; macro này vẫn được dùng trong build release. Chạy foreground bằng `make run` để quan sát lỗi đọc/ghi, checkpoint, compaction và GC.

Sau ví dụ Quick start, các lệnh sau đã được kiểm tra thành công:

```bash
ls -la backing/readme-example.txt*
make benchmarks/meta_inspect
./benchmarks/meta_inspect backing/readme-example.txt.meta
hexdump -C backing/readme-example.txt.meta
```

Inspector trả các trường phân cách bằng tab: `chunks=N`, `logical_size=N`, `raw_chunks=N`; nó đọc v0/v1/v2 và replay journal bằng parser production. Khi có `.current`, alias có thể còn trỏ storage cũ trong lúc handle legacy vẫn mở; muốn xem generation hiện hành cần đối chiếu `.current` với tệp `meta` bên trong generation.

| Triệu chứng hoặc câu hỏi | Nơi kiểm tra |
|---|---|
| Mount không chạy | Thiết bị/quyền FUSE, thư mục mount/backing đã tồn tại, stderr của `main.c` |
| `-EIO` khi đọc | Metadata/CRC/frame trong `chunkio.c`, `compress.c`, `guards.c`; không mặc định gán cho một nguyên nhân |
| `-ESTALE` khi ghi | Trạng thái generation và đường publication/handoff trong `file.c`, `compact.c` |
| Dung lượng nền chưa giảm ngay sau đóng | Tham chiếu còn sống, điều kiện compaction và GC; `release()` có GC đồng bộ nhưng compaction thường được xếp hàng |
| Số benchmark khác nhau | Cấu hình CPU/cache/host, control/A/A, vùng tính giờ và revision; xem [Benchmarks](#benchmarks) |

## Design decisions

Phần này giữ các lựa chọn thiết kế chính và lý do có thể đối chiếu với implementation. Các lý do mô tả cơ chế đang dùng, không thay thế benchmark hay hợp đồng tương thích còn thiếu.

| Quyết định | Lý do và phạm vi hiện tại |
|---|---|
| FUSE thay vì kernel module | Callback và log chạy trong tiến trình người dùng qua libfuse, thuận tiện quan sát/debug |
| Per-file window 16 KiB–1 MiB | Chính sách giảm cho partial RMW, tăng cho full-window write; chỉ đổi khi repack generation |
| Zstd thay vì zlib/LZ4 cho payload | Implementation dùng Zstd, có context reuse và raw fallback; chưa có benchmark trong Git chứng minh Zstd có tỷ lệ nén cao nhất giữa các codec |
| Thư mục + `.data`/`.meta` | Tách blob và bản đồ để kiểm tra bằng inspector; generation thêm `.current`, thư mục và alias |
| Blob append-only | Ghi blob mới trước khi metadata tham chiếu tới nó; đổi tên nguyên tử áp dụng ở publication metadata/generation, không tự biến append thành giao dịch nguyên tử |
| Delta journal + checkpoint | Ghi delta cho phần map đổi, định kỳ ghi lại toàn map; loader bỏ qua phần đuôi chưa commit |
| Path/cache/registry lock hierarchy | Mutation theo path → cache → registry; read dùng cache → registry |
| Background compaction thread | `release()` GC đồng bộ rồi schedule compaction; queue khử trùng lặp, drain khi unmount, fallback đồng bộ khi worker không chạy |
| Writer-preferring cache rwlock | Bảo vệ chuyển bộ handle; read hợp lệ không gọi metadata syscall; không tuyên bố giới hạn chờ cho mọi tải |
| Zstd context theo worker | Reuse context độc lập, không chia sẻ context hay thêm global lock vào đường codec |
| Scratch TLS tách 4 role, có cap | RAW/COMP/WINDOW/CACHED cùng tồn tại; fallback tạm khi nested/oversize hoặc TLS/grow lỗi |
| Path-lock table 64 × 64 | Lookup theo bucket, mutex theo shard, eager reclaim khi hết tham chiếu |
| Generation registry 64 × 64 + GC hai pha | Gom generation cùng logical path vào một shard; I/O resolve/alias/remove ngoài registry shard |

Nguồn tương ứng nằm trong [main.c](src/main.c), [file.c](src/fuse_ops/file.c), [metadata.c](src/core/metadata.c), [compact.c](src/core/compact.c), [compress.c](src/core/compress.c), [chunkio_scratch.c](src/core/chunkio_scratch.c) và [lock.c](src/core/lock.c).

## Glossary

Phần này nhắc lại các thuật ngữ đã giải thích tại lần dùng đầu trong nội dung. Chúng mô tả đối tượng trong implementation, giúp đọc các tên hàm và tài liệu benchmark mà không phải đoán nghĩa.

| Thuật ngữ | Nghĩa trong myfs |
|---|---|
| Generation | Phiên bản lưu trữ gồm cặp dữ liệu/metadata; handle có thể giữ tham chiếu tới phiên bản đó |
| Window | Khoảng địa chỉ logic cố định trong một generation dùng để gom dữ liệu khi repack |
| Chunk | Đoạn dữ liệu có một mục metadata riêng, lưu thành một blob raw hoặc nén |
| RMW | Đọc dữ liệu cũ, sửa phần được yêu cầu rồi ghi lại phần liên quan |
| Epoch | Bộ đếm trong bộ nhớ để so phiên bản metadata với cache handle |
| Claim | Quyền xử lý một công việc kèm định danh được kiểm tra lại khi hoàn tất |
| Handoff | Chuyển handle đang mở sang bộ descriptor/map/tham chiếu generation mới dưới khóa |
| Journal | Nhật ký delta metadata nối sau bản đồ gốc |
| Checkpoint | Ghi lại toàn bản đồ metadata hiện hành để bắt đầu chu kỳ journal mới |
| Alias | Tên bổ sung, ở đây là hard link `.data`/`.meta` phục vụ tương thích |
| GC | Thu hồi storage generation cũ khi đạt điều kiện publication và tham chiếu |
| Compaction | Gom dữ liệu còn sống, repack khi cần và công bố generation mới |
| TLS | Bộ nhớ riêng theo thread, dùng cho context Zstd và buffer scratch |
| A/A | So hai lượt cùng binary để quan sát biến thiên của phép đo |

## License

Phần này ghi tình trạng khai báo giấy phép trong repository. Không có tệp `LICENSE` được tìm thấy trong checkout, nên tài liệu chưa thể chỉ ra điều khoản cấp phép.

License: not specified yet
