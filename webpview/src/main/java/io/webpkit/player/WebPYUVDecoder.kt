package io.webpkit.player

import android.content.Context
import android.hardware.HardwareBuffer
import android.util.Size
import java.nio.ByteBuffer

object WebPYUVDecoder {

    private const val TAG = "WebPYUVDecoder"

    /**
     * 零拷贝路径总开关：帧解码直写 AHardwareBuffer，渲染经 EGLImage 直接采样，
     * 播放期间没有任何逐帧 CPU 拷贝。遇到兼容性问题可在运行时关掉，
     * 自动回退到 ByteBuffer + glTexSubImage2D 的软路径。
     */
    @JvmStatic
    @Volatile
    var hardwareBuffersEnabled: Boolean = true

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

    // JNI: 同上，preferHardware=true 时帧直写 AHardwareBuffer（结果在 hardwareFrames），
    // 硬件分配失败自动回退软路径；maxPixels>0 时解码输出面积超限会强制等比降采样
    external fun decodeAllFramesDirectEx(
        data: ByteBuffer,
        dataSize: Int,
        targetWidth: Int,
        targetHeight: Int,
        maxPixels: Int,
        preferHardware: Boolean,
    ): WebPAnimResult?

    // JNI: 只解析容器头（不解码像素，微秒级），返回
    // [canvasWidth, canvasHeight, frameCount, loopCount, hasAlpha(0/1)]
    external fun getAnimInfo(data: ByteBuffer, dataSize: Int): IntArray?

    // JNI: HardwareBuffer 帧的 clone——每帧生成新的 Java 包装、各自持有底层
    // AHardwareBuffer 的一个引用（零拷贝），释放用 HardwareBuffer.close()
    external fun cloneHardwareBuffers(frames: Array<HardwareBuffer>?): Array<HardwareBuffer>?

    // JNI: EGLImage 桥（必须在 GL 线程调用）
    external fun eglImageCreate(buffer: HardwareBuffer): Long
    external fun eglImageTargetTexture2D(image: Long)
    external fun eglImageDestroy(image: Long)

    // JNI: EGLImage 不可用时的兜底——锁定 AHB 按行上传到当前绑定纹理
    external fun uploadHardwareBufferToTexture(buffer: HardwareBuffer, width: Int, height: Int): Boolean

    // JNI: releaseNativeBuffers(frames: Array<ByteBuffer?>) -> Int (返回释放的KB数)
    // frees malloc'ed pointers backing NewDirectByteBuffer and returns freed memory in KB
    external fun releaseNativeBuffers(frames: Array<ByteBuffer>?): Int

    // JNI: cloneNativeBuffers(frames) -> deep-copied native-backed direct ByteBuffers
    external fun cloneNativeBuffers(frames: Array<ByteBuffer>?): Array<ByteBuffer>?


    /** 容器头信息（不解码像素即可获得），用于解码前的成本预估。 */
    class WebpAnimInfo(
        val width: Int,
        val height: Int,
        val frameCount: Int,
        val loopCount: Int,
        val hasAlpha: Boolean,
    )

    /**
     * 只读容器头，返回画布尺寸/帧数等元信息。文件读取是主要成本（像素不解码），
     * 适合预加载前估算「解码后会占多少内存」。失败返回 null。
     */
    fun peekAnimInfo(context: Context, resId: Int): WebpAnimInfo? {
        if (!nativeLoaded) return null
        return try {
            val data = context.resources.openRawResource(resId).use { input ->
                val bytes = input.readBytes()
                if (bytes.isEmpty()) return null
                ByteBuffer.allocateDirect(bytes.size).apply { put(bytes); position(0) }
            }
            val v = getAnimInfo(data, data.capacity()) ?: return null
            WebpAnimInfo(v[0], v[1], v[2], v[3], v[4] != 0)
        } catch (t: Throwable) {
            WebpLog.w(TAG, "peekAnimInfo failed resId=$resId: ${t.message}")
            null
        }
    }

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

        val profile = WebpDeviceProfile.current()
        val result = try {
            decodeAllFramesDirectEx(
                data,
                data.capacity(),
                targetSize?.width ?: 0,
                targetSize?.height ?: 0,
                profile.maxDecodePixels,
                hardwareBuffersEnabled && profile.hardwareBuffers,
            )
        } catch (t: Throwable) {
            WebpLog.e(TAG, "decode failed: ${t.message}", t)
            null
        }

        if (result == null) {
            WebpLog.e(TAG, "decode returned null (resId=$resId)")
            return null
        }
        val frameCount = result.frameCount
        if (frameCount == 0) {
            WebpLog.w(TAG, "decode 成功但 0 帧 (resId=$resId, canvas=${result.canvasWidth}x${result.canvasHeight}) —— 将不会显示任何内容")
        }
        WebpLog.d(TAG, "Decoded anim: $frameCount frames (hw=${result.hardwareFrames != null}), canvas=${result.canvasWidth}x${result.canvasHeight}, targetSize=$targetSize")
        return result
    }
}
