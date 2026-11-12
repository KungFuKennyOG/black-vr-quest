# arm64-fault: MMIO fault emulation on AArch64 Linux/Android

Stream owner: arm64-fault agent. Nothing committed. All paths are relative to /root/black.

## Summary

- New portable AArch64 load/store decoder and emulator, plus Linux/Android `ucontext_t` glue and a SIGSEGV dispatcher: `third_party/xboxrecomp/src/platform/arm64_fault_decode.{h,c}`.
- The MMIO hooks have an `#elif defined(__aarch64__)` path: `mmio_decode.h` (which OHCI and Kelvin use), `nv2a_mmio_hook.c`, `apu_mmio_hook.c`, `ohci.c` and `memory_watch.c`.
- **The x86-64 builds are unchanged.** I preprocessed every touched file from both the original and the edited tree for `x86_64-w64-windows-gnu` (mingw headers, `_WIN32`) and `x86_64-linux-gnu`. All 10 file/target pairs give the same token stream (md5): nv2a, apu, ohci, memory_watch and kelvin_mmio.
- Tests are in `tests/arm64_fault_decode/`:
  - Native decode test: 183,812 checks, 0 failures (gcc ASan+UBSan).
  - The same decode test as a static arm64 binary under `/root/xtool/arm64run`: passes.
  - **End-to-end arm64 fault test**: real SIGSEGV on a PROT_NONE aperture, through the dispatcher and `mmio_emulate`, then resume. 76 faults serviced, 54 checks, 0 failures. Passes at -O0, -O1, -O2, -O3 and -Os, and on the cortex-a76, neoverse-n1 and max (SVE) CPU models.

## 1. What the x86-64 path does today

There are three copies of the x86-64 decoder:
- `platform/mmio_decode.h` `mmio_emulate()`, used by `usb/ohci.c` and `nv2a/kelvin/kelvin_mmio.c`
- `nv2a_mmio_hook.c` `decode_and_handle()`
- `apu_mmio_hook.c` `apu_decode_and_handle()`

All three are `_WIN32`-only. On POSIX, `AddVectoredExceptionHandler` is a stub that never fires (`win32_compat.c:1915`), `PCONTEXT` is `void *`, and the NV2A/APU/OHCI/watch paths are stubs or `#if _WIN32`.

Common decode: prefixes 66 (operand size 2), F2/F3 (skipped) and REX 40..4F (W selects 8 bytes, R extends reg, B extends rm). ModRM length is 1 byte, plus a SIB byte when rm&7==4, plus disp32 for mod0/rm5, disp8 for mod1, disp32 for mod2. RIP advances by prefix + opcode + ModRM length (+ immediate). The device offset is always the fault VA minus the device base, taken from `ExceptionInformation[1]`. The decoder never computes an address itself.

| Form | mmio_decode.h | nv2a hook | apu hook | Semantics |
|---|---|---|---|---|
| 88/89 MOV r/m,r | yes | yes | yes | write reg; size 1, or 2/4/8 from 66/REX.W. nv2a masks the value to the size; apu/mmio_decode pass the full 64-bit reg |
| C7 MOV r/m,imm32 | yes | yes | yes | write the imm32 at the operand size (REX.W: 8 bytes, imm **not** sign-extended) |
| C6 MOV r/m8,imm8 | yes | yes | yes | 1-byte write |
| 8A/8B MOV r,r/m | yes | yes | yes | read; 1/2 bytes merge into the low bits, 4 bytes zero-extends, 8 bytes full. No AH..BH special case |
| 0F B6/B7 MOVZX | yes | yes | yes | read 1/2 bytes, zero-extend to 64 |
| 0F BE/BF MOVSX | yes | - | - | read 1/2 bytes, sign-extend, written at the operand size (high-byte regs handled) |
| 84/85 TEST r/m,r | yes | yes | yes | read; ZF/SF set, CF/OF cleared |
| 38/39/3A/3B CMP | yes | yes | (38/39) | read; ZF/SF/CF (+OF in mmio_decode) |
| 00-03 ADD, 08-0B OR, 20-23 AND, 28-2B SUB, 30-33 XOR | yes (both directions) | OR, AND r/m,r only | OR, AND r/m,r only | RMW: read, ALU, write the device or the register; flags |
| 80/81/83 ALU r/m,imm | yes (ADC/SBB refused) | - | - | RMW or CMP with an imm8/16/32, sign-extended |
| F6/F7 /0 TEST r/m,imm | yes | - | - | read, flags |
| 0F BA /4 BT r/m,imm8 | yes | - | - | read, CF = bit |
| anything else | return 0 | log the first 20, return false | log the first 20, return false | |

