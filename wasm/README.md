# TS-ANPR — WASM fixed-value demo

The real TS-ANPR engine is a 167 MB ONNX model; it runs natively and (very
slowly) under the x86 emulator in this repo, but it cannot ship to a browser.
This is a **stub**: the exported C API (`anpr_initialize`, `anpr_read_file`,
`anpr_read_pixels` — mirroring [`src/tsanpr.h`](../src/tsanpr.h)) is compiled to
WebAssembly and returns **fixed, canned results** so the API surface can be
exercised from JavaScript.

## Files
- `anpr_stub.c` — the fixed-value implementation of the API.
- `tsanpr.js` — Emscripten output (the `.wasm` is embedded via `SINGLE_FILE`).
- `index.html` — a small browser demo that calls the API and shows the result.
- `build.sh` — rebuild `tsanpr.js` from `anpr_stub.c`.
- `test_node.mjs` — a headless smoke test.

## Run
Serve the folder over HTTP (a `<script src>` to the module needs http, not file://):

    cd wasm
    python -m http.server 8000
    # then open http://localhost:8000/index.html

Or smoke-test headlessly with Node:

    node test_node.mjs
    # init = ""
    # text = 品川 330 あ 12-34
    # json = {"success":true,...}

## Calling from JS
```js
const M = await createTsanpr();
const init     = M.cwrap('anpr_initialize', 'string', ['string']);
const readFile = M.cwrap('anpr_read_file',  'string', ['string','string','string']);
init('text;country=JP');                       // => "" (success)
readFile('licensePlate.jpg', 'text', '');      // => "品川 330 あ 12-34"
```
