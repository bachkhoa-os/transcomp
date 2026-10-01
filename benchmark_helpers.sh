#!/bin/bash

# Drop the Linux page cache when already authorized, but never prompt. Cache
# eviction only improves benchmark isolation; it is not required for correctness.
myfs_drop_page_cache() {
    sync

    if [ "$(id -u)" -eq 0 ]; then
        printf '3\n' > /proc/sys/vm/drop_caches 2>/dev/null || true
        return
    fi

    if command -v sudo > /dev/null 2>&1; then
        printf '3\n' | sudo -n tee /proc/sys/vm/drop_caches \
            > /dev/null 2>&1 || true
    fi
}
