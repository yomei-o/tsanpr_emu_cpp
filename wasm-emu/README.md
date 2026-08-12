# x86emu compiled to WebAssembly

The emulator itself, built to WASM, so the **real** TS-ANPR `tsanpr.dll` + `.eon`
model run in a browser/node — actual inference, not a fixed-value stub.

- `build.sh` — link `emu/src/*.cpp` to `x86emu.js` + `x86emu.wasm` (Emscripten).
- Run under Node with real filesystem access (NODERAWFS):

      EMU_GEMM_SKIP=1 EMU_ZERO_HOOK=1 EMU_STDOUT_TTY=1 \
      node wasm-emu/x86emu.js \
        --pnf "C:\\Windows\\inf\\vxd8.PNF=$PWD/oracle_vxd8_pnf.txt" \
        --iftable "$PWD/oracle_iftable2.txt" --wmi "$PWD/oracle_wmi.txt" \
        --registry "$PWD/backup/policies1.reg" --registry "$PWD/backup/policies2.reg" \
        "$PWD/anpr.exe"

  The flags come **before** the guest path (everything after `anpr.exe` is the
  guest's argv). The guest path is absolute so `baseDir()` resolves
  `engine/tsanpr.dll`. The four oracle flags are the same as the native run.

Status: **it passes and recognises plates.** With the four oracles the WASM
emulator loads `tsanpr.dll` (46 MB) and the encrypted 167 MB model, derives the
fingerprint `73e3e41855fba8d949120fe4ac51b3f4`, passes `anpr_initialize`, and runs
inference — the full `多摩500さ4649` … `越谷300ち7985` set, all three images and
the detection modes, identical to the native run. On node (Apple M2): **~20 min**
for everything, peak RSS ~1.3 GB (first plate ~3 min). That is only ~1.8× the
native emulated run, because `EMU_GEMM_SKIP`'s 4×8 tile is plain C++ and compiles
to wasm too; the interpreted remainder is the rest of the gap.

Note `EMU_AES_SKIP` does **nothing** under wasm — `host_aes.h` defines no host AES
there, so the CBC hook is compiled out and the 167 MB decrypt falls to the
software byte loop (a few minutes). Everything else is unchanged from native.

`.gitignore` keeps the large `x86emu.wasm` out unless you build it locally; the
`x86emu.js` glue is committed.

## Browser (not wired yet)

This build is `NODERAWFS=1`, i.e. node-only — it reads the engine, model and
oracles straight from the local filesystem. A browser build needs those files in
MEMFS instead (a non-NODERAWFS build with the files preloaded) and a small page
that sets the argv and shows stdout. The blocker for hosting it on GitHub Pages is
the model: `tsanpr-2512M.eon` is 167 MB (Git LFS, over GitHub's 100 MB per-file
limit), which Pages does not serve, so the browser demo would need the model
hosted elsewhere (or split). Node is the way to run the real engine today.

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
