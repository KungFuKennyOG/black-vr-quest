/* Kelvin GPU backends: the decoded pixel state (KPixelState) as the combiner constants (PSConst) both backends
 * upload and key their specialised pixel shaders on. Moved unchanged from kelvin_gpu.c (kelvin_gfx.h). Needs
 * kelvin_render.h, psh_regs.h and gfx/kgfx_cfg_types.inc. */
static uint32_t raw_in(const KCombinerInput *in) { return (uint32_t)(in->reg | in->chan | in->mod); }
static uint32_t raw_out(const KCombinerOutput *o)
{
    return (uint32_t)(o->cd | (o->ab << 4) | (o->muxsum << 8) | (o->flags << 12));
}

static void fill_ps_const(PSConst *c, const KPixelState *ps, unsigned int alpha_mode)
{
    int i, k;
    memset(c, 0, sizeof(*c));
    for (i = 0; i < 8; i++) {
        const KCombinerStage *s = &ps->stage[i];
        for (k = 0; k < 4; k++) {
            c->stage[i].rgb_in[k] = raw_in(&s->rgb[k]);
            c->stage[i].alpha_in[k] = raw_in(&s->alpha[k]);
        }
        c->stage[i].outs[0] = raw_out(&s->rgb_out);
        c->stage[i].outs[1] = raw_out(&s->alpha_out);
        memcpy(c->stage[i].c0, s->c0, 16);
        memcpy(c->stage[i].c1, s->c1, 16);
    }
    for (k = 0; k < 4; k++) c->fin_abcd[k] = raw_in(&ps->fin[k]);
    c->fin_efg[0] = raw_in(&ps->fin[4]);
    c->fin_efg[1] = raw_in(&ps->fin[5]);
    c->fin_efg[2] = raw_in(&ps->fin[6]);
    c->fin_efg[3] = (ps->final_enabled ? 1u : 0u) | (ps->clamp_sum ? 2u : 0u) | (ps->inv_v1 ? 4u : 0u) |
                    (ps->inv_r0 ? 8u : 0u) | ((ps->flags & PS_COMBINERCOUNT_MUX_MSB) ? 0x10u : 0u) |
                    ((ps->flags & PS_COMBINERCOUNT_UNIQUE_C0) ? 0x20u : 0u) |
                    ((ps->flags & PS_COMBINERCOUNT_UNIQUE_C1) ? 0x40u : 0u);
    memcpy(c->final_c0, ps->final_c0, 16);
    memcpy(c->final_c1, ps->final_c1, 16);
    c->fogcolor[0] = ps->fog_color[0]; c->fogcolor[1] = ps->fog_color[1]; c->fogcolor[2] = ps->fog_color[2];
    c->gen[0] = (uint32_t)ps->num_stages;
    c->gen[1] = ps->alpha_test ? (uint32_t)ps->alpha_func : 7u;
    c->gen[2] = (uint32_t)ps->alpha_ref;
    c->gen[3] = ps->smooth ? 0u : 1u;          /* flat shading */
    for (i = 0; i < 4; i++) {
        const KTexture *t = &ps->tex[i];
        c->texmode[i] = (uint32_t)ps->tex_mode[i];
        c->texflags[i] = (t->valid ? 1u : 0u) | (t->linear ? 2u : 0u) | (t->alphakill ? 4u : 0u) |
                         ((uint32_t)t->colorkey_mode << 4);
        c->texrcp[i][0] = t->width ? 1.0f / (float)t->width : 1.0f;
        c->texrcp[i][1] = t->height ? 1.0f / (float)t->height : 1.0f;
        c->ckey[i] = ps->color_key[i];
        c->ckeymask[i] = ps->color_key_mask[i];
    }
    c->ofs[0] = alpha_mode;
}
