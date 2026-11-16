/* xr-shell: the Quest button prompts (vr_prompts.h): slot -> Quest input agrees with the VR layer itself
 * (vr_input_map.h), the prompt text, token expansion, the glyph art and its DXT3 writer. No game data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_config.h"
#include "vr_prompts.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void u16(const char *s, uint16_t *out)      /* ASCII, "~XX" (hex) = the token U+F000 + XX */
{
    for (; *s; s++) {
        if (*s == '~' && s[1] && s[2]) {
            *out++ = (uint16_t)(0xF000u + strtoul((char[3]){ s[1], s[2], 0 }, NULL, 16));
            s += 2;
        } else *out++ = (uint16_t)(uint8_t)*s;
    }
    *out = 0;
}
static int eq(const uint16_t *a, const char *b)     /* b: Latin-1 */
{
    for (; *b; a++, b++) if (*a != (uint16_t)(uint8_t)*b) return 0;
    return *a == 0;
}

/* Every action of the VR layer, pressed the way vrp_slot_input says, is what vr_map_actions produces. */
static uint32_t actions_of(VrPromptInput in, int left_handed, int scope_up)
{
    VrInputConfig c;
    VrInputRuntime rt;
    VrInputContext x;
    VrActionState a;
    VrTouchState t;
    uint32_t seen = 0, tick = 0;
    double held_s = in.hold ? (in.kind == VRP_MENU ? 1.3 : 0.8) : 0.2, time = 0.0;
    const double dt = 1.0 / 72.0;
    vr_input_default_config(&c);
    c.left_handed = left_handed;
    memset(&rt, 0, sizeof(rt)); rt.was_in_game = 1;
    memset(&x, 0, sizeof(x)); x.in_game = 1; x.scope_up = scope_up; x.ticks_valid = 1;
    for (time = 0.0; time < held_s + 0.7; time += dt) {
        const int down = time >= 0.05 && time < 0.05 + held_s;      /* a neutral moment first: the stick re-arms */
        memset(&t, 0, sizeof(t));
        if (down) {
            const int l = in.hand == VRP_LEFT;
            switch (in.kind) {
            case VRP_TRIGGER: if (l) t.lt = 1.0f; else t.rt = 1.0f; break;
            case VRP_BUTTON_LOW: if (l) t.x = 1; else t.a = 1; break;
            case VRP_BUTTON_HIGH: if (l) t.y = 1; else t.b = 1; break;
            case VRP_STICK_CLICK: if (l) t.lclick = 1; else t.rclick = 1; break;
            case VRP_STICK_UP: case VRP_STICK_UPDOWN: if (l) t.ly = 1.0f; else t.ry = 1.0f; break;
            case VRP_STICK_DOWN: if (l) t.ly = -1.0f; else t.ry = -1.0f; break;
            case VRP_MENU: t.menu = 1; break;
            default: break;
            }
            switch (in.kind) {
            case VRP_SWING: x.melee_swing = time < 0.05 + dt; break;   /* the swing detector fires once */
            default: break;
            }
        }
        x.game_ticks = ++tick;
        vr_map_actions(&c, &rt, &t, &x, dt, &a);
        seen |= a.held;
    }
    return seen;
}

