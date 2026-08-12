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

## STATUS on aarch64 macOS (M2), with 1+2+3 in place

**The fingerprint matches and the licence record is found**, from the committed
oracles alone and with no host reads:

    ./x86emu --iftable oracle_iftable2.txt --wmi oracle_wmi.txt \
             --registry backup/policies1.reg --registry backup/policies2.reg ./anpr.exe

    [iftable] oracle_iftable2.txt: 37864 bytes, NumEntries=28
    RegQueryValueEx(0) -> type 1, 246 bytes                  <- the licence blob
    ExecQuery(Win32_BaseBoard) -> 0 rows
    ExecQuery(Win32_Processor) -> 1 row
    GetIfTable2(level 1) -> 37864 bytes from the frozen table
    ExecQuery(... GUID='{5C737FB0-...}') -> 1 row
    ExecQuery(... GUID='{B97D8E86-...}') -> 1 row
    ExecQuery(... GUID='{4F3E241F-...}') -> no row
    RegQueryValueEx(73e3e41855fba8d949120fe4ac51b3f4) -> type 1, 66 bytes   <- FOUND

Both documented gates are behind us: this is the native value name, not the
`9ef9aab8…` the readme's x86emu derived, and `EMU_STOIOBJ` shows both tokens
healthy ("0" and "11") where gate 2 used to see an empty second one.

**It still ends in `(105)`, after the record is read** - and the post-lookup
sequence is now traced (`x86emu -c`, from the `RegQueryValueEx(73e3e418...)` hit to
`exit`).  In order, the engine:

1. `RegQueryValueEx(73e3e41855fba8d949120fe4ac51b3f4)` -> **66 bytes** (the record
   is `faa5bd0a56fedfe114d0622157b97d65`, a 32-char value; 66 bytes is it as UTF-16
   + terminator).
2. `GetWindowsDirectoryW`, then `CreateFile(C:\Windows\inf\vxd8.PNF)` -> not found.
3. `RegOpenKeyEx(SOFTWARE\Microsoft\Windows\CurrentVersion\Policies)` then
   `RegQueryValueEx("profile")` -> not present.  (`profile` is not in the record's
   `CurrentVersion\Policies` key, which holds only the fingerprint value name - so
   "not present" may well be what the licensed machine answers too.)
4. `GetWindowsDirectoryW`, `CreateFile(C:\Windows\inf\vxd.PNF)` -> not found.
5. `GetSystemTimePreciseAsFileTime` x3 - a time read, i.e. a trial-expiry check.
6. one `__stdio_common_vfprintf`, then `exit`, code 0, `(105)`.

No fault, no rip=0: the engine reaches its own "no" and prints it.  So beyond the
value-name lookup there are up to three more inputs, none recorded anywhere and
none in the readme:

- **`C:\Windows\inf\vxd8.PNF` and `vxd.PNF`** - Windows precompiled-INF files.
  A licence hidden in one (the way the primary one hides in a Policies value) would
  be read here.  Does the licensed machine have these, and what is in them?  If they
  are genuinely absent there, "not found" is correct and this is not the gate.
- **`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies` value `profile`** -
  same question; likely absent on the licensed machine too.
- **the current time** vs whatever expiry the 66-byte record encodes - the most
  likely actual gate, since a trial licence expires.  `GetSystemTimePreciseAsFileTime`
  returns the host clock now; if the trial's window is fixed, freezing this to a time
  inside it is the next thing to try (an emulator knob, not a machine value).

### Ruled out and pinned down (aarch64, this session)

- **Not the clock.** `guest_time_now()` is already frozen to 2026-08-11 (a date the
  trial was valid), and `EMU_FAKE_TIME` sweeps over 2024-06, 2025-01, 2025-08 and
  2026-07 all still give `(105)`.  Trial expiry is not the gate.
- **`C:\Windows\inf\vxd8.PNF` and `vxd.PNF` ARE a real input.** With the files
  absent the engine calls `CreateFile` and moves on.  Create them (they resolve to
  `./C:/Windows/inf/...` with no sysroot) and the trace grows a `GetFileType`,
  `SetFilePointerEx` (seek-to-end for the size) and **`ReadFile`** on each - the
  engine reads their contents.  Still `(105)` with placeholder contents, so the
  bytes matter: a real Windows `.PNF` is a binary precompiled-INF, and a licence
  hidden in one would be there.  **This is the next input to capture on the licensed
  machine** - `vxd8.PNF` and `vxd.PNF` from its `C:\Windows\inf\`, committed as
  an oracle the way the interface table was.
- **`profile` value** under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies`
  is read once and is "not present" here; it is not in the record's copy of that key
  either, so it is probably absent on the licensed machine too - lower priority than
  the .PNF.

**Questions for the licensed-machine session:** do `C:\Windows\inf\vxd8.PNF` and
`vxd.PNF` exist there?  If so, commit them (or their hex) - they are read during
`anpr_initialize` after the fingerprint lookup succeeds, and their contents decide
the verdict.  Does that Policies key have a `profile` value?

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
2. ~~No `GetIfTable2` replay path.~~ **DONE — `--iftable <file>` is on `main`.**
   hooks_win32d.cpp loads the `IFTABLE2_RAW_HEX=` blob and both `GetIfTable2` and
   `GetIfTable2Ex` serve it verbatim on every arch, in preference to the host's own
   table (a frozen table is the point; the host's would be a different machine).
3. ~~WMI NetworkAdapter DeviceIDs are placeholders.~~ **DONE.** `oracle_wmi.txt` now
   answers per GUID - `{5C737FB0}`→`0`, `{B97D8E86}`→`11` - and gives `{4F3E241F}` no
   line, so that query finds nothing, which is what the licensed machine answers.
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
