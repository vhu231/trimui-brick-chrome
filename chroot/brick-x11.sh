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

# The host's /dev/shm is a symlink to /tmp/shm, which inside the chroot resolves
# to the chroot's own /tmp -- on the overlay, which the firmware mounts sync.
# Chrome still backs its shared memory with files there, so it needs a tmpfs;
# otherwise every buffer it shares with a renderer lives on the eMMC.
mkdir -p "$ROOT/tmp/shm"
grep -q " $ROOT/tmp/shm " /proc/mounts \
    || mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs "$ROOT/tmp/shm"

# The PowerVR Vulkan driver must match the kernel module exactly, so borrow the
# stock /usr/lib read-only instead of copying it; /opt/pvr/lib in the chroot
# links to just the four libraries the driver needs.
mkdir -p "$ROOT/opt/tina-usr-lib"
if ! grep -q " $ROOT/opt/tina-usr-lib " /proc/mounts; then
    mount -o bind /usr/lib "$ROOT/opt/tina-usr-lib"
    mount -o remount,bind,ro "$ROOT/opt/tina-usr-lib"
fi

# The stock firmware leaves CPU policy to whatever ran last: emulator launchers
# routinely take cores 2 and 3 offline and nothing brings them back, and
# ondemand's default sampling period here is 2 s (cpufreq-dt reports a 2 ms
# transition latency, which the governor multiplies by 1000).  Give Chrome all
# four cores and a governor that reacts within 40 ms, and put everything back
# on exit.
CPU=/sys/devices/system/cpu
OD=$CPU/cpufreq/ondemand
cpu_saved=
cpu_boost() {
    gov=$(cat $CPU/cpu0/cpufreq/scaling_governor)
    cpu_saved="$(cat $CPU/cpu2/online) $(cat $CPU/cpu3/online) $gov"
    if [ "$gov" = ondemand ]; then
        cpu_saved="$cpu_saved $(cat $OD/sampling_rate) $(cat $OD/up_threshold)"
        cpu_saved="$cpu_saved $(cat $OD/sampling_down_factor) $(cat $OD/io_is_busy)"
    fi
    echo 1 > $CPU/cpu2/online
    echo 1 > $CPU/cpu3/online
    echo ondemand > $CPU/cpu0/cpufreq/scaling_governor
    echo 40000 > $OD/sampling_rate
    echo 70 > $OD/up_threshold
    echo 5 > $OD/sampling_down_factor
    echo 1 > $OD/io_is_busy
}
cpu_restore() {
    [ -n "$cpu_saved" ] || return 0
    set -- $cpu_saved
    if [ "$3" = ondemand ] && [ "$#" -eq 7 ]; then
        echo "$4" > $OD/sampling_rate
        echo "$5" > $OD/up_threshold
        echo "$6" > $OD/sampling_down_factor
        echo "$7" > $OD/io_is_busy
    else
        echo "$3" > $CPU/cpu0/cpufreq/scaling_governor
    fi
    echo "$1" > $CPU/cpu2/online
    echo "$2" > $CPU/cpu3/online
}

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
    cpu_restore 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cpu_boost 2>/dev/null || true

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
