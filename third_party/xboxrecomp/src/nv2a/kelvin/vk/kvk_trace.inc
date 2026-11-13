/*
 * kvk_trace.inc - the call-trace recorder (kvk_trace.h), #included by kvk_backend.c. Compiled only with KVK_TRACE
 * (host builds: tests/game_render_x86, tests/kelvin_vk); otherwise every hook below is an empty inline.
 *
 *   RECOMP_KVK_TRACE=<dir>        where traces go: <dir>/trace_f<first frame>.ktr
 *   RECOMP_KVK_TRACE_N=<k>        flips per trace (3)
 *   RECOMP_KVK_TRACE_EVERY=<n>    start a trace at every n-th frame (0: never by count)
 *   <dir>/now (a file)            start a trace at the next flip, the file is removed (scripted from outside)
 * The recorder runs on the GPU thread like the calls it records. Texture and palette bytes are written once per
 * content per file (hashed; within one kick a texture address is hashed once).
 */
#include <stddef.h>
#include "kvk_trace.h"
#ifdef KVK_TRACE

static struct {
    int init;                       /* -1 off, 1 armed */
    char dir[512];
    unsigned n, every;
    FILE *f;
    char path[600];
    uint64_t first_frame;
    unsigned flips;
    uint32_t next_blob;
    /* content dedupe: hash -> blob id (per file) */
    uint64_t *bh;
    uint32_t *bid, *blen;
    /* address shortcut within a kick: (base, span, epoch) -> blob id */
    struct { uint32_t base, span, palette; uint64_t epoch; uint32_t id; } ac[4096];
    /* programs written to this file */
    uint64_t prog[2048];
    unsigned nprog;
    /* the call being recorded */
    bool in_draw, vm;
    KvkTrFlip fl;
} TR;

#define TR_BH 65536u

static bool tr_on(void)
{
    if (!TR.init) {
        const char *d = getenv("RECOMP_KVK_TRACE"), *n = getenv("RECOMP_KVK_TRACE_N"), *e = getenv("RECOMP_KVK_TRACE_EVERY");
        TR.init = -1;
        if (d && *d) {
            snprintf(TR.dir, sizeof(TR.dir), "%s", d);
            mkdir(TR.dir, 0775);
            TR.n = n && *n ? (unsigned)atoi(n) : 3u;
            if (!TR.n) TR.n = 3;
            TR.every = e && *e ? (unsigned)atoi(e) : 0u;
            TR.bh = (uint64_t *)calloc(TR_BH, sizeof(uint64_t));
            TR.bid = (uint32_t *)calloc(TR_BH, sizeof(uint32_t));
            TR.blen = (uint32_t *)calloc(TR_BH, sizeof(uint32_t));
            if (TR.bh && TR.bid && TR.blen) TR.init = 1;
            LOG("trace: armed, dir %s, %u flips per trace, every %u frames, or at %s/now", TR.dir, TR.n, TR.every, TR.dir);
        }
    }
    return TR.init > 0 && TR.f != NULL;
}

static void tr_rec(uint32_t tag, const void *p, uint32_t n)
{
    fwrite(&tag, 4, 1, TR.f);
    fwrite(&n, 4, 1, TR.f);
    if (n) fwrite(p, 1, n, TR.f);
}

static uint32_t tr_blob(const void *p, uint64_t n)
{
    uint64_t h;
    uint32_t i, tag = KVK_TR_BLOB, len32;
    struct { uint32_t id, pad; uint64_t bytes; } bhdr;
    if (!p || !n || n > 0xFFFFFFF0u) return 0;
    h = kvk_hash64(p, (size_t)n) ^ (n * 0x9E3779B97F4A7C15ull);
    if (!h) h = 1;
    for (i = (uint32_t)(h % TR_BH);; i = (i + 1) % TR_BH) {
        if (!TR.bh[i]) break;
        if (TR.bh[i] == h && TR.blen[i] == (uint32_t)n) return TR.bid[i];
    }
    TR.bh[i] = h; TR.blen[i] = (uint32_t)n; TR.bid[i] = ++TR.next_blob;
    bhdr.id = TR.bid[i]; bhdr.pad = 0; bhdr.bytes = n;
    len32 = (uint32_t)(sizeof(bhdr) + n);
    fwrite(&tag, 4, 1, TR.f);
    fwrite(&len32, 4, 1, TR.f);
    fwrite(&bhdr, sizeof(bhdr), 1, TR.f);
    fwrite(p, 1, (size_t)n, TR.f);
    return TR.bid[i];
}

