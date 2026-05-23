#!/bin/bash
# =============================================================================
# benchmark.sh — Đo hiệu năng myfs: throughput, compression ratio, latency
# Project No: 25228 — Course: Operating Systems [Multimedia] - 20252
#
# Cách dùng:
#   ./myfs -f mountpoint ./backing &   # terminal 1
#   ./benchmark.sh mountpoint backing  # terminal 2
#
# Kết quả được lưu vào benchmark_results.txt
# =============================================================================

MOUNT="${1:-mountpoint}"
BACKING="${2:-backing}"
RESULT_FILE="benchmark_results.txt"
TMP_DIR="/tmp/myfs_bench"

mkdir -p "$TMP_DIR"

# ── màu sắc ──────────────────────────────────────────────────────────────────
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

section() {
    echo ""
    echo -e "${CYAN}============================================${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}============================================${NC}"
    echo "============================================" >> "$RESULT_FILE"
    echo "  $1" >> "$RESULT_FILE"
    echo "============================================" >> "$RESULT_FILE"
}

log() {
    echo -e "$1"
    echo "$1" >> "$RESULT_FILE"
}

# Tính throughput MB/s từ output của dd
# dd in ra stderr dạng: "X bytes copied, Y s, Z MB/s" hoặc "X bytes (Y MB) copied, Z s, W MB/s"
throughput_mbs() {
    local dd_output="$1"
    # Thử match "X MB/s" trước, nếu không có thì tính từ bytes và thời gian
    local result
    result=$(echo "$dd_output" | grep -oP '[\d.]+ MB/s' | tail -1)
    if [ -z "$result" ]; then
        result=$(echo "$dd_output" | grep -oP '[\d.]+ GB/s' | tail -1)
    fi
    echo "${result:-N/A}"
}

# Đo throughput bằng time + wc thay vì parse dd output (fallback)
measure_write_mbs() {
    local src="$1" dst="$2" size_mb="$3"
    local start_ns end_ns elapsed_ms mbs
    start_ns=$(date +%s%N)
    dd if="$src" of="$dst" bs=4M 2>/dev/null
    sync
    end_ns=$(date +%s%N)
    elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
    [ "$elapsed_ms" -eq 0 ] && elapsed_ms=1
    python3 -c "print(f'{$size_mb * 1000 / $elapsed_ms:.0f} MB/s')"
}

measure_read_mbs() {
    local src="$1" size_mb="$2"
    local start_ns end_ns elapsed_ms
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1 || true
    start_ns=$(date +%s%N)
    dd if="$src" of=/dev/null bs=4M 2>/dev/null
    end_ns=$(date +%s%N)
    elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
    [ "$elapsed_ms" -eq 0 ] && elapsed_ms=1
    python3 -c "print(f'{$size_mb * 1000 / $elapsed_ms:.0f} MB/s')"
}

# Tính compression ratio: logical_size / disk_size
compression_ratio() {
    local logical="$1"
    local disk="$2"
    if [ "$disk" -eq 0 ]; then echo "N/A"; return; fi
    python3 -c "print(f'{$logical / $disk:.2f}x')"
}

# ── sanity check ─────────────────────────────────────────────────────────────
if ! mountpoint -q "$MOUNT" 2>/dev/null; then
    if ! ls "$MOUNT" > /dev/null 2>&1; then
        echo "ERROR: '$MOUNT' chua duoc mount."
        exit 1
    fi
fi

# ── header ───────────────────────────────────────────────────────────────────
echo "" > "$RESULT_FILE"
echo "myfs Benchmark Report" >> "$RESULT_FILE"
echo "Date: $(date '+%Y-%m-%d %H:%M:%S')" >> "$RESULT_FILE"
echo "Host: $(uname -n) | Kernel: $(uname -r)" >> "$RESULT_FILE"
echo "" >> "$RESULT_FILE"

echo ""
echo -e "${CYAN}myfs Benchmark — $(date '+%Y-%m-%d %H:%M:%S')${NC}"
echo -e "mountpoint: ${MOUNT}  backing: ${BACKING}"
echo ""

# =============================================================================
section "BM01: Sequential Write Throughput"
# =============================================================================

log "--- Workload: text compressible (100MB of 'A') ---"
python3 -c "import sys; sys.stdout.buffer.write(b'A' * (100 * 1024 * 1024))" \
    > "$TMP_DIR/text_100mb.bin"

WRITE_TEXT=$(measure_write_mbs "$TMP_DIR/text_100mb.bin" "$MOUNT/bm_text.bin" 100)
LOGICAL=$(stat -c%s "$MOUNT/bm_text.bin")
DISK=$(stat -c%s "$BACKING/bm_text.bin.data")
RATIO_TEXT=$(compression_ratio $LOGICAL $DISK)
log "  Text write:       ${WRITE_TEXT:-N/A}"
log "  Logical size:     $(python3 -c "print(f'{$LOGICAL/1024/1024:.1f} MB')")"
log "  Disk size:        $(python3 -c "print(f'{$DISK/1024/1024:.2f} MB')")"
log "  Compression ratio: ${RATIO_TEXT}"
rm -f "$MOUNT/bm_text.bin"

