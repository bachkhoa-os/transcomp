#!/bin/bash
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
benchmark=${1:-$repo_root/benchmarks/chunkio_scratch_bench}

output=$($benchmark --repetitions 3 --iterations 120 --samples 31 --warmup 4)

printf '%s\n' "$output" | awk -F, '
function fail(message) {
    print "chunkio scratch benchmark contract: " message > "/dev/stderr"
    failed = 1
}
function positive_finite(value, lower) {
    lower = tolower(value)
    return value ~ /^[0-9]+([.][0-9]+)?$/ && value + 0 > 0 &&
           lower !~ /nan|inf/
}
BEGIN {
    expected_header = "workload,threads,repetition,order,mode,ops_per_sec,p50_ns,p99_ns,speedup,raw_bytes,comp_bytes,window_bytes,cached_bytes,total_retained_bytes,timed_growths,temp_acquisitions,retained_delta_bytes"
    split("raw-64k comp-bound-64k partial-rmw-64k adaptive-shrink-1m-512k mixed-window-sizes max-retained oversize-fallback", names, " ")
    for (i = 1; i <= 7; i++)
        known[names[i]] = 1
}
$0 == "CSV_BEGIN" { in_csv = 1; next }
$0 == "CSV_END" { in_csv = 0; saw_end = 1; next }
in_csv && !saw_header {
    if ($0 != expected_header)
        fail("unexpected CSV header: " $0)
    saw_header = 1
    next
}
in_csv {
    rows++
    if (NF != 17) {
        fail("expected 17 columns, got " NF " in row " rows)
        next
    }
    workload = $1
    threads = $2
    repetition = $3
    order = $4
    mode = $5
    if (!(workload in known))
        fail("unknown workload " workload)
    if (threads != 1 && threads != 8)
        fail("unexpected thread count " threads)
    if (mode != "malloc-free" && mode != "tls-reuse")
        fail("unexpected mode " mode)
    expected_order = (repetition % 2 == 0) ? "baseline-first" : "reuse-first"
    if (order != expected_order)
        fail("pair order did not alternate for repetition " repetition)
    if (!positive_finite($6) || !positive_finite($7) ||
        !positive_finite($8) || !positive_finite($9))
        fail("non-positive or non-finite metric in " workload "/" mode)
    for (column = 10; column <= 17; column++) {
        if ($column !~ /^[0-9]+$/)
            fail("non-integer diagnostic column " column " in row " rows)
    }
    if ($10 > 1048576 || $12 > 1048576 || $13 > 1048576)
        fail("raw/window/cached retained cap exceeded")
    if ($11 > 1052672)
        fail("comp retained cap exceeded")
    if ($14 > 4198400)
        fail("per-thread total retained cap exceeded")
    if (mode == "tls-reuse" && workload != "oversize-fallback") {
        if ($15 != 0)
            fail("normal TLS timed phase grew after warm-up")
        if ($16 != 0)
            fail("normal TLS timed phase used a temporary allocation")
    }
    if (mode == "tls-reuse" && workload == "oversize-fallback") {
        if ($15 != 0 || $16 <= 0 || $17 != 0)
            fail("oversize TLS row did not stay temporary-only")
    }
    seen[workload SUBSEP threads SUBSEP mode] = 1
}
END {
    if (!saw_header || !saw_end)
        fail("missing CSV section or header")
    if (rows != 84)
        fail("expected 84 data rows, got " rows)
    for (i = 1; i <= 7; i++) {
        for (thread_index = 1; thread_index <= 2; thread_index++) {
            threads = thread_index == 1 ? 1 : 8
            if (!seen[names[i] SUBSEP threads SUBSEP "malloc-free"])
                fail("missing baseline combination for " names[i] "/" threads)
            if (!seen[names[i] SUBSEP threads SUBSEP "tls-reuse"])
                fail("missing TLS combination for " names[i] "/" threads)
        }
    }
    exit failed ? 1 : 0
}'
