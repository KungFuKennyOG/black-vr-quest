/* Kelvin GPU backends: guest texture decoding (swizzle, texel formats, YUV, DXT) to RGBA8. Moved unchanged from
 * kelvin_gpu.c (kelvin_gfx.h); both backends upload what these produce. Needs kelvin_render.h. Outside Windows,
 * _BitScanForward is the compiler builtin. */
#if !defined(_WIN32) && !defined(_BitScanForward)
#define _BitScanForward(index, mask) ((void)(*(index) = (unsigned long)__builtin_ctz((unsigned int)(mask))))
#endif
static uint32_t spread_bits16(uint32_t v)
{
    v &= 0xFFFF;
    v = (v | (v << 8)) & 0x00FF00FF;
    v = (v | (v << 4)) & 0x0F0F0F0F;
    v = (v | (v << 2)) & 0x33333333;
    v = (v | (v << 1)) & 0x55555555;
    return v;
}

static uint32_t swz_off(unsigned int w, unsigned int h, unsigned int x, unsigned int y)
{
    unsigned long m;
    uint32_t low;
    _BitScanForward(&m, w < h ? w : h);
    low = (1u << m) - 1;
    return spread_bits16(x & low) | (spread_bits16(y & low) << 1) |
           (((x >> m) | (y >> m)) << (2 * m));
}

static uint32_t pack_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

static uint32_t expand5(uint32_t v) { return (v * 255 + 15) / 31; }
static uint32_t expand6(uint32_t v) { return (v * 255 + 31) / 63; }
static uint32_t expand4(uint32_t v) { return v * 17; }

static uint8_t clip_byte(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

static uint32_t texel_rgba(const KTexture *t, const uint8_t *p)
{
    uint32_t v, l;
    switch (t->kind) {
    case TK_Y8:   return pack_rgba(p[0], p[0], p[0], 255);
    case TK_AY8:  return pack_rgba(p[0], p[0], p[0], p[0]);
    case TK_A8:   return pack_rgba(255, 255, 255, p[0]);
    case TK_A8Y8: return pack_rgba(p[0], p[0], p[0], p[1]);
    case TK_G8B8: return pack_rgba(p[0], p[1], p[0], p[1]);
    case TK_R8B8: return pack_rgba(p[1], p[0], p[0], p[1]);
    case TK_1555: case TK_X555:
        v = p[0] | (p[1] << 8);
        return pack_rgba(expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31),
                         t->kind == TK_X555 ? 255 : ((v >> 15) & 1) * 255);
    case TK_4444:
        v = p[0] | (p[1] << 8);
        return pack_rgba(expand4((v >> 8) & 15), expand4((v >> 4) & 15), expand4(v & 15),
                         expand4((v >> 12) & 15));
    case TK_565:
        v = p[0] | (p[1] << 8);
        return pack_rgba(expand5((v >> 11) & 31), expand6((v >> 5) & 63), expand5(v & 31), 255);
    case TK_8888: return pack_rgba(p[2], p[1], p[0], p[3]);
    case TK_X888: return pack_rgba(p[2], p[1], p[0], 255);
    case TK_ABGR: return pack_rgba(p[0], p[1], p[2], p[3]);
    case TK_BGRA: return pack_rgba(p[1], p[2], p[3], p[0]);
    case TK_RGBA: return pack_rgba(p[3], p[2], p[1], p[0]);
    case TK_I8: {
        const uint8_t *q;
        if (!t->palette_mem || p[0] >= t->palette_len) return pack_rgba(0, 0, 0, 255);
        q = t->palette_mem + p[0] * 4;
        return pack_rgba(q[2], q[1], q[0], q[3]);
    }
    case TK_Y16:
        v = p[0] | (p[1] << 8);
        l = (v * 255 + 32767) / 65535;
        return pack_rgba(l, l, l, 255);
    default:
        return pack_rgba(0, 0, 0, 255);
    }
}

static uint32_t yuv_rgba(const KTexture *t, const uint8_t *line, unsigned int x)
{
    int c, d, e;
    if (t->kind == TK_YUY2) {
        c = (int)line[x * 2] - 16;
        if (x % 2) { d = (int)line[x * 2 - 1] - 128; e = (int)line[x * 2 + 1] - 128; }
        else       { d = (int)line[x * 2 + 1] - 128; e = (int)line[x * 2 + 3] - 128; }
    } else {
        c = (int)line[x * 2 + 1] - 16;
        if (x % 2) { d = (int)line[x * 2 - 2] - 128; e = (int)line[x * 2 + 0] - 128; }
        else       { d = (int)line[x * 2 + 0] - 128; e = (int)line[x * 2 + 2] - 128; }
    }
    return pack_rgba(clip_byte((298 * c + 409 * e + 128) >> 8),
                     clip_byte((298 * c - 100 * d - 208 * e + 128) >> 8),
                     clip_byte((298 * c + 516 * d + 128) >> 8), 255);
}

