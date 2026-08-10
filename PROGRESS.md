# x86emu × TS-ANPR — licensing bring-up progress

This tracks the effort to make `x86emu.exe anpr.exe` reach inference (readme.md
documents the original wall). Work is happening on a **real x86 Intel machine**
(i7-1165G7 Tiger Lake), not the readme's QEMU-ARM/Prism stack — so native can be
debugged directly, which the readme said was the way through.

## Gate 1 — machine fingerprint — SOLVED ✅

The license value-name is an MD5-like fingerprint of machine properties. The
emulator computed the wrong one because **HID/SetupAPI device enumeration
returned "no devices"**, where a real machine has HID devices; that empty set
poisoned the sequentially-hashed preimage. (On the readme's ARM VM native also
saw no HID devices, which is why it wasn't caught there.)

Fixed by bridging device enumeration to the host, faithfully:
- `emu/src/hooks_win32d.cpp` — `SetupDiGetClassDevs/EnumDeviceInterfaces/GetDeviceInterfaceDetail`
  forwarded to the host; `HidD_GetAttributes/GetProductString/…` forwarded to the
  real device handle.
- `emu/src/hooks_files.cpp` — `CreateFile` on a device-interface path
  (`\\?\hid#…`) opens the real host device; a small host-handle registry maps the
  guest handle back so HidD_* and CloseHandle reach it.
- `CMakeLists.txt` — link `setupapi hid`.

Result: the emulator now computes the **same fingerprint as native**
(`d4d1fe79…` on this machine) and the license lookup succeeds.

## Gate 2 — post-lookup validation — ROOT CAUSE IDENTIFIED, fix in progress ◐

After the lookup succeeds the engine runs a second validation that still fails
with `(105)`. Traced with a purpose-built native debugger (`scratch_ndbg.cpp`)
diffing native vs emulator instruction-by-instruction:

- The validation calls `std::stoi`; in native the token is `"1"`, in the
  emulator it is an **empty string**, so `std::invalid_argument` is thrown.
- The first control-flow divergence is at `tsanpr.dll+0x204B60C`, a branch on
  the MSVC CRT global **`__isa_available`** (RVA `0x2C34418`): **6 (AVX-512) in
  native vs 1 (SSE2) in the emulator** — because the emulator only advertises
  SSE2 via CPUID. So native and emulator take different CRT string-function code
  paths. This is (so far) benign ISA dispatch, masking the real data divergence.
Progress on isolating gate 2 (ruling things out):

- Forcing native's `__isa_available`→1 (via `ndbg --poke`) makes native take the
  same SSE2 CRT string paths as the emulator, and **native still passes** — so
  the SSE2 paths are correct on real hardware; the emulator's *execution* of them
  is what differs.
- Verified the emulator's `pcmpeqw`, `pmovmskb`, `pmaddwd` implementations are
  correct; the license-validation helper is `tsanpr.dll+0x14A4F50` (reads an int
  at licenseObject+0x49, itoa's it, converts "TS-ANPR", combines, then
  `std::stoi`s a token). The failing token is empty in the emulator.
- Instruction-level diff (aligned SSE2 paths) is confounded by **uninitialised
  stack**: e.g. a `wcslen` at +0x204B714 shows mask `0xfff0` (native) vs `0xc0f0`
  (emu), but both have their lowest set bit at position 4, so both yield length 2
  — the high-bit difference is garbage *past* the terminator and does not affect
  the result. This kind of benign divergence is pervasive and masks the real one.

- Next step (noise-robust): stop diffing instructions; instead compare the actual
  **string values** at the license helper's call boundaries (the split at
  `+0x146DF30` and `std::stoi`/`to_int` at `+0x14A511E`) between native and the
  emulator, and find where the token first becomes empty in the emulator — that
  is the emulator infidelity to fix. Tooling for both sides already exists
  (`ndbg` register/memory dumps + emulator `EMU_REGS`).

## Tools added this session (all reusable)

- `scratch_ndbg.cpp` → `ndbg.exe` — minimal Win32 debugger: breakpoints at
  tsanpr.dll RVAs, register+memory dumps, and a `--trace` mode that logs
  `rip+regs` per instruction for diffing against the emulator. Reads the DLL load
  base from the debug event, so no ASLR patch is needed.
- Emulator diagnostics (all env-gated, inert in normal runs): `X86EMU_RWATCH`
  read-watchpoint, `EMU_REGS=<rip>` register/memory dump, `EMU_REGADDR`,
  `EMU_ITRACE=<addr>:<N>` instruction trace, C++-throw type/message decode.
- `scratch_disasm.py` — capstone disassembly by RVA + `.pdata` function ranges
  and caller/callee resolution.

## Not published here

The machine-specific license material (`backup/` registry keys, the `*.req`
license request carrying this machine's hardware fingerprint) is git-ignored and
must not be published.
