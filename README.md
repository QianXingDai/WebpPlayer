# WebpKit Player

English | [简体中文](README.zh-CN.md)

A self-contained Android library for high-performance **animated WebP** playback,
built on a trimmed, bundled copy of [libwebp](https://chromium.googlesource.com/webm/libwebp)
(encoder + decoder source) compiled to a native `.so`.

It decodes every frame of an animated WebP to RGBA via JNI and renders it either
on an OpenGL ES 2.0 surface (lowest overhead, GPU-composited) or through the
platform `ImageDecoder` / `AnimatedImageDrawable` pipeline.

- **Zero host dependencies** — the library has its own logging, threading and
  context bootstrap. Drop the AAR in and go; no init call required.
- **Native libwebp** — full encode/decode C source is bundled and compiled from
  scratch (no prebuilt binaries), currently packaged for `arm64-v8a`.
- **Frame caching** with a device-aware memory budget and async refill.

## Modules / public API

| Class | What it does |
|-------|--------------|
| `WebpGLView` | A `GLSurfaceView` that decodes and plays a single animated WebP from a raw resource. |
| `WebpImageView` | An `ImageView` that plays an animated WebP via `AnimatedImageDrawable` (sync or async decode). |
| `MultiWebpGLView` | Renders **multiple** WebP layers on one GL surface, each with its own position, size and fps. |
| `MultiWebpGLViewContainer` | Wraps `MultiWebpGLView` and lets a single layer be lifted off the GL surface so another view can be drawn above it. |
| `WebPAnimResultManager` | Decode cache (clone-on-read, native-backed buffers, LRU eviction). |
| `WebPYUVDecoder` | Low-level JNI decoder (`decodeAllFrames`, `decodeAllFramesWithSize`, buffer clone/release). |

## Requirements

- `minSdk` 28 (uses `ImageDecoder` / `AnimatedImageDrawable`)
- ABI: `arm64-v8a`
- NDK + CMake only needed to build from source — consumers of the AAR do not need them.

## Install

### Option A — Gradle dependency (recommended, auto-download)

Add the repository, then one line for the dependency. Transitive deps
(coroutines, lifecycle) are pulled in automatically via the published POM.

`settings.gradle.kts` (or your project `build.gradle`):

```kotlin
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url = uri("https://raw.githubusercontent.com/kakakakacat/WebpPlayer/mvn-repo/") }
    }
}
```

App `build.gradle.kts`:

```kotlin
dependencies {
    implementation("io.webpkit:player:1.0.0")
}
```

<details>
<summary>Groovy DSL</summary>

```groovy
// settings.gradle
maven { url 'https://raw.githubusercontent.com/kakakakacat/WebpPlayer/mvn-repo/' }
// build.gradle
implementation 'io.webpkit:player:1.0.0'
```
</details>

### Option B — drop in the AAR manually

Copy `player-1.0.0.aar` into your app's `libs/` folder and add:

```kotlin
dependencies {
    implementation(files("libs/player-1.0.0.aar"))
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.6.2")
}
```

### Option C — build from source

```bash
git clone https://github.com/kakakakacat/WebpPlayer.git
cd WebpPlayer
./gradlew :webpview:assembleRelease
# → webpview/build/outputs/aar/webpview-release.aar
```

## Usage

### Single animation on a GL surface

```kotlin
val view = WebpGLView(context)
view.setWebpFromRaw(R.raw.my_anim, fps = 20)
view.start()   // play
view.stop()    // pause
```

### Simple ImageView playback

```kotlin
val iv = WebpImageView(context)
iv.setWebpFromRaw(R.raw.my_anim)
iv.start()
```

### Multiple layers

```kotlin
val gl = MultiWebpGLView(context)
gl.setLayers(
    listOf(
        WebpLayer(R.raw.bg,  x = 0f,   y = 0f,   width = 300f, height = 300f, fps = 24),
        WebpLayer(R.raw.fx,  x = 80f,  y = 80f,  width = 140f, height = 140f, fps = 12),
    )
)
gl.start()
gl.setLayerPosition(1, 100f, 100f)   // move a layer without re-decoding
gl.hideLayer(1)                       // toggle visibility
```

> ⚠️ The GL views use `setZOrderOnTop(true)` and are **not** suitable for use
> inside a `RecyclerView` (surface lifecycle / resource conflicts cause flicker).

## Initialization

The application context is captured automatically at process start by a tiny
`ContentProvider` (`WebpInitProvider`). If you strip manifest providers, call
once yourself:

```kotlin
WebpContext.init(applicationContext)
```

To silence logging (e.g. in release):

```kotlin
WebpLog.enabled = false
```

## License

MIT for the wrapper code. Bundled libwebp is BSD-licensed by Google — see
[`LICENSE`](LICENSE) and `webpview/src/main/cpp/libwebp/COPYING`.
