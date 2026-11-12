/*
 * fault_bench.c -- tests/perf: the cost of one trapped MMIO access (SIGSEGV round trip), measured through the same
 * dispatcher and emulator the game uses:
 *
 *   AArch64 (Quest; here under /root/xtool/arm64run): platform/arm64_fault_decode.c's a64_fault_install /
 *       a64_fault_add_handler / a64_uc_emulate_mmio, i.e. the production path minus ART's libsigchain.
 *   x86-64 Linux: tests/game_render_x86/x86_fault_glue.c (mmio_decode.h's x86 decoder), the headless run's path.
 *
 * It times, per access:
 *   plain     a load from an ordinary page (the floor)
 *   segv      a load from a PROT_NONE "device" page serviced by the handler (device returns a counter)
 *   segv-st   a store to it
 *   empty     the kernel round trip alone: a handler that only steps the PC past the instruction
 *   sigprocmask  one rt_sigprocmask syscall (libsigchain adds one per chained signal on Android)
 * and prints ns/access. Usage: fault_bench [iterations]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>

#include "arm64_fault_decode.h"

#if !defined(__aarch64__)
ptrdiff_t g_xbox_mem_offset;     /* x86_fault_glue.c's fault accounting reads it (guest VA = fault - offset) */
#endif

static uint8_t *g_dev;
static volatile uint64_t g_reads, g_writes;
static int g_mode;      /* 0: emulate through the production glue; 1: just step the PC */

static uint64_t dev_read(void *d, uint32_t off, int size) { (void)d; (void)size; return ++g_reads + off; }
static void dev_write(void *d, uint32_t off, uint64_t v, int size) { (void)d; (void)off; (void)v; (void)size; g_writes++; }

static int bench_fault(void *uc, uintptr_t fault)
{
    if (fault < (uintptr_t)g_dev || fault >= (uintptr_t)g_dev + 4096) return 0;
    if (g_mode == 1) {
#if defined(__aarch64__)
        ((ucontext_t *)uc)->uc_mcontext.pc += 4;
#else
        ((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP] += 2;     /* mov (%rdi),%eax / mov %eax,(%rdi): 2 bytes */
#endif
        return 1;
    }
    return a64_uc_emulate_mmio(uc, fault, (uint32_t)(fault - (uintptr_t)g_dev), 4096, NULL, dev_read, dev_write);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static inline uint32_t ld(volatile uint32_t *p)
{
    uint32_t v;
#if defined(__aarch64__)
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
#else
    __asm__ volatile("movl (%1), %0" : "=r"(v) : "D"(p) : "memory");      /* 8b 07: 2 bytes */
#endif
    return v;
}
static inline void st(volatile uint32_t *p, uint32_t v)
{
#if defined(__aarch64__)
    __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(p) : "memory");
#else
    __asm__ volatile("movl %0, (%1)" : : "a"(v), "D"(p) : "memory");      /* 89 07: 2 bytes */
#endif
}

int main(int argc, char **argv)
{
    const long n = argc > 1 ? atol(argv[1]) : 200000;
    static uint32_t plain[1024];
    volatile uint32_t *dev;
    uint64_t sum = 0;
    double t0, t_plain, t_segv, t_st, t_empty, t_mask;
    long i;
    sigset_t s;

    g_dev = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_dev == MAP_FAILED || !a64_fault_install() || !a64_fault_add_handler(bench_fault)) {
        fprintf(stderr, "setup failed\n");
        return 1;
    }
    dev = (volatile uint32_t *)(g_dev + 0x100);

    t0 = now();
    for (i = 0; i < n; i++) sum += ld((volatile uint32_t *)&plain[i & 1023]);
    t_plain = now() - t0;

    for (i = 0; i < 1000; i++) sum += ld(dev);                              /* warm */
    g_reads = 0;
    t0 = now();
    for (i = 0; i < n; i++) sum += ld(dev);
    t_segv = now() - t0;
    if (g_reads != (uint64_t)n) { fprintf(stderr, "FAIL: %llu device reads for %ld loads\n", (unsigned long long)g_reads, n); return 1; }

    t0 = now();
    for (i = 0; i < n; i++) st(dev, (uint32_t)i);
    t_st = now() - t0;
    if (g_writes != (uint64_t)n) { fprintf(stderr, "FAIL: %llu device writes for %ld stores\n", (unsigned long long)g_writes, n); return 1; }

    g_mode = 1;
    t0 = now();
    for (i = 0; i < n; i++) sum += ld(dev);
    t_empty = now() - t0;

    t0 = now();
    for (i = 0; i < n; i++) { sigemptyset(&s); sigprocmask(SIG_BLOCK, &s, NULL); }
    t_mask = now() - t0;

    printf("fault_bench (%s, %ld iterations, checksum %llu)\n",
#if defined(__aarch64__)
           "aarch64: arm64_fault_decode.c dispatcher + a64_uc_emulate_mmio",
#else
           "x86-64: x86_fault_glue.c dispatcher + mmio_decode.h",
#endif
           n, (unsigned long long)(sum & 0xFFFF));
    printf("  plain load        %8.1f ns\n", 1e9 * t_plain / n);
    printf("  trapped load      %8.1f ns   (dispatcher + decode + device read)\n", 1e9 * t_segv / n);
    printf("  trapped store     %8.1f ns\n", 1e9 * t_st / n);
    printf("  bare round trip   %8.1f ns   (handler only steps the PC)\n", 1e9 * t_empty / n);
    printf("  rt_sigprocmask    %8.1f ns   (libsigchain adds one per chained signal)\n", 1e9 * t_mask / n);
    printf("RESULT trapped_load_ns=%.1f trapped_store_ns=%.1f bare_ns=%.1f sigprocmask_ns=%.1f\n", 1e9 * t_segv / n,
           1e9 * t_st / n, 1e9 * t_empty / n, 1e9 * t_mask / n);
    return 0;
}
