#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>
#include <sys/mman.h>
static volatile int g_code;
static volatile uintptr_t g_addr; static volatile int g_hits; static int g_magic_ok;
static void h(int sig, siginfo_t *si, void *uc_) {
  ucontext_t *uc = uc_;
  g_addr = (uintptr_t)si->si_addr; g_code = si->si_code; g_hits++;
  struct _aarch64_ctx *c = (struct _aarch64_ctx *)uc->uc_mcontext.__reserved;
  if (c->magic == 0x46508001) { /* FPSIMD_MAGIC */
    struct fpsimd_context *f = (struct fpsimd_context *)c; g_magic_ok = 1;
    f->vregs[1] = 0x1234; /* modify v1 */
  }
  /* emulate the load: ldr x9, [x8] -> x9 = 0xdeadbeef */
  uint32_t insn = *(uint32_t *)uc->uc_mcontext.pc;
  int rt = insn & 31;
  int is_load = (insn >> 22) & 1; /* LDR/STR (imm, unsigned offset): bit22 = L */
  if (is_load && rt != 31) uc->uc_mcontext.regs[rt] = 0xdeadbeefcafeULL;
  if (uc->uc_mcontext.fault_address != g_addr) g_hits += 100;
  uc->uc_mcontext.pc += 4;
  (void)sig;
}
int main(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, 0)) { perror("sigaction"); return 1; }
  uint64_t out, v1out;
  uintptr_t bad = 0x7e5a0000ULL;  /* unmapped */
  __asm__ volatile(
    "fmov d1, xzr\n"
    "mov x8, %2\n"
    "ldr x9, [x8, #0x10]\n"
    "mov %0, x9\n"
    "fmov %1, d1\n"
    : "=r"(out), "=r"(v1out) : "r"(bad) : "x8", "x9", "v1", "memory");
  printf("hits=%d si_addr=%#lx x9=%#llx v1=%#llx fpsimd_magic=%d\n", g_hits, (unsigned long)g_addr,
         (unsigned long long)out, (unsigned long long)v1out, g_magic_ok);
  int ok = g_hits == 1 && g_addr == bad + 0x10 && out == 0xdeadbeefcafeULL && v1out == 0x1234 && g_magic_ok;
  /* store fault too */
  __asm__ volatile("mov x8, %0\nstr xzr, [x8]\n" :: "r"(bad + 0x100) : "x8", "memory");
  ok = ok && g_hits == 2 && g_addr == bad + 0x100;
  /* PROT_NONE page inside a 4 GiB reservation (MMIO-window style): expect SEGV_ACCERR */
  unsigned char *res = mmap(0, 1ULL << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  ok = ok && res != MAP_FAILED;
  if (res != MAP_FAILED) {
    mprotect(res, 1 << 20, PROT_READ | PROT_WRITE); res[0] = 7;
    uintptr_t mmio = (uintptr_t)res + 0xFD000000ULL; uint64_t v;
    __asm__ volatile("mov x8, %1\nldr w10, [x8, #4]\nmov %0, x10\n" : "=r"(v) : "r"(mmio) : "x8", "x10", "memory");
    printf("prot_none: si_addr=%#lx code=%d (SEGV_ACCERR=%d) v=%#llx\n", (unsigned long)g_addr, g_code, SEGV_ACCERR, (unsigned long long)v);
    ok = ok && g_hits == 3 && g_addr == mmio + 4 && g_code == SEGV_ACCERR && (uint32_t)v == 0xbeefcafe && res[0] == 7;
  }
  printf(ok ? "SEGV TEST PASS\n" : "SEGV TEST FAIL\n");
  return ok ? 0 : 1;
}
