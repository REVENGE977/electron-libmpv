<h1 align="center">
  electron-libmpv<br>
</h1>

<h4 align="center">
  A library to embed mpv in Electron applications using libmpv and node-api.
</h4>

A package based on [mpv.js](https://github.com/Kagami/mpv.js/) for modern Electron apps. Replaces legacy Pepper plugin architecture used in mpv.js with a native C++ window-embedding architecture that hooks directly into the host OS window handle for zero-copy hardware acceleration.
This means your video renders completely inline within your layout, allowing for full HTML/CSS overlays and dynamic window resizing.

Check the example in the example folder. **For the example to work, you'll need libmpv-2.dll in the project's root folder.**

**Note: This library only works on Windows, for now.**

## Requirements
* Install Visual Studio Build Tools with "Desktop development with C++"

## How to install?
```sh
npm install github:REVENGE977/electron-libmpv
```

## How to build?

### Get libmpv

To build and run the package, you need the [libmpv](https://sourceforge.net/projects/mpv-player-windows/files/libmpv/) development files and the compiled runtime binary.

* **Windows**: Download libmpv. You will need `mpv.h`, `client.h` and `libmpv-2.dll.a` for compilation, and `libmpv-2.dll` to run your electron app.
  
These files and folders need to exist in your C: drive inside a folder named mpv-dev


![libmpv](/images/mpv-dev.jpeg)

* **Development/Example:** Place `libmpv-2.dll` in the root of your project directory next to your main `index.js`.
* **Production:** Ensure `libmpv-2.dll` is bundled into your final Electron build directory next to the main executable or within the app's working directory so Windows can locate it at runtime.
  
### Build using node-gyp
```sh
npm run build
```

### Run example
put libmpv-2.dll in the root folder of this project then run using
```sh
npm run example
```