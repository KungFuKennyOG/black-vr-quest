/*
 * kvk_vr_layers.inc - the backend's side of the VR host glue (kvk_vr_hooks.h), #included by kvk_backend.c after
 * compose_pass(). Owned by the vr-glue stream (port/notes/vr-glue.md).
 *
 *   late latch   at the flip, after vr_acquire_eye_targets() late-latched the eye poses, this frame's stereo blocks
 *                (already referenced by the recorded draws, host-coherent, not yet submitted) are rewritten with the
 *                transforms for exactly those poses: the image is drawn for the pose the shell submits with it.
 *   flat frames  a frame the glue says is FLAT_* (menu, pause, FMV, scripted camera, scope) is drawn into the shell's
 *                flat quad-layer image (vr_acquire_flat_target): the main target's picture, the HUD over it. No eye
 *                images are touched; the shell shows that image as an OpenXR quad layer.
 *   HUD layer    in a STEREO frame the HUD surface (draws after the world, see hud_starts) goes to its own OpenXR
 *                quad layer (vr_acquire_hud_target), premultiplied alpha, instead of the virtual screen in the eye
 *                images.
 * Without a shell that provides those targets (weak defaults below) everything stays as before: the virtual screen
 * in the eye images.
 */

/* ---- weak defaults: no VR glue / shell -------------------------------------------------------------------------- */

__attribute__((weak)) bool kvk_vr_stereo_hook(KvkStereoFrame *out, const KvkStereoConfig *cfg, int phase)
{
    (void)out; (void)cfg; (void)phase;
    return false;
}
__attribute__((weak)) int kvk_vr_frame_mode(void) { return -1; }
__attribute__((weak)) void kvk_vr_view_mark(int mode, int slot) { (void)mode; (void)slot; }
__attribute__((weak)) void kvk_vr_frame_done(void) {}
__attribute__((weak)) bool kvk_vr_viewmodel_draw(const float *constants, const uint32_t pos_consts[6])
{
    (void)constants; (void)pos_consts;
    return false;
}
__attribute__((weak)) bool kvk_vr_viewmodel_hook(KvkStereoFrame *out, const KvkStereoConfig *cfg, int phase)
{
    (void)out; (void)cfg; (void)phase;
    return false;
}
__attribute__((weak)) uint32_t kvk_vr_viewmodel_tint(void) { return 0; }
__attribute__((weak)) bool kvk_vr_viewmodel_bones(float *constants, const void *vertex_data)
{
    (void)constants; (void)vertex_data;
    return false;
}
__attribute__((weak)) int vr_acquire_flat_target(VrEyeTarget *out) { (void)out; return 0; }
__attribute__((weak)) void vr_release_flat_target(void) {}
__attribute__((weak)) int vr_acquire_hud_target(VrEyeTarget *out) { (void)out; return 0; }
__attribute__((weak)) void vr_release_hud_target(void) {}
__attribute__((weak)) int kvk_vr_capture_wanted(void) { return 0; }
__attribute__((weak)) void kvk_vr_capture_done(uint8_t *rgba[2], uint32_t w, uint32_t h, uint64_t frame)
{
    (void)w; (void)h; (void)frame;
    free(rgba[0]); free(rgba[1]);
}

/* what this flip holds from the shell, released after the submit */
static struct { bool flat, hud; } s_vrl;

static void vrl_write_block(uint8_t *p, const KvkStereoFrame *st)
{
    memcpy(p, st->eye_clip, 128);
    memcpy(p + 128, st->quad_mvp, 128);
    memcpy(p + 256, st->misc, 16);
    memcpy(p + 272, st->laser_mvp, 256);
    memcpy(p + 528, st->laser_misc, 16);
}

/* The VR laser sight (the glue's laser_mvp, laser_misc): a beam and a dot drawn into the finished world of the main
 * target, against its depth (so walls and bodies in front hide it, and the beam ends in the surface it meets), blended
 * over, no depth write. After the flip's re-latch, so it is where the latched gun is. */
