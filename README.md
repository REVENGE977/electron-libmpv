<h1 align="center">
  electron-libmpv<br>
</h1>

<h4 align="center">
  A cross-platform library to embed mpv in Electron applications using libmpv, Node-API, and zero-copy GPU shared textures.
</h4>

A package inspired by [mpv.js](https://github.com/Kagami/mpv.js/) for modern Electron apps. Instead of legacy Pepper plugins or OS window-handle embedding, `electron-libmpv` uses a cross-platform `mpv_render_context` pipeline with triple-buffered zero-copy GPU shared textures designed for Electron's `sharedTexture` API. Video renders directly into your web layout with hardware acceleration and HDR support, plus an automatic software rendering fallback.

## Platform Support

| Platform | GPU Backend | Shared Texture Handle | HDR Support | Runtime Library |
| :--- | :--- | :--- | :--- | :--- |
| **Windows** | ANGLE (EGL/GLES3) + D3D11 (`angle-d3d11`) | D3D11 Shared NT Handle (`ntHandle`) | Yes (RGBA16F) | `libmpv-2.dll` |
| **Linux** | Headless GBM + EGL DMA-BUF (`egl-dmabuf`) | NativePixmap DMA-BUF FD (`nativePixmap`) | Yes (RGBA16F) | `libmpv.so.2` |
| **macOS** (11.0+) | CGL OpenGL 3.2 Core + IOSurface (`cgl-iosurface`) | `IOSurfaceRef` (`ioSurface`) | Yes (64RGBAHalf) | `libmpv.2.dylib` |
| **Fallback (All)** | Software (`MPV_RENDER_API_TYPE_SW`) | RGBA CPU Buffer (`software`) | No | Any of the above |

On Linux, `libmpv` runs inside a lightweight helper executable (`electron-libmpv-helper`) built alongside the native addon. Frames are passed zero-copy over `AF_UNIX` sockets using `SCM_RIGHTS` DMA-BUF file descriptors, isolating `libmpv` from symbol and allocator conflicts with Electron's bundled `libffmpeg.so` and PartitionAlloc.

## Requirements

### Build Requirements

The `mpv` C headers are bundled in this repository and `libmpv` is loaded dynamically at runtime, so you do not need `libmpv` development libraries to compile the addon.

* **Windows**: Visual Studio Build Tools with the "Desktop development with C++" workload (C++17).
* **Linux**: GCC or Clang with C++17 support, `make`, Python, and EGL/GLES2/GBM development headers (for example, on Debian/Ubuntu: `sudo apt install build-essential libegl-dev libgles2-dev libgbm-dev`).
* **macOS**: Xcode Command Line Tools (`xcode-select --install`), targeting macOS 11.0+.

### Runtime Requirements (`libmpv`)

You only need the compiled `libmpv` shared library available at runtime:

* **Windows**: Download `libmpv` (for example from [mpv-player-windows](https://sourceforge.net/projects/mpv-player-windows/files/libmpv/)). Place `libmpv-2.dll` next to your Electron executable, in `resources\libmpv-2.dll`, in the working directory, or on `PATH`.
* **Linux**: Install `libmpv` via your distribution's package manager (for example `sudo apt install libmpv2` on Debian/Ubuntu, `sudo pacman -S mpv` on Arch, or `sudo dnf install mpv-libs` on Fedora).
* **macOS**: Install via Homebrew (`brew install mpv`) or bundle `libmpv.2.dylib` inside your app's `Frameworks` or `Resources` directory.

You can also specify a custom library path at runtime by passing `libmpvPath` to the `Mpv` constructor or by setting the `ELECTRON_LIBMPV_PATH` environment variable.

## Installation

```sh
npm install github:REVENGE977/electron-libmpv
```

## Building from Source

```sh
npm run build
```

If you are building for a specific Electron version, rebuild with `@electron/rebuild`:

```sh
npm run rebuild
```

## Usage

```js
const Mpv = require("electron-libmpv");

const player = new Mpv({
  libmpvPath: "", // Optional: explicit path to libmpv
  configPath: "", // Optional: path to an mpv.conf file
  width: 1280,
  height: 720,
  onEvent: (event) => {
    // Handles start-file, file-loaded, playback-restart, end-file, property-change, log-message
    console.log("mpv event:", event);
  },
  onFrame: (frame) => {
    // frame.kind is "sharedTexture" (with frame.handle) or "software" (with frame.buffer)
    // Pass frame.handle to Electron's sharedTexture API, then release the slot when done:
    player.releaseFrame(frame.slotId, frame.generation);
  },
});

const init = player.initialize({
  width: 1280,
  height: 720,
  hdr: false,
  preferGpu: true,
});

if (!init.ok) {
  console.error("Failed to initialize mpv:", init.error);
} else {
  console.log("Active backend:", init.backend, "HDR:", init.hdrSupported);
  player.observeProperty("time-pos", "double");
  player.observeProperty("duration", "double");
  player.command("loadfile", "/path/to/video.mkv");
}
```

### API Summary

* `new Mpv(options?)`: Creates a player instance (`libmpvPath`, `configPath`, `width`, `height`, `onEvent`, `onFrame`).
* `initialize(options?)`: Initializes `libmpv` and starts the render thread (`width`, `height`, `hdr`, `preferGpu`). Returns `{ ok, backend, sharedTexture, hdrSupported, libmpvPath, gpuError, error }`.
* `resize(width, height, hdr?)`: Resizes the render surfaces and optionally toggles HDR.
* `releaseFrame(slotId, generation?)`: Returns a triple-buffer frame slot back to the render pool after Electron finishes importing it.
* `enableSoftwareFallback()`: Switches an active GPU backend to software rendering at runtime.
* `command(...args)`: Sends an mpv command (for example `player.command("loadfile", url)`).
* `property(name, value)`: Sets an mpv property (string, number, or boolean).
* `getRawProperty(name, format?)`: Reads an mpv property as `"string"` (default) or `"node"`.
* `observeProperty(name, format?)`: Observes property changes (`"string"`, `"double"`, `"flag"`, `"int64"`, `"node"`).
* `unobserveAll()`: Removes all property observers.
* `getBackendInfo()`: Returns current `libmpv` loading state, active backend, shared texture support, and HDR capability.
* `destroy()`: Shuts down the render thread, helper process (on Linux), and `libmpv` instance.

Full TypeScript definitions are included in [index.d.ts](./index.d.ts).


## Usage

```js
const Mpv = require("electron-libmpv");

const player = new Mpv({
  libmpvPath: "", // Optional: explicit path to libmpv
  configPath: "", // Optional: path to an mpv.conf file
  width: 1280,
  height: 720,
  onEvent: (event) => {
    // Handles start-file, file-loaded, playback-restart, end-file, property-change, log-message
    console.log("mpv event:", event);
  },
  onFrame: (frame) => {
    // frame.kind is "sharedTexture" (with frame.handle) or "software" (with frame.buffer)
    // Pass frame.handle to Electron's sharedTexture API, then release the slot when done:
    player.releaseFrame(frame.slotId, frame.generation);
  },
});

const init = player.initialize({
  width: 1280,
  height: 720,
  hdr: false,
  preferGpu: true,
});

if (!init.ok) {
  console.error("Failed to initialize mpv:", init.error);
} else {
  console.log("Active backend:", init.backend, "HDR:", init.hdrSupported);
  player.observeProperty("time-pos", "double");
  player.observeProperty("duration", "double");
  player.command("loadfile", "/path/to/video.mkv");
}
```

### API Summary

* `new Mpv(options?)`: Creates a player instance (`libmpvPath`, `configPath`, `width`, `height`, `onEvent`, `onFrame`).
* `initialize(options?)`: Initializes `libmpv` and starts the render thread (`width`, `height`, `hdr`, `preferGpu`). Returns `{ ok, backend, sharedTexture, hdrSupported, libmpvPath, gpuError, error }`.
* `resize(width, height, hdr?)`: Resizes the render surfaces and optionally toggles HDR.
* `releaseFrame(slotId, generation?)`: Returns a triple-buffer frame slot back to the render pool after Electron finishes importing it.
* `enableSoftwareFallback()`: Switches an active GPU backend to software rendering at runtime.
* `command(...args)`: Sends an mpv command (for example `player.command("loadfile", url)`).
* `property(name, value)`: Sets an mpv property (string, number, or boolean).
* `getRawProperty(name, format?)`: Reads an mpv property as `"string"` (default) or `"node"`.
* `observeProperty(name, format?)`: Observes property changes (`"string"`, `"double"`, `"flag"`, `"int64"`, `"node"`).
* `unobserveAll()`: Removes all property observers.
* `getBackendInfo()`: Returns current `libmpv` loading state, active backend, shared texture support, and HDR capability.
* `destroy()`: Shuts down the render thread, helper process (on Linux), and `libmpv` instance.

Full TypeScript definitions are included in [index.d.ts](./index.d.ts).