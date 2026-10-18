# myfs — filesystem nén dữ liệu trong suốt

**Ghi file như bình thường, myfs tự nén ở phía sau. Đọc lại thì myfs tự giải nén.** Ứng dụng không cần sửa gì.

myfs chạy ở user space (qua FUSE) trên Linux. Khi bạn ghi file vào một thư mục đã mount, myfs cắt dữ liệu thành các đoạn nhỏ, nén từng đoạn bằng Zstd rồi lưu vào một thư mục thật trên đĩa. Khi đọc, myfs tìm đúng đoạn cần, giải nén và trả về cho ứng dụng.

> **Trạng thái: nguyên mẫu nghiên cứu. Chưa dùng cho dữ liệu quan trọng.**
> myfs mới có các thao tác tệp cơ bản. Nó chưa có `rename`, `fsync`, `chmod`, `symlink`..., và chưa chứng minh được dữ liệu an toàn khi mất điện giữa chừng. Chi tiết ở phần [Hỗ trợ gì và chưa hỗ trợ gì](#hỗ-trợ-gì-và-chưa-hỗ-trợ-gì).

---

## Mục lục

| Nhóm | Mục | Đọc khi bạn muốn |
|---|---|---|
| **A. Dùng thử và đánh giá** | [Giới thiệu](#giới-thiệu) | Biết myfs là gì, xem ví dụ và ba khái niệm cơ bản |
| | [Trạng thái dự án](#trạng-thái-dự-án) | Biết dự án đang ở đâu và đã đo được gì |
| | [Bắt đầu nhanh](#bắt-đầu-nhanh) | Build, mount và thử trong 5 phút |
| | [Hỗ trợ gì và chưa hỗ trợ gì](#hỗ-trợ-gì-và-chưa-hỗ-trợ-gì) | Kiểm tra trước khi cho ứng dụng thật dùng myfs |
| | [Câu hỏi thường gặp](#câu-hỏi-thường-gặp) | Có câu trả lời ngắn cho các thắc mắc hay gặp |
| **B. Hiểu cách hoạt động** | [Cách hoạt động](#cách-hoạt-động) | Hiểu một lần ghi và một lần đọc diễn ra thế nào |
| | [Dữ liệu nằm ở đâu trên đĩa](#dữ-liệu-nằm-ở-đâu-trên-đĩa) | Biết tên file và thư mục trong `backing/` |
| | [Định dạng lưu trữ chi tiết](#định-dạng-lưu-trữ-chi-tiết) | Đọc hoặc sửa metadata: window, chunk, v0/v1/v2, journal |
| | [Đường đọc và đường ghi](#đường-đọc-và-đường-ghi) | Xem từng bước của `read`, `write`, `truncate` |
| | [Chạy nhiều luồng cùng lúc](#chạy-nhiều-luồng-cùng-lúc) | Hiểu thứ tự khóa, dọn dữ liệu cũ (GC), chuyển handle sang phiên bản mới |
| | [Các kỹ thuật tăng tốc](#các-kỹ-thuật-tăng-tốc) | Biết cách tái dùng context Zstd, buffer tạm và window tự điều chỉnh |
| **C. Kiểm thử và đo đạc** | [Kiểm thử](#kiểm-thử) | Biết các lệnh `make test*` và cái nào cần mount |
| | [Benchmark](#benchmark) | Đọc số đo và biết cách diễn giải cho đúng |
| | [Công cụ đo M0](#công-cụ-đo-m0) | Dùng bộ công cụ Python/C của đợt đo M0 |
| **D. Tra cứu** | [Cấu trúc thư mục repo](#cấu-trúc-thư-mục-repo) | Tìm xem file nào nằm ở đâu |
| | [Debug](#debug) | Xem log, đọc metadata, tra bảng triệu chứng |
| | [Quyết định thiết kế](#quyết-định-thiết-kế) | Biết vì sao thiết kế như vậy |
| | [Từ điển thuật ngữ](#từ-điển-thuật-ngữ) | Tra nghĩa một thuật ngữ |
| | [License](#license) | Biết tình trạng giấy phép |

> **Gặp từ lạ?** Mọi thuật ngữ đều có trong [Từ điển thuật ngữ](#từ-điển-thuật-ngữ). Thuật ngữ quan trọng cũng được giải thích ngay lần đầu xuất hiện.

---

## Giới thiệu

myfs là một filesystem nén dữ liệu "trong suốt": người dùng và ứng dụng thao tác với file như bình thường, việc nén và giải nén diễn ra ở bên dưới.

### Một ví dụ

```text
Bạn ghi file 10 MB vào mountpoint/
        |
        |   myfs cắt thành các đoạn nhỏ, nén từng đoạn bằng Zstd
        v
Thư mục backing/ lưu: dữ liệu đã nén + một file "bản đồ" (metadata)
        ^
        |   đọc: myfs tra bản đồ, giải nén đúng đoạn cần, trả cho ứng dụng
        |
Bạn đọc lại file từ mountpoint/ như bình thường
```

### Ba khái niệm cần biết trước

| Khái niệm | Nghĩa là gì |
|---|---|
| **mountpoint** | Thư mục bạn thao tác. Nhìn vào đó bạn thấy file như bình thường, chưa nén. |
| **backing directory** | Thư mục thật trên đĩa, nơi myfs cất dữ liệu đã nén. Đừng sửa tay trong đó. |
| **metadata** | File "bản đồ" ghi mỗi đoạn dữ liệu nằm ở đâu, dài bao nhiêu, có nén không. Thiếu bản đồ thì không đọc lại được dữ liệu. |

Nhiều thuật ngữ khác (chunk, window, generation, journal...) sẽ xuất hiện ở phần [Cách hoạt động](#cách-hoạt-động). Người chỉ muốn dùng thử không cần hiểu chúng ngay.

### myfs là gì và không là gì

| Khía cạnh | myfs hiện tại |
|---|---|
| Giao diện | Các thao tác FUSE cơ bản: tạo, đọc, ghi, cắt (truncate), xóa file; tạo, xóa, liệt kê thư mục |
| Cách dùng | `cat`, ghi nối đuôi, ghi đè một phần qua mountpoint; không phải giải nén thủ công trước khi đọc |
| Lưu trữ | Các đoạn nén (hoặc không nén) kèm bản đồ metadata |
| So với zip/RAR | Không phải công cụ tạo một file nén. myfs cho bạn một thư mục mà bên trong dữ liệu đã được nén sẵn |
| **Không phải** | Filesystem POSIX đầy đủ; nơi lưu dữ liệu quan trọng; thứ đã được chứng minh an toàn khi mất điện |

### Mục tiêu dự án

Hướng phát triển là **một thư mục nén có thể đọc và ghi trực tiếp cho dữ liệu thô trên Linux**: ứng dụng dùng đường dẫn thông thường, giữ nguyên mã và định dạng dữ liệu của mình ([tài liệu M0, mục 1](benchmarks/m0_evidence_finalization.md#1-purpose-authority-and-decision)). Hiện dự án mới ở giai đoạn đo đạc và làm rõ phạm vi tương thích, chưa phải sản phẩm hoàn chỉnh.

---

## Trạng thái dự án

Dự án là nguyên mẫu nghiên cứu. Giai đoạn đo đạc đầu tiên (M0) đã xong. Các giai đoạn tiếp theo chưa bắt đầu.

### Tóm tắt trong năm dòng

1. myfs đọc và ghi được file cơ bản, nén bằng Zstd, có cơ chế dọn dữ liệu cũ và tự điều chỉnh kích thước đoạn.
2. Ngày **2026-10-17**, chủ dự án chấp thuận M0 (đợt đo đạc và thu thập bằng chứng) trong phạm vi thí nghiệm đã ghi lại.
3. Trong M0, myfs chạy một quy trình lọc CSV chậm hơn ext4 khoảng **9% đến 13%**, và **chưa xác định được nguyên nhân**.
4. Nhiều điều quan trọng với một filesystem thật vẫn **chưa được kiểm chứng**: `fsync`, mất điện, nhiều tiến trình cùng truy cập, `rename`...
5. Mọi con số M0 chỉ đúng cho một quy trình, một máy. Đừng suy ra nó cho mọi ứng dụng.

### Lộ trình

Lộ trình đầy đủ được giữ trên máy của chủ dự án (không nằm trong repo). Bảng dưới là tóm tắt.

| Mốc | Trọng tâm | Trạng thái |
|---|---|---|
| **M0** | Đo đạc: baseline lặp lại được, bằng chứng gốc, đầu ra đúng, tách danh sách "khoảng trống tương thích" khỏi kết luận về điểm nghẽn | Đã chấp thuận trong phạm vi thí nghiệm |
| **M1** | Hợp đồng về tính đúng và khôi phục: thấy dữ liệu mới giữa các tiến trình, sync, báo lỗi, phục hồi sau sự cố, cùng các test tương ứng | Chưa bắt đầu |
| **M2** | Tương thích ứng dụng: đổi tên, thuộc tính, thư mục trong phạm vi đã chọn | Chưa bắt đầu |
| **M3** | Giới hạn vận hành (hết đĩa, hết RAM, công việc nền) và tối ưu dựa trên điểm nghẽn đo được | Chưa bắt đầu |
| **M4** | Công cụ sử dụng: vòng đời mount, thống kê, kiểm tra ngoại tuyến, xuất và phục hồi | Chưa bắt đầu |
| **M5** | Thử nghiệm với người dùng, đóng gói tối thiểu, điều kiện phát hành | Chưa bắt đầu |

### Kết quả M0, nói đơn giản

M0 chạy cùng một chương trình lọc CSV trên hai nơi: thư mục ext4 thường và thư mục mount của myfs, rồi so thời gian và dung lượng.

| Câu hỏi | Kết quả |
|---|---|
| Chương trình có chạy đúng trên myfs không? | Có. Cả 18 lần thử đều ra đúng đầu ra, kiểm tra độc lập bằng một "oracle" riêng |
| myfs chậm hơn ext4 bao nhiêu? | Median thời gian chạy: ext4 **6,92 s**, myfs **7,82 s**. Trong 4 cặp so sánh, myfs chậm hơn 9% đến 13% |
| Vì sao chậm hơn? | **Chưa biết.** M0 không xác lập được nguyên nhân. Điều này không có nghĩa là không có điểm nghẽn, chỉ là chưa chứng minh được cái nào |
| Dung lượng trên đĩa | ext4: 173.314.048 byte. myfs: 37.384.192 byte |

**Hãy đọc kỹ các giới hạn:**

- Dữ liệu là CSV **tổng hợp, rất dễ nén**. Tỷ lệ 173 MB xuống 37 MB **không phải** tỷ lệ nén bạn sẽ gặp với dữ liệu thật.
- Một máy, một chương trình, tám lần đo mỗi bên. Không đủ để kết luận chắc chắn về thống kê.
- Một phần giảm dung lượng đến từ việc chương trình tự lọc bớt dòng và cột, **không phải** do filesystem nén.
- Thời gian trên chỉ tính từ lúc khởi chạy đến lúc chương trình kết thúc. Chuẩn bị dữ liệu, kiểm tra và tắt daemon nằm ngoài.

M0 cũng đã đo riêng từng thành phần để tìm nguyên nhân (CPU của Python, các lời gọi sync, các lời gọi đọc/ghi xuống đĩa). Không có phép đo nào chỉ ra nguyên nhân chắc chắn, và **không được cộng các số đó lại** vì chúng đến từ các lần chạy khác nhau. Chi tiết nằm trong [tài liệu chấp thuận M0](benchmarks/m0_evidence_finalization.md). Bằng chứng gốc nằm trên máy chạy đo, không có trong repo.

---

## Bắt đầu nhanh

Phần này hướng dẫn build, mount, thử đọc ghi và unmount. Các lệnh dưới đây chạy từ thư mục gốc của repo.

### Yêu cầu

| Thành phần | Dùng để |
|---|---|
| Linux có `/dev/fuse` và quyền mount FUSE | Chạy filesystem và test có mount |
| GCC, GNU Make, `pkg-config`, glibc (pthread) | Build. Build cần glibc vì dùng rwlock ưu tiên writer của glibc |
| Header và thư viện FUSE 3, Zstd, zlib | `fuse3`, `-lzstd`, `-lz` (xem [Makefile](Makefile)) |
| `fusermount3`, Bash, Python 3, `mountpoint` | Unmount, chạy test |
| `hexdump` | Tùy chọn, để xem byte metadata |

Ví dụ cài gói trên Debian/Ubuntu:

```bash
sudo apt install libfuse3-dev libzstd-dev zlib1g-dev pkg-config gcc make fuse3 python3
```

### Build và mount

```bash
make                      # build bản debug (-g)
make release              # build lại bản tối ưu (-O2 -DNDEBUG)
mkdir -p backing mountpoint
make run                  # mount, chạy ở foreground
```

`make run` chiếm terminal đó. **Mở một terminal khác** cùng thư mục repo để thử:

```bash
echo "Hello World" > mountpoint/readme-example.txt
cat mountpoint/readme-example.txt
ls -la mountpoint/
```

Chạy bộ test có mount (cần myfs đang chạy ở terminal kia):

```bash
make test
```

Khi kiểm tra README này (2026-10-18), bộ test có mount cho kết quả **84 PASS / 0 FAIL**.

### Tháo mount và chạy test không cần mount

```bash
make umount               # tháo mount (fusermount3 -u mountpoint)
make test-unit            # test đơn vị, không cần mount
python3 -B -m unittest discover -s tests -p 'test_m0_*.py'   # test Python của M0
```

`make test-unit` đạt, và bộ test Python M0 cho kết quả **264 tests / OK** khi kiểm tra. Các con số này là kết quả của lần kiểm tra đó, không phải cam kết cho các phiên bản sau.

> **Cẩn thận với `make clean`:** lệnh này xóa cả `backing/*`, tức là toàn bộ dữ liệu đã lưu qua myfs, không chỉ binary. Hãy `make umount` trước và chỉ chạy khi chấp nhận mất dữ liệu thử.

---

## Hỗ trợ gì và chưa hỗ trợ gì

Phần này nói rõ myfs làm được gì, chưa làm được gì và những điều chưa ai kiểm chứng. Hãy đọc kỹ phần này trước khi cho một ứng dụng thật dùng myfs.

### Các thao tác đã có

Danh sách lấy từ bảng thao tác FUSE trong [src/main.c](src/main.c). "Đã có" nghĩa là thao tác đã được đăng ký, không đảm bảo mọi chi tiết giống hệt POSIX.

| Bạn làm gì | Thao tác FUSE | Ghi chú |
|---|---|---|
| Xem thuộc tính file | `getattr` | |
| Liệt kê thư mục | `readdir` | |
| Tạo file | `create`, `mknod` | |
| Mở file | `open` | |
| Đọc, ghi | `read`, `write` | |
| Đóng file | `release` | Xem lưu ý về `fsync` bên dưới |
| Cắt file | `truncate` | |
| Tạo, xóa thư mục | `mkdir`, `rmdir` | |
| Xóa file | `unlink` | |
| Đổi timestamp | `utimens` | **Chỉ giả vờ**: nhận yêu cầu rồi trả thành công mà không làm gì |
| Khởi tạo, dọn dẹp | `init`, `destroy` | `init` bật `kernel_cache`; `destroy` chờ hàng đợi công việc nền xử lý xong |

### Các thao tác chưa có

Nếu thao tác không được đăng ký, myfs không tự quyết định lỗi trả về. Hành vi cụ thể phụ thuộc vào kernel, libfuse và từng ứng dụng, nên các ghi chú dưới đây là **hậu quả có thể xảy ra**, chưa phải kết quả đã thử.

| Thiếu | Hậu quả có thể xảy ra |
|---|---|
| `rename` | Không đổi tên, không di chuyển được file trong mount. Ứng dụng kiểu "ghi file tạm rồi đổi tên để thay thế" có thể không chạy được |
| `fsync`, `flush`, `fsyncdir` | Ứng dụng muốn "ghi xuống đĩa chắc chắn" có thể không nhận được cam kết nào |
| `chmod`, `chown` | Không đổi quyền, chủ sở hữu |
| `symlink`, `link`, `readlink` | Không tạo, không đọc symlink hay hard link |
| `setxattr`, `getxattr`, `listxattr`, `removexattr` | Không có thuộc tính mở rộng |
| `statfs` | Các lệnh như `df` có thể không cho số liệu đúng |
| `access`, `opendir`, `releasedir`, `lock`, `flock`, `fallocate`, `copy_file_range`, `lseek`... | Chưa có callback riêng |

### Những điều chưa được kiểm chứng

Đợt đo M0 đã lập một danh sách các khoảng trống về "hợp đồng" (tức là myfs hứa gì với ứng dụng). Tài liệu M0 ghi rõ đây là **hành vi đọc được từ mã nguồn hoặc điều chưa kiểm chứng, không phải lỗi đã quan sát thấy**: chưa ai thấy mất dữ liệu hay lỗi sync xảy ra. Bản gốc ở [mục 6 của tài liệu M0](benchmarks/m0_evidence_finalization.md#6-compatibility-and-contract-gap-register).

| Mã | Chuyện gì | Cách hiểu |
|---|---|---|
| C01 | myfs không đăng ký `fsync` và `flush` | Chưa đo ứng dụng nhìn thấy gì khi gọi chúng. Đừng đoán mã lỗi |
| C02 | Khi đóng file, `release()` gọi `fsync` lên file dữ liệu nhưng **không kiểm tra kết quả**, rồi vẫn trả thành công. Kernel cũng bỏ qua giá trị trả về của `release` | Nếu `fsync` lỗi ở bước này, có thể ứng dụng không biết. Chưa ai quan sát lỗi như vậy |
| C03 | Mất điện hoặc crash giữa chừng | Mã có kiểm tra sync, xử lý metadata ghi dở và có phục hồi, nhưng **chưa có cam kết** ở mức cả hệ thống. M0 chỉ kiểm chứng việc tắt và mount lại bình thường |
| C04 | Nhiều tiến trình cùng thấy dữ liệu mới | Có test gọi trực tiếp hàm, nhưng chưa test qua mount thật với nhiều tiến trình |
| C05 | Thiếu `rename`, `statfs`, `chmod`, `symlink`, xattr | Ứng dụng nào dùng chúng còn chưa rõ có chạy được không |
| C06 | `utimens` không làm gì | Ứng dụng phụ thuộc timestamp có thể không thấy thay đổi |
| C07 | Hết dung lượng đĩa, hết RAM, hết file descriptor | Đường ghi có kiểm tra một số lỗi, nhưng hợp đồng đầy đủ khi lỗi chưa được xác lập |
| C08 | Chỉ thử một ứng dụng | Cơ sở dữ liệu, `mmap`, nhiều tiến trình cùng ghi nối đuôi, import/export... chưa được thử |

### Giới hạn trong cách cài đặt

- **Chunk legacy rất lớn** (định dạng cũ) vẫn có thể phải giải nén nguyên khối vào RAM khi đọc hoặc chuyển đổi. Chưa có đường giải nén từng phần.
- **Chỉ có một worker compaction** dùng hàng đợi FIFO. Chưa có bằng chứng một worker đủ cho mọi tải.

---

## Câu hỏi thường gặp

**myfs có dùng cho dữ liệu thật được chưa?**
Chưa. Đây là nguyên mẫu nghiên cứu. Chưa có cam kết về `fsync`, mất điện, hay nhiều tiến trình cùng truy cập. Xem [Hỗ trợ gì và chưa hỗ trợ gì](#hỗ-trợ-gì-và-chưa-hỗ-trợ-gì).

**Dữ liệu của tôi sẽ nhỏ đi bao nhiêu?**
Không có con số chung. Phụ thuộc vào loại dữ liệu. Số 173 MB xuống 37 MB trong M0 là của dữ liệu tổng hợp rất dễ nén. File đã nén sẵn (ảnh JPEG, video, file zip) gần như không nhỏ đi nữa. myfs có cơ chế bỏ qua nén cho một số loại file như vậy và chỉ giữ kết quả nén khi tiết kiệm được ít nhất khoảng 12,5%.

**Nếu mất điện giữa lúc ghi thì sao?**
Chưa có cam kết. myfs ghi dữ liệu và đồng bộ xuống đĩa **trước**, rồi mới ghi metadata trỏ tới nó, và có xử lý phần metadata ghi dở khi mở lại. Nhưng chưa ai chạy đợt test mất điện thật để chứng minh. Xem C03 ở trên.

**Tôi có thể sửa file trong `backing/` bằng tay không?**
Không nên. File dữ liệu và file metadata phải khớp nhau. Sửa một bên có thể làm hỏng file. Dùng công cụ `meta_inspect` (xem [Debug](#debug)) để chỉ **xem**.

**Có thể xóa `backing/` khi đang mount không?**
Không. Hãy `make umount` trước.

**Vì sao ghi đè một đoạn nhỏ giữa file chậm?**
Mỗi đoạn dữ liệu được nén riêng. Sửa một phần giữa đoạn nghĩa là phải đọc đoạn cũ, giải nén, sửa, nén lại rồi ghi (gọi là RMW, đọc-sửa-ghi). myfs giảm tác động này bằng cách tự điều chỉnh kích thước đoạn theo thói quen ghi của từng file. Xem [Window tự điều chỉnh](#window-tự-điều-chỉnh).

**myfs khác `zip` ra sao?**
`zip` tạo một file nén duy nhất, muốn dùng phải giải nén. myfs cho bạn một thư mục mà ứng dụng đọc ghi trực tiếp, việc nén diễn ra ngầm.

---

## Cách hoạt động

Phần này kể lại một lần ghi và một lần đọc bằng lời thường. Hiểu câu chuyện này là đủ để đọc hầu hết phần còn lại của tài liệu.

### Câu chuyện: ghi một file 200 KB

Giả sử bạn ghi một file mới 200 KB.

1. **Chia cửa sổ (window).** myfs chia địa chỉ logic của file thành các cửa sổ cố định, mặc định **64 KB** mỗi cửa sổ: 0–64 KB, 64–128 KB, 128–192 KB, 192–256 KB. File 200 KB nằm trong 4 cửa sổ, cửa sổ cuối chỉ có 8 KB.
2. **Nén từng cửa sổ.** Mỗi cửa sổ được nén riêng bằng Zstd, thành một khối gọi là **chunk**. Nếu nén không tiết kiệm được đáng kể thì lưu nguyên dạng.
3. **Nối vào file dữ liệu.** Các chunk được nối vào **cuối** một file dữ liệu (`data`). myfs không ghi chèn vào giữa.
4. **Ghi bản đồ.** Metadata ghi lại với từng chunk: nó ứng với đoạn nào của file, nằm ở đâu trong file dữ liệu, dài bao nhiêu byte, nén hay không, và một mã kiểm tra (checksum).
5. **Thứ tự quan trọng.** myfs ghi dữ liệu và `fdatasync` **trước**, rồi mới ghi metadata và `fdatasync`. Nhờ vậy bản đồ không bao giờ trỏ vào dữ liệu chưa được ghi.

### Câu chuyện: đọc lại

1. myfs tra bản đồ để biết cửa sổ cần đọc nằm ở đâu.
2. Đọc chunk từ file dữ liệu, kiểm tra checksum.
3. Giải nén (nếu có nén) và trả đúng khoảng byte được yêu cầu.

### Câu chuyện: sửa một đoạn giữa file

Bạn ghi đè 100 byte ở giữa cửa sổ thứ hai.

1. myfs đọc chunk cũ của cửa sổ đó và giải nén.
2. Sửa 100 byte trong bộ nhớ.
3. Nén lại và **nối chunk mới vào cuối** file dữ liệu. Chunk cũ vẫn nằm đó nhưng không còn được bản đồ trỏ tới, trở thành "rác".

Cách làm này gọi là RMW (đọc, sửa, ghi). Rác tích lũy dần, nên có một công việc nền gọi là **compaction** dọn lại: gom phần dữ liệu còn sống thành một bản mới gọn gàng.

### Generation: vì sao cần nhiều "phiên bản" của một file

Compaction không sửa file cũ tại chỗ. Nó tạo một **generation** mới (một cặp file dữ liệu và metadata đã dọn sạch), rồi đổi một liên kết `.current` trỏ sang đó. Vì sao làm vậy? Vì có thể đang có người mở file. Họ cứ tiếp tục dùng phiên bản cũ cho tới khi đóng, còn người mở sau sẽ dùng phiên bản mới. Khi không còn ai dùng phiên bản cũ, nó mới bị xóa. Việc xóa này gọi là GC (garbage collection, dọn rác), xem [Chạy nhiều luồng cùng lúc](#chạy-nhiều-luồng-cùng-lúc).

Compaction cũng là lúc myfs có thể đổi kích thước cửa sổ của file, vì đổi cửa sổ nghĩa là phải đóng gói lại toàn bộ dữ liệu theo bố cục mới.

### Bỏ qua nén khi không đáng

- myfs đoán nhanh qua vài byte đầu file ("magic bytes") để nhận ra định dạng đã nén sẵn, và bỏ qua nén cho chúng.
- Nếu đã thử nén, kết quả chỉ được giữ khi `kích thước nén < kích thước gốc − kích thước gốc / 8`, tức là phải tiết kiệm được khoảng 12,5% trở lên. Nếu không, lưu nguyên dạng.

### Các lớp trong mã nguồn

```text
Ứng dụng: open / read / write
        |
   Kernel (VFS)
        |
   Module FUSE trong kernel
        |
   src/main.c            ← đăng ký các thao tác FUSE, vòng đời worker
        |
   src/fuse_ops/         ← thao tác file và thư mục (file.c, dir.c)
        |
   src/core/             ← lõi: nén, I/O chunk, metadata, khóa, compaction
   src/guards/           ← kiểm tra điều kiện dữ liệu
        |
   Thư mục backing trên Linux
```

| Lớp | Việc chính |
|---|---|
| `main.c` | Đọc tham số mount, cấu hình, đăng ký thao tác, khởi động và dừng worker |
| `fuse_ops/` | Xử lý các thao tác file/thư mục, handle, cache metadata |
| `core/` | Nén (`compress.c`), đọc/ghi chunk (`chunkio.c`), bản đồ (`metadata.c`), khóa (`lock.c`), phiên bản và dọn rác (`compact.c`), đường dẫn (`path.c`) |
| `guards/` | Kiểm tra metadata, vùng dữ liệu, kết quả I/O |

---

## Dữ liệu nằm ở đâu trên đĩa

Phần này mô tả tên file thật trong `backing/`. Ví dụ dưới chỉ để minh họa, không phải lệnh.

```text
backing/
  file.txt.data              # file dữ liệu (cặp legacy) hoặc hard-link alias
  file.txt.meta              # file metadata (cặp legacy) hoặc hard-link alias
  file.txt.current           # symlink -> file.txt.g.<hex>  (chọn phiên bản đang dùng)
  file.txt.g.<hex>.owner     # dấu xác nhận generation này thuộc về myfs
  file.txt.g.<hex>/
    data
    meta
```

- File mới bắt đầu bằng cặp `.data` và `.meta`.
- Sau lần compaction đầu tiên, `.current` quyết định phiên bản nào đang hoạt động. Đổi `.current` dùng đổi tên symlink (một thao tác nguyên tử) rồi sync thư mục cha.
- `.data` và `.meta` giờ là **alias** (hard link) để giữ tương thích với cách đặt tên trước đây. Chúng **không** quyết định phiên bản đang hoạt động.
- Phiên bản cũ chỉ được xóa khi đã chắc chắn phiên bản mới được lưu bền và không còn ai tham chiếu. Khi khởi động lại, myfs cũng có thể đánh dấu các phiên bản không hoạt động thuộc sở hữu của nó để thu hồi. Xem [path.c](src/core/path.c) và [compact.c](src/core/compact.c). Thứ tự này là cách cài đặt, **chưa phải cam kết bền khi mất điện**.

---

## Định dạng lưu trữ chi tiết

Phần này dành cho người cần đọc hoặc sửa metadata. Người chỉ dùng thử có thể bỏ qua.

### Window và chunk

| Thuộc tính | Giá trị |
|---|---|
| Window của file mới | 64 KiB |
| Window hợp lệ | Lũy thừa của 2, từ 16 KiB đến 1 MiB |
| Khi tự điều chỉnh | Tăng hoặc giảm **một bước ×2** mỗi lần, và chỉ khi compaction |
| Bố cục "packed" | Tối đa một chunk mỗi window, chunk bắt đầu đúng đầu window |
| Khoảng trống (hole) | Window không có dữ liệu thì không tạo chunk. Đọc vào hole trả về byte 0 |
| Blob | Mỗi chunk nén riêng bằng Zstd hoặc lưu thô, được nối vào cuối file `data` |

Mỗi chunk trong metadata có các trường: `logical_offset`, `physical_offset`, `raw_size`, `stored_size`, `codec_type`, `flags`, `checksum`.

> **Chú ý tên trường dễ gây nhầm:** `raw_size` là số byte của chunk **trên đĩa**, còn `stored_size` là số byte **dữ liệu logic sau khi giải nén**. Tên và nghĩa ngược với trực giác. Xem chú thích trong [myfs.h](src/myfs.h).

`codec_type` bằng 0 là lưu thô, 1 là Zstd. `checksum` là CRC32 của chunk trên đĩa.

### Các phiên bản metadata

| Phiên bản | Ghi chú |
|---|---|
| v0 | Dạng cũ, không có tag phiên bản. Suy ra window 64 KiB. Được chuyển sang dạng có version trước lần thêm journal đầu tiên |
| v1 | Có header version, suy ra window 64 KiB |
| v2 | Lưu `window_size` riêng. Đây là định dạng mà mã hiện tại **ghi** |

v1 và v2 dùng header little-endian 64 byte và mục chunk 32 byte trên đĩa.

### Journal và checkpoint

Thay vì ghi lại toàn bộ bản đồ mỗi lần ghi, myfs **nối thêm** vào một **journal** các thay đổi nhỏ của bản đồ. Mỗi bản ghi có số thứ tự, vùng bản đồ bị thay, kích thước logic mới, các mục mới, mã CRC và dấu "commit" ở cuối. Khi mở file, myfs đọc bản đồ gốc rồi áp lại các bản ghi đã commit.

- Phần đuôi ghi dở (thiếu dữ liệu hoặc thiếu dấu commit) **được bỏ qua**.
- Bản ghi đầy đủ nhưng sai CRC hoặc sai số thứ tự thì **bị từ chối**.

Để journal không phình mãi, myfs định kỳ ghi lại toàn bộ bản đồ, gọi là **checkpoint**:

| Điều kiện (bản build thường) | Ngưỡng |
|---|---|
| Số bản ghi journal đã commit | Từ 1024 trở lên |
| Hoặc kích thước journal | Lớn hơn `max(1 MiB, 2 × (64 + 32 × số_chunk))` byte |
| Bản build có `MYFS_TEST_FAILPOINTS` (dùng cho test) | Ngưỡng số bản ghi hạ xuống 8 |

Checkpoint ghi ra file tạm, sync, đổi tên, rồi sync thư mục cha. Handle đang mở phải mở lại file metadata khi cần.

Ngoài ra myfs còn đọc được file có bố cục chunk cũ (không packed) bằng cách quét tuần tự. Compaction đóng gói lại toàn bộ nội dung thành một generation v2 có một window size duy nhất. Các handle có thể cùng lúc giữ nhiều generation có window size khác nhau mà không lẫn lộn.

---

## Đường đọc và đường ghi

Phần này liệt kê từng bước thật của mã. Nguồn: [file.c](src/fuse_ops/file.c), [chunkio.c](src/core/chunkio.c), [guards.c](src/guards/guards.c).

### Đọc

1. Mỗi handle giữ sẵn file descriptor, thông tin generation và bản đồ chunk. Khi đọc, myfs so **epoch** (số đếm trong bộ nhớ cho biết metadata đã đổi chưa) với cache của handle. Nếu cache còn hợp lệ và epoch không đổi thì **không đọc lại** file `.meta` và không gọi syscall metadata.
2. Nếu cần làm mới cache thì đi qua khóa đường dẫn rồi khóa ghi của cache. Đường đọc dùng cache hợp lệ giữ khóa đọc của cache suốt quá trình tra cứu, `pread`, giải nén và sao chép.
3. Tìm chunk: bố cục packed dùng **tìm kiếm nhị phân** theo đầu window. Bố cục cũ dùng quét tuần tự. Khi đọc vào hole, mã còn quét để tìm chunk kế tiếp.
4. Đọc blob, kiểm tra CRC32, giải nén (hoặc sao chép nếu thô), trả đúng vùng được yêu cầu. Không đảm bảo truy cập ngẫu nhiên O(1).

### Ghi và truncate

1. Lấy khóa đường dẫn, rồi khóa ghi của cache. Làm mới cache. Nếu handle đã bị đánh dấu stale thì từ chối ghi bằng `-ESTALE`.
2. Ghi nối đuôi, ghi đè và ghi vào hole đều làm cùng việc: xác định các window bị chạm tới rồi đóng gói lại. Ghi một phần thì giữ lại phần còn lại bằng RMW. Nối đuôi nhỏ có thể gộp vào chunk cuối.
3. Quyết định nén: xem [Bỏ qua nén khi không đáng](#bỏ-qua-nén-khi-không-đáng).
4. Nối blob mới, `fdatasync` file dữ liệu và kiểm tra lỗi. Rồi nối bản ghi journal (có CRC và commit), `fdatasync` file metadata và kiểm tra lỗi.
5. Sau khi commit: cập nhật cache và epoch, kiểm tra có cần checkpoint không, cập nhật thống kê cho window tự điều chỉnh. Việc **xếp lịch compaction được làm sau khi nhả khóa**. Nếu xếp lịch lỗi thì chỉ ghi log và khôi phục thống kê, **không đảo ngược** lần ghi đã thành công.

`truncate` xử lý bản đồ và phần chunk còn lại. Đường chunk chung hỗ trợ cắt giữa một chunk nén.

> **Lưu ý:** các lỗi sync trên đường ghi đều được kiểm tra. Nhưng `fsync` trong `release()` thì không (mục C02 ở trên).

---

## Chạy nhiều luồng cùng lúc

FUSE có thể gọi myfs từ nhiều thread cùng lúc, và còn có một thread nền làm compaction. Phần này nói về cách chúng không giẫm lên nhau. Người thay đổi code cần nắm các quy tắc này để không thêm thứ tự chờ khóa ngược chiều (dễ gây deadlock).

### Khóa là gì, ở đây có những khóa nào

**Khóa (lock)** giống chìa khóa phòng: ai cầm chìa thì được vào, người khác phải chờ. myfs có ba loại:

| Khóa | Bảo vệ cái gì |
|---|---|
| **Path lock** | Một đường dẫn logic. Chỉ một thao tác thay đổi trên cùng đường dẫn tại một thời điểm |
| **cache_lock** | Bộ nhớ đệm bên trong một handle (bản đồ chunk, file descriptor) |
| **Registry shard** | Sổ đăng ký các generation đang tồn tại và ai đang dùng chúng |

### Thứ tự lấy khóa (quy tắc bắt buộc)

| Trường hợp | Thứ tự |
|---|---|
| Thao tác thay đổi | path lock → `cache_lock` → registry shard |
| Đọc dùng cache còn hợp lệ | `cache_lock` → registry shard (không lấy path lock) |
| Chỉ thao tác trên registry | Được lấy riêng một shard. Khi đang giữ shard thì **không** được chờ path lock hay cache lock |
| Hàng đợi compaction | Nhả `compact_queue_mu` trước khi worker hoặc nhánh dự phòng chờ path lock |

Cache rwlock được tạo kiểu ưu tiên writer (`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`). Nếu không khởi tạo được thì việc gắn handle thất bại. Việc ưu tiên writer và các test hiện có **không** tạo ra cam kết về thời gian chờ tối đa.

### Chia nhỏ khóa để giảm tắc nghẽn

- **Path lock** là một bảng gồm **64 shard × 64 bucket**. Đường dẫn được băm (FNV-1a 64-bit, rồi trộn thêm bằng fmix64) để chọn shard và bucket. Mỗi shard có mutex riêng, nên các đường dẫn khác nhau thường không chờ nhau. Khi một entry hết người dùng thì được gỡ ngay.
- **Generation registry** cũng là bảng **64 × 64 riêng**, băm chỉ theo `logical_path`. Nhờ đó mọi generation của cùng một đường dẫn nằm trong cùng một shard, phục vụ việc chuyển handle sang phiên bản mới (xem bên dưới).

### GC hai pha: dọn phiên bản cũ mà không giữ khóa quá lâu

Xóa một generation cũ cần nhiều thao tác chậm trên đĩa (đọc symlink, đổi alias, xóa file, `fsync`). Nếu giữ khóa của registry trong suốt thời gian đó, mọi thao tác khác sẽ bị chặn. Vì vậy myfs chia làm ba bước:

1. **Claim (nhận việc).** Giữ shard rất ngắn: ghi lại "tôi đang dọn cái này" cùng một **số phiếu** (claim ID), trạng thái chuyển `NONE → PENDING → CLAIMED`.
2. **Làm việc nặng, không giữ shard.** Các thao tác chậm trên đĩa chạy bên ngoài registry shard, trong đường thông thường dưới path lock.
3. **Finalize (hoàn tất).** Giữ shard trở lại, **kiểm tra bản ghi, trạng thái và số phiếu còn khớp**. Khớp thì mới đánh dấu xong. Không khớp (hoặc bước 2 lỗi) thì trả về `PENDING` để thử lại.

Nhờ số phiếu, nếu có gì đó đổi giữa bước 1 và bước 3, bước 3 sẽ thấy và không làm hỏng trạng thái.

### Chuyển handle sang phiên bản mới (live handoff)

Khi compaction tạo ra generation mới trong lúc đang có handle mở để ghi, myfs "chuyển nhà" các handle ghi sang phiên bản mới trong một vùng được khóa. Compaction chuẩn bị và sync generation mới **trước**. Sau đó các handle ghi được sắp theo địa chỉ con trỏ rồi lấy khóa ghi của cache, rồi mới đổi con trỏ file descriptor, bản đồ và tham chiếu, và đóng bộ cũ. Nhả khóa theo thứ tự ngược lại. Nhờ vậy không ai thấy trạng thái nửa cũ nửa mới.

Handle chỉ-đọc giữ generation cũ cho tới khi đóng. Nếu publication ở trạng thái không rõ kết quả, myfs có thể hoàn tất việc chuyển, khôi phục con trỏ cũ, hoặc đánh dấu handle ghi là stale.

### Đóng file và công việc nền

Khi đóng file (`release()`), myfs gỡ handle, sync và đóng descriptor, rồi **chạy GC đủ điều kiện ngay (đồng bộ)** dưới path lock và khóa ghi của cache. Chỉ sau khi nhả cả hai khóa nó mới **xếp lịch** compaction. Có đúng **một worker** lấy việc từ hàng đợi FIFO (có khử trùng lặp). Khi `destroy()` chạy lúc unmount, nó chờ hàng đợi xử lý hết rồi mới hủy registry và bảng khóa. Nếu worker không chạy hoặc đang dừng, việc xếp lịch tự chuyển sang chạy compaction đồng bộ.

Nguồn: [lock.c](src/core/lock.c), [compact.c](src/core/compact.c), [file.c](src/fuse_ops/file.c).

---

## Các kỹ thuật tăng tốc

Phần này giải thích các cơ chế tránh làm lại việc thừa. Số đo hiệu quả nằm riêng ở phần [Benchmark](#benchmark).

### Tái dùng context Zstd

Tạo và hủy context nén mỗi lần gọi khá tốn. Mỗi thread tự tạo một `ZSTD_CCtx` (nén) và một `ZSTD_DCtx` (giải nén) khi cần, giữ lại bằng bộ nhớ riêng của thread (TLS), và giải phóng khi thread kết thúc. Context không chia sẻ giữa các thread nên không cần thêm khóa chung. Mã dùng mức nén mặc định của Zstd, và test đối chiếu từng byte với API một lần gọi.

Trước mỗi lần giải nén, myfs reset cả session lẫn các tham số "dính". Nếu Zstd báo lỗi, context tương ứng bị bỏ để lần sau tạo lại. Nếu không cấp phát được TLS hoặc context thì chuyển sang API một lần gọi ([compress.c](src/core/compress.c)).

### Buffer tạm dùng lại (scratch)

Thao tác đóng gói lại cần vài buffer lớn tạm thời. Thay vì `malloc`/`free` mỗi lần, mỗi thread giữ sẵn bốn buffer, mỗi buffer một vai trò, để các buffer sống cùng lúc không đè lên nhau. Buffer chỉ lớn lên (không co lại giữa các thao tác) và được giải phóng khi thread kết thúc ([chunkio_scratch.c](src/core/chunkio_scratch.c)).

| Vai trò | Giới hạn bộ nhớ giữ lại mỗi thread |
|---|---:|
| `RAW` | 1 MiB |
| `WINDOW` | 1 MiB |
| `CACHED` | 1 MiB |
| `COMP` | `ZSTD_COMPRESSBOUND(1 MiB)` = 1.052.672 byte |
| **Tổng khi đã dùng đủ bốn vai trò** | **4.198.400 byte mỗi thread** |

Nếu cần vượt giới hạn, TLS không dùng được, không mở rộng nổi, hoặc buffer cùng vai trò đang bận thì myfs cấp phát tạm rồi giải phóng khi xong. Giới hạn ở trên **không** tính các cấp phát tạm đó, cũng không tính các buffer khác (dữ liệu gốc của FUSE, buffer sao chép của compaction...).

### Window tự điều chỉnh

Window nhỏ thì ghi đè một phần rẻ hơn nhưng nén kém hơn. Window lớn thì ngược lại. Nên myfs quan sát thói quen ghi của từng file rồi tự đổi, theo các quy tắc trong [compact.c](src/core/compact.c):

| Quy tắc | Điều kiện |
|---|---|
| Mẫu tối thiểu | 128 sự kiện đủ điều kiện |
| **Thu nhỏ** window | Ghi một phần (partial RMW) chiếm từ 60% trở lên. Giảm một bước ×2 |
| **Tăng** window | Ghi trọn window chiếm từ 90% trở lên. Tăng một bước ×2 |
| Điều kiện chi phí thêm | `số_sự_kiện × kích_thước_bước ≥ tổng_dữ_liệu_còn_sống` (tính bão hòa) |
| Nghỉ sau khi đổi | 3600 giây (theo đồng hồ monotonic) |
| Trong khoảng nghỉ | Vẫn tích lũy thống kê. Lần ghi hoặc đóng đủ điều kiện sau khi hết nghỉ sẽ giảm đôi các bộ đếm một lần rồi đánh giá |

Các bộ đếm nằm trong bộ nhớ, thuộc từng generation, và **mất khi daemon khởi động lại**. Window chỉ đổi khi đóng gói sang generation mới, trong khoảng 16 KiB đến 1 MiB. Không trộn nhiều window size trong cùng một generation.

---

## Kiểm thử

Phần này chỉ ra bộ test nào kiểm tra phần nào và cái nào cần mount. Test gọi hàm trực tiếp không thay thế được việc kiểm chứng qua FUSE thật, và test chạy bình thường không chứng minh khả năng chịu mất điện.

### Các lệnh make

| Lệnh | Kiểm tra gì | Cần mount? | Đã chạy khi kiểm tra README? |
|---|---|---|---|
| `make test` | `test_suite.sh`: đọc/ghi, `O_TRUNC`, nối đuôi, xóa, file nhiều chunk, nén hay lưu thô, ghi đè qua biên chunk, hole, truncate chunk nén, thư mục lớn, compaction, migration, ghi song song | **Có** (mount sẵn) | Có: 84 PASS / 0 FAIL |
| `make test-unit` | Metadata, file/handle/cache, registry/GC, Zstd, scratch, khóa, `meta_inspect`; kiểm tra định dạng đầu ra của bốn microbenchmark | Không | Có |
| `make test-generation-registry` | Registry, GC hai pha, các tình huống đồng thời | Không | Đã chạy qua `test-unit` |
| `make test-chunkio-scratch` | Buffer scratch | Không | Đã chạy qua `test-unit` |
| `make test-lock` | Bảng path lock | Không | Đã chạy qua `test-unit` |
| `make tsan-file-ops` | File/handle/chuyển phiên bản dưới ThreadSanitizer | Không | Không |
| `make tsan-generation-registry` | Registry/GC dưới ThreadSanitizer | Không | Không |
| `make test-chunkio-scratch-tsan` | Scratch dưới ThreadSanitizer | Không | Không |
| `make test-lock-tsan` | Path lock dưới ThreadSanitizer | Không | Không |
| `make tsan-generation-registry-bench` | Kiểm tra ngắn benchmark registry/GC dưới ThreadSanitizer | Không | Không |
| `make help` | In danh sách target | Không | Có |

ThreadSanitizer (TSan) là công cụ tự động tìm lỗi data race giữa các thread. Các target TSan chưa được chạy lại cho lần kiểm tra README này.

### Giới hạn của `make test`

- Mục kiểm tra cache chỉ đọc lặp lại một file. Nó không unmount rồi mount lại và không ép cache nguội.
- Mục kiểm tra append kiểm tra nội dung và thời gian. Nó **không** thử mất điện. Ghi chú thủ công trong script không tạo ra cam kết nào về mất điện.
- Việc xác minh sau khi mount lại của M0 là bằng chứng riêng, nằm trong [tài liệu M0](benchmarks/m0_evidence_finalization.md).

### Hook dùng riêng cho test

Macro `MYFS_TEST_FAILPOINTS` bật các "hook" cho phép test ép lỗi, quan sát trạng thái và điều khiển thứ tự chạy của các thread. Các hook này **không** được biên dịch vào binary `myfs` của `make release`. Đây là tùy chọn dành cho test, không phải tùy chọn của người dùng. Xem [compact_test.h](src/core/compact_test.h), [lock_test.h](src/core/lock_test.h), [chunkio_scratch.h](src/core/chunkio_scratch.h), [Makefile](Makefile).

### Test của M0

```bash
python3 -B -m unittest discover -s tests -p 'test_m0_*.py'
```

Các test M0 chạy ngoại tuyến, kiểm tra bộ tạo dữ liệu, bộ kiểm tra đầu ra (oracle), runner và các phép tính. Chúng dùng bản mô phỏng của mount nên **không** phải bằng chứng về FUSE thật.

---

## Benchmark

Phần này phân biệt hai loại phép đo: đo riêng một cơ chế (microbenchmark) và đo cả ứng dụng qua mount. Các con số lịch sử được giữ nguyên kèm nguồn và giới hạn. Không có benchmark dài nào được chạy lại khi viết lại README.

### Có những benchmark nào

| Nguồn | Lệnh | Đo gì | Cần mount? |
|---|---|---|---|
| [zstd_context_bench.c](benchmarks/zstd_context_bench.c) | `make bench-zstd-context` | Gọi Zstd một lần so với tái dùng context | Không |
| [chunkio_scratch_bench.c](benchmarks/chunkio_scratch_bench.c) | `make bench-chunkio-scratch` | `malloc/free` so với buffer scratch | Không |
| [lock_table_bench.c](benchmarks/lock_table_bench.c) | `make bench-lock-table` | Bảng khóa cũ (tuyến tính) so với bảng khóa chia shard | Không |
| [generation_registry_bench.c](benchmarks/generation_registry_bench.c) | `make bench-generation-registry` | Registry và GC thật, kèm I/O thật | Không |
| [benchmark.sh](benchmark.sh) | `make bench` | Mười phần BM01 đến BM10 qua mount, có đối chứng | **Có** |
| [m0_profile.py](benchmarks/m0_profile.py) | Chưa có lệnh make | Vòng đời và phép đo ứng dụng của M0 | Tùy backend |

Các lệnh benchmark ở bảng trên **chưa được chạy lại** khi viết README. `make test-unit` chỉ chạy các kiểm tra ngắn về định dạng đầu ra của benchmark, không tạo ra số đo hiệu năng mới.

Công cụ phụ trợ:

| Lệnh | Mục đích |
|---|---|
| `make build-generation-registry-bench` | Build benchmark với lõi hiện tại |
| `make build-generation-registry-baseline BASELINE_TREE=... BASELINE_OUTPUT=...` | Build cùng nguồn benchmark nhưng liên kết với lõi ở một cây khác (để làm baseline) |
| `make compare-generation-registry-bench ...` | So sánh bốn kết quả (chạy thường và A/A) |
| `make benchmarks/meta_inspect` | Build công cụ đọc metadata |

`benchmark.sh` ghi kết quả vào `benchmark_results.txt` trên máy của bạn (không nằm trong Git). BM08 tạo checksum và script để bạn tự mount lại và kiểm tra, nó không tự remount. Các câu kết luận về "bottleneck" mà BM09 in ra chỉ là suy luận của script dựa trên so sánh throughput, **chưa phải phép đo nhân quả**. Hãy dùng kết luận thận trọng của [mục 7 tài liệu M0](benchmarks/m0_evidence_finalization.md#7-measured-cost-and-bottleneck-disposition).

### Cách đọc số đo cho đúng

- **Ghi lại điều kiện đo:** commit, trình biên dịch, CPU, kernel, filesystem thật, cache, dữ liệu. Gắn (pin) thread vào CPU cụ thể, và để ý các CPU anh em SMT.
- **Luôn có đối chứng (control) và chạy A/A:** A/A nghĩa là chạy hai lần cùng một binary để biết bản thân phép đo dao động bao nhiêu. Chỉ tin sự khác biệt lớn hơn mức dao động đó.
- **Máy không pin CPU nhiễu rất nhiều.** Pin cũng chưa loại hết nhiễu (preemption, tần số CPU thay đổi, trôi giữa các lần chạy).
- **Đừng gọi một lần chạy là "cache nguội"** nếu việc thả cache thất bại.
- **GC chạy liên tục là tải để làm khó, không đại diện cho đời thực.** Tỷ số throughput của microbenchmark không phải mức tăng tốc của cả ứng dụng FUSE.
- `ext` với magic `0xef53` chưa phân biệt được ext2, ext3 và ext4. Campaign dưới đây đã xác nhận bằng cách khác là ext4.

### Kết quả: registry và GC (2026-10-09)

Dữ liệu thô nằm trong Git: [raw outputs](benchmarks/results/2026-10-09-generation-registry-pooled/), [manifest](benchmarks/results/2026-10-09-generation-registry-pooled/manifest.md), [SHA256SUMS](benchmarks/results/2026-10-09-generation-registry-pooled/SHA256SUMS). Gồm 8 file, mỗi file 15 dòng tóm tắt các lần lặp.

Bốn workload:

| Workload | Đo gì |
|---|---|
| `control` | Vòng tính toán CPU thuần, không gọi registry hay GC |
| `scalability` | Đăng ký, chụp, tăng epoch, hủy đăng ký, mỗi thread một đường dẫn riêng |
| `hot-path` | Cùng thao tác trên **một đường dẫn chung** (trường hợp xấu nhất của chia shard) |
| `gc-interference` | Các thao tác nhẹ trên đường dẫn không liên quan, trong khi một thread tạo và thu hồi generation thật liên tục |

Điều kiện đo giữ nguyên: cây ứng viên `743c2b467041994cc64dbcc2714a8952fe406f1e`; baseline là `origin/main` lúc đó (`7f8f335b7a00fc4d928475fa9d947d3a3187b00a`, dùng một mutex toàn cục và giữ nó trong lúc GC làm I/O); máy Intel i5-12450H, Linux `6.17.0-41-generic`, ext4; 1/2/4 thread; 7 lần lặp mỗi dòng; seed 25228. Set A pin foreground vào CPU `0,2,4,6` và GC vào CPU `8`; set B dùng `1,3,5,7` và `9`. Các cặp `0/1`, `2/3`, `4/5`, `6/7` là anh em SMT, nên hai set **không** phải hai nhóm core vật lý độc lập.

**Throughput** (thao tác mỗi giây, cộng cả các thread; hệ số = mới / cũ, lớn hơn 1 là mới nhanh hơn):

| Workload | Set / số thread | Baseline | Mới | Hệ số |
|---|---|---:|---:|---:|
| Đường dẫn riêng | A / 2 | 517.975 | 1.452.712 | 2,805× |
| Đường dẫn riêng | A / 4 | 433.616 | 2.620.246 | 6,043× |
| Đường dẫn riêng | B / 2 | 507.407 | 1.446.853 | 2,851× |
| Đường dẫn riêng | B / 4 | 446.372 | 2.595.749 | 5,815× |
| Một đường dẫn chung | A / 4 | 470.971 | 447.765 | 0,951× |
| Một đường dẫn chung | B / 4 | 465.890 | 452.790 | 0,972× |

**Đọc bảng:** khi nhiều thread dùng các đường dẫn khác nhau, bản mới mở rộng tốt hơn rõ rệt. Khi mọi thread dùng chung **một** đường dẫn, bản mới chậm hơn khoảng 3% đến 5%. Riêng set B / 4 thread không vượt mức dao động A/A của chính dòng đó, nên đừng coi mọi chênh lệch nhỏ là hồi quy đã được chứng minh.

**Độ trễ khi GC chạy** (4 thread foreground, p99.9 tính bằng micro giây). Đây là median của percentile từng lần lặp, không phải percentile gộp tất cả:

| Set | Kiểu I/O | Baseline | Mới |
|---|---|---:|---:|
| A | I/O thật trên ext4 | 8,732 | 3,663 |
| A | Thêm độ trễ giả lập, vẫn trên ext4 | 8,951 | 3,696 |
| B | I/O thật trên ext4 | 11,274 | 3,236 |
| B | Thêm độ trễ giả lập, vẫn trên ext4 | 11,901 | 3,030 |

**Số thao tác bị chậm hơn 1 ms** (gộp 7 lần lặp, 4 thread foreground):

| Trường hợp | Cây | Số thao tác chậm / tổng thao tác | Tỷ lệ gộp | Chậm mỗi chu kỳ GC |
|---|---|---:|---:|---:|
| A, I/O thật | Baseline | 623 / 3.824.159 | 0,000162912 | 3,64 |
| A, I/O thật | Mới | 16 / 7.683.635 | 0,000002082 | 0,11 |
| B, độ trễ giả lập | Baseline | 662 / 2.908.335 | 0,000227622 | 3,34 |
| B, độ trễ giả lập | Mới | 1 / 14.037.798 | 0,000000071 | 0,0068 |

Ý nghĩa: ở bản baseline, mỗi chu kỳ GC chặn trung bình hơn 3 thao tác không liên quan trên 1 ms. Ở bản mới, con số này giảm mạnh. Nhưng bản mới **vẫn còn** vài thao tác vượt 1 ms.

**Cẩn thận khi diễn giải:**

- Control (không có GC, không có registry) ở baseline set A / 4 thread cũng có **9 trên 5.600.000** thao tác vượt 1 ms. Tức là một phần các "cú trễ" đến từ chính hệ điều hành (bị chuyển ngữ cảnh...), không phải GC. Đừng quy mọi cú trễ cho GC.
- `noise_floor_pct=0` trong lần chạy thường **không có nghĩa** nhiễu bằng 0. Mức nhiễu thật phải xem ở các lần chạy A/A. A/A max spread: 29,07% (set A) và 53,58% (set B).
- Thư viện benchmark không đo được thời gian giữ khóa của registry, nên không diễn giải tỷ số throughput thành mức tăng tốc của cả ứng dụng FUSE.

### Kết quả lịch sử: scratch và Zstd

**Trạng thái bằng chứng:** các số dưới đây được chép từ README cũ. Repo **không lưu output gốc**, nên chưa xác minh lại độc lập. Hãy xem chúng là tham khảo, không phải cam kết hiệu năng.

README cũ ghi một lần chạy scratch (7 lần lặp, ngày 2026-10-04). Tỷ số là "dùng scratch TLS so với malloc/free":

| Workload | 1 thread | 8 thread | Bộ nhớ giữ lại mỗi thread |
|---|---:|---:|---:|
| RAW 64 KiB | 1,70× | 1,85× | 65.536 B |
| COMP 64 KiB | 1,73× | 1,70× | 65.824 B |
| Partial RMW 64 KiB | 2,23× | 2,17× | 196.896 B |
| Thu nhỏ window 1 MiB xuống 512 KiB | 34,47× | 76,49× | 3.147.776 B |
| Trộn lẫn 16 KiB đến 1 MiB | 2,37× | 2,21× | 3.149.824 B |
| Giữ lại tối đa | 43,51× | 77,67× | 4.198.400 B |
| Vượt giới hạn (fallback) | 1,03× | 0,99× | 0 B |

README cũ cũng ghi tái dùng context Zstd nhanh hơn gọi một lần: **1,27× đến 2,50×** (một thread, buffer 1 đến 64 KiB) và **1,41× đến 1,67×** (8 thread, buffer 4 KiB). Câu "không thấy tranh chấp khóa" trong README cũ chưa thể kiểm chứng lại từ dữ liệu gốc.

Một lần so sánh `benchmark.sh` (5 lần xen kẽ, cache nguội có xác minh) cho thấy kết quả **chưa kết luận được**: mức dao động của đối chứng ext4 (211 đến 461 MB/s) lớn hơn ngưỡng 5%. Số **1550 MB/s** cho tốc độ giải nén từng xuất hiện trong bảng quyết định cũ nhưng không có phép đo kèm theo, nên **không** dùng nó để chọn codec.

### Tái lập

Kiểm tra dữ liệu gốc chưa bị sửa (thao tác chỉ đọc):

```bash
(cd benchmarks/results/2026-10-09-generation-registry-pooled && sha256sum --check SHA256SUMS)
```

Đo lại cần chọn CPU hợp lệ cho máy của bạn và xác nhận filesystem là ext4, không phải tmpfs. Nếu checkout hiện tại khác commit `743c2b4`, đây là một phép so sánh **mới**, không phải khôi phục đúng binary của campaign cũ.

<details>
<summary>Script so sánh registry/GC với baseline cố định (chưa chạy lại khi viết README)</summary>

Script này chạy benchmark dài và tạo git worktree. CPU trong script là cấu hình của máy đã đo, phải đổi theo topology của máy bạn.

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
# Xác nhận ext4 trước khi tiếp tục; không dùng tmpfs.
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

---

## Công cụ đo M0

M0 là đợt đo đạc đầu tiên của dự án. Phần này giới thiệu bộ công cụ đã dùng. Có một chế độ self-test ngoại tuyến cho phép thử runner mà không cần mount hay chạy lại đợt đo lịch sử.

| File | Vai trò |
|---|---|
| [m0_fixture.py](benchmarks/m0_fixture.py) | Tạo dữ liệu CSV theo seed cố định (kết quả luôn giống nhau), tạo manifest và hash, và có một "oracle" kiểm tra đầu ra độc lập với mã của ứng dụng. Có sẵn một ứng viên dữ liệu entropy cao (SHAKE-256), chưa đo khả năng nén |
| [m0_csv_workflow.py](benchmarks/m0_csv_workflow.py) | Ứng dụng CSV dùng thư viện chuẩn của Python. Đọc và ghi qua đường dẫn thường, từ chối ghi đè đầu ra đã có |
| [m0_profile.py](benchmarks/m0_profile.py) | Dòng lệnh: `prepare`, `run`, `verify`, `cleanup`, `self-test`, cùng `canonical-prepare`, `measure-prepare`, `measure-run`, `measure-self-test`. Quản lý danh tính, hạn chót, log, nguồn gốc binary, kiểm tra |
| [m0_daemon_sync_diagnostic.c](benchmarks/m0_daemon_sync_diagnostic.c) | Bản chẩn đoán link riêng: bọc các lời gọi sync hoặc I/O xuống đĩa của daemon, ghi lại thời gian từng lời gọi, xuất trace có giới hạn kích thước |
| [test_m0_profile.py](tests/test_m0_profile.py) | Test bộ tạo dữ liệu và oracle |
| [test_m0_runner.py](tests/test_m0_runner.py) | Test runner, quyền sở hữu, dọn dẹp, phép đo, chẩn đoán và các tình huống lỗi, đều chạy ngoại tuyến |

Bản chẩn đoán bằng C **không** nằm trong build `myfs` thông thường. Hai chế độ chẩn đoán (sync và I/O) được link riêng và không bật cùng nhau. Trace không đại diện cho thời gian đĩa thực sự phục vụ hay thời gian ứng dụng phải chờ.

Self-test ngoại tuyến (dùng thư mục mới, không dùng trực tiếp thư mục chung như `/tmp`):

```bash
m0_check_dir=$(mktemp -d /tmp/myfs-readme-self-test.XXXXXX)
python3 -B benchmarks/m0_profile.py self-test --parent "$m0_check_dir" --records 100
```

Kết quả mong đợi: `passed: true` và dòng ghi nhãn là kiểm tra đúng ngoại tuyến trên thư mục thường. **Đây không phải benchmark ext4 hay FUSE.** Backend `myfs` của runner cần được chỉ định rõ binary và nguồn gốc của nó, và không tự chuyển sang thư mục thường nếu mount lỗi.

[Tài liệu chấp thuận M0](benchmarks/m0_evidence_finalization.md) là nguồn đầy đủ về phạm vi, các đợt đo và quyết định. **Bằng chứng gốc nằm trên máy đã chạy đo, không có trong repo.** Dữ liệu CSV tạm của các đợt đo và một binary chẩn đoán cũ không còn nữa. Muốn tái lập phải tạo lại dữ liệu và danh tính mới theo quy trình của runner.

---

## Cấu trúc thư mục repo

Danh sách dưới đây lấy từ `git ls-files`, nên một bản clone mới sẽ không có sẵn binary hay kết quả đo nằm ngoài Git.

| Đường dẫn (được Git theo dõi) | Nội dung |
|---|---|
| `README.md`, `Makefile`, `.gitignore` | Tài liệu, lệnh build/test, quy tắc bỏ qua file |
| `plan.md` | Kế hoạch kỹ thuật cũ về cache, journal và window thích nghi. **Không** phải lộ trình M0 đến M5 |
| `src/main.c`, `src/myfs.h` | Điểm vào chương trình, kiểu dữ liệu và giao diện chung |
| `src/fuse_ops/file.c`, `dir.c` | Thao tác file và thư mục |
| `src/core/path.c`, `metadata.c`, `compress.c`, `chunkio.c` | Đường dẫn, định dạng, nén, I/O chunk |
| `src/core/chunkio_scratch.c`, `.h`, `lock.c`, `compact.c` | Buffer scratch, khóa, registry, GC, compaction |
| `src/core/compact_test.h`, `lock_test.h` | Khai báo hook dùng riêng cho test |
| `src/guards/guards.c`, `guards.h` | Kiểm tra điều kiện dữ liệu |
| `test_suite.sh`, `tests/` | Bộ test có mount, test C, shell, Python |
| `benchmark.sh`, `benchmark_helpers.sh` | Benchmark qua mount và hàm hỗ trợ |
| `benchmarks/` | Bốn microbenchmark, `meta_inspect.c`, script build/so sánh registry, công cụ M0 |
| `benchmarks/fixtures/lock_table_linear.c` | Bản bảng khóa tuyến tính đã "đóng băng" để làm baseline |
| `benchmarks/results/2026-10-09-generation-registry-pooled/` | Tám output gốc, manifest và checksum |
| `benchmarks/m0_evidence_finalization.md` | Phạm vi bằng chứng và quyết định chấp thuận M0 |

**Có trên máy bạn nhưng không được Git theo dõi:** thư mục `backing/`, `mountpoint/`, `benchmark-results/` (bị ignore); binary `myfs`, các binary test và microbenchmark, `benchmark_results.txt` (sinh ra khi chạy). Lộ trình đầy đủ của dự án cũng chỉ nằm trên máy của chủ dự án và bị ignore, nên README không liên kết tới nó.

---

## Debug

Phần này giúp bạn xem log và đọc metadata để biết điều gì đang xảy ra. Hãy dùng công cụ đọc metadata có sẵn thay vì tự đoán các byte trên đĩa.

### Xem log

Macro `LOG` trong [myfs.h](src/myfs.h) ghi ra stderr kèm giờ dạng `[HH:MM:SS.mmm]`, và vẫn có trong build release. Chạy `make run` ở foreground để xem lỗi đọc ghi, checkpoint, compaction và GC.

### Đọc metadata

Sau khi chạy ví dụ ở [Bắt đầu nhanh](#bắt-đầu-nhanh):

```bash
ls -la backing/readme-example.txt*
make benchmarks/meta_inspect
./benchmarks/meta_inspect backing/readme-example.txt.meta
hexdump -C backing/readme-example.txt.meta
```

`meta_inspect` in ra các trường phân cách bằng tab, như `chunks=N`, `logical_size=N`, `raw_chunks=N`. Nó đọc được v0/v1/v2 và áp lại journal bằng đúng bộ phân tích của mã thật.

Lưu ý: khi có `.current`, các alias `.data`/`.meta` có thể vẫn trỏ vào dữ liệu cũ nếu vẫn còn handle legacy đang mở. Muốn xem phiên bản đang hoạt động thì đối chiếu `.current` với file `meta` nằm bên trong thư mục generation.

### Bảng triệu chứng

| Triệu chứng | Nơi kiểm tra |
|---|---|
| Mount không chạy | Thiết bị và quyền FUSE; thư mục mountpoint và backing đã tồn tại chưa; stderr in ra từ `main.c` |
| `-EIO` khi đọc | Metadata, CRC, frame nén trong `chunkio.c`, `compress.c`, `guards.c`. Đừng mặc định gán cho một nguyên nhân |
| `-ESTALE` khi ghi | Trạng thái generation và đường publication hoặc chuyển phiên bản trong `file.c`, `compact.c` |
| Dung lượng trong `backing/` chưa giảm ngay sau khi đóng | Còn tham chiếu sống, chưa đủ điều kiện compaction hoặc GC. `release()` chạy GC đồng bộ nhưng compaction thường chỉ được xếp hàng |
| Số benchmark mỗi lần một khác | CPU, cache, tải máy, đối chứng/A/A, vùng tính giờ, commit. Xem [Cách đọc số đo cho đúng](#cách-đọc-số-đo-cho-đúng) |

---

## Quyết định thiết kế

Mỗi dòng nêu lựa chọn, lý do và phạm vi hiện tại. Lý do mô tả cơ chế đang dùng, không thay thế được benchmark hay các cam kết tương thích còn thiếu.

| Quyết định | Lý do và phạm vi |
|---|---|
| FUSE thay vì kernel module | Code và log chạy ở user space qua libfuse, thuận tiện quan sát và debug |
| Window theo từng file, 16 KiB đến 1 MiB | Giảm khi ghi một phần nhiều, tăng khi ghi trọn window nhiều; chỉ đổi khi đóng gói sang generation mới |
| Zstd cho payload | Mã dùng Zstd, có tái dùng context và cơ chế lưu thô. **Chưa** có benchmark trong Git chứng minh Zstd nén tốt nhất so với các codec khác |
| Thư mục với `.data` và `.meta` | Tách dữ liệu và bản đồ để kiểm tra bằng công cụ; generation thêm `.current`, thư mục riêng và alias |
| Blob chỉ nối thêm (append-only) | Ghi blob mới trước khi metadata trỏ tới nó. Đổi tên nguyên tử dùng cho bước công bố metadata/generation, không tự biến việc nối thêm thành một giao dịch nguyên tử |
| Journal delta cộng checkpoint | Chỉ ghi phần bản đồ đổi, định kỳ ghi lại toàn bộ; khi tải, bỏ qua phần đuôi chưa commit |
| Thứ tự khóa path → cache → registry | Thao tác thay đổi lấy theo thứ tự này; đọc dùng cache → registry |
| Thread nền cho compaction | `release()` chạy GC đồng bộ rồi mới xếp lịch compaction; hàng đợi khử trùng lặp, được xả hết khi unmount, có nhánh dự phòng đồng bộ |
| rwlock ưu tiên writer cho cache | Bảo vệ việc chuyển handle; đọc hợp lệ không gọi syscall metadata. Không cam kết thời gian chờ tối đa |
| Context Zstd theo thread | Tái dùng mà không chia sẻ và không thêm khóa chung |
| Scratch TLS bốn vai trò, có giới hạn | Bốn buffer cùng tồn tại được; dự phòng cấp phát tạm khi lồng nhau, quá lớn hoặc lỗi |
| Bảng path lock 64 × 64 | Tra theo bucket, mutex theo shard, thu hồi ngay khi hết tham chiếu |
| Registry generation 64 × 64 và GC hai pha | Gom các generation cùng đường dẫn vào một shard; I/O xóa chạy ngoài khóa registry |

Nguồn tương ứng: [main.c](src/main.c), [file.c](src/fuse_ops/file.c), [metadata.c](src/core/metadata.c), [compact.c](src/core/compact.c), [compress.c](src/core/compress.c), [chunkio_scratch.c](src/core/chunkio_scratch.c), [lock.c](src/core/lock.c).

---

## Từ điển thuật ngữ

Các thuật ngữ được xếp theo nhóm. Mỗi mục giải thích bằng lời thường và nói nó là gì trong myfs.

### Khái niệm cơ bản

| Thuật ngữ | Nghĩa |
|---|---|
| **FUSE** | Cơ chế của Linux cho phép viết filesystem như một chương trình bình thường (user space) thay vì viết trong kernel. Dễ viết và dễ debug hơn, nhưng mỗi thao tác phải đi qua lại giữa kernel và chương trình nên có chi phí |
| **mountpoint** | Thư mục bạn thao tác. Nó "gắn" myfs vào đó |
| **backing directory** | Thư mục thật trên đĩa nơi myfs cất dữ liệu đã nén |
| **metadata** | File "bản đồ" cho biết mỗi đoạn dữ liệu nằm ở đâu |
| **Zstd** | Thuật toán nén dữ liệu |
| **checksum / CRC32** | Một con số tính từ dữ liệu, dùng để phát hiện dữ liệu bị hỏng |

### Cách tổ chức dữ liệu

| Thuật ngữ | Nghĩa |
|---|---|
| **Window** | Một khoảng cố định của file, mặc định 64 KiB. myfs chia file thành các window rồi nén từng window riêng |
| **Chunk** | Phần dữ liệu của một window sau khi nén (hoặc lưu thô), kèm một mục trong bản đồ |
| **Blob** | Chuỗi byte của một chunk khi nằm trong file dữ liệu |
| **Hole** | Khoảng của file chưa từng được ghi. Đọc vào đó trả về toàn byte 0 |
| **Generation** | Một phiên bản lưu trữ của file (một cặp file dữ liệu và metadata). Compaction tạo ra generation mới để không phải sửa file cũ tại chỗ |
| **Alias** | Tên phụ trỏ cùng tới một file, ở đây là hard link `.data` và `.meta` để giữ tương thích |
| **Journal** | Nhật ký nối thêm các thay đổi nhỏ của bản đồ, thay vì ghi lại toàn bộ mỗi lần |
| **Checkpoint** | Ghi lại toàn bộ bản đồ để bắt đầu một nhật ký mới, tránh journal quá dài |

### Hoạt động bên trong

| Thuật ngữ | Nghĩa |
|---|---|
| **RMW (read-modify-write)** | Sửa một phần dữ liệu đã nén: đọc, giải nén, sửa, nén lại, ghi lại |
| **Compaction** | Công việc nền gom dữ liệu còn sống thành một generation mới gọn gàng và có thể đổi window size |
| **GC (garbage collection)** | Thu hồi, xóa generation cũ khi không còn ai dùng |
| **Epoch** | Con số đếm trong bộ nhớ, tăng khi metadata của một generation đổi. Dùng để biết cache của handle có còn đúng không |
| **Handle** | "Phiếu" đại diện cho một lần mở file |
| **Handoff** | Chuyển một handle đang mở sang phiên bản lưu trữ mới trong một bước có khóa |
| **Claim** | Lấy quyền xử lý một việc, kèm số phiếu (claim ID) để bước hoàn tất kiểm tra đúng việc mình nhận |
| **Stale** | Handle còn trỏ vào dữ liệu cũ không còn hợp lệ. Ghi vào handle này bị từ chối (`-ESTALE`) |

### Chạy nhiều luồng

| Thuật ngữ | Nghĩa |
|---|---|
| **Thread** | Một luồng chạy việc. FUSE gọi myfs từ nhiều thread cùng lúc |
| **Lock (khóa)** | Cơ chế để chỉ một thread được vào một vùng nhất định tại một thời điểm |
| **rwlock** | Khóa cho phép nhiều thread đọc cùng lúc nhưng chỉ một thread ghi |
| **Shard** | Một phần của một bảng lớn được chia nhỏ, mỗi phần có khóa riêng để các thread ít phải chờ nhau |
| **TLS (thread-local storage)** | Bộ nhớ riêng của mỗi thread, các thread khác không đụng vào |
| **Deadlock** | Hai thread đợi nhau mãi vì mỗi bên giữ một khóa mà bên kia cần |
| **ThreadSanitizer (TSan)** | Công cụ tự động tìm lỗi "data race" giữa các thread |

### Đo đạc

| Thuật ngữ | Nghĩa |
|---|---|
| **Benchmark** | Phép đo hiệu năng |
| **Baseline** | Phiên bản làm mốc để so sánh |
| **Control (đối chứng)** | Một phép đo không liên quan đến thứ đang thử, để biết máy có ổn định không |
| **A/A** | Chạy hai lần cùng một binary để biết bản thân phép đo dao động bao nhiêu |
| **p99.9** | Mức độ trễ mà 99,9% thao tác nhanh hơn. Dùng để nhìn các cú chậm hiếm gặp |
| **Pin CPU** | Gắn một thread vào một CPU cụ thể để giảm nhiễu |
| **SMT** | Một core vật lý chạy hai thread phần cứng, chúng dùng chung tài nguyên nên có thể ảnh hưởng nhau |
| **Oracle** | Chương trình kiểm tra đầu ra đúng, viết độc lập với mã đang được kiểm tra |
| **M0 đến M5** | Các mốc của lộ trình dự án (xem [Lộ trình](#lộ-trình)) |

---

## License

Repo **chưa có file `LICENSE`**, nên hiện chưa nêu được điều khoản cấp phép.

License: chưa xác định.