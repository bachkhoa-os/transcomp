CC = gcc
COMMON_CFLAGS = -Wall -Wno-format-truncation -pthread -Isrc -Isrc/guards -Isrc/core -Isrc/fuse_ops
DEBUG_CFLAGS = $(COMMON_CFLAGS) -g
RELEASE_CFLAGS = $(COMMON_CFLAGS) -O2 -DNDEBUG
CFLAGS ?= $(DEBUG_CFLAGS)
LIBS = `pkg-config fuse3 --cflags --libs` -lzstd -lz
GENERATION_REGISTRY_BENCH_WRAP_FLAGS = -Wl,--wrap=fsync -Wl,--wrap=rename -Wl,--wrap=unlink -Wl,--wrap=rmdir -Wl,--wrap=readlink -Wl,--wrap=lstat -Wl,--wrap=link -Wl,--wrap=symlink
GENERATION_REGISTRY_BENCH_ARGS ?=
GENERATION_REGISTRY_BENCH_WORKDIR ?=

# Định nghĩa các thư mục mã nguồn
CORE_SRCS = src/core/path.c src/core/metadata.c src/core/compress.c src/core/compact.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/lock.c
OPS_SRCS = src/fuse_ops/file.c src/fuse_ops/dir.c
GUARD_SRCS = src/guards/guards.c

SRCS = src/main.c $(CORE_SRCS) $(OPS_SRCS) $(GUARD_SRCS)

.PHONY: all help test-help release run umount test test-unit test-generation-registry tsan-generation-registry tsan-file-ops test-lock test-lock-tsan test-chunkio-scratch test-chunkio-scratch-tsan bench bench-zstd-context bench-chunkio-scratch bench-lock-table build-generation-registry-bench build-generation-registry-baseline bench-generation-registry tsan-generation-registry-bench force-generation-registry-bench clean

all: myfs

help: test-help

test-help:
	@echo "Available test targets:"
	@echo "  make test-unit                  Full unit/guard suite (no FUSE mount)"
	@echo "  make test-generation-registry  Registry and two-phase GC tests"
	@echo "  make tsan-generation-registry  Registry/GC tests under ThreadSanitizer"
	@echo "  make tsan-file-ops             File/handoff tests under ThreadSanitizer"
	@echo "  make test-lock                  Path-lock table tests"
	@echo "  make test-lock-tsan             Path-lock tests under ThreadSanitizer"
	@echo "  make test-chunkio-scratch       Chunk-I/O scratch tests"
	@echo "  make test-chunkio-scratch-tsan  Chunk-I/O scratch tests under ThreadSanitizer"
	@echo "  make build-generation-registry-bench  Build registry/GC microbenchmark"
	@echo "  make build-generation-registry-baseline BASELINE_TREE=/path BASELINE_OUTPUT=/path/to/binary"
	@echo "  make bench-generation-registry GENERATION_REGISTRY_BENCH_WORKDIR=/path [GENERATION_REGISTRY_BENCH_ARGS='...']"
	@echo "  make tsan-generation-registry-bench  Short registry/GC benchmark under ThreadSanitizer"
	@echo "  make test                       Mounted FUSE integration suite"

myfs: $(SRCS) src/core/chunkio_scratch.h
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

test-unit: tests/test_metadata tests/test_file_ops tests/test_generation_registry tests/test_zstd_context tests/test_chunkio_scratch tests/test_chunkio_scratch_integration tests/test_lock tests/test_meta_inspect benchmarks/zstd_context_bench benchmarks/chunkio_scratch_bench benchmarks/lock_table_bench benchmarks/generation_registry_bench benchmarks/meta_inspect
	@./tests/test_metadata
	@./tests/test_file_ops
	@./tests/test_generation_registry
	@./tests/test_zstd_context
	@./tests/test_chunkio_scratch
	@./tests/test_chunkio_scratch_integration
	@./tests/test_lock
	@./tests/test_meta_inspect ./benchmarks/meta_inspect
	@bash ./tests/test_suite_guard.sh
	@bash ./tests/test_benchmark_helpers.sh
	@bash ./tests/test_zstd_context_benchmark_output.sh ./benchmarks/zstd_context_bench
	@bash ./tests/test_chunkio_scratch_benchmark_output.sh ./benchmarks/chunkio_scratch_bench
	@bash ./tests/test_lock_table_benchmark_output.sh ./benchmarks/lock_table_bench
	@bash ./tests/test_generation_registry_benchmark_output.sh ./benchmarks/generation_registry_bench

tests/test_metadata: tests/test_metadata.c src/core/metadata.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/chunkio_scratch.h src/core/compress.c src/core/path.c src/myfs.h
	$(CC) $(CFLAGS) -o $@ tests/test_metadata.c src/core/metadata.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/compress.c src/core/path.c $(LIBS)

