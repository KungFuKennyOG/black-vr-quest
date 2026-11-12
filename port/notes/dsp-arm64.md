# DSP56300 core on arm64 (Quest)

The MCPX audio DSP is emulated by `third_party/dsp56300` (Rust, Cranelift JIT).

## Build (aarch64-linux-android)

```sh
. /root/xtool/env.sh
rustup target add aarch64-linux-android          # toolchain 1.98.1 is pinned by rust-toolchain.toml
cd third_party/dsp56300
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$ANDROID_CC \
       CC_aarch64_linux_android=$ANDROID_CC AR_aarch64_linux_android=$NDK_BIN/llvm-ar
cargo build --release --locked --no-default-features -p dsp56300-emu-ffi \
      --target aarch64-linux-android --target-dir ../../build-dsp
```

Output: `build-dsp/aarch64-linux-android/release/libdsp56300_emu_ffi.a` (~35 MB, mostly Cranelift).
`port/android/CMakeLists.txt` picks it up automatically; `xboxrecomp/src/apu/CMakeLists.txt` also
builds it itself when `ANDROID` is set. Link with `-ldl` (Cranelift's `dlsym` lookup).

Sandbox note: `index.crates.io` is listed in `NO_PROXY` here, so cargo tried to reach it directly
and failed. Remove it from `NO_PROXY`/`no_proxy` for the first fetch; the crates are cached in
`~/.cargo/registry` afterwards.

## Verification

`tests/dsp_arm64/`: `prog.a56` (MAC/ASL/ADD in a hardware DO loop with post-increment stores to
X and Y) assembled with the in-tree `dsp56300-asm`, plus the crate's own `quickstart.c`. The same
`smoke.c` is linked against the x86-64 and arm64 libraries; the arm64 build runs under
`/root/xtool/arm64run`. The result lines (registers stored to memory and a hash of all X/Y RAM)
are byte-identical.

Not yet verified: real-time speed on the Quest's Cortex-A78C/X1 cores, and W^X / JIT memory
policy on Quest OS (Cranelift maps RW, then flips to RX via mprotect, which Android allows for
app processes).