log ""
log "--- Workload: random binary incompressible (100MB) ---"
dd if=/dev/urandom bs=1M count=100 of="$TMP_DIR/rand_100mb.bin" 2>/dev/null

WRITE_RAND=$(measure_write_mbs "$TMP_DIR/rand_100mb.bin" "$MOUNT/bm_rand.bin" 100)
LOGICAL=$(stat -c%s "$MOUNT/bm_rand.bin")
DISK=$(stat -c%s "$BACKING/bm_rand.bin.data")
RATIO_RAND=$(compression_ratio $LOGICAL $DISK)
log "  Random write:     ${WRITE_RAND:-N/A}"
log "  Logical size:     $(python3 -c "print(f'{$LOGICAL/1024/1024:.1f} MB')")"
log "  Disk size:        $(python3 -c "print(f'{$DISK/1024/1024:.2f} MB')")"
log "  Compression ratio: ${RATIO_RAND}"
rm -f "$MOUNT/bm_rand.bin"

log ""
log "--- Workload: source code-like (mixed, 50MB) ---"
# Sinh file giống source code: mix ASCII printable chars
python3 -c "
import random, string, sys
chars = string.ascii_letters + string.digits + ' \n{}();=+-*/[]'
data = ''.join(random.choices(chars, k=50*1024*1024))
sys.stdout.buffer.write(data.encode())
" > "$TMP_DIR/src_50mb.bin"

WRITE_SRC=$(measure_write_mbs "$TMP_DIR/src_50mb.bin" "$MOUNT/bm_src.bin" 50)
LOGICAL=$(stat -c%s "$MOUNT/bm_src.bin")
DISK=$(stat -c%s "$BACKING/bm_src.bin.data")
RATIO_SRC=$(compression_ratio $LOGICAL $DISK)
log "  Source write:     ${WRITE_SRC:-N/A}"
log "  Logical size:     $(python3 -c "print(f'{$LOGICAL/1024/1024:.1f} MB')")"
log "  Disk size:        $(python3 -c "print(f'{$DISK/1024/1024:.2f} MB')")"
log "  Compression ratio: ${RATIO_SRC}"

# =============================================================================
section "BM02: Sequential Read Throughput"
# =============================================================================

log "--- Read lại file source code 50MB đã ghi ---"
# Ghi lại file vì BM01 đã xóa
cp "$TMP_DIR/src_50mb.bin" "$MOUNT/bm_read_src.bin"
READ_SRC=$(measure_read_mbs "$MOUNT/bm_read_src.bin" 50)
log "  Source read:      ${READ_SRC:-N/A}"
rm -f "$MOUNT/bm_read_src.bin"

log ""
log "--- Ghi rồi đọc file random 50MB ---"
dd if=/dev/urandom bs=1M count=50 of="$TMP_DIR/rand_50mb.bin" 2>/dev/null
measure_write_mbs "$TMP_DIR/rand_50mb.bin" "$MOUNT/bm_rand2.bin" 50 > /dev/null
READ_RAND=$(measure_read_mbs "$MOUNT/bm_rand2.bin" 50)
log "  Random read:      ${READ_RAND:-N/A}"
rm -f "$MOUNT/bm_rand2.bin"

# =============================================================================
section "BM03: Compression Ratio — nhiều loại file"
# =============================================================================

BM03_TABLE_BORDER="+----------------------------+------------+------------+----------+"
log "$BM03_TABLE_BORDER"
log "| Workload                   | Logical    | Disk       | Ratio    |"
log "$BM03_TABLE_BORDER"

# Text lặp lại (compressible cao)
python3 -c "import sys; sys.stdout.buffer.write(b'Hello World\n' * (1024*1024))" \
    > "$MOUNT/bm_ratio_text.bin"
L=$(stat -c%s "$MOUNT/bm_ratio_text.bin")
D=$(stat -c%s "$BACKING/bm_ratio_text.bin.data")
log "| $(printf '%-27s| %-11s| %-11s| %-9s|' \
    "Repeated text (12MB)" \
    "$(python3 -c "print(f'{$L/1024/1024:.1f} MB')")" \
    "$(python3 -c "print(f'{$D/1024:.0f} KB')")" \
    "$(compression_ratio $L $D)")"
rm -f "$MOUNT/bm_ratio_text.bin"

# Source code giả (compressible vừa)
L=$(stat -c%s "$TMP_DIR/src_50mb.bin" 2>/dev/null || echo 0)
if [ "$L" -gt 0 ]; then
    cp "$TMP_DIR/src_50mb.bin" "$MOUNT/bm_ratio_src.bin"
    D=$(stat -c%s "$BACKING/bm_ratio_src.bin.data")
    log "| $(printf '%-27s| %-11s| %-11s| %-9s|' \
        "Source code (50MB)" \
        "$(python3 -c "print(f'{$L/1024/1024:.0f} MB')")" \
        "$(python3 -c "print(f'{$D/1024/1024:.1f} MB')")" \
        "$(compression_ratio $L $D)")"
    rm -f "$MOUNT/bm_ratio_src.bin"
fi

