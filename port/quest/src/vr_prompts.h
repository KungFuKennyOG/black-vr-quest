/*
 * vr_prompts.h - BLACK's button prompts for the Quest controllers. Pure C, header-only, no game data; unit-tested
 * in tests/xr_shell (test_prompts.c). The game glue is src/black_vr_prompts.inc; notes in port/notes/prompts.md.
 *
 * 1. Which Quest input each of BLACK's action slots is on in the VR layer (vr_input_map.h, vr_gestures.h): the
 *    VR layer produces actions, not buttons, so a prompt names the Quest input of the ACTION, whatever Xbox control
 *    the game's live map gives it. left_handed (gun in the left hand) mirrors the hands.
 * 2. The prompt text in the game's own style ("PRESS <glyph> :", "PULL <glyph> :"), as UTF-16 for the game's
 *    bitmap fonts: one font character per Quest input (vrp_glyph_chars: the pad-art characters the game already has,
 *    U+00B2 A, U+00B9 B, U+00B3 X, U+00B0 Y, the plates and the d-pad, and five characters no game text uses), each
 *    carrying that input's glyph in VR.
 * 3. The glyphs: the user-provided Quest glyph sheet (port/quest/assets/quest_glyphs.png, made into
 *    vr_quest_glyphs.h by port/quest/tools/make_quest_glyphs.py), shaded like the game's own pad glyphs and filtered
 *    down to the font's cells, written into the font atlas as DXT3 (the fonts' format) blocks; and the user-provided
 *    controller drawing (quest_controllers.png) for the Controls page's diagram (DXT5, the front end's format).
 */
#ifndef VR_PROMPTS_H
#define VR_PROMPTS_H

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "vr_quest_glyphs.h"

/* ---- 1. action slot -> Quest input -------------------------------------------------------------------------- */

typedef enum {
    VRP_NONE = 0,          /* not on the controllers: the game's own text is left alone */
    VRP_TRIGGER,           /* hand's trigger */
    VRP_GRIP,              /* hand's grip */
    VRP_STICK,             /* hand's stick, any direction (move) */
    VRP_STICK_SIDE,        /* left / right (turn) */
    VRP_STICK_UP,          /* one push up (next weapon) */
    VRP_STICK_DOWN,        /* one push down (previous weapon) */
    VRP_STICK_UPDOWN,      /* up / down (the scope's zoom) */
    VRP_STICK_CLICK,       /* hand's stick click */
    VRP_BUTTON_LOW,        /* A (right hand) / X (left hand) */
    VRP_BUTTON_HIGH,       /* B (right) / Y (left) */
    VRP_MENU,              /* the left controller's Menu button */
    VRP_HEAD,              /* look: the head */
    VRP_RAISE              /* raise the gun to the eye (the scope) */
} VrPromptKind;

typedef struct {
    uint8_t kind;          /* VrPromptKind */
    uint8_t hand;          /* 0 left, 1 right */
    uint8_t hold;          /* held, not tapped (Y hold = Fire mode, Menu hold = Back) */
} VrPromptInput;

enum { VRP_LEFT = 0, VRP_RIGHT = 1 };

/* BLACK's action slots (pc_input.c's table; vr_input_map.h k_vra_slot): 1..4 move, 5/6 look left/right,
 * 7/8 look up/down, 9 fire, 11 reload, 12 crouch, 13 use, 14/15 next/previous weapon, 16 aim, 25 pause,
 * 27 grenade, 30 fire mode, 31 suppressor, 32 melee, 33 health pack, 34 cycle item, 35 scope zoom, 36 back.
 * gun_hand: VRP_RIGHT normally, VRP_LEFT with left_handed = 1. */