static void tr_ctx(KvkTrCtx *c)
{
    c->frame_mode = kvk_vr_frame_mode();
    c->vr_active = vr_active() ? 1u : 0u;
}

static void tr_prog(uint32_t hash, uint32_t start, const uint32_t *words, uint32_t nwords)
{
    const uint64_t k = ((uint64_t)hash << 32) | start;
    unsigned i;
    uint32_t hdr[3];
    if (!words || !nwords) return;
    for (i = 0; i < TR.nprog; i++) if (TR.prog[i] == k) return;
    if (TR.nprog < 2048) TR.prog[TR.nprog++] = k;
    hdr[0] = hash; hdr[1] = start; hdr[2] = nwords;
    {
        const uint32_t tag = KVK_TR_VSPROG, n = 12 + nwords * 4;
        fwrite(&tag, 4, 1, TR.f);
        fwrite(&n, 4, 1, TR.f);
        fwrite(hdr, 4, 3, TR.f);
        fwrite(words, 4, nwords, TR.f);
    }
}

static void tr_vs_ready(uint32_t hash, uint32_t start, const uint32_t *words, uint32_t nwords)
{
    if (tr_on()) tr_prog(hash, start, words, nwords);
}

static uint32_t tr_texture(const KTexture *t)
{
    uint32_t span, slot;
    if (!t->valid || !t->mem || !t->width || !t->height) return 0;
    span = texture_span(t);
    slot = (t->base * 2654435761u >> 20) & 4095u;
    if (TR.ac[slot].id && TR.ac[slot].base == t->base && TR.ac[slot].span == span && TR.ac[slot].palette == t->palette &&
        TR.ac[slot].epoch == G.epoch)
        return TR.ac[slot].id;
    TR.ac[slot].base = t->base; TR.ac[slot].span = span; TR.ac[slot].palette = t->palette; TR.ac[slot].epoch = G.epoch;
    TR.ac[slot].id = tr_blob(t->mem, span);
    return TR.ac[slot].id;
}

static KvkTrDraw s_trd;

static void tr_draw_begin(const KGpuDraw *d)
{
    KvkTrDraw *r = &s_trd;
    int i;
    memset(r, 0, sizeof(*r));
    tr_ctx(&r->ctx);
    r->d = *d;
    if (d->t) { r->t = *d->t; r->has_color = d->t->color != NULL; r->has_zeta = d->t->zeta != NULL; }
    if (d->ps) r->ps = *d->ps;
    if (d->ps) {
        for (i = 0; i < 4; i++) {
            const KTexture *tx = &d->ps->tex[i];
            if (d->ps->tex_mode[i] != PS_TEXTUREMODES_PROJECT2D || !tx->valid) continue;
            r->tex_blob[i] = tr_texture(tx);
            if (tx->palette_mem && tx->palette_len) r->pal_blob[i] = tr_blob(tx->palette_mem, (uint64_t)tx->palette_len * 4u);
        }
    }
    if (d->indices && d->icount) r->idx_blob = tr_blob(d->indices, (uint64_t)d->icount * 4u);
    if (d->gvs) {
        const KGpuVsDraw *g = d->gvs;
        unsigned slot = (g->hash ^ (g->start * 40503u)) % KVK_VS_MAX, k;
        r->has_gvs = 1;
        r->gv = *g;
        for (k = 0; k < KVK_VS_MAX; k++, slot = (slot + 1) % KVK_VS_MAX) {
            const KvkVs *q = &G.vs[slot];
            if (!q->used) break;
            if (q->hash == g->hash && q->start == g->start) { tr_prog(q->hash, q->start, q->words, q->nwords); break; }
        }
        if (g->constants) r->const_blob = tr_blob(g->constants, 192 * 16);
        for (i = 0; i < 16; i++) {
            const KGpuAttrSrc *as = &g->attr[i];
            if (!as->fmt || !as->data || as->hi < as->lo) continue;
            r->attr_blob[i] = tr_blob(as->data + as->lo, (uint64_t)(as->hi - as->lo) + 16u);
        }
    } else if (d->recs && d->vcount) {
        r->recs_blob = tr_blob(d->recs, (uint64_t)d->vcount * d->stride * 16u);
    }
    TR.in_draw = true;
    TR.vm = false;
}