static void vrl_laser(KvkSurf *main_cs)
{
    KvkSurf *zs = V.main_zs;
    KvkPipeDesc d;
    KvkPush pc;
    VkPipeline pipe;
    VkViewport vp;
    VkRect2D sc;
    static const float bc[4] = { 0, 0, 0, 0 };
    uint32_t offs[6] = { 0, 0, 0, 0, 0, 0 };
    int k, zf = V.main_zfunc;
    if (!(V.st.laser_misc[0] > 0.5f) || !main_cs || !zs || !zs->used || !zs->depth || zs->hw != main_cs->hw ||
        zs->hh != main_cs->hh || zs->im.layers != main_cs->im.layers || V.stereo_draws == 0) {
        static unsigned n_why;
        if (V.st.laser_misc[0] > 0.5f && !(n_why++ % 600u))
            LOG("laser: no world depth to draw it against (depth %d, stereo draws %llu)", zs != NULL,
                (unsigned long long)V.stereo_draws);
        return;
    }
    if (zf == 1) zf = 3;                                /* LESS -> LEQUAL */
    else if (zf == 4) zf = 6;                           /* GREATER -> GEQUAL */
    else if (zf != 3 && zf != 6) zf = 3;
    if (!ring_reserve(KVK_STEREO_BYTES + 256) || !pass_begin(main_cs, zs)) return;
    memset(&d, 0, sizeof(d));
    d.vs = G.fix[KVK_FIX_LASER_VS]; d.fs = G.fix[KVK_FIX_LASER_PS]; d.rp = pass_compat();
    d.has_color = true;
    d.blend = pack_blend(true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                         VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 7);   /* colour only */
    d.ztest = true; d.zwrite = false; d.zfunc = zf;
    d.topo = RS_QUADSTRIP;
    d.cull = VK_CULL_MODE_NONE;
    if (!(pipe = pipe_get(&d))) return;
    offs[5] = (uint32_t)stereo_block();
    vp.x = 0.0f; vp.y = 0.0f; vp.width = (float)main_cs->hw; vp.height = (float)main_cs->hh;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    sc.offset.x = sc.offset.y = 0; sc.extent.width = main_cs->hw; sc.extent.height = main_cs->hh;
    bind_pipe(pipe);
    bind_set0(offs);
    set_dynamic(&vp, &sc, bc, 0, 0xFF, 0xFF, 0.0f, 0.0f);
    {
        static unsigned n_drawn;
        if (!(n_drawn++ % 3600u)) LOG("laser: drawn in the world (depth test %d)", zf);
    }
    for (k = 0; k < 2; k++) {
        memset(&pc, 0, sizeof(pc));
        pc.p0[0] = k ? V.st.laser_misc[2] : V.st.laser_misc[1];
        pc.p1[0] = -1.0f;                               /* multiview: each view its own eye */
        pc.p1[1] = (float)k;
        push(&pc);
        vkCmdDraw(cmd(), 4, 1, 0, 0);
    }
}

/* The glue's fade (V.st.fade, 0..1): black blended over the finished eye picture, after the laser, before the HUD --
 * the world fading in where the game cuts from its loading screen straight to a level still streaming in. */
