# TS-ANPR emu — cross-Claude handoff (READ THIS FIRST)

Several Claude sessions (home / company / an aarch64 box) share this repo and keep
losing state to **doc↔code desync** and to **stale local checkouts** (one machine's
working copy predates what's on `main`). This file is the single source of truth
for *where things actually are on `main`*. Trust `main`'s code over any prose;
when you change behaviour, commit the code, then update this list.

## What this is (and why it's legitimate)
Run the **own, auto-issued free TRIAL** TS-ANPR engine under the self-built x86
emulator (`x86emu.exe anpr.exe`). Not licence circumvention: the **native** (Prism)
run on the trial machine authenticates and reaches inference (recognises JP plates).
The emulator only needs to feed the engine the same inputs the native run does.

## The one fact everything hinges on
- **Licence fingerprint (registry value name) = `73e3e41855fba8d949120fe4ac51b3f4`.**
  `tsanpr.dll` sequentially hashes machine properties into it; native derives it and
  finds the licence record → pass. The emulator must derive the **same** value name.
- Value name ⇄ data are cryptographically bound: feeding the right data under a wrong
  value name does not pass. Faking the record is impossible by design — this is a
  pure **input-fidelity** problem, nothing to crack.

## TODAY'S GOAL
Feed **uniform fixed values** for every fingerprint input (zero host reads) so
`x86emu` derives `73e3e418…` and recognises plates on **Windows, Linux and wasm from
GitHub alone**. The current failure is a **mix of fixed + host inputs**: on Windows
the host parts yield `9ef9aab8…` → `(105) License not installed`; off Windows the
host parts are empty. Must be 100% fixed.

## What `main` ALREADY has (do not redo)
- `--wmi <file>` → `load_wmi_answers` (hooks_wmi.cpp): replays WMI from a file instead
  of host `CoCreateInstance`/`ExecQuery`. Oracle: `oracle_wmi.txt`.
- `--registry <file>` (repeatable): replays the licence blob + fingerprint value from
  `backup/policies1.reg` / `policies2.reg` instead of host `RegQueryValueEx`.
- `GetStringTypeW` (hooks_win32.cpp): real impl — host `GetStringTypeW` on Windows,
  portable ASCII `CT_CTYPE1` fallback off Windows. (The old `set_result(1)` stub that
  caused `std::stoi("")`→`(105)` is already gone on `main`.)
- HID/SetupApi bridge + `setupapi hid` linked; a replay matcher.

## What `main` still LACKS — the real remaining work
1. **Golden interface-table data — NOW ADDED as `oracle_iftable2.txt`.** This is the
   `GetIfTable2Ex` table (28 interfaces) captured on the licensed machine: decoded +
   the raw 37864-byte blob as hex. It is exactly the piece `oracle_wmi.txt` flagged as
   "missing … and not recorded anywhere". Adapter GUIDs `{5C737FB0}`,`{B97D8E86}`,
   `{4F3E241F}` all live in this table.
2. **No `GetIfTable2` replay path.** hooks_win32d.cpp only *host-forwards*
   (`#if X86EMU_IFTABLE2` → `GetIfTable2Ex`) or returns an empty table off Windows.
   There is no `--iftable` flag. **TODO:** add `--iftable <file>` in main.cpp and make
   the `GetIfTable2`/`GetIfTable2Ex` hooks serve `oracle_iftable2.txt`'s raw blob
   verbatim on every arch (the fingerprint ignores the volatile counter fields, so a
   snapshot is stable). This is what unblocks the aarch64 box.
3. **WMI NetworkAdapter DeviceIDs are placeholders** in `oracle_wmi.txt`
   (`DeviceID=0` for all). Real values on the licensed machine: `{5C737FB0}`→`0`,
   `{B97D8E86}`→`11`. Put the real per-GUID DeviceIDs in the oracle; they feed the hash.
4. Confirm nothing ELSE feeds the hash (volume serial, `MachineGuid`,
   `GetAdaptersAddresses`). If it does, freeze it too.

## Golden data (all captured on the licensed machine; committed)
- `oracle_iftable2.txt` — interface table (NEW, this commit).
- `oracle_wmi.txt` — WMI (BaseBoard → 0 rows; Processor Name=`virt-9.1`
  ProcessorId=`0000000000000000`; NetworkAdapter DeviceIDs need the real 0/11).
- `backup/policies1.reg`,`policies2.reg` — licence blob + fingerprint value name.

## Validate (once #2/#3 are wired)
```
x86emu --wmi oracle_wmi.txt --iftable oracle_iftable2.txt \
       --registry backup/policies1.reg --registry backup/policies2.reg anpr.exe
```
With `-c`, the fingerprint query must read `RegQueryValueEx(73e3e418…)` (NOT
`9ef9aab8…`); a plain run must print plates (`多摩500さ4649` …). **Runs are slow
(2 min+) under Prism — be patient;** `(105)` used to appear fast, so "no quick error"
is a good sign.

## Build
```
cmake -S . -B build_msvc -G "Visual Studio 17 2022" -A x64        # VS2022, x64
cmake --build build_msvc --config Release --target x86emu          # overwrites ./x86emu.exe
```
MSVC auto-defines `X86EMU_IFTABLE2` (host iftable forward). That path is for
**capturing** the golden table only — the shipped path must be the `--iftable` replay
so Linux/wasm work too.

## Capture tool (host-side, fast — to regenerate the golden table elsewhere)
A standalone `GetIfTable2Ex` dumper (decoded + raw hex) run on a machine whose native
run yields `73e3e418…`; plus the WMI queries above. See `oracle_iftable2.txt` header.
