/**
 * BLACK - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       BLACK (retail original Xbox)
 *   Title ID:    0x45410083
 *   Base addr:   0x00010000
 *   Entry point: 0x00025A3F
 *   Code size:   2,125,740 bytes (.text)
 *   Sections:    13
 *   Kernel imports: 115
 */

#if defined(__ANDROID__)
/* Quest (aarch64 Android): the XR shell (port/quest) calls black_game_main on
 * its game thread. No Win32 SDK, dbghelp or VEH here: the runtime's POSIX
 * Win32 layer, logcat, and the SIGSEGV dispatcher of
 * platform/arm64_fault_decode take their places (see the __ANDROID__ blocks). */
#include "black_android.h"
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "platform/arm64_fault_decode.h"
#include "vr_bridge.h"        /* VrDiagCounters (port/quest) */
#include "xbox_ordinals.h"    /* kernel export names (port/quest/src) */
/* The faulting context's pc / sp / return address. The Quest is AArch64; the same code also runs natively on an
 * x86-64 Linux host in tests/game_render_x86 (needs _GNU_SOURCE for REG_*), which has no link register. */
#if defined(__aarch64__)
#define BLACK_UC_PC(u) ((uintptr_t)(u)->uc_mcontext.pc)
#define BLACK_UC_SP(u) ((uintptr_t)(u)->uc_mcontext.sp)
#define BLACK_UC_LR(u) ((uintptr_t)(u)->uc_mcontext.regs[30])
#elif defined(__x86_64__)
#define BLACK_UC_PC(u) ((uintptr_t)(u)->uc_mcontext.gregs[REG_RIP])
#define BLACK_UC_SP(u) ((uintptr_t)(u)->uc_mcontext.gregs[REG_RSP])
#define BLACK_UC_LR(u) ((uintptr_t)0)
#endif
#else
#include <windows.h>
#include <dbghelp.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "recomp_icall_feedback.h"

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
#ifndef RECOMP_REGFILE   /* Android: members of the one register-file object (recomp_regfile.h) */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
#endif
extern ptrdiff_t g_xbox_mem_offset;
extern int recomp_dispatch_init(void);

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run from the project root in PowerShell:
 *   Push-Location third_party/xboxrecomp
 *   & ..\..\.venv\Scripts\python.exe -m tools.xbe_parser ..\..\game\default.xbe --json ..\..\.work\xbe-analysis-check.json
 *   Pop-Location
 */
#define YOUR_GAME_ENTRY_POINT   0x00025A3F  /* XBE entry point VA */
#if defined(__ANDROID__)
/* Absolute, from the shell's working directory (black_android_paths below). */
#define YOUR_GAME_XBE_PATH      black_android_xbe_path
#define YOUR_GAME_DIR           black_android_game_dir
#else
#define YOUR_GAME_XBE_PATH      "game\\default.xbe"
#define YOUR_GAME_DIR            "game"
#endif

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* Native keyboard/mouse: how a keyboard action finds the control the live action map gives it
 * (recomp_manual.c, BLACK_PC_INPUT). */
extern int black_pc_input_resolver(int slot);
extern void pc_input_set_control_resolver(int (*resolver)(int slot));
extern int black_pc_input_gameplay_blocked_query(void);
extern void black_controls_install(void);
extern void pc_input_set_gameplay_blocked_query(int (*query)(void));
extern void black_photo_install(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
#if defined(__ANDROID__)
    /* The generated sub_XXXXXXXX functions are exported from the .so, so the
     * dynamic symbol table names them even in a stripped APK library. */
    Dl_info info;

    if (dladdr(rip, &info) && info.dli_sname)
        fprintf(stderr, "  in %s+0x%llX\n", info.dli_sname,
                (unsigned long long)((uintptr_t)rip - (uintptr_t)info.dli_saddr));
#else
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);
#endif

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone_sized(uint8_t *ram_ptr, size_t ram_size);
extern int mcpx_apu_set_stereo_fallback_pair(MCPXAPUState *d,
                                             unsigned int left_bin,
                                             unsigned int right_bin);
extern void mcpx_apu_dump_state(void);
extern void xbox_AddHangReport(void (*report)(void));
extern unsigned long mcpx_apu_frame_thread_id(void);
extern void xbox_AddHangThread(unsigned long tid, const char *name);
extern MCPXAPUState *g_apu_state;
extern void mcpx_apu_irq_service_begin(void *opaque);
extern void mcpx_apu_irq_service_end(void *opaque);
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* The APU's interrupt line, as the kernel delivers it. */
static void apu_irq_line(int asserted)
{
    xbox_SetInterruptLine(XBOX_APU_VECTOR, asserted);
}

/* The trapped span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not the whole MCPX aperture. AC'97 above it stays plain memory,
 * which is what the codec-ready bit needs. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

/* Guest memory the crash report may read without faulting again: main RAM
 * and the contiguous window, both mapped by xbox_MemoryLayoutInit. A second
 * fault inside the handler would recurse, so anything else is skipped. */
static int guest_readable(uint32_t va, uint32_t bytes)
{
    if (va >= 0x00010000u && va + bytes <= 0x04000000u && va + bytes > va)
        return 1;
    if (va >= 0x80000000u && va + bytes <= 0x84000000u && va + bytes > va)
        return 1;
    return 0;
}

static void dump_guest_words(const char *label, uint32_t va, int count)
{
    const uint32_t *p;
    int i;

    if (!g_xbox_mem_offset || !guest_readable(va, (uint32_t)count * 4u))
        return;
    p = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
    fprintf(stderr, "  %s 0x%08X:", label, va);
    for (i = 0; i < count; i++) {
        if (i % 8 == 0)
            fprintf(stderr, "\n    +%02X:", i * 4);
        fprintf(stderr, " %08X", p[i]);
    }
    fprintf(stderr, "\n");
}

/* Raw guest stack and the objects it points at.
 *
 * Only eax/ecx/edx/esp are global; ebx/esi/edi/ebp live in the generated
 * function's locals, so the register line cannot say what a crashing function
 * was working on. Its arguments are on the guest stack, though, and one level
 * of pointers from there is usually the object whose state is wrong.
 * RECOMP_CRASH_PEEK=va,va,... dumps further addresses found that way on the
 * next run (the layout is deterministic from boot). */
