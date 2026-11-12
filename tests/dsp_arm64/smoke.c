/* DSP56300 JIT smoke test: runs the same program on x86-64 and arm64 and prints a
 * checksum of all results, so the two can be compared. */
#include "dsp56300.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "prog.inc"
extern int quickstart_test(void);
static uint32_t pram[4096], xram[4096], yram[4096];
int main(void) {
    quickstart_test();
    Dsp56300MemoryRegion p = {.start=0,.end=4096,.kind=DSP56300_REGION_BUFFER,.data={.buffer={.base=pram,.offset=0}}};
    Dsp56300MemoryRegion x = {.start=0,.end=4096,.kind=DSP56300_REGION_BUFFER,.data={.buffer={.base=xram,.offset=0}}};
    Dsp56300MemoryRegion y = {.start=0,.end=4096,.kind=DSP56300_REGION_BUFFER,.data={.buffer={.base=yram,.offset=0}}};
    Dsp56300CreateInfo info = {.memory_map={.x_regions=&x,.x_count=1,.y_regions=&y,.y_count=1,.p_regions=&p,.p_count=1}};
    Dsp56300Jit *d = dsp56300_create(&info);
    if (!d) { puts("create failed"); return 1; }
    for (unsigned i = 0; i < sizeof prog/sizeof prog[0]; i++) dsp56300_write_memory(d, DSP56300_MEM_SPACE_P, i, prog[i]);
    dsp56300_run(d, 100000);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 4096; i++) { h = (h ^ xram[i]) * 1099511628211ull; h = (h ^ yram[i]) * 1099511628211ull; }
    Dsp56300JitStats st; dsp56300_get_jit_stats(d, &st);
    printf("x300=%06X x301=%06X x302=%06X x100=%06X y231=%06X hash=%016llX\n",
           xram[0x300], xram[0x301], xram[0x302], xram[0x100], yram[0x231], (unsigned long long)h);
    dsp56300_destroy(d);
    return 0;
}
