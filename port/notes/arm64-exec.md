# arm64-exec: running aarch64 (Quest 3) code in the x86_64 container

Status (2026-11-02): WORKING. QEMU linux-user built from source; static NDK binaries run, including
SIGSEGV handlers with full arm64 rt_sigframe/ucontext + rt_sigreturn. Unicorn / custom loader not needed.

## Pieces
| Path | What |
|---|---|
| `/root/xtool/qemu/bin/qemu-aarch64` | QEMU v10.1.5 linux-user, aarch64 only, static-pie (no runtime deps) |
| `/root/xtool/arm64run` | wrapper: `arm64run ./prog [args]` -> exit code / stdio / signals passed through |
| `/root/xtool/android-arm64-static.cmake` | NDK r27c toolchain + `-static` executables + `CMAKE_CROSSCOMPILING_EMULATOR=arm64run` |
| `/root/xtool/arm64-tests/` | self-tests (`hello neon malloc segv io` .c, `cmake/` ctest project) |
| `/root/deps/qemu`, `/root/deps/qemu-b2` | QEMU source (shallow, tag v10.1.5) and build dir |

## Usage
```sh
. /root/xtool/env.sh
$ANDROID_CC -static -O2 -o t t.c -lm && /root/xtool/arm64run ./t args; echo $?

# CMake / ctest (add_test(NAME x COMMAND x) is auto-wrapped with arm64run):
cmake -G Ninja -S . -B build-a64 -DCMAKE_TOOLCHAIN_FILE=/root/xtool/android-arm64-static.cmake
cmake --build build-a64 && ctest --test-dir build-a64 --output-on-failure
```
arm64run env knobs: `ARM64RUN_CPU` (default `cortex-a76`, ARMv8.2 + fp16/dotprod, no SVE, close to
Quest 3's cores), `ARM64RUN_STRACE=1` (guest syscall log), `ARM64RUN_GDB=1234` (wait for gdb on port),
`ARM64RUN_ARGS` (raw qemu options).

Only STATIC executables run: the NDK has no runtime `/system/bin/linker64` or bionic `.so`s. Test shared
code (e.g. what goes into libmain.so) by linking its static lib / objects into a static test exe.
SHARED libraries still build normally with the toolchain file; only executables get `-static`.

## Results (2 cores)
| Test | Result | Wall time |
|---|---|---|
| hello (argv, stdout+stderr, `return atoi(argv[1])` -> exit 7) | pass | 0.03 s |
| neon (vfmaq_f32, vaddvq_u8, libm sqrt/sin/exp2) | pass | 0.03 s |
| malloc (400k malloc/free up to 20 KB, 64 MB calloc, realloc chain) | pass | 3.2-3.7 s |
| io (fopen/read/fstat/unlink, clock_gettime, getrandom, mmap/mprotect, 4 pthreads + mutex) | pass | 0.17 s |
| cxx (libc++ static: map/string/vector, exceptions) | pass | 0.04 s |
| segv (see below) | pass | 0.02 s |
| full ctest suite (7 tests) | 7/7 | ~3.5-4.5 s |

Process startup is about 25 ms. Plain integer loop: about 300M iterations/s (TCG JIT).

## SIGSEGV / MMIO fault path (for the fault-decoder stream)
`arm64-tests/segv.c` covers the MMIO decoder's needs, and it passes:
- load from an unmapped address -> handler gets `si_addr` = exact effective address (base + imm), `si_code` SEGV_MAPERR;
  `uc_mcontext.fault_address` == si_addr; handler writes `regs[rt]` and `pc += 4`, and rt_sigreturn restores it.
- `__reserved[0]` is `fpsimd_context` (magic 0x46508001); a modified `vregs[1]` is restored on sigreturn.
- store fault (`str xzr`) is delivered too. NOTE: rt==31 means XZR/WZR for loads and stores, but `regs[31]`
  in mcontext is NOT a register (mcontext has regs[0..30] + sp). Writing `regs[31]` clobbers memory
  next to sp (the test originally crashed this way). The decoder must special-case Rt==31.
- PROT_NONE page inside a 4 GiB PROT_NONE reservation (Xbox-memory-window style) -> SEGV_ACCERR, exact address.

Differences from a real Quest kernel to keep in mind:
- QEMU emits NO `esr_context` (ESR_MAGIC 0x45535201) record. The real kernel does for data aborts.
  The decoder must decode the instruction at pc and must not depend on ESR (treat ESR as an optional hint).
- With `ARM64RUN_CPU=max` (SVE enabled), QEMU adds an `sve_context` and restores V regs from it, so
  changes to fpsimd vregs are lost (segv test FAILS on that check under `max`). Quest 3 has no SVE;
  keep the default CPU, or `max,sve=off,sme=off` (passes).
- Guest pages are 4 KiB (AT_PAGESZ=4096), matching Quest.

## Build recipe (reproducible, ~3 min on 2 cores)
glib-2.0 2.80 dev files, zlib, pixman, meson (vendored wheel in qemu/python/wheels) and ninja are present.
gitlab.com is blocked (proxy 403), so the `berkeley-softfloat-3` subproject (used only by tests/fp) can't be
fetched. Patched `tests/fp/meson.build` with `subdir_done()` on line 2 (the only local change).
```sh
git clone --depth 1 -b v10.1.5 https://github.com/qemu/qemu /root/deps/qemu
sed -i '1i # arm64-exec: skip fp tests (gitlab subproject unreachable)\nsubdir_done()' /root/deps/qemu/tests/fp/meson.build
mkdir /root/deps/qemu-b2 && cd /root/deps/qemu-b2
../qemu/configure --target-list=aarch64-linux-user --disable-system --disable-tools --disable-docs \
  --disable-werror --disable-rust --static --prefix=/root/xtool/qemu      # ~15 s
ninja qemu-aarch64                                                        # ~2m50s
mkdir -p /root/xtool/qemu/bin && cp qemu-aarch64 /root/xtool/qemu/bin/ && strip /root/xtool/qemu/bin/qemu-aarch64
```

## Toolchain-file gotcha
NDK r27c with CMake 3.28 goes through `android-legacy.toolchain.cmake`, which sets a *normal* variable
`CMAKE_EXE_LINKER_FLAGS` that shadows the cache, so `-DCMAKE_EXE_LINKER_FLAGS=-static` or `*_INIT` alone are
silently ignored (the result is a PIE with interpreter /system/bin/linker64, and every ctest test fails at once).
The toolchain file appends `-static` to that variable after including the NDK file, and uses `*_INIT` when
`-DANDROID_USE_LEGACY_TOOLCHAIN_FILE=OFF`. Both modes are verified at 7/7.
Check: `file build/<exe>` must say "statically linked".
