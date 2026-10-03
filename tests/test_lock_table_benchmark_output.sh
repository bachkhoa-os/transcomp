#!/bin/bash
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
benchmark=${1:-$repo_root/benchmarks/lock_table_bench}
output=$($benchmark \
    --workers 1 \
    --resident-paths 0 \
    --repetitions 2 \
    --warmup-ms 10 \
    --duration-ms 20 \
    --latency-samples 101)

require_line() {
    if ! printf '%s\n' "$output" | grep -Fqx -- "$1"; then
        echo "missing lock-table benchmark line: $1" >&2
        exit 1
    fi
}

require_match() {
    if ! printf '%s\n' "$output" | grep -Eq -- "$1"; then
        echo "missing lock-table benchmark row matching: $1" >&2
        exit 1
    fi
}

require_line "implementation,workers,resident_paths,repetition,operations,elapsed_ns,ops_per_sec,p50_ns,p99_ns,latency_samples"
require_match '^linear,1,0,1,[0-9]+,[0-9]+,[0-9]+\.[0-9]+,[0-9]+,[0-9]+,101$'
require_match '^sharded,1,0,1,[0-9]+,[0-9]+,[0-9]+\.[0-9]+,[0-9]+,[0-9]+,101$'

actual_order=$(printf '%s\n' "$output" |
    awk -F, '$1 == "linear" || $1 == "sharded" { print $1 "," $4 }')
expected_order='linear,1
sharded,1
sharded,2
linear,2'
if [ "$actual_order" != "$expected_order" ]; then
    echo "benchmark did not alternate backend order across paired repetitions" >&2
    printf 'actual order:\n%s\n' "$actual_order" >&2
    exit 1
fi

require_line "+---------+----------------+----------------+--------------+----------+----------+---------+"
require_line "| Workers | Resident paths | Implementation | Ops/sec      | p50 (ns) | p99 (ns) | Speedup |"
require_match '^\| +1 \| +0 \| linear +\| +[0-9]+ \| +[0-9]+ \| +[0-9]+ \| +- +\|$'
require_match '^\| +1 \| +0 \| sharded +\| +[0-9]+ \| +[0-9]+ \| +[0-9]+ \| +[0-9]+\.[0-9][0-9]x \|$'

require_line "Defaults: workers=1,4,8,16; resident_paths=0,64,256; repetitions=7; warmup=100ms; throughput=500ms; latency_samples=5001/worker"
