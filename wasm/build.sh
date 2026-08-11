#!/bin/sh
# Build the fixed-value ANPR WASM stub. Requires an activated Emscripten (emcc).
# On this machine emsdk lives at C:/emsdk:
#   EM_CONFIG=C:/emsdk/.emscripten  and  C:/emsdk/upstream/emscripten/emcc.exe
set -e
emcc anpr_stub.c -O2 -o tsanpr.js \
  -sMODULARIZE=1 -sEXPORT_NAME=createTsanpr -sSINGLE_FILE=1 \
  -sEXPORTED_FUNCTIONS=_anpr_initialize,_anpr_read_file,_anpr_read_pixels,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=cwrap,ccall,UTF8ToString,stringToUTF8,lengthBytesUTF8 \
  -sALLOW_MEMORY_GROWTH=1
echo "built tsanpr.js (wasm embedded via SINGLE_FILE); open index.html over http to run"
