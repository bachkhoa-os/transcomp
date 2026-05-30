#!/bin/bash
# =============================================================================
# demo.sh — Kịch bản Demo Bảo vệ Project HĐH (Transparent Compression)
# Project No: 25228 — Nhóm: Hùng, Đại, Toàn
# Có tích hợp ASCII Tables để hiển thị trực quan
# =============================================================================

MOUNT="mountpoint"
BACKING="backing"
LOG_FILE="fuse_demo.log"

# Màu sắc
CYAN='\033[1;36m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
RED='\033[1;31m'
NC='\033[0m'

pause() {
    echo ""
    read -p "$(echo -e ${YELLOW}"[Ấn Enter để tiếp tục demo...]"${NC})"
    echo ""
}

print_step() {
    echo -e "${CYAN}================================================================================${NC}"
    echo -e "${CYAN} BƯỚC $1: $2${NC}"
    echo -e "${CYAN}================================================================================${NC}"
}

# 1. Dọn dẹp và Khởi động
clear
echo -e "${GREEN}BẮT ĐẦU KỊCH BẢN DEMO - TRANSPARENT COMPRESSION FILE SYSTEM${NC}"
echo "Đang dọn dẹp môi trường cũ..."
fusermount3 -u $MOUNT 2>/dev/null
rm -rf $BACKING/* $MOUNT/* 2>/dev/null
mkdir -p $MOUNT $BACKING

echo "Đang biên dịch lại source code..."
make clean > /dev/null && make > /dev/null

echo "Đang mount file system (FUSE chạy ngầm, log ghi vào $LOG_FILE)..."
./myfs -f $MOUNT ./$BACKING > $LOG_FILE 2>&1 &
FUSE_PID=$!
sleep 1

if mountpoint -q "$MOUNT" 2>/dev/null; then
    echo -e "${GREEN}[OK] File system đã được mount tại ./$MOUNT${NC}"
else
    echo -e "${RED}[LỖI] Không thể mount FUSE!${NC}"
    exit 1
fi
pause

# 2. Tính năng cốt lõi: Nén tự động (Transparent Compression)
print_step "1" "DEMO TRANSPARENT COMPRESSION (DỮ LIỆU VĂN BẢN)"
echo "=> Tạo một file văn bản lặp lại kích thước 20MB (Compressible)..."
python3 -c "import sys; sys.stdout.buffer.write(b'Dai Hoc Bach Khoa Ha Noi - OS Project 2026\n' * 500000)" > $MOUNT/demo_text.txt
echo -e "${GREEN}[Xong] Đã tạo file $MOUNT/demo_text.txt${NC}"

L_SIZE=$(ls -lh $MOUNT/demo_text.txt | awk '{print $5}')
P_SIZE=$(ls -lh $BACKING/demo_text.txt.data | awk '{print $5}')

LOGICAL_B=$(stat -c%s "$MOUNT/demo_text.txt")
DISK_B=$(stat -c%s "$BACKING/demo_text.txt.data")
SAVED=$(python3 -c "print(f'{(1 - $DISK_B/$LOGICAL_B)*100:.2f}%')")

echo ""
echo "=> BẢNG SO SÁNH DUNG LƯỢNG (TEXT FILE):"
echo -e "${CYAN}+--------------------+--------------------------------+-------------+${NC}"
echo -e "${CYAN}| Goc nhin (View)    | Duong dan file (Path)          | Dung luong  |${NC}"
echo -e "${CYAN}+--------------------+--------------------------------+-------------+${NC}"
printf "| %-18s | %-30s | %-11s |\n" "Logical (User)" "$MOUNT/demo_text.txt" "$L_SIZE"
printf "| %-18s | %-30s | %-11s |\n" "Physical (Disk)" "$BACKING/demo_text.txt.data" "$P_SIZE"
echo -e "${CYAN}+--------------------+--------------------------------+-------------+${NC}"
echo -e "=> ${GREEN}Tiết kiệm được: $SAVED không gian lưu trữ!${NC}"
pause

# 3. Tính năng Heuristic: Bỏ qua nén với dữ liệu không phù hợp
print_step "2" "HEURISTIC SKIP COMPRESSION (DỮ LIỆU ĐÃ NÉN/BINARY)"
echo "=> Tạo file giả lập định dạng JPEG (Incompressible) kích thước 5MB..."
python3 -c "
import sys, os
header = bytes([0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10]) + b'JFIF\x00'
payload = os.urandom(5 * 1024 * 1024 - len(header))
sys.stdout.buffer.write(header + payload)
" > $MOUNT/demo_image.jpg
echo -e "${GREEN}[Xong] Đã tạo file $MOUNT/demo_image.jpg${NC}"

L_SIZE_JPG=$(ls -lh $MOUNT/demo_image.jpg | awk '{print $5}')
P_SIZE_JPG=$(ls -lh $BACKING/demo_image.jpg.data | awk '{print $5}')

echo ""
echo "=> BẢNG SO SÁNH DUNG LƯỢNG (JPEG FILE):"
echo -e "${CYAN}+--------------------+--------------------------------+-------------+${NC}"
echo -e "${CYAN}| Goc nhin (View)    | Duong dan file (Path)          | Dung luong  |${NC}"
echo -e "${CYAN}+--------------------+--------------------------------+-------------+${NC}"
printf "| %-18s | %-30s | %-11s |\n" "Logical (User)" "$MOUNT/demo_image.jpg" "$L_SIZE_JPG"
printf "| %-18s | %-30s | %-11s |\n" "Physical (Disk)" "$BACKING/demo_image.jpg.data" "$P_SIZE_JPG"
echo -e "${CYAN}+--------------------+--------------------------------+-------------+${NC}"
echo -e "=> ${YELLOW}Nhờ nhận diện Magic Bytes, thuật toán Zstd được bỏ qua (Raw Data) giúp tiết kiệm CPU!${NC}"
pause

# 4. Tính năng khó nhất: Read-Modify-Write (RMW)
print_step "3" "GHI ĐÈ TỪNG PHẦN - READ-MODIFY-WRITE (RMW)"
echo "=> Tạo một file văn bản nhỏ..."
echo "HELLO OPERATING SYSTEM" > $MOUNT/demo_rmw.txt
CONTENT_BEFORE=$(cat $MOUNT/demo_rmw.txt)

echo "=> Tiến hành ghi đè chữ 'FUSE' vào giữa file (Offset = 6)..."
printf 'FUSE' | dd of=$MOUNT/demo_rmw.txt bs=1 seek=6 conv=notrunc 2>/dev/null
CONTENT_AFTER=$(cat $MOUNT/demo_rmw.txt)

echo ""
echo "=> BẢNG SO SÁNH NỘI DUNG (PARTIAL OVERWRITE):"
echo -e "${CYAN}+--------------------+--------------------------------+${NC}"
echo -e "${CYAN}| Trang thai         | Noi dung file                  |${NC}"
echo -e "${CYAN}+--------------------+--------------------------------+${NC}"
printf "| %-18s | %-30s |\n" "Ban dau" "$CONTENT_BEFORE"
printf "| %-18s | %-30s |\n" "Sau RMW (offset=6)" "$CONTENT_AFTER"
echo -e "${CYAN}+--------------------+--------------------------------+${NC}"
echo -e "=> ${GREEN}Hệ thống đã tự ngầm đọc chunk nén lên RAM, patch chữ FUSE, nén lại và ghi xuống disk.${NC}"
pause

# 5. Persistence: Bền vững dữ liệu sau Remount
print_step "4" "METADATA PERSISTENCE (KIỂM TRA TÍNH TOÀN VẸN SAU REMOUNT)"
MD5_BEFORE=$(md5sum $MOUNT/demo_text.txt | awk '{print $1}')

echo "=> 1. Đã lấy mã MD5 của file demo_text.txt"
echo "=> 2. Tiến hành Unmount (Tắt File System)..."
fusermount3 -u $MOUNT
sleep 1

echo "=> 3. Tiến hành Remount (Khởi động lại File System)..."
./myfs -f $MOUNT ./$BACKING >> $LOG_FILE 2>&1 &
FUSE_PID=$!
sleep 1

echo "=> 4. Kiểm tra lại mã MD5..."
MD5_AFTER=$(md5sum $MOUNT/demo_text.txt | awk '{print $1}')

if [ "$MD5_BEFORE" == "$MD5_AFTER" ]; then
    STATUS="[KHOP]"
else
    STATUS="[LOI]"
fi

echo ""
echo "=> BẢNG ĐỐI CHIẾU MÃ BĂM (MD5):"
echo -e "${CYAN}+-----------------+----------------------------------+----------------------------------+----------+${NC}"
echo -e "${CYAN}| File            | MD5 Truoc (Unmount)              | MD5 Sau (Remount)                | Ket qua  |${NC}"
echo -e "${CYAN}+-----------------+----------------------------------+----------------------------------+----------+${NC}"
printf "| %-15s | %-32s | %-32s | %-8s |\n" "demo_text.txt" "$MD5_BEFORE" "$MD5_AFTER" "$STATUS"
echo -e "${CYAN}+-----------------+----------------------------------+----------------------------------+----------+${NC}"

if [ "$MD5_BEFORE" == "$MD5_AFTER" ]; then
    echo -e "=> ${GREEN}TUYỆT VỜI! Metadata và dữ liệu được nạp lại thành công 100%.${NC}"
else
    echo -e "=> ${RED}DỮ LIỆU BỊ CORRUPT!${NC}"
fi
pause

# 6. Teardown
print_step "5" "KẾT THÚC DEMO"
echo "Đang dọn dẹp..."
fusermount3 -u $MOUNT 2>/dev/null
kill $FUSE_PID 2>/dev/null
echo -e "${GREEN}Cảm ơn thầy và các bạn đã theo dõi phần demo của Nhóm 25228!${NC}"
echo ""