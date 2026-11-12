/* grx_kstats.c -- tests/game_render_x86: the Kelvin engine's counters for grx_perf.c (compiled with the Kelvin core's
 * host flags). Read without the engine lock: monotonically increasing 64-bit counters, a torn read is harmless. */
#include "kelvin_int.h"
#include "grx_perf.h"

void grx_kelvin_counters(uint64_t out[8])
{
    const KelvinStats *s = &g_kelvin.stats;
    out[0] = s->words;
    out[1] = s->methods;
    out[2] = s->flips;
    out[3] = s->draws;
    out[4] = s->prims;
    out[5] = s->vertices;
    out[6] = s->triangles;
    out[7] = s->draws_deferred;
}
