#!/bin/sh
# Build switch_rumble_bridge for arm64 Android with the extracted NDK r27c.
set -e
NDK=/d/Develop/AndroidDev/ndk/android-ndk-r27c
CC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
DIR=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DIR/bin"
"$CC" --target=aarch64-linux-android28 -O2 -Wall -Wextra \
  -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
  -o "$DIR/bin/switch_rumble_bridge" \
  "$DIR/src/switch_rumble_bridge.c"
file "$DIR/bin/switch_rumble_bridge" 2>/dev/null || ls -l "$DIR/bin"
echo BUILD_OK