static inline VrPromptInput vrp_slot_input(int slot, int gun_hand)
{
    const uint8_t gun = gun_hand == VRP_LEFT ? VRP_LEFT : VRP_RIGHT, off = (uint8_t)(1 - gun);
    VrPromptInput r = { VRP_NONE, gun, 0 };
    switch (slot) {
    case 1: case 2: case 3: case 4: r.kind = VRP_STICK; r.hand = off; break;
    case 5: case 6: r.kind = VRP_STICK_SIDE; break;
    case 7: case 8: r.kind = VRP_HEAD; break;
    case 9: r.kind = VRP_TRIGGER; break;
    case 11: r.kind = VRP_BUTTON_LOW; break;
    case 12: r.kind = VRP_STICK_CLICK; r.hand = off; break;
    case 13: r.kind = VRP_STICK_CLICK; break;
    case 14: r.kind = VRP_STICK_UP; break;
    case 15: r.kind = VRP_STICK_DOWN; break;
    case 16: r.kind = VRP_RAISE; break;
    case 25: r.kind = VRP_MENU; r.hand = VRP_LEFT; break;
    case 27: r.kind = VRP_TRIGGER; r.hand = off; break;
    case 30: r.kind = VRP_BUTTON_HIGH; r.hand = off; r.hold = 1; break;
    case 31: r.kind = VRP_BUTTON_HIGH; r.hand = off; break;
    case 32: r.kind = VRP_BUTTON_HIGH; break;
    case 33: r.kind = VRP_BUTTON_LOW; r.hand = off; break;
    case 35: r.kind = VRP_STICK_UPDOWN; break;
    case 36: r.kind = VRP_MENU; r.hand = VRP_LEFT; r.hold = 1; break;
    default: break;        /* 10, 17..24, 26, 28, 29, 34 (cycle item): not on the controllers in VR */
    }
    return r;
}

/* ---- 2. the text -------------------------------------------------------------------------------------------- */

#define VRP_CH_A        0x00B2u    /* the game's own A..Y characters */
#define VRP_CH_B        0x00B9u
#define VRP_CH_X        0x00B3u
#define VRP_CH_Y        0x00B0u
#define VRP_CH_LT       0x2514u    /* the trigger plate */
#define VRP_CH_RT       0x2518u    /* the stick plate */
#define VRP_CH_RG       0x250Cu    /* Black: the right grip in VR menus (vr_map_menu_pad) */
#define VRP_CH_LG       0x2510u    /* White: the left grip */
#define VRP_CH_R_UD     0x25B2u    /* the d-pad's four (in VR menus the right stick is the d-pad) */
#define VRP_CH_L_UD     0x25BCu
#define VRP_CH_R_LR     0x25BAu
#define VRP_CH_L_LR     0x25C4u
#define VRP_CH_MENU     0x00A5u    /* five characters the game's text never uses, given cells of their own */
#define VRP_CH_R_CLICK  0x20ACu
#define VRP_CH_L_CLICK  0x00A3u
#define VRP_CH_R_STICK  0x00BBu
#define VRP_CH_L_STICK  0x00ABu

/* Which glyph each character carries in VR. `own`: the character's cell already is big enough to reuse. */
typedef struct { uint16_t ch; uint8_t glyph, own; } VrpGlyphChar;
static const VrpGlyphChar vrp_glyph_chars[] = {
    { VRP_CH_A, VQG_A, 1 }, { VRP_CH_B, VQG_B, 1 }, { VRP_CH_X, VQG_X, 1 }, { VRP_CH_Y, VQG_Y, 1 },
    { VRP_CH_LT, VQG_LT, 1 }, { VRP_CH_RT, VQG_RT, 1 }, { VRP_CH_RG, VQG_RG, 1 }, { VRP_CH_LG, VQG_LG, 1 },
    { VRP_CH_R_UD, VQG_R_STICK_UD, 1 }, { VRP_CH_L_UD, VQG_L_STICK_UD, 1 },
    { VRP_CH_R_LR, VQG_R_STICK_LR, 1 }, { VRP_CH_L_LR, VQG_L_STICK_LR, 1 },
    { VRP_CH_MENU, VQG_MENU, 0 }, { VRP_CH_R_CLICK, VQG_R_STICK_CLICK, 0 }, { VRP_CH_L_CLICK, VQG_L_STICK_CLICK, 0 },
    { VRP_CH_R_STICK, VQG_R_STICK_ALL, 0 }, { VRP_CH_L_STICK, VQG_L_STICK_ALL, 0 },
};
#define VRP_GLYPH_CHARS ((int)(sizeof(vrp_glyph_chars) / sizeof(vrp_glyph_chars[0])))

