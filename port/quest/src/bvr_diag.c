/*
 * bvr_diag.c - BLACK VR device preflight, process-level checks (bvr_diag.h). One logcat line per check, tag BlackVR:
 * "PREFLIGHT <name>: OK|WARN|FAIL|SKIP <details>". What each line means and what to do on a FAIL:
 * port/notes/device-preflight.md.
 *
 * Everything here undoes itself: test mappings are unmapped, the test fault handler is disarmed, temporary signal
 * handlers are restored. Each check that could kill the process (a deliberate fault, a syscall a seccomp filter might
 * refuse) logs what it is about to do first, so a log that ends there still names the cause.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <android/log.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#if !defined(BVR_NO_LIBANDROID)
#include <android/sharedmem.h>
#endif
#endif

#include "bvr_diag.h"

#define PF_TAG "BlackVR"

/* ---- the runtime's SIGSEGV dispatcher (platform/arm64_fault_decode.h); weak: the smoke APK has no runtime ------- */
typedef int (*bvr_a64_handler)(void *uc, uintptr_t fault_addr);
typedef uint64_t (*bvr_a64_rd)(void *dev, uint32_t off, int size);
typedef void (*bvr_a64_wr)(void *dev, uint32_t off, uint64_t val, int size);
extern int a64_fault_install(void) __attribute__((weak));
extern int a64_fault_add_handler(bvr_a64_handler h) __attribute__((weak));
extern int a64_uc_emulate_mmio(void *uc, uintptr_t fault_addr, uint32_t fault_off, uint32_t dev_size, void *dev,
                               bvr_a64_rd rd, bvr_a64_wr wr) __attribute__((weak));

/* ---- logging ------------------------------------------------------------------------------------------------------ */
static BvrPfSummary s_sum;

static void pf_write(int prio, const char *fmt, va_list ap)
{
    char line[1800];
    vsnprintf(line, sizeof(line), fmt, ap);
    __android_log_print(prio, PF_TAG, "%s", line);
}

static void pf_logf(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pf_logf(int prio, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pf_write(prio, fmt, ap);
    va_end(ap);
}

static void append_name(char *dst, size_t cap, const char *name)
{
    size_t n = strlen(dst);
    if (n + strlen(name) + 2 < cap) snprintf(dst + n, cap - n, "%s%s", n ? " " : "", name);
}

BvrPfStatus bvr_pf_report(const char *name, BvrPfStatus st, int fatal, const char *fmt, ...)
{
    static const char *const word[] = { "OK", "WARN", "FAIL", "SKIP" };
    char body[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    pf_logf(st == BVR_PF_FAIL ? ANDROID_LOG_ERROR : st == BVR_PF_WARN ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
            "PREFLIGHT %s: %s %s%s", name, word[st], body, st == BVR_PF_FAIL && fatal ? " [FATAL]" : "");
    switch (st) {
    case BVR_PF_OK: s_sum.ok++; break;
    case BVR_PF_WARN: s_sum.warn++; break;
    case BVR_PF_FAIL:
        s_sum.fail++;
        append_name(s_sum.fail_names, sizeof(s_sum.fail_names), name);
        if (fatal) { s_sum.fatal++; append_name(s_sum.fatal_names, sizeof(s_sum.fatal_names), name); }
        break;
    default: s_sum.skip++; break;
    }
    return st;
}

void bvr_pf_detail(const char *name, const char *fmt, ...)
{
    char body[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    pf_logf(ANDROID_LOG_INFO, "PREFLIGHT %s | %s", name, body);
}

const BvrPfSummary *bvr_pf_summary(void) { return &s_sum; }

void bvr_pf_summary_line(const char *verdict)
{
    pf_logf(s_sum.fail ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
            "PREFLIGHT summary: %d OK, %d WARN, %d FAIL, %d SKIP%s%s%s%s -> %s", s_sum.ok, s_sum.warn, s_sum.fail,
            s_sum.skip, s_sum.fail ? " (failed: " : "", s_sum.fail_names, s_sum.fail ? ")" : "",
            s_sum.fatal ? " FATAL" : "", verdict ? verdict : "");
}

/* errno as its name: the one-line reports must survive translation-free reading on any device */
static const char *errname(int e)
{
    switch (e) {
    case 0: return "0";
    case EPERM: return "EPERM"; case ENOENT: return "ENOENT"; case EIO: return "EIO"; case EBADF: return "EBADF";
    case EAGAIN: return "EAGAIN"; case ENOMEM: return "ENOMEM"; case EACCES: return "EACCES"; case EFAULT: return "EFAULT";
    case EBUSY: return "EBUSY"; case EEXIST: return "EEXIST"; case ENODEV: return "ENODEV"; case ENOTDIR: return "ENOTDIR";
    case EISDIR: return "EISDIR"; case EINVAL: return "EINVAL"; case ENFILE: return "ENFILE"; case EMFILE: return "EMFILE";
    case EFBIG: return "EFBIG"; case ENOSPC: return "ENOSPC"; case EROFS: return "EROFS"; case ENOSYS: return "ENOSYS";
    case ELOOP: return "ELOOP"; case ENAMETOOLONG: return "ENAMETOOLONG"; case EOPNOTSUPP: return "EOPNOTSUPP";
    case EOVERFLOW: return "EOVERFLOW"; case ETXTBSY: return "ETXTBSY"; case ESRCH: return "ESRCH";
    default: return "E?";
    }
}
static char *errtxt(int e, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s %d (%s)", errname(e), e, strerror(e));
    return buf;
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int read_text(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0) { buf[0] = 0; return -1; }
    n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return (int)n;
}

/* "Key:   value kB" in a /proc file */
static long long proc_kb(const char *path, const char *key)
{
    char line[256];
    long long v = -1;
    const size_t kl = strlen(key);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, kl) && line[kl] == ':') { v = atoll(line + kl + 1); break; }
    fclose(f);
    return v;
}

/* ---- banner ------------------------------------------------------------------------------------------------------- */
#ifndef PR_GET_TAGGED_ADDR_CTRL
#define PR_GET_TAGGED_ADDR_CTRL 56
#endif

static void prop(const char *name, char *out, size_t cap)
{
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX];
    v[0] = 0;
    __system_property_get(name, v);
    snprintf(out, cap, "%s", v[0] ? v : "?");
#else
    (void)name;
    snprintf(out, cap, "%s", "n/a");
#endif
}

