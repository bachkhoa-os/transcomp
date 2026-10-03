#!/bin/bash
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
benchmark=${1:-$repo_root/benchmarks/zstd_context_bench}
output=$($benchmark)

require_line() {
    if ! printf '%s\n' "$output" | grep -Fqx -- "$1"; then
        echo "missing benchmark table line: $1" >&2
        exit 1
    fi
}

require_match() {
    if ! printf '%s\n' "$output" | grep -Eq -- "$1"; then
        echo "missing benchmark table row matching: $1" >&2
        exit 1
    fi
}

require_line "+------------+---------+-----------+--------------+------------+------------+-----------------------------------+"
require_line "| Operation  | Bytes   | Mode      | Ops/sec      | p50 (ns)   | p99 (ns)   | Speedup                           |"
require_match '^\| compress +\| +1024 \| one-shot +\|'
require_match '^\| decompress +\| +65536 \| TLS-reuse +\|'

require_line "+------------+---------+---------+------------------+-------------------+---------+"
require_line "| Operation  | Bytes   | Workers | One-shot ops/sec | TLS reuse ops/sec | Speedup |"
require_match '^\| compress +\| +4096 \| +8 \|'
require_match '^\| decompress +\| +4096 \| +8 \|'

if printf '%s\n' "$output" | grep -Fq "operation    bytes  mode"; then
    echo "legacy unbordered benchmark header is still present" >&2
    exit 1
fi
