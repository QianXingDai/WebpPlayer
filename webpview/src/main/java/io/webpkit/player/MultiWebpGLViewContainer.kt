package io.webpkit.player

import android.content.Context
import android.util.AttributeSet
import android.view.ViewTreeObserver
import android.widget.FrameLayout
import android.widget.ImageView
import kotlin.math.roundToInt

/**
 * Wraps a [MultiWebpGLView] and lets a **single** layer be lifted off the GL
 * surface so another view can be drawn above it.
 *
 * Why: [MultiWebpGLView] uses `setZOrderOnTop(true)`, so no sibling view can ever
 * be drawn above it. When only one of several animations must be covered (e.g. 3
 * layers, only 1 needs an overlay on top of it), call [freezeLayer]: that single
 * layer is dropped from the GL surface and replaced, pixel-for-pixel, by a
 * same-size [WebpImageView] playing the same WebP. Since that ImageView lives in
 * the normal view hierarchy (the GL surface stays transparent where the layer used
 * to be), a covering view can now be drawn above it — while the other layers keep
 * animating on the GL surface, completely unaffected.
 *
 * Usage:
 * ```kotlin
 * container.glView.setLayers(...)
 * container.glView.start()
 *
 * container.freezeLayer(1)    // layer #1 → ImageView overlay, layers #0/#2 keep playing
 * container.unfreezeLayer(1)  // hand layer #1 back to the GL surface
 * ```
 */
class MultiWebpGLViewContainer @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : FrameLayout(context, attrs) {

    private val TAG = "MultiWebpGLViewContainer"

    /** The wrapped GL view. Use it directly for setLayers / layer visibility etc. */
    val glView: MultiWebpGLView = MultiWebpGLView(context)

    // Per-layer freeze overlays: GL layer index -> the WebpImageView that took its
    // place. Each overlay plays the same WebP and lives in the normal view
    // hierarchy, so covering views can be drawn above it (the GL surface is
    // ZOrderOnTop and therefore can never be covered directly).
    private val frozenLayers = mutableMapOf<Int, WebpImageView>()
    private val layerUnfreezeFallbacks = mutableMapOf<Int, Runnable>()

    // Pending delayed freeze/unfreeze runnables, so a detach can cancel them.
    private val pendingDelayedOps = mutableListOf<Runnable>()