static void vrl_fade(KvkSurf *main_cs)
{
    KvkSurf *zs = V.main_zs;
    KvkPipeDesc d;
    KvkPush pc;
    VkPipeline pipe;
    VkViewport vp;
    VkRect2D sc;
    static const float bc[4] = { 0, 0, 0, 0 };
    uint32_t offs[6] = { 0, 0, 0, 0, 0, 0 };
    if (!(V.st.fade > 0.002f) || !main_cs) return;
    if (zs && (!zs->used || !zs->depth || zs->hw != main_cs->hw || zs->hh != main_cs->hh ||
               zs->im.layers != main_cs->im.layers)) zs = NULL;
    if (!ring_reserve(KVK_STEREO_BYTES + 256) || !pass_begin(main_cs, zs)) return;
    memset(&d, 0, sizeof(d));
    d.vs = G.fix[KVK_FIX_LASER_VS]; d.fs = G.fix[KVK_FIX_LASER_PS]; d.rp = pass_compat();
    d.has_color = true;
    d.blend = pack_blend(true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                         VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 7);
    d.ztest = false; d.zwrite = false;
    d.topo = RS_QUADSTRIP;
    d.cull = VK_CULL_MODE_NONE;
    if (!(pipe = pipe_get(&d))) return;
    offs[5] = (uint32_t)stereo_block();
    vp.x = 0.0f; vp.y = 0.0f; vp.width = (float)main_cs->hw; vp.height = (float)main_cs->hh;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    sc.offset.x = sc.offset.y = 0; sc.extent.width = main_cs->hw; sc.extent.height = main_cs->hh;
    bind_pipe(pipe);
    bind_set0(offs);
    set_dynamic(&vp, &sc, bc, 0, 0xFF, 0xFF, 0.0f, 0.0f);
    memset(&pc, 0, sizeof(pc));
    pc.p0[0] = V.st.fade > 1.0f ? 1.0f : V.st.fade;
    pc.p1[0] = -1.0f;
    pc.p1[1] = 2.0f;                                    /* kind 2: the whole picture */
    push(&pc);
    vkCmdDraw(cmd(), 4, 1, 0, 0);
}

/* Phase FLIP: the glue's final transforms (for the poses vr_acquire_eye_targets just latched) into every stereo block
 * this frame's recorded draws use. The memory is host-coherent and the frame is not submitted yet. */
static void vrl_relatch(void)
{
    KvkStereoFrame st;
    KvkFrame *f = cur_frame();
    int i;
    if (!kvk_vr_stereo_hook(&st, &V.cfg, KVK_VR_PHASE_FLIP)) return;
    V.st = st;
    /* gun latch: the weapon's block, with the hand delta latched with these poses (else the frame's own) */
    if (!kvk_vr_viewmodel_hook(&V.st_vm, &V.cfg, KVK_VR_PHASE_FLIP)) V.st_vm = V.st;
    for (i = 0; i < f->nchunk; i++) {
        if (f->chunk[i].stereo) vrl_write_block(f->chunk[i].b.map + f->chunk[i].stereo_off, &V.st);
        if (f->chunk[i].stereo_vm) vrl_write_block(f->chunk[i].b.map + f->chunk[i].stereo_vm_off, &V.st_vm);
    }
}

static KvkSurf *vrl_hud_surface(const KvkSurf *main_cs)
{
    unsigned int i;
    KvkSurf *hud_cs = NULL;
    if (!V.hud_used || !main_cs) return NULL;
    for (i = 0; i < KVK_MAX_SURF; i++)
        if (G.surf[i].used && G.surf[i].hud && !G.surf[i].depth && G.surf[i].va == main_cs->va) hud_cs = &G.surf[i];
    return hud_cs;
}

typedef struct VrlQuad { KvkSurf *s; uint32_t layer; bool opaque, blend; } VrlQuad;

/* One single-view pass over a 2D target (layer 0 of a quad-layer swapchain image), cleared to transparent black: each
 * quad covers the whole target, sampled from layer `layer` of its surface. opaque: rgb, alpha 1. Otherwise the
 * surface's premultiplied colour, blended over (blend) or copied with its alpha. */