The `memory_watch.c` VEH is not a decoder. It uses a guard page plus the trap flag (EFlags |= 0x100), then re-arms on SINGLE_STEP.

Where the faulting address comes from: `ExceptionInformation[1]` (host address), and `[0]` gives read/write. Callers compute `xbox_va = fault - g_xbox_mem_offset`.

## 2. AArch64 decoder (`arm64_fault_decode.{h,c}`)

It is pure C with no OS headers outside the `__aarch64__ && !_WIN32` glue section, so it builds with gcc, clang and the NDK at `-Wall -Wextra -Werror`.

`a64_decode(insn, &a)` reports: kind, size (1/2/4/8/16), count (2 for pairs), is_load, is_write, is_fp, is_signed, reg_bits (32 = Wt with the upper half cleared, 64 otherwise), rt, rt2, rn (31 = SP), rm (31 = XZR), rs, ext (UXTW/LSL/SXTW/SXTX), shift, imm (already scaled), writeback, post_index, acquire, release, exclusive and atomic_op.

Decoded forms:
- LDR/STR with an unsigned immediate, every size, including LDRB/LDRH/LDRSB/LDRSH (to W and X)/LDRSW/STRB/STRH and B/H/S/D/Q.
- Unscaled LDUR/STUR (all sizes), pre-index, post-index, and LDTR/STTR.
- Register offset with UXTW, LSL, SXTW or SXTX, and the scaled shift.
- LDP/STP/LDNP/STNP/LDPSW in W, X, S, D and Q, with signed offset, pre-index or post-index.
- LDAR/STLR/LDLAR/STLLR in B/H/W/X, and LDAPR/LDAPUR/STLUR (RCpc).
- LSE LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN (and the ST* aliases), SWP and CAS, each with the A/L variants.
- LDXR/STXR/LDAXR/STLXR. These are decoded, but **the MMIO path refuses them** (see below).

Refused forms: prefetch, LD1..LD4 and the lane/replicate forms, LDXP/STXP/CASP, MTE STG/STGP/LDG, PAC LDRAA/LDRAB, LD64B, literal loads, SVE, and the CONSTRAINED UNPREDICTABLE forms:
- writeback where the base is also a data register
- load pairs with Rt == Rt2
- STXR where Rs == Rt or Rs == Rn

Register 31 is XZR/WZR for Rt, Rt2, Rm and Rs (loads into it are discarded, stores from it write 0) and SP for Rn.

Execution:
- **`a64_execute`** takes a register file (`x[31]`, `sp`, `pc`, `v[32]` low 64 bits, plus `v_hi[32]` for Q) and callbacks `read(addr, size)` / `write(addr, size, val)`. A Q register becomes two 8-byte accesses. It applies sign/zero extension, clears the upper half for W, and clears bits 64-127 of V for FP loads narrower than Q. Both halves of an LDP are read before either register is written, so `ldp x1, x0, [x0]` is correct. It then applies writeback and sets pc += 4.
- **`a64_emulate_mmio`** works in device terms with the same `(dev, off, size)` callbacks as `mmio_decode.h`. Each element's offset is derived from its own address relative to the fault address. That matters because the fault can be reported on the second element of an LDP/STP, and a Q register spans two offsets. Top-byte (TBI) tags are ignored. Before touching the device it refuses if:
  - the fault is outside the instruction's access, or
  - the access begins below register 0 (an LDP straddling RAM and the aperture), or
  - with `dev_size` set, the access runs past the end of the device.