static void test_mapping_matches_layer(void)
{
    int a, lh;
    for (lh = 0; lh < 2; lh++)
        for (a = 0; a < VRA_COUNT; a++) {
            const VrPromptInput in = vrp_slot_input(k_vra_slot[a], lh ? VRP_LEFT : VRP_RIGHT);
            uint32_t seen;
            CHECK(in.kind != VRP_NONE);
            seen = actions_of(in, lh, a == VRA_SCOPE_ZOOM);
            if (!(seen & VRA_BIT(a))) fprintf(stderr, "action %s (left_handed %d): kind %d hand %d not produced (0x%X)\n",
                                              k_vra_name[a], lh, in.kind, in.hand, seen);
            CHECK(seen & VRA_BIT(a));
            /* a tap of Y / Menu is not its hold, nor the other way round */
            if (a == VRA_SUPPRESSOR) CHECK(!(seen & VRA_BIT(VRA_FIRE_MODE)));
            if (a == VRA_FIRE_MODE) CHECK(!(seen & VRA_BIT(VRA_SUPPRESSOR)));
            if (a == VRA_PAUSE) CHECK(!(seen & VRA_BIT(VRA_BACK)));
            if (a == VRA_BACK) CHECK(!(seen & VRA_BIT(VRA_PAUSE)));
        }
    /* the hands: right-handed fire is the right trigger, Use the right click, move the left stick */
    CHECK(vrp_slot_input(9, VRP_RIGHT).kind == VRP_TRIGGER && vrp_slot_input(9, VRP_RIGHT).hand == VRP_RIGHT);
    CHECK(vrp_slot_input(27, VRP_RIGHT).hand == VRP_LEFT && vrp_slot_input(27, VRP_LEFT).hand == VRP_RIGHT);
    CHECK(vrp_slot_input(1, VRP_RIGHT).kind == VRP_STICK && vrp_slot_input(1, VRP_RIGHT).hand == VRP_LEFT);
    CHECK(vrp_slot_input(13, VRP_LEFT).kind == VRP_STICK_CLICK && vrp_slot_input(13, VRP_LEFT).hand == VRP_LEFT);
    CHECK(vrp_slot_input(30, VRP_RIGHT).hold && !vrp_slot_input(31, VRP_RIGHT).hold);
    CHECK(vrp_slot_input(36, VRP_LEFT).kind == VRP_MENU && vrp_slot_input(36, VRP_RIGHT).hand == VRP_LEFT);
    CHECK(vrp_slot_input(16, VRP_RIGHT).kind == VRP_RAISE && vrp_slot_input(7, VRP_RIGHT).kind == VRP_HEAD);
    CHECK(vrp_slot_input(34, VRP_RIGHT).kind == VRP_NONE && vrp_slot_input(0, VRP_RIGHT).kind == VRP_NONE &&
          vrp_slot_input(37, VRP_RIGHT).kind == VRP_NONE);
}

