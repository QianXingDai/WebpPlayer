#include <jni.h>
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
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

// ---------------------------------------------------------------------------
// AHardwareBuffer frame storage (zero-copy GPU path).
//
// Frames are decoded straight into AHardwareBuffers (CPU/GPU shared memory).
// The renderer wraps each one in an EGLImage and samples it directly — no
// per-frame glTexSubImage2D upload, no staging copy, ever. Lifetime rides on
// AHardwareBuffer's own refcount: every Java HardwareBuffer wrapper holds one
// reference, clone = new wrapper, release = HardwareBuffer.close().
// ---------------------------------------------------------------------------

// Writes one decoded canvas (scaled if needed) into a fresh AHardwareBuffer
// and returns a Java HardwareBuffer that owns it. Returns null on failure.
jobject WriteFrameToHardwareBuffer(
    JNIEnv* env,
    const uint8_t* canvas,
    int origW,
    int origH,
    int finalW,
    int finalH,
    bool needScale) {
  AHardwareBuffer_Desc desc = {};
  desc.width = static_cast<uint32_t>(finalW);
  desc.height = static_cast<uint32_t>(finalH);
  desc.layers = 1;
  desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  // CPU_READ_RARELY keeps a software-upload escape hatch for devices where
  // EGLImage creation fails at render time.
  desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY |
               AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

  AHardwareBuffer* ahb = nullptr;
  if (AHardwareBuffer_allocate(&desc, &ahb) != 0 || ahb == nullptr) {
    LOGE("WriteFrameToHardwareBuffer: allocate failed (%dx%d)", finalW, finalH);
    return nullptr;
  }

  void* dst = nullptr;
  if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, nullptr, &dst) != 0 ||
      dst == nullptr) {
    LOGE("WriteFrameToHardwareBuffer: lock failed");
    AHardwareBuffer_release(ahb);
    return nullptr;
  }

  AHardwareBuffer_Desc actual = {};
  AHardwareBuffer_describe(ahb, &actual);
  const int dstStrideBytes = static_cast<int>(actual.stride) * 4;

  if (needScale) {
    // libyuv handles the destination stride natively
    libyuv::ARGBScale(
        canvas, origW * 4, origW, origH,
        static_cast<uint8_t*>(dst), dstStrideBytes, finalW, finalH,
        libyuv::kFilterBilinear);
  } else if (static_cast<int>(actual.stride) == finalW) {
    std::memcpy(dst, canvas, static_cast<size_t>(finalW) * finalH * 4);
  } else {
    for (int y = 0; y < finalH; ++y) {
      std::memcpy(
          static_cast<uint8_t*>(dst) + static_cast<size_t>(y) * dstStrideBytes,
          canvas + static_cast<size_t>(y) * finalW * 4,
          static_cast<size_t>(finalW) * 4);
    }
  }

  AHardwareBuffer_unlock(ahb, nullptr);

  jobject wrapper = AHardwareBuffer_toHardwareBuffer(env, ahb);
  // The Java wrapper holds its own reference; drop the allocation reference so
  // the wrapper is the sole owner.
  AHardwareBuffer_release(ahb);
  if (wrapper == nullptr) {
    LOGE("WriteFrameToHardwareBuffer: toHardwareBuffer failed");
  }
  return wrapper;
}

// Calls HardwareBuffer.close() on every non-null element (rollback helper).
void CloseHardwareBuffersInArray(JNIEnv* env, jobjectArray array) {
  if (array == nullptr) return;
  jclass hbCls = env->FindClass("android/hardware/HardwareBuffer");
  if (hbCls == nullptr) return;
  jmethodID closeMethod = env->GetMethodID(hbCls, "close", "()V");
  env->DeleteLocalRef(hbCls);
  if (closeMethod == nullptr) return;
  const jsize len = env->GetArrayLength(array);
  for (jsize i = 0; i < len; ++i) {
    jobject hb = env->GetObjectArrayElement(array, i);
    if (hb == nullptr) continue;
    env->CallVoidMethod(hb, closeMethod);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(hb);
  }
}

// ---------------------------------------------------------------------------
// EGLImage helpers (must run on the GL thread). Extension entry points are
// resolved once via eglGetProcAddress.
// ---------------------------------------------------------------------------

PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBufferANDROID = nullptr;
PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR = nullptr;
PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES = nullptr;
std::once_flag g_egl_init_flag;
bool g_egl_ready = false;

