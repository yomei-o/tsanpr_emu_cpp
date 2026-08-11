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

## Host-oracle record/replay (real inference off-Windows)

Off-Windows there is no WMI/registry/HID/adapter-table, so the machine-fingerprint
inputs the licence needs are captured on a real Windows run and replayed:

- `EMU_HOSTREC=<file> x86emu.exe anpr.exe` — record what each host hook writes to
  guest memory (+ its return) into `<file>` (kill once the licence has passed).
- `EMU_HOSTREP=<file>` (in the WASM build) — replay those bytes in order instead
  of calling the host. Implemented in `dispatch_hook` + a guest-write recorder in
  `Memory`; only fingerprint/licence hooks are recorded (registry, WMI ExecQuery/
  Get/Next, HID, GetIfTable2*), chosen so the call order matches between record
  and replay.

Status: the WASM emulator loads the real DLL, and replay drives it past the WMI/
registry/HID/adapter oracle calls with no desync. A remaining fidelity issue (a
null indirect call, rip=0, in the licence-validation code around tsanpr.dll
+0x14EBD82) still has to be resolved before the licence fully validates off-
Windows — the last host-state difference to track down. Once past it, inference
runs exactly as in the native emulated run (correct, but hours-slow).

Build the WASM emulator with exceptions on so faults report instead of aborting:
`em++ -O2 -fexceptions emu/src/*.cpp ... -sNO_DISABLE_EXCEPTION_CATCHING`.