static void test_text(void)
{
    uint16_t out[64], buf[256];
    CHECK(vrp_prompt(vrp_slot_input(11, VRP_RIGHT), out, 64) == 9);
    CHECK(eq(out, "PRESS \xB2 :"));
    CHECK(vrp_prompt(vrp_slot_input(11, VRP_LEFT), out, 64) > 0 && out[6] == VRP_CH_X);
    CHECK(vrp_prompt(vrp_slot_input(30, VRP_RIGHT), out, 64) > 0 && eq(out, "HOLD \xB0 :"));
    CHECK(vrp_prompt(vrp_slot_input(30, VRP_LEFT), out, 64) > 0 && out[5] == VRP_CH_B);
    CHECK(vrp_prompt(vrp_slot_input(36, VRP_RIGHT), out, 64) > 0 && out[0] == 'H' && out[5] == VRP_CH_MENU);
    CHECK(vrp_prompt(vrp_slot_input(9, VRP_RIGHT), out, 64) == 8 && out[5] == VRP_CH_RT && eq(out + 6, " :"));
    CHECK(vrp_prompt(vrp_slot_input(9, VRP_LEFT), out, 64) == 8 && out[5] == VRP_CH_LT);
    CHECK(vrp_prompt(vrp_slot_input(12, VRP_RIGHT), out, 64) > 0 && out[0] == 'C' && out[6] == VRP_CH_L_CLICK);
    CHECK(vrp_prompt(vrp_slot_input(13, VRP_RIGHT), out, 64) > 0 && out[6] == VRP_CH_R_CLICK);
    CHECK(vrp_prompt(vrp_slot_input(14, VRP_RIGHT), out, 64) > 0 && out[5] == VRP_CH_R_UD && eq(out + 6, " UP :"));
    CHECK(vrp_prompt(vrp_slot_input(15, VRP_LEFT), out, 64) > 0 && out[5] == VRP_CH_L_UD && eq(out + 6, " DOWN :"));
    CHECK(vrp_prompt(vrp_slot_input(5, VRP_RIGHT), out, 64) > 0 && out[5] == VRP_CH_R_LR);
    CHECK(vrp_prompt(vrp_slot_input(1, VRP_RIGHT), out, 64) > 0 && out[5] == VRP_CH_L_STICK);
    CHECK(vrp_prompt(vrp_slot_input(34, VRP_RIGHT), out, 64) == -1);
    CHECK(vrp_prompt(vrp_slot_input(9, VRP_RIGHT), out, 8) == -1 && out[0] == 0);     /* does not fit */
    /* short forms */
    CHECK(vrp_short(vrp_slot_input(13, VRP_RIGHT), out, 64, VRP_SHORT_BARE) == 1 && out[0] == VRP_CH_R_CLICK);
    CHECK(vrp_short(vrp_slot_input(14, VRP_RIGHT), out, 64, 0) == 4 && out[0] == VRP_CH_R_UD && eq(out + 1, " UP"));
    CHECK(vrp_short(vrp_slot_input(14, VRP_RIGHT), out, 64, VRP_SHORT_BARE) == 1);
    CHECK(vrp_short(vrp_slot_input(30, VRP_RIGHT), out, 64, 0) == 6 && eq(out, "HOLD \xB0"));
    CHECK(vrp_short(vrp_slot_input(16, VRP_RIGHT), out, 64, 0) > 0 && eq(out, "RAISE GUN TO EYE"));
    /* every input's character carries a glyph; the characters are distinct */
    {
        int k, h, i, j;
        for (k = VRP_TRIGGER; k <= VRP_MENU; k++)
            for (h = 0; h < 2; h++) {
                VrPromptInput in = { (uint8_t)k, (uint8_t)h, 0 };
                CHECK(vrp_char_glyph(vrp_input_char(in)) >= 0);
            }
        for (i = 0; i < VRP_GLYPH_CHARS; i++) {
            CHECK(vrp_glyph_chars[i].glyph < VQG_COUNT);
            for (j = i + 1; j < VRP_GLYPH_CHARS; j++) CHECK(vrp_glyph_chars[i].ch != vrp_glyph_chars[j].ch);
        }
        CHECK(vrp_char_glyph(VRP_CH_A) == VQG_A && vrp_char_glyph(VRP_CH_MENU) == VQG_MENU && vrp_char_glyph('A') == -1);
    }
    /* expansion */
    u16("~0b to reload your weapon", buf);
    CHECK(vrp_expand(buf, 256, 0, VRP_RIGHT) == 1 && eq(buf, "PRESS \xB2 : to reload your weapon"));
    u16("\r\n\r\n~0b to continue", buf);
    CHECK(vrp_expand(buf, 256, 1, VRP_LEFT) == 1 && eq(buf, "\r\n\r\nPRESS \xB2 : to continue"));   /* confirm: A */
    u16("~10~10 to aim sniper rifle", buf);
    CHECK(vrp_expand(buf, 256, 0, VRP_RIGHT) == 2 && eq(buf, "RAISE GUN TO EYE : to aim sniper rifle"));
    u16("~10 to set sniper zoom", buf);
    CHECK(vrp_expand(buf, 256, 0, VRP_RIGHT) == 1 && buf[5] == VRP_CH_R_UD);                      /* the zoom input */
    CHECK(eq(buf + 6, " UP/DOWN : to set sniper zoom"));
    u16("  ~0e or  ~0f to switch", buf);
    CHECK(vrp_expand(buf, 256, 0, VRP_RIGHT) == 2);
    u16("~22 is not on the controllers", buf);                                                   /* slot 34 */
    CHECK(vrp_expand(buf, 256, 0, VRP_RIGHT) == 0 && buf[0] == 0xF022u);
    u16("~25 ok", buf);
    CHECK(vrp_expand(buf, 256, 0, VRP_RIGHT) == 1 && buf[6] == VRP_CH_A);                         /* U+F025: A */
    u16("no tokens", buf);
    CHECK(vrp_expand(buf, 256, 0, VRP_RIGHT) == 0 && eq(buf, "no tokens"));
    /* capacity: the result (with the NUL) never exceeds it; a token that does not fit is left alone */
    u16("~0b to reload", buf);
    CHECK(vrp_expand(buf, 20, 0, VRP_RIGHT) == 1 && eq(buf, "PRESS \xB2 : to reload"));
    u16("~0b to reload", buf);
    CHECK(vrp_expand(buf, 19, 0, VRP_RIGHT) == 0 && buf[0] == 0xF00Bu);
    u16("~09 ~1b", buf);
    CHECK(vrp_expand(buf, 12, 0, VRP_RIGHT) == 1 && buf[0] == 'P' && buf[9] == 0xF01Bu);          /* first only */
    {
        char u8[64];
        const uint16_t s[] = { 'a', 0x00B2, 0x2518, 0 };
        CHECK(vrp_utf8(s, u8, 64) == 6 && !strcmp(u8, "a\xC2\xB2\xE2\x94\x98"));
        CHECK(vrp_utf8(s, u8, 6) == -1);
    }
}

