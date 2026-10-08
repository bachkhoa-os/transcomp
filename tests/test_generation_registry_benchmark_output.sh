#!/bin/bash
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
benchmark=${1:-$repo_root/benchmarks/generation_registry_bench}

allowed_cpus=
if [ -r "/proc/$$/status" ]; then
    allowed_cpus=$(awk '$1 == "Cpus_allowed_list:" { print $2; exit }' "/proc/$$/status")
fi
if [ -z "$allowed_cpus" ] && command -v taskset >/dev/null 2>&1; then
    allowed_cpus=$(taskset -pc "$$" 2>/dev/null | sed 's/^.*: //')
fi
first_cpu=${allowed_cpus%%,*}
first_cpu=${first_cpu%%-*}
case $first_cpu in
    ''|*[!0-9]*)
        echo "generation registry benchmark contract: could not discover an allowed CPU" >&2
        exit 1
        ;;
esac

expand_cpu_list()
{
    printf '%s\n' "$1" | tr ',' '\n' | while IFS= read -r part; do
        case $part in
            *-*)
                first=${part%-*}
                last=${part#*-}
                seq "$first" "$last"
                ;;
            *)
                printf '%s\n' "$part"
                ;;
        esac
    done
}

gc_cpu=
sibling_list=$first_cpu
topology_available=false
if [ -r "/sys/devices/system/cpu/cpu${first_cpu}/topology/thread_siblings_list" ]; then
    sibling_list=$(cat "/sys/devices/system/cpu/cpu${first_cpu}/topology/thread_siblings_list")
    topology_available=true
fi
sibling_cpus=$(expand_cpu_list "$sibling_list")
if [ "$topology_available" = true ]; then
    for candidate in $(expand_cpu_list "$allowed_cpus"); do
        is_sibling=false
        for sibling in $sibling_cpus; do
            if [ "$candidate" = "$sibling" ]; then
                is_sibling=true
                break
            fi
        done
        if [ "$is_sibling" = false ] &&
           [ -r "/sys/devices/system/cpu/cpu${candidate}/topology/thread_siblings_list" ]; then
            gc_cpu=$candidate
            break
        fi
    done
fi

workdir=$(mktemp -d "${TMPDIR:-/tmp}/generation-registry-benchmark-output.XXXXXX")
trap 'rm -rf -- "$workdir"' EXIT

benchmark_args=(--short --aa --cpus "$first_cpu" --gc-iterations 5000
    --min-gc-cycles 1 --workdir "$workdir")
if [ -n "$gc_cpu" ]; then
    benchmark_args+=(--gc-cpu "$gc_cpu")
fi
output=$("$benchmark" "${benchmark_args[@]}")

leftover=$(find "$workdir" -mindepth 1 -print -quit)
if [ -n "$leftover" ]; then
    echo "generation registry benchmark contract: leaked workdir entry: $leftover" >&2
    exit 1
fi

printf '%s\n' "$output" | awk -F, \
    -v expected_foreground_cpu="$first_cpu" \
    -v expected_gc_cpu="${gc_cpu:-inherited}" '
