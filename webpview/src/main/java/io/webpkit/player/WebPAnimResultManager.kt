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

        tryCloneFromCache(key)?.let { return it }

        // 同一 key 的解码串行化：并发 miss 时只有第一个线程真正解码，
        // 其余线程在锁上等它完成后直接命中缓存（之前会各解一次、丢一份）
        return withKeyLock(key) {
            tryCloneFromCache(key)?.let { return@withKeyLock it }

            logD("getWebPAnimResult: cache miss, decoding once for $key")
            val decoded = runCatching {
                decodeDelegate(key.resId, key.size)
            }.onFailure {
                logE("decode failed for caller: $key, ${it.message}")
            }.getOrNull() ?: return@withKeyLock null

            synchronized(cacheLock) {
                val existing = webPAnimResultCache[key]
                if (existing != null) {
                    releaseAnimResult(decoded)
                    return@withKeyLock cloneAnimResult(existing)
                }

                webPAnimResultCache[key] = decoded
                val cloned = cloneAnimResult(decoded)
                trimCacheLocked()
                cloned
            }
        }
    }

    private fun tryCloneFromCache(key: CacheKey): WebPAnimResult? {
        synchronized(cacheLock) {
            webPAnimResultCache[key]?.let { cached ->
                removedFlags.remove(key)
                logD("getWebPAnimResult: cloned from cache: $key")
                cloneAnimResult(cached)?.let { return it }
                logW("getWebPAnimResult: clone failed, evict cache and decode again: $key")
                webPAnimResultCache.remove(key)?.let { releaseAnimResult(it) }
            }
        }
        return null
    }

    // 每个 key 一把解码锁。条目数最多为不同资源 key 的数量（很小），不主动清理——
    // 清理与 computeIfAbsent 并发会产生两把锁，反而破坏去重。
    private val decodeLocks = ConcurrentHashMap<CacheKey, Any>()

    private inline fun <T> withKeyLock(key: CacheKey, block: () -> T): T {
        val lock = decodeLocks.computeIfAbsent(key) { Any() }
        return synchronized(lock) { block() }
    }

    private fun ensureRefillInBackground(key: CacheKey) {
        if (!refillAsyncEnabled) return

        val existing = pendingRefillTasks[key]
        if (existing != null && !existing.isCompleted) {
            logD("refill already running for $key, reuse it")
            return
        }

        // refill 也走解码 dispatcher：吃到并行度上限和后台线程优先级
        val deferred = WebpExecutors.globalAsyncScope.async(
            WebpDeviceProfile.current().decodeDispatcher()
        ) {
            // 与 getWebPAnimResult 共享同一把 key 锁：避免 refill 和直接取用并发双解码
            val anim = withKeyLock(key) {
                if (synchronized(cacheLock) { webPAnimResultCache.containsKey(key) }) {
                    logD("refill: already cached by a concurrent getter, skip: $key")
                    return@withKeyLock null
                }
                logD("refill: start decoding for cache: $key")
                runCatching { decodeDelegate(key.resId, key.size) }.onFailure {
                    logE("refill decode failed: $key, ${it.message}")
                }.getOrNull()
            }

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
            if (!isWorthPreloading(key)) {
                preloadWebpResIds.remove(key)
                return@forEach
            }
            ensureRefillInBackground(key)
        }
    }

    /**
     * 预加载前置预判：只读容器头（不解码像素）估算解码成品大小，超出缓存预算的
     * 直接跳过——缓存进去也会被立刻逐出，解码纯属白做（实测一次能省 1 秒级 CPU）。
     * 拿不到元信息时不拦截，交给正常解码流程。
     */
    private fun isWorthPreloading(key: CacheKey): Boolean {
        val budget = cacheBudgetBytesOverride ?: WebpDeviceProfile.current().cacheBudgetBytes
        val info = runCatching {
            WebPYUVDecoder.peekAnimInfo(WebpContext.get(), key.resId)
        }.getOrNull() ?: return true
        if (info.width <= 0 || info.height <= 0 || info.frameCount <= 0) return true

        // 与 native ComputeFinalSize 同口径：aspect-fit、不放大、受 maxDecodePixels 约束
        var w = info.width
        var h = info.height
        val target = key.size
        if (target != null && target.width > 0 && target.height > 0) {
            val scale = minOf(
                target.width.toDouble() / w,
                target.height.toDouble() / h,
                1.0,
            )
            w = (w * scale).toInt().coerceAtLeast(1)
            h = (h * scale).toInt().coerceAtLeast(1)
        }
        val maxPixels = WebpDeviceProfile.current().maxDecodePixels
        if (maxPixels > 0 && w.toLong() * h > maxPixels) {
            val scale = kotlin.math.sqrt(maxPixels.toDouble() / (w.toLong() * h))
            w = (w * scale).toInt().coerceAtLeast(1)
            h = (h * scale).toInt().coerceAtLeast(1)
        }

        val estimatedBytes = info.frameCount.toLong() * w * h * 4
        if (estimatedBytes > budget) {
            logW(
                "preload skipped: $key 预估 ${estimatedBytes / 1024}KB " +
                    "(${info.frameCount}帧 ${w}x$h) 超出缓存预算 ${budget / 1024}KB，缓存后会被立刻逐出"
            )
            return false
        }
        return true
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
        if (anim == null) return
        anim.frames?.let { frames ->
            runCatching {
                releaseFramesDelegate(frames)
            }.onFailure {
                logE("release anim failed: ${it.message}")
            }
        }
        anim.hardwareFrames?.forEach { hb ->
            runCatching { if (!hb.isClosed) hb.close() }
        }
    }

    private fun cloneAnimResultInternal(anim: WebPAnimResult): WebPAnimResult? {
        // 硬件帧：clone = 每帧一个新 Java 包装，各自引用同一块 AHardwareBuffer（零拷贝）
        anim.hardwareFrames?.let { hwFrames ->
            val clonedHw = WebPYUVDecoder.cloneHardwareBuffers(hwFrames) ?: return null
            return WebPAnimResult(
                frames = null,
                hardwareFrames = clonedHw,
                canvasWidth = anim.canvasWidth,
                canvasHeight = anim.canvasHeight,
                durations = anim.durations.copyOf(),
                loopCount = anim.loopCount,
            )
        }
        val frames = anim.frames ?: return WebPAnimResult(
            frames = null,
            canvasWidth = anim.canvasWidth,
            canvasHeight = anim.canvasHeight,
            durations = anim.durations.copyOf(),
            loopCount = anim.loopCount,
        )
        // native 侧 clone 是引用计数 +1（共享同一块帧内存），不再是深拷贝
        val clonedFrames = WebPYUVDecoder.cloneNativeBuffers(frames) ?: return null
        return WebPAnimResult(
            frames = clonedFrames,
            canvasWidth = anim.canvasWidth,
            canvasHeight = anim.canvasHeight,
            durations = anim.durations.copyOf(),
            loopCount = anim.loopCount,
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
        anim.hardwareFrames?.let {
            return it.size.toLong() * anim.canvasWidth * anim.canvasHeight * 4
        }
        return anim.frames?.sumOf { it.capacity().toLong() } ?: 0L
    }
}