static void dump_guest_crash_memory(void)
{
    const uint32_t *sp;
    int i, shown = 0;

    if (!g_xbox_mem_offset || !guest_readable(g_esp, 32 * 4))
        return;
    dump_guest_words("guest stack", g_esp, 32);
    sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
    for (i = 0; i < 32 && shown < 12; i++) {
        uint32_t v = sp[i];
        char label[32];
        if (v > g_xbox_code_lo && v < g_xbox_code_hi)
            continue;                       /* return address, not an object */
        if (!guest_readable(v, 0x60))
            continue;
        snprintf(label, sizeof(label), "[esp+%02X] ->", i * 4);
        dump_guest_words(label, v, 24);
        shown++;
    }
    {
        const char *spec = getenv("RECOMP_CRASH_PEEK");
        char buf[256], *tok, *ctx = NULL;
        if (spec) {
            strncpy(buf, spec, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            for (tok = strtok_s(buf, ",", &ctx); tok;
                 tok = strtok_s(NULL, ",", &ctx))
                dump_guest_words("peek", (uint32_t)strtoul(tok, NULL, 0), 24);
        }
    }
}

#if !defined(__ANDROID__)
/* RECOMP_FPE_TRAP=1 (lab diagnostic): the main thread runs with the invalid-operation exception unmasked (re-armed once a
 * frame); the first few distinct places that make a NaN or compare one are printed, then the exception is masked again and
 * the instruction runs on. Finds where a NaN is made. */
static __declspec(thread) int s_fpe_stepping;
static LONG CALLBACK fpe_veh(PEXCEPTION_POINTERS ep)
{
    static struct { uint64_t rip; long count; } seen[64];
    static int nseen;
    static volatile LONG total;
    int i;
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && s_fpe_stepping) {
        /* the instruction that trapped has run masked: arm the trap again */
        ep->ContextRecord->EFlags &= ~0x100u;
        ep->ContextRecord->MxCsr &= ~0x80u;
        s_fpe_stepping = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_FLT_INVALID_OPERATION) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&total);
    for (i = 0; i < nseen && seen[i].rip != ep->ContextRecord->Rip; i++) {}
    if (i == nseen && nseen < 64) {
        seen[nseen].rip = ep->ContextRecord->Rip; seen[nseen].count = 0; nseen++;
        fprintf(stderr, "[FPE] #%d invalid operation at RIP=0x%llX (total so far %ld)\n", i, (unsigned long long)ep->ContextRecord->Rip, (long)total);
        fprintf(stderr, "  RIP = black_xbox_recomp.exe+0x%llX\n",
                (unsigned long long)(ep->ContextRecord->Rip - (uintptr_t)GetModuleHandleA(NULL)));
        {
            IMAGEHLP_LINE64 line;
            DWORD ldisp = 0;
            memset(&line, 0, sizeof(line));
            line.SizeOfStruct = sizeof(line);
            if (SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)ep->ContextRecord->Rip, &ldisp, &line))
                fprintf(stderr, "  at %s:%lu\n", line.FileName, (unsigned long)line.LineNumber);
        }
        print_guest_context((void *)ep->ContextRecord->Rip);
        fflush(stderr);
    }
    if (i < nseen) {
        long c = ++seen[i].count;
        if (c == 100 || c == 10000 || c == 1000000)
            fprintf(stderr, "[FPE] #%d at RIP=0x%llX: %ld hits\n", i, (unsigned long long)ep->ContextRecord->Rip, c);
    }
    ep->ContextRecord->MxCsr |= 0x80;
    ep->ContextRecord->EFlags |= 0x100u;      /* single-step the instruction, then unmask again */
    s_fpe_stepping = 1;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* APU registers -> the emulated APU.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        /* Image-relative RIP survives ASLR, so it can be looked up in the
         * PDB or a map file after the fact. */
        fprintf(stderr, "  RIP = black_xbox_recomp.exe+0x%llX\n",
            (unsigned long long)(ep->ContextRecord->Rip -
                                 (uintptr_t)GetModuleHandleA(NULL)));
        fprintf(stderr, "  PC regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  PC regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  PC VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);
        dump_guest_crash_memory();

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    } else {
        /* Other faults that end the process (a divide by zero in generated code, say): name where, as for a bad access. */
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_INT_OVERFLOW ||
            code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION) {
            fprintf(stderr, "[CRASH] exception 0x%08lX at RIP=0x%llX\n", (unsigned long)code,
                    (unsigned long long)ep->ContextRecord->Rip);
            fprintf(stderr, "  RIP = black_xbox_recomp.exe+0x%llX\n",
                    (unsigned long long)(ep->ContextRecord->Rip - (uintptr_t)GetModuleHandleA(NULL)));
            fprintf(stderr, "  PC regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n", g_eax, g_ecx, g_edx, g_esp);
            fprintf(stderr, "  native regs: rax=%llX rcx=%llX rdx=%llX rbx=%llX rsi=%llX rdi=%llX\n",
                    (unsigned long long)ep->ContextRecord->Rax, (unsigned long long)ep->ContextRecord->Rcx,
                    (unsigned long long)ep->ContextRecord->Rdx, (unsigned long long)ep->ContextRecord->Rbx,
                    (unsigned long long)ep->ContextRecord->Rsi, (unsigned long long)ep->ContextRecord->Rdi);
            print_guest_context((void *)ep->ContextRecord->Rip);
            fflush(stderr);
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}
#endif /* !__ANDROID__ */

#if defined(__ANDROID__)
/* ── Quest (aarch64 Android) host glue ─────────────────────── */

#define BLACK_LOG_TAG "BlackGame"
#ifndef BLACK_VR_TAG
#define BLACK_VR_TAG "BlackVR"   /* the shell's tag: PREFLIGHT / CRASH / HEARTBEAT / STALL lines */
#endif

static char black_android_root[512];      /* the shell's working directory  */
static char black_android_game_dir[600];  /* <root>/game: default.xbe + disc */
static char black_android_save_dir[600];  /* <root>/save                     */
static char black_android_xbe_path[640];

/* MessageBoxA has nowhere to go on a headset: the shell keeps showing its
 * own scene, so the message goes to logcat (and stderr, which is logcat too). */
static void black_android_fatal(const char *text, const char *caption)
{
    __android_log_print(ANDROID_LOG_ERROR, BLACK_LOG_TAG, "%s: %s", caption, text);
}

/* stdout/stderr -> logcat. The runtime and the game report everything with
 * printf/fprintf(stderr), and an Android app's stdio goes to /dev/null. A pipe
 * replaces fds 1 and 2; a thread forwards it line by line (tag BlackGame).
 * BLACKVR_STDIO_LOGCAT=0 leaves stdio alone; BLACKVR_LOG_FILE=path also
 * appends every line to that file. */
static int black_android_log_pipe[2] = { -1, -1 };
static int black_android_log_file = -1;

/* The on-headset log file (logs/blackvr_last.log under the game root, the previous run's as blackvr_prev.log):
 * the game's stdio and this file's BlackVR lines (HEARTBEAT, STALL), so a freeze or crash with the USB cable off
 * still leaves a record. Rolled over to blackvr_last.1.log past BLACKVR_LOG_FILE_MAX_MB (64). */
