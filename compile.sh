#!/usr/bin/env bash
# One-command native build for libwebpkit.so (arm64-v8a).
#
# NOTE: you usually do NOT need to run this — `./gradlew :webpview:assembleRelease`
# already compiles this exact native library from the bundled libwebp source and
# packages it into the AAR. This script is here for the standalone flow: it builds
# the .so on its own into dist/ so you can inspect or hand-place it.
set -e

# ==== detect Android NDK ====
if [ -z "$ANDROID_NDK" ]; then
  export ANDROID_NDK=~/Library/Android/sdk/ndk/27.2.12479018
  echo "⚙️  ANDROID_NDK not set, using default: $ANDROID_NDK"
else
  echo "✅ Using ANDROID_NDK from environment: $ANDROID_NDK"
fi

ABI=arm64-v8a
PLATFORM=android-28
CPP_DIR="$(cd "$(dirname "$0")" && pwd)/webpview/src/main/cpp"
BUILD_DIR="$(cd "$(dirname "$0")" && pwd)/build/native/$ABI"
DIST_DIR="$(cd "$(dirname "$0")" && pwd)/dist/$ABI"

CMAKE_BIN=$(command -v cmake || echo /opt/homebrew/bin/cmake)

if command -v sysctl >/dev/null 2>&1; then
  JOBS=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
else
  JOBS=4
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$DIST_DIR"

"$CMAKE_BIN" -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=$ABI \
  -DANDROID_PLATFORM=$PLATFORM \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release \
  -S "$CPP_DIR" \
  -B "$BUILD_DIR"

"$CMAKE_BIN" --build "$BUILD_DIR" --parallel "$JOBS"

cp "$BUILD_DIR/libwebpkit.so" "$DIST_DIR/libwebpkit.so"
echo "✅ Done. Output: $DIST_DIR/libwebpkit.so"
