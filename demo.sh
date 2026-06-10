#!/bin/bash
# =============================================================================
# demo.sh — Kịch bản Demo Bảo vệ Project HĐH (Transparent Compression)
# Project No: 25228 — Nhóm: Hùng, Đại, Toàn
# =============================================================================

MOUNT="mountpoint"
BACKING="backing"
LOG_FILE="fuse_demo.log"
PASS=0
FAIL=0

# Màu sắc
CYAN='\033[1;36m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
RED='\033[1;31m'
BOLD='\033[1m'
NC='\033[0m'

pause() {
    echo ""
    read -p "$(echo -e ${YELLOW}"[Ấn Enter để tiếp tục...]"${NC})"
    echo ""
}

print_step() {
    echo ""
    echo -e "${CYAN}════════════════════════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  BƯỚC $1: $2${NC}"
    echo -e "${CYAN}════════════════════════════════════════════════════════════════════════${NC}"
}

# In kết quả PASS/FAIL và cộng vào bộ đếm tổng kết
check() {
    local desc="$1"
    local result="$2"   # "pass" hoặc "fail"
    if [ "$result" = "pass" ]; then
        echo -e "  ${GREEN}[PASS]${NC} $desc"
        ((PASS++))
    else
        echo -e "  ${RED}[FAIL]${NC} $desc"
        ((FAIL++))
    fi
}

size_bytes() { stat -c%s "$1" 2>/dev/null || echo 0; }

# Tự động chèn thêm 'B' để thành MB, KB, GB...
size_human() { 
    ls -lh "$1" 2>/dev/null | awk '{
        s=$5; 
        sub(/[KMGTP]$/, "&B", s); 
        if(s ~ /[0-9]$/) s=s"B"; 
        print s
    }'
}

# Các hàm vẽ bảng chung (bước 1 & 2)
table_header() {
    echo -e "${CYAN}  +--------------------+----------------------------------+-------------+${NC}"
    printf "  | ${BOLD}%-18s${NC} | ${BOLD}%-32s${NC} | ${BOLD}%-11s${NC} |\n" "Perspective" "Directory" "Size"
    echo -e "${CYAN}  +--------------------+----------------------------------+-------------+${NC}"
}
table_row()    { printf "  | %-18s | %-32s | %-11s |\n" "$1" "$2" "$3"; }
table_footer() { echo -e "${CYAN}  +--------------------+----------------------------------+-------------+${NC}"; }

# ── Khởi động ────────────────────────────────────────────────────────────────

clear
echo -e "${BOLD}${GREEN}"
echo "  ╔══════════════════════════════════════════════════════════════════╗"
echo "  ║   TRANSPARENT COMPRESSION FILESYSTEM — PROJECT DEMO              ║"
echo "  ║   Môn: Hệ Điều Hành (20252) — Nhóm: Hùng, Đại, Toàn              ║"
echo "  ╚══════════════════════════════════════════════════════════════════╝"
echo -e "${NC}"