void bvr_pf_begin(const char *version)
{
    struct utsname u;
    char model[96], maker[64], rel[32], sdk[16], build[128], selinux[128], freqs[160] = "", seccomp[48];
    long ncpu = sysconf(_SC_NPROCESSORS_CONF), page = sysconf(_SC_PAGESIZE), i;
    int tag;
    void *probe;
    memset(&u, 0, sizeof(u));
    uname(&u);
    prop("ro.product.manufacturer", maker, sizeof(maker));
    prop("ro.product.model", model, sizeof(model));
    prop("ro.build.version.release", rel, sizeof(rel));
    prop("ro.build.version.sdk", sdk, sizeof(sdk));
    prop("ro.build.display.id", build, sizeof(build));
    if (read_text("/proc/self/attr/current", selinux, sizeof(selinux)) <= 0) snprintf(selinux, sizeof(selinux), "?");
    for (i = 0; i < ncpu && i < 16; i++) {
        char p[96], v[32];
        size_t n = strlen(freqs);
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
        if (read_text(p, v, sizeof(v)) <= 0) snprintf(v, sizeof(v), "?");
        else snprintf(v, sizeof(v), "%ld", atol(v) / 1000);
        snprintf(freqs + n, sizeof(freqs) - n, "%s%s", i ? "/" : "", v);
    }
    {
        long long sc = proc_kb("/proc/self/status", "Seccomp"), nf = proc_kb("/proc/self/status", "Seccomp_filters");
        snprintf(seccomp, sizeof(seccomp), "%s%s%lld", sc == 2 ? "filter" : sc == 1 ? "strict" : sc == 0 ? "off" : "?",
                 nf >= 0 ? " x" : "", nf >= 0 ? nf : 0);
    }
    tag = prctl(PR_GET_TAGGED_ADDR_CTRL, 0, 0, 0, 0);
    probe = malloc(64);
    pf_logf(ANDROID_LOG_INFO, "PREFLIGHT begin: BLACK VR %s, pid %d; %s %s, Android %s (SDK %s) %s; kernel %s %s %s; "
            "page %ld, %ld CPUs (max MHz %s); SELinux %s; seccomp %s; tagged-addr ABI %s, heap pointer %p",
            version ? version : "?", (int)getpid(), maker, model, rel, sdk, build, u.sysname, u.release, u.machine,
            page, ncpu, freqs, selinux, seccomp, tag < 0 ? "off" : (tag & 1) ? "on" : "off", probe);
    free(probe);
}