**Exclusives are refused in the MMIO path.** The e2e test found an infinite spin: an emulated LDAXR does not arm the CPU's exclusive monitor (and the exception return clears it anyway), so the real STLXR fails without faulting and the loop retries forever. The fix is to refuse, which gives a named crash instead of a hang. Build with `-march=armv8.1-a` or later (Quest 3 is v8.2) so that atomics compile to LDADD/CAS/SWP, which are handled. Note that at default flags the NDK emits `__aarch64_*` outline-atomic calls, which use LSE at runtime on the Quest. `memory_watch.c` uses `a64_execute` directly; it would see an LDXR, but that is a diagnostic, and the same caveat applies.

## 3. Glue (`__aarch64__ && !_WIN32`)

- **`a64_uc_get` / `a64_uc_put`** map `uc_mcontext.regs[0..30]`, `sp` and `pc`. V0..V31 come from the `fpsimd_context` record (FPSIMD_MAGIC 0x46508001) found by walking `__reserved`, including the `extra_context` spill. Every record is bounds-checked. If the kernel saved an `sve_context` that holds register data, Z0..Z31 are read and written as well, because the kernel then restores from SVE and ignores fpsimd. Only the V registers that a load actually wrote are written back (`a64_fp_dest_mask`), so the SVE upper bits of every other register are preserved. FP accesses are refused if no FP record exists or the thread is in SME streaming mode.
- **`a64_uc_emulate_mmio(uc, fault_addr, off, dev_size, dev, rd, wr)`** is the whole job of a handler: decode, check, emulate, write back, step.
- **`a64_uc_access_is_write(uc)`** is the counterpart of `ExceptionInformation[0]`. It is derived from the instruction, not from ESR. The coordinator noted that QEMU emits no esr_context while real kernels do; this way the result does not depend on that.
- **`a64_fault_install()` / `a64_fault_add_handler(h)`** form a SIGSEGV dispatcher, standing in for `AddVectoredExceptionHandler` (a no-op stub on POSIX). The most recently added handler runs first. Flags are `SA_SIGINFO | SA_NODEFER | SA_RESTART`. NODEFER lets a device model that touches another trapped aperture nest, as it does under VEH. There is no SA_ONSTACK, because device models can need more stack than bionic's alternate stack. An unclaimed fault is chained to the previous handler (debuggerd, so the tombstone still happens), otherwise SIG_DFL. At most 16 handlers, and they cannot be removed.
- **`mmio_decode.h`** gains an `#elif defined(__aarch64__)` `mmio_emulate(ctx, off, dev, rd, wr)` with the same signature, where ctx is the ucontext. It takes the fault address from `uc_mcontext.fault_address`. AArch64 has no memory-operand ALU, TEST, CMP or BT, so flags need no emulation. **`kelvin_mmio.c` and `ohci.c` therefore need no decoder changes.** (Kelvin is still `<windows.h>`/SRWLOCK and Windows-only for other reasons.)
- **`nv2a_mmio_hook.c`**: aarch64 versions of `nv2a_hook_init`, `nv2a_hook_handle_mmio` (via `a64_uc_emulate_mmio`, bounded to 16 MB, with decode-fail logging) and `nv2a_hook_handle_vram`. The `fault_addr` argument must be the real host fault address.
- **`apu_mmio_hook.c`**: an aarch64 `apu_hook_handle_mmio`, bounded to 512 KB. The trace prints the direction taken from the instruction.
- **`ohci.c`**: an aarch64 `xbox_OhciHandleMmio`, a dispatcher handler `ohci_a64_fault`, and protection of both 4 KB blocks in `xbox_OhciInit`. On aarch64 it also starts `ohci_thread`, as Windows does; without it the controller never runs frames. All of this is gated by `RECOMP_USB` as before.
- **`memory_watch.c`**: there is no user-mode single-step, so the page stays PROT_NONE. Each access is logged, then the page is opened, the instruction is emulated against real memory with `a64_execute`, and the page is closed again. Symbols come from `dladdr`.

