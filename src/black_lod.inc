/* Native geometry LOD and prop visibility: scale only the compared metric.
 * The real camera distance, draw-sort keys, native mesh masks and count/index
 * bounds stay owned by BLACK. Camera FOV and texture mip selection are separate.
 */
enum {
    BLACK_LOD_DISTANCE = 0,
    BLACK_LOD_SQUARED_DISTANCE = 1,
    BLACK_LOD_SCREEN_RATIO = 2,
    BLACK_LOD_CHARACTER = 3            /* a character's distance metric (1C55A0): as BLACK_LOD_DISTANCE */
};

float black_lod_value(float native_value, unsigned mode)
{
    const unsigned caller_csr = _mm_getcsr();
    fenv_t caller_environment;
    PcVideoSettings settings;
    uint32_t bits;
    float value = native_value;
    unsigned percent;
    const int character = mode == BLACK_LOD_CHARACTER;
    if (character) mode = BLACK_LOD_DISTANCE;
    fegetenv(&caller_environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    pc_video_get(&settings);
    percent = settings.lod_distance;
    memcpy(&bits, &native_value, sizeof(bits));
    /* Exact identity includes negative values, signed zero, infinity, and NaN
     * payloads. Invalid/unknown settings or modes also retain the native value. */
    if ((percent == 200u || percent == 300u || percent == 500u || percent == 800u || percent == 1000u) &&
        bits > 0u && bits < 0x7F800000u && mode <= BLACK_LOD_SCREEN_RATIO) {
        const float scale = (float)percent / 100.0f;
        if (mode == BLACK_LOD_SCREEN_RATIO) value = native_value * scale;
        else if (mode == BLACK_LOD_SQUARED_DISTANCE) value = native_value / (scale * scale);
        else value = native_value / scale;
    }
    {
        /* RECOMP_BLACK_LOD_TEST=<factor> (diagnostic): every distance metric as if that many times further, so the
         * game picks its low-detail models up close (Kenny's vanishing limbs are the 352-index character model,
         * 2026-11-16) */
        static float test = -1.0f;
        if (test < 0.0f) { const char *e = getenv("RECOMP_BLACK_LOD_TEST"); test = e && atof(e) > 0.0 ? (float)atof(e) : 0.0f; }
        if (test > 0.0f && bits > 0u && bits < 0x7F800000u) {
            if (mode == BLACK_LOD_DISTANCE) value *= test;
        }
    }
#ifdef BLACK_VR
    {
        /* VR: characters keep their detailed models 8 times further out (RECOMP_BLACK_CHAR_LOD=<factor>, 1: the game's
         * own distances). On the headset the game's low-detail character models (352 indices and the like) lose their
         * head, arms and legs now and then (Kenny 2026-11-15/16: "only the upper body and butt/crotch floating"; the
         * skin dumps of his 19:22 session show those models wherever characters are far or small); not on SwiftShader.
         * Until that is understood they are simply not used at the distances a fight happens at. */
        static float boost = -1.0f;
        if (boost < 0.0f) { const char *e = getenv("RECOMP_BLACK_CHAR_LOD"); boost = e && atof(e) >= 1.0 ? (float)atof(e) : 8.0f; }
        if (character && boost > 1.0f && bits > 0u && bits < 0x7F800000u) value /= boost;
    }
#endif
    fesetenv(&caller_environment);
    _mm_setcsr(caller_csr);
    return value;
}

/* Opt-in bounded evidence for matched private routes. The choice is read after
 * the complete native selector, including forced/count-zero/count-one paths. */
void black_lod_trace(uint32_t site, uint32_t object)
{
    static RECOMP_TLS unsigned emitted;
    const char *enabled = getenv("RECOMP_BLACK_LOD_TRACE");
    uint32_t camera, distance_bits, factor_bits;
    float distance, factor, adjusted;
    PcVideoSettings settings;
    fenv_t caller_environment;
    unsigned caller_csr;
#ifdef BLACK_VR
    if (site == 0x001C55A0u && (!enabled || strcmp(enabled, "1")) && black_pc_ram_span(object, 0xC9u) &&
        black_pc_ram_span(0x002D12B8u, 4u)) {
        /* VR, every 5 s for 40 ms (two or three game frames): every character the game picked a model for, with the
         * choice. Read against the renderer's skin dumps (kvk_backend.c), which list the bodies actually drawn at
         * about the same moment: a character here and no body there was dropped after this point; a character
         * missing here was never considered (Kenny's vanishing bodies, 2026-11-16). */
        static RECOMP_TLS double window_until, next_at;
        struct timespec ts;
        double now;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
        if (now >= next_at) { next_at = now + 5.0; window_until = now + 0.04; }
        if (now <= window_until) {
            const unsigned csr0 = _mm_getcsr();
            fenv_t env0;
            const uint32_t cam = GUEST32(0x002D12B8u);
            uint32_t db = GUEST32(object + 0xC0u), fb = black_pc_ram_span(cam, 0x16A4u) ? GUEST32(cam + 0x16A0u) : 0u;
            uint32_t pos[3] = { 0u, 0u, 0u };
            float d, f, px, py, pz;
            if (black_pc_ram_span(object, 0x40u)) { pos[0] = GUEST32(object + 0x30u); pos[1] = GUEST32(object + 0x34u); pos[2] = GUEST32(object + 0x38u); }
            fegetenv(&env0);
            fesetenv(FE_DFL_ENV);
            _mm_setcsr(0x1F80u);
            memcpy(&d, &db, 4); memcpy(&f, &fb, 4); memcpy(&px, &pos[0], 4); memcpy(&py, &pos[1], 4); memcpy(&pz, &pos[2], 4);
            fprintf(stderr, "[LOD] character %08X model %u of its set, distance %.2f, camera factor %.3f, +30 %.2f %.2f %.2f, "
                    "flags +C4 %08X +C8 %08X\n", object, (unsigned)*(const uint8_t *)((uintptr_t)g_xbox_mem_offset + object + 0xC8u),
                    (double)d, (double)f, (double)px, (double)py, (double)pz, GUEST32(object + 0xC4u), GUEST32(object + 0xC8u));
            fesetenv(&env0);
            _mm_setcsr(csr0);
        }
        return;
    }
#endif
    if (!enabled || strcmp(enabled, "1") || emitted >= 512u || !black_pc_ram_span(object, 0xC9u) ||
        !black_pc_ram_span(0x002D12B8u, 4u)) return;
    camera = GUEST32(0x002D12B8u);
    if (!black_pc_ram_span(camera, 0x16A4u)) return;
    caller_csr = _mm_getcsr();
    fegetenv(&caller_environment);
    fesetenv(FE_DFL_ENV);
    _mm_setcsr(0x1F80u);
    distance_bits = GUEST32(object + 0xC0u);
    factor_bits = GUEST32(camera + 0x16A0u);
    memcpy(&distance, &distance_bits, sizeof(distance));
    memcpy(&factor, &factor_bits, sizeof(factor));
    pc_video_get(&settings);
    adjusted = black_lod_value(distance * factor, BLACK_LOD_DISTANCE);
    fprintf(stderr, "[LOD] site=%08X object=%08X selected=%u distance=%.9g native_metric=%.9g scaled_metric=%.9g percent=%u\n",
            site, object, (unsigned)*(const uint8_t *)((uintptr_t)g_xbox_mem_offset + object + 0xC8u),
            (double)distance, (double)(distance * factor), (double)adjusted, settings.lod_distance);
    ++emitted;
    fesetenv(&caller_environment);
    _mm_setcsr(caller_csr);
}


/* ---- pose trace (vanishing head/legs, 2026-11-17) -----------------------------------------------------------------
 * On the headset a soldier's head and leg bones snap to one fixed other pose for 0.3-0.9 s and snap back; torso and arm
 * bones do not move. sub_00091EF0 produces the pose: sub_00097E00 has sub_00097380 walk the skeleton's animation tree
 * (root at +0xC, recursing per node) into one of two shared scratch poses (0x2C3270, 0x2C3F50). These three calls
 * (scripts/generate.py pose_trace_hooks) log, per skeleton, that walk whenever it differs from the one before: which
 * nodes were visited, their first words, the flags they were visited with, and where the result is. Reads only. */
typedef struct BlackPoseNode { uint32_t node, w[6], child[8], flags, extra; float time, prev; } BlackPoseNode;
typedef struct BlackPoseWalk { uint32_t skeleton, result, state, f98C, f970; int n, total, overrides, f1C8; BlackPoseNode l[8]; } BlackPoseWalk;
static BlackPoseWalk black_pose_now;
static BlackPoseWalk black_pose_last[24];
static unsigned long long black_pose_changes;
static int black_pose_open;

void black_pose_begin(uint32_t skeleton)
{
    black_pose_now.skeleton = skeleton; black_pose_now.result = 0; black_pose_now.n = 0; black_pose_now.total = 0;
    black_pose_now.state = black_pose_now.f98C = black_pose_now.f970 = 0u; black_pose_now.overrides = black_pose_now.f1C8 = 0;
    if (black_pc_ram_span(skeleton, 0x9B0u)) {
        black_pose_now.state = GUEST32(skeleton); black_pose_now.f98C = GUEST32(skeleton + 0x98Cu);
        black_pose_now.f970 = GUEST32(skeleton + 0x970u);
        black_pose_now.overrides = (int16_t)(GUEST32(skeleton + 0x5Cu) & 0xFFFFu);       /* bone overrides (sub_00095680) */
        black_pose_now.f1C8 = (int16_t)(GUEST32(skeleton + 0x1C8u) & 0xFFFFu);
    }
    black_pose_open = 1;
}

/* sub_00097380(skeleton, node, x, time, previous time, flags, n): its arguments, at its entry */
void black_pose_node(uint32_t esp)
{
    BlackPoseNode *l;
    uint32_t node, t, p;
    int i;
    if (!black_pose_open || !black_pc_ram_span(esp, 0x20u)) return;
    black_pose_now.total++;
    if (black_pose_now.n >= 8) return;
    node = GUEST32(esp + 8u);
    l = &black_pose_now.l[black_pose_now.n++];
    l->node = node; l->flags = GUEST32(esp + 0x18u); l->extra = GUEST32(esp + 0x1Cu);
    t = GUEST32(esp + 0x10u); p = GUEST32(esp + 0x14u);
    memcpy(&l->time, &t, 4); memcpy(&l->prev, &p, 4);
    for (i = 0; i < 6; i++) l->w[i] = black_pc_ram_span(node, 0x18u) ? GUEST32(node + 4u * (uint32_t)i) : 0u;
    for (i = 0; i < 8; i++) l->child[i] = black_pc_ram_span(l->w[4], 0x20u) ? GUEST32(l->w[4] + 4u * (uint32_t)i) : 0u;   /* what the node points at */
}

void black_pose_end(uint32_t result)
{
    BlackPoseWalk *last = NULL;
    int i, changed = 0;
    if (!black_pose_open) return;
    black_pose_open = 0;
    black_pose_now.result = result;
    for (i = 0; i < 24; i++) {
        if (black_pose_last[i].skeleton == black_pose_now.skeleton) { last = &black_pose_last[i]; break; }
        if (!last && !black_pose_last[i].skeleton) last = &black_pose_last[i];
    }
    if (!last) return;
    if (last->skeleton != black_pose_now.skeleton || last->result != result || last->n != black_pose_now.n ||
        last->total != black_pose_now.total || last->state != black_pose_now.state || last->overrides != black_pose_now.overrides ||
        last->f98C != black_pose_now.f98C || last->f1C8 != black_pose_now.f1C8) changed = 1;
    for (i = 0; !changed && i < black_pose_now.n; i++)
        if (last->l[i].node != black_pose_now.l[i].node || last->l[i].flags != black_pose_now.l[i].flags ||
            last->l[i].w[0] != black_pose_now.l[i].w[0] || last->l[i].w[1] != black_pose_now.l[i].w[1] ||
            last->l[i].w[4] != black_pose_now.l[i].w[4] || last->l[i].child[0] != black_pose_now.l[i].child[0] ||
            last->l[i].child[1] != black_pose_now.l[i].child[1] || last->l[i].child[2] != black_pose_now.l[i].child[2] ||
            last->l[i].child[3] != black_pose_now.l[i].child[3]) changed = 1;
    if (changed) {
        black_pose_changes++;
        if (black_pose_changes <= 1500u || !(black_pose_changes % 500u)) {
            /* the host's own floating-point state for the formatting, the guest's back afterwards (as black_lod_trace) */
            const unsigned csr0 = _mm_getcsr();
            fenv_t env0;
            char line[1100]; int n = 0;
            fegetenv(&env0);
            fesetenv(FE_DFL_ENV);
            _mm_setcsr(0x1F80u);
            line[0] = 0;
            for (i = 0; i < black_pose_now.n && n < 980; i++) {
                const BlackPoseNode *l = &black_pose_now.l[i];
                n += snprintf(line + n, sizeof line - (size_t)n, " | node %08X [%08X %08X %08X %08X %08X %08X] -> [%08X %08X %08X %08X %08X "
                              "%08X %08X %08X] flags %X/%d t %.4f prev %.4f", l->node, l->w[0], l->w[1], l->w[2], l->w[3], l->w[4], l->w[5],
                              l->child[0], l->child[1], l->child[2], l->child[3], l->child[4], l->child[5], l->child[6], l->child[7],
                              l->flags, (int)l->extra, (double)l->time, (double)l->prev);
            }
            fprintf(stderr, "[POSE] skeleton %08X state %08X (was %08X) overrides %d +1C8 %d +98C %08X +970 %08X: %d node visit(s), result %08X "
                    "(was %d visit(s), %08X)%s [change %llu]\n", black_pose_now.skeleton, black_pose_now.state,
                    last->skeleton == black_pose_now.skeleton ? last->state : 0u, black_pose_now.overrides, black_pose_now.f1C8,
                    black_pose_now.f98C, black_pose_now.f970, black_pose_now.total, result,
                    last->skeleton == black_pose_now.skeleton ? last->total : -1,
                    last->skeleton == black_pose_now.skeleton ? last->result : 0u, line, black_pose_changes);
            fesetenv(&env0);
            _mm_setcsr(csr0);
        }
    }
    *last = black_pose_now;
}
