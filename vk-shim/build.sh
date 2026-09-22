#!/bin/sh
# Build the Vulkan WSI layer and the Xorg module on the host.  Needs zig
# (https://ziglang.org/download/ -- self-contained, nothing to install) and the
# Vulkan headers (`brew install vulkan-headers`, or point VULKAN_INCLUDE at a
# Vulkan-Headers checkout).  Neither output links against anything but glibc:
# xcb is loaded at run time and the Xorg symbols resolve against the server.
#   ZIG=/path/to/zig vk-shim/build.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ZIG=${ZIG:-zig}
INC=${VULKAN_INCLUDE:-$(brew --prefix 2>/dev/null || echo /usr)/include}
[ -f "$INC/vulkan/vk_layer.h" ] || { echo "vulkan/vk_layer.h not found under $INC" >&2; exit 1; }
# glibc 2.17 is the oldest aarch64 glibc, so the result loads against any.
CC="$ZIG cc -target aarch64-linux-gnu.2.17 -O2 -Wall -Wextra -fno-sanitize=all -shared -fPIC -fvisibility=hidden -s"
$CC -I"$INC" -o "$HERE/libVkLayer_brick_x11_wsi.so" "$HERE/brick_x11_wsi.c" -lpthread -ldl
$CC -nostdlib -Wl,-z,undefs -o "$HERE/libbrickext.so" "$HERE/brick_xext.c"
echo "built $HERE/libVkLayer_brick_x11_wsi.so $HERE/libbrickext.so"
