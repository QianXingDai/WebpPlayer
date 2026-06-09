package io.webpkit.player

import android.util.Size
import androidx.annotation.RawRes
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import java.nio.ByteBuffer
import java.util.LinkedHashMap
import java.util.concurrent.ConcurrentHashMap

/**
 * 缓存保存一份 native-backed 原件，调用方拿到的是 clone，可安全释放。
 */
object WebPAnimResultManager {

    private data class CacheKey(val resId: Int, val size: Size?)

    private val cacheLock = Any()
    private val webPAnimResultCache = LinkedHashMap<CacheKey, WebPAnimResult>(8, 0.75f, true)
    private val preloadWebpResIds = mutableSetOf<CacheKey>()
    private val pendingRefillTasks = ConcurrentHashMap<CacheKey, Deferred<WebPAnimResult?>>()
    private val removedFlags = ConcurrentHashMap.newKeySet<CacheKey>()

    private var decodeDelegate: (Int, Size?) -> WebPAnimResult? = { resId, size ->
        decodeWebpAnimResultInternal(resId, size)
    }
    private var releaseFramesDelegate: (Array<ByteBuffer>?) -> Unit = { frames ->
        if (frames != null) {
            WebPYUVDecoder.releaseNativeBuffers(frames)
        }
    }
    private var cloneDelegate: (WebPAnimResult) -> WebPAnimResult? = { anim ->
        cloneAnimResultInternal(anim)
    }
    private var refillAsyncEnabled: Boolean = true
    private var cacheBudgetBytesOverride: Long? = null
    private var maxCacheEntriesOverride: Int? = null

    fun removeWebPAnimResult(resId: Int, size: Size? = null): WebPAnimResult? {
        val key = CacheKey(resId, size)
        logD("removeWebPAnimResult: $key")
        val removed = synchronized(cacheLock) { webPAnimResultCache.remove(key) }
        removedFlags.add(key)
        pendingRefillTasks.remove(key)?.let { deferred ->
            try {
                deferred.cancel()
            } catch (t: Throwable) {
                logE("cancel pending refill failed: ${t.message}")
            }
        }
        preloadWebpResIds.remove(key)
        return removed
    }

    fun getWebPAnimResult(resId: Int, size: Size? = null): WebPAnimResult? {
        val key = CacheKey(resId, size)

        synchronized(cacheLock) {
            webPAnimResultCache[key]?.let { cached ->
                removedFlags.remove(key)
                logD("getWebPAnimResult: cloned from cache: $key")
                cloneAnimResult(cached)?.let { return it }
                logW("getWebPAnimResult: clone failed, evict cache and decode again: $key")
                val removed = webPAnimResultCache.remove(key)
                if (removed != null) {
                    releaseAnimResult(removed)
                }
            }
        }

        logD("getWebPAnimResult: cache miss, decoding once for $key")
        val decoded = runCatching {
            decodeDelegate(resId, size)
        }.onFailure {
            logE("decode failed for caller: $key, ${it.message}")
        }.getOrNull() ?: return null

        synchronized(cacheLock) {
            val existing = webPAnimResultCache[key]
            if (existing != null) {
                releaseAnimResult(decoded)
                return cloneAnimResult(existing)
            }

            webPAnimResultCache[key] = decoded
            val cloned = cloneAnimResult(decoded)
            trimCacheLocked()
            return cloned
        }
    }

    private fun ensureRefillInBackground(key: CacheKey) {
        if (!refillAsyncEnabled) return

        val existing = pendingRefillTasks[key]
        if (existing != null && !existing.isCompleted) {
            logD("refill already running for $key, reuse it")
            return
        }

        val deferred = WebpExecutors.globalAsyncScope.async {
            logD("refill: start decoding for cache: $key")
            val anim = runCatching { decodeDelegate(key.resId, key.size) }.onFailure {
                logE("refill decode failed: $key, ${it.message}")
            }.getOrNull()

            if (anim != null) {
                val wasRemoved = removedFlags.remove(key)
                if (!wasRemoved) {
                    val previous = synchronized(cacheLock) {
                        val existing = webPAnimResultCache[key]
                        if (existing == null) {
                            webPAnimResultCache[key] = anim
                            trimCacheLocked()
                        }
                        existing
                    }
                    if (previous == null) {
                        logD("refill: cached result for $key")
                    } else {
                        releaseAnimResult(anim)
                    }
                } else {
                    logD("refill: skip caching because removed: $key")
                    releaseAnimResult(anim)
                }
            }
            anim
        }

        val prev = pendingRefillTasks.putIfAbsent(key, deferred)
        if (prev != null) {
            try {
                deferred.cancel()
            } catch (_: Throwable) {
            }
            logD("refill: another task exists, reused it for $key")
        } else {
            deferred.invokeOnCompletion {
                pendingRefillTasks.remove(key, deferred)
                preloadWebpResIds.remove(key)
            }
        }
    }