tests/test_file_ops: tests/test_file_ops.c $(CORE_SRCS) $(OPS_SRCS) $(GUARD_SRCS) src/core/compact_test.h src/core/chunkio_scratch.h src/myfs.h
	$(CC) $(CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ tests/test_file_ops.c $(CORE_SRCS) $(OPS_SRCS) $(GUARD_SRCS) $(LIBS)

tsan-file-ops:
	$(CC) $(COMMON_CFLAGS) -g -O1 -DMYFS_TEST_FAILPOINTS -fsanitize=thread -fno-omit-frame-pointer -o /tmp/myfs-test-file-ops-tsan tests/test_file_ops.c $(CORE_SRCS) $(OPS_SRCS) $(GUARD_SRCS) $(LIBS)
	@TSAN_OPTIONS=halt_on_error=1 /tmp/myfs-test-file-ops-tsan

tests/test_generation_registry: tests/test_generation_registry.c $(CORE_SRCS) src/core/compact_test.h src/core/lock_test.h src/core/chunkio_scratch.h src/myfs.h
	$(CC) $(CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ tests/test_generation_registry.c $(CORE_SRCS) $(LIBS)

test-generation-registry: tests/test_generation_registry
	@./tests/test_generation_registry

tsan-generation-registry:
	$(CC) $(COMMON_CFLAGS) -g -O1 -DMYFS_TEST_FAILPOINTS -fsanitize=thread -fno-omit-frame-pointer -o /tmp/myfs-test-generation-registry-tsan tests/test_generation_registry.c $(CORE_SRCS) $(LIBS)
	@TSAN_OPTIONS=halt_on_error=1 /tmp/myfs-test-generation-registry-tsan

tests/test_zstd_context: tests/test_zstd_context.c src/core/compress.c src/myfs.h
	$(CC) $(CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ tests/test_zstd_context.c src/core/compress.c $(LIBS)

tests/test_chunkio_scratch: tests/test_chunkio_scratch.c src/core/chunkio_scratch.c src/core/chunkio_scratch.h src/myfs.h
	$(CC) $(CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ tests/test_chunkio_scratch.c src/core/chunkio_scratch.c $(LIBS)

tests/test_chunkio_scratch_integration: tests/test_chunkio_scratch_integration.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/chunkio_scratch.h src/core/compress.c src/myfs.h
	$(CC) $(CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ tests/test_chunkio_scratch_integration.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/compress.c $(LIBS)

test-chunkio-scratch: tests/test_chunkio_scratch tests/test_chunkio_scratch_integration
	@./tests/test_chunkio_scratch
	@./tests/test_chunkio_scratch_integration

test-chunkio-scratch-tsan:
	$(CC) $(COMMON_CFLAGS) -g -O1 -DMYFS_TEST_FAILPOINTS -fsanitize=thread -fno-omit-frame-pointer -o /tmp/myfs-test-chunkio-scratch-tsan tests/test_chunkio_scratch.c src/core/chunkio_scratch.c $(LIBS)
	@TSAN_OPTIONS=halt_on_error=1 /tmp/myfs-test-chunkio-scratch-tsan
	$(CC) $(COMMON_CFLAGS) -g -O1 -DMYFS_TEST_FAILPOINTS -fsanitize=thread -fno-omit-frame-pointer -o /tmp/myfs-test-chunkio-scratch-integration-tsan tests/test_chunkio_scratch_integration.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/compress.c $(LIBS)
	@TSAN_OPTIONS=halt_on_error=1 /tmp/myfs-test-chunkio-scratch-integration-tsan

tests/test_lock: tests/test_lock.c src/core/lock.c src/myfs.h
	$(CC) $(CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ tests/test_lock.c src/core/lock.c $(LIBS)

test-lock: tests/test_lock
	@./tests/test_lock

test-lock-tsan:
	$(CC) $(COMMON_CFLAGS) -g -O1 -DMYFS_TEST_FAILPOINTS -fsanitize=thread -fno-omit-frame-pointer -o /tmp/myfs-test-lock-tsan tests/test_lock.c src/core/lock.c $(LIBS)
	@TSAN_OPTIONS=halt_on_error=1 /tmp/myfs-test-lock-tsan

tests/test_meta_inspect: tests/test_meta_inspect.c src/core/metadata.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/chunkio_scratch.h src/core/compress.c src/core/path.c src/myfs.h
	$(CC) $(CFLAGS) -o $@ tests/test_meta_inspect.c src/core/metadata.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/compress.c src/core/path.c $(LIBS)

bench: release benchmarks/meta_inspect
	@chmod +x benchmark.sh
	@./benchmark.sh mountpoint backing

benchmarks/meta_inspect: benchmarks/meta_inspect.c src/core/metadata.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/chunkio_scratch.h src/core/compress.c src/core/path.c src/myfs.h
	$(CC) $(RELEASE_CFLAGS) -o $@ benchmarks/meta_inspect.c src/core/metadata.c src/core/chunkio.c src/core/chunkio_scratch.c src/core/compress.c src/core/path.c $(LIBS)

benchmarks/zstd_context_bench: benchmarks/zstd_context_bench.c src/core/compress.c src/myfs.h
	$(CC) $(RELEASE_CFLAGS) -o $@ benchmarks/zstd_context_bench.c src/core/compress.c $(LIBS)

bench-zstd-context: benchmarks/zstd_context_bench
	@./benchmarks/zstd_context_bench

benchmarks/chunkio_scratch_bench: benchmarks/chunkio_scratch_bench.c src/core/chunkio_scratch.c src/core/chunkio_scratch.h src/myfs.h
	$(CC) $(RELEASE_CFLAGS) -DMYFS_TEST_FAILPOINTS -o $@ benchmarks/chunkio_scratch_bench.c src/core/chunkio_scratch.c $(LIBS)

bench-chunkio-scratch: benchmarks/chunkio_scratch_bench
	@./benchmarks/chunkio_scratch_bench

benchmarks/lock_table_bench: benchmarks/lock_table_bench.c benchmarks/fixtures/lock_table_linear.c src/core/lock.c src/myfs.h
	$(CC) $(RELEASE_CFLAGS) -o $@ benchmarks/lock_table_bench.c benchmarks/fixtures/lock_table_linear.c src/core/lock.c $(LIBS)

bench-lock-table: benchmarks/lock_table_bench
	@./benchmarks/lock_table_bench

benchmarks/generation_registry_bench: force-generation-registry-bench benchmarks/generation_registry_bench.c $(CORE_SRCS) src/core/chunkio_scratch.h src/myfs.h
	$(CC) $(RELEASE_CFLAGS) -DMYFS_BENCH_TREE_HASH='"$(shell git rev-parse --verify HEAD 2>/dev/null || echo unknown)"' $(GENERATION_REGISTRY_BENCH_WRAP_FLAGS) -o $@ benchmarks/generation_registry_bench.c $(CORE_SRCS) $(LIBS)

force-generation-registry-bench:

build-generation-registry-bench: benchmarks/generation_registry_bench

build-generation-registry-baseline:
	@test -n "$(BASELINE_TREE)" || { echo "BASELINE_TREE=/path/to/origin-main-worktree is required" >&2; exit 2; }
	@test -n "$(BASELINE_OUTPUT)" || { echo "BASELINE_OUTPUT=/path/to/output-binary is required" >&2; exit 2; }
	@./benchmarks/build_generation_registry_baseline.sh "$(BASELINE_TREE)" "$(BASELINE_OUTPUT)"

bench-generation-registry: benchmarks/generation_registry_bench
	@test -n "$(GENERATION_REGISTRY_BENCH_WORKDIR)" || { echo "GENERATION_REGISTRY_BENCH_WORKDIR=/real/filesystem/path is required" >&2; exit 2; }
	@./benchmarks/generation_registry_bench --workdir "$(GENERATION_REGISTRY_BENCH_WORKDIR)" $(GENERATION_REGISTRY_BENCH_ARGS)

tsan-generation-registry-bench:
	$(CC) $(COMMON_CFLAGS) -g -O1 -fsanitize=thread -fno-omit-frame-pointer -DMYFS_BENCH_TREE_HASH='"$(shell git rev-parse --verify HEAD 2>/dev/null || echo unknown)-tsan"' $(GENERATION_REGISTRY_BENCH_WRAP_FLAGS) -o /tmp/myfs-generation-registry-bench-tsan benchmarks/generation_registry_bench.c $(CORE_SRCS) $(LIBS)
	@workdir=$$(mktemp -d /tmp/myfs-generation-registry-bench-tsan.XXXXXX); \
	trap 'rmdir "$$workdir"' EXIT; \
	cpus=$$(awk '$$1 == "Cpus_allowed_list:" { print $$2; exit }' /proc/$$$$/status); \
	cpu=$${cpus%%,*}; cpu=$${cpu%%-*}; \
	TSAN_OPTIONS=halt_on_error=1 /tmp/myfs-generation-registry-bench-tsan --short --cpus "$$cpu" --workdir "$$workdir"

clean:
	@if mountpoint -q mountpoint 2>/dev/null; then \
		echo "[ERROR] mountpoint dang duoc mount. Chay 'make umount' truoc."; \
		exit 1; \
	fi
	rm -f myfs verify_remount.sh tests/test_metadata tests/test_file_ops tests/test_generation_registry tests/test_zstd_context tests/test_chunkio_scratch tests/test_chunkio_scratch_integration tests/test_lock tests/test_meta_inspect benchmarks/meta_inspect benchmarks/zstd_context_bench benchmarks/chunkio_scratch_bench benchmarks/lock_table_bench benchmarks/generation_registry_bench
	rm -rf backing/* .myfs_bench.*
