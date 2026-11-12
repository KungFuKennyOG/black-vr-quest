/*
 * memory_watch.c -- who touches this guest memory?
 *
 * RECOMP_WATCH=<guest addr>[,<bytes>] logs every read or write of that range
 * with the recompiled function doing it, which names the guest function
 * (sub_<guest address>), and after a write the dword it left. RECOMP_WATCH_MAX
 * caps the accesses logged (default 200).
 *
 * The question it answers is one a register dump and a stack cannot: which
 * code consumes a value. BLACK's controller object held START as 1.0 while
 * its title screen kept asking for START, and the code that should read the
 * field was the thing to find.
 *
 * How: the page holding the range is made a guard page. Any access to it
 * faults once and drops the guard; the handler logs the access if it hit the
 * range, sets the trap flag, and re-arms the guard on the single step after
 * the instruction completes. Every access to that page pays two exceptions,
 * so this is a diagnostic, and off unless set.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <dbghelp.h>
#if defined(_MSC_VER)
#pragma comment(lib, "dbghelp.lib")
#endif

static uint32_t s_watch_va, s_watch_len;
static uintptr_t s_page;                 /* host address of the guarded page */
static volatile LONG s_budget = 200;
static __declspec(thread) int s_rearm;   /* this thread stepped over it */
static __declspec(thread) uint32_t s_wrote; /* write to report after the step */

static void watch_arm(void)
{
    DWORD old;
    VirtualProtect((LPVOID)s_page, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
}

static void watch_log(PEXCEPTION_POINTERS ep, uint32_t va, int is_write)
{
    char buf[sizeof(SYMBOL_INFO) + 128];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    DWORD64 rip = ep->ContextRecord->Rip;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 127;
    if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym))
        fprintf(stderr, "[WATCH] %s 0x%08X (+0x%X) by %s+0x%llX tid %lu\n",
                is_write ? "write" : "read ", va, va - s_watch_va, sym->Name,
                (unsigned long long)disp, GetCurrentThreadId());
    else
        fprintf(stderr, "[WATCH] %s 0x%08X (+0x%X) by %p tid %lu\n",
                is_write ? "write" : "read ", va, va - s_watch_va,
                (void *)(uintptr_t)rip, GetCurrentThreadId());
    fflush(stderr);
}

