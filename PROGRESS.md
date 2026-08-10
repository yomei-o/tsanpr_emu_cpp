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

## Gate 2 — post-lookup validation — ROOT CAUSE FOUND AND FIXED ✅

**ROOT CAUSE: the emulator's `GetStringTypeW` hook was a stub** —
`win32("GetStringTypeW", 4, [](Emulator& e){ e.set_result(1); })` returned success
but never wrote the `lpCharType` output buffer, so the static-CRT `num_get` read
stack garbage (e.g. `0xF948`) as the character-type flags for `'1'`. The correct
`CT_CTYPE1` flags for `'1'` are `0x0284` (`C1_DEFINED|C1_XDIGIT|C1_DIGIT`); the
garbage had **no `C1_DIGIT` bit**, so `num_get` treated `'1'` as a non-digit,
parsed **zero digits**, and the license token came out empty → `std::stoi("")`
threw → `(105)`. Fixed in `emu/src/hooks_win32.cpp` by bridging `GetStringTypeW`
to the host API (reads the guest source chars, calls the real `GetStringTypeW`,
writes the real classification WORDs back to guest memory; portable fallback
classifies ASCII for number parsing). After the fix `anpr_initialize()` no longer
returns 105 and anpr proceeds to plate recognition.

How it was found (the differential-tracing chain that led here):
- Captured 60k-instruction traces of the validation from 0x14A6EAF in both the
  emulator (`EMU_ITRACE`) and native (`ndbg --trace … 2c34418 1`, poking
  `__isa_available`→1 so both take the SSE2 CRT paths), kept only app-logic RVAs,
  and diffed with a **resync** step to skip benign divergences.
- The real divergence bottomed out at a `num_get` parse loop whose per-character
  decision is a `ctype::is`-style test: `0x14B1780` → `0x150D1AC` calls
  `GetStringTypeW` and `test bx, ax; setne al`. For `'1'` the classification word
  `ax` was `0x0284` in native but `0xF948` in the emulator — i.e. the emulator's
  `GetStringTypeW` was returning garbage, which is the stub above.
- `EMU_SLICE` confirmed the downstream effect in one run: the 4th conversion's
  source `"1"` became an empty conversion input (the parse consumed 0 chars),
  while native kept `"1"`.

### (historical) earlier trace notes that led to the fix

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

Ruled out (none is the cause): the emulator's `pcmpeqw`/`pmovmskb`/`pmaddwd`
(correct); locale APIs (`GetLocaleInfoW`/`localeconv` are not even called — the
DLL uses its own static-CRT locale); the WMI→BSTR path (the DeviceID comes back
as a correct BSTR "1", byte-length prefix and all); the `'/'`-vs-`0` byte at
`[rbp-0x69]` (passed to a string-concat at `+0x146DF30` that overwrites the arg —
unused); the wcslen mask `0xfff0`-vs-`0xc0f0` (garbage past the terminator, both
length 2). All of these are benign.

What is actually true: `std::stoi`'s token is `"1"` in native and `""` in the
emulator. Native's value is the network DeviceID `"1"` (WMI returns it correctly
to the emulator too). Somewhere in the *guest-side string processing* between the
correct WMI BSTR and the `std::stoi`, the emulator turns `"1"` into `""` — but no
GP or XMM register (0-15) diverges through the traced region, so it is neither a
control-flow nor an obvious ALU/SSE difference. It is a subtle,
data-/memory-dependent execution infidelity.

- Built `ndbg` step-over (skip non-DLL excursions) so native traces reach deep
  code; the license validation `0x14A4F50/0x14ED7A0` is a long loop over the
  region vocabulary (Japanese/Korean place names, EU country codes) plus machine
  properties, and the token `std::stoi` is >8000 instructions in.
- First real (non-benign, non-pointer) divergence in the aligned SSE2 trace: at a
  `wcslen` (tsanpr.dll+0x204B6EE) the length register `r9` is **4 in native but 6
  in the emulator**, measuring the BaseBoard Product "L140MU". Both sides read
  "L140MU" (6) from WMI, so the correct behaviour null-terminates it to "L140" (4)
  at this stage and the emulator fails to; i.e. a buffer-content divergence.
