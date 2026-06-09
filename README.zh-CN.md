# WebpKit Player

[English](README.md) | 简体中文

一个**独立、零宿主依赖**的 Android 动画 WebP 播放库，底层内置了一份
[libwebp](https://chromium.googlesource.com/webm/libwebp) 源码（编码 + 解码），
通过 CMake 从源码现编译出原生 `.so`（无预编译二进制）。

它通过 JNI 把动画 WebP 的每一帧解码为 RGBA，然后用两种方式渲染：OpenGL ES 2.0
表面（开销最低、GPU 合成），或平台自带的 `ImageDecoder` / `AnimatedImageDrawable`
流水线。

- **零宿主依赖** —— 自带日志、线程调度和 Context 引导。AAR 丢进去即用，无需调用任何初始化方法。
- **原生 libwebp** —— 完整的编/解码 C 源码原样内置并从零编译，当前打包架构为 `arm64-v8a`。
- **帧缓存** —— 带设备自适应的内存预算和异步回填（refill）。

## 公开 API

| 类 | 作用 |
|----|------|
| `WebpGLView` | 一个 `GLSurfaceView`，从 raw 资源解码并播放**单个**动画 WebP。 |
| `WebpImageView` | 一个 `ImageView`，通过 `AnimatedImageDrawable` 播放动画 WebP（支持同步/异步解码）。 |
| `MultiWebpGLView` | 在**一个** GL 表面上渲染**多个** WebP 图层，每层有独立的位置、尺寸和帧率。 |
| `MultiWebpGLViewContainer` | 包装 `MultiWebpGLView`，可把单个图层从 GL 表面"抬"出来，让别的 View 盖在它上面。 |
| `WebPAnimResultManager` | 解码缓存（读取时 clone、native 内存承载、LRU 淘汰）。 |
| `WebPYUVDecoder` | 底层 JNI 解码器（`decodeAllFrames`、`decodeAllFramesWithSize`、缓冲区 clone/release）。 |

## 环境要求

- `minSdk` 28（用到 `ImageDecoder` / `AnimatedImageDrawable`）
- ABI：`arm64-v8a`
- NDK + CMake 仅在**从源码构建**时需要——直接用 AAR 的使用者无需安装。

## 接入

### 方式 A —— 直接丢 AAR

把 `webpview-release.aar` 拷进 App 的 `libs/` 目录，然后：

```kotlin
dependencies {
    implementation(files("libs/webpview-release.aar"))
    // AAR 需要的传递依赖：
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.6.2")
}
```

### 方式 B —— 从源码构建

```bash
git clone https://github.com/QianXingDai/WebpPlayer.git
cd WebpPlayer
./gradlew :webpview:assembleRelease
# 产物 → webpview/build/outputs/aar/webpview-release.aar
```

> `./gradlew :webpview:assembleRelease` 这一条命令会**自动**从内置的 libwebp 源码
> 编译出 `.so` 并打进 AAR，无需单独编译。如果你想单独构建原生库，也可以运行根目录的
> `./compile.sh`（产物输出到 `dist/arm64-v8a/`）。

## 使用示例

### GL 表面播放单个动画

```kotlin
val view = WebpGLView(context)
view.setWebpFromRaw(R.raw.my_anim, fps = 20)
view.start()   // 播放
view.stop()    // 暂停
```

### 用 ImageView 简单播放

```kotlin
val iv = WebpImageView(context)
iv.setWebpFromRaw(R.raw.my_anim)
iv.start()
```

### 多图层

```kotlin
val gl = MultiWebpGLView(context)
gl.setLayers(
    listOf(
        WebpLayer(R.raw.bg, x = 0f,  y = 0f,  width = 300f, height = 300f, fps = 24),
        WebpLayer(R.raw.fx, x = 80f, y = 80f, width = 140f, height = 140f, fps = 12),
    )
)
gl.start()
gl.setLayerPosition(1, 100f, 100f)   // 不重新解码即可移动图层
gl.hideLayer(1)                       // 切换可见性
```

> ⚠️ GL 系列 View 使用了 `setZOrderOnTop(true)`，**不适合**放在 `RecyclerView` 里
> （表面生命周期 / 资源冲突会导致黑屏或闪烁）。

## 初始化

应用 Context 会在进程启动时由一个极小的 `ContentProvider`（`WebpInitProvider`）
自动捕获，使用者无需任何操作。如果你裁剪了清单里的 provider，请手动调用一次：

```kotlin
WebpContext.init(applicationContext)
```

关闭日志（例如 Release 包）：

```kotlin
WebpLog.enabled = false
```

## 架构说明

- `webpview/src/main/cpp/libwebp/` 是 libwebp 的**逐字节原样拷贝**，未改动任何源文件。
- 唯一改动的原生文件是 JNI 胶水 `WebPYUVDecoder.cpp`，仅把函数名与 `FindClass` 的包名
  由原始包改为 `io.webpkit.player`（JNI 按包名逐字绑定，重命名后这是必须的）。
- 其余适配（日志、线程、Context、视图基类）全部集中在上层 Kotlin 代码。

## 许可证

封装代码采用 MIT 许可。内置的 libwebp 由 Google 以 BSD 许可证发布——详见
[`LICENSE`](LICENSE) 与 `webpview/src/main/cpp/libwebp/COPYING`。
