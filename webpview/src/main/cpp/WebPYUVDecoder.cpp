#include <jni.h>
#include <android/log.h>
#include "webp/demux.h"
#include "webp/decode.h"
#include "libyuv/scale_argb.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "WebPYUVDecoder", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "WebPYUVDecoder", __VA_ARGS__)

namespace {

// ---------------------------------------------------------------------------
// Owned-buffer registry.
//
// Ownership/refcount of every buffer we hand to Java lives in a side table
// keyed by pointer, so checking whether a buffer is ours never dereferences
// memory around a foreign pointer (the old header-before-the-data scheme read
// 16 bytes in front of arbitrary direct buffers).
//
// clone is a refcount bump instead of a deep copy: frame pixels are immutable
// after decode, so every clone can share the same backing memory.
// ---------------------------------------------------------------------------

struct OwnedBufferInfo {
  size_t size;
  int refcount;
};

std::mutex g_registry_mutex;
std::unordered_map<void*, OwnedBufferInfo> g_owned_buffers;

void* AllocateOwnedBuffer(size_t size) {
  void* ptr = std::malloc(size > 0 ? size : 1);
  if (ptr == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  g_owned_buffers[ptr] = OwnedBufferInfo{size, 1};
  return ptr;
}

bool RetainOwnedBuffer(void* ptr, jlong capacity) {
  if (ptr == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  auto it = g_owned_buffers.find(ptr);
  if (it == g_owned_buffers.end()) return false;
  if (static_cast<jlong>(it->second.size) != capacity) return false;
  ++it->second.refcount;
  return true;
}

// Decrements the refcount; frees when it hits zero.
// Returns the number of bytes actually freed (0 if still referenced or foreign).
size_t ReleaseOwnedBuffer(void* ptr) {
  if (ptr == nullptr) return 0;
  size_t freed = 0;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto it = g_owned_buffers.find(ptr);
    if (it == g_owned_buffers.end()) return 0;
    if (--it->second.refcount > 0) return 0;
    freed = it->second.size;
    g_owned_buffers.erase(it);
  }
  std::free(ptr);
  return freed;
}

// ---------------------------------------------------------------------------
// RGBA frame scaling, delegated to libyuv's NEON-optimized ARGBScale.
// Scaling treats the 4 channels identically, so the RGBA byte order (libyuv
// calls its 4x8bit format "ARGB") and premultiplied alpha are both fine.
// ---------------------------------------------------------------------------

void ScaleRgbaFrame(
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

// Aspect-fit the original canvas inside the target box. Never upscales: a
// target larger than the source keeps the original size (GPU sampling handles
// magnification for free). Output dimensions are kept even (legacy contract of
// WebPAnimResult) but never collapse below 2.
void ComputeFinalSize(int origW, int origH, int targetW, int targetH, int* outW, int* outH) {
  int w = origW;
  int h = origH;
  if (targetW > 0 && targetH > 0 && (targetW < origW || targetH < origH)) {
    const double scale = std::min(
        static_cast<double>(targetW) / origW,
        static_cast<double>(targetH) / origH);
    if (scale < 1.0) {
      w = static_cast<int>(origW * scale + 0.5);
      h = static_cast<int>(origH * scale + 0.5);
    }
  }
  if (w != origW || h != origH) {
    w &= ~1;
    h &= ~1;
  }
  if (w < 2) w = std::min(origW, 2);
  if (h < 2) h = std::min(origH, 2);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  *outW = w;
  *outH = h;
}

void ReleaseAll(std::vector<void*>& buffers) {
  for (void* ptr : buffers) ReleaseOwnedBuffer(ptr);
  buffers.clear();
}

// ---------------------------------------------------------------------------
// Shared decode core. targetW/targetH <= 0 means "original size".
// Any per-frame failure fails the whole decode: callers must never receive a
// frame array with null holes (the Kotlin side types elements as non-null).
// ---------------------------------------------------------------------------

jobject DecodeAllFramesCore(
    JNIEnv* env,
    const uint8_t* bytes,
    size_t size,
    int targetW,
    int targetH) {
  if (bytes == nullptr || size == 0) {
    LOGE("decode: empty input");
    return nullptr;
  }

  WebPData webp_data;
  webp_data.bytes = bytes;
  webp_data.size = size;

  WebPAnimDecoderOptions dec_options;
  WebPAnimDecoderOptionsInit(&dec_options);
  dec_options.color_mode = MODE_rgbA;
  dec_options.use_threads = 1;

  WebPAnimDecoder* dec = WebPAnimDecoderNew(&webp_data, &dec_options);
  if (dec == nullptr) {
    LOGE("decode: WebPAnimDecoderNew failed");
    return nullptr;
  }

  WebPAnimInfo anim_info;
  if (!WebPAnimDecoderGetInfo(dec, &anim_info)) {
    LOGE("decode: WebPAnimDecoderGetInfo failed");
    WebPAnimDecoderDelete(dec);
    return nullptr;
  }

  const int origW = static_cast<int>(anim_info.canvas_width);
  const int origH = static_cast<int>(anim_info.canvas_height);
  const int frameCount = static_cast<int>(anim_info.frame_count);
  // 0 = loop forever; N = play N times (passed straight through to Java)
  const int loopCount = static_cast<int>(anim_info.loop_count);

  int finalW = origW;
  int finalH = origH;
  ComputeFinalSize(origW, origH, targetW, targetH, &finalW, &finalH);
  const bool needScale = (finalW != origW || finalH != origH);
  const size_t frameSize = static_cast<size_t>(finalW) * static_cast<size_t>(finalH) * 4;

  jclass byteBufferCls = env->FindClass("java/nio/ByteBuffer");
  if (byteBufferCls == nullptr) {
    LOGE("decode: Failed to find ByteBuffer class");
    WebPAnimDecoderDelete(dec);
    return nullptr;
  }

  jobjectArray frameArray = env->NewObjectArray(frameCount, byteBufferCls, nullptr);
  env->DeleteLocalRef(byteBufferCls);
  if (frameArray == nullptr) {
    LOGE("decode: Failed to create frame array");
    WebPAnimDecoderDelete(dec);
    return nullptr;
  }

  std::vector<int> durations(static_cast<size_t>(frameCount), 0);
  std::vector<void*> allocated_buffers;
  allocated_buffers.reserve(static_cast<size_t>(frameCount));

  uint8_t* canvas = nullptr;
  int timestamp = 0;
  int prevTimestamp = 0;
  bool failed = false;

  for (int i = 0; i < frameCount; ++i) {
    if (!WebPAnimDecoderGetNext(dec, &canvas, &timestamp)) {
      LOGE("decode: WebPAnimDecoderGetNext failed at frame %d", i);
      failed = true;
      break;
    }
    durations[i] = timestamp - prevTimestamp;
    prevTimestamp = timestamp;

    uint8_t* out = static_cast<uint8_t*>(AllocateOwnedBuffer(frameSize));
    if (out == nullptr) {
      LOGE("decode: allocation failed for frame %d (%zu bytes)", i, frameSize);
      failed = true;
      break;
    }
    allocated_buffers.push_back(out);

    if (needScale) {
      ScaleRgbaFrame(canvas, origW, origH, out, finalW, finalH);
    } else {
      std::memcpy(out, canvas, frameSize);
    }

    jobject byteBuf = env->NewDirectByteBuffer(out, static_cast<jlong>(frameSize));
    if (byteBuf == nullptr) {
      LOGE("decode: NewDirectByteBuffer failed for frame %d", i);
      failed = true;
      break;
    }
    env->SetObjectArrayElement(frameArray, i, byteBuf);
    env->DeleteLocalRef(byteBuf);
  }

  WebPAnimDecoderDelete(dec);

  if (failed) {
    ReleaseAll(allocated_buffers);
    env->DeleteLocalRef(frameArray);
    return nullptr;
  }

  jclass resultCls = env->FindClass("io/webpkit/player/WebPAnimResult");
  if (resultCls == nullptr) {
    LOGE("decode: Failed to find WebPAnimResult class");
    ReleaseAll(allocated_buffers);
    env->DeleteLocalRef(frameArray);
    return nullptr;
  }

  jmethodID ctor = env->GetMethodID(resultCls, "<init>", "([Ljava/nio/ByteBuffer;II[II)V");
  if (ctor == nullptr) {
    LOGE("decode: Failed to find WebPAnimResult constructor");
    env->DeleteLocalRef(resultCls);
    ReleaseAll(allocated_buffers);
    env->DeleteLocalRef(frameArray);
    return nullptr;
  }

  jintArray jdur = env->NewIntArray(frameCount);
  if (jdur == nullptr) {
    LOGE("decode: Failed to create duration array");
    env->DeleteLocalRef(resultCls);
    ReleaseAll(allocated_buffers);
    env->DeleteLocalRef(frameArray);
    return nullptr;
  }
  if (frameCount > 0) {
    env->SetIntArrayRegion(jdur, 0, frameCount, durations.data());
  }

  jobject result = env->NewObject(resultCls, ctor, frameArray, finalW, finalH, jdur, loopCount);
  if (result == nullptr) {
    LOGE("decode: Failed to create WebPAnimResult object");
    ReleaseAll(allocated_buffers);
  }

  env->DeleteLocalRef(resultCls);
  env->DeleteLocalRef(jdur);
  env->DeleteLocalRef(frameArray);
  return result;
}

// Clone = refcount bump on every owned buffer; falls back to a deep copy for
// foreign direct buffers. All-or-nothing: any failure rolls back acquired refs.
jobjectArray CloneFrames(JNIEnv* env, jobjectArray frameArray) {
  if (frameArray == nullptr) return nullptr;

  jclass byteBufferCls = env->FindClass("java/nio/ByteBuffer");
  if (byteBufferCls == nullptr) {
    LOGE("CloneFrames: Failed to find ByteBuffer class");
    return nullptr;
  }

  const jsize len = env->GetArrayLength(frameArray);
  jobjectArray cloned = env->NewObjectArray(len, byteBufferCls, nullptr);
  env->DeleteLocalRef(byteBufferCls);
  if (cloned == nullptr) {
    LOGE("CloneFrames: Failed to create output array");
    return nullptr;
  }

  std::vector<void*> acquired;  // refs/allocations to roll back on failure
  acquired.reserve(static_cast<size_t>(len));

  for (jsize i = 0; i < len; ++i) {
    jobject buf = env->GetObjectArrayElement(frameArray, i);
    if (buf == nullptr) continue;

    const jlong cap = env->GetDirectBufferCapacity(buf);
    void* src = env->GetDirectBufferAddress(buf);
    env->DeleteLocalRef(buf);
    if (src == nullptr || cap <= 0) {
      LOGE("CloneFrames: Invalid direct buffer at index %d", static_cast<int>(i));
      ReleaseAll(acquired);
      env->DeleteLocalRef(cloned);
      return nullptr;
    }

    void* shared = nullptr;
    if (RetainOwnedBuffer(src, cap)) {
      shared = src;
    } else {
      // Foreign buffer: keep old deep-copy behavior.
      shared = AllocateOwnedBuffer(static_cast<size_t>(cap));
      if (shared == nullptr) {
        LOGE("CloneFrames: allocation failed at index %d", static_cast<int>(i));
        ReleaseAll(acquired);
        env->DeleteLocalRef(cloned);
        return nullptr;
      }
      std::memcpy(shared, src, static_cast<size_t>(cap));
    }
    acquired.push_back(shared);

    jobject clonedBuf = env->NewDirectByteBuffer(shared, cap);
    if (clonedBuf == nullptr) {
      LOGE("CloneFrames: NewDirectByteBuffer failed at index %d", static_cast<int>(i));
      ReleaseAll(acquired);
      env->DeleteLocalRef(cloned);
      return nullptr;
    }
    env->SetObjectArrayElement(cloned, i, clonedBuf);
    env->DeleteLocalRef(clonedBuf);
  }

  return cloned;
}

}  // namespace

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jint JNICALL
Java_io_webpkit_player_WebPYUVDecoder_releaseNativeBuffers(JNIEnv* env, jobject, jobjectArray frameArray) {
  if (frameArray == nullptr) return 0;

  jlong totalBytesFreed = 0;
  const jsize len = env->GetArrayLength(frameArray);

  for (jsize i = 0; i < len; i++) {
    jobject buf = env->GetObjectArrayElement(frameArray, i);
    if (buf == nullptr) continue;

    void* ptr = env->GetDirectBufferAddress(buf);
    env->DeleteLocalRef(buf);
    totalBytesFreed += static_cast<jlong>(ReleaseOwnedBuffer(ptr));
  }

  const jint totalKB = static_cast<jint>((totalBytesFreed + 1023) / 1024);
  if (totalKB > 0) {
    LOGI("releaseNativeBuffers: total freed=%lld bytes (%d KB)", (long long)totalBytesFreed, totalKB);
  }
  return totalKB;
}

JNIEXPORT jobjectArray JNICALL
Java_io_webpkit_player_WebPYUVDecoder_cloneNativeBuffers(JNIEnv* env, jobject, jobjectArray frameArray) {
  return CloneFrames(env, frameArray);
}

JNIEXPORT jobject JNICALL
Java_io_webpkit_player_WebPYUVDecoder_decodeAllFrames(JNIEnv* env, jobject, jbyteArray data) {
  if (data == nullptr) return nullptr;
  jbyte* input = env->GetByteArrayElements(data, nullptr);
  if (input == nullptr) return nullptr;
  const jsize length = env->GetArrayLength(data);

  jobject result = DecodeAllFramesCore(
      env, reinterpret_cast<const uint8_t*>(input), static_cast<size_t>(length), 0, 0);

  env->ReleaseByteArrayElements(data, input, JNI_ABORT);
  return result;
}

JNIEXPORT jobject JNICALL
Java_io_webpkit_player_WebPYUVDecoder_decodeAllFramesWithSize(
    JNIEnv* env,
    jobject,
    jbyteArray data,
    jint targetWidth,
    jint targetHeight) {
  if (data == nullptr) return nullptr;
  jbyte* input = env->GetByteArrayElements(data, nullptr);
  if (input == nullptr) return nullptr;
  const jsize length = env->GetArrayLength(data);

  jobject result = DecodeAllFramesCore(
      env, reinterpret_cast<const uint8_t*>(input), static_cast<size_t>(length),
      targetWidth, targetHeight);

  env->ReleaseByteArrayElements(data, input, JNI_ABORT);
  return result;
}

// Zero-copy entry point: reads the webp payload straight out of a direct
// ByteBuffer, so the whole file is never pinned/copied through a Java array
// while the (potentially long) decode runs.
JNIEXPORT jobject JNICALL
Java_io_webpkit_player_WebPYUVDecoder_decodeAllFramesDirect(
    JNIEnv* env,
    jobject,
    jobject buffer,
    jint dataSize,
    jint targetWidth,
    jint targetHeight) {
  if (buffer == nullptr || dataSize <= 0) return nullptr;
  void* addr = env->GetDirectBufferAddress(buffer);
  const jlong cap = env->GetDirectBufferCapacity(buffer);
  if (addr == nullptr || cap < static_cast<jlong>(dataSize)) {
    LOGE("decodeAllFramesDirect: invalid direct buffer (cap=%lld, size=%d)",
         (long long)cap, (int)dataSize);
    return nullptr;
  }
  return DecodeAllFramesCore(
      env, static_cast<const uint8_t*>(addr), static_cast<size_t>(dataSize),
      targetWidth, targetHeight);
}

#ifdef __cplusplus
}  // extern "C"
#endif
