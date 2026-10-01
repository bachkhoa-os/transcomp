#!/bin/bash
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
calls_file=$(mktemp /tmp/myfs-benchmark-sudo-XXXXXX)
trap 'rm -f "$calls_file"' EXIT

sudo() {
    printf '%s\n' "$*" >> "$calls_file"
    return 1
}

id() {
    if [ "${1:-}" = "-u" ]; then
        printf '1000\n'
        return
    fi
    command id "$@"
}

sync() {
    :
}

# shellcheck source=../benchmark_helpers.sh
. "$repo_root/benchmark_helpers.sh"

myfs_drop_page_cache

if [ "$(cat "$calls_file")" != "-n tee /proc/sys/vm/drop_caches" ]; then
    echo "benchmark cache drop did not invoke sudo in non-interactive mode" >&2
    exit 1
fi