/* ---- game folder ------------------------------------------------------------------------------------------------ */
int bvr_pf_gamedir(const char *const *candidates, int n, long long expect_size, const char *expect_sig_hex,
                   BvrGameDir *out)
{
    char tried[1400] = "", real_seen[8][PATH_MAX];
    int i, nreal = 0;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < n; i++) {
        char dir[600], xbe[640], why[200], e[64], real[PATH_MAX];
        struct stat st;
        int fd, dup = 0, k;
        size_t tl;
        if (!candidates[i] || !*candidates[i]) continue;
        snprintf(dir, sizeof(dir), "%s/game", candidates[i]);
        snprintf(xbe, sizeof(xbe), "%s/default.xbe", dir);
        /* the same folder under another name (/sdcard -> /storage/emulated/0) is tried once */
        if (realpath(candidates[i], real)) {
            for (k = 0; k < nreal; k++) if (!strcmp(real_seen[k], real)) dup = 1;
            if (!dup && nreal < 8) snprintf(real_seen[nreal++], sizeof(real_seen[0]), "%s", real);
        }
        if (dup) {
            snprintf(why, sizeof(why), "same folder as an earlier candidate");
        } else if (stat(dir, &st) != 0) {
            const int err = errno;
            snprintf(why, sizeof(why), "%s", errtxt(err, e, sizeof(e)));
            if (err == EACCES || err == EPERM) {
                out->unreadable_seen = 1;
                snprintf(out->unreadable, sizeof(out->unreadable), "%s", candidates[i]);
            }
        } else if (!S_ISDIR(st.st_mode)) {
            snprintf(why, sizeof(why), "not a directory");
        } else if (access(dir, R_OK | X_OK) != 0) {
            snprintf(why, sizeof(why), "folder exists, no access: %s", errtxt(errno, e, sizeof(e)));
            out->unreadable_seen = 1;
            snprintf(out->unreadable, sizeof(out->unreadable), "%s", candidates[i]);
        } else if ((fd = open(xbe, O_RDONLY | O_CLOEXEC)) < 0) {
            const int err = errno;
            snprintf(why, sizeof(why), "folder OK, default.xbe: %s", errtxt(err, e, sizeof(e)));
            if (err == EACCES || err == EPERM) {
                out->unreadable_seen = 1;
                snprintf(out->unreadable, sizeof(out->unreadable), "%s", candidates[i]);
            }
        } else {
            unsigned char hdr[0x1000];
            ssize_t got = read(fd, hdr, sizeof(hdr));
            struct stat xs;
            const int have_size = fstat(fd, &xs) == 0;
            close(fd);
            if (got < 4) {
                snprintf(why, sizeof(why), "default.xbe unreadable (read %zd bytes: %s)", got, errtxt(errno, e, sizeof(e)));
            } else if (memcmp(hdr, "XBEH", 4) != 0) {
                snprintf(why, sizeof(why), "default.xbe is not an XBE (magic %02X %02X %02X %02X)", hdr[0], hdr[1],
                         hdr[2], hdr[3]);
            } else {
                char sig[40] = "", detail[1100], extra[200] = "";
                uint32_t base = 0, cert = 0, title = 0;
                int entries = 0, levels = 0, mism = 0;
                DIR *d;
                struct dirent *de;
                for (k = 0; k < 16 && 4 + k < got; k++) snprintf(sig + 2 * k, sizeof(sig) - 2 * (size_t)k, "%02x", hdr[4 + k]);
                if (got >= 0x11C) {
                    memcpy(&base, hdr + 0x104, 4);
                    memcpy(&cert, hdr + 0x118, 4);
                    if (cert > base && cert - base + 12 <= (uint32_t)got) memcpy(&title, hdr + (cert - base) + 8, 4);
                }
                if ((d = opendir(dir)) != NULL) {
                    while ((de = readdir(d)) != NULL) {
                        if (de->d_name[0] == '.') continue;
                        entries++;
                        if (!strcasecmp(de->d_name, "levels")) levels = 1;
                    }
                    closedir(d);
                }
                out->found = 1;
                out->xbe_size = have_size ? (long long)xs.st_size : -1;
                snprintf(out->root, sizeof(out->root), "%s", candidates[i]);
                snprintf(out->game_dir, sizeof(out->game_dir), "%s", dir);
                if (expect_size > 0 && out->xbe_size != expect_size) {
                    mism = 1;
                    snprintf(extra, sizeof(extra), "; SIZE MISMATCH: the APK was built from a %lld-byte XBE", expect_size);
                } else if (expect_sig_hex && *expect_sig_hex && strncasecmp(expect_sig_hex, sig, strlen(sig))) {
                    mism = 1;
                    snprintf(extra, sizeof(extra), "; SIGNATURE MISMATCH: the APK was built from XBE sig %.32s",
                             expect_sig_hex);
                }
                snprintf(detail, sizeof(detail), "%s (default.xbe %lld bytes, XBEH, title 0x%08X, sig %s; %d entries in "
                         "game/%s)%s", dir, out->xbe_size, title, sig, entries, levels ? ", levels/ present" : ", NO levels/",
                         extra);
                tl = strlen(tried);
                snprintf(tried + tl, sizeof(tried) - tl, "%s[%d] %s: found", tl ? "; " : "", i + 1, candidates[i]);
                bvr_pf_detail("gamedir", "tried %s", tried);
                if (mism)
                    bvr_pf_report("gamedir", BVR_PF_FAIL, 0, "%s -- the recompiled code matches another XBE: rebuild "
                                  "the APK from this disc's files, or copy the files the APK was built from", detail);
                else if (!levels)
                    bvr_pf_report("gamedir", BVR_PF_WARN, 0, "%s -- the extraction looks incomplete", detail);
                else
                    bvr_pf_report("gamedir", BVR_PF_OK, 0, "%s", detail);
                return 1;
            }
        }
        tl = strlen(tried);
        snprintf(tried + tl, sizeof(tried) - tl, "%s[%d] %s/game: %s", tl ? "; " : "", i + 1, candidates[i], why);
    }
    bvr_pf_report("gamedir", BVR_PF_FAIL, 0, "no game/default.xbe found (test pattern only); tried %s%s%s", tried,
                  out->unreadable_seen ? " -- access denied: grant All files access (adb shell appops set "
                                         "com.kenny.blackvr MANAGE_EXTERNAL_STORAGE allow) or copy to the first folder"
                                       : "",
                  "");
    return 0;
}

