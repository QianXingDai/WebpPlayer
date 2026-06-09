package io.webpkit.player

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

/**
 * Internal coroutine scope for background decode/refill work. Self-contained so the
 * library does not depend on any host-app threading utilities.
 */
internal object WebpExecutors {
    private val job = SupervisorJob()

    /** Long-lived scope for background decoding tasks. */
    val globalAsyncScope: CoroutineScope = CoroutineScope(job + Dispatchers.Default)
}
