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
