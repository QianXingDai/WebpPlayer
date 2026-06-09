package io.webpkit.player

import android.content.Context

/**
 * Holds the application [Context] the decoder needs to open raw resources.
 *
 * It is populated automatically at app startup by [WebpInitProvider], so library
 * consumers do not have to call anything. If you strip manifest providers (or run
 * in an environment without one), call [init] yourself once, e.g. from your
 * Application.onCreate().
 */
object WebpContext {

    @Volatile
    private var appContext: Context? = null

    /** Manually provide a context. Safe to call multiple times. */
    @JvmStatic
    fun init(context: Context) {
        if (appContext == null) {
            appContext = context.applicationContext
        }
    }

    /** The application context. Throws if the library was never initialized. */
    @JvmStatic
    fun get(): Context = appContext
        ?: error(
            "WebpContext not initialized. This normally happens automatically via " +
                "WebpInitProvider; if you removed it, call WebpContext.init(context) once."
        )
}