static void vrl_pass(KvkEyeView *ev, const VrlQuad *q, int nq)
{
    VkRenderPass rp = rp_get(ev->fmt, VK_FORMAT_UNDEFINED, 1, LOAD_CLEAR, LOAD_LOAD);
    VkRenderPass compat = rp_get(ev->fmt, VK_FORMAT_UNDEFINED, 1, LOAD_LOAD, LOAD_LOAD);
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue cv;
    VkViewport vp = { 0.0f, 0.0f, (float)ev->w, (float)ev->h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { ev->w, ev->h } };
    static const float bc[4] = { 0, 0, 0, 0 };
    uint32_t offs[6] = { 0, 0, 0, 0, 0, 0 };
    KvkStereoFrame full;
    VkDeviceSize off;
    int i;
    if (!rp || !compat) return;
    if (!ev->fb[0]) {
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = compat;
        fi.attachmentCount = 1;
        fi.pAttachments = &ev->view;
        fi.width = ev->w; fi.height = ev->h; fi.layers = 1;
        if (!VKOK(vkCreateFramebuffer(G.dev, &fi, NULL, &ev->fb[0]))) return;
        ev->rp[0] = compat;
    }
    if (!ring_reserve(KVK_STEREO_BYTES + 256)) return;
    /* quad_mvp: the unit quad (x, y in -0.5..0.5) onto the whole target */
    memset(&full, 0, sizeof(full));
    for (i = 0; i < 2; i++) {
        kvk_mat4_identity(full.eye_clip[i]);
        kvk_mat4_identity(full.quad_mvp[i]);
        full.quad_mvp[i][0] = 2.0f;
        full.quad_mvp[i][5] = 2.0f;
    }
    vrl_write_block(ring_take(KVK_STEREO_BYTES, ualign(), &off), &full);
    offs[5] = (uint32_t)off;
    memset(&cv, 0, sizeof(cv));
    bi.renderPass = rp;
    bi.framebuffer = ev->fb[0];
    bi.renderArea.extent.width = ev->w;
    bi.renderArea.extent.height = ev->h;
    bi.clearValueCount = 1;
    bi.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd(), &bi, VK_SUBPASS_CONTENTS_INLINE);
    P.pipe = VK_NULL_HANDLE; P.set1 = VK_NULL_HANDLE; P.bound0 = false; P.dyn = false;
    for (i = 0; i < nq; i++) {
        const uint32_t bl = q[i].blend
            ? pack_blend(true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                         VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 15)
            : blend_none(15);
        VkPipeline pipe = fixed_pipe(G.fix[KVK_FIX_QUAD_VS], G.fix[KVK_FIX_QUAD_PS], compat, true, false, RS_QUADSTRIP, bl);
        KvkPush pc;
        if (!pipe || !q[i].s) continue;
        memset(&pc, 0, sizeof(pc));
        pc.p0[0] = (float)(q[i].layer < q[i].s->im.layers ? q[i].layer : 0);
        pc.p0[1] = q[i].opaque ? -1.0f : 1.0f;
        pc.p1[0] = 0.0f;
        bind_pipe(pipe);
        bind_set0(offs);
        bind_set1(tset_single(q[i].s->im.view, G.smp_linear));
        set_dynamic(&vp, &sc, bc, 0, 0xFF, 0xFF, 0.0f, 0.0f);
        push(&pc);
        vkCmdDraw(cmd(), 4, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd());
    P.pipe = VK_NULL_HANDLE; P.set1 = VK_NULL_HANDLE; P.bound0 = false; P.dyn = false;
}

static KvkEyeView *vrl_target_view(const VrEyeTarget *t)
{
    const VkFormat vf = s_dev.swapchain_mutable_format ? unorm_twin((VkFormat)t->format) : (VkFormat)t->format;
    return eye_view((VkImage)(uintptr_t)t->image, t->array_layer, 1, vf, t->width, t->height);
}

/* A FLAT_* frame: the whole picture (main target, HUD over it) into the flat quad-layer image. False: not a flat frame
 * or no flat target (the eye images then show it on the virtual screen, as without a glue). */
static bool vrl_compose_flat(KvkSurf *main_cs)
{
    VrEyeTarget ft;
    VrlQuad q[2];
    KvkSurf *hud_cs;
    KvkEyeView *ev;
    int nq = 0;
    if (kvk_vr_frame_mode() <= 0 || !vr_active() || !vr_acquire_flat_target(&ft)) return false;
    s_vrl.flat = true;
    hud_cs = vrl_hud_surface(main_cs);
    surf_for_read(main_cs);
    if (hud_cs) surf_for_read(hud_cs);
    pass_end();
    if (!(ev = vrl_target_view(&ft))) return true;
    q[nq].s = main_cs; q[nq].layer = 0; q[nq].opaque = true; q[nq].blend = false; nq++;
    if (hud_cs) { q[nq].s = hud_cs; q[nq].layer = 0; q[nq].opaque = false; q[nq].blend = true; nq++; }
    vrl_pass(ev, q, nq);
    return true;
}