static char black_android_log_path[600];
static unsigned long long black_android_log_bytes, black_android_log_max = 64ull << 20;
static pthread_mutex_t black_android_log_lock = PTHREAD_MUTEX_INITIALIZER;
static void black_android_log_file_write(const char *tag, const char *line, size_t n)
{
    char stamp[48];
    struct timespec ts;
    struct tm tmv;
    int sl;
    if (black_android_log_file < 0) return;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    sl = snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03ld %d %s: ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  ts.tv_nsec / 1000000, (int)gettid(), tag);
    pthread_mutex_lock(&black_android_log_lock);
    if (black_android_log_path[0] && black_android_log_bytes > black_android_log_max) {
        char old[620];
        snprintf(old, sizeof(old), "%.*s.1.log", (int)(strlen(black_android_log_path) - 4), black_android_log_path);
        close(black_android_log_file);
        rename(black_android_log_path, old);
        black_android_log_file = open(black_android_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
        black_android_log_bytes = 0;
    }
    if (black_android_log_file >= 0) {
        struct iovec v[3];
        v[0].iov_base = stamp; v[0].iov_len = (size_t)(sl > 0 ? sl : 0);
        v[1].iov_base = (void *)line; v[1].iov_len = n;
        v[2].iov_base = (void *)"\n"; v[2].iov_len = 1;
        (void)!writev(black_android_log_file, v, 3);
        black_android_log_bytes += v[0].iov_len + n + 1;
    }
    pthread_mutex_unlock(&black_android_log_lock);
}

/* __android_log_print for this file's BlackVR lines, also into the on-headset log file */
static void black_vr_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void black_vr_log(int prio, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    __android_log_write(prio, BLACK_VR_TAG, line);
    black_android_log_file_write(BLACK_VR_TAG, line, (size_t)n);
}

static void black_android_log_line(char *line, size_t n)
{
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = 0;
    __android_log_write(ANDROID_LOG_INFO, BLACK_LOG_TAG, line);
    black_android_log_file_write(BLACK_LOG_TAG, line, n);
}

static void *black_android_log_pump(void *arg)
{
    char buf[1025];
    size_t have = 0;
    ssize_t got;
    (void)arg;
    while ((got = read(black_android_log_pipe[0], buf + have, sizeof(buf) - 1 - have)) > 0) {
        char *start = buf, *nl;
        have += (size_t)got;
        while ((nl = memchr(start, '\n', (size_t)(buf + have - start))) != NULL) {
            *nl = 0;
            black_android_log_line(start, (size_t)(nl - start));
            start = nl + 1;
        }
        have -= (size_t)(start - buf);
        memmove(buf, start, have);
        if (have == sizeof(buf) - 1) {                 /* a line longer than the buffer */
            buf[have] = 0;
            black_android_log_line(buf, have);
            have = 0;
        }
    }
    return NULL;
}

static void black_android_stdio_to_logcat(void)
{
    const char *opt = getenv("BLACKVR_STDIO_LOGCAT"), *file = getenv("BLACKVR_LOG_FILE");
    pthread_t thread;
    if (black_android_log_pipe[0] >= 0 || (opt && *opt == '0'))
        return;
    {
        const char *max = getenv("BLACKVR_LOG_FILE_MAX_MB");
        if (max && atoi(max) > 0) black_android_log_max = (unsigned long long)atoi(max) << 20;
    }
    if (file && *file && strcmp(file, "0")) {
        black_android_log_file = open(file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0660);
    } else if (!file || strcmp(file, "0")) {
        /* default: <working directory = game root>/logs/blackvr_last.log, the previous run's kept as blackvr_prev.log */
        char cwd[512], prev[600], older[620];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(black_android_log_path, sizeof(black_android_log_path), "%s/logs", cwd);
            mkdir(black_android_log_path, 0770);
            snprintf(prev, sizeof(prev), "%s/logs/blackvr_prev.log", cwd);
            snprintf(older, sizeof(older), "%s/logs/blackvr_last.1.log", cwd);
            snprintf(black_android_log_path, sizeof(black_android_log_path), "%s/logs/blackvr_last.log", cwd);
            rename(black_android_log_path, prev);
            unlink(older);
            black_android_log_file = open(black_android_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
            if (black_android_log_file < 0) black_android_log_path[0] = 0;
        }
    }
    if (pipe(black_android_log_pipe) != 0)
        return;
    fflush(stdout);
    fflush(stderr);
    dup2(black_android_log_pipe[1], 1);
    dup2(black_android_log_pipe[1], 2);
    if (pthread_create(&thread, NULL, black_android_log_pump, NULL) == 0)
        pthread_detach(thread);
}

/* Give the log thread a moment to forward what a crash report just wrote,
 * before the fault is passed on and the process dies. */
static void black_android_log_drain(void)
{
    int i, pending = 0;
    fflush(stdout);
    fflush(stderr);
    for (i = 0; i < 50 && black_android_log_pipe[0] >= 0; i++) {
        if (ioctl(black_android_log_pipe[0], FIONREAD, &pending) != 0 || pending == 0)
            break;
        usleep(10000);
    }
    usleep(20000);   /* the last line may still be in the pump's buffer */
}

/* Paths. The shell (port/quest/src/xr_main.c) chdir()s to the folder holding
 * game/ and save/ (the app's external files dir, or /sdcard/BLACK) and creates
 * save/ there. argv[1], when given, names that folder instead. The disc files
 * are read in place from <root>/game; saves go to <root>/save, where
 * black_checkpoint.inc's relative save/checkpoints also lands.
 * BLACKVR_GAME_DIR / BLACKVR_SAVE_DIR (blackvr.ini env lines) override. */
static void black_android_paths(int argc, char **argv)
{
    const char *game = getenv("BLACKVR_GAME_DIR"), *save = getenv("BLACKVR_SAVE_DIR");
    if (argc > 1 && argv[1] && argv[1][0] && chdir(argv[1]) != 0)
        fprintf(stderr, "[BOOT] chdir %s failed (errno %d)\n", argv[1], errno);
    if (!getcwd(black_android_root, sizeof(black_android_root)))
        strcpy(black_android_root, ".");
    snprintf(black_android_game_dir, sizeof(black_android_game_dir), "%s/game", black_android_root);
    snprintf(black_android_save_dir, sizeof(black_android_save_dir), "%s/save", black_android_root);
    if (game && *game)
        snprintf(black_android_game_dir, sizeof(black_android_game_dir), "%s", game);
    if (save && *save)
        snprintf(black_android_save_dir, sizeof(black_android_save_dir), "%s", save);
    snprintf(black_android_xbe_path, sizeof(black_android_xbe_path), "%s/default.xbe", black_android_game_dir);
    if (mkdir(black_android_save_dir, 0770) != 0 && errno != EEXIST)
        fprintf(stderr, "[BOOT] cannot create %s (errno %d)\n", black_android_save_dir, errno);
    fprintf(stderr, "[BOOT] game files %s, saves %s\n", black_android_game_dir, black_android_save_dir);
}

/* The launcher (launcher/src/core/GameSession.cpp gameEnvironment) sets these
 * for every Windows/Linux run; on the Quest nothing does, so the same defaults
 * apply here. Anything blackvr.ini set first wins (setenv without overwrite).
 * Movies are skipped by default: the runtime's player is Media Foundation
 * only, and a disc dump without videos/ has nothing to play. */
