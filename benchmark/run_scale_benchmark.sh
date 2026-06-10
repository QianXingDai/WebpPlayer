#!/usr/bin/env bash
# 编译并运行缩放实现对比基准：旧版手写逐通道双线性 vs libyuv ARGBScale。
# libyuv 直接复用 webpview/src/main/cpp/libyuv 下 vendor 的同一份源码，
# 编译宏与 Android 构建保持一致（禁用 SVE/SME，启用 NEON）。
set -euo pipefail
cd "$(dirname "$0")"

LIBYUV=../webpview/src/main/cpp/libyuv
OUT=build
mkdir -p "$OUT"

# 与 CMakeLists 相同的源码集合：排除依赖 libjpeg 的文件
SRCS=$(ls "$LIBYUV"/source/*.cc | grep -vE "(mjpeg_decoder|mjpeg_validate|convert_jpeg)\.cc$")

# *_neon64.cc 需要 dotprod/i8mm 的 -march 才能汇编（与 Android 构建一致，
# 运行时 CPU 检测保证不支持的机器不会执行到这些内核）
NEON64_SRCS=""
OTHER_SRCS=""
for f in $SRCS; do
  case "$f" in
    *_neon64.cc) NEON64_SRCS="$NEON64_SRCS $f" ;;
    *)           OTHER_SRCS="$OTHER_SRCS $f" ;;
  esac
done

CXX=${CXX:-clang++}
CXXFLAGS="-O2 -std=c++17 -I$LIBYUV/include -DLIBYUV_DISABLE_SVE -DLIBYUV_DISABLE_SME"

echo "[1/3] 编译 libyuv 通用部分..."
mkdir -p "$OUT/obj"
for f in $OTHER_SRCS; do
  o="$OUT/obj/$(basename "$f" .cc).o"
  [ "$o" -nt "$f" ] || $CXX $CXXFLAGS -c "$f" -o "$o"
done

echo "[2/3] 编译 libyuv neon64 内核 (-march=armv8.2-a+dotprod+i8mm)..."
for f in $NEON64_SRCS; do
  o="$OUT/obj/$(basename "$f" .cc).o"
  [ "$o" -nt "$f" ] || $CXX $CXXFLAGS -march=armv8.2-a+dotprod+i8mm -c "$f" -o "$o"
done

echo "[3/3] 编译并链接基准程序..."
$CXX $CXXFLAGS scale_benchmark.cpp "$OUT"/obj/*.o -o "$OUT/scale_benchmark"

echo
"$OUT/scale_benchmark" "$@"
