#include <jni.h>
#include <android/log.h>
#include "webp/demux.h"
#include "webp/decode.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "WebPYUVDecoder", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "WebPYUVDecoder", __VA_ARGS__)

namespace {

constexpr uint64_t kOwnedBufferMagic = 0x5745425042554631ULL;  // "WEBPBUF1"

struct OwnedBufferHeader {
  uint64_t magic;
  size_t size;
};

void* AllocateOwnedBuffer(size_t size) {
  auto* header = static_cast<OwnedBufferHeader*>(std::malloc(sizeof(OwnedBufferHeader) + size));
  if (header == nullptr) return nullptr;
  header->magic = kOwnedBufferMagic;
  header->size = size;
  return static_cast<void*>(header + 1);
}

OwnedBufferHeader* HeaderFromData(void* data) {
  if (data == nullptr) return nullptr;
  return reinterpret_cast<OwnedBufferHeader*>(data) - 1;
}

bool IsOwnedBuffer(void* data, jlong capacity) {
  if (data == nullptr || capacity < 0) return false;
  OwnedBufferHeader* header = HeaderFromData(data);
  return header != nullptr &&
         header->magic == kOwnedBufferMagic &&
         static_cast<jlong>(header->size) == capacity;
}

void FreeOwnedBuffer(void* data) {
  OwnedBufferHeader* header = HeaderFromData(data);
  if (header != nullptr && header->magic == kOwnedBufferMagic) {
    header->magic = 0;
    std::free(header);
  }
}

inline uint8_t BilinearChannel(
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

void ScaleRgbaBilinear(
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
      dst[dstIdx + 0] = BilinearChannel(src, srcWidth, x0, y0, x1, y1, 0, wx, wy);
      dst[dstIdx + 1] = BilinearChannel(src, srcWidth, x0, y0, x1, y1, 1, wx, wy);
      dst[dstIdx + 2] = BilinearChannel(src, srcWidth, x0, y0, x1, y1, 2, wx, wy);
      dst[dstIdx + 3] = BilinearChannel(src, srcWidth, x0, y0, x1, y1, 3, wx, wy);
    }
  }
}

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

  std::vector<void*> allocated_buffers;
  for (jsize i = 0; i < len; ++i) {
    jobject buf = env->GetObjectArrayElement(frameArray, i);
    if (buf == nullptr) {
      continue;
    }

    const jlong cap = env->GetDirectBufferCapacity(buf);
    void* src = env->GetDirectBufferAddress(buf);
    if (src == nullptr || cap <= 0) {
      env->DeleteLocalRef(buf);
      LOGE("CloneFrames: Invalid direct buffer at index %d", static_cast<int>(i));
      for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
      env->DeleteLocalRef(cloned);
      return nullptr;
    }

    void* dst = AllocateOwnedBuffer(static_cast<size_t>(cap));
    if (dst == nullptr) {
      env->DeleteLocalRef(buf);
      LOGE("CloneFrames: allocation failed at index %d", static_cast<int>(i));
      for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
      env->DeleteLocalRef(cloned);
      return nullptr;
    }
    std::memcpy(dst, src, static_cast<size_t>(cap));
    allocated_buffers.push_back(dst);

    jobject clonedBuf = env->NewDirectByteBuffer(dst, cap);
    if (clonedBuf == nullptr) {
      env->DeleteLocalRef(buf);
      LOGE("CloneFrames: NewDirectByteBuffer failed at index %d", static_cast<int>(i));
      FreeOwnedBuffer(dst);
      allocated_buffers.pop_back();
      for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
      env->DeleteLocalRef(cloned);
      return nullptr;
    }

    env->SetObjectArrayElement(cloned, i, clonedBuf);
    env->DeleteLocalRef(clonedBuf);
    env->DeleteLocalRef(buf);
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

    const jlong cap = env->GetDirectBufferCapacity(buf);
    void* ptr = env->GetDirectBufferAddress(buf);
    if (IsOwnedBuffer(ptr, cap)) {
      FreeOwnedBuffer(ptr);
      totalBytesFreed += cap;
      LOGI("releaseNativeBuffers: freed buffer[%d] capacity=%lld bytes", (int)i, (long long)cap);
    } else {
      LOGI("releaseNativeBuffers: skipped foreign buffer[%d] capacity=%lld addr=%p", (int)i, (long long)cap, ptr);
    }
    env->DeleteLocalRef(buf);
  }

  const jint totalKB = static_cast<jint>((totalBytesFreed + 1023) / 1024);
  LOGI("releaseNativeBuffers: total freed=%lld bytes (%d KB)", (long long)totalBytesFreed, totalKB);
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
  jsize length = env->GetArrayLength(data);

  WebPData webp_data;
  webp_data.bytes = reinterpret_cast<const uint8_t*>(input);
  webp_data.size = static_cast<size_t>(length);

  WebPAnimDecoderOptions dec_options;
  WebPAnimDecoderOptionsInit(&dec_options);
  dec_options.color_mode = MODE_rgbA;
  dec_options.use_threads = 1;

  WebPAnimDecoder* dec = WebPAnimDecoderNew(&webp_data, &dec_options);
  if (dec == nullptr) {
    LOGE("decodeAllFrames: WebPAnimDecoderNew failed");
    env->ReleaseByteArrayElements(data, input, JNI_ABORT);
    return nullptr;
  }

  WebPAnimInfo anim_info;
  if (!WebPAnimDecoderGetInfo(dec, &anim_info)) {
    LOGE("decodeAllFrames: WebPAnimDecoderGetInfo failed");
    WebPAnimDecoderDelete(dec);
    env->ReleaseByteArrayElements(data, input, JNI_ABORT);
    return nullptr;
  }

  const int canvas_w = anim_info.canvas_width;
  const int canvas_h = anim_info.canvas_height;
  const int frame_count = anim_info.frame_count;
  const size_t canvas_size = static_cast<size_t>(canvas_w) * static_cast<size_t>(canvas_h) * 4;

  jclass byteBufferCls = env->FindClass("java/nio/ByteBuffer");
  if (byteBufferCls == nullptr) {
    LOGE("decodeAllFrames: Failed to find ByteBuffer class");
    WebPAnimDecoderDelete(dec);
    env->ReleaseByteArrayElements(data, input, JNI_ABORT);
    return nullptr;
  }

  jobjectArray frameArray = env->NewObjectArray(frame_count, byteBufferCls, nullptr);
  env->DeleteLocalRef(byteBufferCls);
  if (frameArray == nullptr) {
    LOGE("decodeAllFrames: Failed to create frame array");
    WebPAnimDecoderDelete(dec);
    env->ReleaseByteArrayElements(data, input, JNI_ABORT);
    return nullptr;
  }

  std::vector<int> durations(frame_count);
  std::vector<void*> allocated_buffers;
  uint8_t* buf = nullptr;
  int timestamp = 0;
  int prevTimestamp = 0;
  int frameIndex = 0;
  bool decodingFailed = false;

  while (WebPAnimDecoderHasMoreFrames(dec) && frameIndex < frame_count) {
    if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) {
      LOGE("decodeAllFrames: WebPAnimDecoderGetNext failed at frame %d", frameIndex);
      decodingFailed = true;
      break;
    }

    durations[frameIndex] = timestamp - prevTimestamp;
    prevTimestamp = timestamp;

    void* frame_copy = AllocateOwnedBuffer(canvas_size);
    if (frame_copy != nullptr) {
      std::memcpy(frame_copy, buf, canvas_size);
      allocated_buffers.push_back(frame_copy);
      jobject byteBuf = env->NewDirectByteBuffer(frame_copy, static_cast<jlong>(canvas_size));
      if (byteBuf != nullptr) {
        env->SetObjectArrayElement(frameArray, frameIndex, byteBuf);
        env->DeleteLocalRef(byteBuf);
      } else {
        LOGE("decodeAllFrames: Failed to create DirectByteBuffer for frame %d", frameIndex);
        FreeOwnedBuffer(frame_copy);
        allocated_buffers.pop_back();
        env->SetObjectArrayElement(frameArray, frameIndex, nullptr);
      }
    } else {
      LOGE("decodeAllFrames: allocation failed for frame %d", frameIndex);
      env->SetObjectArrayElement(frameArray, frameIndex, nullptr);
    }

    frameIndex++;
  }

  WebPAnimDecoderDelete(dec);
  env->ReleaseByteArrayElements(data, input, JNI_ABORT);

  if (decodingFailed) {
    LOGE("decodeAllFrames: Decoding failed, cleaning up");
    for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
    env->DeleteLocalRef(frameArray);
    return nullptr;
  }

  jclass resultCls = env->FindClass("io/webpkit/player/WebPAnimResult");
  if (resultCls == nullptr) {
    LOGE("decodeAllFrames: Failed to find WebPAnimResult class");
    for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
    env->DeleteLocalRef(frameArray);
    return nullptr;
  }

  jmethodID ctor = env->GetMethodID(resultCls, "<init>", "([Ljava/nio/ByteBuffer;II[I)V");
  if (ctor == nullptr) {
    LOGE("decodeAllFrames: Failed to find WebPAnimResult constructor");
    env->DeleteLocalRef(resultCls);
    for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
    env->DeleteLocalRef(frameArray);
    return nullptr;
  }

  jintArray jdur = env->NewIntArray(frame_count);
  if (jdur == nullptr) {
    LOGE("decodeAllFrames: Failed to create duration array");
    env->DeleteLocalRef(resultCls);
    for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
    env->DeleteLocalRef(frameArray);
    return nullptr;
  }

  env->SetIntArrayRegion(jdur, 0, frame_count, durations.data());
  jobject result = env->NewObject(resultCls, ctor, frameArray, canvas_w, canvas_h, jdur);
  if (result == nullptr) {
    LOGE("decodeAllFrames: Failed to create WebPAnimResult object");
    for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
  }

  env->DeleteLocalRef(resultCls);
  env->DeleteLocalRef(jdur);
  env->DeleteLocalRef(frameArray);
  return result;
}

