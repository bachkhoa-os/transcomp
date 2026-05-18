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
    echo -e "${CYAN}══════════════════════════════════════════${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}══════════════════════════════════════════${NC}"
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
section "TC10: cp và diff"
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
section "TC11: mkdir + rmdir"
# =============================================================================

mkdir "$MOUNT/test_dir"
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
# Cleanup
# =============================================================================
cleanup
rm -f /tmp/myfs_expected_big.txt /tmp/myfs_rand.bin /tmp/myfs_host.txt

# =============================================================================
# Kết quả
# =============================================================================
echo ""
echo -e "${CYAN}══════════════════════════════════════════${NC}"
echo -e "  Kết quả: ${GREEN}${PASS} PASS${NC} / ${RED}${FAIL} FAIL${NC} / ${TOTAL} total"
echo -e "${CYAN}══════════════════════════════════════════${NC}"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}  ✓ Tất cả test cases passed.${NC}"
    exit 0
else
    echo -e "${RED}  ✗ ${FAIL} test case(s) failed.${NC}"
    exit 1
fi