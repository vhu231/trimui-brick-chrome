#!/bin/sh
# TRIMUI Apps entry: MainUI exits before this runs, so the framebuffer is free.
LOG=/opt/brick-chrome/tmp/brick-app.log
mkdir -p /opt/brick-chrome/tmp
[ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 262144 ] && rm -f "$LOG"

if [ ! -x /opt/brick-chrome/brick-x11.sh ]; then
    echo "brick-chrome is not installed" >> "$LOG"
    exit 1
fi

HERE=/mnt/SDCARD/Apps/Chrome

HOMEPAGE=https://www.bing.com
[ -f "$HERE/homepage.txt" ] && HOMEPAGE=$(head -n 1 "$HERE/homepage.txt")

# UI scale, as a percentage-style factor: 1.5 = 150%.  The chroot cannot see
# /mnt/SDCARD, so read it here and pass it through the environment.
SCALE=1.5
[ -f "$HERE/scale.txt" ] && SCALE=$(head -n 1 "$HERE/scale.txt" | tr -dc '0-9.')
case "$SCALE" in ''|.|*.*.*) SCALE=1.5 ;; esac

# Pointer speed in pixels per frame at full stick deflection (50 frames/s).
POINTER=12
[ -f "$HERE/pointer.txt" ] && POINTER=$(head -n 1 "$HERE/pointer.txt" | tr -dc '0-9.')
case "$POINTER" in ''|.|*.*.*) POINTER=12 ;; esac

export BRICK_POINTER_SPEED="$POINTER"
export BRICK_SCALE="$SCALE"
export BRICK_WIN_W=$(awk -v s="$SCALE" 'BEGIN{printf "%d", 1024/s + 0.5}')
export BRICK_WIN_H=$(awk -v s="$SCALE" 'BEGIN{printf "%d", 768/s + 0.5}')

exec /opt/brick-chrome/brick-x11.sh /usr/local/bin/brick-chrome-start \
    "$HOMEPAGE" >> "$LOG" 2>&1
