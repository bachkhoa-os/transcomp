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

.PHONY: all release run umount test test-unit test-lock test-lock-tsan bench bench-zstd-context bench-lock-table clean

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

test-unit: tests/test_metadata tests/test_file_ops tests/test_zstd_context tests/test_lock tests/test_meta_inspect benchmarks/zstd_context_bench benchmarks/lock_table_bench benchmarks/meta_inspect
	@./tests/test_metadata
	@./tests/test_file_ops
	@./tests/test_zstd_context
	@./tests/test_lock
	@./tests/test_meta_inspect ./benchmarks/meta_inspect
	@bash ./tests/test_suite_guard.sh
	@bash ./tests/test_benchmark_helpers.sh
	@bash ./tests/test_zstd_context_benchmark_output.sh ./benchmarks/zstd_context_bench
	@bash ./tests/test_lock_table_benchmark_output.sh ./benchmarks/lock_table_bench

tests/test_metadata: tests/test_metadata.c src/core/metadata.c src/core/chunkio.c src/core/compress.c src/core/path.c src/myfs.h
	$(CC) $(CFLAGS) -o $@ tests/test_metadata.c src/core/metadata.c src/core/chunkio.c src/core/compress.c src/core/path.c $(LIBS)

tests/test_file_ops: tests/test_file_ops.c $(CORE_SRCS) $(OPS_SRCS) $(GUARD_SRCS) src/myfs.h
	$(CC) $(CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ tests/test_file_ops.c $(CORE_SRCS) $(OPS_SRCS) $(GUARD_SRCS) $(LIBS)

tests/test_zstd_context: tests/test_zstd_context.c src/core/compress.c src/myfs.h
	$(CC) $(CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ tests/test_zstd_context.c src/core/compress.c $(LIBS)

tests/test_lock: tests/test_lock.c src/core/lock.c src/myfs.h
	$(CC) $(CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ tests/test_lock.c src/core/lock.c $(LIBS)

test-lock: tests/test_lock
	@./tests/test_lock

test-lock-tsan:
	$(CC) $(COMMON_CFLAGS) -g -O1 -DMYFS_TEST_FAILPOINTS -fsanitize=thread -fno-omit-frame-pointer -o /tmp/myfs-test-lock-tsan tests/test_lock.c src/core/lock.c $(LIBS)
	@TSAN_OPTIONS=halt_on_error=1 /tmp/myfs-test-lock-tsan

tests/test_meta_inspect: tests/test_meta_inspect.c src/core/metadata.c src/core/chunkio.c src/core/compress.c src/core/path.c src/myfs.h
	$(CC) $(CFLAGS) -o $@ tests/test_meta_inspect.c src/core/metadata.c src/core/chunkio.c src/core/compress.c src/core/path.c $(LIBS)

bench: release benchmarks/meta_inspect
	@chmod +x benchmark.sh
	@./benchmark.sh mountpoint backing

benchmarks/meta_inspect: benchmarks/meta_inspect.c src/core/metadata.c src/core/chunkio.c src/core/compress.c src/core/path.c src/myfs.h
	$(CC) $(RELEASE_CFLAGS) -o $@ benchmarks/meta_inspect.c src/core/metadata.c src/core/chunkio.c src/core/compress.c src/core/path.c $(LIBS)

benchmarks/zstd_context_bench: benchmarks/zstd_context_bench.c src/core/compress.c src/myfs.h
	$(CC) $(RELEASE_CFLAGS) -o $@ benchmarks/zstd_context_bench.c src/core/compress.c $(LIBS)

bench-zstd-context: benchmarks/zstd_context_bench
	@./benchmarks/zstd_context_bench

benchmarks/lock_table_bench: benchmarks/lock_table_bench.c benchmarks/fixtures/lock_table_linear.c src/core/lock.c src/myfs.h
	$(CC) $(RELEASE_CFLAGS) -o $@ benchmarks/lock_table_bench.c benchmarks/fixtures/lock_table_linear.c src/core/lock.c $(LIBS)

bench-lock-table: benchmarks/lock_table_bench
	@./benchmarks/lock_table_bench

clean:
	@if mountpoint -q mountpoint 2>/dev/null; then \
		echo "[ERROR] mountpoint dang duoc mount. Chay 'make umount' truoc."; \
		exit 1; \
	fi
	rm -f myfs verify_remount.sh tests/test_metadata tests/test_file_ops tests/test_zstd_context tests/test_lock tests/test_meta_inspect benchmarks/meta_inspect benchmarks/zstd_context_bench benchmarks/lock_table_bench
	rm -rf backing/* .myfs_bench.*
