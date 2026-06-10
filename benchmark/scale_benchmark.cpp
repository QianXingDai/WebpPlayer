// 缩放实现对比基准：旧版手写逐通道双线性 vs libyuv ARGBScale (NEON)
//
// 旧实现原样取自 git 历史中的 WebPYUVDecoder.cpp（commit a24d59b 之前的版本），
// 新实现即当前 WebPYUVDecoder.cpp 实际调用的 libyuv::ARGBScale(kFilterBilinear)。
// 输出每个场景的单帧耗时、吞吐（MP/s）、加速比，以及两者输出的像素差异统计
// （两种双线性的相位/舍入略有不同，存在少量 1~2 级灰度差是正常的）。
//
// 用法: ./run_scale_benchmark.sh  （脚本负责编译并运行）

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "libyuv/scale_argb.h"

// ===========================================================================
// 旧实现（原样拷贝，仅函数名加 _Old 后缀）
// ===========================================================================

static inline uint8_t BilinearChannel_Old(
    const uint8_t* src,
    int width,
    int x0,
    int y0,
    int x1,
    int y1,
    int channel,
    uint32_t wx,
    uint32_t wy) {
  const uint32_t invWx = 256u - wx;
  const uint32_t invWy = 256u - wy;

  const int idx00 = (y0 * width + x0) * 4 + channel;
  const int idx10 = (y0 * width + x1) * 4 + channel;
  const int idx01 = (y1 * width + x0) * 4 + channel;
  const int idx11 = (y1 * width + x1) * 4 + channel;

  const uint32_t top = src[idx00] * invWx + src[idx10] * wx;
  const uint32_t bottom = src[idx01] * invWx + src[idx11] * wx;
  return static_cast<uint8_t>((top * invWy + bottom * wy + 32768u) >> 16);
}

static void ScaleRgbaBilinear_Old(
    const uint8_t* src,
    int srcWidth,
    int srcHeight,
    uint8_t* dst,
    int dstWidth,
    int dstHeight) {
  if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0) return;

  if (srcWidth == dstWidth && srcHeight == dstHeight) {
    std::memcpy(dst, src, static_cast<size_t>(srcWidth) * static_cast<size_t>(srcHeight) * 4);
    return;
  }

  const uint32_t xScale = (dstWidth > 1)
      ? static_cast<uint32_t>(((static_cast<uint64_t>(srcWidth - 1)) << 16) / (dstWidth - 1))
      : 0u;
  const uint32_t yScale = (dstHeight > 1)
      ? static_cast<uint32_t>(((static_cast<uint64_t>(srcHeight - 1)) << 16) / (dstHeight - 1))
      : 0u;

  for (int y = 0; y < dstHeight; ++y) {
    const uint32_t srcYFixed = yScale * static_cast<uint32_t>(y);
    const int y0 = static_cast<int>(srcYFixed >> 16);
    const int y1 = (y0 + 1 < srcHeight) ? (y0 + 1) : y0;
    const uint32_t wy = (srcHeight > 1) ? ((srcYFixed >> 8) & 0xffu) : 0u;

    for (int x = 0; x < dstWidth; ++x) {
      const uint32_t srcXFixed = xScale * static_cast<uint32_t>(x);
      const int x0 = static_cast<int>(srcXFixed >> 16);
      const int x1 = (x0 + 1 < srcWidth) ? (x0 + 1) : x0;
      const uint32_t wx = (srcWidth > 1) ? ((srcXFixed >> 8) & 0xffu) : 0u;

      const int dstIdx = (y * dstWidth + x) * 4;
      dst[dstIdx + 0] = BilinearChannel_Old(src, srcWidth, x0, y0, x1, y1, 0, wx, wy);
      dst[dstIdx + 1] = BilinearChannel_Old(src, srcWidth, x0, y0, x1, y1, 1, wx, wy);
      dst[dstIdx + 2] = BilinearChannel_Old(src, srcWidth, x0, y0, x1, y1, 2, wx, wy);
      dst[dstIdx + 3] = BilinearChannel_Old(src, srcWidth, x0, y0, x1, y1, 3, wx, wy);
    }
  }
}

// ===========================================================================
// 新实现（与 WebPYUVDecoder.cpp 中的 ScaleRgbaFrame 一致）
// ===========================================================================

static void ScaleRgbaFrame_New(
    const uint8_t* src,
    int srcWidth,
    int srcHeight,
    uint8_t* dst,
    int dstWidth,
    int dstHeight) {
  if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0) return;
  if (srcWidth == dstWidth && srcHeight == dstHeight) {
    std::memcpy(dst, src, static_cast<size_t>(srcWidth) * static_cast<size_t>(srcHeight) * 4);
    return;
  }
  libyuv::ARGBScale(
      src, srcWidth * 4, srcWidth, srcHeight,
      dst, dstWidth * 4, dstWidth, dstHeight,
      libyuv::kFilterBilinear);
}

