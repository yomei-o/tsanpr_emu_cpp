# x86emu compiled to WebAssembly

The emulator itself, built to WASM, so the **real** TS-ANPR `tsanpr.dll` + `.eon`
model run in a browser/node — actual inference, not a fixed-value stub.

- `build.sh` — link `emu/src/*.cpp` to `x86emu.js` + `x86emu.wasm` (Emscripten).
- Run under Node with real filesystem access (NODERAWFS):
  `node wasm-emu/x86emu.js "$PWD/anpr.exe"` (absolute path is required so the
  guest's `baseDir()` resolves `engine/tsanpr.dll`).

Status: the WASM emulator loads `tsanpr.dll`, resolves `anpr_initialize`, and
reaches the licence check. On a real Windows host the licence passes; off-Windows
(browser/node) there is no WMI/registry/HID, so the machine-fingerprint inputs
must be supplied as captured "host oracle" data for the licence to validate — see
the record/replay work in the emulator hooks. Inference itself is correct but very
slow under the interpreter (hours), the same as the native emulated run.

`.gitignore` keeps the large `x86emu.wasm` out unless you build it locally.
