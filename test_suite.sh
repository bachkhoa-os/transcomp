#!/bin/bash
# =============================================================================
# test_suite.sh — Regression test suite cho myfs (transparent compression)
# Project No: 25228 — Course: Operating Systems [Multimedia] - 20252
#
# Cách dùng:
#   ./test_suite.sh <mountpoint> <backing_dir>
#
# Ví dụ:
#   ./myfs -f mountpoint ./backing &   # terminal 1
#   ./test_suite.sh mountpoint backing  # terminal 2
#
# Script tự động mount/unmount KHÔNG được xử lý ở đây — FUSE process phải
# đang chạy trước khi gọi script này.
# =============================================================================

MOUNT="${1:-mountpoint}"
BACKING="${2:-backing}"
PASS=0
FAIL=0
TOTAL=0

# ── màu sắc ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# ── helpers ───────────────────────────────────────────────────────────────────
ok() {
    echo -e "  ${GREEN}[PASS]${NC} $1"
    PASS=$((PASS + 1))
    TOTAL=$((TOTAL + 1))
}

fail() {
    echo -e "  ${RED}[FAIL]${NC} $1"
    echo -e "         expected: ${YELLOW}$2${NC}"
    echo -e "         got:      ${YELLOW}$3${NC}"
    FAIL=$((FAIL + 1))
    TOTAL=$((TOTAL + 1))
}

section() {
    echo ""
    echo -e "${CYAN}==========================================${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}==========================================${NC}"
}

# assert_eq <label> <expected> <actual>
assert_eq() {
    if [ "$2" = "$3" ]; then
        ok "$1"
    else
        fail "$1" "$2" "$3"
    fi
}

# assert_file_eq <label> <expected_file_or_string> <actual_file>
assert_content() {
    local label="$1"
    local expected="$2"
    local actual
    actual=$(cat "$3" 2>/dev/null)
    if [ "$expected" = "$actual" ]; then
        ok "$label"
    else
        fail "$label" "$expected" "$actual"
    fi
}

# assert_diff <label> <file_a> <file_b>  — bit-by-bit compare
assert_diff() {
    if diff -q "$2" "$3" > /dev/null 2>&1; then
        ok "$1"
    else
        fail "$1" "files identical" "files differ"
    fi
}

# assert_exists <label> <path>
assert_exists() {
    if [ -e "$2" ]; then
        ok "$1"
    else
        fail "$1" "exists" "not found: $2"
    fi
}

# assert_not_exists <label> <path>
assert_not_exists() {
    if [ ! -e "$2" ]; then
        ok "$1"
    else
        fail "$1" "not exists" "found: $2"
    fi
}

cleanup() {
    rm -f "$MOUNT"/test_* "$MOUNT"/big_* "$MOUNT"/edge_* \
          "$MOUNT"/rmw_* "$MOUNT"/magic_* "$MOUNT"/remount_* 2>/dev/null
}

# ── sanity check ──────────────────────────────────────────────────────────────
if [ ! -d "$MOUNT" ]; then
    echo -e "${RED}ERROR: mountpoint '$MOUNT' không tồn tại.${NC}"
    exit 1
fi

if ! mountpoint -q "$MOUNT" 2>/dev/null; then
    # Một số hệ thống không có mountpoint command, thử cách khác
    if ! ls "$MOUNT" > /dev/null 2>&1; then
        echo -e "${RED}ERROR: '$MOUNT' chưa được mount.${NC}"
        exit 1
    fi
fi

echo ""
echo -e "${CYAN}myfs test suite — $(date '+%Y-%m-%d %H:%M:%S')${NC}"
echo -e "mountpoint: ${MOUNT}  backing: ${BACKING}"
cleanup

# =============================================================================
section "TC01: Basic write + read"
# =============================================================================

echo "HELLO MYFS" > "$MOUNT/test_basic.txt"
assert_content "TC01.1 — basic echo write + cat read" \
    "HELLO MYFS" "$MOUNT/test_basic.txt"

# Kiểm tra backing tồn tại cả .data và .meta
assert_exists "TC01.2 — .data tồn tại trong backing" \
    "$BACKING/test_basic.txt.data"
assert_exists "TC01.3 — .meta tồn tại trong backing" \
    "$BACKING/test_basic.txt.meta"

# ls -l phải trả đúng kích thước logical (11 bytes = "HELLO MYFS\n")
SIZE=$(stat -c%s "$MOUNT/test_basic.txt" 2>/dev/null)
assert_eq "TC01.4 — logical size đúng (11 bytes)" "11" "$SIZE"

# =============================================================================
section "TC02: Overwrite bằng redirect (O_TRUNC path)"
# =============================================================================

echo "FIRST VERSION" > "$MOUNT/test_trunc.txt"
echo "SECOND VERSION" > "$MOUNT/test_trunc.txt"
assert_content "TC02.1 — overwrite bằng redirect" \
    "SECOND VERSION" "$MOUNT/test_trunc.txt"

