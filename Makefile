CC = gcc
COMMON_CFLAGS = -Wall -Wno-format-truncation -pthread -Isrc -Isrc/guards -Isrc/core -Isrc/fuse_ops
DEBUG_CFLAGS = $(COMMON_CFLAGS) -g
RELEASE_CFLAGS = $(COMMON_CFLAGS) -O2 -DNDEBUG
CFLAGS ?= $(DEBUG_CFLAGS)
LIBS = `pkg-config fuse3 --cflags --libs` -lzstd -lz

# Định nghĩa các thư mục mã nguồn
CORE_SRCS = src/core/path.c src/core/metadata.c src/core/compress.c src/core/compact.c src/core/chunkio.c src/core/lock.c
OPS_SRCS = src/fuse_ops/file.c src/fuse_ops/dir.c
GUARD_SRCS = src/guards/guards.c

SRCS = src/main.c $(CORE_SRCS) $(OPS_SRCS) $(GUARD_SRCS)

.PHONY: all release run umount test bench clean

all: myfs

myfs: $(SRCS)
	$(CC) $(CFLAGS) -o myfs $(SRCS) $(LIBS)

release:
	$(CC) $(RELEASE_CFLAGS) -o myfs $(SRCS) $(LIBS)

run: myfs
	./myfs -f mountpoint ./backing

umount:
	fusermount3 -u mountpoint

test:
	@chmod +x test_suite.sh
	@./test_suite.sh mountpoint backing

bench: release
	@chmod +x benchmark.sh
	@./benchmark.sh mountpoint backing

clean:
	@if mountpoint -q mountpoint 2>/dev/null; then \
		echo "[ERROR] mountpoint dang duoc mount. Chay 'make umount' truoc."; \
		exit 1; \
	fi
	rm -f myfs verify_remount.sh
	rm -rf backing/* .myfs_bench.*