void bvr_pf_storage(const char *game_root, const char *save_dir, const char *internal_dir)
{
    const char *dirs[3] = { game_root, save_dir, internal_dir };
    static const char *const label[3] = { "game", "save", "internal" };
    char body[700] = "", e[64];
    long long min_free = -1;
    int i, writable = -1, werr = 0;
    for (i = 0; i < 3; i++) {
        struct statvfs v;
        size_t bl = strlen(body);
        if (!dirs[i] || !*dirs[i]) continue;
        if (statvfs(dirs[i], &v) == 0) {
            const long long free_mb = (long long)v.f_bavail * (long long)v.f_frsize / (1024 * 1024);
            const long long tot_mb = (long long)v.f_blocks * (long long)v.f_frsize / (1024 * 1024);
            snprintf(body + bl, sizeof(body) - bl, "%s%s %s: %lld MB free of %lld", bl ? ", " : "", label[i], dirs[i],
                     free_mb, tot_mb);
            if (i < 2 && (min_free < 0 || free_mb < min_free)) min_free = free_mb;
        } else {
            snprintf(body + bl, sizeof(body) - bl, "%s%s %s: statvfs %s", bl ? ", " : "", label[i], dirs[i],
                     errtxt(errno, e, sizeof(e)));
        }
    }
    /* the title writes save/ (kernel saves, Partition*.img, checkpoints, the shader caches are elsewhere) */
    if (save_dir && *save_dir) {
        char p[700];
        int fd;
        snprintf(p, sizeof(p), "%s/.preflight_write_test", save_dir);
        if ((fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660)) < 0) {
            writable = 0;
            werr = errno;
        } else {
            static const char k[] = "BLACK VR preflight\n";
            writable = write(fd, k, sizeof(k) - 1) == (ssize_t)(sizeof(k) - 1) && fsync(fd) == 0;
            if (!writable) werr = errno;
            close(fd);
            unlink(p);
        }
    }
    if (writable == 0)
        bvr_pf_report("storage", BVR_PF_FAIL, 0, "save folder %s NOT writable (%s): saves and the Z: partition image "
                      "fail -- %s", save_dir, errtxt(werr, e, sizeof(e)), body);
    else if (min_free >= 0 && min_free < 300)
        bvr_pf_report("storage", BVR_PF_FAIL, 0, "only %lld MB free (needs ~300 MB for saves, logs, caches): %s",
                      min_free, body);
    else if (min_free >= 0 && min_free < 1024)
        bvr_pf_report("storage", BVR_PF_WARN, 0, "low: %s; save folder writable", body);
    else
        bvr_pf_report("storage", BVR_PF_OK, 0, "%s; save folder %s", body, writable > 0 ? "writable" : "not checked");
}

/* ---- memory ------------------------------------------------------------------------------------------------------- */
void bvr_pf_memory(long java_class_mb, long java_large_class_mb, long long am_avail, long long am_threshold, int am_low)
{
    const long long total = proc_kb("/proc/meminfo", "MemTotal"), avail = proc_kb("/proc/meminfo", "MemAvailable");
    const long long swap_t = proc_kb("/proc/meminfo", "SwapTotal"), swap_f = proc_kb("/proc/meminfo", "SwapFree");
    const long long rss = proc_kb("/proc/self/status", "VmRSS"), vms = proc_kb("/proc/self/status", "VmSize");
    char am[200] = "", aslim[80] = "?";
    {
        /* RLIMIT_AS: the 4 GB guest reservation counts against it */
        FILE *f = fopen("/proc/self/limits", "r");
        char line[200];
        while (f && fgets(line, sizeof(line), f))
            if (!strncmp(line, "Max address space", 17)) {
                char *p = line + 17;
                while (*p == ' ') p++;
                if (!strncmp(p, "unlimited", 9)) snprintf(aslim, sizeof(aslim), "unlimited");
                else snprintf(aslim, sizeof(aslim), "%lld MB (the guest window needs 4096+)", atoll(p) >> 20);
            }
        if (f) fclose(f);
    }
    if (java_class_mb >= 0)
        snprintf(am, sizeof(am), "; ActivityManager: memory class %ld MB (large %ld), avail %lld MB, LMK threshold %lld MB%s",
                 java_class_mb, java_large_class_mb, am_avail / (1024 * 1024), am_threshold / (1024 * 1024),
                 am_low ? ", LOW MEMORY now" : "");
    {
        const BvrPfStatus st = (avail >= 0 && avail < 700 * 1024) || am_low ? BVR_PF_WARN : BVR_PF_OK;
        bvr_pf_report("memory", st, 0, "MemTotal %lld MB, MemAvailable %lld MB, swap %lld/%lld MB free; this process RSS "
                      "%lld MB, VmSize %lld MB; address-space limit %s%s%s", total / 1024, avail / 1024, swap_f / 1024,
                      swap_t / 1024, rss / 1024, vms / 1024, aslim, am,
                      st == BVR_PF_WARN ? " -- the game needs ~0.6 GB RSS plus GPU memory: close other apps (the "
                                          "low-memory killer ends the app without a crash line)" : "");
    }
}

/* ---- address space: the guest window -------------------------------------------------------------------------- */
#define PF_GUEST_SPAN   ((size_t)1 << 32)
#define PF_RAM          ((size_t)64 << 20)
#define PF_MIRRORS      28
#define PF_CONTIG_OFF   ((size_t)0x80000000u)
#define PF_APERTURE_OFF ((size_t)0xFE800000u)

static int pf_memfd(size_t size, const char **how, int *err)
{
    int fd = -1;
    *err = 0;
#if defined(__NR_memfd_create)
    fd = (int)syscall(__NR_memfd_create, "bvr-preflight", 1u /* MFD_CLOEXEC */);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) == 0) { *how = "memfd_create"; return fd; }
        *err = errno;
        close(fd);
        fd = -1;
    } else {
        *err = errno;
    }
#endif
#if defined(__ANDROID__) && !defined(BVR_NO_LIBANDROID)
    fd = ASharedMemory_create("bvr-preflight", size);
    if (fd >= 0) { *how = "ASharedMemory (ashmem fallback)"; return fd; }
#endif
    *how = "none";
    return -1;
}

