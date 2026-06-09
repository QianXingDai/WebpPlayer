package io.webpkit.player

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.Rect
import android.graphics.drawable.Drawable
import android.view.Gravity

class BitmapForegroundDrawable(
    private val bitmap: Bitmap,
    private val gravity: Int = Gravity.CENTER,
    private val scale: Float = 1.0f,
    private val translateX: Float = 0f,
    private val translateY: Float = 0f
) : Drawable() {

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

    override fun draw(canvas: Canvas) {
        val viewWidth = bounds.width()
        val viewHeight = bounds.height()

        val scaledWidth = (bitmap.width * scale).toInt()
        val scaledHeight = (bitmap.height * scale).toInt()

        val outRect = Rect()

        // 使用系统的 Gravity 工具方法，自动算出 bitmap 在父容器中的位置
        Gravity.apply(
            gravity,                   // 传入组合的 gravity
            scaledWidth, scaledHeight, // bitmap 的尺寸
            Rect(0, 0, viewWidth, viewHeight), // 父容器区域
            outRect                    // 输出结果
        )

        // 偏移
        outRect.offset(translateX.toInt(), translateY.toInt())

        canvas.drawBitmap(
            bitmap,
            null,
            outRect,
            paint
        )
    }


    override fun setAlpha(alpha: Int) {
        paint.alpha = alpha
    }

    override fun setColorFilter(colorFilter: ColorFilter?) {
        paint.colorFilter = colorFilter
    }

    override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
}