/* The glyph a font character carries in VR, -1 for none. */
static inline int vrp_char_glyph(uint16_t c)
{
    int i;
    for (i = 0; i < VRP_GLYPH_CHARS; i++) if (vrp_glyph_chars[i].ch == c) return vrp_glyph_chars[i].glyph;
    return -1;
}

typedef struct { uint16_t *out; unsigned cap, n; int ok; } VrpBuf;

static inline void vrp_put(VrpBuf *b, uint16_t c)
{
    if (b->n + 1u < b->cap) b->out[b->n++] = c; else b->ok = 0;
}
static inline void vrp_puts(VrpBuf *b, const char *s)
{
    for (; *s; s++) vrp_put(b, (uint16_t)(unsigned char)*s);
}

/* The font character of an input (its glyph), 0 when the input is not a control (head, raise). */
static inline uint16_t vrp_input_char(VrPromptInput in)
{
    const int l = in.hand == VRP_LEFT;
    switch (in.kind) {
    case VRP_TRIGGER: return l ? VRP_CH_LT : VRP_CH_RT;
    case VRP_GRIP: return l ? VRP_CH_LG : VRP_CH_RG;
    case VRP_STICK: return l ? VRP_CH_L_STICK : VRP_CH_R_STICK;
    case VRP_STICK_SIDE: return l ? VRP_CH_L_LR : VRP_CH_R_LR;
    case VRP_STICK_UP: case VRP_STICK_DOWN: case VRP_STICK_UPDOWN: return l ? VRP_CH_L_UD : VRP_CH_R_UD;
    case VRP_STICK_CLICK: return l ? VRP_CH_L_CLICK : VRP_CH_R_CLICK;
    case VRP_BUTTON_LOW: return l ? VRP_CH_X : VRP_CH_A;
    case VRP_BUTTON_HIGH: return l ? VRP_CH_Y : VRP_CH_B;
    case VRP_MENU: return VRP_CH_MENU;
    default: return 0;
    }
}

static inline const char *vrp_direction(VrPromptKind k)
{
    return k == VRP_STICK_UP ? "UP" : k == VRP_STICK_DOWN ? "DOWN" : k == VRP_STICK_UPDOWN ? "UP/DOWN" : NULL;
}

/* "VERB glyph :", the form of the game's BUTTON_* strings, into out (UTF-16, NUL terminated, cap units with the NUL).
 * Returns the length, or -1 for an input that has none (VRP_NONE) or does not fit. */
static inline int vrp_prompt(VrPromptInput in, uint16_t *out, unsigned cap)
{
    VrpBuf b = { out, cap, 0, 1 };
    const char *dir = vrp_direction((VrPromptKind)in.kind);
    const uint16_t ch = vrp_input_char(in);
    if (!out || !cap || in.kind == VRP_NONE) return -1;
    switch (in.kind) {
    case VRP_HEAD: vrp_puts(&b, "LOOK WITH YOUR HEAD :"); break;
    case VRP_RAISE: vrp_puts(&b, "RAISE GUN TO EYE :"); break;
    default:
        vrp_puts(&b, in.hold ? "HOLD " : in.kind == VRP_TRIGGER ? "PULL " : in.kind == VRP_STICK_CLICK ? "CLICK " :
                     in.kind == VRP_STICK || in.kind == VRP_STICK_SIDE || dir ? "PUSH " : "PRESS ");
        vrp_put(&b, ch);
        if (dir) { vrp_put(&b, ' '); vrp_puts(&b, dir); }
        vrp_puts(&b, " :");
        break;
    }
    if (!b.ok) { out[0] = 0; return -1; }
    out[b.n] = 0;
    return (int)b.n;
}

/* The short form for lists and captions: ["HOLD "] glyph [" UP"], no verb or colon ("hold <glyph> to pick up" has
 * its own verb). VRP_SHORT_BARE: the glyph alone (with "HOLD " for a held button). Same return as vrp_prompt. */
