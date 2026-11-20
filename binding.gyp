{
  "targets": [
    {
      "target_name": "electron-libmpv",
      "sources": ["index.cc"],
      "include_dirs": [
        ".",
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "defines": [ "NAPI_DISABLE_CPP_EXCEPTIONS" ],
      "conditions": [
        ["OS=='win'", {
          "libraries": [
            "d3d11.lib",
            "dxgi.lib"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": [ "/std:c++17" ]
            }
          }
        }],
        ["OS=='linux'", {
          "libraries": ["-ldl"],
          "cflags_cc": ["-std=c++17"],
          "dependencies": ["electron-libmpv-helper"]
        }],
        ["OS=='mac'", {
          "libraries": [
            "-framework OpenGL",
            "-framework IOSurface",
            "-framework CoreFoundation"
          ],
          "xcode_settings": {
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "MACOSX_DEPLOYMENT_TARGET": "11.0",
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES"
          }
        }]
      ]
    },
    {
      "target_name": "electron-libmpv-helper",
      "type": "executable",
      "sources": ["src/mpv_helper_linux.cc"],
      "include_dirs": ["."],
      "conditions": [
        ["OS=='linux'", {
          "libraries": ["-ldl", "-lpthread"],
          "cflags_cc": ["-std=c++17", "-pthread"]
        }],
        ["OS!='linux'", {
          "type": "none",
          "sources!": ["src/mpv_helper_linux.cc"]
        }]
      ]
    }
  ]
}
