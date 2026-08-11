# Accelerating onnxruntime under an x86 emulator (AES hook + GEMM hook)

A reusable playbook for making **onnxruntime (MLAS) inference tolerable when the guest
binary runs under a software x86-64 emulator/interpreter**. Written from the TS-ANPR
bring-up (a closed-source `tsanpr.dll` + AES-encrypted `.eon` ONNX model), but the two
hot spots below are generic to *any* onnxruntime build, so this doc is meant to be
copied into other emulator projects.

The core idea both hooks share:

> An interpreter pays ~50–150 host instructions per guest instruction. onnxruntime spends
> essentially all of its time in two tiny, hot, well-understood routines — **AES round
> functions** (model decrypt) and the **MLAS SGEMM micro-kernel** (inference). Detect the
> guest executing one of these, run the *host's* native equivalent over the guest's
> memory, and skip the interpreted body. Because both routines have a fixed, public
> contract, the native result is **bit-for-bit / numerically identical** — correctness is
> preserved, only the interpreter overhead is removed.

Two independent levers:

| Hook | Speeds up | Mechanism | Fidelity |
|------|-----------|-----------|----------|
| **AES-NI** | model **load/decrypt** | replace emulated AES round opcodes with host `AES-NI` intrinsics | bit-identical (FIPS-197) |
| **GEMM**  | model **inference** | detect the MLAS SGEMM kernel, compute the tile natively (Eigen/loops), skip the kernel | numerically identical (same float32 GEMM) |

---

## 1. AES hook — accelerate model decryption

### Why
Encrypted ONNX weights are decrypted with AES. AES decryption of a ~160 MB model executes
**hundreds of millions of AES round instructions** (measured here: **135,926,778 `AESDEC`
+ 10,485,760 `AESDECLAST`**, plus `AESIMC`/`AESKEYGENASSIST`). A software AES round is an
S-box lookup + GF(2⁸) MixColumns ≈ ~100 host ops *each*; under the interpreter that is the
dominant cost of model **load** (not inference).

### What it is
The guest doesn't call an `AES()` function you can intercept — it emits the **AES-NI
instructions** directly (`0F 38 DC/DD/DE/DF`, `0F 3A DF`). So the hook lives in the
emulator's **SSE/AES instruction decoder**, not in an import shim. Each AES-NI opcode has a
host intrinsic with *identical* semantics, so you just forward to it.

### How (this repo: `emu/src/sse.cpp`)
Guarded by `#define X86EMU_HOST_AESNI 1` (only on a real x86 host — **not** wasm/emscripten,
which lack the intrinsics; there the software path stays):

```c
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#  define X86EMU_HOST_AESNI 1
#  include <wmmintrin.h>   // AES-NI intrinsics
#endif
```

Map each opcode to its intrinsic (operands are the guest XMM registers, read/written as
`__m128i`):

| Guest opcode | Meaning | Host intrinsic |
|---|---|---|
| `66 0F 38 DB` | AESIMC (inv MixColumns for equiv-inverse key schedule) | `_mm_aesimc_si128(s)` |
| `66 0F 38 DC` | AESENC | `_mm_aesenc_si128(d,s)` |
| `66 0F 38 DD` | AESENCLAST | `_mm_aesenclast_si128(d,s)` |
| `66 0F 38 DE` | AESDEC | `_mm_aesdec_si128(d,s)` |
| `66 0F 38 DF` | AESDECLAST | `_mm_aesdeclast_si128(d,s)` |
| `66 0F 3A DF /imm8` | AESKEYGENASSIST | `_mm_aeskeygenassist_si128(s, imm8)` |

`AESKEYGENASSIST` takes an **immediate** rcon, and intrinsics require a compile-time
constant, so switch on the (small, fixed) set of rcon values the key schedule uses
(`0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36`, plus `0x00`) and call the intrinsic
with the literal.

Keep the **software fallback** compiled in under `#else` so the same source builds for
wasm and non-x86 hosts. A census counter (`X86EMU_AES_COUNT`) is handy to prove the volume
and confirm the hook fires.

### Gotchas
- Byte order: XMM state in the emulator must be laid out so `_mm_loadu_si128` over it
  matches the guest's little-endian register bytes. Load/store with `memcpy`, don't
  reinterpret struct fields.
- This is **load-time only**. It does nothing for inference — that's the GEMM hook.
- Bit-identical, so it can never change recognition output; safe to always enable on x86.

---