static void black_android_env_defaults(void)
{
    static const char *const defaults[][2] = {
        { "RECOMP_AC97_READY", "1" },
        { "RECOMP_APU_DSP_ACK", "0x83218810" },
        { "RECOMP_NV2A_KELVIN", "1" },
        { "RECOMP_VBLANK", "1" },
        { "RECOMP_VBLANK_KEEP_GRID", "1" },
        { "RECOMP_USB", "1" },
        { "RECOMP_FB_WINDOW", "1" },
        { "RECOMP_KELVIN_GPU", "1" },
        { "RECOMP_KELVIN_THREADS", "3" },
        { "RECOMP_LANGUAGE", "en-US" },
        { "RECOMP_BLACK_60HZ", "1" },
        { "RECOMP_BLACK_SKIP_STARTUP_VIDEOS", "1" },
        { "RECOMP_BLACK_SKIP_MOVIES", "1" },
    };
    char path[700];
    size_t i;
    for (i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
        setenv(defaults[i][0], defaults[i][1], 0);
    snprintf(path, sizeof(path), "%s/video.ini", black_android_save_dir);
    setenv("RECOMP_VIDEO_INI", path, 0);
    snprintf(path, sizeof(path), "%s/controls.ini", black_android_save_dir);
    setenv("RECOMP_CONTROLS_INI", path, 0);
    snprintf(path, sizeof(path), "%s/kgpu-cache", black_android_root);
    setenv("RECOMP_KGPU_CACHE", path, 0);
}

/* The emulated APU's trapped registers: what veh_handler does on Windows,
 * through the SIGSEGV dispatcher (port/notes/arm64-fault.md). */
static int apu_fault(void *uc, uintptr_t fault)
{
    uint32_t va = (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset);
    return va >= APU_TRAP_BASE && va < APU_TRAP_END
        && apu_hook_handle_mmio(uc, fault, va, a64_uc_access_is_write(uc));
}

/* Crash report, the counterpart of veh_handler's. It is the first dispatcher
 * handler added, so it runs after every device handler (APU, AC97, OHCI,
 * Kelvin, the memory watch) and only sees faults none of them took. It never
 * claims one: the fault then goes on to debuggerd, which writes the tombstone. */
static volatile int black_android_crashed;

static void black_android_native_report(const ucontext_t *u)
{
    Dl_info self, at;
    const uintptr_t *sp = (const uintptr_t *)BLACK_UC_SP(u);
    int i, shown = 0;
    if (!dladdr((void *)black_android_native_report, &self))
        memset(&self, 0, sizeof(self));
    fprintf(stderr, "  pc=0x%llX lr=0x%llX sp=0x%llX",
            (unsigned long long)BLACK_UC_PC(u), (unsigned long long)BLACK_UC_LR(u),
            (unsigned long long)BLACK_UC_SP(u));
    if (self.dli_fbase)
        fprintf(stderr, " (pc = %s+0x%llX)", self.dli_fname ? self.dli_fname : "?",
                (unsigned long long)(BLACK_UC_PC(u) - (uintptr_t)self.dli_fbase));
    fprintf(stderr, "\n");
    /* Return addresses into this library on the native stack, innermost first. */
    fprintf(stderr, "  Native stack (first 8 return addrs):\n");
    for (i = 0; i < 256 && shown < 8 && sp; i++) {
        if (dladdr((void *)sp[i], &at) && at.dli_fbase == self.dli_fbase && self.dli_fbase) {
            fprintf(stderr, "    [%d] +0x%llX %s\n", i,
                    (unsigned long long)(sp[i] - (uintptr_t)self.dli_fbase),
                    at.dli_sname ? at.dli_sname : "");
            shown++;
        }
    }
}

/* ── Crash and stall breadcrumbs (port/notes/device-preflight.md) ──────────
 * What the runtime, the shell and the Kelvin engine offer when they are
 * linked; weak, because the arm64 boot test stubs some of them out. */
extern int xbox_kernel_last_call(uint32_t *ordinal, uint32_t *caller, int *in_call, uint32_t *ms_ago)
    __attribute__((weak));
extern uint64_t xbox_kernel_call_total(void) __attribute__((weak));
extern void vr_diag_counters(VrDiagCounters *out) __attribute__((weak));
extern void kelvin_diag_state(uint64_t counters[4], uint32_t *get, uint32_t *put, uint32_t *flags)
    __attribute__((weak));
extern void xbox_DumpGuestThreads(void);
extern volatile uint64_t g_icall_count;


static int64_t black_android_t0_ns;

static int64_t black_android_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static double black_android_uptime(void)
{
    return black_android_t0_ns ? (double)(black_android_now_ns() - black_android_t0_ns) * 1e-9 : 0.0;
}

/* "libblackvr.so!sub_0012AB30+0x1C4": the generated functions are exported,
 * so a stripped APK library still names the guest function. */
static void black_android_sym(uintptr_t a, char *out, size_t cap)
{
    Dl_info di;
    if (a && dladdr((void *)a, &di) && di.dli_fname) {
        const char *lib = strrchr(di.dli_fname, '/');
        lib = lib ? lib + 1 : di.dli_fname;
        if (di.dli_sname)
            snprintf(out, cap, "%s!%s+0x%llX", lib, di.dli_sname,
                     (unsigned long long)(a - (uintptr_t)di.dli_saddr));
        else
            snprintf(out, cap, "%s+0x%llX", lib, (unsigned long long)(a - (uintptr_t)di.dli_fbase));
    } else {
        snprintf(out, cap, "0x%llX", (unsigned long long)a);
    }
}

/* Which guest window a fault address is in, or that it is host memory. */
static void black_android_where(uintptr_t fault, char *out, size_t cap)
{
    const uintptr_t off = (uintptr_t)g_xbox_mem_offset;
    if (off && fault >= off && fault - off < ((uintptr_t)1 << 32)) {
        const uint32_t va = (uint32_t)(fault - off);
        const char *w = va < 0x1000u ? "guest NULL page"
                      : va < 0x04000000u ? "guest RAM"
                      : va < 0x74000000u ? "guest RAM mirror"
                      : va >= 0x80000000u && va < 0x84000000u ? "contiguous RAM window"
                      : va >= 0xF0000000u && va < 0xF4000000u ? "tiled framebuffer window"
                      : va >= 0xFD000000u && va < 0xFE000000u ? "NV2A registers"
                      : va >= 0xFE000000u && va < APU_TRAP_BASE ? "kernel thunk area"
                      : va >= APU_TRAP_BASE && va < APU_TRAP_END ? "APU registers"
                      : va >= APU_TRAP_END && va < 0xFF000000u ? "MCPX/AC97/USB registers"
                      : va >= 0xFF000000u ? "flash" : "unmapped guest address";
        snprintf(out, cap, "guest VA 0x%08X (%s)", va, w);
    } else {
        snprintf(out, cap, "host address, outside the guest window");
    }
}

static void black_android_shell_state(char *out, size_t cap)
{
    VrDiagCounters d;
    int64_t last;
    out[0] = 0;
    if (!vr_diag_counters)
        return;
    vr_diag_counters(&d);
    last = d.last_eye_release_ns > d.last_flat_release_ns ? d.last_eye_release_ns : d.last_flat_release_ns;
    if (last)
        snprintf(out, cap, "; %llu stereo + %llu flat frames shown, the last %.1f s ago (game frame %u)",
                 (unsigned long long)d.eye_releases, (unsigned long long)d.flat_releases,
                 (double)(black_android_now_ns() - last) * 1e-9, d.last_game_frame);
    else
        snprintf(out, cap, "; no frame shown yet");
}

/* The one line a crash report starts with, straight to logcat (tag BlackVR):
 * it does not wait for the stdio pipe the long report goes through, so it
 * survives a process that dies before the pipe is drained. */
static void black_android_crash_line(const char *what, uintptr_t pc, uintptr_t lr, uintptr_t fault, int have_fault)
{
    char tname[32] = "?", pcs[192], lrs[192], where[96], at[160] = "", kcall[192] = "no kernel call made on this thread",
         shell[160];
    uint32_t ord = 0, caller = 0, ago = 0;
    int in_call = 0;
    prctl(PR_GET_NAME, tname, 0, 0, 0);
    black_android_sym(pc, pcs, sizeof(pcs));
    black_android_sym(lr, lrs, sizeof(lrs));
    if (have_fault) {
        black_android_where(fault, where, sizeof(where));
        snprintf(at, sizeof(at), "fault address 0x%llX = %s; ", (unsigned long long)fault, where);
    }
    if (xbox_kernel_last_call && xbox_kernel_last_call(&ord, &caller, &in_call, &ago))
        snprintf(kcall, sizeof(kcall), "last kernel call: ordinal %u %s from guest 0x%08X, %s %u ms ago", ord,
                 xbox_ordinal_name(ord), caller, in_call ? "still inside it, entered" : "returned, entered", ago);
    black_android_shell_state(shell, sizeof(shell));
    __android_log_print(ANDROID_LOG_FATAL, BLACK_VR_TAG,
                        "CRASH %s on thread \"%s\" (tid %ld) at %.1f s: %spc %s, lr %s; guest esp 0x%08X eax 0x%08X; "
                        "%s%s", what, tname, (long)syscall(SYS_gettid), black_android_uptime(), at, pcs, lrs, g_esp,
                        g_eax, kcall, shell);
}

static int black_android_crash_fault(void *uc, uintptr_t fault_addr)
{
    const ucontext_t *u = (const ucontext_t *)uc;
    if (black_android_crashed++)
        return 0;
    black_android_crash_line("SIGSEGV (no device handler claimed it)", BLACK_UC_PC(u), BLACK_UC_LR(u), fault_addr, 1);
    fprintf(stderr, "[CRASH] Access violation at pc=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)BLACK_UC_PC(u), (unsigned long long)fault_addr,
            a64_uc_access_is_write(uc) == 1 ? "write" : "read");
    fprintf(stderr, "  PC regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  PC regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n", g_ebx, g_esi, g_edi);
    fprintf(stderr, "  PC VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
    print_guest_context((void *)BLACK_UC_PC(u));
    dump_guest_crash_memory();
    black_android_native_report(u);
    black_android_log_drain();
    return 0;
}

/* The other fatal signals (veh_handler's "other faults that end the process"):
 * report, then put the previous handler back and return, so the instruction
 * re-runs (or abort() re-raises) into debuggerd. */
static const int black_android_fatal_signals[] = { SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGABRT };
static struct sigaction black_android_prev_actions[sizeof(black_android_fatal_signals) / sizeof(int)];

static void black_android_fatal_signal(int sig, siginfo_t *si, void *uc)
{
    const ucontext_t *u = (const ucontext_t *)uc;
    size_t i;
    if (!black_android_crashed++) {
        static const char *const names[] = { "SIGBUS", "SIGILL", "SIGTRAP", "SIGFPE", "SIGABRT" };
        const char *nm = "signal";
        for (i = 0; i < sizeof(black_android_fatal_signals) / sizeof(int); i++)
            if (black_android_fatal_signals[i] == sig)
                nm = names[i];
        black_android_crash_line(nm, BLACK_UC_PC(u), BLACK_UC_LR(u), (uintptr_t)si->si_addr,
                                 sig == SIGBUS || sig == SIGFPE || sig == SIGILL);
        fprintf(stderr, "[CRASH] signal %d (code %d) at pc=0x%llX, addr=0x%llX\n", sig, si->si_code,
                (unsigned long long)BLACK_UC_PC(u), (unsigned long long)(uintptr_t)si->si_addr);
        fprintf(stderr, "  PC regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                g_eax, g_ecx, g_edx, g_esp);
        print_guest_context((void *)BLACK_UC_PC(u));
        black_android_native_report(u);
        black_android_log_drain();
    }
    for (i = 0; i < sizeof(black_android_fatal_signals) / sizeof(int); i++)
        if (black_android_fatal_signals[i] == sig)
            sigaction(sig, &black_android_prev_actions[i], NULL);
}

/* The process's memory as the low-memory killer sees it (RSS, PSS, swap) and its address space (VmSize and the
 * number of mappings: the 4 GB PROT_NONE guest window is VmSize, not RSS), to logcat: once after the memory layout
 * is up, then at 20 s, 60 s and every 5 minutes (port/notes/perf.md). */
static void black_android_log_memory(const char *when)
{
    char line[256];
    long vm_size = -1, vm_rss = -1, rss_anon = -1, rss_file = -1, rss_shmem = -1, vm_swap = -1, pss = -1, maps = 0;
    FILE *f;
    if ((f = fopen("/proc/self/status", "r")) != NULL) {
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "VmSize: %ld", &vm_size);
            sscanf(line, "VmRSS: %ld", &vm_rss);
            sscanf(line, "RssAnon: %ld", &rss_anon);
            sscanf(line, "RssFile: %ld", &rss_file);
            sscanf(line, "RssShmem: %ld", &rss_shmem);
            sscanf(line, "VmSwap: %ld", &vm_swap);
        }
        fclose(f);
    }
    if ((f = fopen("/proc/self/smaps_rollup", "r")) != NULL) {
        while (fgets(line, sizeof(line), f)) sscanf(line, "Pss: %ld", &pss);
        fclose(f);
    }
    if ((f = fopen("/proc/self/maps", "r")) != NULL) {
        while (fgets(line, sizeof(line), f)) if (strchr(line, '\n')) maps++;
        fclose(f);
    }
    fprintf(stderr, "[MEM] %s: RSS %ld MB (anon %ld, file %ld, shmem %ld), PSS %ld MB, swap %ld MB, address space %ld MB in %ld "
                    "mappings\n", when, vm_rss / 1024, rss_anon / 1024, rss_file / 1024, rss_shmem / 1024, pss / 1024,
            vm_swap / 1024, vm_size / 1024, maps);
    fflush(stderr);
}

static void *black_android_memory_watch(void *arg)
{
    static const unsigned waits[] = { 20, 40 };
    unsigned i = 0, t = 0;
    char when[32];
    (void)arg;
    for (;;) {
        const unsigned w = i < 2 ? waits[i] : 300;
        sleep(w);
        t += w;
        i++;
        snprintf(when, sizeof(when), "%u s after boot", t);
        black_android_log_memory(when);
    }
    return NULL;
}

/* ── Progress watchdog ─────────────────────────────────────────────────────
 * A HEARTBEAT line every 10 s (frames shown and their rate, the game's guest
 * and kernel call rates, the pusher, the XR loop, RSS), and while no frame has
 * been shown for 5 s a STALL line every 5 s: what is still moving, where the
 * game thread's host code is (sampled with SIGURG: pc, lr and a frame-pointer
 * walk, named by dladdr), its last kernel call and its guest stack; every 30 s
 * of a stall the guest thread table as well (xbox_DumpGuestThreads). Tag
 * BlackVR. BLACKVR_HEARTBEAT=0 turns it off. port/notes/device-preflight.md. */
#define BLACK_PW_FRAMES 12
static uint32_t *s_pw_esp, *s_pw_eax;        /* the game thread's own guest registers (thread-local) */
static pthread_t s_pw_game;
static uintptr_t s_pw_stack_lo, s_pw_stack_hi;
static struct {
    volatile unsigned seq;
    uintptr_t pc, lr, ret[BLACK_PW_FRAMES];
    int n, have_kcall, in_call;
    uint32_t ord, caller, ago;
} s_pw;

static void black_android_sample_signal(int sig, siginfo_t *si, void *ucv)
{
    const ucontext_t *u = (const ucontext_t *)ucv;
    uintptr_t fp;
    int n = 0;
    (void)sig;
    (void)si;
    if (!pthread_equal(pthread_self(), s_pw_game))
        return;
    s_pw.pc = BLACK_UC_PC(u);
    s_pw.lr = BLACK_UC_LR(u);
#if defined(__aarch64__)
    fp = (uintptr_t)u->uc_mcontext.regs[29];
#else
    fp = 0;        /* x86-64 (the headless test host): no frame records at -O2, rbp is a general register */
#endif
    /* AArch64 Android code keeps frame records: [fp] = caller's fp, [fp+8] = return address */
    while (n < BLACK_PW_FRAMES && fp >= s_pw_stack_lo && fp + 16 <= s_pw_stack_hi && !(fp & 7)) {
        const uintptr_t next = ((const uintptr_t *)fp)[0];
        s_pw.ret[n++] = ((const uintptr_t *)fp)[1];
        if (next <= fp)
            break;
        fp = next;
    }
    s_pw.n = n;
    s_pw.have_kcall = xbox_kernel_last_call &&
                      xbox_kernel_last_call(&s_pw.ord, &s_pw.caller, &s_pw.in_call, &s_pw.ago);
    __atomic_add_fetch(&s_pw.seq, 1, __ATOMIC_RELEASE);
}

static long black_android_rss_mb(void)
{
    long pages = 0, rss = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &rss) != 2)
            rss = 0;
        fclose(f);
    }
    return rss * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
}

