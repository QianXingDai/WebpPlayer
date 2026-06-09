package io.webpkit.player

import android.content.Context
import android.util.Size
import java.nio.ByteBuffer

object WebPYUVDecoder {
    init {
        System.loadLibrary("webpkit")
    }

    // JNI: 原有方法，解码为原始尺寸
    external fun decodeAllFrames(data: ByteArray): WebPAnimResult?

    // JNI: 新增方法，解码为指定尺寸（targetWidth/targetHeight > 0 时缩放）
    external fun decodeAllFramesWithSize(data: ByteArray, targetWidth: Int, targetHeight: Int): WebPAnimResult?

    // JNI: releaseNativeBuffers(frames: Array<ByteBuffer?>) -> Int (返回释放的KB数)
    // frees malloc'ed pointers backing NewDirectByteBuffer and returns freed memory in KB
    external fun releaseNativeBuffers(frames: Array<ByteBuffer>?): Int

    // JNI: cloneNativeBuffers(frames) -> deep-copied native-backed direct ByteBuffers
    external fun cloneNativeBuffers(frames: Array<ByteBuffer>?): Array<ByteBuffer>?


    fun decodeRawWebPToAnim(context: Context, resId: Int, targetSize: Size? = null): WebPAnimResult? {
        val inputStream = context.resources.openRawResource(resId)
        val bytes = inputStream.readBytes()
        inputStream.close()

        val result = try {
            if (targetSize != null && targetSize.width > 0 && targetSize.height > 0) {
                // 使用指定尺寸解码
                decodeAllFramesWithSize(bytes, targetSize.width, targetSize.height)?.also {
                    // 将 targetSize 记录到结果中
                    return WebPAnimResult(it.frames, it.canvasWidth, it.canvasHeight, it.durations)
                }
            } else {
                // 使用原始尺寸解码
                decodeAllFrames(bytes)
            }
        } catch (t: Throwable) {
            WebpLog.e("WebPYUVDecoder", "decode failed: ${t.message}")
            null
        }

        if (result == null) {
            WebpLog.e("WebPYUVDecoder", "decode returned null")
            return null
        }
        WebpLog.d("WebPYUVDecoder", "Decoded anim: ${result.frames?.size} frames, canvas=${result.canvasWidth}x${result.canvasHeight}, targetSize=$targetSize")
        return result
    }
}