## 2. GEMM hook — accelerate inference (the real bottleneck)

### Why
Inference is ~99% floating-point **GEMM**. onnxruntime's MLAS runs it through one tiny
SSE2/AVX micro-kernel executed billions of times. Under an interpreter this is the
"3-hour" part. Replacing the kernel with a native matmul over the same guest memory is the
single biggest speed lever (a JIT is the other).

### Finding the kernel
Sample the guest instruction pointer during inference (a cheap periodic `rip` sampler) and
bucket by address. The SGEMM kernel shows up as **a handful of consecutive addresses inside
one ~1 KB function** eating a large fraction of samples. Its signature inner loop:

```
movss  xmm12, [rcx]        ; load one A element
shufps xmm12, xmm12, 0     ; broadcast it across all 4 lanes
movups xmm8,  [rdx-0x80]   ; load a 4-wide chunk of B
movups xmm9,  [rdx-0x70]   ; next 4-wide chunk
mulps  xmm8, xmm12 / addps xmm0, xmm8   ; multiply-accumulate into the tile
```

### ⚠ Read the disassembly — do NOT trust the mainline MLAS source
The public `onnxruntime/core/mlas/lib/amd64/SgemmKernelSse2.asm` documents *a* kernel, but
a shipped build may embed a **customized/fused variant** whose register map, packed-B
stride, tile shape, and epilogue differ. Here the mainline "rcx=A, rdx=packed-B 16-wide,
`C=Σ A[m*lda+k]·Bpacked[(n/16)…]`" model gave **~O(C) error even in row 0** — the tell that
the model is wrong at the layout level. **The only reliable method is to disassemble the
actual kernel and transcribe its exact pointer walk.** With the DLL on disk this needs no
emulator run — parse the PE, RVA→file-offset, and disassemble with capstone
(`scratch_disasm*.py` in this repo shows the pattern).

### The kernel as actually disassembled (TS-ANPR build)
The real function (entry `+0x21F1F80`, standard Win64 prologue saving xmm6–15) **dispatches
on CountM** right after moving its stack args into registers:

```
cmp r11, 3       ; r11 = CountM  ([rsp+0x118])
je  <M==3 path>  ; 0x21F2494
jb  <M<3 path>   ; 0x21F2831  (M==1, M==2 sub-paths)
   <M>=4 path falls through>
```

**So there is one code path per tile height** — analysing the fall-through (M≥4) when your
calls are M=3 is a guaranteed mismatch. Pick the path your calls actually take.

**M==3 path — output tile is 3 rows × 8 cols**, held in `xmm0..5` (2 vectors/row). Key facts
(all strides in the units shown):

| Thing | Source | Value seen |
|---|---|---|
| A base | `rdi` (`rcx=rdi`) | — |
| B base | `rdx` = `[rsp+0xf8]` = orig B **+0x80** (bias) | — |
| C base | `r8` | — |
| C row stride `ldc` | `[rsp+0x130]` bytes (store reads `rax` from here) | 204800 floats (C is a strided view) |
| B leading dim `rsi` | `[rsp+0x128]` bytes | 1152 floats |
| m-block count | `r11=[rsp+0x138]` | 2 |
| k-iter count | `r12=[rsp+0x140]` | 3 |
| A block stride `rbp` | `[rsp+0x110]` bytes | 8 floats |
| A m-block stride `r15` | `[rsp+0x120]` bytes | 2536 floats |
| K-edge bound `r13,r14` | `-[rsp+0x148]`, `[rsp+0x150]`; per m-block `r13 -= [rsp+0x158]` | — |
| store flags | `[rsp+0x180]` (edx) | 0 here |

**The exact loop (this is the native replacement, verified to err ≈ 1e-6):**

```c
float Cacc[3][8] = {0};
uint64_t rcx = A, rdx = B;          // byte pointers
int64_t  r13 = -*(i64*)(sp+0x148);
for (mb = 0; mb < r11; ++mb) {                       // m-blocks
    for (it = 0; it < r12; ++it) {                   // k-iters
        if ((uint64_t)(rcx + r13) < r14)             // K-edge mask (skip if >=)
            for (s = 0; s < 8; ++s) {                // 8 contiguous A elems / iter
                float a = *(float*)(rcx + s*4);
                for (m = 0; m < 3; ++m)
                  for (j = 0; j < 8; ++j)
                    Cacc[m][j] += a * *(float*)(rdx + m*rsi + s*0x20 - 0x80 + j*4);
            }
        rcx += rbp;   rdx += 0x100;
    }
    rcx += r15;   r13 -= *(i64*)(sp+0x158);
}
```