# chunk map phải reset về 1 chunk (không accumulate từ lần write cũ)
NUM_CHUNKS=$(python3 -c "
import struct, sys
with open('$BACKING/test_trunc.txt.meta', 'rb') as f:
    n = struct.unpack('<I', f.read(4))[0]
print(n)
" 2>/dev/null)
assert_eq "TC02.2 — chunk map không accumulate sau overwrite" "1" "$NUM_CHUNKS"

# =============================================================================
section "TC03: Partial overwrite (RMW)"
# =============================================================================

echo "HELLO WORLD" > "$MOUNT/rmw_test.txt"
printf 'FUSE!' | dd of="$MOUNT/rmw_test.txt" bs=1 seek=6 conv=notrunc 2>/dev/null
assert_content "TC03.1 — partial overwrite (RMW) đúng nội dung" \
    "HELLO FUSE!" "$MOUNT/rmw_test.txt"

# RMW nhiều lần liên tiếp
printf 'OS' | dd of="$MOUNT/rmw_test.txt" bs=1 seek=6 conv=notrunc 2>/dev/null
assert_content "TC03.2 — RMW lần 2 trên cùng file" \
    "HELLO OSSE!" "$MOUNT/rmw_test.txt"

# =============================================================================
section "TC04: File lớn hơn 64KB (multi-chunk)"
# =============================================================================

# Tạo file text 128KB — compressible
python3 -c "print('A' * 131072, end='')" > "$MOUNT/big_text.txt"
LOGICAL=$(stat -c%s "$MOUNT/big_text.txt")
assert_eq "TC04.1 — logical size đúng cho file 128KB" "131072" "$LOGICAL"

# Đọc lại và so sánh bit-by-bit
python3 -c "print('A' * 131072, end='')" > /tmp/myfs_expected_big.txt
assert_diff "TC04.2 — nội dung file 128KB khớp bit-by-bit" \
    /tmp/myfs_expected_big.txt "$MOUNT/big_text.txt"

# =============================================================================
section "TC05: File binary ngẫu nhiên (incompressible)"
# =============================================================================

dd if=/dev/urandom bs=4096 count=4 of=/tmp/myfs_rand.bin 2>/dev/null
cp /tmp/myfs_rand.bin "$MOUNT/big_rand.bin"
assert_diff "TC05.1 — binary random file khớp bit-by-bit" \
    /tmp/myfs_rand.bin "$MOUNT/big_rand.bin"

# =============================================================================
section "TC06: Magic byte detection (incompressible formats)"
# =============================================================================

# JPEG magic: FF D8 FF E0
printf '\xFF\xD8\xFF\xE0test jpeg content' > "$MOUNT/magic_jpeg.jpg"
CODEC=$(python3 -c "
import struct
with open('$BACKING/magic_jpeg.jpg.meta', 'rb') as f:
    f.read(4)   # num_chunks
    f.read(8)   # logical_size
    # chunk: logical_offset(8) + raw_size(4) + stored_size(4) + codec_type(1)
    f.read(8); f.read(4); f.read(4)
    codec = struct.unpack('B', f.read(1))[0]
print(codec)
" 2>/dev/null)
assert_eq "TC06.1 — JPEG magic byte → codec=0 (raw)" "0" "$CODEC"

# ZIP magic: 50 4B 03 04
printf '\x50\x4B\x03\x04test zip content' > "$MOUNT/magic_zip.zip"
CODEC=$(python3 -c "
import struct
with open('$BACKING/magic_zip.zip.meta', 'rb') as f:
    f.read(4); f.read(8); f.read(8); f.read(4); f.read(4)
    codec = struct.unpack('B', f.read(1))[0]
print(codec)
" 2>/dev/null)
assert_eq "TC06.2 — ZIP magic byte → codec=0 (raw)" "0" "$CODEC"

# =============================================================================
section "TC07: Truncate"
# =============================================================================

echo "HELLO WORLD TRUNCATE" > "$MOUNT/test_trunc2.txt"
truncate -s 5 "$MOUNT/test_trunc2.txt"
SIZE=$(stat -c%s "$MOUNT/test_trunc2.txt")
assert_eq "TC07.1 — truncate -s 5: logical size đúng" "5" "$SIZE"

CONTENT=$(cat "$MOUNT/test_trunc2.txt")
assert_eq "TC07.2 — truncate -s 5: nội dung đúng" "HELLO" "$CONTENT"

# Truncate về 0
truncate -s 0 "$MOUNT/test_trunc2.txt"
SIZE=$(stat -c%s "$MOUNT/test_trunc2.txt")
assert_eq "TC07.3 — truncate -s 0: logical size = 0" "0" "$SIZE"

# Ghi lại sau truncate 0
echo "FRESH" > "$MOUNT/test_trunc2.txt"
assert_content "TC07.4 — ghi lại sau truncate 0" "FRESH" "$MOUNT/test_trunc2.txt"

# =============================================================================
section "TC08: Unlink"
# =============================================================================

echo "DELETE ME" > "$MOUNT/test_unlink.txt"
rm "$MOUNT/test_unlink.txt"
assert_not_exists "TC08.1 — file không còn trong mountpoint" \
    "$MOUNT/test_unlink.txt"
assert_not_exists "TC08.2 — .data bị xóa trong backing" \
    "$BACKING/test_unlink.txt.data"
assert_not_exists "TC08.3 — .meta bị xóa trong backing" \
    "$BACKING/test_unlink.txt.meta"

# =============================================================================
section "TC09: Append nhiều lần liên tiếp"
# =============================================================================

rm -f "$MOUNT/test_append.txt"
for i in 1 2 3 4 5; do
    echo "line $i" >> "$MOUNT/test_append.txt"
done

LINES=$(wc -l < "$MOUNT/test_append.txt")
assert_eq "TC09.1 — append 5 lần → 5 dòng" "5" "$LINES"

LAST=$(tail -1 "$MOUNT/test_append.txt")
assert_eq "TC09.2 — dòng cuối đúng" "line 5" "$LAST"

# =============================================================================
section "TC10: Sao chép (cp) và so sánh (diff)"
# =============================================================================

echo "COPY TEST CONTENT" > "$MOUNT/test_cp_src.txt"
cp "$MOUNT/test_cp_src.txt" "$MOUNT/test_cp_dst.txt"
assert_diff "TC10.1 — cp trong mountpoint: nội dung khớp" \
    "$MOUNT/test_cp_src.txt" "$MOUNT/test_cp_dst.txt"

# cp từ host vào mountpoint
echo "FROM HOST" > /tmp/myfs_host.txt
cp /tmp/myfs_host.txt "$MOUNT/test_cp_host.txt"
assert_diff "TC10.2 — cp từ host vào mountpoint" \
    /tmp/myfs_host.txt "$MOUNT/test_cp_host.txt"

# =============================================================================
section "TC11: Tạo và xóa thư mục (mkdir + rmdir)"
# =============================================================================

mkdir "$MOUNT/test_dir" 2>/dev/null
assert_exists "TC11.1 — mkdir tạo thư mục" "$MOUNT/test_dir"

echo "in subdir" > "$MOUNT/test_dir/sub.txt"
assert_content "TC11.2 — ghi file trong subdir" "in subdir" "$MOUNT/test_dir/sub.txt"

rm "$MOUNT/test_dir/sub.txt"
rmdir "$MOUNT/test_dir"
assert_not_exists "TC11.3 — rmdir xóa thư mục" "$MOUNT/test_dir"

# =============================================================================
section "TC12: Edge cases"
# =============================================================================

# File rỗng
touch "$MOUNT/edge_empty.txt"
SIZE=$(stat -c%s "$MOUNT/edge_empty.txt")
assert_eq "TC12.1 — file rỗng: logical size = 0" "0" "$SIZE"
CONTENT=$(cat "$MOUNT/edge_empty.txt")
assert_eq "TC12.2 — file rỗng: cat trả về empty" "" "$CONTENT"

# File 1 byte
printf 'X' > "$MOUNT/edge_1byte.txt"
SIZE=$(stat -c%s "$MOUNT/edge_1byte.txt")
assert_eq "TC12.3 — file 1 byte: logical size = 1" "1" "$SIZE"

# Filename với space
echo "spaces test" > "$MOUNT/edge_with spaces.txt"
assert_content "TC12.4 — filename có space" "spaces test" "$MOUNT/edge_with spaces.txt"

# =============================================================================
section "TC13: File > 64KB — multi-chunk write + partial overwrite qua ranh giới"
# =============================================================================

# Tạo file 128KB toàn chữ A
python3 -c "import sys; sys.stdout.buffer.write(b'A' * 131072)" > "$MOUNT/big_border.bin"

# Đọc lại bit-by-bit
python3 -c "import sys; sys.stdout.buffer.write(b'A' * 131072)" > /tmp/myfs_big_border_expected.bin
assert_diff "TC13.1 — write 128KB + read bit-by-bit"     /tmp/myfs_big_border_expected.bin "$MOUNT/big_border.bin"

# Partial overwrite tại offset 65530 — vắt qua ranh giới 64KB (65536)
# Vùng ghi: [65530, 65530+12) = span qua offset 65536
python3 -c "import os; fd = os.open('$MOUNT/big_border.bin', os.O_WRONLY); os.lseek(fd, 65530, 0); os.write(fd, b'CROSSBORDER!'); os.close(fd)"

# Verify bằng cách đọc đúng vùng đó
ACTUAL=$(python3 -c "
with open('$MOUNT/big_border.bin', 'rb') as f:
    f.seek(65530)
    print(f.read(12).decode())
" 2>/dev/null)
assert_eq "TC13.2 — partial overwrite qua ranh giới 64KB" "CROSSBORDER!" "$ACTUAL"

# Phần trước ranh giới không bị ảnh hưởng
BEFORE=$(python3 -c "
with open('$MOUNT/big_border.bin', 'rb') as f:
    f.seek(65520)
    print(f.read(10).decode())
" 2>/dev/null)
assert_eq "TC13.3 — vùng trước ranh giới không bị ảnh hưởng" "AAAAAAAAAA" "$BEFORE"

# Phần sau ranh giới không bị ảnh hưởng
AFTER=$(python3 -c "
with open('$MOUNT/big_border.bin', 'rb') as f:
    f.seek(65542)
    print(f.read(10).decode())
" 2>/dev/null)
assert_eq "TC13.4 — vùng sau ranh giới không bị ảnh hưởng" "AAAAAAAAAA" "$AFTER"

# =============================================================================
section "TC14: Append nhiều lần — correctness + logical size"
# =============================================================================

rm -f "$MOUNT/tc14_append.txt"

# Append 20 dòng
for i in $(seq 1 20); do
    echo "line $i" >> "$MOUNT/tc14_append.txt"
done

# Số dòng
LINES=$(wc -l < "$MOUNT/tc14_append.txt")
assert_eq "TC14.1 — append 20 lần → 20 dòng" "20" "$LINES"

# Dòng đầu và cuối
FIRST=$(head -1 "$MOUNT/tc14_append.txt")
assert_eq "TC14.2 — dòng đầu đúng" "line 1" "$FIRST"

LAST=$(tail -1 "$MOUNT/tc14_append.txt")
assert_eq "TC14.3 — dòng cuối đúng" "line 20" "$LAST"

# Logical size phai bang tong so byte: line 1-9 = 7 bytes, line 10-20 = 8 bytes
EXPECTED_SIZE=$(python3 -c "print(sum(len('line ' + str(i) + '\n') for i in range(1,21)))")
ACTUAL_SIZE=$(stat -c%s "$MOUNT/tc14_append.txt")
assert_eq "TC14.4 — logical size sau 20 lần append" "$EXPECTED_SIZE" "$ACTUAL_SIZE"

# =============================================================================
section "TC15: Remount nhiều lần — persistence"
# =============================================================================

# Test này chỉ chạy được nếu có quyền unmount (cần FUSE đang chạy)
# Kiểm tra data persist sau khi đọc từ cold cache (không remount thực sự,
# nhưng verify metadata đồng bộ bằng cách đọc lại nhiều lần liên tiếp)

echo "PERSIST TEST" > "$MOUNT/tc15_persist.txt"

# Đọc lại 5 lần liên tiếp — verify không có cache corruption
ALL_OK=1
for i in $(seq 1 5); do
    CONTENT=$(cat "$MOUNT/tc15_persist.txt" 2>/dev/null)
    if [ "$CONTENT" != "PERSIST TEST" ]; then
        ALL_OK=0
        break
    fi
done
assert_eq "TC15.1 — đọc lặp lại 5 lần nhất quán" "1" "$ALL_OK"

# Write sau nhiều lần đọc vẫn đúng
echo "UPDATED" > "$MOUNT/tc15_persist.txt"
assert_content "TC15.2 — write sau nhiều lần đọc" "UPDATED" "$MOUNT/tc15_persist.txt"

# Verify backing store nhất quán với mountpoint
META_SIZE=$(python3 -c "
import struct
with open('$BACKING/tc15_persist.txt.meta', 'rb') as f:
    f.read(4)  # num_chunks
    size = struct.unpack('<Q', f.read(8))[0]
print(size)
" 2>/dev/null)
MOUNT_SIZE=$(stat -c%s "$MOUNT/tc15_persist.txt")
assert_eq "TC15.3 — logical_size trong .meta khớp với stat" "$MOUNT_SIZE" "$META_SIZE"

# =============================================================================
section "TC16: File > 64KB — compressible content"
# =============================================================================

# File text lặp lại — compressible tốt, kiểm tra compression ratio
python3 << 'PY' > "$MOUNT/tc16_compress.txt"
import sys
sys.stdout.buffer.write((b"Hello World! " * 100 + b"\n") * 100)
PY
python3 << 'PY' > /tmp/myfs_tc16_expected.txt
import sys
sys.stdout.buffer.write((b"Hello World! " * 100 + b"\n") * 100)
PY

assert_diff "TC16.1 — file text 130KB compressible đọc lại đúng"     /tmp/myfs_tc16_expected.txt "$MOUNT/tc16_compress.txt"

# Disk size phải nhỏ hơn logical size (đã được nén)
LOGICAL=$(stat -c%s "$MOUNT/tc16_compress.txt")
DISK=$(stat -c%s "$BACKING/tc16_compress.txt.data")
if [ "$DISK" -lt "$LOGICAL" ]; then
    ok "TC16.2 — compression hiệu quả disk=${DISK} logical=${LOGICAL}"
else
    fail "TC16.2 — compression không hiệu quả" "disk < $LOGICAL" "disk=$DISK"
fi

# =============================================================================
section "TC17: Partial overwrite — đầu file và cuối file"
# =============================================================================

echo "ABCDEFGHIJ" > "$MOUNT/tc17_edges.txt"

# Overwrite đầu file (offset=0)
printf 'XY' | dd of="$MOUNT/tc17_edges.txt" bs=1 seek=0 conv=notrunc 2>/dev/null
assert_content "TC17.1 — overwrite đầu file" "XYCDEFGHIJ" "$MOUNT/tc17_edges.txt"

# Overwrite cuối file (offset=8, 2 bytes cuối trước newline)
printf 'ZZ' | dd of="$MOUNT/tc17_edges.txt" bs=1 seek=8 conv=notrunc 2>/dev/null
assert_content "TC17.2 — overwrite cuối file" "XYCDEFGHZZ" "$MOUNT/tc17_edges.txt"

# =============================================================================
section "TC18: Garbage collection — compact sau RMW"
# =============================================================================

# Compact được kiểm tra bằng cách: sau nhiều lần ghi đè lên cùng file,
# kích thước đĩa không được tăng quá 2x kích thước logic (compact giữ nó ở mức hợp lý).
# File ngẫu nhiên 512KB là blob không nén được (raw), mỗi RMW tạo orphan ~512KB.
# Compact chạy trong myfs_release() của mỗi descriptor — nên kích thước đĩa ổn định.

dd if=/dev/urandom bs=512K count=1 of="$MOUNT/tc18_gc.bin" 2>/dev/null
LOGICAL=$(stat -c%s "$MOUNT/tc18_gc.bin")
SIZE_INITIAL=$(stat -c%s "$BACKING/tc18_gc.bin.data")
 
# 5 lần ghi đè (mỗi lần là 1 write+release cycle riêng)
for i in $(seq 1 5); do
    dd if=/dev/urandom bs=512K count=1 of="$MOUNT/tc18_gc.bin" conv=notrunc 2>/dev/null
done

# Compaction giờ chạy trên background worker — chờ worker xử lý xong queue
sleep 2
SIZE_FINAL=$(stat -c%s "$BACKING/tc18_gc.bin.data")
 
# Sau compact chạy liên tục, disk size không được > 2x initial
# (nếu compact không chạy, size sẽ là 6x initial)
if [ "$SIZE_FINAL" -le "$((SIZE_INITIAL * 2))" ]; then
    ok "TC18.1 — compact giữ disk ổn định (initial=${SIZE_INITIAL} final=${SIZE_FINAL})"
else
    fail "TC18.1 — disk bloat quá lớn (compact không chạy?)" "<= $((SIZE_INITIAL * 2))" "${SIZE_FINAL}"
fi
 
# Logical size không đổi
LOGICAL_AFTER=$(stat -c%s "$MOUNT/tc18_gc.bin")
assert_eq "TC18.2 — logical size không đổi qua các lần ghi đè" "$LOGICAL" "$LOGICAL_AFTER"
 
# Đọc lại được sau compact (không bị corrupt)
READ_SIZE=$(cat "$MOUNT/tc18_gc.bin" 2>/dev/null | wc -c)
assert_eq "TC18.3 — đọc lại đúng số byte sau compact" "$LOGICAL" "$READ_SIZE"

# =============================================================================
section "TC19: Sparse hole write chồng lấn chunk phía sau (stale read)"
# =============================================================================

# Ghi vào hole trước một chunk đã tồn tại, vùng ghi phủ lên chunk đó.
# Trước fix: chunk mới chồng lấn chunk cũ trong map, read linear scan gặp
# chunk cũ trước → trả dữ liệu cũ. Đọc qua O_DIRECT để bypass kernel page
# cache (kernel_cache=1 sẽ che bug vì cache giữ dữ liệu vừa ghi).
rm -f "$MOUNT/tc19_overlap.bin"
python3 -c "
import os
fd = os.open('$MOUNT/tc19_overlap.bin', os.O_CREAT | os.O_WRONLY, 0o644)
os.pwrite(fd, b'BBBB', 100)
os.close(fd)
fd = os.open('$MOUNT/tc19_overlap.bin', os.O_WRONLY)
os.pwrite(fd, b'CCCCCCCC', 96)
os.close(fd)
"
dd if="$MOUNT/tc19_overlap.bin" of=/tmp/myfs_tc19.bin iflag=direct bs=4096 2>/dev/null
ACTUAL=$(python3 -c "
data = open('/tmp/myfs_tc19.bin','rb').read()
expect = b'\x00'*96 + b'CCCCCCCC'
print('OK' if data == expect else 'MISMATCH tail=%r' % data[96:])
")
assert_eq "TC19.1 — write phủ hole+chunk: dữ liệu mới thắng (direct read)" "OK" "$ACTUAL"

OVERLAP=$(python3 -c "
import struct
d = open('$BACKING/tc19_overlap.bin.meta','rb').read()
n = struct.unpack_from('<I', d, 0)[0]
ranges = sorted((struct.unpack_from('<Q', d, 12+32*i)[0],
                 struct.unpack_from('<Q', d, 12+32*i)[0] +
                 struct.unpack_from('<I', d, 12+32*i+12)[0]) for i in range(n))
ok = all(ranges[i][1] <= ranges[i+1][0] for i in range(len(ranges)-1))
print('OK' if ok else 'OVERLAP %r' % ranges)
")
assert_eq "TC19.2 — chunk map không chứa range chồng lấn" "OK" "$OVERLAP"

# Sparse write ở offset giảm dần: mảng chunk phải giữ thứ tự sắp xếp
rm -f "$MOUNT/tc19_sparse.bin"
python3 -c "
import os
fd = os.open('$MOUNT/tc19_sparse.bin', os.O_CREAT | os.O_WRONLY, 0o644)
os.pwrite(fd, b'DD', 200)
os.close(fd)
fd = os.open('$MOUNT/tc19_sparse.bin', os.O_WRONLY)
os.pwrite(fd, b'EE', 50)
os.close(fd)
"
SORTED=$(python3 -c "
import struct
d = open('$BACKING/tc19_sparse.bin.meta','rb').read()
n = struct.unpack_from('<I', d, 0)[0]
offs = [struct.unpack_from('<Q', d, 12+32*i)[0] for i in range(n)]
ends = [offs[i] + struct.unpack_from('<I', d, 12+32*i+12)[0] for i in range(n)]
ok = n >= 1 and offs == sorted(offs) and \
     all(ends[i] <= offs[i+1] for i in range(n-1))
print('OK' if ok else 'BAD %r' % offs)
")
assert_eq "TC19.3 — sparse write offset giảm dần: map sắp xếp, không chồng lấn" "OK" "$SORTED"

# Write phủ qua nhiều sparse chunk cùng lúc
python3 -c "
import os
fd = os.open('$MOUNT/tc19_sparse.bin', os.O_WRONLY)
os.pwrite(fd, b'F' * 200, 40)
os.close(fd)
"
dd if="$MOUNT/tc19_sparse.bin" of=/tmp/myfs_tc19.bin iflag=direct bs=4096 2>/dev/null
ACTUAL=$(python3 -c "
data = open('/tmp/myfs_tc19.bin','rb').read()
expect = bytes(40) + b'F' * 200
print('OK' if data == expect else 'MISMATCH len=%d' % len(data))
")
assert_eq "TC19.4 — write phủ nhiều sparse chunk: nội dung khớp" "OK" "$ACTUAL"

# Write bắt đầu trong hole, phủ chunk và vượt EOF
rm -f "$MOUNT/tc19_eof.bin"
python3 -c "
import os
fd = os.open('$MOUNT/tc19_eof.bin', os.O_CREAT | os.O_WRONLY, 0o644)
os.pwrite(fd, b'GGGG', 100)
os.close(fd)
fd = os.open('$MOUNT/tc19_eof.bin', os.O_WRONLY)
os.pwrite(fd, b'H' * 30, 90)
os.close(fd)
"
SIZE=$(stat -c%s "$MOUNT/tc19_eof.bin")
assert_eq "TC19.5 — write hole→qua EOF: logical size mở rộng" "120" "$SIZE"
dd if="$MOUNT/tc19_eof.bin" of=/tmp/myfs_tc19.bin iflag=direct bs=4096 2>/dev/null
ACTUAL=$(python3 -c "
data = open('/tmp/myfs_tc19.bin','rb').read()
expect = bytes(90) + b'H' * 30
print('OK' if data == expect else 'MISMATCH')
")
assert_eq "TC19.6 — nội dung sau write qua EOF đúng" "OK" "$ACTUAL"

# =============================================================================
section "TC20: Truncate vào giữa chunk nén"
# =============================================================================

# Trước fix: truncate chỉ shrink stored_size, blob nén vẫn giải nén ra kích
# thước cũ → không vừa buffer → mọi lần đọc sau trả -EIO vĩnh viễn.
rm -f "$MOUNT/tc20_trunc.bin"
python3 -c "
import os
fd = os.open('$MOUNT/tc20_trunc.bin', os.O_CREAT | os.O_WRONLY, 0o644)
os.write(fd, b'A' * 8192)
os.close(fd)
"
CODEC=$(python3 -c "
import struct
d = open('$BACKING/tc20_trunc.bin.meta','rb').read()
print(struct.unpack_from('<QIIBB2xIQ', d, 12)[3])
")
assert_eq "TC20.1 — 8KB chữ A được nén (codec=1)" "1" "$CODEC"

truncate -s 4096 "$MOUNT/tc20_trunc.bin"
dd if="$MOUNT/tc20_trunc.bin" of=/tmp/myfs_tc20.bin iflag=direct bs=4096 2>/dev/null
RET=$?
CONTENT_OK=$(python3 -c "
data = open('/tmp/myfs_tc20.bin','rb').read()
print('OK' if data == b'A'*4096 else 'BAD len=%d' % len(data))
")
assert_eq "TC20.2 — direct read sau truncate giữa chunk nén thành công" "0" "$RET"
assert_eq "TC20.3 — nội dung 4096 byte A đúng" "OK" "$CONTENT_OK"

META_OK=$(python3 -c "
import struct
d = open('$BACKING/tc20_trunc.bin.meta','rb').read()
n = struct.unpack_from('<I', d, 0)[0]
lsize = struct.unpack_from('<Q', d, 4)[0]
stored = struct.unpack_from('<QIIBB2xIQ', d, 12)[2]
print('OK' if n == 1 and lsize == 4096 and stored == 4096
      else 'BAD n=%d lsize=%d stored=%d' % (n, lsize, stored))
")
assert_eq "TC20.4 — .meta cập nhật stored_size sau rewrite" "OK" "$META_OK"

# RMW trên chunk vừa được rewrite phải hoạt động
printf 'ZZZZ' | dd of="$MOUNT/tc20_trunc.bin" bs=1 seek=100 conv=notrunc 2>/dev/null
dd if="$MOUNT/tc20_trunc.bin" of=/tmp/myfs_tc20.bin iflag=direct bs=4096 2>/dev/null
PATCH_OK=$(python3 -c "
data = open('/tmp/myfs_tc20.bin','rb').read()
expect = b'A'*100 + b'ZZZZ' + b'A'*(4096-104)
print('OK' if data == expect else 'BAD')
")
assert_eq "TC20.5 — RMW sau truncate hoạt động" "OK" "$PATCH_OK"

# Truncate đúng ranh giới chunk: chunk giữ lại không cần rewrite
rm -f "$MOUNT/tc20_bound.bin"
python3 -c "
import os
fd = os.open('$MOUNT/tc20_bound.bin', os.O_CREAT | os.O_WRONLY, 0o644)
os.write(fd, b'B' * 65536)
os.write(fd, b'C' * 65536)
os.close(fd)
"
truncate -s 65536 "$MOUNT/tc20_bound.bin"
BOUND_OK=$(python3 -c "
import struct
d = open('$BACKING/tc20_bound.bin.meta','rb').read()
n = struct.unpack_from('<I', d, 0)[0]
stored = struct.unpack_from('<QIIBB2xIQ', d, 12)[2]
print('OK' if n == 1 and stored == 65536 else 'BAD n=%d stored=%d' % (n, stored))
")
assert_eq "TC20.6 — truncate tại ranh giới chunk: chunk đầu giữ nguyên" "OK" "$BOUND_OK"
dd if="$MOUNT/tc20_bound.bin" of=/tmp/myfs_tc20.bin iflag=direct bs=65536 2>/dev/null
BCONTENT=$(python3 -c "
data = open('/tmp/myfs_tc20.bin','rb').read()
print('OK' if data == b'B'*65536 else 'BAD')
")
assert_eq "TC20.7 — nội dung sau truncate ranh giới đúng" "OK" "$BCONTENT"

# =============================================================================
section "TC21: Thư mục lớn — readdir không giới hạn 2048 entry"
# =============================================================================

# Fixture tạo thẳng trong backing (1 lệnh touch) — qua FUSE sẽ rất chậm.
# Đếm bằng os.listdir: chỉ getdents, không sort, không stat từng entry
# (không phụ thuộc biến thể ls của hệ thống).
mkdir -p "$BACKING/tc21_dir"
( cd "$BACKING/tc21_dir" && touch f{1..2200}.data )
COUNT=$(python3 -c "
import os
print(sum(1 for n in os.listdir('$MOUNT/tc21_dir') if n.startswith('f')))
" 2>/dev/null)
assert_eq "TC21.1 — 2200 file hiện đủ trong listing (trước fix: 2048)" "2200" "$COUNT"

( cd "$BACKING/tc21_dir" && touch f{1..2200}.meta )
COUNT=$(python3 -c "
import os
print(sum(1 for n in os.listdir('$MOUNT/tc21_dir') if n.startswith('f')))
" 2>/dev/null)
assert_eq "TC21.2 — dedup .data/.meta ở quy mô lớn" "2200" "$COUNT"

# Artifact nội bộ của generation storage phải bị ẩn khỏi listing
HEX32="0123456789abcdef0123456789abcdef"
mkdir -p "$BACKING/tc21_dir/x.g.$HEX32"
touch "$BACKING/tc21_dir/x.g.$HEX32.owner" \
      "$BACKING/tc21_dir/y.data.alias.data.123" \
      "$BACKING/tc21_dir/z.current.tmp.$HEX32"
LEAKED=$(python3 -c "
import os
names = os.listdir('$MOUNT/tc21_dir')
print(sum(1 for n in names
          if '.g.' in n or '.alias.' in n or '.current.tmp.' in n))
" 2>/dev/null)
assert_eq "TC21.3 — generation dir/owner marker/alias/tmp bị ẩn" "0" "$LEAKED"

rm -rf "$BACKING/tc21_dir"

# =============================================================================
section "TC22: Durability — fdatasync blob trước khi publish metadata"
# =============================================================================

# Crash-test thực sự cần kill/restart daemon — không làm được từ trong suite
# (mount do terminal khác giữ). Quy trình thủ công:
#   1. ./myfs -f mountpoint ./backing   (terminal khác)
#   2. while :; do dd if=/dev/urandom of=mountpoint/crash.bin bs=64K count=4 conv=notrunc 2>/dev/null; done
#   3. kill -9 <pid myfs>; fusermount3 -u mountpoint; mount lại
#   4. cat mountpoint/crash.bin > /dev/null — phải đọc được, không -EIO
#      (-EIO do CRC mismatch = metadata trỏ tới blob chưa persist = fail).
# Ở đây chỉ kiểm tra non-regression và độ trễ không thoái hoá bất thường.

echo "DURABLE" > "$MOUNT/tc22_dur.txt"
printf 'XX' | dd of="$MOUNT/tc22_dur.txt" bs=1 seek=2 conv=notrunc 2>/dev/null
dd if="$MOUNT/tc22_dur.txt" of=/tmp/myfs_tc22.bin iflag=direct bs=512 2>/dev/null
ACTUAL=$(cat /tmp/myfs_tc22.bin)
assert_eq "TC22.1 — write + RMW + direct read nhất quán" "DUXXBLE" "$ACTUAL"

START=$(date +%s)
for i in $(seq 1 50); do
    echo "append $i" >> "$MOUNT/tc22_dur.txt"
done
ELAPSED=$(( $(date +%s) - START ))
if [ "$ELAPSED" -le 30 ]; then
    ok "TC22.2 — 50 lần append với fdatasync trong thời gian hợp lý (${ELAPSED}s)"
else
    fail "TC22.2 — append quá chậm sau khi thêm fdatasync" "<= 30s" "${ELAPSED}s"
fi

# =============================================================================
section "TC23: Chunk packing — 1MB ghi 4KB/lần → chunk 64KB"
# =============================================================================

# Trước packing: 256 write call = 256 chunk nhỏ. Sau packing: các append gộp
# vào chunk đuôi của cửa sổ hiện tại → đúng 16 chunk 64KB head-aligned.
rm -f "$MOUNT/tc23_pack.bin"
python3 -c "
import os
fd = os.open('$MOUNT/tc23_pack.bin', os.O_CREAT | os.O_WRONLY, 0o644)
ref = open('/tmp/myfs_tc23_ref.bin', 'wb')
for i in range(256):
    block = bytes([i % 256]) * 4096
    os.write(fd, block)
    ref.write(block)
os.close(fd)
ref.close()
"
PACK_OK=$(python3 -c "
import struct
d = open('$BACKING/tc23_pack.bin.meta','rb').read()
n = struct.unpack_from('<I', d, 0)[0]
lsize = struct.unpack_from('<Q', d, 4)[0]
recs = [struct.unpack_from('<QIIBB2xIQ', d, 12+32*i) for i in range(n)]
aligned = all(r[0] % 65536 == 0 and 0 < r[2] <= 65536 for r in recs)
print('OK' if n == 16 and lsize == 1048576 and aligned and len(d) == 12+32*n
      else 'BAD n=%d lsize=%d aligned=%s' % (n, lsize, aligned))
")
assert_eq "TC23.1 — 256 append 4KB → đúng 16 chunk 64KB head-aligned" "OK" "$PACK_OK"

dd if="$MOUNT/tc23_pack.bin" of=/tmp/myfs_tc23_read.bin iflag=direct bs=65536 2>/dev/null
cmp -s /tmp/myfs_tc23_ref.bin /tmp/myfs_tc23_read.bin
assert_eq "TC23.2 — nội dung 1MB khớp bit-by-bit (direct read)" "0" "$?"

# =============================================================================
section "TC24: Overwrite vắt qua nhiều cửa sổ"
# =============================================================================

rm -f "$MOUNT/tc24_cross.bin"
python3 -c "
import os
fd = os.open('$MOUNT/tc24_cross.bin', os.O_CREAT | os.O_WRONLY, 0o644)
os.write(fd, b'P' * 65536)
os.write(fd, b'Q' * 65536)
os.write(fd, b'R' * 65536)
os.pwrite(fd, b'X' * 81920, 61440)
os.close(fd)
"
dd if="$MOUNT/tc24_cross.bin" of=/tmp/myfs_tc24.bin iflag=direct bs=65536 2>/dev/null
CROSS_OK=$(python3 -c "
data = open('/tmp/myfs_tc24.bin','rb').read()
expect = b'P'*61440 + b'X'*81920 + b'R'*(196608-143360)
print('OK' if data == expect else 'BAD len=%d' % len(data))
")
assert_eq "TC24.1 — overwrite 80KB vắt qua 3 cửa sổ: nội dung đúng" "OK" "$CROSS_OK"

N=$(python3 -c "
import struct
print(struct.unpack_from('<I', open('$BACKING/tc24_cross.bin.meta','rb').read(), 0)[0])
")
assert_eq "TC24.2 — vẫn đúng 3 chunk sau overwrite" "3" "$N"

# =============================================================================
section "TC25: Append gộp vào chunk đuôi"
# =============================================================================

rm -f "$MOUNT/tc25_tail.txt"
printf 'AAAAAAAAAA' > "$MOUNT/tc25_tail.txt"
printf 'BBBBBBBBBBBBBBBBBBBB' >> "$MOUNT/tc25_tail.txt"
N=$(python3 -c "
import struct
print(struct.unpack_from('<I', open('$BACKING/tc25_tail.txt.meta','rb').read(), 0)[0])
")
assert_eq "TC25.1 — append nhỏ gộp vào chunk đuôi (1 chunk, không sibling)" "1" "$N"
assert_content "TC25.2 — nội dung sau merge đúng" \
    "AAAAAAAAAABBBBBBBBBBBBBBBBBBBB" "$MOUNT/tc25_tail.txt"

python3 -c "
import os
fd = os.open('$MOUNT/tc25_tail.txt', os.O_WRONLY | os.O_APPEND)
os.write(fd, b'C' * 65536)
os.close(fd)
"
N=$(python3 -c "
import struct
print(struct.unpack_from('<I', open('$BACKING/tc25_tail.txt.meta','rb').read(), 0)[0])
")
assert_eq "TC25.3 — append vượt ranh giới cửa sổ → tách 2 chunk" "2" "$N"

# =============================================================================
section "TC26: Sparse file — cửa sổ trống không tạo chunk"
# =============================================================================

rm -f "$MOUNT/tc26_sparse.bin"
python3 -c "
import os
fd = os.open('$MOUNT/tc26_sparse.bin', os.O_CREAT | os.O_WRONLY, 0o644)
os.pwrite(fd, b'SPARSE!!', 300000)
os.close(fd)
"
META_OK=$(python3 -c "
import struct
d = open('$BACKING/tc26_sparse.bin.meta','rb').read()
n = struct.unpack_from('<I', d, 0)[0]
lsize = struct.unpack_from('<Q', d, 4)[0]
off = struct.unpack_from('<Q', d, 12)[0]
print('OK' if n == 1 and off == 262144 and lsize == 300008
      else 'BAD n=%d off=%d lsize=%d' % (n, off, lsize))
")
assert_eq "TC26.1 — 1 chunk tại window base 262144, 4 cửa sổ đầu trống" "OK" "$META_OK"

dd if="$MOUNT/tc26_sparse.bin" of=/tmp/myfs_tc26.bin iflag=direct bs=65536 2>/dev/null
SPARSE_OK=$(python3 -c "
data = open('/tmp/myfs_tc26.bin','rb').read()
expect = bytes(300000) + b'SPARSE!!'
print('OK' if data == expect else 'BAD len=%d' % len(data))
")
assert_eq "TC26.2 — hole đọc ra zero, dữ liệu đúng vị trí" "OK" "$SPARSE_OK"

# =============================================================================
section "TC27: Migration — file legacy unpacked được repack qua compaction"
# =============================================================================

# Giả lập file từ format cũ: 3 chunk 5000 byte raw, không theo cửa sổ.
python3 -c "
import struct
data = b''.join(bytes([65+i]) * 5000 for i in range(3))
open('$BACKING/tc27_legacy.bin.data','wb').write(data)
meta = struct.pack('<I', 3) + struct.pack('<Q', 15000)
for i in range(3):
    meta += struct.pack('<QIIBB2xIQ', i*5000, 5000, 5000, 0, 0, 0, i*5000)
open('$BACKING/tc27_legacy.bin.meta','wb').write(meta)
"
CONTENT_OK=$(python3 -c "
with open('$MOUNT/tc27_legacy.bin','rb') as f:
    data = f.read()
expect = b'A'*5000 + b'B'*5000 + b'C'*5000
print('OK' if data == expect else 'BAD len=%d' % len(data))
")
assert_eq "TC27.1 — đọc file legacy unpacked (tolerant tier)" "OK" "$CONTENT_OK"

# release của lần đọc trên kích hoạt compaction migration (file chưa packed
# được repack bất kể waste); chờ chút cho release xử lý xong.
sleep 1
PACKED=$(python3 -c "
import struct
d = open('$BACKING/tc27_legacy.bin.meta','rb').read()
n = struct.unpack_from('<I', d, 0)[0]
recs = [struct.unpack_from('<QIIBB2xIQ', d, 12+32*i) for i in range(n)]
ok = n == 1 and all(r[0] % 65536 == 0 and 0 < r[2] <= 65536 for r in recs)
print('OK' if ok else 'BAD n=%d' % n)
")
assert_eq "TC27.2 — sau release: repack thành packed (1 chunk 15000 byte)" "OK" "$PACKED"

dd if="$MOUNT/tc27_legacy.bin" of=/tmp/myfs_tc27.bin iflag=direct bs=65536 2>/dev/null
AFTER_OK=$(python3 -c "
data = open('/tmp/myfs_tc27.bin','rb').read()
expect = b'A'*5000 + b'B'*5000 + b'C'*5000
print('OK' if data == expect else 'BAD')
")
assert_eq "TC27.3 — nội dung không đổi sau migration" "OK" "$AFTER_OK"

# Meta cố tình KHÔNG sắp xếp → sort-on-load + tolerant tier vẫn đọc đúng
python3 -c "
import struct
data = b'X'*100 + b'Y'*100
open('$BACKING/tc27_unsorted.bin.data','wb').write(data)
meta = struct.pack('<I', 2) + struct.pack('<Q', 200)
meta += struct.pack('<QIIBB2xIQ', 100, 100, 100, 0, 0, 0, 100)
meta += struct.pack('<QIIBB2xIQ', 0, 100, 100, 0, 0, 0, 0)
open('$BACKING/tc27_unsorted.bin.meta','wb').write(meta)
"
UNSORTED_OK=$(python3 -c "
with open('$MOUNT/tc27_unsorted.bin','rb') as f:
    data = f.read()
print('OK' if data == b'X'*100 + b'Y'*100 else 'BAD %r' % data[:10])
")
assert_eq "TC27.4 — meta chưa sắp xếp: sort-on-load, đọc đúng nội dung" "OK" "$UNSORTED_OK"

# =============================================================================
section "TC28: Per-file locking — ghi song song nhiều file"
# =============================================================================

# 4 writer song song trên 4 file khác nhau — với per-file locking chúng chạy
# đồng thời; kiểm tra không có corruption chéo giữa các file.
rm -f "$MOUNT"/tc28_par_*.bin
for i in 1 2 3 4; do
    python3 -c "
import os
fd = os.open('$MOUNT/tc28_par_$i.bin', os.O_CREAT | os.O_WRONLY, 0o644)
for j in range(64):
    os.write(fd, bytes([($i * 16 + j) % 256]) * 8192)
os.close(fd)
" &
done
wait

PAR_OK="ALL"
for i in 1 2 3 4; do
    ONE=$(python3 -c "
data = open('$MOUNT/tc28_par_$i.bin','rb').read()
expect = b''.join(bytes([($i * 16 + j) % 256]) * 8192 for j in range(64))
print('OK' if data == expect else 'BAD')
" 2>/dev/null)
    [ "$ONE" = "OK" ] || PAR_OK="BAD file $i"
done
assert_eq "TC28.1 — 4 writer song song: nội dung cả 4 file đúng" "ALL" "$PAR_OK"

SIZES_OK=$(python3 -c "
import os
sizes = [os.path.getsize('$MOUNT/tc28_par_%d.bin' % i) for i in (1,2,3,4)]
print('OK' if all(s == 64*8192 for s in sizes) else 'BAD %r' % sizes)
")
assert_eq "TC28.2 — logical size cả 4 file = 512KB" "OK" "$SIZES_OK"

# =============================================================================
section "TC29: Background compaction — thu hồi waste không chặn release"
# =============================================================================

# Ghi đè nhiều lần tạo orphan blob; release chỉ enqueue, worker thu hồi sau.
rm -f "$MOUNT/tc29_bg.bin"
dd if=/dev/urandom bs=256K count=1 of="$MOUNT/tc29_bg.bin" 2>/dev/null
for i in 1 2 3; do
    dd if=/dev/urandom bs=256K count=1 of="$MOUNT/tc29_bg.bin" conv=notrunc 2>/dev/null
done
sleep 2   # chờ worker xử lý queue
LOGICAL=$(stat -c%s "$MOUNT/tc29_bg.bin")
DISK=$(stat -c%s "$BACKING/tc29_bg.bin.data")
if [ "$DISK" -le "$((LOGICAL * 2))" ]; then
    ok "TC29.1 — worker thu hồi waste (logical=${LOGICAL} disk=${DISK})"
else
    fail "TC29.1 — waste không được thu hồi" "<= $((LOGICAL * 2))" "$DISK"
fi

READ_OK=$(cat "$MOUNT/tc29_bg.bin" 2>/dev/null | wc -c)
assert_eq "TC29.2 — đọc lại đúng số byte sau background compact" "$LOGICAL" "$READ_OK"

# =============================================================================
# Cleanup thêm
# =============================================================================
rm -f /tmp/myfs_big_border_expected.bin /tmp/myfs_tc16_expected.txt \
      /tmp/myfs_tc19.bin /tmp/myfs_tc20.bin /tmp/myfs_tc22.bin \
      /tmp/myfs_tc23_ref.bin /tmp/myfs_tc23_read.bin /tmp/myfs_tc24.bin \
      /tmp/myfs_tc26.bin /tmp/myfs_tc27.bin

# =============================================================================
# Cleanup
# =============================================================================
cleanup
rm -f /tmp/myfs_expected_big.txt /tmp/myfs_rand.bin /tmp/myfs_host.txt

# =============================================================================
# Kết quả
# =============================================================================
echo ""
echo -e "${CYAN}==========================================${NC}"
echo -e "  Kết quả: ${GREEN}${PASS} PASS${NC} / ${RED}${FAIL} FAIL${NC} / ${TOTAL} total"
echo -e "${CYAN}==========================================${NC}"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}  [OK] Tat ca test cases passed.${NC}"
    exit 0
else
    echo -e "${RED}  [FAIL] ${FAIL} test cases failed.${NC}"
    exit 1
fi