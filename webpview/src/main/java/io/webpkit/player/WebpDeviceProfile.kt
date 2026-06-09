package io.webpkit.player

import android.os.Build
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CoroutineDispatcher

data class WebpDeviceProfile(
    val name: String,
    val decodeParallelism: Int,
    val cacheBudgetBytes: Long,
    val maxCacheEntries: Int,
) {
    fun decodeDispatcher(): CoroutineDispatcher {
        return Dispatchers.IO.limitedParallelism(decodeParallelism.coerceAtLeast(1))
    }

    companion object {
        fun current(): WebpDeviceProfile {
            return resolve(
                model = Build.MODEL.orEmpty(),
                boardPlatform = Build.BOARD.orEmpty(),
                socModel = readBuildField("SOC_MODEL"),
                gpuRenderer = ""
            )
        }

        fun resolve(
            model: String,
            boardPlatform: String,
            socModel: String,
            gpuRenderer: String,
        ): WebpDeviceProfile {
            val normalizedModel = model.lowercase()
            val normalizedBoard = boardPlatform.lowercase()
            val normalizedSoc = socModel.lowercase()
            val normalizedGpu = gpuRenderer.lowercase()

            if (
                normalizedModel.contains("dn-eng41") ||
                normalizedBoard.contains("bengal") ||
                normalizedSoc.contains("sm6225") ||
                normalizedGpu.contains("adreno 610")
            ) {
                return WebpDeviceProfile(
                    name = "sm6225_adreno610",
                    decodeParallelism = 1,
                    cacheBudgetBytes = 8L * 1024 * 1024,
                    maxCacheEntries = 1,
                )
            }

            if (
                normalizedModel.contains("dn-eng81") ||
                normalizedBoard.contains("mt6789") ||
                normalizedSoc.contains("mt8781") ||
                normalizedGpu.contains("mali-g57")
            ) {
                return WebpDeviceProfile(
                    name = "mt8781_malig57",
                    decodeParallelism = 2,
                    cacheBudgetBytes = 20L * 1024 * 1024,
                    maxCacheEntries = 2,
                )
            }

            return WebpDeviceProfile(
                name = "default",
                decodeParallelism = 2,
                cacheBudgetBytes = 12L * 1024 * 1024,
                maxCacheEntries = 1,
            )
        }

        private fun readBuildField(fieldName: String): String {
            return runCatching {
                Build::class.java.getField(fieldName).get(null) as? String
            }.getOrNull().orEmpty()
        }
    }
}
