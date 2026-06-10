package io.webpkit.player

import android.hardware.HardwareBuffer
import java.nio.ByteBuffer

/**
 * 容器：存放 JNI 返回的所有帧。两种存储模式，二选一：
 * - frames: 每帧为 malloc 的 direct ByteBuffer（RGBA），渲染时逐帧 glTexSubImage2D 上传
 * - hardwareFrames: 每帧为 AHardwareBuffer（CPU/GPU 共享内存），渲染时经 EGLImage
 *   直接采样，零拷贝零上传
 *
 * canvasWidth/canvasHeight: 是用于所有帧的统一尺寸（已保证为偶数）
 * durations: 每帧持续时间（ms）
 * loopCount: WebP 文件声明的循环次数；0 = 无限循环，N = 播放 N 次后停止
 */
class WebPAnimResult(
    val frames: Array<ByteBuffer>?,
    val hardwareFrames: Array<HardwareBuffer>? = null,
    val canvasWidth: Int,
    val canvasHeight: Int,
    val durations: IntArray,
    val loopCount: Int = 0,
) {
    constructor(
        frames: Array<ByteBuffer>?,
        canvasWidth: Int,
        canvasHeight: Int,
        durations: IntArray,
        loopCount: Int = 0,
    ) : this(frames, null, canvasWidth, canvasHeight, durations, loopCount)

    val frameCount: Int
        get() = hardwareFrames?.size ?: frames?.size ?: 0
}

/**
 * 统一释放入口：ByteBuffer 帧走 native 引用计数释放，HardwareBuffer 帧 close()
 * （AHardwareBuffer 自带引用计数，最后一个引用关闭时才真正释放内存）。
 * 幂等含义由底层保证：重复对同一数组调用是安全的（已释放的指针会被注册表跳过，
 * 已 close 的 HardwareBuffer 会被 isClosed 跳过）。
 */
internal fun WebPAnimResult?.releaseNative() {
    if (this == null) return
    frames?.let {
        try {
            WebPYUVDecoder.releaseNativeBuffers(it)
        } catch (_: Throwable) {
        }
    }
    hardwareFrames?.forEach { hb ->
        try {
            if (!hb.isClosed) hb.close()
        } catch (_: Throwable) {
        }
    }
}
