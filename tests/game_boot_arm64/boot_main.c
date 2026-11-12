/*
 * game_boot_arm64: boots the real arm64 BLACK game build (black_game + the
 * xr_* runtime, exactly as linked into libblackvr.so) as a static executable
 * under /root/xtool/arm64run, with the GPU, USB, video and the XR shell stubbed
 * (boot_stubs.c).
 *
 * It runs src/main.c's black_game_main() -- crash reporter, memory layout,
 * APU + fault handlers, kernel, path resolver, kernel bridge, flat dispatch,
 * the recompiled entry point -- with the working directory holding the user's
 * own game/ (read in place, never copied) and a scratch save/ directory.
 *
 * A monitor thread reports how far the guest got and stops the run:
 *   - at the first GPU touch: the title's pushbuffer PUT pointer moves (its
 *     first command submission to the NV2A), or a stubbed GPU entry point is
 *     called (BOOT_STOP_AT_GPU=0 keeps going), or
 *   - after BOOT_SECONDS (default 300) of guest execution,
 * printing the guest's indirect-call count, its last 16 indirect-call targets
 * and the NV2A registers it has written.
 *
 * Usage: game_boot_arm64 <folder holding game/> <scratch dir for save/>
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern int black_game_main(int argc, char **argv);
extern ptrdiff_t g_xbox_mem_offset;
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;
extern void *xbox_GetMemoryBase(void);

/* boot_stubs.c */
extern volatile int boot_gpu_touched;
extern volatile int boot_nv2a_mapped;
extern const char *volatile boot_gpu_first;

#define NV2A_BASE      0xFD000000u
#define NV2A_DMA_PUT   0x800040u
#define NV2A_DMA_GET   0x800044u
#define NV2A_PCRTC_START 0x600800u

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint32_t nv2a_reg(uint32_t off)
{
    /* g_xbox_mem_offset is published before the NV2A aperture is mapped; reading
     * it in between faults (a harness race, not a guest bug). */
    if (!__atomic_load_n(&boot_nv2a_mapped, __ATOMIC_ACQUIRE) || !g_xbox_mem_offset || !xbox_GetMemoryBase())
        return 0;
    return *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + NV2A_BASE + off);
}

static void report(const char *why, double t0)
{
    unsigned i, idx = g_icall_trace_idx;
    fprintf(stderr, "\n[BOOT-TEST] ==== %s after %.1f s ====\n", why, now_s() - t0);
    fprintf(stderr, "[BOOT-TEST] memory layout: %s (offset 0x%llX)\n",
            xbox_GetMemoryBase() ? "up" : "not up", (unsigned long long)g_xbox_mem_offset);
    fprintf(stderr, "[BOOT-TEST] guest indirect calls dispatched: %llu\n",
            (unsigned long long)g_icall_count);
    fprintf(stderr, "[BOOT-TEST] last indirect-call targets (oldest first):");
    for (i = 0; i < 16; i++) {
        uint32_t v = g_icall_trace[(idx + i) & 15];
        if (v)
            fprintf(stderr, " %08X", v);
    }
    fprintf(stderr, "\n[BOOT-TEST] NV2A: DMA PUT=%08X GET=%08X PCRTC_START=%08X\n",
            nv2a_reg(NV2A_DMA_PUT), nv2a_reg(NV2A_DMA_GET), nv2a_reg(NV2A_PCRTC_START));
    fprintf(stderr, "[BOOT-TEST] first stubbed GPU entry called: %s\n",
            boot_gpu_first ? boot_gpu_first : "(none)");
    fflush(stderr);
}

static double s_t0;
static int s_rc;

