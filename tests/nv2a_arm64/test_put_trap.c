/*
 * nv2a-put-trap (arm64 only): kelvin_mmio.c's AArch64 path through real SIGSEGVs.
 *
 * 1. The kickoff handshake. A "title" thread fills a ring with words using plain stores, then
 *    stores DMA_PUT with a plain STR into the USER page -- as recompiled D3D does. The page is
 *    read-only, so the store faults into kelvin_a64_fault, which fences and stores through the
 *    alias. A "pusher" thread reads PUT with kelvin_user_r32_acquire, checks every word up to
 *    it, and publishes GET with kelvin_user_w32_release; the title reads GET with plain loads
 *    (no fault) to find ring space. Any stale word is counted.
 *    (QEMU on an x86 host does not reorder stores, so this proves the mechanism and the data
 *    path -- every PUT store trapped and landed -- not the ARM reordering itself; the ordering
 *    argument is in port/notes/nv2a-arm64.md.)
 * 2. The trapped registers: PMC_BOOT_0, PGRAPH INTR (write-1-to-clear) and INTR_EN, PCRTC
 *    INTR_EN, through plain loads/stores of every width, an STP pair and an LDP.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "platform/win32_compat.h"
#include "kelvin_int.h"
#include "kelvin.h"

#define AP_VA 0xFD000000u
static uint8_t *s_ap;

/* ---- what kelvin_mmio.c needs from the rest of the runtime ---- */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)((uintptr_t)s_ap - AP_VA); }
uint8_t *kelvin_guest(uint32_t va) { return (uint8_t *)xbox_GetMemoryOffset() + va; }
uint8_t *kelvin_aperture(uint32_t offset) { return kelvin_guest(KELVIN_APERTURE_VA + offset); }
uint32_t kelvin_aperture_r32(uint32_t offset) { return *(volatile uint32_t *)kelvin_aperture(offset); }
uint32_t kelvin_pramin_r32(uint32_t offset) { return kelvin_aperture_r32(KELVIN_PRAMIN_BASE + offset); }
void kgpu_symbol_name(uint64_t addr, char *out, size_t cap) { snprintf(out, cap, "%llx", (unsigned long long)addr); }
volatile long long g_kelvin_notify_raise_qpc, g_kelvin_notify_ticks, g_kelvin_notify_count;  /* kelvin_pgraph.c */
static uint8_t s_dac[0x400];
void nv2a_dac_write(uint32_t offset, uint8_t value) { s_dac[offset & 0x3FF] = value; }
uint8_t nv2a_dac_read(uint32_t offset) { return s_dac[offset & 0x3FF]; }

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { s_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- 1. kickoffs -------------------------------------------------------------------- */
#define RING_WORDS 4096u                  /* bytes = 16 KiB */
#define KICKS 6000
static uint32_t s_ring[RING_WORDS];       /* plain memory, as the pushbuffer */
static atomic_int s_stop;
static atomic_long s_stale, s_words_checked;

static volatile uint32_t *user(uint32_t off) { return (volatile uint32_t *)(s_ap + off); }

static uint32_t word_for(uint32_t seq) { return seq * 0x9E3779B1u ^ 0x5A5A0000u; }

static void *pusher(void *arg)
{
    uint32_t get = 0, seq = 0;
    (void)arg;
    while (!atomic_load(&s_stop)) {
        uint32_t put = kelvin_user_r32_acquire(KELVIN_USER_DMA_PUT);
        while (get != put) {
            uint32_t w = s_ring[get / 4];
            if (w != word_for(seq) && atomic_fetch_add(&s_stale, 1) < 5) {
                printf("  stale word at 0x%X: %08X expected %08X\n", get, w, word_for(seq));
            }
            atomic_fetch_add(&s_words_checked, 1);
            seq++;
            get = (get + 4) % (RING_WORDS * 4);
        }
        kelvin_user_w32_release(KELVIN_USER_DMA_GET, get);
        kelvin_user_w32_release(KELVIN_PFIFO_BASE + 0x3244u /* CACHE1_DMA_GET */, get);
    }
    return NULL;
}

static void kickoffs(void)
{
    pthread_t th;
    uint32_t put = 0, seq = 0;
    int k;
    pthread_create(&th, NULL, pusher, NULL);
    for (k = 0; k < KICKS; k++) {
        uint32_t n = 1 + (uint32_t)(rand() % 200), i;
        for (i = 0; i < n; i++) {
            /* room: GET (plain load from the read-only USER page) must not be the next slot */
            while ((put + 4) % (RING_WORDS * 4) == *user(KELVIN_USER_DMA_GET)) {}
            s_ring[put / 4] = word_for(seq++);
            put = (put + 4) % (RING_WORDS * 4);
        }
        *user(KELVIN_USER_DMA_PUT) = put;      /* plain STR: faults, fenced, lands through the alias */
    }
    while (*user(KELVIN_USER_DMA_GET) != put) {}
    atomic_store(&s_stop, 1);
    pthread_join(th, NULL);
    CHECK(atomic_load(&s_stale) == 0, "%ld stale pushbuffer words", (long)atomic_load(&s_stale));
    CHECK((uint32_t)atomic_load(&s_words_checked) == seq, "pusher read %ld of %u words",
          (long)atomic_load(&s_words_checked), seq);
    CHECK(kelvin_aperture_r32(KELVIN_PFIFO_BASE + 0x3244u) == put, "CACHE1 GET");
    printf("  %d kickoffs (%u words) through the trapped PUT: %ld stale\n", KICKS, seq, (long)atomic_load(&s_stale));
}

/* ---- 2. registers ------------------------------------------------------------------- */
static void registers(KelvinState *pg)
{
    volatile uint32_t *boot = (volatile uint32_t *)(s_ap + 0x000000);
    volatile uint32_t *intr = (volatile uint32_t *)(s_ap + KELVIN_PGRAPH_BASE + NV_PGRAPH_INTR);
    volatile uint32_t *intr_en = (volatile uint32_t *)(s_ap + KELVIN_PGRAPH_BASE + NV_PGRAPH_INTR_EN);
    volatile uint8_t *intr_en_b = (volatile uint8_t *)intr_en;
    volatile uint16_t *user16 = (volatile uint16_t *)(s_ap + KELVIN_USER_BASE + 0x80);
    uint64_t pair;

    CHECK(*boot == 0x02A000A3u, "PMC_BOOT_0 read %08X", *boot);
    *intr_en = 0x00001100u;
    CHECK(pg->enabled_interrupts == 0x00001100u, "INTR_EN write reached the model (%08X)", pg->enabled_interrupts);
    intr_en_b[2] = 0x01;                        /* byte store merges */
    CHECK(pg->enabled_interrupts == 0x00011100u, "byte INTR_EN merge (%08X)", pg->enabled_interrupts);
    CHECK(*intr_en == 0x00011100u, "INTR_EN read back");
    kelvin_lock();
    pg->pending_interrupts = 0x00001101u;
    kelvin_unlock();
    CHECK(*intr == 0x00001101u, "INTR read %08X", *intr);
    *intr = 0x00000001u;                        /* write 1 to clear */
    CHECK(pg->pending_interrupts == 0x00001100u, "INTR write-1-to-clear (%08X)", pg->pending_interrupts);
    /* an STP pair across INTR_EN and the next register, and an LDP back */
    __asm__ volatile("stp %w1, %w2, [%0]" :: "r"(intr_en), "r"(0x5u), "r"(0x77u) : "memory");
    CHECK(pg->enabled_interrupts == 0x5u, "STP first element (%08X)", pg->enabled_interrupts);
    __asm__ volatile("ldr %0, [%1]" : "=r"(pair) : "r"(intr_en) : "memory");
    CHECK((uint32_t)pair == 0x5u && (uint32_t)(pair >> 32) == 0x77u, "64-bit load %016llX", (unsigned long long)pair);
    /* a narrow store on the USER page (not PUT) is emulated too */
    *user16 = 0xBEEF;
    CHECK(*user16 == 0xBEEF, "16-bit USER store");
    printf("  trapped registers: BOOT_0, INTR/INTR_EN (32/8-bit, STP, 64-bit LDR), USER 16-bit\n");
}

int main(void)
{
    static KelvinState pg;
    static uint32_t regs[0x2000 / 4];
    void *p = mmap(NULL, 16u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 2; }
    s_ap = (uint8_t *)p;
    memset(&pg, 0, sizeof(pg));
    pg.regs = regs;
    if (!kelvin_mmio_start(&pg)) { printf("FAIL: kelvin_mmio_start\n"); return 1; }
    CHECK(kelvin_owns_register(0x400100u) && !kelvin_owns_register(0x800040u), "ownership");
    registers(&pg);
    kickoffs();
    printf("%s\n", s_fail ? "FAIL" : "PASS");
    return s_fail != 0;
}