function fail(message) {
    print "generation registry benchmark contract: " message > "/dev/stderr"
    failed = 1
}
function nonnegative_number(value, normalized) {
    normalized = tolower(value)
    return value ~ /^([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/ &&
           normalized !~ /nan|inf/
}
function positive_number(value) {
    return nonnegative_number(value) && value + 0 > 0
}
function nonnegative_integer(value) {
    return value ~ /^[0-9]+$/
}
function note_prefix(prefix, name) {
    if (index($0, prefix) == 1)
        header_count[name]++
}
BEGIN {
    expected_header = "workload,io_mode,threads,gc_threads,repetitions,operations,median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,median_per_thread_ops_per_sec,p50_ns,p99_ns,p999_ns,max_ns,delayed_fraction,gc_median_cycles_per_sec,gc_min_cycles,aa_median_spread_pct,aa_max_spread_pct,noise_floor_pct,win_gate,self_check"
    rule = "Rule: do not report a difference as a win unless it exceeds the observed A/A spread."
    split("control scalability hot-path", non_gc_workloads, " ")
    split("real synthetic-delay", gc_modes, " ")
}
{
    note_prefix("GENERATION_REGISTRY_BENCHMARK,version=1", "benchmark")
    note_prefix("TREE,hash=", "tree")
    note_prefix("HOST,", "host")
    note_prefix("AFFINITY,", "affinity")
    note_prefix("TOPOLOGY,cpu=", "topology")
    note_prefix("ASSIGNMENT,role=foreground,", "foreground_assignment")
    note_prefix("ASSIGNMENT,role=gc,", "gc_assignment")
    note_prefix("GC_LOCK_DUTY_CYCLE,status=not-measurable,", "gc_duty_cycle")
    note_prefix("GC_WINDOW,stop=minimum-iterations-and-minimum-cycles,timeout_seconds=30", "gc_window")
    note_prefix("METRIC,max_ns=informational", "max_metric")
    note_prefix("CONFIG,", "config")
    note_prefix("MIXED,skipped,reason=public-api-no-shard-placement", "mixed")
    note_prefix("SELF_CHECK,registry_empty=inferred-from-balanced-public-lifecycle,victims=retired,owned_files=removed", "self_check_scope")

    if (index($0, "TOPOLOGY,cpu=") == 1) {
        split($2, topology_cpu, "=")
        topology_seen[topology_cpu[2]] = 1
    }
    if (index($0, "ASSIGNMENT,role=foreground,") == 1 &&
        $4 != "cpu=" expected_foreground_cpu)
        fail("unexpected foreground CPU assignment: " $0)
    if (index($0, "ASSIGNMENT,role=gc,") == 1 &&
        $4 != "cpu=" expected_gc_cpu)
        fail("unexpected GC CPU assignment: " $0)

    if ($0 == "CSV_BEGIN") {
        csv_begin_count++
        if (in_csv || csv_end_count)
            fail("CSV_BEGIN is duplicated or out of order")
        in_csv = 1
        next
    }
    if ($0 == "CSV_END") {
        csv_end_count++
        if (!in_csv)
            fail("CSV_END appeared without CSV_BEGIN")
        in_csv = 0
        next
    }
    if ($0 == "SUMMARY_BEGIN") {
        summary_begin_count++
        if (in_summary || summary_end_count)
            fail("SUMMARY_BEGIN is duplicated or out of order")
        if (!csv_end_count)
            fail("SUMMARY_BEGIN appeared before CSV_END")
        in_summary = 1
        next
    }
    if ($0 == "SUMMARY_END") {
        summary_end_count++
        if (!in_summary)
            fail("SUMMARY_END appeared without SUMMARY_BEGIN")
        in_summary = 0
        next
    }
    if (in_summary && $0 == rule)
        saw_rule = 1

    if (!in_csv)
        next
    if (!saw_csv_header) {
        if ($0 != expected_header)
            fail("unexpected CSV header: " $0)
        saw_csv_header = 1
        next
    }

    rows++
    if (NF != 22) {
        fail("expected 22 columns, got " NF " in row " rows)
        next
    }

    workload = $1
    io_mode = $2
    threads = $3
    gc_threads = $4
    repetitions = $5
    operations = $6

    if (!nonnegative_integer(threads) || (threads != 1 && threads != 2))
        fail("unexpected thread count " threads " in row " rows)
    if (!nonnegative_integer(gc_threads))
        fail("non-integer GC thread count in row " rows)
    if (!nonnegative_integer(repetitions) || repetitions + 0 <= 0)
        fail("non-positive repetition count in row " rows)
    if (!nonnegative_integer(operations) || operations + 0 <= 0)
        fail("non-positive operation count in row " rows)
    if (!nonnegative_integer($17))
        fail("non-integer gc_min_cycles in row " rows)

    for (column = 7; column <= 20; column++) {
        if (column != 17 && !nonnegative_number($column))
            fail("invalid numeric column " column " in row " rows)
    }
    for (column = 7; column <= 10; column++) {
        if (!positive_number($column))
            fail("non-positive throughput column " column " in row " rows)
    }
    if ($8 + 0 > $7 + 0 || $7 + 0 > $9 + 0)
        fail("throughput min/median/max are out of order in row " rows)
    if ($11 + 0 > $12 + 0 || $12 + 0 > $13 + 0 || $13 + 0 > $14 + 0)
        fail("latency percentiles are out of order in row " rows)
    if ($15 + 0 > 1)
        fail("delayed_fraction is greater than one in row " rows)
    if ($21 != "aa-reference")
        fail("unexpected win_gate in row " rows ": " $21)
    if ($22 != "pass")
        fail("self-check did not pass in row " rows)

    if (workload == "control" || workload == "scalability" || workload == "hot-path") {
        if (io_mode != "none")
            fail("unexpected I/O mode for " workload ": " io_mode)
        if (gc_threads + 0 != 0)
            fail("non-GC workload has GC threads in row " rows)
        if ($16 + 0 != 0 || $17 + 0 != 0)
            fail("non-GC workload has non-zero GC metrics in row " rows)
    } else if (workload == "gc-interference") {
        if (io_mode != "real" && io_mode != "synthetic-delay")
            fail("unexpected GC I/O mode " io_mode)
        if (gc_threads + 0 <= 0)
            fail("GC workload has no GC threads in row " rows)
        if (!positive_number($16) || $17 + 0 <= 0)
            fail("GC workload did not complete a cycle in row " rows)
    } else {
        fail("unknown workload " workload)
    }

    key = workload SUBSEP io_mode SUBSEP threads
    if (++seen[key] > 1)
        fail("duplicate workload combination " workload "/" io_mode "/" threads)
}
END {
    if (header_count["benchmark"] != 1)
        fail("expected one GENERATION_REGISTRY_BENCHMARK,version=1 header")
    if (header_count["tree"] != 1)
        fail("expected one TREE,hash= header")
    if (header_count["host"] != 1)
        fail("expected one HOST header")
    if (header_count["affinity"] != 1)
        fail("expected one AFFINITY header")
    if (header_count["topology"] < 1)
        fail("expected at least one CPU topology line")
    if (!topology_seen[expected_foreground_cpu])
        fail("missing foreground CPU topology line")
    if (expected_gc_cpu != "inherited" && !topology_seen[expected_gc_cpu])
        fail("missing GC CPU topology line")
    if (header_count["foreground_assignment"] != 2)
        fail("expected one foreground assignment per short-run worker")
    if (header_count["gc_assignment"] != 1)
        fail("expected one GC assignment")
    if (header_count["gc_duty_cycle"] != 1)
        fail("expected one public-API GC duty-cycle status line")
    if (header_count["gc_window"] != 1)
        fail("expected one GC measurement-window line")
    if (header_count["max_metric"] != 1)
        fail("expected one informational max-latency marker")
    if (header_count["config"] != 1)
        fail("expected one CONFIG header")
    if (header_count["mixed"] != 1)
        fail("expected one public-API mixed-workload skip line")
    if (header_count["self_check_scope"] != 1)
        fail("expected one public-API self-check scope line")
    if (csv_begin_count != 1 || csv_end_count != 1 || !saw_csv_header || in_csv)
        fail("missing or unterminated CSV section")
    if (summary_begin_count != 1 || summary_end_count != 1 || in_summary)
        fail("missing or unterminated summary section")
    if (!saw_rule)
        fail("missing A/A comparison rule in summary")
    if (rows != 10)
        fail("expected 10 CSV rows, got " rows)

    for (workload_index = 1; workload_index <= 3; workload_index++) {
        workload = non_gc_workloads[workload_index]
        for (threads = 1; threads <= 2; threads++) {
            key = workload SUBSEP "none" SUBSEP threads
            if (!seen[key])
                fail("missing combination " workload "/none/" threads)
        }
    }
    for (mode_index = 1; mode_index <= 2; mode_index++) {
        io_mode = gc_modes[mode_index]
        for (threads = 1; threads <= 2; threads++) {
            key = "gc-interference" SUBSEP io_mode SUBSEP threads
            if (!seen[key])
                fail("missing combination gc-interference/" io_mode "/" threads)
        }
    }
    exit failed ? 1 : 0
}'

comparison_dir=$workdir/comparison
mkdir "$comparison_dir"
printf '%s\n' "$output" >"$comparison_dir/baseline.out"
awk -F, -v OFS=, '
    $0 == "CSV_BEGIN" { in_csv = 1; print; next }
    in_csv && !header_seen { header_seen = 1; print; next }
    in_csv && !changed && $1 == "control" && $2 == "none" && $3 == 1 {
        $7 = sprintf("%.3f", $7 * 2)
        changed = 1
    }
    { print }
    END { if (!changed) exit 1 }
' "$comparison_dir/baseline.out" >"$comparison_dir/current.out"
awk -F, -v OFS=, '
    $0 == "CSV_BEGIN" { in_csv = 1; print; next }
    in_csv && !header_seen { header_seen = 1; print; next }
    in_csv && NF == 22 { $19 = "0.500000" }
    { print }
' "$comparison_dir/baseline.out" >"$comparison_dir/current-aa.out"
cp "$comparison_dir/current-aa.out" "$comparison_dir/baseline-aa.out"

comparison=$("$repo_root/benchmarks/compare_generation_registry_results.sh" \
    "$comparison_dir/current.out" "$comparison_dir/baseline.out" \
    "$comparison_dir/current-aa.out" "$comparison_dir/baseline-aa.out")
printf '%s\n' "$comparison" | awk -F, '
BEGIN {
    expected_header = "workload,io_mode,threads,current_median_ops_per_sec,baseline_median_ops_per_sec,relative_abs_diff_pct,current_aa_max_spread_pct,baseline_aa_max_spread_pct,row_gate_pct,global_gate_pct,exceeds_row_gate,exceeds_global_gate"
}
NR == 1 {
    if ($0 != expected_header) {
        print "generation registry benchmark comparison contract: unexpected header: " $0 > "/dev/stderr"
        failed = 1
    }
    next
}
{
    rows++
    if (NF != 12) {
        print "generation registry benchmark comparison contract: expected 12 columns" > "/dev/stderr"
        failed = 1
        next
    }
    if ($11 == "yes") row_yes++
    else if ($11 == "no") row_no++
    else failed = 1
    if ($12 == "yes") global_yes++
    else if ($12 == "no") global_no++
    else failed = 1
}
END {
    if (rows != 10 || row_yes != 1 || row_no != 9 ||
        global_yes != 1 || global_no != 9)
        failed = 1
    exit failed ? 1 : 0
}'