Instruction length and PC math: AArch64 is fixed at 4 bytes and pc += 4 happens only on success. None of the callers do PC arithmetic. The fault address is si_addr (the kernel strips the tag), and `a64_emulate_mmio` masks tags on the base register too.

## 4. Tests (`tests/arm64_fault_decode/`)

- **`gen_vectors.py`** produces `vectors_gen.h`, which is checked in. It uses the NDK's clang and llvm-objdump:
  - About 360 curated assembly lines covering every form above, plus 50 must-refuse words: prefetch, LD1, LDXP, CASP, STGP, LDRAA, LD64B, literals, non-memory instructions, and hand-encoded UNPREDICTABLE/unallocated words.
  - `accessor_sample.c`: the runtime's MEM8/16/32, SMEM*, MEMF/MEMD, STK*, the XMM lane fallback, rep-movs loops, RMW, polls, memcpy accessors and atomics. It is compiled at -O0/1/2/3/s/z × {default, armv8.2-a, +rcpc, -mno-outline-atomics, cortex-a78c} = 30 configurations, giving 9,076 load/stores and 368 distinct words. **Every one decodes and matches.**
  - Expected fields are parsed from objdump's text, independently of the decoder. The four words from the task statement (B9400020, B9000420, 39400062, 29400440) are vectors 1-4.
- **`test_decode.c`** (native):
  - field-by-field match for every vector
  - each accepted vector executed four times with random registers against a recording memory, checking address, access count and size, stored values, the loaded register with sign/zero/W/XZR semantics, untouched registers, writeback and pc
  - hand-written edge cases: LDP into its own base, S/Q clearing V, LDRSB W vs X, LSE ops including signed versus unsigned max, CAS success and failure, STXR status, SP pre/post-index, and the MMIO adaptor (LDP faulting on the 2nd element, STP Q halves, TBI, every refusal path leaving state untouched)
  - a 65k-word sweep
- **Mutation check.** I injected 20 single-line decoder bugs (wrong SXTW, post-index address, the W upper half, XZR, pair scale, Q high half, the shift, the Q-size guard, the LDAPR fields, SMAX signedness, STXR status, straddle and offset derivation, and others). **The suite catches every one.** Two survived the first pass; I added vectors for them.
- **`test_fault_e2e.c`** (arm64 only):
  - Sets up a 4 GiB PROT_NONE reservation with RAM opened at the bottom and two model devices at 0xFD000000 and 0xFED00000.
  - Exercises inline-asm forms (B/H/W/X, signed, XZR, pre/post-index, LDP/STP, S/D/Q including clearing and preservation of V, LDAR/STLRB, LDADDAL/CASAL/STSET/SWPAL) and the runtime accessor macros as compiled C (each poll read reaches the device exactly once).
  - Checks the declined paths: a non-device fault reaches the previous handler, LD1 is declined, LDAXR is declined, and an LDP straddling below the device base is declined.
- **`CMakeLists.txt`**: natively it runs `arm64-fault-decode`. With `-DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake` it also runs `arm64-fault-e2e` through the emulator. Both pass under ctest.

## 5. Integration still needed (other owners' files)