static void black_android_stall_report(double stalled_s, uint64_t frames, double icall_rate, double kcall_rate,
                                       uint64_t dwords, const uint64_t kc[4], uint32_t get, uint32_t put,
                                       uint32_t kflags, const VrDiagCounters *d, double xr_rate)
{
    char pcs[192] = "not sampled", lrs[192] = "", stack[900] = "", kcall[200] = "no kernel call on the game thread",
         gstack[200] = "";
    const unsigned before = __atomic_load_n(&s_pw.seq, __ATOMIC_ACQUIRE);
    int i;
    if (s_pw_stack_hi && pthread_kill(s_pw_game, SIGURG) == 0) {
        for (i = 0; i < 20 && __atomic_load_n(&s_pw.seq, __ATOMIC_ACQUIRE) == before; i++)
            usleep(5000);
        if (__atomic_load_n(&s_pw.seq, __ATOMIC_ACQUIRE) != before) {
            black_android_sym(s_pw.pc, pcs, sizeof(pcs));
            black_android_sym(s_pw.lr, lrs, sizeof(lrs));
            for (i = 0; i < s_pw.n; i++) {
                char nm[192];
                size_t l = strlen(stack);
                black_android_sym(s_pw.ret[i], nm, sizeof(nm));
                snprintf(stack + l, sizeof(stack) - l, "%s%s", i ? " < " : "", nm);
            }
            if (s_pw.have_kcall)
                snprintf(kcall, sizeof(kcall), "game thread's last kernel call: ordinal %u %s from guest 0x%08X, %s %u ms",
                         s_pw.ord, xbox_ordinal_name(s_pw.ord), s_pw.caller,
                         s_pw.in_call ? "STILL INSIDE IT for" : "returned; entered", s_pw.ago);
        } else {
            snprintf(pcs, sizeof(pcs), "no answer to the sample signal in 100 ms");
        }
    }
    /* the guest call chain: return addresses into the code sections on the game thread's guest stack */
    if (s_pw_esp && g_xbox_mem_offset) {
        const uint32_t esp = *s_pw_esp;
        int shown = 0;
        snprintf(gstack, sizeof(gstack), "guest esp 0x%08X:", esp);
        for (i = 0; i < 256 && shown < 10 && guest_readable(esp + (uint32_t)i * 4u, 4); i++) {
            const uint32_t v = *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + esp + (uint32_t)i * 4u);
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                size_t l = strlen(gstack);
                snprintf(gstack + l, sizeof(gstack) - l, " %08X", v);
                shown++;
            }
        }
    }
    black_vr_log(ANDROID_LOG_WARN,
                        "STALL no new frame for %.0f s (%llu shown so far; XR loop %.0f/s, session %s, mode %d): guest "
                        "icalls %.0f/s, kernel calls %.0f/s; Kelvin +%llu pushbuffer words, flips %llu, PUT 0x%08X GET "
                        "0x%08X%s%s%s%s%s", stalled_s, (unsigned long long)frames, xr_rate,
                        d->session_running ? "running" : "NOT running", d->present_mode, icall_rate, kcall_rate,
                        (unsigned long long)dwords, (unsigned long long)kc[2], put, get,
                        kflags & 8 ? " (engine not running)" : put != get ? " (pusher behind)" : " (pusher idle: nothing submitted)",
                        kflags & 1 ? ", waiting for a notify ack" : "", kflags & 2 ? ", waiting for a context switch" : "",
                        kflags & 4 ? ", in a flip stall" : "",
                        icall_rate < 1.0 && kcall_rate < 1.0 ? " -- the game thread is blocked or spinning without calls"
                                                             : "");
    black_vr_log(ANDROID_LOG_WARN, "STALL game thread: pc %s, lr %s; frames %s; %s; %s", pcs, lrs,
                        stack[0] ? stack : "(none)", kcall, gstack);
}

