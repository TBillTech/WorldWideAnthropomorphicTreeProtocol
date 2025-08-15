{
  "targets": [
    {
      "target_name": "wwatp_quic_native",
      "sources": [ "src/addon.cc" ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "<(module_root_dir)/../../common/transport/include"
      ],
      "libraries": [
        "-L<(module_root_dir)/../../build",
        "-lwwatp_quic"
      ],
      "cflags_cc": [ "-std=c++17" ],
      "defines": [ "NAPI_DISABLE_CPP_EXCEPTIONS" ],
      "conditions": [
        [ 'OS=="linux"', {
          "link_settings": {
            "libraries": [
              "-Wl,-rpath,<(module_root_dir)/../../build"
            ]
          }
        }]
      ]
    }
  ]
}
