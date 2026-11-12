/*
 * nv2a_pgraph_none.c - the pgraph_d3d11_* entry points with no translator behind
 * them, for hosts without Direct3D 11 (the Quest / Android arm64 build).
 *
 * nv2a_core.c (the xemu-derived register model, still the home of the PRAMDAC
 * palette that kelvin_mmio.c uses) routes PGRAPH methods through
 * pgraph_d3d11_method first. nv2a_pgraph_d3d11.c needs the D3D8->D3D11 device,
 * which does not exist there; the Kelvin engine executes the pushbuffer instead.
 * So every method is "not handled": nv2a_core stores it in its register file
 * and logs it, exactly as it does for a method the translator declines.
 *
 * Built only by port/android/nv2a.cmake; the Windows and desktop builds keep
 * nv2a_pgraph_d3d11.c.
 */
#include <string.h>

#include "nv2a_pgraph_d3d11.h"

void pgraph_d3d11_init(void) {}
void pgraph_d3d11_shutdown(void) {}

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel;
    (void)method;
    (void)param;
    return 0;
}

void pgraph_d3d11_flush(void) {}
void pgraph_d3d11_set_chyron_scroll(uint32_t frame) { (void)frame; }

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}