- WALL: native and emulator execute the SAME rip sequence with NO register
  divergence right up to that point, yet the buffer content differs — so the
  divergent write happened inside a region the diff can't see: either a non-DLL
  excursion the step-over skips (e.g. ntdll RtlCopyMemory), or a memory-to-memory
  store whose value never passes through a logged register. Combined with native
  ASLR (can't re-read native heap post-hoc) and loop-coincidental rip alignment
  (data divergences desync while rips still match), the empty-token root resists
  every isolation method tried (instruction/XMM diff, object/string-content diff,
  data/read watchpoints).
- Next: instrument the emulator to log the ACTUAL bytes of every wmemcpy/memcpy
  and every std::string assign into the property buffers during validation, and
  diff those byte streams against a native capture — i.e. capture the data flow
  directly rather than inferring it from register/rip traces. Or obtain the
  fingerprint/validation spec from TS-Solution.

### Refined & partly-corrected picture (latest session)

Dumping the actual `std::wstring` object at the `wcstol` call (`EMU_STOIOBJ` at
0x14ED076) corrected the earlier read:

- `std::stoi` is called **twice**. #1: size=1 cap=7 inline `"1"` → succeeds.
  #2: size=0 cap=7 inline `""` → throws. So the DeviceID→`"1"` conversion is
  **not** the failure (the first token is a correct `"1"` in the emulator too),
  and it is **not** an SSO size-vs-data mismatch — the 2nd token's wstring is
  **genuinely empty** (size 0). The empty one is the SECOND token.
- WMI is **faithful**: with `x86emu -c`, every property the guest reads is
  correct and non-empty in *both* collection passes — Product `"L140MU"`,
  SerialNumber `"Not Applicable"`, Name `"11th Gen…i7-1165G7"`, ProcessorId
  `"BFEBFBFF000806C1"`, DeviceID `"1"` (all VT_BSTR). So the empty token is
  produced by guest-side string processing, not a missing input.
- **ISA-alignment breakthrough for diffing.** The first control-flow divergence
  at 0x204B60C is exactly `cmp __isa_available,5; jbe` — native takes the AVX-512
  path (=6), the emulator the SSE2 path (=1). Poking native's `__isa_available`→1
  (`ndbg --trace 14a6eaf 7000 2c34418 1`) puts BOTH on the SSE2 path; the
  first divergence then moves from aligned step 656 all the way to **step 5391**,
  and that one is a benign `wcslen` alignment-prologue artifact (`start & 0xf`
  differs because the string lives at different addresses, so the scalar
  pre-loop runs a different number of iterations before the SSE body — same final
  length). All 7 measured strings in the window are byte-identical. So control
  flow matches; the divergence is a data value, and register diffs on the aligned
  prefix are swamped by heap-allocator internals (0x206Bxxx) that legitimately
  differ between the emulator heap and the native heap.
- Caller chains at the two stoi calls (stack-walk in `EMU_STOIOBJ`): both go
  through the number-parse helper at **0x14B1796**; the empty (2nd) call has an
  extra frame ~**0x2066C4D** (an IPP/CRT-region routine). The gate-2 validation
  helper 0x14A4F50 takes a static descriptor whose embedded `std::wstring` is
  `"License"` and builds a small (0x90-byte) parse facet from `.rdata` constants.
- STATE: the empty 2nd token is a subtle data-/heap-dependent execution
  infidelity that does not surface as any clean register/RVA/measured-string
  divergence once ISA paths are aligned. Reaching the exact mis-executed
  instruction needs either byte-level data-flow capture into the token buffer, or
  the vendor's validation spec. New env probes this session: `EMU_STOIOBJ`
  (wstring object + caller stack-walk at the wcstol), `EMU_VALSRC` (tokenizer
  input at 0x14A6EAF).

### Exact divergent instruction pinned — 0x14BEBF4 (resync-diff)

Captured 60k-instruction `IT rva+16regs` traces of the validation call
(from 0x14A6EAF) in BOTH emulator (`EMU_ITRACE`) and native (`ndbg --trace …
2c34418 1`, poking `__isa_available`→1 so both take the SSE2 CRT paths), kept
only app-logic RVAs (`< 0x1a00000`, dropping the IPP/CRT/heap-allocator region
whose internals legitimately differ between the two heaps), and diffed with a
**resync** step (skip to the next 3-in-a-row re-alignment on mismatch). That
filters every benign divergence — the `__isa_available` dispatch (0x204B60C), the
`wcslen` alignment prologue (0x204B6E4), and a `std::wstring` overlap check
(0x14A54CE, where emu and native reach 0x14A54ED by different branches but both
end rdi=rsi=7). The first **non-re-syncing** divergence is:

**tsanpr.dll+0x14BEBF4** — `cmp rdx,r8 ; je 0x14BED38`, where `rdx`=start and
`r8`=end of the character range fed to a boost::lexical_cast-style stream/num_get
number conversion:
- **Emulator:** `rdx==r8` (both 0xAFD630) → **empty range** → takes the branch to
  0x14BED38 and converts **0 characters** → the token is an empty `std::wstring`
  → `std::stoi("")` throws → (105).
- **Native:** `rdx=…3B0, r8=…3B2` → a **1-wchar range `"1"`** → falls through to
  the loop body 0x14BEBFA and converts `"1"` → stoi returns 1 → passes.

Both ranges are stack buffers; their endpoints are produced by the enclosing
function **0x14BEF70** (facet/iterator virtual calls, refcount incs at 0x1482060).
So the emulator hands the number-conversion an **empty input** where native hands
it `"1"`. That is the mechanical cause of the empty stoi token.

- Next: step up into 0x14BEF70's stream/facet setup to find why the emulator
  leaves the put-area/range empty (length 0 vs 1) — most likely a virtual-dispatch
  or facet-read infidelity in the boost lexical_cast stream. Trace artifacts:
  `scratch_bigtrace.log` (emu) / `scratch_bignat.log` (native).

Followed it one more level (register diff of the aligned 0x14BEF70 window +
`EMU_REGS` memory dumps):

- At **0x14BEFC6** (`mov rax,[rsi]`, rsi=wstring+0x10) the value read is the
  **std::wstring SIZE**: `rax=0` in the emulator, `rax=1` in native. Then
  `0x14BEFC9 lea r8,[rbx+rax*2]` makes the end pointer, so emu's range is empty
  and native's spans one wchar. So the *source* wstring handed to the conversion
  has **size 0 in the emulator, size 1 in native**.
- `EMU_REGS` dump of that source wstring (emu addr 0xAFD630) shows it is
  **genuinely empty** — the inline SSO bytes are zero, NOT "1" with a stale size.
  So this is not an SSO size-vs-data mismatch; the value really is empty.
- The conversion runs several times in sequence; dumping the *sliced* source
  (`EMU_REGS=1415bf16b`, rsi) shows the machine properties flowing through:
  "L140MU", "Not Applicable", "11th Gen…i7-1165G7", "BFEBFBFF000806C1", and "1".
  The wstring at 0xAFD630 holds "1" (size 1) at the slicer 0x14BF16B but is empty
  by the converter 0x14BEFC6 — i.e. the **substr/extraction in 0x14BF160-0x14BF1A1
  slices ZERO characters out of the source "1" in the emulator** (a std::wstring
  find/offset where the emulator's pointer math lands one wchar off), producing
  the empty conversion input.
- CAVEAT: at this depth the two runs' loop iterations can misalign (earlier benign
  desyncs), so the exact "which pointer is wrong" in 0x14BF160 needs a same-run
  capture (arm a watch on the specific slice at its known address) before naming
  the single mis-executed emulator instruction. Solid, run-robust facts: the
  0x14BEBF4 empty-range branch; size 0-vs-1 at 0x14BEFC6; the source wstring is
  genuinely empty (memory-dumped); the token is the 2nd/validation stoi.

### Single-run proof + the operation is a trailing-TRIM (env EMU_SLICE)

`EMU_SLICE` logs, in ONE emulator run, the source wstring at the slicer
(0x14BF16B) and the conversion-input wstring at the converter (0x14BEFC6) with a
seq#, plus the length math at 0x14BF160. Result — the converter runs on four
values (L140MU, "1", L140MU, "1"); the first three come out unchanged, the 4th is
`SRC 0xAFD630 "1"` → `CONV 0xAFD630 ""` (same buffer, emptied). The length probe
shows the operation is a **trailing trim/erase of `len` chars**, where
`len = (rbx - [rsp+0x90]) / 2`:
- all working conversions: `rbx == [rsp+0x90]` → **len 0** → nothing trimmed →
  string kept (so L140MU and the first "1" survive);
- the failing 4th: `rbx=0xAFD632, [rsp+0x90]=0xAFD630` → **len 1** → trims the one
  and only char off "1" → empty. Native computes len 0 here (keeps "1").

So the bug is NOT the length subtraction itself — it is the **end pointer `rbx`**,
which the emulator sets to `0xAFD632` (one wchar past "1") where it should equal
the begin pointer `0xAFD630`. That `rbx` is produced just above by a std/boost
**stream-iterator two-level virtual-call sequence** at 0x14BF120-0x14BF13F
(`mov rax,[rcx]; call [rax+0x10]` then `mov r8,[[rax]]; call r8` — num_get /
istreambuf_iterator advance+deref). So the emulator's stream iterator lands its
end/EOF position one wchar off for this 4th value, making the trim erase the digit.

- Next (narrow): trace INTO the 0x14BF129 / 0x14BF13F virtual calls to find the
  specific mis-executed routine that returns the off-by-one iterator/end pointer
  (candidate: an SSE string/`char_traits` compare or a facet's get). That single
  routine is the emulator infidelity to fix. Probe: env `EMU_SLICE`.

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
