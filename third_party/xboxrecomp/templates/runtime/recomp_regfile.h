/*
 * recomp_regfile.h -- the guest register file as ONE thread-local object (Android / bionic; port/notes/perf.md 9).
 *
 * Why: the registers used to be ~40 separate __thread variables (g_eax, g_esp, g_xmm0, g_fp_stack, ...). The title
 * runs from libblackvr.so, which the NativeActivity dlopen()s, and bionic gives a dlopen()ed library dynamic TLS only:
 * every distinct TLS variable a function touches costs one TLSDESC call into bionic's tlsdesc_resolver_dynamic (call
 * site + ~17 instructions, 4-5 dependent loads), and the generated code touches ~10 per guest call: 82,500 call sites
 * in the .so, ~680,000 resolver calls per level-1 frame. initial-exec is refused for a dlopen()ed library and lld
 * cannot link AArch64 local-dynamic (perf.md 1.5). With one object the compiler fetches its address once per function
 * and reaches every register at a constant offset from it.
 *
 * How: the old names stay. Each g_xxx is a macro for a member of g_recomp_regs, so the generated code, the runtime and
 * the title's hand-written code are unchanged at the source level. The object is defined once, in
 * src/kernel/xbox_memory_layout.c. Behaviour is identical: same types, same per-thread storage, same initial values.
 * Member names carry an rf_ prefix because the generated code #defines eax, xmm0, ... to the g_ names.
 *
 * Who: RECOMP_REGFILE is set by recomp_types.h and xbox_memory_layout.h on Android (or by -DRECOMP_REGFILE), never
 * on Windows (_WIN32: the PC build keeps the separate variables, token for token). A translation unit that declares
 * a register the old way while another uses the object cannot link (the old symbols no longer exist), so the two models
 * can never be mixed silently. -DRECOMP_NO_REGFILE keeps the old model on Android too (every TU must agree).
 *
 * Included by recomp_types.h (also copied next to the generated code's copy of it by tools/recomp) and by
 * src/kernel/xbox_memory_layout.h. Requires RECOMP_TLS, RecompXmm and RecompMmx to be defined first.
 */
#ifndef RECOMP_REGFILE_H
#define RECOMP_REGFILE_H

#include <stdint.h>

typedef struct RecompRegFile {
    /* the integer registers the generated code keeps in locals between calls; hottest first */
    uint32_t rf_esp, rf_eax, rf_ecx, rf_edx, rf_esi, rf_edi, rf_ebx, rf_ebp;
    uint32_t rf_seh_ebp;               /* SEH frame bridge */
    uint32_t rf_fs_base;               /* linear base of fs: (this thread's TIB) */
    int rf_df;                         /* EFLAGS.DF */
    int rf_fp_top;                     /* x87 stack top */
    int rf_fp_cmp;
    uint16_t rf_fp_control_word;
    uint16_t rf_fp_cc;
    double rf_fp_stack[8];             /* x87 stack (binary64 model) */
    RecompXmm rf_xmm[8];               /* 16-byte aligned: the object is */
    RecompMmx rf_mm[8];
} RecompRegFile;

extern RECOMP_TLS RecompRegFile g_recomp_regs;

#define g_eax              (g_recomp_regs.rf_eax)
#define g_ecx              (g_recomp_regs.rf_ecx)
#define g_edx              (g_recomp_regs.rf_edx)
#define g_esp              (g_recomp_regs.rf_esp)
#define g_ebx              (g_recomp_regs.rf_ebx)
#define g_esi              (g_recomp_regs.rf_esi)
#define g_edi              (g_recomp_regs.rf_edi)
#define g_ebp              (g_recomp_regs.rf_ebp)
#define g_seh_ebp          (g_recomp_regs.rf_seh_ebp)
#define g_fs_base          (g_recomp_regs.rf_fs_base)
#define g_df               (g_recomp_regs.rf_df)
#define g_fp_top           (g_recomp_regs.rf_fp_top)
#define g_fp_cmp           (g_recomp_regs.rf_fp_cmp)
#define g_fp_control_word  (g_recomp_regs.rf_fp_control_word)
#define g_fp_cc            (g_recomp_regs.rf_fp_cc)
#define g_fp_stack         (g_recomp_regs.rf_fp_stack)
#define g_xmm0             (g_recomp_regs.rf_xmm[0])
#define g_xmm1             (g_recomp_regs.rf_xmm[1])
#define g_xmm2             (g_recomp_regs.rf_xmm[2])
#define g_xmm3             (g_recomp_regs.rf_xmm[3])
#define g_xmm4             (g_recomp_regs.rf_xmm[4])
#define g_xmm5             (g_recomp_regs.rf_xmm[5])
#define g_xmm6             (g_recomp_regs.rf_xmm[6])
#define g_xmm7             (g_recomp_regs.rf_xmm[7])
#define g_mm0              (g_recomp_regs.rf_mm[0])
#define g_mm1              (g_recomp_regs.rf_mm[1])
#define g_mm2              (g_recomp_regs.rf_mm[2])
#define g_mm3              (g_recomp_regs.rf_mm[3])
#define g_mm4              (g_recomp_regs.rf_mm[4])
#define g_mm5              (g_recomp_regs.rf_mm[5])
#define g_mm6              (g_recomp_regs.rf_mm[6])
#define g_mm7              (g_recomp_regs.rf_mm[7])

/* Initial values, as the separate definitions had them (xbox_memory_layout.c); everything else starts at 0 */
#define RECOMP_REGFILE_INIT(fs_base_init, fp_cw_init, fp_cc_init) \
    { .rf_fs_base = (fs_base_init), .rf_fp_control_word = (fp_cw_init), .rf_fp_cc = (fp_cc_init) }

#endif /* RECOMP_REGFILE_H */