enum { VRP_SHORT_BARE = 1 };
static inline int vrp_short(VrPromptInput in, uint16_t *out, unsigned cap, int flags)
{
    VrpBuf b = { out, cap, 0, 1 };
    const char *dir = vrp_direction((VrPromptKind)in.kind);
    const uint16_t ch = vrp_input_char(in);
    if (!out || !cap || in.kind == VRP_NONE) return -1;
    if (!ch) vrp_puts(&b, in.kind == VRP_HEAD ? "HEAD" : "RAISE GUN TO EYE");
    else {
        if (in.hold) vrp_puts(&b, "HOLD ");
        vrp_put(&b, ch);
        if (dir && !(flags & VRP_SHORT_BARE)) { vrp_put(&b, ' '); vrp_puts(&b, dir); }
    }
    if (!b.ok) { out[0] = 0; return -1; }
    out[b.n] = 0;
    return (int)b.n;
}

/* Expands the game's prompt tokens (U+F001 + slot .. U+F024; U+F025 is the game's "confirm", always A) in place.
 * `cap` units including the NUL. Every token on the controllers becomes vrp_prompt's text; repeats of the same
 * token in a row collapse to one ("<aim><aim> to aim sniper rifle": one raise in VR). A text after the aim token
 * that names the sniper zoom gets the zoom input (the game names Aim there). `confirm_only`: every token is the
 * confirm button, A (the hint's "to continue"). Tokens off the controllers, or that do not fit, are left for the
 * game's own formatter. Returns the number replaced. */
static inline int vrp_expand(uint16_t *buf, unsigned cap, int confirm_only, int gun_hand)
{
    enum { MAX = 512 };
    uint16_t out[MAX], text[64];
    unsigned i, n = 0, len = 0;
    int replaced = 0;
    uint16_t last = 0;
    if (!buf || cap < 2) return 0;
    while (len < cap && buf[len]) len++;
    if (len >= cap) return 0;
    for (i = 0; i < len; i++) {
        const uint16_t c = buf[i];
        int t = -1;
        if (c >= 0xF001u && c <= 0xF025u) {
            VrPromptInput in;
            int slot = c == 0xF025u ? -1 : (int)(c - 0xF000u);
            if (c == last && n > 0 && !confirm_only) { replaced++; continue; }   /* the same input again */
            if (confirm_only || slot < 0) { in.kind = VRP_BUTTON_LOW; in.hand = VRP_RIGHT; in.hold = 0; }
            else {
                static const char zoom[] = " TO SET SNIPER ZOOM";
                unsigned k = 0;
                in = vrp_slot_input(slot, gun_hand);
                if (slot == 16) {
                    while (zoom[k] && i + 1 + k < len) {
                        uint16_t ch = buf[i + 1 + k];
                        if (ch >= 'a' && ch <= 'z') ch = (uint16_t)(ch - 32);
                        if (ch != (uint16_t)(unsigned char)zoom[k]) break;
                        k++;
                    }
                    if (!zoom[k]) in = vrp_slot_input(35, gun_hand);
                }
            }
            t = vrp_prompt(in, text, sizeof(text) / sizeof(text[0]));
            if (t >= 0 && n + (unsigned)t < MAX && n + (unsigned)t + (len - i - 1) < cap) {
                memcpy(out + n, text, (size_t)t * 2);
                n += (unsigned)t;
                replaced++;
                last = c;
                continue;
            }
        }
        if (n + 1 >= MAX) return 0;
        out[n++] = c;
        last = 0;
    }
    if (!replaced || n >= cap) return 0;
    memcpy(buf, out, (size_t)n * 2);
    buf[n] = 0;
    return replaced;
}

