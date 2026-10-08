#!/usr/bin/env bash

set -euo pipefail

usage()
{
    printf 'Usage: %s BASELINE_TREE OUTPUT [extra compiler flags...]\n' \
        "$(basename -- "$0")" >&2
}

die()
{
    printf '%s: %s\n' "$(basename -- "$0")" "$*" >&2
    exit 1
}

if (( $# < 2 )); then
    usage
    exit 2
fi

baseline_arg=$1
output_arg=$2
shift 2
extra_cflags=("$@")

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd -P)
benchmark_source=$repo_root/benchmarks/generation_registry_bench.c

[[ -d $baseline_arg ]] || die "baseline tree is not a directory: $baseline_arg"
baseline_tree=$(CDPATH= cd -- "$baseline_arg" && pwd -P)
[[ -f $baseline_tree/src/myfs.h ]] || \
    die "baseline tree is missing src/myfs.h: $baseline_tree"
[[ -f $benchmark_source ]] || \
    die "current tree is missing benchmarks/generation_registry_bench.c: $repo_root"

output_dir_arg=$(dirname -- "$output_arg")
output_name=$(basename -- "$output_arg")
[[ $output_name != / && -n $output_name ]] || die "invalid output path: $output_arg"
[[ -d $output_dir_arg ]] || die "output directory does not exist: $output_dir_arg"
output_dir=$(CDPATH= cd -- "$output_dir_arg" && pwd -P)
output=$output_dir/$output_name

baseline_hash=$(git -C "$baseline_tree" rev-parse --verify HEAD) || \
    die "cannot determine git hash for baseline tree: $baseline_tree"

baseline_sources=(
    "$baseline_tree/src/core/path.c"
    "$baseline_tree/src/core/metadata.c"
    "$baseline_tree/src/core/compress.c"
    "$baseline_tree/src/core/compact.c"
    "$baseline_tree/src/core/chunkio.c"
    "$baseline_tree/src/core/chunkio_scratch.c"
    "$baseline_tree/src/core/lock.c"
)
for source in "${baseline_sources[@]}"; do
    [[ -f $source ]] || die "baseline tree is missing required source: $source"
done

command -v pkg-config >/dev/null 2>&1 || die "pkg-config is required"
pkg-config --exists fuse3 || die "pkg-config package fuse3 is required"
fuse_cflags_text=$(pkg-config --cflags fuse3) || \
    die "cannot read compiler flags for pkg-config package fuse3"
fuse_libs_text=$(pkg-config --libs fuse3) || \
    die "cannot read linker flags for pkg-config package fuse3"
read -r -a fuse_cflags <<< "$fuse_cflags_text"
read -r -a fuse_libs <<< "$fuse_libs_text"

cc_value=${CC:-gcc}
read -r -a cc_command <<< "$cc_value"
(( ${#cc_command[@]} > 0 )) || die "CC is empty"

defines_wrapper()
{
    local wrapper=$1
    awk -v wrapper="__wrap_$wrapper" '
        index($0, wrapper "(") || $0 ~ wrapper "[[:space:]]*\\(" {
            in_signature = 1
        }
        in_signature && /\{/ {
            found = 1
            exit
        }
        in_signature && /;/ {
            in_signature = 0
        }
        END {
            exit found ? 0 : 1
        }
    ' "$benchmark_source"
}

wrap_flags=()
for wrapped in fsync rename unlink rmdir readlink lstat link symlink; do
    if defines_wrapper "$wrapped"; then
        wrap_flags+=("-Wl,--wrap=$wrapped")
    fi
done

compile_args=(
    -Wall
    -Wno-format-truncation
    -pthread
    "-I$baseline_tree/src"
    "-I$baseline_tree/src/guards"
    "-I$baseline_tree/src/core"
    "-I$baseline_tree/src/fuse_ops"
    -O2
    -DNDEBUG
    "-DMYFS_BENCH_TREE_HASH=\"$baseline_hash\""
    "${fuse_cflags[@]}"
    "${extra_cflags[@]}"
    -o
    "$output"
    "$benchmark_source"
    "${baseline_sources[@]}"
    "${wrap_flags[@]}"
    "${fuse_libs[@]}"
    -lzstd
    -lz
)

printf 'Building generation registry benchmark for %s (%s)\n' \
    "$baseline_tree" "$baseline_hash" >&2
"${cc_command[@]}" "${compile_args[@]}"
