#!/bin/bash
set -u

repo_root=$(cd "$(dirname "$0")/.." && pwd)
tmp_root=$(mktemp -d /tmp/myfs-suite-guard-XXXXXX)
trap 'rm -rf "$tmp_root"' EXIT
mkdir "$tmp_root/plain-mount" "$tmp_root/backing"

set +e
(cd "$repo_root" && ./test_suite.sh \
    "$tmp_root/plain-mount" "$tmp_root/backing") \
    >"$tmp_root/output.log" 2>&1
status=$?
set -e

if [ "$status" -eq 0 ]; then
    echo "test suite unexpectedly accepted an unmounted directory"
    exit 1
fi
if ! grep -q "chưa được mount" "$tmp_root/output.log"; then
    echo "test suite did not report the unmounted-directory error"
    exit 1
fi
if find "$tmp_root/plain-mount" -mindepth 1 -print -quit | grep -q .; then
    echo "test suite wrote fixtures into an unmounted directory"
    exit 1
fi
