/*
 * tests/device_preflight: the Quest app's process-level preflight (port/quest/src/bvr_diag.c) run as a plain program,
 * with the runtime's real SIGSEGV dispatcher -- arm64_fault_decode.c as a static arm64 binary under QEMU
 * (/root/xtool/arm64run), or tests/game_render_x86's x86-64 glue natively. Every PREFLIGHT line is printed as the
 * app logs it. Checks: each line is emitted, the deliberate trap round trip (load, store, nested trap) works on the
 * main thread and on a second thread with the larger alternate signal stack, the guest window and its memfd mirrors
 * map, the JIT path runs, the game folder search reports every path and catches an XBE that does not match.
 *
 * Usage: test_preflight <scratch dir>     (everything it creates lives there)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bvr_diag.h"

/* ---- liblog stand-in: lines go to stdout, and are kept for the checks ------------------------------------------- */
static char s_lines[256][400];
static int s_nlines;
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%d %s: %s\n", prio, tag, line);
    fflush(stdout);
    if (s_nlines < 256) snprintf(s_lines[s_nlines++], sizeof(s_lines[0]), "%s", line);
    return 0;
}
#if defined(__x86_64__)
ptrdiff_t g_xbox_mem_offset;     /* x86_fault_glue.c's per-fault accounting reads it */
#endif

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("CHECK FAILED %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); s_fail++; } } while (0)

static const char *find_line(const char *prefix)
{
    int i;
    for (i = s_nlines - 1; i >= 0; i--) if (!strncmp(s_lines[i], prefix, strlen(prefix))) return s_lines[i];
    return NULL;
}
static int line_is(const char *name, const char *status)
{
    char want[128];
    const char *l;
    snprintf(want, sizeof(want), "PREFLIGHT %s: ", name);
    l = find_line(want);
    return l && !strncmp(l + strlen(want), status, strlen(status));
}

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s: %s\n", path, strerror(errno)); exit(2); }
    fwrite(data, 1, n, f);
    fclose(f);
}

static void *second_thread(void *arg)
{
    size_t *alt = (size_t *)arg;
    *alt = bvr_thread_altstack(128 * 1024);
    alt[1] = (size_t)bvr_pf_sigsegv("sigsegv-thread");
    return NULL;
}

int main(int argc, char **argv)
{
    char root[512], a[600], b[600], c[600], p[700];
    const char *cands[4];
    BvrGameDir gd;
    unsigned char xbe[0x400];
    pthread_t th;
    pthread_attr_t at;
    size_t alt[2] = { 0, 0 };
    const BvrPfSummary *sum;
    if (argc < 2) { printf("usage: test_preflight <scratch dir>\n"); return 2; }
    snprintf(root, sizeof(root), "%s", argv[1]);
    mkdir(root, 0770);

    bvr_pf_begin("device_preflight test");
    CHECK(find_line("PREFLIGHT begin: BLACK VR device_preflight test"), "begin line");

    /* game folder: a missing candidate, an inaccessible-looking one is not portable (root), a good one */
    snprintf(a, sizeof(a), "%s/nothing_here", root);
    snprintf(b, sizeof(b), "%s/BlackVR", root);
    snprintf(c, sizeof(c), "%s/BlackVR/../BlackVR", root);    /* the same folder by another name: tried once */
    snprintf(p, sizeof(p), "%s/game", b);
    mkdir(b, 0770);
    mkdir(p, 0770);
    snprintf(p, sizeof(p), "%s/game/levels", b);
    mkdir(p, 0770);
    memset(xbe, 0, sizeof(xbe));
    memcpy(xbe, "XBEH", 4);
    {
        uint32_t base = 0x10000, cert = 0x10000 + 0x200, title = 0x45410083u;
        int k;
        for (k = 0; k < 16; k++) xbe[4 + k] = (unsigned char)(0xA0 + k);
        memcpy(xbe + 0x104, &base, 4);
        memcpy(xbe + 0x118, &cert, 4);
        memcpy(xbe + 0x200 + 8, &title, 4);
    }
    snprintf(p, sizeof(p), "%s/game/default.xbe", b);
    write_file(p, xbe, sizeof(xbe));
    cands[0] = a; cands[1] = b; cands[2] = c; cands[3] = NULL;
    CHECK(bvr_pf_gamedir(cands, 3, (long long)sizeof(xbe), "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf", &gd) == 1, "gamedir found");
    CHECK(gd.found && !strcmp(gd.root, b), "gamedir root %s", gd.root);
    CHECK(line_is("gamedir", "OK"), "gamedir OK line");
    CHECK(strstr(find_line("PREFLIGHT gamedir | tried"), "nothing_here/game: ENOENT") != NULL, "missing candidate reported");
    CHECK(strstr(find_line("PREFLIGHT gamedir: OK"), "title 0x45410083") != NULL, "title id read");
    /* an XBE the APK was not built from */
    CHECK(bvr_pf_gamedir(cands, 3, (long long)sizeof(xbe) + 1, NULL, &gd) == 1, "gamedir found (size mismatch)");
    CHECK(line_is("gamedir", "FAIL") && strstr(find_line("PREFLIGHT gamedir: FAIL"), "SIZE MISMATCH"), "size mismatch");
    /* nothing found */
    cands[1] = a;
    CHECK(bvr_pf_gamedir(cands, 2, 0, NULL, &gd) == 0 && line_is("gamedir", "FAIL"), "gamedir missing");

    snprintf(p, sizeof(p), "%s/save", b);
    mkdir(p, 0770);
    bvr_pf_storage(b, p, root);
    CHECK(line_is("storage", "OK") || line_is("storage", "WARN"), "storage line");
    CHECK(strstr(find_line("PREFLIGHT storage:"), "writable") != NULL, "save folder writable");

    bvr_pf_memory(-1, -1, 0, 0, 0);
    CHECK(line_is("memory", "OK") || line_is("memory", "WARN"), "memory line");

    CHECK(bvr_pf_addrspace() == 1 && line_is("addrspace", "OK"), "addrspace");
    CHECK(bvr_pf_sigsegv("sigsegv") == 1 && line_is("sigsegv", "OK"), "trap round trip, main thread");
    bvr_pf_jit();
    CHECK(line_is("jit", "OK"), "jit");
    bvr_pf_syscalls();
    CHECK(line_is("syscalls", "OK"), "syscalls");

    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1u << 20);
    CHECK(pthread_create(&th, &at, second_thread, alt) == 0, "thread");
    pthread_join(th, NULL);
    CHECK(alt[0] >= 128 * 1024, "alternate signal stack %zu", alt[0]);
    CHECK(alt[1] == 1 && line_is("sigsegv-thread", "OK"), "trap round trip, second thread");

    sum = bvr_pf_summary();
    bvr_pf_summary_line("test");
    CHECK(sum->fatal == 0, "no fatal FAIL (%s)", sum->fatal_names);
    CHECK(sum->fail == 2, "two intentional gamedir FAILs, got %d (%s)", sum->fail, sum->fail_names);
    CHECK(find_line("PREFLIGHT summary:") != NULL, "summary line");
    printf("%s: %d check(s) failed\n", s_fail ? "FAIL" : "PASS", s_fail);
    return s_fail ? 1 : 0;
}