/* HEARTBEAT threads line: the busiest threads of this process over the last beat, as % of one core (utime+stime from
 * /proc/self/task/<tid>/stat) with the CPU each last ran on, and the current clock of each CPU it may run on. Shows
 * whether the game thread (bx-main), the Kelvin GPU thread or the APU is what holds a frame rate down. */
#include <dirent.h>
typedef struct { int tid; unsigned long long ticks; } BvrTaskTicks;
static void black_android_thread_line(double dt)
{
    static BvrTaskTicks prev[192];
    static int nprev;
    BvrTaskTicks cur[192];
    struct { int tid, cpu; double pct; char name[20]; } top[192];
    int ncur = 0, ntop = 0, i, j;
    long hz = sysconf(_SC_CLK_TCK);
    char line[640], freq[160] = "";
    size_t l;
    DIR *dir = opendir("/proc/self/task");
    struct dirent *e;
    if (!dir || hz <= 0) { if (dir) closedir(dir); return; }
    while ((e = readdir(dir)) && ncur < 192) {
        char path[64], buf[512], *p, *q;
        FILE *f;
        int tid = atoi(e->d_name), cpu = -1, field;
        unsigned long long ut = 0, st = 0;
        if (tid <= 0) continue;
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        if (!(f = fopen(path, "r"))) continue;
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); continue; }
        fclose(f);
        if (!(p = strchr(buf, '(')) || !(q = strrchr(buf, ')'))) continue;
        /* fields after ")": 3 state ... 14 utime 15 stime ... 39 processor */
        {
            char *tok, *save = NULL;
            field = 3;
            for (tok = strtok_r(q + 2, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
                if (field == 14) ut = strtoull(tok, NULL, 10);
                else if (field == 15) st = strtoull(tok, NULL, 10);
                else if (field == 39) { cpu = atoi(tok); break; }
            }
        }
        cur[ncur].tid = tid;
        cur[ncur].ticks = ut + st;
        for (j = 0; j < nprev; j++)
            if (prev[j].tid == tid) {
                double pct = (double)(cur[ncur].ticks - prev[j].ticks) * 100.0 / ((double)hz * dt);
                if (pct >= 2.0) {
                    size_t nl = (size_t)(q - p - 1);
                    if (nl >= sizeof(top[0].name)) nl = sizeof(top[0].name) - 1;
                    memcpy(top[ntop].name, p + 1, nl);
                    top[ntop].name[nl] = 0;
                    top[ntop].tid = tid; top[ntop].cpu = cpu; top[ntop].pct = pct;
                    ntop++;
                }
                break;
            }
        ncur++;
    }
    closedir(dir);
    memcpy(prev, cur, sizeof(cur[0]) * (size_t)ncur);
    nprev = ncur;
    if (!ntop) return;
    for (i = 0; i < ntop; i++)   /* busiest first */
        for (j = i + 1; j < ntop; j++)
            if (top[j].pct > top[i].pct) { __typeof__(top[0]) t = top[i]; top[i] = top[j]; top[j] = t; }
    for (i = 0; i < 8; i++) {
        char path[80];
        FILE *f;
        long khz = 0;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        if ((f = fopen(path, "r"))) { if (fscanf(f, "%ld", &khz) != 1) khz = 0; fclose(f); }
        if (khz > 0) { l = strlen(freq); snprintf(freq + l, sizeof(freq) - l, "%s%d:%ld", freq[0] ? " " : "", i, khz / 1000); }
    }
    l = (size_t)snprintf(line, sizeof(line), "HEARTBEAT threads (%% of a core):");
    for (i = 0; i < ntop && i < 10 && l < sizeof(line) - 40; i++)
        l += (size_t)snprintf(line + l, sizeof(line) - l, " %s/%d %.0f%% (cpu%d)%s", top[i].name, top[i].tid, top[i].pct, top[i].cpu,
                              i + 1 < ntop && i < 9 ? "," : "");
    if (freq[0] && l < sizeof(line) - 20) snprintf(line + l, sizeof(line) - l, "; MHz %s", freq);
    black_vr_log(ANDROID_LOG_INFO, "%s", line);
}