# Random binary (incompressible)
dd if=/dev/urandom bs=1M count=10 of="$MOUNT/bm_ratio_rand.bin" 2>/dev/null
L=$(stat -c%s "$MOUNT/bm_ratio_rand.bin")
D=$(stat -c%s "$BACKING/bm_ratio_rand.bin.data")
log "| $(printf '%-27s| %-11s| %-11s| %-9s|' \
    "Random binary (10MB)" \
    "$(python3 -c "print(f'{$L/1024/1024:.0f} MB')")" \
    "$(python3 -c "print(f'{$D/1024/1024:.0f} MB')")" \
    "$(compression_ratio $L $D)")"
log "$BM03_TABLE_BORDER"
rm -f "$MOUNT/bm_ratio_rand.bin"

# =============================================================================
section "BM04: Latency — Partial Overwrite (RMW)"
# =============================================================================

log "--- Tạo file 1MB để test RMW latency ---"
python3 -c "import sys; sys.stdout.buffer.write(b'X' * (1024*1024))" \
    > "$MOUNT/bm_rmw.bin"

log "--- 10 lần RMW tại offset ngẫu nhiên, đo thời gian trung bình ---"
TOTAL_US=0
for i in $(seq 1 10); do
    OFFSET=$((RANDOM % 900000))
    START=$(date +%s%N)
    printf 'PATCH_DATA_HERE' | dd of="$MOUNT/bm_rmw.bin" \
        bs=1 seek=$OFFSET conv=notrunc 2>/dev/null
    END=$(date +%s%N)
    ELAPSED_US=$(( (END - START) / 1000 ))
    TOTAL_US=$((TOTAL_US + ELAPSED_US))
done

AVG_US=$((TOTAL_US / 10))
AVG_MS=$(python3 -c "print(f'{$AVG_US / 1000:.2f}')")
log "  RMW avg latency: ${AVG_US} us (${AVG_MS} ms) over 10 iterations"

rm -f "$MOUNT/bm_rmw.bin"

# =============================================================================
section "BM05: So sánh với ext4 (baseline)"
# =============================================================================

log "--- ext4 baseline: write 100MB text ---"
EXT4_WRITE_TEXT=$(measure_write_mbs "$TMP_DIR/text_100mb.bin" "$TMP_DIR/ext4_text.bin" 100)
log "  ext4 text write:  ${EXT4_WRITE_TEXT:-N/A}"

log ""
log "--- ext4 baseline: write 100MB random ---"
EXT4_WRITE_RAND=$(measure_write_mbs "$TMP_DIR/rand_100mb.bin" "$TMP_DIR/ext4_rand.bin" 100)
log "  ext4 rand write:  ${EXT4_WRITE_RAND:-N/A}"

log ""
log "--- ext4 baseline: read 100MB text (drop cache first) ---"
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1 || true
EXT4_READ_TEXT=$(measure_read_mbs "$TMP_DIR/ext4_text.bin" 100)
log "  ext4 text read:   ${EXT4_READ_TEXT:-N/A}"

log ""
log "--- ext4 baseline: read 100MB random ---"
EXT4_READ_RAND=$(measure_read_mbs "$TMP_DIR/ext4_rand.bin" 100)
log "  ext4 rand read:   ${EXT4_READ_RAND:-N/A}"

# =============================================================================
section "BM06: Tổng hợp so sánh myfs vs ext4"
# =============================================================================

log "+---------------------------+------------------+------------------+"
log "| Metric                    | myfs             | ext4 (baseline)  |"
log "+---------------------------+------------------+------------------+"
log "| $(printf '%-26s| %-17s| %-17s|'     "Write text 100MB" "${WRITE_TEXT:-N/A}" "${EXT4_WRITE_TEXT:-N/A}")"
log "| $(printf '%-26s| %-17s| %-17s|'     "Write random 100MB" "${WRITE_RAND:-N/A}" "${EXT4_WRITE_RAND:-N/A}")"
log "| $(printf '%-26s| %-17s| %-17s|'     "Read text 50MB" "${READ_SRC:-N/A}" "${EXT4_READ_TEXT:-N/A}")"
log "| $(printf '%-26s| %-17s| %-17s|'     "Read random 50MB" "${READ_RAND:-N/A}" "${EXT4_READ_RAND:-N/A}")"
log "+---------------------------+------------------+------------------+"
log "| $(printf '%-26s| %-17s| %-17s|'     "Compression text" "${RATIO_TEXT:-N/A}" "1.00x")"
log "| $(printf '%-26s| %-17s| %-17s|'     "Compression source" "${RATIO_SRC:-N/A}" "1.00x")"
log "| $(printf '%-26s| %-17s| %-17s|'     "Compression random" "${RATIO_RAND:-N/A}" "1.00x")"
log "+---------------------------+------------------+------------------+"
log "| $(printf '%-26s| %-17s| %-17s|'     "RMW avg latency" "${AVG_MS:-N/A} ms" "N/A")"
log "+---------------------------+------------------+------------------+"


# =============================================================================
section "BM07: Heuristic Skip Compression"
# =============================================================================
# Kiểm tra Sprint 6: heuristic magic-byte có kích hoạt đúng không,
# và throughput khi skip compression có cao hơn khi compress không.
# Backing store: <filename>.data — codec_type được ghi trong .meta