/* UTF-16 -> UTF-8 (the game's localized strings are UTF-8). Returns bytes written without the NUL, -1 if too long. */
static inline int vrp_utf8(const uint16_t *s, char *out, unsigned cap)
{
    unsigned n = 0;
    for (; *s; s++) {
        const unsigned c = *s;
        const unsigned need = c < 0x80u ? 1u : c < 0x800u ? 2u : 3u;
        if (n + need + 1 > cap) return -1;
        if (need == 1) out[n++] = (char)c;
        else if (need == 2) { out[n++] = (char)(0xC0u | (c >> 6)); out[n++] = (char)(0x80u | (c & 0x3Fu)); }
        else { out[n++] = (char)(0xE0u | (c >> 12)); out[n++] = (char)(0x80u | ((c >> 6) & 0x3Fu)); out[n++] = (char)(0x80u | (c & 0x3Fu)); }
    }
    out[n] = 0;
    return (int)n;
}

/* ---- 3. the glyphs ------------------------------------------------------------------------------------------- */

/* The look of the game's own pad glyphs (a lit button with its letter), in gray only (one line in RGB: DXT3's four
 * colours per block keep it exact) and light, so a glyph stays legible once the text colour tints it: what the art
 * encloses is a light disc lit from the top (VRP_LIT_TOP to VRP_LIT_BOTTOM), the art's lines on it dark (the letter,
 * the rings), the art's lines outside it (a stick's arrows) light, and a dark rim just outside everything. */
#define VRP_LIT_TOP 0xF0u
#define VRP_LIT_BOTTOM 0xB4u
#define VRP_INK 0x1Cu
#define VRP_RIM 0x08u
#define VRP_RIM_A 0.75f

/* Area-filtered sample of a master channel (0..1) over the target texel [x0, x1) x [y0, y1) in master texels. */
static inline float vrp_area(int glyph, int channel, float x0, float y0, float x1, float y1)
{
    float sum = 0.0f, wsum = 0.0f;
    int x, y;
    for (y = (int)floorf(y0); y < (int)ceilf(y1); y++)
        for (x = (int)floorf(x0); x < (int)ceilf(x1); x++) {
            const float wx = fminf((float)x + 1.0f, x1) - fmaxf((float)x, x0), wy = fminf((float)y + 1.0f, y1) - fmaxf((float)y, y0);
            float v;
            if (wx <= 0.0f || wy <= 0.0f || x < 0 || y < 0 || x >= VQG_SIZE || y >= VQG_SIZE) continue;
            if (glyph < 0) v = (float)vqc_coverage(x, y) / 15.0f;
            else v = (float)(channel ? vqg_backdrop(glyph, x, y) : vqg_coverage(glyph, x, y)) / 15.0f;
            sum += v * wx * wy; wsum += wx * wy;
        }
    return wsum > 0.0f ? sum / wsum : 0.0f;
}

/* Glyph `glyph` (VQG_*) as a w x h straight-alpha RGBA8 font cell, shaded (`shade` 0: plain white art on clear, as
 * a settings page may want it). */
static inline void vrp_glyph_render(int glyph, unsigned w, unsigned h, uint8_t *rgba, int shade)
{
    const float sx = (float)VQG_SIZE / (float)w, sy = (float)VQG_SIZE / (float)h;
    float *line = (float *)malloc(sizeof(float) * w * h);
    unsigned x, y;
    if (!line) { memset(rgba, 0, (size_t)w * h * 4); return; }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            /* shrunk, the art's thin lines would fade: their coverage is lifted in proportion (none at the
             * master's size or above) */
            const float gain = fmaxf(1.0f, 0.6f * sx);
            line[y * w + x] = fminf(1.0f, gain * vrp_area(glyph, 0, (float)x * sx, (float)y * sy, (float)(x + 1) * sx, (float)(y + 1) * sy));
        }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;
            const float a = line[y * w + x];
            float r = 0.0f, ca = 0.0f, rim = 0.0f, back;
            int dx, dy;
            if (!shade) { o[0] = o[1] = o[2] = 0xFF; o[3] = (uint8_t)lroundf(a * 255.0f); continue; }
            back = vrp_area(glyph, 1, (float)x * sx, (float)y * sy, (float)(x + 1) * sx, (float)(y + 1) * sy);
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    const int xx = (int)x + dx, yy = (int)y + dy;
                    if (xx >= 0 && yy >= 0 && xx < (int)w && yy < (int)h && line[yy * w + xx] > rim) rim = line[yy * w + xx];
                }
            {
                const float lit = ((float)VRP_LIT_TOP + ((float)VRP_LIT_BOTTOM - (float)VRP_LIT_TOP) * ((float)y + 0.5f) / (float)h) / 255.0f;
                const float ink = (float)VRP_INK / 255.0f, edge = fmaxf(rim, back) * VRP_RIM_A;
                const float line_c = ink * back + lit * (1.0f - back);     /* dark on the disc, light off it */
                /* under-to-over (premultiplied): rim, disc, lines */
                r = (float)VRP_RIM / 255.0f * edge; ca = edge;
                r = lit * back + r * (1.0f - back); ca = back + ca * (1.0f - back);
                r = line_c * a + r * (1.0f - a); ca = a + ca * (1.0f - a);
            }
            if (ca > 0.0f) r /= ca;
            o[0] = o[1] = o[2] = (uint8_t)lroundf(fminf(r, 1.0f) * 255.0f);
            o[3] = (uint8_t)lroundf(fminf(ca, 1.0f) * 255.0f);
        }
    free(line);
}

