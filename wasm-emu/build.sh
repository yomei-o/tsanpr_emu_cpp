#!/bin/sh
# Build the x86 emulator ITSELF to WebAssembly, so the real tsanpr.dll runs in a
# browser/node (real inference, not a stub). Requires emsdk (C:/emsdk here).
#   EM_CONFIG=C:/emsdk/.emscripten
set -e
cd "$(dirname "$0")/.."
em++ -std=c++17 -O2 emu/src/*.cpp -Iemu/src -o wasm-emu/x86emu.js \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4294967296 -sINITIAL_MEMORY=536870912 \
  -sSTACK_SIZE=8388608 -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_UNIMPLEMENTED_SYSCALLS=1
echo "built wasm-emu/x86emu.js (+ .wasm)"
echo "run (node, needs an ABSOLUTE guest path so baseDir resolves the engine):"
echo "  node wasm-emu/x86emu.js \"\$PWD/anpr.exe\""