1. **Build**: add `arm64_fault_decode.c` to the `platform` library for Android, by adding it to the `add_library(platform STATIC ...)` list in `src/platform/CMakeLists.txt`, at least under aarch64. Until then, aarch64 links of nv2a, apu, usb and kernel fail with undefined `a64_*`. I did not edit that file because it is outside my scope.
2. **Game-side fault handler** (`src/main.c` `veh_handler` is Windows VEH): on Android, register this with `a64_fault_install()` and `a64_fault_add_handler()` after `xbox_MemoryLayoutInit`:
   ```c
   static int apu_fault(void *uc, uintptr_t fault) {
       uint32_t va = (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset);
       return va >= 0xFE800000u && va < 0xFE880000u
           && apu_hook_handle_mmio(uc, fault, va, 0);
   }
   ```
   Do the same for NV2A if its aperture is ever trapped. Today it is plain RAM in `xbox_memory_layout.c`.
3. `xbox_memory_layout.c` traps the APU with `VirtualProtect(PAGE_NOACCESS)`, which is `mprotect` on POSIX, so it works unchanged. OHCI does its own protect.
4. If anything else on POSIX relies on `AddVectoredExceptionHandler`, it should move to `a64_fault_add_handler`. The win32_compat stub is unchanged.

## 6. Verified vs. unverified

Verified here:
- decoding of every compiler-emitted accessor form
- semantics on the abstract register file
- real SIGSEGV delivery with the bionic rt_sigframe under QEMU linux-user (cortex-a76, neoverse-n1, max): reading and writing regs, sp, pc and fpsimd vregs, and resuming
- chaining to the previous handler
- NDK -O2 compilation of every edited file with zero warnings (needs sse2neon shims for x86 intrinsics headers pulled in by kernel.h and recomp_types.h, which is another stream's problem)
- x86-64 preprocessed output unchanged

Only on device:
- **SVE Z-register writeback.** It is implemented, but under QEMU max the thread never used SVE, so the record was probably header-only. Quest 3 (Cortex-A78C) has no SVE, so this path should be dead there anyway.
- **Real kernel frames carrying esr_context and extra_context.** The walker handles them, but they were not exercised.
- **ART/libsigchain.** In an app process, `sigaction` goes through libsigchain. ART claims faults in its own code first and then calls ours. This should work but is unverified; with NativeActivity, test that MMIO faults reach the dispatcher and that unclaimed faults still produce a tombstone.
- **Fault cost.** Each register access is a kernel round trip, so measure DirectSound/APU polling rates on the Quest.
- **Page size.** The trapped OHCI 4 KB blocks assume 4 KB host pages, which the Quest 3 kernel uses. On a 16 KB-page kernel, neighbouring MCPX registers would also be trapped and served by the OHCI model.
- `memory_watch.c` aarch64 mode: compiled, but not run end to end.
- The aarch64 `nv2a_hook_*`, `apu_hook_handle_mmio` and `xbox_OhciHandleMmio` bodies are compiled but not exercised against the real device models. The e2e test drives the same `mmio_emulate`/`a64_uc_emulate_mmio` entry points with a model device.

## Risks and open issues

- **Exclusives against device registers are refused.** If title code ever compiles a device access to LDXR/STXR (only plain-v8.0 builds without outline atomics would), the process crashes with a decode message instead of hanging. Build v8.1 or later.
- **No SA_ONSTACK.** A stack overflow fault will not reach the dispatcher (the kernel cannot build the frame), and goes straight to the default action.
- **Concurrency.** Handlers run on the faulting thread with no lock in the dispatcher, the same as VEH. Device models need their own locking, as on Windows (the Windows NV2A and APU stats counters are not atomic either).
- **Q-register accesses arrive at the device as two 8-byte calls**, and LDP/STP as two element calls. Device models that only handle 4-byte accesses must split 8-byte ones, as kelvin's `access_write` already does.
- **OHCI on aarch64 now starts `ohci_thread`.** This is still behind `RECOMP_USB`. The thread needs `xbox_AllocThreadTib` and the generated runtime, the same as Windows.