/* A STEREO frame's HUD into the HUD quad-layer image (copied with its premultiplied alpha). False: no HUD target. */
static bool vrl_hud_layer(KvkSurf *hud_cs)
{
    VrEyeTarget ht;
    VrlQuad q;
    KvkEyeView *ev;
    if (!hud_cs || !vr_acquire_hud_target(&ht)) return false;
    s_vrl.hud = true;
    if (!(ev = vrl_target_view(&ht))) return true;
    q.s = hud_cs; q.layer = 0; q.opaque = false; q.blend = false;
    vrl_pass(ev, &q, 1);
    return true;
}

/* ---- both-eye capture (kvk_vr_hooks.h) ---- */
static struct { bool on; KvkImage img; KvkBuf buf; uint32_t w, h; } s_cap;

static void vrl_capture_free(void)
{
    int i;
    for (i = 0; i < KVK_EYEVIEW_MAX; i++) {                 /* its view (the frame using it has completed) */
        KvkEyeView *q = &G.eyeview[i];
        if (!q->view || q->img != s_cap.img.img) continue;
        if (q->fb[0]) vkDestroyFramebuffer(G.dev, q->fb[0], NULL);
        if (q->fb[1]) vkDestroyFramebuffer(G.dev, q->fb[1], NULL);
        tset_forget(q->view, VK_NULL_HANDLE);
        vkDestroyImageView(G.dev, q->view, NULL);
        memset(q, 0, sizeof(*q));
    }
    img_release(&s_cap.img);
    buf_destroy(&s_cap.buf);
    s_cap.on = false;
}

/* In a STEREO frame's composition: the eye pictures once more, into a 2-layer image of ours, copied to a buffer. */
static void vrl_capture_record(KvkSurf *main_cs, KvkSurf *hud_cs, bool world, uint32_t w, uint32_t h)
{
    VkBufferImageCopy r;
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    KvkEyeView *ev;
    if (G.layers < 2 || !kvk_vr_capture_wanted()) return;
    if (!img_create(&s_cap.img, VK_FORMAT_R8G8B8A8_UNORM, w, h, 2, 1,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) { LOG("capture: no image"); return; }
    if (!buf_create(&s_cap.buf, (VkDeviceSize)w * h * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BUF_READBACK) ||
        !(ev = eye_view(s_cap.img.img, 0, 2, VK_FORMAT_R8G8B8A8_UNORM, w, h))) {
        LOG("capture: no readback buffer");
        vrl_capture_free();
        return;
    }
    s_cap.on = true; s_cap.w = w; s_cap.h = h;
    pass_end();
    barrier(cmd(), &s_cap.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    compose_pass(ev, 0, main_cs, hud_cs, world);
    barrier(cmd(), &s_cap.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 2;
    r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd(), s_cap.img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_cap.buf.buf, 1, &r);
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = s_cap.buf.buf;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
}

/* After the capture frame's (waited) submit: the two pictures to the glue. */
static void vrl_capture_deliver(uint64_t frame)
{
    uint8_t *px[2];
    const size_t n = (size_t)s_cap.w * s_cap.h * 4;
    if (!s_cap.on) return;
    if (!s_cap.buf.coherent) {
        VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        mr.memory = s_cap.buf.mem; mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(G.dev, 1, &mr);
    }
    px[0] = (uint8_t *)malloc(n);
    px[1] = (uint8_t *)malloc(n);
    if (px[0] && px[1]) {
        memcpy(px[0], s_cap.buf.map, n);
        memcpy(px[1], s_cap.buf.map + n, n);
        kvk_vr_capture_done(px, s_cap.w, s_cap.h, frame);
    } else {
        free(px[0]); free(px[1]);
    }
    vrl_capture_free();
}

/* After the flip's submit: hand the images back. The HUD before the eyes: the shell publishes both with the eye
 * release (one projection layer + its HUD, never a HUD of another frame). */
static void vrl_after_submit(bool eyes)
{
    if (s_vrl.flat) vr_release_flat_target();
    if (s_vrl.hud) vr_release_hud_target();
    if (eyes) vr_release_eye_targets();
    s_vrl.flat = s_vrl.hud = false;
    kvk_vr_frame_done();
}
