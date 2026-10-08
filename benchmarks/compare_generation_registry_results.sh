#!/bin/sh

set -eu

program=${0##*/}

usage()
{
    printf 'Usage: %s CURRENT BASELINE CURRENT_AA BASELINE_AA\n' "$program" >&2
}

die()
{
    printf '%s: %s\n' "$program" "$*" >&2
    exit 1
}

if [ "$#" -ne 4 ]; then
    usage
    exit 2
fi

current=$1
baseline=$2
current_aa=$3
baseline_aa=$4

validate_input()
{
    input_label=$1
    input_path=$2

    [ -f "$input_path" ] ||
        die "$input_label output is not a regular file: $input_path"
    [ -r "$input_path" ] ||
        die "$input_label output is not readable: $input_path"
}

validate_input current "$current"
validate_input baseline "$baseline"
validate_input current-aa "$current_aa"
validate_input baseline-aa "$baseline_aa"

# Keep awk from interpreting a bare name containing '=' or beginning with '-'
# as something other than a file operand.
case $current in
    /*) ;;
    *) current=./$current ;;
esac
case $baseline in
    /*) ;;
    *) baseline=./$baseline ;;
esac
case $current_aa in
    /*) ;;
    *) current_aa=./$current_aa ;;
esac
case $baseline_aa in
    /*) ;;
    *) baseline_aa=./$baseline_aa ;;
esac

LC_ALL=C
export LC_ALL

awk -F, -v program="$program" '
function fail(message) {
    print program ": " message > "/dev/stderr"
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

function positive_integer(value) {
    return value ~ /^[0-9]+$/ && value + 0 > 0
}

function describe_key(key) {
    return key_workload[key] "/" key_io_mode[key] "/" key_threads[key]
}

BEGIN {
    expected_header = "workload,io_mode,threads,gc_threads,repetitions,operations,median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,median_per_thread_ops_per_sec,p50_ns,p99_ns,p999_ns,max_ns,delayed_fraction,gc_median_cycles_per_sec,gc_min_cycles,aa_median_spread_pct,aa_max_spread_pct,noise_floor_pct,win_gate,self_check"

    role_name[1] = "current"
    role_name[2] = "baseline"
    role_name[3] = "current-aa"
    role_name[4] = "baseline-aa"
    expected_role["current"] = 1
    expected_role["baseline"] = 1
    expected_role["current-aa"] = 1
    expected_role["baseline-aa"] = 1
}

{
    sub(/\r$/, "", $0)

    if (!(input_role in expected_role)) {
        fail("internal error: input has no role")
        next
    }

    if (index($0, "TREE,hash=") == 1) {
        tree_count[input_role]++
        split($2, tree_field, "=")
        tree_hash[input_role] = tree_field[2]
    }

    if ($0 == "CSV_BEGIN") {
        begin_count[input_role]++
        if (in_csv[input_role] || end_count[input_role])
            fail(input_role ": CSV_BEGIN is duplicated or out of order")
        in_csv[input_role] = 1
        next
    }

    if ($0 == "CSV_END") {
        end_count[input_role]++
        if (!in_csv[input_role])
            fail(input_role ": CSV_END appeared without CSV_BEGIN")
        in_csv[input_role] = 0
        next
    }

    if (!in_csv[input_role])
        next

    if (!header_seen[input_role]) {
        header_seen[input_role] = 1
        if ($0 != expected_header)
            fail(input_role ": unexpected CSV header: " $0)
        next
    }

    row_count[input_role]++
    row_number = row_count[input_role]
    if (NF != 22) {
        fail(input_role ": expected 22 columns, got " NF \
             " in CSV row " row_number)
        next
    }

    valid = 1
    if ($1 == "") {
        fail(input_role ": empty workload in CSV row " row_number)
        valid = 0
    }
    if ($2 == "") {
        fail(input_role ": empty io_mode in CSV row " row_number)
        valid = 0
    }
    if (!positive_integer($3)) {
        fail(input_role ": invalid threads value in CSV row " row_number \
             ": " $3)
        valid = 0
    }
    if (!positive_number($7)) {
        fail(input_role ": invalid median_ops_per_sec in CSV row " \
             row_number ": " $7)
        valid = 0
    }
    if (!nonnegative_number($19)) {
        fail(input_role ": invalid aa_max_spread_pct in CSV row " \
             row_number ": " $19)
        valid = 0
    }
    if ($22 != "pass") {
        fail(input_role ": self-check did not pass in CSV row " row_number)
        valid = 0
    }
    if ((input_role == "current-aa" || input_role == "baseline-aa") &&
        $21 != "aa-reference") {
        fail(input_role ": CSV row " row_number \
             " is not an A/A reference")
        valid = 0
    }
    if (!valid)
        next

    key = $1 SUBSEP $2 SUBSEP $3
    if (++seen[input_role, key] > 1) {
        fail(input_role ": duplicate key " $1 "/" $2 "/" $3)
        next
    }

    all_keys[key] = 1
    key_workload[key] = $1
    key_io_mode[key] = $2
    key_threads[key] = $3
    median[input_role, key] = $7 + 0
    aa_max_spread[input_role, key] = $19 + 0

    if (input_role == "current")
        current_order[++current_rows] = key
}

END {
    for (role_index = 1; role_index <= 4; role_index++) {
        role = role_name[role_index]
        if (begin_count[role] != 1)
            fail(role ": expected exactly one CSV_BEGIN marker")
        if (end_count[role] != 1)
            fail(role ": expected exactly one CSV_END marker")
        if (in_csv[role])
            fail(role ": CSV section is unterminated")
        if (!header_seen[role])
            fail(role ": missing CSV header")
        if (row_count[role] == 0)
            fail(role ": CSV section contains no rows")
        if (tree_count[role] != 1 || tree_hash[role] == "")
            fail(role ": expected exactly one TREE,hash header")
    }

    if (tree_hash["current"] != tree_hash["current-aa"])
        fail("current and current-aa tree hashes differ")
    if (tree_hash["baseline"] != tree_hash["baseline-aa"])
        fail("baseline and baseline-aa tree hashes differ")

    if (failed)
        exit 1

    for (key in all_keys) {
        for (role_index = 1; role_index <= 4; role_index++) {
            role = role_name[role_index]
            if (!seen[role, key])
                fail(role ": missing key " describe_key(key))
        }
    }

    if (failed)
        exit 1

    global_gate = 0
    for (key in all_keys) {
        if (aa_max_spread["current-aa", key] > global_gate)
            global_gate = aa_max_spread["current-aa", key]
        if (aa_max_spread["baseline-aa", key] > global_gate)
            global_gate = aa_max_spread["baseline-aa", key]
    }

    print "workload,io_mode,threads,current_median_ops_per_sec," \
          "baseline_median_ops_per_sec,relative_abs_diff_pct," \
          "current_aa_max_spread_pct,baseline_aa_max_spread_pct," \
          "row_gate_pct,global_gate_pct,exceeds_row_gate," \
          "exceeds_global_gate"

    for (row_index = 1; row_index <= current_rows; row_index++) {
        key = current_order[row_index]
        current_median = median["current", key]
        baseline_median = median["baseline", key]
        low = current_median < baseline_median \
            ? current_median : baseline_median
        high = current_median > baseline_median \
            ? current_median : baseline_median
        difference = (high - low) * 100 / low
        displayed_difference = sprintf("%.6f", difference) + 0

        current_gate = aa_max_spread["current-aa", key]
        baseline_gate = aa_max_spread["baseline-aa", key]
        row_gate = current_gate > baseline_gate \
            ? current_gate : baseline_gate

        exceeds_row = displayed_difference > row_gate ? "yes" : "no"
        exceeds_global = displayed_difference > global_gate ? "yes" : "no"

        printf "%s,%s,%s,%.3f,%.3f,%.6f,%.6f,%.6f," \
               "%.6f,%.6f,%s,%s\n", \
               key_workload[key], key_io_mode[key], key_threads[key], \
               current_median, baseline_median, difference, \
               current_gate, baseline_gate, row_gate, global_gate, \
               exceeds_row, exceeds_global
    }
}
' \
    input_role=current "$current" \
    input_role=baseline "$baseline" \
    input_role=current-aa "$current_aa" \
    input_role=baseline-aa "$baseline_aa"