static LONG CALLBACK watch_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    if (code == STATUS_GUARD_PAGE_VIOLATION
            && ep->ExceptionRecord->NumberParameters >= 2) {
        uintptr_t fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        if (fault < s_page || fault >= s_page + 4096)
            return EXCEPTION_CONTINUE_SEARCH;
        {
            uint32_t va = (uint32_t)(fault - (uintptr_t)xbox_GetMemoryOffset());
            int is_write = ep->ExceptionRecord->ExceptionInformation[0] != 0;
            if (va >= s_watch_va && va - s_watch_va < s_watch_len
                    && InterlockedDecrement(&s_budget) >= 0) {
                watch_log(ep, va, is_write);
                if (is_write)
                    s_wrote = va;       /* say what landed, after the step */
            }
        }
        /* The guard is gone now; step this one instruction, then re-arm. */
        s_rearm = 1;
        ep->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == STATUS_SINGLE_STEP && s_rearm) {
        s_rearm = 0;
        if (s_wrote) {
            uint32_t at = s_wrote & ~3u;
            fprintf(stderr, "[WATCH]   now [0x%08X] = %08X\n", at,
                    *(volatile uint32_t *)((uintptr_t)at
                                           + (uintptr_t)xbox_GetMemoryOffset()));
            fflush(stderr);
            s_wrote = 0;
        }
        watch_arm();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void xbox_MemoryWatchInit(void)
{
    const char *spec = getenv("RECOMP_WATCH");
    const char *max = getenv("RECOMP_WATCH_MAX");
    char *end;

    if (!spec || !*spec)
        return;
    s_watch_va = (uint32_t)strtoul(spec, &end, 0);
    s_watch_len = (*end == ',') ? (uint32_t)strtoul(end + 1, NULL, 0) : 4;
    if (!s_watch_len)
        s_watch_len = 4;
    if (max)
        s_budget = strtol(max, NULL, 0);
    s_page = ((uintptr_t)s_watch_va + (uintptr_t)xbox_GetMemoryOffset())
             & ~(uintptr_t)0xFFF;
    if (((uintptr_t)s_watch_va + (uintptr_t)xbox_GetMemoryOffset() + s_watch_len
            - 1) >= s_page + 4096)
        fprintf(stderr, "[WATCH] range crosses a page; only the first is"
                        " watched\n");
    AddVectoredExceptionHandler(1, watch_veh);
    watch_arm();
    fprintf(stderr, "[WATCH] watching 0x%08X, %u bytes (at most %ld lines)\n",
            s_watch_va, s_watch_len, (long)s_budget);
    fflush(stderr);
}
#elif defined(__aarch64__)
/* AArch64 Linux/Android. There is no trap flag to single-step with from
 * user mode, so instead of guard + step + re-arm, the page stays PROT_NONE
 * and the faulting instruction is emulated against the real memory by
 * platform/arm64_fault_decode.c: open the page, perform the access, close
 * it. A thread touching the page in that window is not seen -- this is a
 * diagnostic, as on Windows. Symbols come from dladdr, so only exported
 * names resolve; the PC is printed either way. */
#include "platform/arm64_fault_decode.h"
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

static uint32_t s_watch_va, s_watch_len;
static uintptr_t s_page;
static long s_page_size = 4096;
static volatile long s_budget = 200;

static uint64_t watch_rd(void *c, uint64_t addr, int size)
{
    uint64_t v = 0;
    (void)c;
    switch (size) {
    case 1: v = *(volatile uint8_t  *)(uintptr_t)addr; break;
    case 2: v = *(volatile uint16_t *)(uintptr_t)addr; break;
    case 4: v = *(volatile uint32_t *)(uintptr_t)addr; break;
    default: v = *(volatile uint64_t *)(uintptr_t)addr; break;
    }
    return v;
}

static void watch_wr(void *c, uint64_t addr, int size, uint64_t v)
{
    (void)c;
    switch (size) {
    case 1: *(volatile uint8_t  *)(uintptr_t)addr = (uint8_t)v; break;
    case 2: *(volatile uint16_t *)(uintptr_t)addr = (uint16_t)v; break;
    case 4: *(volatile uint32_t *)(uintptr_t)addr = (uint32_t)v; break;
    default: *(volatile uint64_t *)(uintptr_t)addr = v; break;
    }
}

static int watch_fault(void *uc, uintptr_t fault)
{
    static const a64_mem_ops ops = { watch_rd, watch_wr };
    a64_access a;
    a64_regs r;
    uint32_t va;
    uint64_t start;
    int have;

    if (fault < s_page || fault >= s_page + (uintptr_t)s_page_size)
        return 0;
    if (!a64_decode(a64_uc_insn(uc), &a))
        return 0;
    have = a64_uc_get(uc, &r);
    if (a.is_fp && !(have & A64_UC_HAVE_FPSIMD))
        return 0;
    start = a64_address(&a, &r);
    /* Both ends of the access must be on the watched page: the open/close
     * below covers that page only. */
    if (start < s_page || start + a64_span(&a) > s_page + (uintptr_t)s_page_size)
        return 0;

    va = (uint32_t)(fault - (uintptr_t)xbox_GetMemoryOffset());
    if (va >= s_watch_va && va - s_watch_va < s_watch_len
            && __atomic_sub_fetch(&s_budget, 1, __ATOMIC_SEQ_CST) >= 0) {
        Dl_info info;
        uintptr_t pc = (uintptr_t)a64_uc_pc(uc);
        if (dladdr((void *)pc, &info) && info.dli_sname)
            fprintf(stderr, "[WATCH] %s 0x%08X (+0x%X) by %s+0x%lX tid %d\n",
                    a.is_write ? "write" : "read ", va, va - s_watch_va,
                    info.dli_sname, (unsigned long)(pc - (uintptr_t)info.dli_saddr),
                    (int)gettid());
        else
            fprintf(stderr, "[WATCH] %s 0x%08X (+0x%X) by %p tid %d\n",
                    a.is_write ? "write" : "read ", va, va - s_watch_va,
                    (void *)pc, (int)gettid());
    } else {
        va = 0;
    }

    mprotect((void *)s_page, (size_t)s_page_size, PROT_READ | PROT_WRITE);
    a64_execute(&r, &a, &ops, NULL);
    if (va && a.is_write) {
        uint32_t at = va & ~3u;
        fprintf(stderr, "[WATCH]   now [0x%08X] = %08X\n", at,
                *(volatile uint32_t *)((uintptr_t)at
                                       + (uintptr_t)xbox_GetMemoryOffset()));
    }
    mprotect((void *)s_page, (size_t)s_page_size, PROT_NONE);
    a64_uc_put(uc, &r, a64_fp_dest_mask(&a));
    return 1;
}

void xbox_MemoryWatchInit(void)
{
    const char *spec = getenv("RECOMP_WATCH");
    const char *max = getenv("RECOMP_WATCH_MAX");
    char *end;
    long ps = sysconf(_SC_PAGESIZE);

    if (!spec || !*spec)
        return;
    if (ps > 0)
        s_page_size = ps;
    s_watch_va = (uint32_t)strtoul(spec, &end, 0);
    s_watch_len = (*end == ',') ? (uint32_t)strtoul(end + 1, NULL, 0) : 4;
    if (!s_watch_len)
        s_watch_len = 4;
    if (max)
        s_budget = strtol(max, NULL, 0);
    s_page = ((uintptr_t)s_watch_va + (uintptr_t)xbox_GetMemoryOffset())
             & ~(uintptr_t)(s_page_size - 1);
    if (((uintptr_t)s_watch_va + (uintptr_t)xbox_GetMemoryOffset() + s_watch_len
            - 1) >= s_page + (uintptr_t)s_page_size)
        fprintf(stderr, "[WATCH] range crosses a page; only the first is"
                        " watched\n");
    if (!a64_fault_install() || !a64_fault_add_handler(watch_fault)) {
        fprintf(stderr, "[WATCH] no fault handler; not watching\n");
        return;
    }
    mprotect((void *)s_page, (size_t)s_page_size, PROT_NONE);
    fprintf(stderr, "[WATCH] watching 0x%08X, %u bytes (at most %ld lines)\n",
            s_watch_va, s_watch_len, (long)s_budget);
    fflush(stderr);
}
#else
void xbox_MemoryWatchInit(void) {}
#endif