static void tr_draw_end(void)
{
    s_trd.vm = TR.vm ? 1u : 0u;
    TR.in_draw = false;
    tr_rec(KVK_TR_DRAW, &s_trd, sizeof(s_trd));
}

static bool tr_note_vm(bool vm)
{
    if (TR.in_draw) TR.vm = vm;
    return vm;
}

static void tr_stereo(const KvkStereoFrame *st)
{
    if (tr_on()) tr_rec(KVK_TR_STEREO, st, sizeof(*st));
}

static void tr_clear(const KelvinTarget *t, bool color, bool depth, bool stencil, uint32_t color_value, uint32_t zs_value,
                     unsigned int channel_mask, int x0, int y0, int x1, int y1, unsigned int color_format)
{
    KvkTrClear c;
    memset(&c, 0, sizeof(c));
    tr_ctx(&c.ctx);
    c.t = *t; c.has_color = t->color != NULL; c.has_zeta = t->zeta != NULL;
    c.color = color; c.depth = depth; c.stencil = stencil; c.color_value = color_value; c.zs_value = zs_value;
    c.channel_mask = channel_mask; c.color_format = color_format; c.x0 = x0; c.y0 = y0; c.x1 = x1; c.y1 = y1;
    tr_rec(KVK_TR_CLEAR, &c, sizeof(c));
}

static void tr_simple(uint32_t tag, const void *p, uint32_t n)
{
    if (tr_on()) tr_rec(tag, p, n);
}

/* vrl_relatch's answers, the shell targets the flip got */
static void tr_flip_relatch(bool ok, const KvkStereoFrame *st, bool vm_ok, const KvkStereoFrame *st_vm)
{
    TR.fl.relatch_ok = ok; TR.fl.relatch_vm_ok = vm_ok;
    if (st) TR.fl.relatch = *st;
    if (st_vm) TR.fl.relatch_vm = *st_vm;
}

static void tr_flip_begin(const KelvinTarget *t, unsigned int color_format, uint32_t aw, uint32_t ah)
{
    memset(&TR.fl, 0, sizeof(TR.fl));
    if (!tr_on()) return;
    tr_ctx(&TR.fl.ctx);
    TR.fl.t = *t; TR.fl.has_color = t->color != NULL; TR.fl.has_zeta = t->zeta != NULL;
    TR.fl.color_format = color_format; TR.fl.aspect_w = aw; TR.fl.aspect_h = ah;
}

