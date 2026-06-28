{
  "targets": [
    {
      "target_name": "electron-libmpv",
      "sources": ["index.cc"],
      "include_dirs": [
        "node_modules/node-addon-api"
      ],
      "dependencies": [
        "node_modules/node-addon-api/node_addon_api.gyp:node_addon_api"
      ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "defines": [ "NAPI_DISABLE_CPP_EXCEPTIONS" ],
      "conditions": [
        ["OS=='win'", {
          "include_dirs": [
            "C:/mpv-dev/include"
          ],
          "libraries": [
            "-llibmpv-2.dll.a"
          ],
          "library_dirs": [
            "C:/mpv-dev/x86_64"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": { "ExceptionHandling": 1 }
          }
        }],
        ["OS=='linux'", {
          "libraries": ["-lmpv"],
          "cflags_cc": ["-std=c++17"]
        }],
        ["OS=='mac'", {
          "libraries": ["-lmpv"],
          "xcode_settings": {
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "MACOSX_DEPLOYMENT_TARGET": "10.15",
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES"
          }
        }]
      ]
    }
  ]
}