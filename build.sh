#!/usr/bin/env bash
# Rebuild the WASM engine. Needs emscripten (emsdk) on PATH: `source /path/to/emsdk/emsdk_env.sh`.
set -euo pipefail
cd "$(dirname "$0")"
command -v em++ >/dev/null || { echo "em++ not found — source emsdk_env.sh first"; exit 1; }
em++ -std=c++17 -O3 -I src/aicodix src/wasm-glue.cpp -o web/rattlegram.js \
  -sMODULARIZE=1 -sEXPORT_NAME=createRattlegram -sENVIRONMENT=web \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAP16,HEAP32,HEAPF32,HEAPU8,UTF8ToString \
  -sEXPORTED_FUNCTIONS=_rg_encode,_rg_dec_create,_rg_dec_destroy,_rg_dec_push,_rg_dec_fetch,_rg_bandwidth,_malloc,_free \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sSTACK_SIZE=5MB
echo "built web/rattlegram.{js,wasm}"