    init {
        addView(glView, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
    }

    /**
     * Freeze a **single** layer (by its index in [MultiWebpGLView.setLayers] order)
     * without touching the others.
     *
     * The layer is removed from the GL surface and replaced, pixel-for-pixel, by a
     * [WebpImageView] playing the same WebP at the same position/size. Because that
     * ImageView lives in the normal view hierarchy (the GL surface stays transparent
     * where the layer used to be), a covering view can now be drawn above it. All
     * other layers keep playing on the GL surface, completely unaffected.
     *
     * The replacement keeps animating (auto-play), so visually nothing stops.
     * Safe to call repeatedly; a no-op if this layer is already frozen.
     *
     * @param delayMs  if > 0, wait this many milliseconds before performing the
     *                 freeze. The wait is cancelled if the view detaches first.
     * @param onFrozen invoked (main thread) once the GL layer has actually been
     *                 dropped and the overlay is in place — a good moment to show
     *                 the covering view for a clean hand-off.
     */
    fun freezeLayer(index: Int, delayMs: Long = 0, onFrozen: (() -> Unit)? = null) {
        runDelayed(delayMs) { freezeLayerInternal(index, onFrozen) }
    }

    private fun freezeLayerInternal(index: Int, onFrozen: (() -> Unit)?) {
        if (frozenLayers.containsKey(index)) {
            onFrozen?.invoke()
            return
        }
        val cfg = glView.getLayerConfig(index)
        val rect = glView.getLayerDisplayRect(index)
        if (cfg == null || rect == null) {
            WebpLog.w(TAG, "freezeLayer($index): layer not ready (config/size unknown), ignored")
            onFrozen?.invoke()
            return
        }

        val overlay = WebpImageView(context).apply {
            scaleType = ImageView.ScaleType.FIT_XY
        }
        val lp = LayoutParams(
            rect.width().roundToInt().coerceAtLeast(1),
            rect.height().roundToInt().coerceAtLeast(1),
        ).apply {
            leftMargin = rect.left.roundToInt()
            topMargin = rect.top.roundToInt()
        }
        // Added below the GL surface (which is ZOrderOnTop). The surface clears the
        // layer's region to transparent once we hide it, revealing this overlay.
        addView(overlay, lp)
        frozenLayers[index] = overlay

        // Decode off the main thread, then start playback. The GL layer keeps playing
        // until the overlay is decoded AND has drawn its first frame — so there is no
        // main-thread jank and no transparent gap during the hand-off.
        overlay.setWebpFromRawAsync(cfg.resId) { success ->
            if (frozenLayers[index] !== overlay) return@setWebpFromRawAsync // superseded
            if (!success) {
                // Decode failed: fall back to hiding the GL layer directly so we don't
                // strand a live layer under a covering view.
                WebpLog.w(TAG, "freezeLayer($index): overlay decode failed, hiding GL layer directly")
                glView.hideLayer(index)
                onFrozen?.invoke()
                return@setWebpFromRawAsync
            }
            overlay.start()
            overlay.viewTreeObserver.addOnPreDrawListener(object : ViewTreeObserver.OnPreDrawListener {
                override fun onPreDraw(): Boolean {
                    overlay.viewTreeObserver.removeOnPreDrawListener(this)
                    post {
                        // Re-check: a freeze→unfreeze→freeze race may have replaced it.
                        if (frozenLayers[index] === overlay) {
                            glView.hideLayer(index)
                            onFrozen?.invoke()
                        }
                    }
                    return true
                }
            })
        }
    }

    /**
     * Reverse [freezeLayer]: bring the layer back onto the GL surface and remove its
     * [WebpImageView] overlay. The overlay is kept up until the GL surface has drawn
     * a real frame containing the layer again, so the swap is seamless. No-op if the
     * layer isn't frozen.
     *
     * @param delayMs if > 0, wait this many milliseconds before performing the
     *                unfreeze. The wait is cancelled if the view detaches first.
     */
    fun unfreezeLayer(index: Int, delayMs: Long = 0) {
        runDelayed(delayMs) { unfreezeLayerInternal(index) }
    }

    private fun unfreezeLayerInternal(index: Int) {
        val overlay = frozenLayers.remove(index) ?: return

        val removeOverlay = {
            overlay.stop()
            removeView(overlay)
            layerUnfreezeFallbacks.remove(index)?.let { removeCallbacks(it) }
        }

        // Show the GL layer first; tear down the overlay only after a completed draw.
        glView.doOnNextFrameDrawn {
            // Extra hop so the buffer swap has landed before we drop the overlay.
            post { removeOverlay() }
        }
        glView.showLayer(index)

        // Fallback: never strand an overlay if the surface fails to redraw.
        val fallback = Runnable { removeOverlay() }
        layerUnfreezeFallbacks[index] = fallback
        postDelayed(fallback, 800)
    }

    /** Run [action] now if [delayMs] <= 0, else after [delayMs] (cancellable on detach). */
    private fun runDelayed(delayMs: Long, action: () -> Unit) {
        if (delayMs <= 0) {
            action()
            return
        }
        var r: Runnable? = null
        r = Runnable {
            pendingDelayedOps.remove(r)
            action()
        }
        pendingDelayedOps.add(r)
        postDelayed(r, delayMs)
    }

    private fun clearLayerOverlays() {
        pendingDelayedOps.forEach { removeCallbacks(it) }
        pendingDelayedOps.clear()
        layerUnfreezeFallbacks.values.forEach { removeCallbacks(it) }
        layerUnfreezeFallbacks.clear()
        frozenLayers.values.forEach { overlay ->
            overlay.stop()
            removeView(overlay)
        }
        frozenLayers.clear()
    }

    override fun onDetachedFromWindow() {
        clearLayerOverlays()
        super.onDetachedFromWindow()
    }
}