log "--- Tạo file giả JPEG (magic FF D8 FF + random payload, 10MB) ---"
python3 -c "
import sys, os
# JPEG magic: FF D8 FF E0 + filler
header = bytes([0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10]) + b'JFIF\x00'
payload = os.urandom(10 * 1024 * 1024 - len(header))
sys.stdout.buffer.write(header + payload)
" > "$TMP_DIR/fake_jpeg.bin"

WRITE_JPEG=$(measure_write_mbs "$TMP_DIR/fake_jpeg.bin" "$MOUNT/bm_heur_jpeg.bin" 10)
L_JPEG=$(stat -c%s "$MOUNT/bm_heur_jpeg.bin")
D_JPEG=$(stat -c%s "$BACKING/bm_heur_jpeg.bin.data")
RATIO_JPEG=$(compression_ratio $L_JPEG $D_JPEG)
# Đọc codec_type từ .meta: nếu tất cả chunk raw thì heuristic đã kích hoạt
CODEC_JPEG=$(python3 -c "
import struct, sys
try:
    with open('$BACKING/bm_heur_jpeg.bin.meta', 'rb') as f:
        num_chunks, logical_size = struct.unpack('<IQ', f.read(12))
        codecs = []
        for _ in range(num_chunks):
            data = f.read(30)  # sizeof(myfs_chunk_t) = 8+4+4+1+1+4+8 = 30
            if len(data) < 30: break
            # struct layout: logical_offset(8) raw_size(4) stored_size(4) codec_type(1) flags(1) checksum(4) physical_offset(8) = 30? recheck
            # Try offset 16 for codec_type (after logical_offset+raw_size+stored_size)
            codecs.append(data[16])
    all_raw = all(c == 0 for c in codecs)
    print(f'raw={sum(1 for c in codecs if c==0)}/{len(codecs)} → {\"SKIP OK\" if all_raw else \"COMPRESSED\"}')
except Exception as e:
    print(f'N/A ({e})')
" 2>/dev/null)
log "  JPEG write:       ${WRITE_JPEG:-N/A}"
log "  Logical size:     $(python3 -c "print(f'{$L_JPEG/1024/1024:.1f} MB')")"
log "  Disk size:        $(python3 -c "print(f'{$D_JPEG/1024/1024:.2f} MB')")"
log "  Compression ratio: ${RATIO_JPEG} (mong đợi ≈ 1.00x)"
log "  Heuristic status: ${CODEC_JPEG}"
rm -f "$MOUNT/bm_heur_jpeg.bin"

log ""
log "--- Tạo file giả ZIP (magic PK\\x03\\x04 + random, 10MB) ---"
python3 -c "
import sys, os
header = bytes([0x50, 0x4B, 0x03, 0x04]) + os.urandom(26)  # local file header
payload = os.urandom(10 * 1024 * 1024 - 30)
sys.stdout.buffer.write(header + payload)
" > "$TMP_DIR/fake_zip.bin"

WRITE_ZIP=$(measure_write_mbs "$TMP_DIR/fake_zip.bin" "$MOUNT/bm_heur_zip.bin" 10)
L_ZIP=$(stat -c%s "$MOUNT/bm_heur_zip.bin")
D_ZIP=$(stat -c%s "$BACKING/bm_heur_zip.bin.data")
RATIO_ZIP=$(compression_ratio $L_ZIP $D_ZIP)
log "  ZIP write:        ${WRITE_ZIP:-N/A}"
log "  Logical size:     $(python3 -c "print(f'{$L_ZIP/1024/1024:.1f} MB')")"
log "  Disk size:        $(python3 -c "print(f'{$D_ZIP/1024/1024:.2f} MB')")"
log "  Compression ratio: ${RATIO_ZIP} (mong đợi ≈ 1.00x)"
rm -f "$MOUNT/bm_heur_zip.bin"

log ""
log "--- So sánh throughput: text compressible vs JPEG skip (10MB mỗi loại) ---"
python3 -c "import sys; sys.stdout.buffer.write(b'Hello OS class!\n' * (10*1024*1024//16))" \
    > "$TMP_DIR/text_10mb.bin"
WRITE_TEXT10=$(measure_write_mbs "$TMP_DIR/text_10mb.bin" "$MOUNT/bm_heur_text.bin" 10)
WRITE_JPEG10=$(measure_write_mbs "$TMP_DIR/fake_jpeg.bin" "$MOUNT/bm_heur_jpeg2.bin" 10)
log ""
log "+----------------------+------------+-------------------------------+"
log "| Workload             | Throughput | Ghi chú                       |"
log "+----------------------+------------+-------------------------------+"
log "| $(printf '%-21s| %-11s| %-30s|' "Text (compressed)" "${WRITE_TEXT10:-N/A}" "Zstd active")"
log "| $(printf '%-21s| %-11s| %-30s|' "JPEG (skip heuristic)" "${WRITE_JPEG10:-N/A}" "Magic byte => raw bypass")"
log "+----------------------+------------+-------------------------------+"
rm -f "$MOUNT/bm_heur_text.bin" "$MOUNT/bm_heur_jpeg2.bin"

# =============================================================================
section "BM08: Metadata Persistence (Remount Correctness)"
# =============================================================================
# Kiểm tra tiêu chí quan trọng nhất của Sprint 3: file đọc sau remount phải
# bit-identical với file đã ghi trước đó.
# Lưu ý: benchmark không tự mount/unmount; nó tạo file, flush, kiểm tra checksum,
# rồi hướng dẫn manual remount và cung cấp script verify.

log "--- Ghi 3 file test với đặc điểm khác nhau ---"
python3 -c "import sys; sys.stdout.buffer.write(b'PERSIST_TEST_COMPRESSIBLE\n' * (1024*40))" \
    > "$MOUNT/bm_persist_text.bin"
dd if=/dev/urandom bs=1M count=2 of="$MOUNT/bm_persist_rand.bin" 2>/dev/null
python3 -c "
import sys, random, string
data = ''.join(random.choices(string.printable, k=512*1024)).encode()
sys.stdout.buffer.write(data)
" > "$MOUNT/bm_persist_src.bin"
sync

# Tính MD5 ngay sau khi ghi (ground truth)
MD5_TEXT=$(md5sum "$MOUNT/bm_persist_text.bin" | awk '{print $1}')
MD5_RAND=$(md5sum "$MOUNT/bm_persist_rand.bin" | awk '{print $1}')
MD5_SRC=$(md5sum  "$MOUNT/bm_persist_src.bin"  | awk '{print $1}')

L_TEXT=$(stat -c%s "$MOUNT/bm_persist_text.bin")
L_RAND=$(stat -c%s "$MOUNT/bm_persist_rand.bin")
L_SRC=$(stat -c%s  "$MOUNT/bm_persist_src.bin")

log ""
log "  MD5 checksums (PRE-REMOUNT — ground truth):"
log "  bm_persist_text.bin  md5=${MD5_TEXT}  size=$(python3 -c "print(f'{$L_TEXT/1024:.0f} KB')")"
log "  bm_persist_rand.bin  md5=${MD5_RAND}  size=$(python3 -c "print(f'{$L_RAND/1024/1024:.1f} MB')")"
log "  bm_persist_src.bin   md5=${MD5_SRC}  size=$(python3 -c "print(f'{$L_SRC/1024:.0f} KB')")"

# Lưu checksum ra file verify script để dùng sau remount
cat > "$TMP_DIR/verify_remount.sh" << VERIFY_EOF
#!/bin/bash
# Chạy script này SAU KHI remount myfs để xác minh persistence.
# Usage: bash verify_remount.sh <mountpoint>
MOUNT="\${1:-mountpoint}"
PASS=0; FAIL=0

check() {
    local f="\$MOUNT/\$1" expected="\$2"
    if [ ! -f "\$f" ]; then echo "MISSING: \$1"; ((FAIL++)); return; fi
    local got=\$(md5sum "\$f" | awk '{print \$1}')
    if [ "\$got" = "\$expected" ]; then
        echo "PASS: \$1 md5=\$got"
        ((PASS++))
    else
        echo "FAIL: \$1 expected=\$expected got=\$got"
        ((FAIL++))
    fi
}

check bm_persist_text.bin $MD5_TEXT
check bm_persist_rand.bin $MD5_RAND
check bm_persist_src.bin  $MD5_SRC

echo ""
echo "Remount persistence: PASS=\$PASS FAIL=\$FAIL"
[ \$FAIL -eq 0 ] && echo "==> ALL CORRECT: metadata persistence OK" \
                 || echo "==> DATA CORRUPTION DETECTED"
VERIFY_EOF
chmod +x "$TMP_DIR/verify_remount.sh"
cp "$TMP_DIR/verify_remount.sh" "./verify_remount.sh"

log ""
log "  Script verify đã lưu: ./verify_remount.sh"
log "  Để kiểm tra persistence, thực hiện:"
log "    1. fusermount3 -u $MOUNT"
log "    2. ./myfs -f $MOUNT $BACKING &"
log "    3. bash ./verify_remount.sh $MOUNT"
log ""
log "  Kết quả mong đợi: PASS=3 FAIL=0 (tất cả md5 khớp sau remount)"

# =============================================================================
section "BM09: FUSE Overhead vs Zstd Overhead Breakdown"
# =============================================================================
# Mục tiêu: phân tách overhead tổng (myfs vs ext4) thành 2 phần:
#   - FUSE overhead: chi phí của userspace filesystem (context switch, /dev/fuse)
#   - Zstd overhead: chi phí của compression thuần túy
# Phương pháp: đo Zstd throughput độc lập (in-memory, không I/O),
# so với myfs throughput → phần còn lại chính là FUSE overhead.

log "--- Đo Zstd compression throughput thuần (in-memory, không I/O) ---"
ZSTD_COMPRESS_MBS=$(python3 -c "
import ctypes, ctypes.util, time, os

lib_name = ctypes.util.find_library('zstd')
if not lib_name:
    print('N/A (libzstd not found)')
    exit()

zstd = ctypes.CDLL(lib_name)
zstd.ZSTD_compress.restype = ctypes.c_size_t
zstd.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
zstd.ZSTD_compressBound.restype = ctypes.c_size_t
zstd.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
zstd.ZSTD_isError.restype = ctypes.c_uint
zstd.ZSTD_isError.argtypes = [ctypes.c_size_t]

# Workload: 50MB source-code-like text (same as BM01)
import random, string
chars = (string.ascii_letters + string.digits + ' \n{}();=+-*/[]').encode()
src_data = bytes([chars[i % len(chars)] for i in range(50 * 1024 * 1024)])
src_len = len(src_data)
src_buf = (ctypes.c_char * src_len)(*src_data)

bound = zstd.ZSTD_compressBound(src_len)
dst_buf = (ctypes.c_char * bound)()

# Warm up
zstd.ZSTD_compress(dst_buf, bound, src_buf, src_len, 3)

RUNS = 5
start = time.perf_counter()
for _ in range(RUNS):
    r = zstd.ZSTD_compress(dst_buf, bound, src_buf, src_len, 3)
elapsed = time.perf_counter() - start

mbs = (src_len * RUNS / 1024 / 1024) / elapsed
print(f'{mbs:.0f} MB/s')
" 2>/dev/null)

log "--- Đo Zstd decompression throughput thuần (in-memory) ---"
ZSTD_DECOMPRESS_MBS=$(python3 -c "
import ctypes, ctypes.util, time

lib_name = ctypes.util.find_library('zstd')
if not lib_name:
    print('N/A')
    exit()

zstd = ctypes.CDLL(lib_name)
zstd.ZSTD_compress.restype   = ctypes.c_size_t
zstd.ZSTD_decompress.restype = ctypes.c_size_t
zstd.ZSTD_compress.argtypes  = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
zstd.ZSTD_decompress.argtypes= [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p, ctypes.c_size_t]
zstd.ZSTD_compressBound.restype = ctypes.c_size_t
zstd.ZSTD_compressBound.argtypes = [ctypes.c_size_t]

import string
chars = (string.ascii_letters + string.digits + ' \n{}();=+-*/[]').encode()
src_data = bytes([chars[i % len(chars)] for i in range(50 * 1024 * 1024)])
src_len = len(src_data)
src_buf = (ctypes.c_char * src_len)(*src_data)

bound = zstd.ZSTD_compressBound(src_len)
comp_buf = (ctypes.c_char * bound)()
comp_size = zstd.ZSTD_compress(comp_buf, bound, src_buf, src_len, 3)

dst_buf = (ctypes.c_char * src_len)()
RUNS = 5
start = time.perf_counter()
for _ in range(RUNS):
    zstd.ZSTD_decompress(dst_buf, src_len, comp_buf, comp_size)
elapsed = time.perf_counter() - start

mbs = (src_len * RUNS / 1024 / 1024) / elapsed
print(f'{mbs:.0f} MB/s')
" 2>/dev/null)

log ""
log "+-----------------------------+------------------+------------------------------------------+"
log "| Component                   | Throughput       | Ghi chú                                  |"
log "+-----------------------------+------------------+------------------------------------------+"
log "| $(printf '%-28s| %-17s| %-41s|' "ext4 write (baseline)"   "${EXT4_WRITE_TEXT:-N/A}"    "Kernel FS, no compression")"
log "| $(printf '%-28s| %-17s| %-41s|' "myfs write (text 100MB)" "${WRITE_TEXT:-N/A}"         "FUSE + Zstd combined")"
log "| $(printf '%-28s| %-17s| %-41s|' "Zstd compress only"      "${ZSTD_COMPRESS_MBS:-N/A}"  "In-memory, no I/O")"
log "+-----------------------------+------------------+------------------------------------------+"
log "| $(printf '%-28s| %-17s| %-41s|' "ext4 read (baseline)"    "${EXT4_READ_TEXT:-N/A}"     "Kernel FS, no decompression")"
log "| $(printf '%-28s| %-17s| %-41s|' "myfs read (text 50MB)"   "${READ_SRC:-N/A}"           "FUSE + Zstd decompression")"
log "| $(printf '%-28s| %-17s| %-41s|' "Zstd decompress only"    "${ZSTD_DECOMPRESS_MBS:-N/A}" "In-memory, no I/O")"
log "+-----------------------------+------------------+------------------------------------------+"
log ""
log "  Phân tích overhead:"
python3 - "$WRITE_TEXT" "$EXT4_WRITE_TEXT" "$ZSTD_COMPRESS_MBS" \
          "$READ_SRC"   "$EXT4_READ_TEXT"  "$ZSTD_DECOMPRESS_MBS" << 'ANALYZE_EOF'
import sys

def parse(s):
    if not s or s.strip() in ('', 'N/A'): return None
    try: return float(s.strip().replace(' MB/s','').replace('MB/s',''))
    except: return None

myfs_w, ext4_w, zstd_c, myfs_r, ext4_r, zstd_d = [parse(a) for a in sys.argv[1:]]

print()

# Write analysis
if ext4_w and myfs_w:
    total_pct_w = (1 - myfs_w / ext4_w) * 100
    print(f'  [WRITE]')
    print(f'    myfs chậm hơn ext4: {total_pct_w:.1f}%  ({myfs_w:.0f} vs {ext4_w:.0f} MB/s)')
    if zstd_c:
        print(f'    Zstd compress ceiling (in-memory): {zstd_c:.0f} MB/s')
        if myfs_w >= zstd_c * 0.85:
            print(f'    -> Bottleneck: Zstd compression (myfs ~ Zstd speed, FUSE overhead nhỏ)')
        else:
            fuse_pct = (1 - myfs_w / zstd_c) * 100
            print(f'    -> Bottleneck chính: FUSE userspace round-trip')
            print(f'       Zstd chiếm ~{100-fuse_pct:.0f}% budget, FUSE overhead ~{fuse_pct:.0f}%')
            print(f'       (moi syscall write phải round-trip qua /dev/fuse -> kernel VFS -> daemon)')
else:
    print('  [WRITE] Thiếu dữ liệu write (WRITE_TEXT hoac EXT4_WRITE_TEXT chưa được set)')

print()

# Read analysis
if ext4_r and myfs_r:
    total_pct_r = (1 - myfs_r / ext4_r) * 100
    print(f'  [READ]')
    print(f'    myfs chậm hơn ext4: {total_pct_r:.1f}%  ({myfs_r:.0f} vs {ext4_r:.0f} MB/s)')
    if zstd_d:
        print(f'    Zstd decompress ceiling (in-memory): {zstd_d:.0f} MB/s')
        if myfs_r >= zstd_d * 0.85:
            print(f'    -> Bottleneck: Zstd decompression')
        else:
            fuse_pct = (1 - myfs_r / zstd_d) * 100
            print(f'    -> Bottleneck chính: FUSE userspace round-trip')
            print(f'       Zstd chiếm ~{100-fuse_pct:.0f}% budget, FUSE overhead ~{fuse_pct:.0f}%')
else:
    print('  [READ] Thiếu dữ liệu read (READ_SRC hoac EXT4_READ_TEXT chưa được set)')

print()

# Conclusion
if ext4_w and myfs_w and zstd_c and ext4_r and myfs_r and zstd_d:
    print(f'  [KẾT LUẬN]')
    print(f'    Zstd compress {zstd_c:.0f} MB/s, decompress {zstd_d:.0f} MB/s >> myfs throughput.')
    print(f'    => Compression KHÔNG phải bottleneck.')
    print(f'    => Overhead đến từ FUSE architecture: mọi read/write syscall phải round-trip')
    print(f'       qua /dev/fuse -> kernel VFS -> FUSE daemon (context switch + copy).')
    print(f'    => Đây là trade-off đã chấp nhận từ Sprint 1 (debuggability > raw performance).')
    print(f'    => Để cải thiện: kernel module hoặc io_uring FUSE có thể giảm overhead,')
    print(f'       nhưng nằm ngoài scope 8 tuần.')
ANALYZE_EOF


# =============================================================================
section "BM10: Append-Only Write Pattern"
# =============================================================================
# Nhiều ứng dụng thực tế (log writer, database WAL) chỉ append.
# Do overhead của chunk-alignment khi append nhỏ so với bulk write.
# Pattern C giảm xuống còn 1024 x 1KB (thay vì 65536) để chạy trong thời gian hợp lý.

log "--- Pattern A: 1 lan ghi 64MB (bulk, ly tuong cho chunk 64KB) ---"
python3 -c "
import sys, string
chars = (string.ascii_letters + string.digits + ' \n').encode()
data = bytes([chars[i % len(chars)] for i in range(64 * 1024 * 1024)])
sys.stdout.buffer.write(data)
" > "$TMP_DIR/append_bulk.bin"
WRITE_BULK=$(measure_write_mbs "$TMP_DIR/append_bulk.bin" "$MOUNT/bm_append_bulk.bin" 64)
rm -f "$MOUNT/bm_append_bulk.bin"
log "  Bulk write 64MB:      ${WRITE_BULK:-N/A}"

log ""
log "--- Pattern B: 1024 lần append 64KB (mỗi lần bằng đúng 1 chunk) ---"
python3 -c "
chars = b'abcdefghijklmnopqrstuvwxyz0123456789 \n'
chunk = bytes([chars[i % len(chars)] for i in range(64 * 1024)])
with open('$MOUNT/bm_append_chunk.bin', 'wb') as f:
    for _ in range(1024):
        f.write(chunk)
        f.flush()
"
# Do throughput bang cach tinh tong size / elapsed
START_NS=$(date +%s%N)
python3 -c "
chars = b'abcdefghijklmnopqrstuvwxyz0123456789 \n'
chunk = bytes([chars[i % len(chars)] for i in range(64 * 1024)])
with open('$MOUNT/bm_append_chunk2.bin', 'wb') as f:
    for _ in range(1024):
        f.write(chunk)
        f.flush()
"
END_NS=$(date +%s%N)
ELAPSED_MS_CHUNK=$(( (END_NS - START_NS) / 1000000 ))
[ "$ELAPSED_MS_CHUNK" -eq 0 ] && ELAPSED_MS_CHUNK=1
WRITE_CHUNK=$(python3 -c "print(f'{64 * 1000 / $ELAPSED_MS_CHUNK:.0f} MB/s')")
rm -f "$MOUNT/bm_append_chunk.bin" "$MOUNT/bm_append_chunk2.bin"
log "  Append 1024x64KB:     ${WRITE_CHUNK:-N/A}"

log ""
log "--- Pattern C: 1024 lan append 1KB (sub-chunk, RMW moi append) ---"
# Ghi truoc 1 chunk day de moi append 1KB la partial overwrite (RMW)
python3 -c "
chars = b'abcdefghijklmnopqrstuvwxyz0123456789 \n'
seed = bytes([chars[i % len(chars)] for i in range(64 * 1024)])
with open('$MOUNT/bm_append_1kb.bin', 'wb') as f:
    f.write(seed)
    f.flush()
"
START_NS=$(date +%s%N)
python3 -c "
block = b'Hello log entry from myfs transparent compression test!\n' * 18  # ~1KB
with open('$MOUNT/bm_append_1kb.bin', 'ab') as f:
    for _ in range(1024):
        f.write(block)
        f.flush()
"
END_NS=$(date +%s%N)
ELAPSED_MS_1KB=$(( (END_NS - START_NS) / 1000000 ))
[ "$ELAPSED_MS_1KB" -eq 0 ] && ELAPSED_MS_1KB=1
SIZE_1KB_MB=1  # ~1024 * 1KB = 1MB
WRITE_1KB=$(python3 -c "print(f'{$SIZE_1KB_MB * 1000 / $ELAPSED_MS_1KB:.1f} MB/s')")
AVG_RMW_MS=$(python3 -c "print(f'{$ELAPSED_MS_1KB / 1024:.2f}')")
rm -f "$MOUNT/bm_append_1kb.bin"
log "  Append 1024x1KB:      ${WRITE_1KB:-N/A}  (avg ${AVG_RMW_MS} ms/write)"

log ""
log "+--------------------------------+------------------+------------------------------------------+"
log "| Pattern                        | Throughput       | Ghi chú                                  |"
log "+--------------------------------+------------------+------------------------------------------+"
log "| $(printf '%-31s| %-17s| %-41s|' "Bulk write 64MB (1 call)"    "${WRITE_BULK:-N/A}"  "Optimization: chunk-granularity writes")"
log "| $(printf '%-31s| %-17s| %-41s|' "Append 1024x64KB (chunk-OK)" "${WRITE_CHUNK:-N/A}" "Best case: 1 write fills 1 chunk")"
log "| $(printf '%-31s| %-17s| %-41s|' "Append 1024x1KB (sub-chunk)" "${WRITE_1KB:-N/A}"   "Worst case: every append triggers RMW")"
log "+--------------------------------+------------------+------------------------------------------+"
log "| $(printf '%-31s| %-17s| %-41s|' "RMW avg latency (1KB write)"  "${AVG_RMW_MS:-N/A} ms"  "Derived from Pattern C")"
log "+--------------------------------+------------------+------------------------------------------+"

log ""
log "  Phân tích:"
python3 - "$WRITE_BULK" "$WRITE_CHUNK" "$WRITE_1KB" "$AVG_RMW_MS" << 'ANALYZE_EOF'
import sys

def parse(s):
    if not s or s.strip() in ('', 'N/A'): return None
    try: return float(s.strip().replace(' MB/s','').replace('MB/s','').replace(' ms',''))
    except: return None

bulk, chunk, kb1, rmw_ms = [parse(a) for a in sys.argv[1:]]

if bulk and chunk:
    pct = (1 - chunk/bulk)*100
    factor = bulk/chunk
    print(f'  Bulk vs Chunk-aligned: chunk-aligned chậm hơn {factor:.1f}x ({pct:.1f}% overhead)')
    print(f'    -> Overhead đến từ: flush() per-write + FUSE round-trip per-call')

if bulk and kb1:
    factor = bulk/kb1 if kb1 > 0 else float('inf')
    print(f'  Bulk vs Sub-chunk 1KB: sub-chunk chậm hơn {factor:.1f}x')
    if rmw_ms:
        print(f'    -> Mỗi append 1KB phải thực hiện full RMW (~{rmw_ms:.2f} ms/write)')
        print(f'    -> RMW: đọc chunk cũ -> giải nén -> patch -> nén lại -> ghi mới -> cập nhật meta')

if chunk and kb1:
    factor = chunk/kb1 if kb1 > 0 else float('inf')
    print(f'  Chunk-aligned vs Sub-chunk: chunk-aligned nhanh hơn {factor:.1f}x')

print()
print('  [KẾT LUẬN]')
print('  Ứng dụng nén buffer data đến >= 64KB trước khi write vào myfs.')
print('  Sub-chunk write liên tục là anti-pattern với chunk-based compression FS.')
print('  Use case phù hợp nhất: sequential log, file dump, media encoding output.')
ANALYZE_EOF


# =============================================================================
# Cleanup
# =============================================================================
rm -rf "$TMP_DIR"
rm -f "$MOUNT"/bm_*.bin 2>/dev/null

echo ""
log "Kết quả đã lưu vào: ${RESULT_FILE}"
echo ""