static void test_dxt3(void)
{
    uint8_t blk[16], px[16][4], back[16][4], atlas[8 * 8], cell[3 * 3 * 4], before[64];
    unsigned i;
    for (i = 0; i < 16; i++) { px[i][0] = px[i][1] = px[i][2] = (uint8_t)(i * 17); px[i][3] = (uint8_t)(i * 17); }
    vrp_dxt3_encode(px, blk);
    vrp_dxt3_decode(blk, back);
    for (i = 0; i < 16; i++) {
        CHECK(back[i][3] == px[i][3]);                                  /* 4-bit alpha is exact for multiples of 17 */
        CHECK(abs((int)back[i][0] - (int)px[i][0]) <= 48);             /* a gray ramp in 4 colours */
    }
    /* two colours: exact */
    for (i = 0; i < 16; i++) { const uint8_t v = (i & 1) ? 0xFF : 0x00; px[i][0] = px[i][1] = px[i][2] = v; px[i][3] = 255; }
    vrp_dxt3_encode(px, blk);
    vrp_dxt3_decode(blk, back);
    for (i = 0; i < 16; i++) CHECK(back[i][0] == px[i][0] && back[i][3] == 255);
    /* a patch keeps the texels outside its cell */
    memset(atlas, 0, sizeof(atlas));
    for (i = 0; i < 4; i++) {                                           /* 8x8 atlas: 4 blocks, all opaque white */
        uint8_t w[16][4];
        unsigned k;
        for (k = 0; k < 16; k++) { w[k][0] = w[k][1] = w[k][2] = w[k][3] = 255; }
        vrp_dxt3_encode(w, atlas + i * 16);
    }
    memcpy(before, atlas, 64);
    memset(cell, 0, sizeof(cell));                                      /* a transparent 3x3 at (3, 3) */
    CHECK(vrp_dxt3_patch(atlas, 8, 8, 3, 3, 3, 3, cell));
    for (i = 0; i < 4; i++) {
        unsigned k;
        vrp_dxt3_decode(atlas + i * 16, back);
        for (k = 0; k < 16; k++) {
            const unsigned x = (i & 1) * 4 + (k & 3), y = (i >> 1) * 4 + (k >> 2);
            const int inside = x >= 3 && x < 6 && y >= 3 && y < 6;
            CHECK(back[k][3] == (inside ? 0 : 255));
            if (!inside) CHECK(back[k][0] == 255);
        }
    }
    CHECK(!vrp_dxt3_patch(atlas, 8, 8, 6, 6, 3, 3, cell));             /* outside: refused */
    CHECK(!vrp_dxt3_patch(atlas, 6, 8, 0, 0, 3, 3, cell));
    (void)before;
}

static void test_art(void)
{
    static uint8_t rgba[48 * 48 * 4], diag[256 * 128 * 4];
    int g;
    unsigned i, sz;
    for (g = 0; g < VQG_COUNT; g++)
        for (sz = 25; sz <= 48; sz += 23) {
            unsigned opaque = 0, light = 0, dark = 0;
            vrp_glyph_render(g, sz, sz, rgba, 1);
            for (i = 0; i < sz * sz; i++) {
                const uint8_t *p = rgba + i * 4;
                CHECK(abs((int)p[0] - (int)p[1]) <= 1 && abs((int)p[0] - (int)p[2]) <= 1);   /* gray only */
                if (p[3] > 200) opaque++;
                if (p[3] > 160 && p[0] > 150) light++;
                if (p[3] > 150 && p[0] < 60) dark++;
            }
            CHECK(opaque > sz * sz / 10);    /* the glyph fills its cell */
            CHECK(light > sz / 3);           /* the light art (the grips are wide and thin at 25) */
            CHECK(dark > sz / 3);            /* its dark lines / rim */
        }
    /* plain: white art, alpha = coverage */
    vrp_glyph_render(VQG_A, 48, 48, rgba, 0);
    for (i = 0, sz = 0; i < 48 * 48; i++) { CHECK(rgba[i * 4] == 255); sz += rgba[i * 4 + 3] > 128; }
    CHECK(sz > 100);
    /* the diagram: lines, both controllers (left and right halves), a small gap between them */
    vrp_diagram_render(256, 128, diag);
    {
        unsigned left = 0, right = 0, middle = 0, y, x;
        for (y = 0; y < 128; y++)
            for (x = 0; x < 256; x++) {
                const unsigned a = diag[(y * 256 + x) * 4 + 3];
                if (a > 128) { if (x < 126) left++; else if (x > 130) right++; else middle++; }
            }
        CHECK(left > 300 && right > 300 && middle == 0);
    }
    CHECK(vrp_glyph_advance(16.0f) == 19.0f);
}

int main(void)
{
    test_mapping_matches_layer();
    test_text();
    test_dxt3();
    test_art();
    printf("xr-shell-prompts: %u checks passed\n", checks);
    return 0;
}