/* The Controls page's diagram (the user's controller drawing) as w x h straight-alpha RGBA8: light thin lines on
 * clear, the look of the game's own pad diagram. */
static inline void vrp_diagram_render(unsigned w, unsigned h, uint8_t *rgba)
{
    const float sx = (float)VQC_W / (float)w, sy = (float)VQC_H / (float)h;
    unsigned x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;
            float sum = 0.0f, wsum = 0.0f;
            int xx, yy;
            const float x0 = (float)x * sx, x1 = (float)(x + 1) * sx, y0 = (float)y * sy, y1 = (float)(y + 1) * sy;
            for (yy = (int)floorf(y0); yy < (int)ceilf(y1); yy++)
                for (xx = (int)floorf(x0); xx < (int)ceilf(x1); xx++) {
                    const float wx = fminf((float)xx + 1.0f, x1) - fmaxf((float)xx, x0), wy = fminf((float)yy + 1.0f, y1) - fmaxf((float)yy, y0);
                    if (wx <= 0.0f || wy <= 0.0f || xx >= VQC_W || yy >= VQC_H) continue;
                    sum += (float)vqc_coverage(xx, yy) / 15.0f * wx * wy; wsum += wx * wy;
                }
            o[0] = o[1] = o[2] = 0xE6;
            o[3] = (uint8_t)lroundf(fminf((wsum > 0.0f ? sum / wsum : 0.0f) * 1.6f, 1.0f) * 255.0f);   /* thin lines stay visible shrunk */
        }
}

/* A glyph in text is this many times the font's capital height: a little taller than the letters, so a line of
 * hint text keeps its height. Its advance (texels) is the cell plus a gap: one texel before (offset +1) and two
 * after, so the art never reaches the next character. */
#define VRP_GLYPH_CAPS 1.45f
static inline float vrp_glyph_advance(float cell_w) { return cell_w + 3.0f; }

/* ---- DXT3 (the fonts' format: Xbox D3DFMT_DXT2/3, 16 bytes per 4x4 block, explicit 4-bit alpha, the colour
 * block always in its four-colour mode) ------------------------------------------------------------------------- */

static inline void vrp_565(unsigned c, unsigned rgb[3])
{
    const unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    rgb[0] = (r << 3) | (r >> 2); rgb[1] = (g << 2) | (g >> 4); rgb[2] = (b << 3) | (b >> 2);
}

static inline void vrp_dxt3_decode(const uint8_t blk[16], uint8_t px[16][4])
{
    unsigned pal[4][3], i, k;
    const unsigned c0 = blk[8] | ((unsigned)blk[9] << 8), c1 = blk[10] | ((unsigned)blk[11] << 8);
    const uint32_t idx = blk[12] | ((uint32_t)blk[13] << 8) | ((uint32_t)blk[14] << 16) | ((uint32_t)blk[15] << 24);
    vrp_565(c0, pal[0]); vrp_565(c1, pal[1]);
    for (k = 0; k < 3; k++) {
        pal[2][k] = (2u * pal[0][k] + pal[1][k] + 1u) / 3u;
        pal[3][k] = (pal[0][k] + 2u * pal[1][k] + 1u) / 3u;
    }
    for (i = 0; i < 16; i++) {
        const unsigned *c = pal[(idx >> (2 * i)) & 3u];
        px[i][0] = (uint8_t)c[0]; px[i][1] = (uint8_t)c[1]; px[i][2] = (uint8_t)c[2];
        px[i][3] = (uint8_t)(((blk[i / 2] >> ((i & 1u) * 4)) & 15u) * 17u);
    }
}