bool EnsureEglProcs() {
  std::call_once(g_egl_init_flag, [] {
    p_eglGetNativeClientBufferANDROID =
        reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    p_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    p_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    p_glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    g_egl_ready = p_eglGetNativeClientBufferANDROID != nullptr &&
                  p_eglCreateImageKHR != nullptr &&
                  p_eglDestroyImageKHR != nullptr &&
                  p_glEGLImageTargetTexture2DOES != nullptr;
    if (!g_egl_ready) {
      LOGE("EnsureEglProcs: EGLImage extensions unavailable, falling back to software upload");
    }
  });
  return g_egl_ready;
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
// useHardwareBuffers: frames go into AHardwareBuffers (HardwareBuffer[] in the
// result) instead of malloc'd direct ByteBuffers.
// ---------------------------------------------------------------------------

jobject DecodeAllFramesCore(
    JNIEnv* env,
    const uint8_t* bytes,
    size_t size,
    int targetW,
    int targetH,
    bool useHardwareBuffers) {
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

  jclass elemCls = env->FindClass(
      useHardwareBuffers ? "android/hardware/HardwareBuffer" : "java/nio/ByteBuffer");
  if (elemCls == nullptr) {
    LOGE("decode: Failed to find frame element class");
    WebPAnimDecoderDelete(dec);
    return nullptr;
  }

  jobjectArray frameArray = env->NewObjectArray(frameCount, elemCls, nullptr);
  env->DeleteLocalRef(elemCls);
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

    if (useHardwareBuffers) {
      jobject hb = WriteFrameToHardwareBuffer(env, canvas, origW, origH, finalW, finalH, needScale);
      if (hb == nullptr) {
        failed = true;
        break;
      }
      env->SetObjectArrayElement(frameArray, i, hb);
      env->DeleteLocalRef(hb);
      continue;
    }

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
    if (useHardwareBuffers) CloseHardwareBuffersInArray(env, frameArray);
    env->DeleteLocalRef(frameArray);
    return nullptr;
  }

  // Frees everything accumulated so far (both modes); used by failure paths below.
  auto cleanupFrames = [&]() {
    ReleaseAll(allocated_buffers);
    if (useHardwareBuffers) CloseHardwareBuffersInArray(env, frameArray);
    env->DeleteLocalRef(frameArray);
  };

  jclass resultCls = env->FindClass("io/webpkit/player/WebPAnimResult");
  if (resultCls == nullptr) {
    LOGE("decode: Failed to find WebPAnimResult class");
    cleanupFrames();
    return nullptr;
  }

  jmethodID ctor = env->GetMethodID(
      resultCls, "<init>",
      "([Ljava/nio/ByteBuffer;[Landroid/hardware/HardwareBuffer;II[II)V");
  if (ctor == nullptr) {
    LOGE("decode: Failed to find WebPAnimResult constructor");
    env->DeleteLocalRef(resultCls);
    cleanupFrames();
    return nullptr;
  }

  jintArray jdur = env->NewIntArray(frameCount);
  if (jdur == nullptr) {
    LOGE("decode: Failed to create duration array");
    env->DeleteLocalRef(resultCls);
    cleanupFrames();
    return nullptr;
  }
  if (frameCount > 0) {
    env->SetIntArrayRegion(jdur, 0, frameCount, durations.data());
  }

  jobject result = env->NewObject(
      resultCls, ctor,
      useHardwareBuffers ? nullptr : frameArray,
      useHardwareBuffers ? frameArray : nullptr,
      finalW, finalH, jdur, loopCount);
  if (result == nullptr) {
    LOGE("decode: Failed to create WebPAnimResult object");
    ReleaseAll(allocated_buffers);
    if (useHardwareBuffers) CloseHardwareBuffersInArray(env, frameArray);
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
      env, reinterpret_cast<const uint8_t*>(input), static_cast<size_t>(length), 0, 0,
      /*useHardwareBuffers=*/false);

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
      targetWidth, targetHeight, /*useHardwareBuffers=*/false);

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
      targetWidth, targetHeight, /*useHardwareBuffers=*/false);
}

// Like decodeAllFramesDirect, with an opt-in zero-copy mode: frames decode
// straight into AHardwareBuffers (HardwareBuffer[] in the result) for
// EGLImage-backed rendering. Falls back to the software ByteBuffer path if
// any hardware allocation fails.
JNIEXPORT jobject JNICALL
Java_io_webpkit_player_WebPYUVDecoder_decodeAllFramesDirectEx(
    JNIEnv* env,
    jobject,
    jobject buffer,
    jint dataSize,
    jint targetWidth,
    jint targetHeight,
    jboolean preferHardware) {
  if (buffer == nullptr || dataSize <= 0) return nullptr;
  void* addr = env->GetDirectBufferAddress(buffer);
  const jlong cap = env->GetDirectBufferCapacity(buffer);
  if (addr == nullptr || cap < static_cast<jlong>(dataSize)) {
    LOGE("decodeAllFramesDirectEx: invalid direct buffer (cap=%lld, size=%d)",
         (long long)cap, (int)dataSize);
    return nullptr;
  }

  if (preferHardware == JNI_TRUE) {
    jobject result = DecodeAllFramesCore(
        env, static_cast<const uint8_t*>(addr), static_cast<size_t>(dataSize),
        targetWidth, targetHeight, /*useHardwareBuffers=*/true);
    if (result != nullptr) return result;
    if (env->ExceptionCheck()) env->ExceptionClear();
    LOGE("decodeAllFramesDirectEx: hardware-buffer path failed, falling back to software");
  }

  return DecodeAllFramesCore(
      env, static_cast<const uint8_t*>(addr), static_cast<size_t>(dataSize),
      targetWidth, targetHeight, /*useHardwareBuffers=*/false);
}

