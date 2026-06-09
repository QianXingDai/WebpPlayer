package io.webpkit.player

import android.util.Log

/**
 * Lightweight logging facade for the WebP player. Wraps [android.util.Log] so the
 * library carries no dependency on any host-app logger. Disable globally via
 * [enabled] (e.g. in release builds).
 */
object WebpLog {

    /** Master switch. Set to false to silence all library logging. */
    @JvmStatic
    @Volatile
    var enabled: Boolean = true

    @JvmStatic
    fun v(tag: String?, msg: String?) {
        if (enabled) Log.v(tag.orEmpty(), msg.orEmpty())
    }

    @JvmStatic
    fun i(tag: String?, msg: String?) {
        if (enabled) Log.i(tag.orEmpty(), msg.orEmpty())
    }

    @JvmStatic
    fun d(tag: String?, msg: String?) {
        if (enabled) Log.d(tag.orEmpty(), msg.orEmpty())
    }

    @JvmStatic
    fun w(tag: String?, msg: String?) {
        if (enabled) Log.w(tag.orEmpty(), msg.orEmpty())
    }

    @JvmStatic
    fun w(tag: String?, msg: String?, throwable: Throwable?) {
        if (enabled) Log.w(tag.orEmpty(), msg.orEmpty(), throwable)
    }

    @JvmStatic
    fun e(tag: String?, msg: String?) {
        if (enabled) Log.e(tag.orEmpty(), msg.orEmpty())
    }

    @JvmStatic
    fun e(tag: String?, msg: String?, throwable: Throwable?) {
        if (enabled) Log.e(tag.orEmpty(), msg.orEmpty(), throwable)
    }
}
