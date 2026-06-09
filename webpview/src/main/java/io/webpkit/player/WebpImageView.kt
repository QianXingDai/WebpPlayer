package io.webpkit.player

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.ImageDecoder
import android.graphics.drawable.AnimatedImageDrawable
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.AttributeSet
import android.util.Size
import android.view.Gravity
import androidx.annotation.RawRes

open class WebpImageView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : android.widget.ImageView(context, attrs) {

    companion object {
        private const val TAG = "WebpImageView"
        // 全局监测变量
        private var totalInstances = 0
        private var activeInstances = 0
        private var totalInvalidateCount = 0L
        private var totalDrawCount = 0L
        private var totalDrawTime = 0L
        private var maxDrawTime = 0L
        private var lastInvalidateTime = 0L
        private val monitorHandler = Handler(Looper.getMainLooper())

        // 定期打印 WebpImageView 状态
        private val monitorRunnable = object : Runnable {
            override fun run() {
                if (activeInstances > 0) {
                    val avgDrawTime = if (totalDrawCount > 0) totalDrawTime / totalDrawCount else 0
                    WebpLog.i(TAG, "📊 [WebpMonitor] Active: $activeInstances/$totalInstances, invalidates: $totalInvalidateCount, draws: $totalDrawCount, avgDraw: ${avgDrawTime}ms, maxDraw: ${maxDrawTime}ms")
                }
                monitorHandler.postDelayed(this, 30_000)
            }
        }

        // Shared background decoder so off-main-thread decodes don't each spin up a thread.
        private val decodeExecutor = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
            Thread(r, "WebpImageView-decode").apply { isDaemon = true }
        }

        init {
            monitorHandler.postDelayed(monitorRunnable, 30_000)
        }
    }

    private var instanceId = 0
    private var invalidateCount = 0L
    private var drawCount = 0L
    private var instanceDrawTime = 0L
    private var instanceMaxDrawTime = 0L
    private var startTime = 0L
    private var isAnimating = false
    private var resId: Int? = null

    init {
        instanceId = ++totalInstances
        WebpLog.d(TAG, "[WebpImageView#$instanceId] Created (total: $totalInstances)")
    }

    fun setWebpFromRaw(
        @RawRes resId: Int,
        targetSize: Size? = null,
        fps: Int = 20,
        overlay: Boolean = false,
        isCacheAll: Boolean = false,
    ) {
        if (resId == this.resId) {
            return
        }
        this.resId = resId
        val source = ImageDecoder.createSource(context.resources, resId)
        val drawable = ImageDecoder.decodeDrawable(source)
        WebpLog.d(TAG, "[WebpImageView#$instanceId] setWebpFromRaw resId=$resId, drawable=${drawable::class.simpleName}")
        setImageDrawable(drawable)
    }

    /**
     * Like [setWebpFromRaw] but decodes off the main thread to avoid jank when the
     * WebP is large. [onReady] is invoked on the main thread once the drawable has
     * been set (or after a failed/superseded decode, with success=false). The view
     * does not auto-start — call [start] from [onReady] if you want playback.
     */
    fun setWebpFromRawAsync(
        @RawRes resId: Int,
        onReady: ((success: Boolean) -> Unit)? = null,
    ) {
        if (resId == this.resId) {
            onReady?.invoke(true)
            return
        }
        this.resId = resId
        val appResources = context.applicationContext.resources
        decodeExecutor.execute {
            val drawable = runCatching {
                ImageDecoder.decodeDrawable(ImageDecoder.createSource(appResources, resId))
            }.onFailure {
                WebpLog.e(TAG, "[WebpImageView#$instanceId] async decode failed resId=$resId: ${it.message}")
            }.getOrNull()
            post {
                // A newer setWebp* call may have superseded this one while decoding.
                if (this.resId != resId) {
                    onReady?.invoke(false)
                    return@post
                }
                if (drawable == null) {
                    this.resId = null // allow a later retry
                    onReady?.invoke(false)
                    return@post
                }
                setImageDrawable(drawable)
                onReady?.invoke(true)
            }
        }
    }

    fun setForegroundBitmap(
        bitmap: Bitmap?,
        gravity: Int = Gravity.CENTER,
        scale: Float = 1.0f,
        translateX: Float = 0f,
        translateY: Float = 0f
    ) {
        if (bitmap == null || bitmap.isRecycled) return
        foreground = BitmapForegroundDrawable(bitmap, gravity, scale, translateX, translateY)
    }


    fun start() {
        (drawable as? AnimatedImageDrawable)?.let { anim ->
            if (!isAnimating) {
                isAnimating = true
                activeInstances++
                startTime = SystemClock.uptimeMillis()
                invalidateCount = 0
                drawCount = 0
                instanceDrawTime = 0
                instanceMaxDrawTime = 0
                WebpLog.d(TAG, "[WebpImageView#$instanceId] Animation started (active: $activeInstances)")
            }
            anim.start()
        }
    }

    fun stop() {
        (drawable as? AnimatedImageDrawable)?.let { anim ->
            if (isAnimating) {
                isAnimating = false
                activeInstances--
                val duration = SystemClock.uptimeMillis() - startTime
                val avgDrawTime = if (drawCount > 0) instanceDrawTime / drawCount else 0
                WebpLog.d(TAG, "[WebpImageView#$instanceId] Animation stopped after ${duration}ms:")
                WebpLog.d(TAG, "   invalidates: $invalidateCount, draws: $drawCount")
                WebpLog.d(TAG, "   avgDraw: ${avgDrawTime}ms, maxDraw: ${instanceMaxDrawTime}ms")
                WebpLog.d(TAG, "   (active: $activeInstances)")
            }
            anim.stop()
        }
    }

    override fun onDraw(canvas: Canvas) {
        if (isAnimating) {
            val drawStart = SystemClock.uptimeMillis()
            super.onDraw(canvas)
            val drawTime = SystemClock.uptimeMillis() - drawStart

            drawCount++
            totalDrawCount++
            instanceDrawTime += drawTime
            totalDrawTime += drawTime

            if (drawTime > instanceMaxDrawTime) {
                instanceMaxDrawTime = drawTime
            }
            if (drawTime > maxDrawTime) {
                maxDrawTime = drawTime
            }

            // 如果单次绘制超过 16ms，说明有问题
            if (drawTime > 16) {
                WebpLog.w(TAG, "[WebpImageView#$instanceId] ⚠️ Slow onDraw: ${drawTime}ms")
            }
        } else {
            super.onDraw(canvas)
        }
    }

    override fun invalidate() {
        super.invalidate()
        if (isAnimating) {
            invalidateCount++
            totalInvalidateCount++
            val now = SystemClock.uptimeMillis()
            // 如果两次 invalidate 间隔超过 100ms，可能有问题
            if (lastInvalidateTime > 0 && now - lastInvalidateTime > 100) {
                WebpLog.w(TAG, "[WebpImageView#$instanceId] ⚠️ Long gap between invalidates: ${now - lastInvalidateTime}ms")
            }
            lastInvalidateTime = now
        }
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        if (isAnimating) {
            isAnimating = false
            activeInstances--
            WebpLog.d(TAG, "[WebpImageView#$instanceId] Detached while animating (active: $activeInstances)")
        }
        WebpLog.d(TAG, "[WebpImageView#$instanceId] onDetachedFromWindow")
    }
}