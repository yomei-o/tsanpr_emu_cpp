#!/bin/sh
# Build x86emu to WebAssembly for the BROWSER (see build.sh for the node build).
#
# The difference is the filesystem.  build.sh uses NODERAWFS, i.e. the real disk,
# which a browser has none of - so this builds against MEMFS and the page fetches
# the engine, model, images and oracles at runtime and writes them in with
# FS.writeFile before running.  MODULARIZE + a factory lets the page populate the
# FS and set the argv/env first, then call main itself.
#
#   EM_CONFIG=... sh wasm-emu/build_web.sh
#   (then serve the repo root over http and open wasm-emu/web/index.html)
set -e
cd "$(dirname "$0")/.."
em++ -std=c++17 -O2 emu/src/*.cpp -Iemu/src -o wasm-emu/web/x86emu.js \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2147483648 -sINITIAL_MEMORY=268435456 \
  -sSTACK_SIZE=8388608 -sEXIT_RUNTIME=1 -sALLOW_UNIMPLEMENTED_SYSCALLS=1 \
  -sMODULARIZE=1 -sEXPORT_NAME=createX86emu -sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV \
  -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1
echo "built wasm-emu/web/x86emu.js (+ .wasm)"
echo "serve the repo ROOT over http (so /engine, /img, /oracle_*.txt resolve), e.g.:"
echo "  (cd \"\$(git rev-parse --show-toplevel)\" && python3 -m http.server 8000)"
echo "then open http://localhost:8000/wasm-emu/web/"