static void *black_android_progress_watch(void *arg)
{
    int64_t last_progress, last_beat, last_stall_log = 0, last_dump = 0, prev_t;
    uint64_t prev_frames = (uint64_t)-1, beat_frames = 0, beat_xr = 0, beat_icalls = 0, beat_kcalls = 0, beat_words = 0;
    uint64_t prev_icalls = 0, prev_kcalls = 0, prev_words = 0, prev_xr = 0;
    int stalled = 0;
    (void)arg;
    last_progress = last_beat = prev_t = black_android_now_ns();
    for (;;) {
        VrDiagCounters d;
        uint64_t kc[4] = { 0, 0, 0, 0 }, frames, icalls, kcalls;
        uint32_t get = 0, put = 0, kflags = 8;
        int64_t now;
        double dt1;
        sleep(1);
        now = black_android_now_ns();
        dt1 = (double)(now - prev_t) * 1e-9;
        memset(&d, 0, sizeof(d));
        if (vr_diag_counters)
            vr_diag_counters(&d);
        if (kelvin_diag_state)
            kelvin_diag_state(kc, &get, &put, &kflags);
        frames = vr_diag_counters ? d.eye_releases + d.flat_releases : kc[2];
        icalls = g_icall_count;
        kcalls = xbox_kernel_call_total ? xbox_kernel_call_total() : 0;
        if (frames != prev_frames) {
            if (stalled)
                black_vr_log(ANDROID_LOG_INFO, "STALL over: frames again after %.0f s",
                                    (double)(now - last_progress) * 1e-9);
            stalled = 0;
            prev_frames = frames;
            last_progress = now;
        }
        if (now - last_beat >= 10000000000LL) {
            const double dt = (double)(now - last_beat) * 1e-9;
            black_vr_log(ANDROID_LOG_INFO,
                                "HEARTBEAT t=%.0f s: frames %llu (%.1f/s; stereo %llu, flat %llu, game frame %u, mode %d), "
                                "XR loop %.1f/s; guest icalls %.0f/s, kernel calls %.0f/s; Kelvin flips %llu, %.0f words/s, "
                                "draws %llu, PUT 0x%08X GET 0x%08X; RSS %ld MB", black_android_uptime(),
                                (unsigned long long)frames, (double)(frames - beat_frames) / dt,
                                (unsigned long long)d.eye_releases, (unsigned long long)d.flat_releases, d.last_game_frame,
                                d.present_mode, (double)(d.xr_frames - beat_xr) / dt,
                                (double)(icalls - beat_icalls) / dt, (double)(kcalls - beat_kcalls) / dt,
                                (unsigned long long)kc[2], (double)(kc[0] - beat_words) / dt, (unsigned long long)kc[3],
                                put, get, black_android_rss_mb());
            black_android_thread_line(dt);
            beat_frames = frames;
            beat_xr = d.xr_frames;
            beat_icalls = icalls;
            beat_kcalls = kcalls;
            beat_words = kc[0];
            last_beat = now;
        }
        if (now - last_progress >= 5000000000LL && now - last_stall_log >= 5000000000LL) {
            stalled = 1;
            last_stall_log = now;
            black_android_stall_report((double)(now - last_progress) * 1e-9, frames,
                                       dt1 > 0 ? (double)(icalls - prev_icalls) / dt1 : 0,
                                       dt1 > 0 ? (double)(kcalls - prev_kcalls) / dt1 : 0, kc[0] - prev_words, kc, get, put,
                                       kflags, &d, dt1 > 0 ? (double)(d.xr_frames - prev_xr) / dt1 : 0);
            if (!last_dump || now - last_dump >= 30000000000LL) {
                fprintf(stderr, "[STALL] guest threads (every 30 s of a stall):\n");
                xbox_DumpGuestThreads();
                last_dump = now;
            }
        }
        prev_icalls = icalls;
        prev_kcalls = kcalls;
        prev_words = kc[0];
        prev_xr = d.xr_frames;
        prev_t = now;
    }
    return NULL;
}