static void tr_open(void)
{
    KvkTrHeader h;
    const VrGameConfig *gc = vr_game_config();
    struct { uint32_t have; VrGameConfig c; } g;
    snprintf(TR.path, sizeof(TR.path), "%s/trace_f%06llu.ktr", TR.dir, (unsigned long long)G.st.frames);
    TR.f = fopen(TR.path, "wb");
    if (!TR.f) { LOG("trace: %s: %s", TR.path, strerror(errno)); return; }
    setvbuf(TR.f, NULL, _IOFBF, 1 << 20);
    memset(&h, 0, sizeof(h));
    h.magic = KVK_TR_MAGIC; h.version = KVK_TR_VERSION;
    h.host_w = G.host_w; h.host_h = G.host_h; h.layers = G.layers; h.hud_w = G.hud_w; h.hud_h = G.hud_h;
    h.sz_draw = sizeof(KGpuDraw); h.sz_ps = sizeof(KPixelState); h.sz_target = sizeof(KelvinTarget);
    h.sz_vsdraw = sizeof(KGpuVsDraw); h.sz_stereo = sizeof(KvkStereoFrame); h.sz_gamecfg = sizeof(VrGameConfig);
    h.first_frame = G.st.frames;
    fwrite(&h, sizeof(h), 1, TR.f);
    TR.first_frame = G.st.frames;
    TR.flips = 0;
    TR.next_blob = 0;
    TR.nprog = 0;
    memset(TR.bh, 0, TR_BH * sizeof(uint64_t));
    memset(TR.ac, 0, sizeof(TR.ac));
    memset(&g, 0, sizeof(g));
    if (gc) { g.have = 1; g.c = *gc; }
    tr_rec(KVK_TR_GAMECFG, &g, sizeof(g));
    {
        /* the viewmodel programs in force (kgpu_set_viewmodel may have been called long before) */
        struct { int32_t count; float scale; uint32_t h[8]; } r;
        int i;
        memset(&r, 0, sizeof(r));
        r.count = s_vm_count; r.scale = s_vm_scale;
        for (i = 0; i < r.count && i < 8; i++) r.h[i] = s_vm_hash[i];
        tr_rec(KVK_TR_VIEWMODEL, &r, sizeof(r));
    }
    LOG("trace: recording %u flips from frame %llu to %s", TR.n, (unsigned long long)G.st.frames, TR.path);
}

static void tr_close(void)
{
    long end;
    if (!TR.f) return;
    end = ftell(TR.f);
    fseek(TR.f, (long)offsetof(KvkTrHeader, frames), SEEK_SET);
    fwrite(&TR.flips, 4, 1, TR.f);
    fclose(TR.f);
    TR.f = NULL;
    LOG("trace: %s done, %u flips, %.1f MB", TR.path, TR.flips, (double)end / (1024.0 * 1024.0));
}

/* after flip_now: the flip's record, then a trace ends or begins at this frame boundary */
static void tr_flip_end(bool eyes, bool flat, bool hud)
{
    tr_on();
    if (TR.init <= 0) return;
    if (TR.f) {
        TR.fl.eye_targets = eyes; TR.fl.flat_target = flat; TR.fl.hud_target = hud;
        tr_rec(KVK_TR_FLIP, &TR.fl, sizeof(TR.fl));
        if (++TR.flips >= TR.n) tr_close();
        return;
    }
    {
        char now[600];
        bool go = TR.every && G.st.frames % TR.every == 0;
        snprintf(now, sizeof(now), "%s/now", TR.dir);
        if (!go && access(now, F_OK) == 0) { go = true; unlink(now); }
        if (go) tr_open();
    }
}

#else   /* !KVK_TRACE */
static inline bool tr_on(void) { return false; }
static inline void tr_vs_ready(uint32_t hash, uint32_t start, const uint32_t *words, uint32_t nwords)
{ (void)hash; (void)start; (void)words; (void)nwords; }
static inline void tr_draw_begin(const KGpuDraw *d) { (void)d; }
static inline void tr_draw_end(void) {}
static inline bool tr_note_vm(bool vm) { return vm; }
static inline void tr_stereo(const KvkStereoFrame *st) { (void)st; }
static inline void tr_clear(const KelvinTarget *t, bool color, bool depth, bool stencil, uint32_t color_value,
                            uint32_t zs_value, unsigned int channel_mask, int x0, int y0, int x1, int y1,
                            unsigned int color_format)
{ (void)t; (void)color; (void)depth; (void)stencil; (void)color_value; (void)zs_value; (void)channel_mask;
  (void)x0; (void)y0; (void)x1; (void)y1; (void)color_format; }
static inline void tr_simple(uint32_t tag, const void *p, uint32_t n) { (void)tag; (void)p; (void)n; }
static inline void tr_flip_relatch(bool ok, const KvkStereoFrame *st, bool vm_ok, const KvkStereoFrame *st_vm)
{ (void)ok; (void)st; (void)vm_ok; (void)st_vm; }
static inline void tr_flip_begin(const KelvinTarget *t, unsigned int color_format, uint32_t aw, uint32_t ah)
{ (void)t; (void)color_format; (void)aw; (void)ah; }
static inline void tr_flip_end(bool eyes, bool flat, bool hud) { (void)eyes; (void)flat; (void)hud; }
#endif