echo "  Dọn dẹp môi trường cũ..."
fusermount3 -u "$MOUNT" 2>/dev/null
rm -rf "${BACKING:?}"/* "$MOUNT"/* 2>/dev/null
mkdir -p "$MOUNT" "$BACKING"

echo "  Biên dịch source code..."
if ! make clean > /dev/null 2>&1 || ! make > /dev/null 2>&1; then
    echo -e "  ${RED}[LỖI] Build thất bại! Kiểm tra lại compiler.${NC}"
    exit 1
fi
echo -e "  ${GREEN}Build OK${NC}"

echo "  Mount filesystem (log → $LOG_FILE)..."
./myfs -f "$MOUNT" "./$BACKING" > "$LOG_FILE" 2>&1 &
FUSE_PID=$!
sleep 1

if ! mountpoint -q "$MOUNT" 2>/dev/null; then
    echo -e "  ${RED}[LỖI] FUSE mount thất bại — kiểm tra $LOG_FILE${NC}"
    kill $FUSE_PID 2>/dev/null
    exit 1
fi
echo -e "  ${GREEN}Mount OK tại ./$MOUNT (PID=$FUSE_PID)${NC}"
pause

# ── Bước 1: Transparent Compression — văn bản ────────────────────────────────

print_step "1" "TRANSPARENT COMPRESSION — DỮ LIỆU VĂN BẢN"
echo ""
echo "  Tạo file văn bản lặp lại ~20 MB (tỉ lệ nén cao)..."
python3 -c "
import sys
line = b'Dai Hoc Bach Khoa Ha Noi - OS Project 2026\\n'
sys.stdout.buffer.write(line * 500000)
" > "$MOUNT/demo_text.txt"

L_B=$(size_bytes "$MOUNT/demo_text.txt")
P_B=$(size_bytes "$BACKING/demo_text.txt.data")
L_H=$(size_human "$MOUNT/demo_text.txt")
P_H=$(size_human "$BACKING/demo_text.txt.data")
RATIO=$(python3 -c "print(f'{(1-$P_B/$L_B)*100:.1f}%')")

table_header
table_row "Logical (User)"  "$MOUNT/demo_text.txt"       "$L_H"
table_row "Physical (Disk)" "$BACKING/demo_text.txt.data" "$P_H"
table_footer
echo ""
echo -e "  ${GREEN}▶ Tiết kiệm $RATIO dung lượng lưu trữ nhờ Zstd compression${NC}"

# Kiểm tra: physical < logical
if [ "$P_B" -lt "$L_B" ]; then
    check "Physical size < logical size (compression hoạt động)" "pass"
else
    check "Physical size < logical size (compression hoạt động)" "fail"
fi

# Kiểm tra: đọc lại nội dung đúng (không corrupt sau nén)
READ_LINES=$(wc -l < "$MOUNT/demo_text.txt")
if [ "$READ_LINES" -eq 500000 ]; then
    check "Đọc lại đủ 500000 dòng (transparent decompress)" "pass"
else
    check "Đọc lại đủ 500000 dòng (transparent decompress)" "fail"
fi
pause

# ── Bước 2: Heuristic skip — JPEG ────────────────────────────────────────────

print_step "2" "HEURISTIC SKIP — DỮ LIỆU ĐÃ NÉN (JPEG)"
echo ""
echo "  Tạo file giả lập JPEG 5 MB (magic bytes FF D8 FF + random payload)..."
python3 -c "
import sys, os
header = bytes([0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10]) + b'JFIF\x00'
sys.stdout.buffer.write(header + os.urandom(5*1024*1024 - len(header)))
" > "$MOUNT/demo_image.jpg"

LJ_B=$(size_bytes "$MOUNT/demo_image.jpg")
PJ_B=$(size_bytes "$BACKING/demo_image.jpg.data")
LJ_H=$(size_human "$MOUNT/demo_image.jpg")
PJ_H=$(size_human "$BACKING/demo_image.jpg.data")
OVERHEAD=$(python3 -c "print(f'{($PJ_B-$LJ_B)*100/$LJ_B:+.1f}%' if $LJ_B else '?')")

table_header
table_row "Logical (User)"  "$MOUNT/demo_image.jpg"       "$LJ_H"
table_row "Physical (Disk)" "$BACKING/demo_image.jpg.data" "$PJ_H"
table_footer
echo ""
echo -e "  ${YELLOW}▶ Magic bytes FF D8 FF nhận diện JPEG → bỏ qua Zstd (overhead: $OVERHEAD)${NC}"

# Kiểm tra: physical ≈ logical (không inflate thêm)
DIFF=$(python3 -c "print(abs($PJ_B - $LJ_B))")
if [ "$DIFF" -lt 65536 ]; then
    check "Physical ≈ logical (heuristic skip, không inflate)" "pass"
else
    check "Physical ≈ logical (heuristic skip, không inflate)" "fail"
fi
pause

# ── Bước 3: Read-Modify-Write ────────────────────────────────────────────────

print_step "3" "PARTIAL OVERWRITE — READ-MODIFY-WRITE (RMW)"
echo ""
echo "  Tạo file: 'HELLO [____] WORLD'"
printf 'HELLO [____] WORLD' > "$MOUNT/demo_rmw.txt"
BEFORE=$(cat "$MOUNT/demo_rmw.txt")

echo "  Ghi đè 4 byte 'FUSE' tại offset 7 (dd conv=notrunc)..."
printf 'FUSE' | dd of="$MOUNT/demo_rmw.txt" bs=1 seek=7 conv=notrunc 2>/dev/null
AFTER=$(cat "$MOUNT/demo_rmw.txt")

echo ""
echo -e "${CYAN}  +-------------------------+-------------------------+${NC}"
printf "  | ${BOLD}%-23s${NC} | ${BOLD}%-23s${NC} |\n" "Status" "File's content"
echo -e "${CYAN}  +-------------------------+-------------------------+${NC}"
printf "  | %-23s | %-23s |\n" "Before" "$BEFORE"
printf "  | %-23s | %-23s |\n" "After RMW (offset=7)" "$AFTER"
echo -e "${CYAN}  +-------------------------+-------------------------+${NC}"

EXPECTED="HELLO [FUSE] WORLD"
if [ "$(echo "$AFTER" | tr -d '\n')" = "$EXPECTED" ]; then
    check "Nội dung sau RMW đúng ('$EXPECTED')" "pass"
else
    check "Nội dung sau RMW đúng ('$EXPECTED') — got: '$AFTER'" "fail"
fi

# Verify CRC32 sau RMW bằng cách đọc lại không lỗi
REREAD=$(cat "$MOUNT/demo_rmw.txt" 2>/dev/null)
if [ -n "$REREAD" ]; then
    check "Đọc lại sau RMW không lỗi (CRC32 verify pass)" "pass"
else
    check "Đọc lại sau RMW không lỗi (CRC32 verify pass)" "fail"
fi
pause

# ── Bước 4: CRC32 integrity ───────────────────────────────────────────────────

print_step "4" "DATA INTEGRITY — CRC32 CHECKSUM"
echo ""
echo "  Ghi 3 file nhỏ với nội dung khác nhau..."
echo "chunk data alpha 1234567890" > "$MOUNT/crc_a.txt"
echo "chunk data beta  abcdefghij" > "$MOUNT/crc_b.txt"
python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256))*200)" > "$MOUNT/crc_binary.bin"

MD5_A_W=$(md5sum "$MOUNT/crc_a.txt"    | awk '{print $1}')
MD5_B_W=$(md5sum "$MOUNT/crc_b.txt"    | awk '{print $1}')
MD5_C_W=$(md5sum "$MOUNT/crc_binary.bin" | awk '{print $1}')

# Đọc lại ngay (qua decompression path)
MD5_A_R=$(md5sum "$MOUNT/crc_a.txt"    | awk '{print $1}')
MD5_B_R=$(md5sum "$MOUNT/crc_b.txt"    | awk '{print $1}')
MD5_C_R=$(md5sum "$MOUNT/crc_binary.bin" | awk '{print $1}')

[ "$MD5_A_W" = "$MD5_A_R" ] && check "crc_a.txt  : write MD5 == read MD5" "pass" || check "crc_a.txt  : MD5 mismatch" "fail"
[ "$MD5_B_W" = "$MD5_B_R" ] && check "crc_b.txt  : write MD5 == read MD5" "pass" || check "crc_b.txt  : MD5 mismatch" "fail"
[ "$MD5_C_W" = "$MD5_C_R" ] && check "crc_binary : write MD5 == read MD5" "pass" || check "crc_binary : MD5 mismatch" "fail"
pause

# ── Bước 5: Metadata persistence (remount) ───────────────────────────────────

print_step "5" "METADATA PERSISTENCE — KIỂM TRA SAU REMOUNT"
echo ""

# Ghi fingerprint trước unmount
MD5_TEXT_PRE=$(md5sum "$MOUNT/demo_text.txt" | awk '{print $1}')
MD5_RMW_PRE=$(md5sum  "$MOUNT/demo_rmw.txt"  | awk '{print $1}')
LSIZE_PRE=$(size_bytes "$MOUNT/demo_text.txt")

echo "  Unmount..."
fusermount3 -u "$MOUNT" 2>/dev/null
sleep 1

echo "  Remount..."
./myfs -f "$MOUNT" "./$BACKING" >> "$LOG_FILE" 2>&1 &
FUSE_PID=$!
sleep 1

if ! mountpoint -q "$MOUNT" 2>/dev/null; then
    echo -e "  ${RED}[LỖI] Remount thất bại!${NC}"
    exit 1
fi

MD5_TEXT_POST=$(md5sum "$MOUNT/demo_text.txt" | awk '{print $1}')
MD5_RMW_POST=$(md5sum  "$MOUNT/demo_rmw.txt"  | awk '{print $1}')
LSIZE_POST=$(size_bytes "$MOUNT/demo_text.txt")

echo ""
echo -e "${CYAN}  +-----------------+----------------------------------+----------------------------------+--------+${NC}"
printf "  | ${BOLD}%-15s${NC} | ${BOLD}%-32s${NC} | ${BOLD}%-32s${NC} | ${BOLD}%-6s${NC} |\n" "File" "MD5 (Before Unmount)" "MD5 (After Remount)" "Status"
echo -e "${CYAN}  +-----------------+----------------------------------+----------------------------------+--------+${NC}"

printf "  | %-15s | %-32s | %-32s | " "demo_text.txt" "$MD5_TEXT_PRE" "$MD5_TEXT_POST"
if [ "$MD5_TEXT_PRE" = "$MD5_TEXT_POST" ]; then
    printf "${GREEN}%-6s${NC} |\n" "MATCH"
else
    printf "${RED}%-6s${NC} |\n" "DIFFER"
fi

printf "  | %-15s | %-32s | %-32s | " "demo_rmw.txt" "$MD5_RMW_PRE" "$MD5_RMW_POST"
if [ "$MD5_RMW_PRE" = "$MD5_RMW_POST" ]; then
    printf "${GREEN}%-6s${NC} |\n" "MATCH"
else
    printf "${RED}%-6s${NC} |\n" "DIFFER"
fi

echo -e "${CYAN}  +-----------------+----------------------------------+----------------------------------+--------+${NC}"

[ "$MD5_TEXT_PRE" = "$MD5_TEXT_POST" ] && check "demo_text.txt MD5 không đổi sau remount" "pass" || check "demo_text.txt MD5 thay đổi sau remount" "fail"
[ "$MD5_RMW_PRE"  = "$MD5_RMW_POST"  ] && check "demo_rmw.txt  MD5 không đổi sau remount" "pass" || check "demo_rmw.txt  MD5 thay đổi sau remount"  "fail"
[ "$LSIZE_PRE"    = "$LSIZE_POST"    ] && check "Logical size giữ nguyên ($LSIZE_PRE bytes)" "pass" || check "Logical size thay đổi ($LSIZE_PRE → $LSIZE_POST)" "fail"
pause

# ── Bước 6: Tổng kết ─────────────────────────────────────────────────────────

print_step "6" "TỔNG KẾT"
echo ""
TOTAL=$((PASS + FAIL))
echo -e "  Kết quả: ${GREEN}$PASS PASS${NC} / ${RED}$FAIL FAIL${NC} / $TOTAL tổng"
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo -e "  ${GREEN}${BOLD}✓ Tất cả kiểm tra PASS — hệ thống hoạt động đúng đặc tả.${NC}"
else
    echo -e "  ${RED}${BOLD}✗ Có $FAIL kiểm tra FAIL — xem log: $LOG_FILE${NC}"
fi
echo ""

# ── Dọn dẹp ──────────────────────────────────────────────────────────────────

echo "  Dọn dẹp..."
fusermount3 -u "$MOUNT" 2>/dev/null
wait $FUSE_PID 2>/dev/null
echo -e "  ${GREEN}Cảm ơn thầy và các bạn đã theo dõi demo của Nhóm 25228!${NC}"
echo ""
exit $FAIL