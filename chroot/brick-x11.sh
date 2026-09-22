#!/bin/sh
set -eu

ROOT=/opt/brick-chrome
DISPLAY_NUM=:1

bind_once() {
    source=$1
    target=$ROOT$1
    mkdir -p "$target"
    if ! grep -q " $target " /proc/mounts; then
        mount -o bind "$source" "$target"
    fi
}

bind_once /proc
bind_once /sys
bind_once /dev
bind_once /dev/pts
cp /etc/resolv.conf "$ROOT/etc/resolv.conf"

# Chrome's profile and cache would otherwise grow inside the chroot, which sits
# on the overlay root filesystem the stock system shares -- and that had got
# down to a few hundred megabytes free.  Keep them on UDISK, which is ext4 with
# room to spare.  Not the SD card: it is vfat, and Chrome's LevelDB and SQLite
# stores need POSIX permissions and locking that vfat cannot give them.
STORE=/mnt/UDISK/brick-chrome

relocate() {
    source=$STORE/$1
    target=$ROOT$2
    [ -d /mnt/UDISK ] || return 0
    if [ ! -d "$source" ]; then
        mkdir -p "$STORE" || return 0
        if [ -d "$target" ] && ! grep -q " $target " /proc/mounts; then
            mv "$target" "$source" || return 0
        else
            mkdir -p "$source"
        fi
        chown -R 1000:1000 "$source"
    fi
    mkdir -p "$target"
    grep -q " $target " /proc/mounts || mount -o bind "$source" "$target"
}

relocate profile /home/brick/.config/brick-profile
relocate cache   /home/brick/.cache

xpid=
padpid=
cleanup() {
    [ -z "$padpid" ] || kill "$padpid" 2>/dev/null || true
    [ -z "$xpid" ] || kill "$xpid" 2>/dev/null || true
    [ -z "$xpid" ] || wait "$xpid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

chroot "$ROOT" /usr/lib/xorg/Xorg "$DISPLAY_NUM" \
    -config /etc/X11/xorg.conf -novtswitch -sharevts -keeptty \
    -nolisten tcp -s 0 -logfile /tmp/brick-xorg.log \
    >"$ROOT/tmp/brick-xorg-stdout.log" 2>&1 &
xpid=$!

i=0
while [ ! -S "$ROOT/tmp/.X11-unix/X1" ]; do
    if ! kill -0 "$xpid" 2>/dev/null; then
        echo "X11 failed; see $ROOT/tmp/brick-xorg.log" >&2
        exit 1
    fi
    i=$((i + 1))
    [ "$i" -lt 50 ] || { echo "X11 startup timed out" >&2; exit 1; }
    sleep 0.2
done

DISPLAY="$DISPLAY_NUM" chroot "$ROOT" /usr/bin/python3 \
    /usr/local/bin/brick-pad.py >"$ROOT/tmp/brick-pad.log" 2>&1 &
padpid=$!

if [ "$#" -eq 0 ]; then
    set -- /usr/bin/xterm
fi
DISPLAY="$DISPLAY_NUM" chroot "$ROOT" "$@"
