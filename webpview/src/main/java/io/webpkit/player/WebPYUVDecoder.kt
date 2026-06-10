package io.webpkit.player

import android.content.Context
import android.util.Size
import java.nio.ByteBuffer

object WebPYUVDecoder {

    private const val TAG = "WebPYUVDecoder"

    /**
     * 原生库是否加载成功。当以 aar 形式集成却没有把 libwebpkit.so 打进包里时，
     * 这里会是 false —— 这是「看不到 webp」最常见的原因，务必先看这条日志。
     */
    @JvmStatic
    @Volatile
    var nativeLoaded: Boolean = false
        private set

    init {
        nativeLoaded = try {
            System.loadLibrary("webpkit")
            WebpLog.i(TAG, "System.loadLibrary(\"webpkit\") 成功，原生解码可用")
            true
        } catch (t: Throwable) {
            WebpLog.e(
                TAG,
                "System.loadLibrary(\"webpkit\") 失败！aar 中可能缺少 libwebpkit.so（检查 abi/jniLibs 是否打包）: ${t.message}",
                t
            )
            false
        }
    }

    // JNI: 原有方法，解码为原始尺寸
    external fun decodeAllFrames(data: ByteArray): WebPAnimResult?

    // JNI: 新增方法，解码为指定尺寸（targetWidth/targetHeight > 0 时缩放）
    external fun decodeAllFramesWithSize(data: ByteArray, targetWidth: Int, targetHeight: Int): WebPAnimResult?

    // JNI: direct ByteBuffer 直传版本——native 直接读 buffer 地址，解码全程不 pin/拷贝 Java 数组。
    // targetWidth/targetHeight <= 0 时按原始尺寸解码。
    external fun decodeAllFramesDirect(
        data: ByteBuffer,
        dataSize: Int,
        targetWidth: Int,
        targetHeight: Int,
    ): WebPAnimResult?

    // JNI: releaseNativeBuffers(frames: Array<ByteBuffer?>) -> Int (返回释放的KB数)
    // frees malloc'ed pointers backing NewDirectByteBuffer and returns freed memory in KB
    external fun releaseNativeBuffers(frames: Array<ByteBuffer>?): Int

    // JNI: cloneNativeBuffers(frames) -> deep-copied native-backed direct ByteBuffers
    external fun cloneNativeBuffers(frames: Array<ByteBuffer>?): Array<ByteBuffer>?


    fun decodeRawWebPToAnim(context: Context, resId: Int, targetSize: Size? = null): WebPAnimResult? {
        if (!nativeLoaded) {
            WebpLog.e(TAG, "decodeRawWebPToAnim 跳过：原生库未加载，无法解码 resId=$resId")
            return null
        }

        val data = try {
            context.resources.openRawResource(resId).use { input ->
                val bytes = input.readBytes()
                if (bytes.isEmpty()) {
                    WebpLog.e(TAG, "decodeRawWebPToAnim: raw 资源为空 resId=$resId")
                    return null
                }
                // 拷进 direct buffer，native 侧零拷贝读取，解码全程不与 GC 交互
                ByteBuffer.allocateDirect(bytes.size).apply {
                    put(bytes)
                    position(0)
                }
            }
        } catch (t: Throwable) {
            WebpLog.e(TAG, "读取 raw 资源失败 resId=$resId: ${t.message}", t)
            return null
        }
        WebpLog.d(TAG, "decodeRawWebPToAnim: resId=$resId, bytes=${data.capacity()}, targetSize=$targetSize")

        val result = try {
            decodeAllFramesDirect(
                data,
                data.capacity(),
                targetSize?.width ?: 0,
                targetSize?.height ?: 0,
            )
        } catch (t: Throwable) {
            WebpLog.e(TAG, "decode failed: ${t.message}", t)
            null
        }

        if (result == null) {
            WebpLog.e(TAG, "decode returned null (resId=$resId)")
            return null
        }
        val frameCount = result.frames?.size ?: 0
        if (frameCount == 0) {
            WebpLog.w(TAG, "decode 成功但 0 帧 (resId=$resId, canvas=${result.canvasWidth}x${result.canvasHeight}) —— 将不会显示任何内容")
        }
        WebpLog.d(TAG, "Decoded anim: $frameCount frames, canvas=${result.canvasWidth}x${result.canvasHeight}, targetSize=$targetSize")
        return result
    }
}