// Clone = one new Java HardwareBuffer wrapper per frame, each holding its own
// reference to the same underlying AHardwareBuffer. All-or-nothing.
JNIEXPORT jobjectArray JNICALL
Java_io_webpkit_player_WebPYUVDecoder_cloneHardwareBuffers(
    JNIEnv* env, jobject, jobjectArray frameArray) {
  if (frameArray == nullptr) return nullptr;

  jclass hbCls = env->FindClass("android/hardware/HardwareBuffer");
  if (hbCls == nullptr) return nullptr;
  const jsize len = env->GetArrayLength(frameArray);
  jobjectArray cloned = env->NewObjectArray(len, hbCls, nullptr);
  env->DeleteLocalRef(hbCls);
  if (cloned == nullptr) return nullptr;

  for (jsize i = 0; i < len; ++i) {
    jobject hb = env->GetObjectArrayElement(frameArray, i);
    if (hb == nullptr) continue;
    AHardwareBuffer* ahb = AHardwareBuffer_fromHardwareBuffer(env, hb);
    env->DeleteLocalRef(hb);
    if (ahb == nullptr) {
      LOGE("cloneHardwareBuffers: fromHardwareBuffer failed at index %d", (int)i);
      CloseHardwareBuffersInArray(env, cloned);
      env->DeleteLocalRef(cloned);
      return nullptr;
    }
    jobject wrapper = AHardwareBuffer_toHardwareBuffer(env, ahb);
    if (wrapper == nullptr) {
      LOGE("cloneHardwareBuffers: toHardwareBuffer failed at index %d", (int)i);
      CloseHardwareBuffersInArray(env, cloned);
      env->DeleteLocalRef(cloned);
      return nullptr;
    }
    env->SetObjectArrayElement(cloned, i, wrapper);
    env->DeleteLocalRef(wrapper);
  }
  return cloned;
}

// ---- EGLImage bridge (GL thread only) ----

JNIEXPORT jlong JNICALL
Java_io_webpkit_player_WebPYUVDecoder_eglImageCreate(
    JNIEnv* env, jobject, jobject hardwareBuffer) {
  if (hardwareBuffer == nullptr || !EnsureEglProcs()) return 0;
  AHardwareBuffer* ahb = AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer);
  if (ahb == nullptr) return 0;
  EGLClientBuffer clientBuf = p_eglGetNativeClientBufferANDROID(ahb);
  if (clientBuf == nullptr) return 0;
  const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  EGLImageKHR image = p_eglCreateImageKHR(
      eglGetDisplay(EGL_DEFAULT_DISPLAY), EGL_NO_CONTEXT,
      EGL_NATIVE_BUFFER_ANDROID, clientBuf, attrs);
  if (image == EGL_NO_IMAGE_KHR) {
    LOGE("eglImageCreate: eglCreateImageKHR failed (0x%x)", eglGetError());
    return 0;
  }
  return reinterpret_cast<jlong>(image);
}

// Binds the EGLImage as the storage of the currently bound GL_TEXTURE_2D.
JNIEXPORT void JNICALL
Java_io_webpkit_player_WebPYUVDecoder_eglImageTargetTexture2D(
    JNIEnv*, jobject, jlong image) {
  if (image != 0 && EnsureEglProcs()) {
    p_glEGLImageTargetTexture2DOES(
        GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(image));
  }
}

JNIEXPORT void JNICALL
Java_io_webpkit_player_WebPYUVDecoder_eglImageDestroy(
    JNIEnv*, jobject, jlong image) {
  if (image != 0 && EnsureEglProcs()) {
    p_eglDestroyImageKHR(
        eglGetDisplay(EGL_DEFAULT_DISPLAY), reinterpret_cast<EGLImageKHR>(image));
  }
}

// Software escape hatch when EGLImage creation fails: locks the buffer for CPU
// read and uploads into the currently bound (and pre-allocated) texture.
JNIEXPORT jboolean JNICALL
Java_io_webpkit_player_WebPYUVDecoder_uploadHardwareBufferToTexture(
    JNIEnv* env, jobject, jobject hardwareBuffer, jint width, jint height) {
  if (hardwareBuffer == nullptr || width <= 0 || height <= 0) return JNI_FALSE;
  AHardwareBuffer* ahb = AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer);
  if (ahb == nullptr) return JNI_FALSE;
  void* src = nullptr;
  if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr, &src) != 0 ||
      src == nullptr) {
    return JNI_FALSE;
  }
  AHardwareBuffer_Desc desc = {};
  AHardwareBuffer_describe(ahb, &desc);
  if (static_cast<jint>(desc.stride) == width) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RGBA, GL_UNSIGNED_BYTE, src);
  } else {
    // GLES2 has no UNPACK_ROW_LENGTH: upload row by row
    for (jint y = 0; y < height; ++y) {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, 1,
                      GL_RGBA, GL_UNSIGNED_BYTE,
                      static_cast<const uint8_t*>(src) +
                          static_cast<size_t>(y) * desc.stride * 4);
    }
  }
  AHardwareBuffer_unlock(ahb, nullptr);
  return JNI_TRUE;
}

#ifdef __cplusplus
}  // extern "C"
#endif