/* On the game thread (its guest registers are thread-local), before the entry point. */
static void black_android_progress_start(void)
{
    const char *opt = getenv("BLACKVR_HEARTBEAT");
    pthread_attr_t at;
    pthread_t th;
    struct sigaction sa;
    if (opt && *opt == '0')
        return;
    s_pw_esp = &g_esp;
    s_pw_eax = &g_eax;
    s_pw_game = pthread_self();
    if (pthread_getattr_np(pthread_self(), &at) == 0) {
        void *lo = NULL;
        size_t sz = 0;
        if (pthread_attr_getstack(&at, &lo, &sz) == 0 && lo) {
            s_pw_stack_lo = (uintptr_t)lo;
            s_pw_stack_hi = (uintptr_t)lo + sz;
        }
        pthread_attr_destroy(&at);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = black_android_sample_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGURG, &sa, NULL) != 0)
        s_pw_stack_hi = 0;                       /* no sampling, the rest still works */
    if (pthread_create(&th, NULL, black_android_progress_watch, NULL) == 0) {
        pthread_setname_np(th, "bvr-watch");
        pthread_detach(th);
    }
}

static void black_android_install_crash_reporter(void)
{
    struct sigaction sa;
    size_t i;
    if (!a64_fault_install() || !a64_fault_add_handler(black_android_crash_fault))
        fprintf(stderr, "[BOOT] SIGSEGV dispatcher unavailable: no crash report\n");
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = black_android_fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(black_android_fatal_signals) / sizeof(int); i++)
        sigaction(black_android_fatal_signals[i], &sa, &black_android_prev_actions[i]);
}
#endif /* __ANDROID__ */

#if defined(__ANDROID__)
#define MessageBoxA(owner, text, caption, flags) black_android_fatal((text), (caption))
#endif

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    RECOMP_ICALL_FEEDBACK_INIT();

    printf("=== BLACK - Static Recompilation %s ===\n", BLACK_VERSION);
    printf("Loading XBE...\n");

#if defined(__ANDROID__)
    /* The crash reporter first, so every device handler added later (AC97 in
     * MemoryLayoutInit, the APU below, OHCI, Kelvin) runs before it. */
    black_android_install_crash_reporter();
    if (getenv("RECOMP_FPE_TRAP"))
        fprintf(stderr, "[BOOT] RECOMP_FPE_TRAP needs the Windows build; ignored\n");
#else
    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    if (getenv("RECOMP_FPE_TRAP")) AddVectoredExceptionHandler(1, fpe_veh);
#endif

    /* Step 1: Load XBE */
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing PC memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize PC memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("PC memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);
#if defined(__ANDROID__)
    /* The APU registers, which veh_handler services on Windows. After
     * MemoryLayoutInit, per port/notes/arm64-fault.md. */
    if (!a64_fault_add_handler(apu_fault))
        fprintf(stderr, "[BOOT] could not register the APU fault handler\n");
    black_android_log_memory("memory layout up");
    {
        pthread_t mt;
        if (pthread_create(&mt, NULL, black_android_memory_watch, NULL) == 0)
            pthread_detach(mt);
    }
#endif

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, notifiers and sample data,
     * so it gets physical address 0 as the title's MmGetPhysicalAddress
     * sees it: the contiguous window. Given the guest RAM base it read the
     * low guest addresses, which are separate storage, so DirectSound's
     * voice links, samples and notifiers never reached it or came back. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone_sized(
            (uint8_t *)xbox_GetPhysicalMemoryBase(),
            xbox_GetPhysicalMemorySize());
        /* Its interrupt goes to the kernel's APU vector, where DirectSound
         * connects the ISR that completes stream packets. */
        if (g_apu_state) {
            /* BLACK sends its primary stereo mix to custom DSP bins 19/20. */
            mcpx_apu_set_stereo_fallback_pair(g_apu_state, 19, 20);
            /* Register before the title can activate or connect the APU ISR.
             * The APU stays alive until process exit; if explicit retirement
             * is added, stop kernel delivery before removing this guard. */
            xbox_SetInterruptServiceGuard(XBOX_APU_VECTOR,
                                          mcpx_apu_irq_service_begin,
                                          mcpx_apu_irq_service_end, g_apu_state);
            mcpx_apu_set_irq_line(apu_irq_line);
            xbox_AddHangReport(mcpx_apu_dump_state);
            xbox_AddHangThread(mcpx_apu_frame_thread_id(), "APU frame");
        }
        fprintf(stderr, "[BOOT] emulated APU %s (physical RAM backing=%zu bytes)\n",
                g_apu_state ? "up" : "FAILED to initialise",
                xbox_GetPhysicalMemorySize());
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing PC kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
#if defined(__ANDROID__)
        xbox_path_init(YOUR_GAME_DIR, black_android_save_dir);
#else
        xbox_path_init(YOUR_GAME_DIR, "save");
#endif
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();
#if defined(__ANDROID__)
    /* the progress heartbeat / stall report (tag BlackVR), on by default */
    black_android_progress_start();
#endif

    /* Before the guest reads its first pad report: keyboard actions resolve through the live map. */
    pc_input_set_control_resolver(black_pc_input_resolver);
    pc_input_set_gameplay_blocked_query(black_pc_input_gameplay_blocked_query);
    black_controls_install();
    black_photo_install();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

#if defined(__ANDROID__)
/* Quest: the XR shell (port/quest/src/xr_main.c) runs this on its own 16 MB
 * game thread, with the folder holding game/ and save/ as the working
 * directory. argv[1], if present, names that folder instead. */
int black_game_main(int argc, char **argv)
{
    black_android_t0_ns = black_android_now_ns();
    black_android_stdio_to_logcat();
    black_android_paths(argc, argv);
    black_android_env_defaults();
    return WinMain(NULL, NULL, (LPSTR)"", 0);
}
#else
/* Console entry point (for debugging -- lets you see printf output) */
#ifdef BLACK_VR
/* Quest: the XR shell (port/quest/src/xr_main.c) runs this on its own game thread,
 * with the folder holding game/ and save/ as the working directory. */
int black_game_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
#endif /* __ANDROID__ */
