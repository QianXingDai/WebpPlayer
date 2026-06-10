package io.webpkit.player

import java.nio.ByteBuffer

/**
 * 容器：存放 JNI 返回的所有帧（每帧为 direct ByteBuffer，顺序为 RGBA）
 * canvasWidth/canvasHeight: 是用于所有帧的统一尺寸（已保证为偶数）
 * durations: 每帧持续时间（ms）
 * loopCount: WebP 文件声明的循环次数；0 = 无限循环，N = 播放 N 次后停止
 */
class WebPAnimResult(
    val frames: Array<ByteBuffer>?,
    val canvasWidth: Int,
    val canvasHeight: Int,
    val durations: IntArray,
    val loopCount: Int = 0,
)