int bvr_pf_addrspace(void)
{
    char e[64], why[300] = "", fb[160] = "";
    uint8_t *base;
    const char *how = "?";
    int fd, err = 0, ok = 0, mapped_mirrors = 0, i;
    long long mmap_min = -1, max_maps = -1;
    char buf[32];
    if (read_text("/proc/sys/vm/mmap_min_addr", buf, sizeof(buf)) > 0) mmap_min = atoll(buf);
    if (read_text("/proc/sys/vm/max_map_count", buf, sizeof(buf)) > 0) max_maps = atoll(buf);
    /* as xbox_MemoryLayoutInit: 4 GB, no access, no commit; the OS picks the base (no fixed low address) */
    base = (uint8_t *)mmap(NULL, PF_GUEST_SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        err = errno;
        /* the fallback the runtime does not have: does a smaller window fit? (it would need the apertures moved) */
        {
            void *half = mmap(NULL, PF_GUEST_SPAN / 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            snprintf(fb, sizeof(fb), "; a 2 GB reservation %s", half == MAP_FAILED ? "fails too" : "works (no runtime "
                     "fallback exists: the apertures sit at fixed offsets up to +0xFF000000)");
            if (half != MAP_FAILED) munmap(half, PF_GUEST_SPAN / 2);
        }
        bvr_pf_report("addrspace", BVR_PF_FAIL, 1, "reserving the 4 GB guest window (mmap PROT_NONE|MAP_NORESERVE) failed: "
                      "%s%s; vm.mmap_min_addr %lld, vm.max_map_count %lld", errtxt(err, e, sizeof(e)), fb, mmap_min,
                      max_maps);
        return 0;
    }
    fd = pf_memfd(PF_RAM, &how, &err);
    if (fd < 0) {
        snprintf(why, sizeof(why), "64 MB shared memory: memfd_create %s and the ASharedMemory fallback both failed",
                 errtxt(err, e, sizeof(e)));
    } else {
        void *v = mmap(base, PF_RAM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        if (v != base) {
            snprintf(why, sizeof(why), "mapping the 64 MB RAM view MAP_SHARED|MAP_FIXED at the base (%s): %s", how,
                     errtxt(errno, e, sizeof(e)));
        } else {
            for (i = 1; i <= PF_MIRRORS; i++) {
                void *m = mmap(base + (size_t)i * PF_RAM, PF_RAM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
                if (m == MAP_FAILED) {
                    snprintf(why, sizeof(why), "mirror %d of %d (base+0x%08zX): %s", i, PF_MIRRORS, (size_t)i * PF_RAM,
                             errtxt(errno, e, sizeof(e)));
                    break;
                }
                mapped_mirrors++;
            }
            if (!why[0]) {
                void *c = mmap(base + PF_CONTIG_OFF, PF_RAM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
                if (c == MAP_FAILED) {
                    snprintf(why, sizeof(why), "contiguous window at base+0x80000000: %s", errtxt(errno, e, sizeof(e)));
                } else {
                    volatile uint32_t *a = (volatile uint32_t *)(base + 0x448);
                    volatile uint32_t *b = (volatile uint32_t *)(base + (size_t)PF_MIRRORS * PF_RAM + 0x448);
                    volatile uint32_t *ct = (volatile uint32_t *)(base + PF_CONTIG_OFF + 0x448);
                    *a = 0x5AFEC0DEu;
                    if (*b != 0x5AFEC0DEu || *ct != 0x5AFEC0DEu) {
                        snprintf(why, sizeof(why), "the views do not alias (mirror reads %08X, contiguous %08X)", *b, *ct);
                    } else {
                        /* an aperture page as the APU/OHCI traps make it: anonymous, then PROT_NONE */
                        void *ap = mmap(base + PF_APERTURE_OFF, 0x1000, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                        if (ap == MAP_FAILED || mprotect(ap, 0x1000, PROT_NONE) != 0)
                            snprintf(why, sizeof(why), "aperture page at base+0xFE800000 (map + PROT_NONE): %s",
                                     errtxt(errno, e, sizeof(e)));
                        else
                            ok = 1;
                    }
                }
            }
        }
        close(fd);
    }
    munmap(base, PF_GUEST_SPAN);
    if (ok)
        bvr_pf_report("addrspace", BVR_PF_OK, 0, "4 GB guest window reserved at %p, 64 MB RAM via %s, %d mirrors + the "
                      "contiguous window alias it, trapped aperture page OK; vm.mmap_min_addr %lld (unused: the base is "
                      "OS-chosen), vm.max_map_count %lld", (void *)base, how, mapped_mirrors, mmap_min, max_maps);
    else
        bvr_pf_report("addrspace", BVR_PF_FAIL, 1, "%s (window at %p, %d mirrors mapped before the failure); "
                      "vm.max_map_count %lld", why, (void *)base, mapped_mirrors, max_maps);
    return ok;
}

/* ---- SIGSEGV trap round trip ---------------------------------------------------------------------------------- */
static volatile int s_trap_armed;
static uintptr_t s_trap_page;
static volatile unsigned s_trap_hits, s_trap_onstack, s_trap_depth, s_trap_max_depth, s_trap_nested_ok;
static volatile uintptr_t s_trap_alt_size, s_trap_alt_used, s_trap_sp;
static volatile uint32_t s_trap_reg[8];
static char s_prev_desc[160];

static uint64_t trap_rd(void *dev, uint32_t off, int size)
{
    (void)dev; (void)size;
    if (off == 0x40 && s_trap_depth == 1) {
        /* a device model touching another trapped register from inside its handler: nests (SA_NODEFER) */
        const uint32_t inner = ((volatile uint32_t *)s_trap_page)[0x80 / 4];
        if (inner == 0xC0DE0080u) s_trap_nested_ok++;
    }
    return 0xC0DE0000u + off;
}
static void trap_wr(void *dev, uint32_t off, uint64_t v, int size)
{
    (void)dev; (void)size;
    s_trap_reg[(off >> 2) & 7] = (uint32_t)v;
}

static int trap_fault(void *uc, uintptr_t fault)
{
    stack_t ss;
    int done;
    if (!s_trap_armed || fault < s_trap_page || fault - s_trap_page >= 0x1000) return 0;
    s_trap_hits++;
    s_trap_sp = (uintptr_t)&ss;
    if (sigaltstack(NULL, &ss) == 0 && (ss.ss_flags & SS_ONSTACK)) {
        const uintptr_t top = (uintptr_t)ss.ss_sp + ss.ss_size, used = top - (uintptr_t)&ss;
        s_trap_onstack++;
        s_trap_alt_size = ss.ss_size;
        if (used > s_trap_alt_used) s_trap_alt_used = used;
    }
    if (++s_trap_depth > s_trap_max_depth) s_trap_max_depth = s_trap_depth;
    done = a64_uc_emulate_mmio(uc, fault, (uint32_t)(fault - s_trap_page), 0x1000, NULL, trap_rd, trap_wr);
    s_trap_depth--;
    return done;
}

static void sym_of(void *p, char *out, size_t cap)
{
    Dl_info di;
    if (!p) { snprintf(out, cap, "none"); return; }
    if (dladdr(p, &di) && di.dli_fname) {
        const char *lib = strrchr(di.dli_fname, '/');
        snprintf(out, cap, "%s!%s", lib ? lib + 1 : di.dli_fname, di.dli_sname ? di.dli_sname : "?");
    } else {
        snprintf(out, cap, "%p", p);
    }
}

static int maps_has(const char *needle)
{
    char line[512];
    int hit = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    while (f && !hit && fgets(line, sizeof(line), f)) hit = strstr(line, needle) != NULL;
    if (f) fclose(f);
    return hit;
}

int bvr_pf_sigsegv(const char *name)
{
    static int registered;
    struct sigaction cur;
    char prev[160], via[80], e[64];
    volatile uint32_t *p;
    uint32_t v1, v2;
    int64_t t0, t1;
    unsigned i, before;
    stack_t ss;
    const int sigchain = maps_has("libsigchain");
    if (!a64_fault_install || !a64_fault_add_handler || !a64_uc_emulate_mmio) {
        bvr_pf_report(name, BVR_PF_SKIP, 0, "no runtime linked (shell-only smoke build)");
        return 1;
    }
    /* who handles SIGSEGV before the dispatcher (through libsigchain this is the user-level action: debuggerd) */
    if (!s_prev_desc[0]) {           /* before the dispatcher is installed; later calls see the dispatcher itself */
        memset(&cur, 0, sizeof(cur));
        sigaction(SIGSEGV, NULL, &cur);
        sym_of((cur.sa_flags & SA_SIGINFO) ? (void *)cur.sa_sigaction : (void *)cur.sa_handler, s_prev_desc,
               sizeof(s_prev_desc));
        if (!(cur.sa_flags & SA_SIGINFO) && cur.sa_handler == SIG_DFL) snprintf(s_prev_desc, sizeof(s_prev_desc), "SIG_DFL");
    }
    snprintf(prev, sizeof(prev), "%s", s_prev_desc);
    snprintf(via, sizeof(via), "%s", sigchain ? "through ART's libsigchain" : "direct (no libsigchain in this process)");
    if (!a64_fault_install()) {
        bvr_pf_report(name, BVR_PF_FAIL, 1, "sigaction(SIGSEGV) for the dispatcher failed: %s", errtxt(errno, e, sizeof(e)));
        return 0;
    }
    if (!registered) {
        if (!a64_fault_add_handler(trap_fault)) {
            bvr_pf_report(name, BVR_PF_FAIL, 1, "the dispatcher's handler table is full");
            return 0;
        }
        registered = 1;
    }
    s_trap_page = (uintptr_t)mmap(NULL, 0x1000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((void *)s_trap_page == MAP_FAILED) {
        bvr_pf_report(name, BVR_PF_FAIL, 1, "mmap of the test page: %s", errtxt(errno, e, sizeof(e)));
        return 0;
    }
    s_trap_hits = s_trap_onstack = s_trap_max_depth = s_trap_nested_ok = 0;
    s_trap_alt_used = s_trap_alt_size = 0;
    memset((void *)s_trap_reg, 0, sizeof(s_trap_reg));
    bvr_pf_detail(name, "arming: deliberate faults on a PROT_NONE page at %p follow (%s; previous SIGSEGV action %s). "
                  "If the log ends here, the fault never reached the runtime's dispatcher", (void *)s_trap_page, via, prev);
    p = (volatile uint32_t *)s_trap_page;
    s_trap_armed = 1;
    v1 = p[1];                              /* load: device returns 0xC0DE0000 + offset */
    p[2] = 0x12345678u;                     /* store: device records it */
    t0 = mono_ns();
    for (i = 0; i < 200; i++) (void)p[3];
    t1 = mono_ns();
    before = s_trap_hits;
    bvr_pf_detail(name, "nested fault next (a trap taken inside a trap handler, as device models do: needs SA_NODEFER "
                  "honoured by the signal chain)");
    v2 = p[0x40 / 4];
    s_trap_armed = 0;
    munmap((void *)s_trap_page, 0x1000);
    memset(&ss, 0, sizeof(ss));
    sigaltstack(NULL, &ss);
    {
        const int ok = v1 == 0xC0DE0004u && s_trap_reg[2] == 0x12345678u && v2 == 0xC0DE0040u && s_trap_nested_ok == 1 &&
                       s_trap_hits == 204u && before == 202u;
        char stack[200];
        if (s_trap_onstack)
            snprintf(stack, sizeof(stack), "handler on the alternate signal stack (%zu KB, deepest use %zu bytes)",
                     (size_t)s_trap_alt_size / 1024, (size_t)s_trap_alt_used);
        else
            snprintf(stack, sizeof(stack), "handler on the thread's own stack (alternate stack %s%zu KB)",
                     (ss.ss_flags & SS_DISABLE) ? "none, " : "", (ss.ss_flags & SS_DISABLE) ? (size_t)0 : ss.ss_size / 1024);
        if (ok)
            bvr_pf_report(name, BVR_PF_OK, 0, "trapped load, store and a nested trap emulated and resumed %s; %.2f us per "
                          "trap; %s; previous action %s (unclaimed faults go there: tombstone)", via,
                          (double)(t1 - t0) / 200.0 / 1000.0, stack, prev);
        else
            bvr_pf_report(name, BVR_PF_FAIL, 1, "trap results wrong: load %08X (want C0DE0004), store %08X (want 12345678), "
                          "nested %08X/%u (want C0DE0040/1), %u faults (%u before the nested one; want 204/202) %s; %s",
                          v1, s_trap_reg[2], v2, s_trap_nested_ok, s_trap_hits, before, via, stack);
        return ok;
    }
}

/* ---- syscalls under a temporary SIGSYS handler ------------------------------------------------------------------- */
static volatile sig_atomic_t s_sigsys;

static void pf_sigsys(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    (void)sig; (void)si;
    s_sigsys = 1;
    /* a seccomp SECCOMP_RET_TRAP: the call did not run; resume after it with -ENOSYS */
#if defined(__aarch64__)
    uc->uc_mcontext.regs[0] = (uint64_t)(int64_t)-ENOSYS;
#elif defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RAX] = -ENOSYS;
#else
    (void)uc;
#endif
}

/* returns the raw result; *blocked = refused by seccomp (SIGSYS), *err = errno */
static long pf_syscall(long nr, long a, long b, long c, int *blocked, int *err)
{
    struct sigaction sa, old;
    long r;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = pf_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    s_sigsys = 0;
    sigaction(SIGSYS, &sa, &old);
    errno = 0;
    r = syscall(nr, a, b, c);
    *err = errno;
    sigaction(SIGSYS, &old, NULL);
    *blocked = s_sigsys;
    return r;
}

#ifndef MEMBARRIER_CMD_QUERY
#define MEMBARRIER_CMD_QUERY 0
#endif
#define PF_MB_GLOBAL 1
#define PF_MB_SYNC_CORE 32
#define PF_MB_REG_SYNC_CORE 64

/* cranelift-jit's pipeline_flush_mt on aarch64 Linux/Android (wasmtime-internal-jit-icache-coherence): SYNC_CORE,
 * on EPERM register then SYNC_CORE, on EINVAL GLOBAL; any other error panics the JIT ("Failed pipeline flush"). */
static int pf_membarrier(char *out, size_t cap)
{
#if defined(__NR_membarrier)
    int blocked, err;
    char e[64];
    long q = pf_syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 0, 0, &blocked, &err), r;
    if (blocked) { snprintf(out, cap, "membarrier REFUSED by seccomp (SIGSYS)"); return 0; }
    if (q < 0) { snprintf(out, cap, "membarrier(QUERY) %s", errtxt(err, e, sizeof(e))); return 0; }
    r = pf_syscall(__NR_membarrier, PF_MB_SYNC_CORE, 0, 0, &blocked, &err);
    if (r < 0 && err == EPERM) {
        pf_syscall(__NR_membarrier, PF_MB_REG_SYNC_CORE, 0, 0, &blocked, &err);
        r = pf_syscall(__NR_membarrier, PF_MB_SYNC_CORE, 0, 0, &blocked, &err);
    } else if (r < 0 && err == EINVAL) {
        r = pf_syscall(__NR_membarrier, PF_MB_GLOBAL, 0, 0, &blocked, &err);
    }
    snprintf(out, cap, "membarrier supported 0x%lX, cranelift's SYNC_CORE sequence %s", q,
             r == 0 ? "OK" : blocked ? "REFUSED by seccomp" : errtxt(err, e, sizeof(e)));
    return r == 0;
#else
    snprintf(out, cap, "membarrier: no syscall number on this host");
    return 1;
#endif
}

int bvr_pf_jit(void)
{
#if defined(__aarch64__)
    static const uint32_t code[2] = { 0x52800540u /* mov w0, #42 */, 0xd65f03c0u /* ret */ };
#elif defined(__x86_64__)
    static const uint8_t code[6] = { 0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3 };   /* mov eax, 42; ret */
#endif
#if defined(__aarch64__) || defined(__x86_64__)
    const size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    char e[64], mb[200], rwx[120];
    uint8_t *m = (uint8_t *)mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int r = -1, mb_ok;
    if (m == MAP_FAILED) {
        bvr_pf_report("jit", BVR_PF_FAIL, 0, "mmap RW for code: %s", errtxt(errno, e, sizeof(e)));
        return 0;
    }
    memcpy(m, code, sizeof(code));
    __builtin___clear_cache((char *)m, (char *)m + sizeof(code));
    if (mprotect(m, pg, PROT_READ | PROT_EXEC) != 0) {
        const int err = errno;
        munmap(m, pg);
        bvr_pf_report("jit", BVR_PF_FAIL, 0, "mprotect RW->RX refused: %s -- the DSP56300's Cranelift JIT panics "
                      "(\"unable to make memory readable+executable\") when the APU starts; execmem denied by SELinux?",
                      errtxt(err, e, sizeof(e)));
        return 0;
    }
    r = ((int (*)(void))(uintptr_t)m)();
    /* W^X: is a writable+executable mapping refused? (informational: cranelift never asks for one) */
    if (mprotect(m, pg, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) snprintf(rwx, sizeof(rwx), "RWX allowed (no W^X)");
    else snprintf(rwx, sizeof(rwx), "RWX refused (%s: W^X enforced; fine, the JIT flips RW->RX)", errname(errno));
    munmap(m, pg);
    bvr_pf_detail("jit", "membarrier(SYNC_CORE) next, as cranelift-jit issues it after each compile");
    mb_ok = pf_membarrier(mb, sizeof(mb));
    if (r != 42)
        return bvr_pf_report("jit", BVR_PF_FAIL, 0, "code ran but returned %d (want 42): icache flush broken?; %s; %s", r,
                             rwx, mb) == BVR_PF_OK;
    if (!mb_ok)
        return bvr_pf_report("jit", BVR_PF_FAIL, 0, "RW->RX code runs, but %s -- cranelift-jit panics (\"Failed pipeline "
                             "flush\") at the DSP56300's first compile; %s", mb, rwx) == BVR_PF_OK;
    bvr_pf_report("jit", BVR_PF_OK, 0, "RW->RX mprotect and run OK; %s; %s", rwx, mb);
    return 1;
#else
    bvr_pf_report("jit", BVR_PF_SKIP, 0, "no test code for this CPU");
    return 1;
#endif
}

void bvr_pf_syscalls(void)
{
    char body[600] = "", e[64];
    int blocked, err, bad = 0;
    long r;
    size_t bl;
#if defined(__NR_memfd_create)
    r = pf_syscall(__NR_memfd_create, (long)"bvr-probe", 1, 0, &blocked, &err);
    if (r >= 0) close((int)r);
    bl = strlen(body);
    snprintf(body + bl, sizeof(body) - bl, "memfd_create %s", r >= 0 ? "OK" : blocked ? "REFUSED (seccomp)" : errtxt(err, e, sizeof(e)));
    if (r < 0) bad = 1;
#endif
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        r = pf_syscall(__NR_sched_getaffinity, 0, (long)sizeof(set), (long)&set, &blocked, &err);
        bl = strlen(body);
        if (r < 0) {
            snprintf(body + bl, sizeof(body) - bl, ", sched_getaffinity %s", blocked ? "REFUSED (seccomp)" : errtxt(err, e, sizeof(e)));
        } else {
            int n = CPU_COUNT(&set);
            long r2 = pf_syscall(__NR_sched_setaffinity, 0, (long)sizeof(set), (long)&set, &blocked, &err);
            snprintf(body + bl, sizeof(body) - bl, ", affinity %d CPUs allowed, sched_setaffinity (same mask) %s", n,
                     r2 == 0 ? "OK" : blocked ? "REFUSED (seccomp)" : errtxt(err, e, sizeof(e)));
        }
    }
    bl = strlen(body);
    snprintf(body + bl, sizeof(body) - bl, "; not used by the runtime: personality, mremap, userfaultfd, ptrace, "
             "perf_event_open; thread priorities are tracked only (SetThreadPriority makes no syscall)");
    bvr_pf_report("syscalls", bad ? BVR_PF_WARN : BVR_PF_OK, 0, "%s%s", body,
                  bad ? " -- memfd refused: the runtime falls back to ASharedMemory (see addrspace)" : "");
}

/* ---- alternate signal stack ----------------------------------------------------------------------------------- */
static pthread_key_t s_alt_key;
static pthread_once_t s_alt_once = PTHREAD_ONCE_INIT;

typedef struct { void *map; size_t len; } PfAlt;

static void alt_free(void *v)
{
    PfAlt *a = (PfAlt *)v;
    stack_t off;
    memset(&off, 0, sizeof(off));
    off.ss_flags = SS_DISABLE;
    sigaltstack(&off, NULL);
    munmap(a->map, a->len);
    free(a);
}
static void alt_key(void) { pthread_key_create(&s_alt_key, alt_free); }

size_t bvr_thread_altstack(size_t min_bytes)
{
    stack_t cur, ns;
    const size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    size_t len;
    PfAlt *a;
    uint8_t *m;
    memset(&cur, 0, sizeof(cur));
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_size >= min_bytes) return cur.ss_size;
    if (cur.ss_flags & SS_ONSTACK) return cur.ss_size;      /* running on it: cannot be swapped now */
    pthread_once(&s_alt_once, alt_key);
    if (pthread_getspecific(s_alt_key)) return cur.ss_size;  /* ours already */
    len = (min_bytes + pg - 1) / pg * pg + pg;              /* + a guard page below */
    m = (uint8_t *)mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return (cur.ss_flags & SS_DISABLE) ? 0 : cur.ss_size;
    mprotect(m, pg, PROT_NONE);
    memset(&ns, 0, sizeof(ns));
    ns.ss_sp = m + pg;
    ns.ss_size = len - pg;
    if (sigaltstack(&ns, NULL) != 0 || !(a = (PfAlt *)malloc(sizeof(*a)))) {
        munmap(m, len);
        return (cur.ss_flags & SS_DISABLE) ? 0 : cur.ss_size;
    }
    a->map = m;
    a->len = len;
    pthread_setspecific(s_alt_key, a);
    return ns.ss_size;
}
