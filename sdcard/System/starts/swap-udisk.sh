#!/bin/sh
# 1 GB of swap on UDISK.  The device ships with none and has only ~975 MB of
# RAM, so a second Chrome renderer could not start: V8 reports
# "Failed to reserve virtual memory for CodeRange" when mmap returns ENOMEM.
#
# UDISK rather than the SD card: it is ext4, measured about five times faster to
# write here, and the file lands in a handful of extents instead of a few
# hundred.  runtrimui.sh runs every *.sh in this directory at boot, in sequence,
# so keep it quick and idempotent.
SWAP=/mnt/UDISK/swapfile
SIZE_MB=1024

grep -q "^$SWAP " /proc/swaps && exit 0
if [ ! -f "$SWAP" ]; then
    dd if=/dev/zero of="$SWAP" bs=1M count="$SIZE_MB" 2>/dev/null || exit 0
fi
chmod 600 "$SWAP"
# Re-run mkswap when the signature is missing.  A half-finished attempt leaves
# the file allocated but unusable -- which is exactly what a cancelled "enable
# swap" in BrickTools leaves behind, and it fails with
# "Unable to find swap-space signature".
if ! swapon "$SWAP" 2>/dev/null; then
    mkswap "$SWAP" >/dev/null 2>&1
    swapon "$SWAP" 2>/dev/null
fi
echo 30 > /proc/sys/vm/swappiness