JNIEXPORT jobject JNICALL
Java_io_webpkit_player_WebPYUVDecoder_decodeAllFramesWithSize(
    JNIEnv* env,
    jobject,
    jbyteArray data,
    jint targetWidth,
    jint targetHeight) {
  if (data == nullptr) {
    LOGE("decodeAllFramesWithSize: data is null");
    return nullptr;
  }

  jbyte* dataPtr = env->GetByteArrayElements(data, nullptr);
  jsize dataSize = env->GetArrayLength(data);
  if (dataPtr == nullptr || dataSize <= 0) {
    LOGE("decodeAllFramesWithSize: invalid data");
    if (dataPtr != nullptr) env->ReleaseByteArrayElements(data, dataPtr, JNI_ABORT);
    return nullptr;
  }

  WebPData webp_data;
  webp_data.bytes = reinterpret_cast<const uint8_t*>(dataPtr);
  webp_data.size = static_cast<size_t>(dataSize);

  WebPAnimDecoderOptions dec_options;
  WebPAnimDecoderOptionsInit(&dec_options);
  dec_options.color_mode = MODE_rgbA;
  dec_options.use_threads = 1;

  WebPAnimDecoder* dec = WebPAnimDecoderNew(&webp_data, &dec_options);
  if (dec == nullptr) {
    env->ReleaseByteArrayElements(data, dataPtr, JNI_ABORT);
    LOGE("decodeAllFramesWithSize: WebPAnimDecoderNew failed");
    return nullptr;
  }

  WebPAnimInfo anim_info;
  if (!WebPAnimDecoderGetInfo(dec, &anim_info)) {
    LOGE("decodeAllFramesWithSize: WebPAnimDecoderGetInfo failed");
    WebPAnimDecoderDelete(dec);
    env->ReleaseByteArrayElements(data, dataPtr, JNI_ABORT);
    return nullptr;
  }

  const int origWidth = anim_info.canvas_width;
  const int origHeight = anim_info.canvas_height;
  const int frameCount = anim_info.frame_count;
  const int finalWidth = (targetWidth > 0) ? (targetWidth & ~1) : (origWidth & ~1);
  const int finalHeight = (targetHeight > 0) ? (targetHeight & ~1) : (origHeight & ~1);

  jclass bufferClass = env->FindClass("java/nio/ByteBuffer");
  if (bufferClass == nullptr) {
    LOGE("decodeAllFramesWithSize: Failed to find ByteBuffer class");
    WebPAnimDecoderDelete(dec);
    env->ReleaseByteArrayElements(data, dataPtr, JNI_ABORT);
    return nullptr;
  }

  jobjectArray frames = env->NewObjectArray(frameCount, bufferClass, nullptr);
  env->DeleteLocalRef(bufferClass);
  if (frames == nullptr) {
    LOGE("decodeAllFramesWithSize: Failed to create frames array");
    WebPAnimDecoderDelete(dec);
    env->ReleaseByteArrayElements(data, dataPtr, JNI_ABORT);
    return nullptr;
  }

  jintArray durations = env->NewIntArray(frameCount);
  if (durations == nullptr) {
    LOGE("decodeAllFramesWithSize: Failed to create durations array");
    env->DeleteLocalRef(frames);
    WebPAnimDecoderDelete(dec);
    env->ReleaseByteArrayElements(data, dataPtr, JNI_ABORT);
    return nullptr;
  }

  jint* durPtr = env->GetIntArrayElements(durations, nullptr);
  if (durPtr == nullptr) {
    LOGE("decodeAllFramesWithSize: Failed to get durations array elements");
    env->DeleteLocalRef(frames);
    env->DeleteLocalRef(durations);
    WebPAnimDecoderDelete(dec);
    env->ReleaseByteArrayElements(data, dataPtr, JNI_ABORT);
    return nullptr;
  }

  std::vector<void*> allocated_buffers;
  uint8_t* buf = nullptr;
  int timestamp = 0;
  int prevTimestamp = 0;
  bool decodingFailed = false;

  for (int i = 0; i < frameCount; i++) {
    if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) {
      LOGE("decodeAllFramesWithSize: WebPAnimDecoderGetNext failed at frame %d", i);
      decodingFailed = true;
      break;
    }

    durPtr[i] = timestamp - prevTimestamp;
    prevTimestamp = timestamp;

    const size_t scaledSize = static_cast<size_t>(finalWidth) * static_cast<size_t>(finalHeight) * 4;
    uint8_t* scaledBuf = static_cast<uint8_t*>(AllocateOwnedBuffer(scaledSize));
    if (scaledBuf == nullptr) {
      LOGE("decodeAllFramesWithSize: allocation failed for frame %d", i);
      env->SetObjectArrayElement(frames, i, nullptr);
      continue;
    }

    allocated_buffers.push_back(scaledBuf);

    ScaleRgbaBilinear(buf, origWidth, origHeight, scaledBuf, finalWidth, finalHeight);

    jobject frameBuf = env->NewDirectByteBuffer(scaledBuf, static_cast<jlong>(scaledSize));
    if (frameBuf != nullptr) {
      env->SetObjectArrayElement(frames, i, frameBuf);
      env->DeleteLocalRef(frameBuf);
    } else {
      LOGE("decodeAllFramesWithSize: Failed to create DirectByteBuffer for frame %d", i);
      FreeOwnedBuffer(scaledBuf);
      allocated_buffers.pop_back();
      env->SetObjectArrayElement(frames, i, nullptr);
    }
  }

  env->ReleaseIntArrayElements(durations, durPtr, decodingFailed ? JNI_ABORT : 0);
  WebPAnimDecoderDelete(dec);
  env->ReleaseByteArrayElements(data, dataPtr, JNI_ABORT);

  if (decodingFailed) {
    LOGE("decodeAllFramesWithSize: Decoding failed, cleaning up allocated memory");
    for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
    env->DeleteLocalRef(frames);
    env->DeleteLocalRef(durations);
    return nullptr;
  }

  jclass resultClass = env->FindClass("io/webpkit/player/WebPAnimResult");
  if (resultClass == nullptr) {
    LOGE("decodeAllFramesWithSize: Failed to find WebPAnimResult class");
    for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
    env->DeleteLocalRef(frames);
    env->DeleteLocalRef(durations);
    return nullptr;
  }

  jmethodID constructor = env->GetMethodID(resultClass, "<init>", "([Ljava/nio/ByteBuffer;II[I)V");
  if (constructor == nullptr) {
    LOGE("decodeAllFramesWithSize: Failed to find WebPAnimResult constructor");
    for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
    env->DeleteLocalRef(resultClass);
    env->DeleteLocalRef(frames);
    env->DeleteLocalRef(durations);
    return nullptr;
  }

  jobject result = env->NewObject(resultClass, constructor, frames, finalWidth, finalHeight, durations);
  if (result == nullptr) {
    LOGE("decodeAllFramesWithSize: Failed to create WebPAnimResult object");
    for (void* ptr : allocated_buffers) FreeOwnedBuffer(ptr);
  }

  env->DeleteLocalRef(resultClass);
  env->DeleteLocalRef(frames);
  env->DeleteLocalRef(durations);
  return result;
}

#ifdef __cplusplus
}  // extern "C"
#endif
