/*
 * bvr_diag.h - BLACK VR device preflight: process-level self-checks, run once at startup before the game thread,
 * each reported as ONE logcat line (tag BlackVR):
 *
 *     PREFLIGHT <name>: OK|WARN|FAIL|SKIP <details>
 *
 * and a closing "PREFLIGHT summary: ..." line. port/notes/device-preflight.md says what each line means and what to
 * do on a FAIL. Pure C on POSIX: built into libblackvr.so (Android), into tests/device_preflight (static arm64 under
 * QEMU, and native x86-64) and into tests/game_render_x86 (the headless game). Needs the SIGSEGV dispatcher of
 * platform/arm64_fault_decode.h (a64_fault_install / a64_fault_add_handler / a64_uc_emulate_mmio); the shell-only
 * smoke APK, which has no runtime, gets weak references and reports the trap check as SKIP.
 *
 * The Vulkan, OpenXR, AAudio and JNI checks need the shell's state and live in xr_preflight.c.
 */
#ifndef BVR_DIAG_H
#define BVR_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BVR_PF_OK = 0, BVR_PF_WARN, BVR_PF_FAIL, BVR_PF_SKIP } BvrPfStatus;

/* One PREFLIGHT line. A FAIL with fatal set means the game cannot run (it is not started; the shell shows a red
 * panel). Returns st. */
BvrPfStatus bvr_pf_report(const char *name, BvrPfStatus st, int fatal, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
/* A detail line under a check ("PREFLIGHT <name> | ..."), for what does not fit on the one line. */
void bvr_pf_detail(const char *name, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

typedef struct {
    int ok, warn, fail, skip;
    int fatal;                     /* number of fatal FAILs */
    char fatal_names[160];         /* "sigsegv addrspace" */
    char fail_names[240];
} BvrPfSummary;
const BvrPfSummary *bvr_pf_summary(void);
/* "PREFLIGHT summary: 12 OK, 1 WARN, 0 FAIL, 1 SKIP -> <verdict>" */
void bvr_pf_summary_line(const char *verdict);

/* Banner: version, device, OS, kernel, page size, CPUs (cpufreq), seccomp mode, heap pointer tagging. */
void bvr_pf_begin(const char *version);

/* ---- game folder ------------------------------------------------------------------------------------------------ */
typedef struct {
    char root[512];        /* the folder holding game/ (becomes the working directory) */
    char game_dir[600];    /* <root>/game */
    int  found;
    long long xbe_size;
    int  unreadable_seen;  /* a candidate exists but access was denied (All files access missing) */
    char unreadable[512];
} BvrGameDir;
/* candidates: folders holding game/ (each tried as <c>/game/default.xbe), in order; every one is logged.
 * expect_size / expect_sig_hex (the XBE the APK was generated from; 0 / NULL: unknown) check the copy found. */
int bvr_pf_gamedir(const char *const *candidates, int n, long long expect_size, const char *expect_sig_hex,
                   BvrGameDir *out);
/* Free space on each folder, and that the save folder is writable (create + write + fsync + unlink). */
void bvr_pf_storage(const char *game_root, const char *save_dir, const char *internal_dir);

/* ---- process ------------------------------------------------------------------------------------------------------ */
/* /proc/meminfo (MemTotal, MemAvailable, swap) and this process's RSS; java_class_mb < 0: unknown. */
void bvr_pf_memory(long java_class_mb, long java_large_class_mb, long long am_avail, long long am_threshold,
                   int am_low);
/* The guest window as xbox_MemoryLayoutInit makes it: 4 GB PROT_NONE|MAP_NORESERVE reservation, a 64 MB memfd
 * (ASharedMemory fallback) mapped MAP_SHARED|MAP_FIXED at the base, 28 mirrors and the contiguous window at
 * +0x80000000, aliasing checked, a trapped (PROT_NONE) aperture page; everything is unmapped again. FATAL on FAIL. */
int bvr_pf_addrspace(void);
/* A deliberate trapped access: a PROT_NONE test page, a load and a store through the runtime's SIGSEGV dispatcher
 * (through ART's libsigchain in an app), emulated and resumed; reports the handler's stack (alternate or not, and
 * how much it used), the cost of one trap and the previous SIGSEGV handler (debuggerd). FATAL on FAIL. */
int bvr_pf_sigsegv(const char *name);
/* RW -> RX mprotect of anonymous memory, as the DSP56300's Cranelift JIT does (memmap2 + region::protect), the code
 * run; the W^X policy (is RWX refused?), and the membarrier(SYNC_CORE) sequence cranelift-jit runs on aarch64 (a
 * failure there panics the JIT). Syscalls are probed with a temporary SIGSYS handler, so a seccomp refusal is
 * reported instead of killing the process. */
int bvr_pf_jit(void);
/* Syscalls the runtime uses that an app's seccomp filter could refuse: memfd_create, membarrier, sched_*affinity. */
void bvr_pf_syscalls(void);

/* ---- per thread ------------------------------------------------------------------------------------------------- */
/* Give the calling thread an alternate signal stack of at least min_bytes (mmap'd, guard page below), unless it has
 * one that big; freed at thread exit. In an app the SIGSEGV handlers run on it: ART's libsigchain installs its
 * handler SA_ONSTACK, and bionic's own per-thread stack is 32 KB (16 KB on 32-bit), shared with the kernel's signal
 * frame (~4.6 KB on arm64) and every nested trap. Returns the size in effect (0: none). */
size_t bvr_thread_altstack(size_t min_bytes);

#ifdef __cplusplus
}
#endif

#endif /* BVR_DIAG_H */
