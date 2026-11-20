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


/* ---- animation detail (the vanishing head and legs, found 2026-11-19) -------------------------------------------
 * sub_0017DA70 lowers a character's animation detail when it is far (level 1) or when the game's flat camera does not
 * see it (level 2): fewer bones are animated and the scratch-pose pool shrinks, so the head and leg bones keep a stale
 * pose and blends are dropped. The headset log of 2026-11-19 15:25 shows it: a soldier's bones jump to the wrong pose
 * exactly when his evaluation's result moves from the third scratch pose (a blend) to the first (no blend), and back.
 * In VR the body is on show in both cases, so the detail stays full (Kenny, 2026-11-20: bodies stopped breaking). RECOMP_BLACK_ANIM_LOD=1 gives the game's own
 * behaviour back. Returns 1 to keep full detail; logs, per character, when what the game wanted changes. */
int black_anim_keep_full(uint32_t character, unsigned wanted)
{
#ifdef BLACK_VR
    static int keep = -1;
    static struct { uint32_t character; unsigned wanted; unsigned long long calls, since; } t[16];
    static unsigned long long calls, lines;
    int i, slot = -1;
    if (keep < 0) {
        const char *e = getenv("RECOMP_BLACK_ANIM_LOD");
        keep = !(e && e[0] == '1');
        fprintf(stderr, "[ANIM] animation detail: %s\n", keep ? "always full in VR (the game would lower it for far and unseen characters)"
                                                              : "the game's own (RECOMP_BLACK_ANIM_LOD=1)");
    }
    calls++;
    for (i = 0; i < 16; i++) {
        if (t[i].character == character) { slot = i; break; }
        if (slot < 0 && !t[i].character) slot = i;
    }
    if (slot >= 0) {
        /* a new stretch: this character was not lowered on its last few updates, or the level differs */
        if (t[slot].character != character || t[slot].wanted != wanted || calls - t[slot].calls > 64u) {
            if (lines++ < 400u || !(lines % 200u))
                fprintf(stderr, "[ANIM] character %08X: the game wants animation level %u (%s)%s [%llu]\n", character, wanted,
                        wanted == 1u ? "far" : "not seen by the flat camera", keep ? "; kept full" : "", lines);
            t[slot].since = calls;
        }
        t[slot].character = character; t[slot].wanted = wanted; t[slot].calls = calls;
    }
    return keep;
#else
    (void)character; (void)wanted;
    return 0;
#endif
}