/* <scratch>/root/game/<entry> -> <user root>/game/<entry>, for every entry. */
static int make_scratch_root(const char *user_root, const char *scratch, char *root, size_t n)
{
    char src[1024], game[1100], link[1400], target[1400];
    DIR *d;
    struct dirent *de;

    mkdir(scratch, 0770);
    snprintf(root, n, "%s/root", scratch);
    mkdir(root, 0770);
    snprintf(game, sizeof(game), "%s/game", root);
    mkdir(game, 0770);
    snprintf(src, sizeof(src), "%s/game", user_root);
    if (!(d = opendir(src))) {
        fprintf(stderr, "[BOOT-TEST] cannot open %s: %s\n", src, strerror(errno));
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(link, sizeof(link), "%s/%s", game, de->d_name);
        snprintf(target, sizeof(target), "%s/%s", src, de->d_name);
        if (symlink(target, link) != 0 && errno != EEXIST) {
            fprintf(stderr, "[BOOT-TEST] symlink %s: %s\n", link, strerror(errno));
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static void *game_thread(void *arg)
{
    s_rc = black_game_main(2, (char **)arg);
    return NULL;
}

static void *monitor(void *arg)
{
    const char *e = getenv("BOOT_SECONDS");
    const double limit = e ? atof(e) : 300.0;
    const int stop_at_gpu = !getenv("BOOT_STOP_AT_GPU") || atoi(getenv("BOOT_STOP_AT_GPU")) != 0;
    uint64_t last_calls = 0;
    double last_print = now_s();
    (void)arg;
    for (;;) {
        double t = now_s();
        uint32_t put = nv2a_reg(NV2A_DMA_PUT);
        usleep(20000);
        if (stop_at_gpu && put != 0) {
            report("STOP: first GPU touch (pushbuffer PUT moved)", s_t0);
            _exit(0);
        }
        if (stop_at_gpu && boot_gpu_touched) {
            report("STOP: first GPU touch (stubbed GPU entry point)", s_t0);
            _exit(0);
        }
        if (t - last_print >= 15.0) {
            fprintf(stderr, "[BOOT-TEST] t=%.0fs icalls=%llu (+%llu)\n", t - s_t0,
                    (unsigned long long)g_icall_count,
                    (unsigned long long)(g_icall_count - last_calls));
            last_calls = g_icall_count;
            last_print = t;
        }
        if (t - s_t0 >= limit) {
            report("STOP: time limit (guest still running)", s_t0);
            _exit(3);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    char save[1100], root[1024];
    char *args[3];
    pthread_t mon, game;
    pthread_attr_t at;
    int rc;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <folder holding game/> <scratch dir>\n", argv[0]);
        return 2;
    }
    /* The guest writes E:\\UDATA, E:\\TDATA (E: is the game dir, as on Windows),
     * the kernel log and save/ under its working directory. None of that may
     * land in the user's folder: the run uses <scratch>/root, whose game/ holds
     * one symlink per entry of the user's game/ (read in place, nothing copied),
     * so anything new is created in the scratch tree. */
    if (make_scratch_root(argv[1], argv[2], root, sizeof(root)) != 0)
        return 2;
    snprintf(save, sizeof(save), "%s/save", root);
    setenv("BLACKVR_SAVE_DIR", save, 1);
    /* stdio stays on the terminal (logcat does not exist here). */
    setenv("BLACKVR_STDIO_LOGCAT", "0", 1);
    setvbuf(stderr, NULL, _IONBF, 0);

    args[0] = argv[0];
    args[1] = root;
    args[2] = NULL;
    s_t0 = now_s();
    pthread_create(&mon, NULL, monitor, NULL);

    /* The shell's game thread: 16 MB stack (port/quest/src/xr_main.c). */
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20);
    if (pthread_create(&game, &at, game_thread, args) != 0) {
        fprintf(stderr, "[BOOT-TEST] cannot create the game thread\n");
        return 1;
    }
    pthread_attr_destroy(&at);
    pthread_join(game, NULL);
    rc = s_rc;
    report("STOP: black_game_main returned", s_t0);
    fprintf(stderr, "[BOOT-TEST] black_game_main returned %d\n", rc);
    return rc == 0 ? 0 : 4;
}