    fun preloadWebPAnimResults(resIds: Set<Int>, size: Size? = null) {
        val keys = resIds.map { CacheKey(it, size) }.toSet()
        preloadWebpResIds.addAll(keys)
        keys.forEach { key ->
            removedFlags.remove(key)
            if (synchronized(cacheLock) { webPAnimResultCache.containsKey(key) }) {
                preloadWebpResIds.remove(key)
                return@forEach
            }
            ensureRefillInBackground(key)
        }
    }

    fun clear() {
        logD("clear all")
        val removed = synchronized(cacheLock) {
            val values = webPAnimResultCache.values.toList()
            webPAnimResultCache.clear()
            values
        }
        removed.forEach { releaseAnimResult(it) }
        preloadWebpResIds.clear()
        removedFlags.clear()
        pendingRefillTasks.values.forEach { it.cancel() }
        pendingRefillTasks.clear()
    }

    internal fun resetForTests(
        decodeBlock: (Int, Size?) -> WebPAnimResult?,
        releaseBlock: (Array<ByteBuffer>?) -> Unit,
        cloneBlock: (WebPAnimResult) -> WebPAnimResult?,
        refillAsync: Boolean,
        maxCacheEntriesOverride: Int? = null,
        cacheBudgetBytesOverride: Long? = null,
    ) {
        clear()
        decodeDelegate = decodeBlock
        releaseFramesDelegate = releaseBlock
        cloneDelegate = cloneBlock
        refillAsyncEnabled = refillAsync
        this.maxCacheEntriesOverride = maxCacheEntriesOverride
        this.cacheBudgetBytesOverride = cacheBudgetBytesOverride
    }

    private fun cloneAnimResult(anim: WebPAnimResult): WebPAnimResult? {
        return runCatching {
            cloneDelegate(anim)
        }.onFailure {
            logE("clone anim failed: ${it.message}")
        }.getOrNull()
    }

    private fun releaseAnimResult(anim: WebPAnimResult?) {
        val frames = anim?.frames ?: return
        runCatching {
            releaseFramesDelegate(frames)
        }.onFailure {
            logE("release anim failed: ${it.message}")
        }
    }

    private fun cloneAnimResultInternal(anim: WebPAnimResult): WebPAnimResult? {
        val frames = anim.frames ?: return WebPAnimResult(
            frames = null,
            canvasWidth = anim.canvasWidth,
            canvasHeight = anim.canvasHeight,
            durations = anim.durations.copyOf(),
        )
        val clonedFrames = WebPYUVDecoder.cloneNativeBuffers(frames) ?: return null
        return WebPAnimResult(
            frames = clonedFrames,
            canvasWidth = anim.canvasWidth,
            canvasHeight = anim.canvasHeight,
            durations = anim.durations.copyOf(),
        )
    }

    private fun decodeWebpAnimResultInternal(@RawRes resId: Int, size: Size? = null): WebPAnimResult? {
        return runCatching {
            logD("decodeRawResource start: resId=$resId, size=$size")
            WebPYUVDecoder.decodeRawWebPToAnim(WebpContext.get(), resId, size)
        }.onFailure {
            logE("decodeRawResource failed: ${it.message}")
        }.getOrNull().also {
            logD("decodeRawResource end: resId=$resId, size=$size")
        }
    }

    private fun logD(message: String) {
        runCatching { WebpLog.d("WebPAnimResultManager", message) }
    }

    private fun logW(message: String) {
        runCatching { WebpLog.w("WebPAnimResultManager", message) }
    }

    private fun logE(message: String) {
        runCatching { WebpLog.e("WebPAnimResultManager", message) }
    }

    private fun trimCacheLocked() {
        val maxEntries = maxCacheEntriesOverride ?: WebpDeviceProfile.current().maxCacheEntries
        val maxBytes = cacheBudgetBytesOverride ?: WebpDeviceProfile.current().cacheBudgetBytes

        fun currentBytes(): Long = webPAnimResultCache.values.sumOf { estimateAnimBytes(it) }

        while (webPAnimResultCache.isNotEmpty() &&
            (webPAnimResultCache.size > maxEntries || currentBytes() > maxBytes)
        ) {
            val eldestEntry = webPAnimResultCache.entries.iterator().next()
            webPAnimResultCache.remove(eldestEntry.key)
            releaseAnimResult(eldestEntry.value)
            logD("trimCacheLocked: evicted ${eldestEntry.key}")
        }
    }

    private fun estimateAnimBytes(anim: WebPAnimResult): Long {
        return anim.frames?.sumOf { it.capacity().toLong() } ?: 0L
    }
}