static inline unsigned vrp_to565(const uint8_t *c)
{
    return ((unsigned)(c[0] * 31 + 127) / 255u << 11) | ((unsigned)(c[1] * 63 + 127) / 255u << 5) | ((unsigned)(c[2] * 31 + 127) / 255u);
}

static inline void vrp_dxt3_encode(const uint8_t px[16][4], uint8_t blk[16])
{
    unsigned i, k, lo = 0, hi = 0, pal[4][3], c0, c1;
    int have = 0, lmin = 1 << 30, lmax = -1;
    uint32_t idx = 0;
    memset(blk, 0, 16);
    for (i = 0; i < 16; i++) {
        const unsigned a = ((unsigned)px[i][3] * 15u + 127u) / 255u;
        blk[i / 2] |= (uint8_t)(a << ((i & 1u) * 4));
        if (px[i][3]) {
            const int l = px[i][0] * 299 + px[i][1] * 587 + px[i][2] * 114;
            if (l < lmin) { lmin = l; lo = i; }
            if (l > lmax) { lmax = l; hi = i; }
            have = 1;
        }
    }
    if (!have) return;                       /* fully transparent: colour irrelevant */
    c0 = vrp_to565(px[hi]); c1 = vrp_to565(px[lo]);
    blk[8] = (uint8_t)c0; blk[9] = (uint8_t)(c0 >> 8); blk[10] = (uint8_t)c1; blk[11] = (uint8_t)(c1 >> 8);
    vrp_565(c0, pal[0]); vrp_565(c1, pal[1]);
    for (k = 0; k < 3; k++) {
        pal[2][k] = (2u * pal[0][k] + pal[1][k] + 1u) / 3u;
        pal[3][k] = (pal[0][k] + 2u * pal[1][k] + 1u) / 3u;
    }
    for (i = 0; i < 16; i++) {
        unsigned best = 0, bd = ~0u, j;
        for (j = 0; j < 4; j++) {
            const int dr = (int)px[i][0] - (int)pal[j][0], dg = (int)px[i][1] - (int)pal[j][1], db = (int)px[i][2] - (int)pal[j][2];
            const unsigned d = (unsigned)(dr * dr + dg * dg + db * db);
            if (d < bd) { bd = d; best = j; }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    blk[12] = (uint8_t)idx; blk[13] = (uint8_t)(idx >> 8); blk[14] = (uint8_t)(idx >> 16); blk[15] = (uint8_t)(idx >> 24);
}

/* Writes a w x h RGBA8 picture at texel (x, y) of a DXT3 atlas (linear block order, atlas_w x atlas_h, both multiples
 * of 4). Blocks the cell only partly covers keep their other texels. Returns 0 when the cell is outside the atlas. */
static inline int vrp_dxt3_patch(uint8_t *blocks, unsigned atlas_w, unsigned atlas_h, unsigned x, unsigned y,
                                 unsigned w, unsigned h, const uint8_t *rgba)
{
    unsigned bx, by;
    if (!blocks || !rgba || atlas_w % 4 || atlas_h % 4 || !w || !h || x + w > atlas_w || y + h > atlas_h) return 0;
    for (by = y / 4; by <= (y + h - 1) / 4; by++)
        for (bx = x / 4; bx <= (x + w - 1) / 4; bx++) {
            uint8_t *blk = blocks + ((size_t)by * (atlas_w / 4) + bx) * 16, px[16][4];
            unsigned i;
            vrp_dxt3_decode(blk, px);
            for (i = 0; i < 16; i++) {
                const unsigned tx = bx * 4 + (i & 3u), ty = by * 4 + (i >> 2);
                if (tx >= x && tx < x + w && ty >= y && ty < y + h)
                    memcpy(px[i], rgba + ((size_t)(ty - y) * w + (tx - x)) * 4, 4);
            }
            vrp_dxt3_encode(px, blk);
        }
    return 1;
}

/* DXT5 (Xbox 0x0F, the front end's texture pages): interpolated 8-bit alpha, the DXT colour block. */
static inline void vrp_dxt5_decode(const uint8_t blk[16], uint8_t px[16][4])
{
    unsigned pal[8], i;
    uint64_t bits = 0;
    uint8_t c[16];
    vrp_dxt3_decode(blk, px);                                /* colours: the same block at +8 */
    pal[0] = blk[0]; pal[1] = blk[1];
    if (pal[0] > pal[1]) for (i = 0; i < 6; i++) pal[2 + i] = ((6 - i) * pal[0] + (1 + i) * pal[1] + 3) / 7;
    else { for (i = 0; i < 4; i++) pal[2 + i] = ((4 - i) * pal[0] + (1 + i) * pal[1] + 2) / 5; pal[6] = 0; pal[7] = 255; }
    for (i = 0; i < 6; i++) bits |= (uint64_t)blk[2 + i] << (8 * i);
    for (i = 0; i < 16; i++) c[i] = (uint8_t)pal[(bits >> (3 * i)) & 7u];
    for (i = 0; i < 16; i++) px[i][3] = c[i];
}

static inline void vrp_dxt5_encode(const uint8_t px[16][4], uint8_t blk[16])
{
    unsigned i, lo = 255, hi = 0, pal[8];
    uint64_t bits = 0;
    vrp_dxt3_encode(px, blk);                                /* colour block (bytes 8..15); alpha replaced below */
    for (i = 0; i < 16; i++) { if (px[i][3] < lo) lo = px[i][3]; if (px[i][3] > hi) hi = px[i][3]; }
    if (hi == lo) { blk[0] = blk[1] = (uint8_t)hi; memset(blk + 2, 0, 6); return; }
    pal[0] = hi; pal[1] = lo;
    for (i = 0; i < 6; i++) pal[2 + i] = ((6 - i) * hi + (1 + i) * lo + 3) / 7;
    for (i = 0; i < 16; i++) {
        unsigned j, best = 0, bd = 1000;
        for (j = 0; j < 8; j++) {
            const unsigned dd = (unsigned)abs((int)px[i][3] - (int)pal[j]);
            if (dd < bd) { bd = dd; best = j; }
        }
        bits |= (uint64_t)best << (3 * i);
    }
    blk[0] = (uint8_t)hi; blk[1] = (uint8_t)lo;
    for (i = 0; i < 6; i++) blk[2 + i] = (uint8_t)(bits >> (8 * i));
}

static inline int vrp_dxt5_patch(uint8_t *blocks, unsigned atlas_w, unsigned atlas_h, unsigned x, unsigned y,
                                 unsigned w, unsigned h, const uint8_t *rgba)
{
    unsigned bx, by;
    if (!blocks || !rgba || atlas_w % 4 || atlas_h % 4 || !w || !h || x + w > atlas_w || y + h > atlas_h) return 0;
    for (by = y / 4; by <= (y + h - 1) / 4; by++)
        for (bx = x / 4; bx <= (x + w - 1) / 4; bx++) {
            uint8_t *blk = blocks + ((size_t)by * (atlas_w / 4) + bx) * 16, px[16][4];
            unsigned i;
            vrp_dxt5_decode(blk, px);
            for (i = 0; i < 16; i++) {
                const unsigned tx = bx * 4 + (i & 3u), ty = by * 4 + (i >> 2);
                if (tx >= x && tx < x + w && ty >= y && ty < y + h)
                    memcpy(px[i], rgba + ((size_t)(ty - y) * w + (tx - x)) * 4, 4);
            }
            vrp_dxt5_encode(px, blk);
        }
    return 1;
}

#endif /* VR_PROMPTS_H */