// ===========================================================================
// 基准框架
// ===========================================================================

using ScaleFn = void (*)(const uint8_t*, int, int, uint8_t*, int, int);

static double BenchOne(ScaleFn fn,
                       const uint8_t* src, int sw, int sh,
                       uint8_t* dst, int dw, int dh,
                       int iterations) {
  // 预热
  for (int i = 0; i < 3; ++i) fn(src, sw, sh, dst, dw, dh);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    fn(src, sw, sh, dst, dw, dh);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  return totalMs / iterations;
}

struct Scenario {
  int sw, sh, dw, dh;
  const char* note;
};

int main() {
  // 典型场景：解码后整帧降采样（与 decodeAllFramesDirect 的 targetSize 用法一致）
  const Scenario scenarios[] = {
      {1080, 1080, 540, 540, "2x 降采样（大贴纸）"},
      {1080, 1080, 360, 360, "3x 降采样"},
      {1080, 1920, 270, 480, "4x 降采样（全屏动画 -> 小窗）"},
      {720,  720,  480, 480, "1.5x 降采样"},
      {512,  512,  200, 200, "2.56x 降采样（小素材）"},
      {640,  360,  600, 338, "轻微降采样（接近原尺寸）"},
  };

  std::printf("%-30s %12s %12s %8s %14s\n",
              "scenario", "old ms/帧", "libyuv ms/帧", "加速比", "最大像素差");
  std::printf("%s\n", std::string(80, '-').c_str());

  double geomean = 1.0;
  int count = 0;

  for (const Scenario& s : scenarios) {
    const size_t srcSize = static_cast<size_t>(s.sw) * s.sh * 4;
    const size_t dstSize = static_cast<size_t>(s.dw) * s.dh * 4;
    std::vector<uint8_t> src(srcSize);
    std::vector<uint8_t> dstOld(dstSize, 0);
    std::vector<uint8_t> dstNew(dstSize, 0);

    // 确定性伪随机内容（带平滑渐变 + 噪声，接近真实图像统计特性）
    std::mt19937 rng(42);
    for (int y = 0; y < s.sh; ++y) {
      for (int x = 0; x < s.sw; ++x) {
        const size_t i = (static_cast<size_t>(y) * s.sw + x) * 4;
        src[i + 0] = static_cast<uint8_t>((x * 255 / s.sw + (rng() & 15)) & 0xff);
        src[i + 1] = static_cast<uint8_t>((y * 255 / s.sh + (rng() & 15)) & 0xff);
        src[i + 2] = static_cast<uint8_t>(((x + y) * 127 / (s.sw + s.sh) + (rng() & 15)) & 0xff);
        src[i + 3] = 255;
      }
    }

    // 迭代次数按帧大小调整，让每个场景测量时长在同一量级
    const int iterations = static_cast<int>(
        std::max<int64_t>(20, 300LL * 1000 * 1000 / static_cast<int64_t>(srcSize)));

    const double oldMs = BenchOne(ScaleRgbaBilinear_Old,
                                  src.data(), s.sw, s.sh, dstOld.data(), s.dw, s.dh, iterations);
    const double newMs = BenchOne(ScaleRgbaFrame_New,
                                  src.data(), s.sw, s.sh, dstNew.data(), s.dw, s.dh, iterations);

    // 输出差异（两种双线性实现的采样相位/舍入不同，少量差异正常）
    int maxDiff = 0;
    int64_t sumDiff = 0;
    for (size_t i = 0; i < dstSize; ++i) {
      const int d = std::abs(static_cast<int>(dstOld[i]) - static_cast<int>(dstNew[i]));
      if (d > maxDiff) maxDiff = d;
      sumDiff += d;
    }
    const double meanDiff = static_cast<double>(sumDiff) / dstSize;

    const double speedup = oldMs / newMs;
    geomean *= speedup;
    ++count;

    char label[64];
    std::snprintf(label, sizeof(label), "%dx%d->%dx%d", s.sw, s.sh, s.dw, s.dh);
    std::printf("%-30s %10.3f %12.3f %7.2fx %6d (avg %.2f)\n",
                label, oldMs, newMs, speedup, maxDiff, meanDiff);
    std::printf("    %s, iters=%d\n", s.note, iterations);
  }

  std::printf("%s\n", std::string(80, '-').c_str());
  std::printf("几何平均加速比: %.2fx\n", std::pow(geomean, 1.0 / count));
  std::printf("\n注: 本机 (macOS arm64, NEON) 结果反映相对加速比；绝对耗时请在目标 Android 设备\n");
  std::printf("    上复测（A53/A55 等小核上 NEON 相对标量的收益通常更明显）。\n");
  return 0;
}