static void decode_level(const KTexture *t, unsigned int level, uint32_t *out)
{
    unsigned int w = t->level_w[level], h = t->level_h[level], x, y;
    const uint8_t *base = t->mem + t->level_offset[level];
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t px;
            if (t->kind == TK_YUY2 || t->kind == TK_UYVY) {
                px = yuv_rgba(t, base + (size_t)y * t->pitch, x);
            } else if (t->linear) {
                px = texel_rgba(t, base + (size_t)y * t->pitch + (size_t)x * t->bpp);
            } else {
                px = texel_rgba(t, base + (size_t)swz_off(w, h, x, y) * t->bpp);
            }
            out[(size_t)y * w + x] = px;
        }
    }
}

static bool kind_is_dxt(unsigned int kind)
{
    return kind == TK_DXT1 || kind == TK_DXT3 || kind == TK_DXT5;
}

/* Bytes of guest memory the texture occupies. */
static uint32_t texture_span(const KTexture *t)
{
    uint32_t total = 0;
    unsigned int level;
    for (level = 0; level < t->levels && level < KTEX_MAX_LEVELS; level++) {
        unsigned int w = t->level_w[level], h = t->level_h[level];
        if (kind_is_dxt(t->kind)) {
            total = t->level_offset[level] + ((w + 3) / 4) * ((h + 3) / 4) * (t->kind == TK_DXT1 ? 8 : 16);
        } else if (t->linear) {
            total = t->level_offset[level] + t->pitch * h;
        } else {
            total = t->level_offset[level] + w * h * t->bpp;
        }
    }
    return total;
}

/* Reference BC1/2/3 decoder for levels D3D11 refuses (not a multiple of four). */
static void dxt_decode_block(unsigned int kind, const uint8_t *b, uint32_t px[16])
{
    const uint8_t *cb = kind == TK_DXT1 ? b : b + 8;
    uint32_t c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8), idx, pal[4], i;
    uint8_t alpha[16];
    #define EXP(c, r, g, bb) do { r = expand5((c >> 11) & 31); g = expand6((c >> 5) & 63); bb = expand5(c & 31); } while (0)
    uint32_t r0, g0, b0, r1, g1, b1;
    EXP(c0, r0, g0, b0); EXP(c1, r1, g1, b1);
    pal[0] = pack_rgba(r0, g0, b0, 255);
    pal[1] = pack_rgba(r1, g1, b1, 255);
    if (kind == TK_DXT1 && c0 <= c1) {
        pal[2] = pack_rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        pal[3] = pack_rgba(0, 0, 0, 0);
    } else {
        pal[2] = pack_rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        pal[3] = pack_rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    }
    memcpy(&idx, cb + 4, 4);
    if (kind == TK_DXT3) {
        for (i = 0; i < 16; i++) alpha[i] = (uint8_t)expand4((b[i / 2] >> ((i & 1) * 4)) & 15);
    } else if (kind == TK_DXT5) {
        uint64_t bits = 0;
        uint8_t a0 = b[0], a1 = b[1], ap[8];
        memcpy(&bits, b + 2, 6);
        ap[0] = a0; ap[1] = a1;
        if (a0 > a1) { for (i = 1; i < 7; i++) ap[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7); }
        else { for (i = 1; i < 5; i++) ap[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5); ap[6] = 0; ap[7] = 255; }
        for (i = 0; i < 16; i++) alpha[i] = ap[(bits >> (3 * i)) & 7];
    } else {
        memset(alpha, 255, sizeof(alpha));
    }
    for (i = 0; i < 16; i++) {
        uint32_t c = pal[(idx >> (2 * i)) & 3];
        px[i] = kind == TK_DXT1 ? c : (c & 0x00FFFFFFu) | ((uint32_t)alpha[i] << 24);
    }
    #undef EXP
}

static void decode_dxt_level(const KTexture *t, unsigned int level, uint32_t *out)
{
    unsigned int w = t->level_w[level], h = t->level_h[level], bw = (w + 3) / 4, bh = (h + 3) / 4;
    unsigned int block = t->kind == TK_DXT1 ? 8 : 16, bx, by, i;
    const uint8_t *base = t->mem + t->level_offset[level];
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            uint32_t px[16];
            dxt_decode_block(t->kind, base + ((size_t)by * bw + bx) * block, px);
            for (i = 0; i < 16; i++) {
                unsigned int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) out[(size_t)y * w + x] = px[i];
            }
        }
    }
}