**Epilogue is a separate store function** (`+0x21F3BB0` for M=3), driven by the flags byte:
`bit0` → add current C (accumulate), `bit1` → add a bias/residual buffer at `[rsp+0x178]`,
`bit2` → **ReLU** (`maxps` with 0). Then `movups` the 6 accumulators to
`C[0..2][0..7]` (rows at `r8`, `r8+ldc`, `r8+2*ldc`) and `add r8, 0x20` for the next 8-col
panel. Fusing bias+activation into the GEMM epilogue is why you can't match C with a plain
matmul when those bits are set.

The contraction here is **K = r11·(8·r12) = 2·24 = 48**, walked as A-contiguous 8-float
groups with a per-m-block jump of `r15`, and gated per k-iter by the `rcx+r13 vs r14` edge
mask (the mask is exactly what makes a naive `Σ_{k<Kfull}` formula wrong at tile edges).

### Verify BEFORE you skip (mandatory)
Getting it wrong silently corrupts recognition. Shadow-verify first:

1. At kernel entry, stash all the pointers/strides/counts above (+ `C_before`).
2. At the **next** kernel entry (the real kernel has now written C), replay the loop above
   natively and compare to the guest's actual C. Report max abs error.
3. The real kernel still runs during this phase → **cannot corrupt output**. Only flip to
   replace mode once err ≈ 0 across many calls **and every M-path is transcribed**.

In this repo the harness lives in `emu/src/cpu.cpp` behind `EMU_GEMM=1`; the M=3 transcription
above verifies at **err ≈ 1e-6** (float-order noise) across every sampled tile.

### Replace mode (the speed win)
Once verified: at the kernel's function entry (`+0x21F1F80`, a clean call boundary), compute
all output tiles natively (loop the outer N/M panels the same way the kernel does), write C,
then `ret` — pop the return address and set `rip` to it, restoring `rsp`. Return value: this
kernel returns rows-handled in `rax`. Hooking the **function entry** (not the mid-function
micro-kernel) is cleaner because the ABI is a normal `call`. Handle **all CountM paths**
(1,2,3,≥4) — each has its own tile height but the same B/stride/flags model.

### Native compute options
The per-tile math is plain float32. Hand loops (as transcribed) are already ~100× fewer host
ops than interpreting. **Eigen** (`third_party/eigen_flat`, `#include "Eigen_Core.h"`) helps
for the larger outer GEMM, but note B is in the kernel's own packed/strided form — index it
with the disassembled addressing, don't assume row-major.

---

## Reuse checklist for a new onnxruntime-under-emulator project
1. Turn on the `rip` sampler; confirm two hot clusters: AES round ops (load) and a ~1 KB
   float kernel (inference).
2. Wire `X86EMU_HOST_AESNI` in the SSE/AES decoder (x86 host only). Instant model-load win,
   zero risk.
3. Locate the SGEMM kernel from the sampler, then **disassemble it from the on-disk DLL**
   (PE parse + capstone) — do not assume the mainline MLAS layout; shipped builds fuse and
   customize. Find the CountM dispatch and analyse the path your calls actually take.
4. Transcribe the exact pointer walk (all strides + the K-edge mask + the epilogue store
   flags: accumulate / bias / ReLU) and shadow-verify against real C until err≈0.
5. Flip to replace mode at the function entry (native compute + `rip`→return address),
   covering every CountM path.
6. Keep everything behind a cached flag so a normal run pays one branch — see the
   performance note below.

## Performance note (applies to every hook)
A diagnostic that calls `getenv` (or does any non-trivial check) **per guest instruction**
is ruinous — inference is billions of instructions. Gate the *entire* hook/diagnostic
block behind **one cached `static bool`** resolved once at startup; the common path is then
a single predest branch. This alone was ~10× on inference here.

---

*Status in this repo:* AES-NI hook — **done, bit-identical, shipping**. GEMM hook — the
custom kernel is **fully disassembled and the M=3 path transcribed + numerically verified at
err ≈ 1e-6** (harness in `emu/src/cpu.cpp` behind `EMU_GEMM=1`, which also dumps the pre-GEMM
profile on first fire). Remaining: transcribe the M=1/2/≥4 paths and flip to replace-mode at
the function entry. See the project memory `anpr-onnx-bringup` for live status.
