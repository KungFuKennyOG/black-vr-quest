/* BEGIN BLACK_VR_PROMPTS
 * The game's button prompts and its Controls page name the Quest controllers (port/quest/src/vr_prompts.h for the
 * mapping, the text and the art; port/notes/prompts.md). Built only with BLACK_VR, active once the VR shell's
 * session has run (vr_active; sticky, so a headset taken off does not flip prompts back mid-hint); env
 * BLACKVR_PROMPTS=0 leaves every prompt to the game. Included by recomp_manual.c before black_hud_input.inc. Game
 * thread, except the pad filter (OHCI thread), which reads one word.
 *
 * How BLACK builds prompts (see the notes for the addresses):
 *   - text: localized strings hold tokens U+F000 + action slot; the formatter sub_00113BF0 replaces each with the
 *     localized BUTTON_<control> string ("PRESS ² :") of the control the live map [0x2F3BD4] gives the slot. In VR
 *     the tokens are expanded first (vrp_expand, from the VR layer's own action -> Quest input table), for the same
 *     four bounded callers the keyboard prompts use (BLACK_PC_INPUT 3), and in the HUD hint cache's re-format.
 *   - pickup captions ("hold %s to pick up"): built from a one-byte face glyph; black_pickup_refresh splices the
 *     Use input's glyph there (as it splices the keyboard key).
 *   - the glyphs: characters of the bitmap fonts Small.bin / Big.bin (atlas DXT3, glyph records 0x28 bytes), drawn
 *     by the HUD and by the APT front end alike. In VR one character per Quest input carries that input's glyph (the
 *     user-provided sheet, port/quest/assets/quest_glyphs.png -> vr_quest_glyphs.h; vrp_glyph_render), written
 *     once into the loaded font's atlas in guest memory; the Kelvin texture cache sees the new content and uploads it
 *     once (no per-draw cost). U+00A5 (unused by the game's text) is pointed at a free cell holding the Menu glyph.
 *   - the Controls page (the front end's CUSTOM SETUP page: a pad diagram and a list of Xbox controls with the action
 *     on each): in VR its action names carry their Quest input, its title says QUEST CONTROLS, and it is read-only
 *     (left/right and A are taken from the pad while it is shown), because the VR layer resolves its actions through
 *     the live map: which Xbox control an action is on never changes which Quest input triggers it.
 */
#include "vr_prompts.h"

static uint8_t *bnm_read_game_file(const char *xbox_path, size_t *size);   /* black_native_menu.inc */
static void bnm_get_string(uint32_t at, char *out, size_t cap);

enum { BLACK_PROMPT_VR = 2 };                 /* prompt "devices" beside PcDevice: 2 + (gun hand is the left) */
static int bvp_on = -1;                       /* -1 not yet decided, 0 off (env), 1 on */
static int bvp_trace = -1;

static int bvp_tracing(void)
{
    if (bvp_trace < 0) { const char *e = getenv("BLACKVR_PROMPT_TRACE"); bvp_trace = e && *e && *e != '0'; }
    return bvp_trace;
}

static int black_vr_prompts_on(void)
{
    if (bvp_on < 0) {
        const char *e = getenv("BLACKVR_PROMPTS");
        if (e && *e == '0') { bvp_on = 0; fprintf(stderr, "[BLACK_VR] BLACKVR_PROMPTS=0: the game's own prompts\n"); }
        else if (vr_active()) { bvp_on = 1; fprintf(stderr, "[BLACK_VR] prompts name the Quest controllers\n"); }
    }
    return bvp_on == 1;
}

static int black_vr_gun_hand(void)
{
    const VrGameConfig *g = vr_game_config();
    return g && g->aim_hand == 0 ? VRP_LEFT : VRP_RIGHT;
}

/* The prompt device: the PcDevice the PC build uses, or BLACK_PROMPT_VR (+1 left-handed) in VR. A change (the
 * handedness from a settings page) re-formats a shown hint, as a keyboard/pad change does. */
static int black_prompt_device(void)
{
    if (black_vr_prompts_on()) return BLACK_PROMPT_VR + (black_vr_gun_hand() == VRP_LEFT);
    return (int)pc_input_prompt_device();
}
static int black_prompt_is_vr(int device) { return device >= BLACK_PROMPT_VR; }
static int black_prompt_gun_hand(int device) { return device == BLACK_PROMPT_VR + 1 ? VRP_LEFT : VRP_RIGHT; }

static int black_vr_expand_prompt(uint16_t *text, unsigned capacity, int confirm_only, int device)
{
    const unsigned csr = _mm_getcsr();
    int n;
    _mm_setcsr(0x1F80u);
    n = vrp_expand(text, capacity, confirm_only, black_prompt_gun_hand(device));
    _mm_setcsr(csr);
    return n;
}

/* The pickup caption's glyph for Use (slot 13), UTF-16, no verb. 0 when there is none. */
static int black_vr_use_label(int device, uint16_t *out, unsigned cap)
{
    return vrp_short(vrp_slot_input(13, black_prompt_gun_hand(device)), out, cap, VRP_SHORT_BARE) > 0;
}

/* Harness aid (env BLACKVR_PROMPT_TEST=1, never set on the headset): the HUD hint body the game formats next
 * (caller 0x12B4EF, 200 units) is replaced by a text holding every prompt token the shipped strings use, so one
 * reachable hint ("FIND THE SHOTGUN" early in level 1) shows them all through the real HUD, fonts and formatter. */
static void black_vr_prompt_test(uint32_t caller, uint32_t buffer)
{
    static int on = -1;
    static unsigned n;
    static const char *const texts[] = {
        "~09 to fire weapon\r\n~0b to reload your weapon\r\n ~20 for melee attack\r\n~1b to throw grenade",
        "press ~0c to crouch\r\n~0d use\r\n~21 to use stored health packs\r\n~1f to use suppressor",
        "~1e to switch weapon fire modes\r\n  ~0e or  ~0f to switch weapons\r\n~10~10 to aim sniper rifle\r\n~10 to set sniper zoom",
    };
    const char *t;
    uint16_t *out;
    unsigned k = 0;
    if (on < 0) {                                   /* =N: start with text N (1..3) */
        const char *e = getenv("BLACKVR_PROMPT_TEST");
        on = e && *e && *e != '0';
        if (on) n = (unsigned)(atoi(e) > 0 ? atoi(e) - 1 : 0);
    }
    if (!on || caller != 0x0012B4EFu || !black_pc_ram_span(buffer, 400u)) return;
    out = (uint16_t *)((uintptr_t)g_xbox_mem_offset + buffer);
    for (t = texts[n++ % 3u]; *t && k < 199u; t++)
        if (*t == '~' && t[1] && t[2]) {
            const char hex[3] = { t[1], t[2], 0 };
            out[k++] = (uint16_t)(0xF000u + strtoul(hex, NULL, 16));
            t += 2;
        } else out[k++] = (uint16_t)(unsigned char)*t;
    out[k] = 0;
    fprintf(stderr, "[BLACK_VR] prompts: BLACKVR_PROMPT_TEST: hint text %u injected\n", (n - 1u) % 3u);
}

/* ---- the glyph art in the fonts --------------------------------------------------------------------------- */

typedef struct BvpFont {
    uint32_t font, data;                  /* the font object (its file image) and its atlas texels, as patched */
    uint8_t check[16];                    /* one patched block, to notice a reload at the same address */
    uint32_t check_at;
    unsigned retry;
} BvpFont;
static BvpFont bvp_fonts[2];

static float bvp_f32(uint32_t va) { float v; memcpy(&v, (const void *)((uintptr_t)g_xbox_mem_offset + va), 4); return v; }
static void bvp_put_f32(uint32_t va, float v) { memcpy((void *)((uintptr_t)g_xbox_mem_offset + va), &v, 4); }

/* The glyph record of character c (the game's own lookup: bucket [font+20+(c&7F)*4], then forward to [font+1C]). */
static uint32_t bvp_glyph(uint32_t font, uint16_t c)
{
    uint32_t g = GUEST32(font + 0x20u + (c & 0x7Fu) * 4u), end = GUEST32(font + 0x1Cu);
    unsigned k;
    for (k = 0; k < 512u && black_pc_ram_span(g, 0x28u); k++, g += 0x28u) {
        if (*(const uint16_t *)((uintptr_t)g_xbox_mem_offset + g + 0x1Cu) == c) return g;
        if (g == end) break;
    }
    return 0;
}

static int bvp_cell(uint32_t g, unsigned aw, unsigned ah, unsigned r[4])
{
    const float u = bvp_f32(g) * (float)aw, v = bvp_f32(g + 4u) * (float)ah;
    const float w = bvp_f32(g + 8u) * (float)aw, h = bvp_f32(g + 12u) * (float)ah;
    if (!(u >= 0.0f && v >= 0.0f && w >= 1.0f && h >= 1.0f && u + w <= (float)aw + 0.5f && v + h <= (float)ah + 0.5f))
        return 0;
    r[0] = (unsigned)lroundf(u); r[1] = (unsigned)lroundf(v); r[2] = (unsigned)lroundf(w); r[3] = (unsigned)lroundf(h);
    return r[0] + r[2] <= aw && r[1] + r[3] <= ah;
}

static void bvp_draw(uint8_t *blocks, unsigned aw, unsigned ah, const unsigned r[4], int glyph)
{
    uint8_t *rgba = (uint8_t *)malloc((size_t)r[2] * r[3] * 4u);
    if (!rgba) return;
    vrp_glyph_render(glyph, r[2], r[3], rgba, 1);
    vrp_dxt3_patch(blocks, aw, ah, r[0], r[1], r[2], r[3], rgba);
    free(rgba);
}

/* A free cell of size n x n (one texel of margin) in the atlas: no glyph record covers it. */
static int bvp_free_cell(uint32_t font, unsigned aw, unsigned ah, unsigned n, unsigned *fx, unsigned *fy)
{
    uint8_t *used = (uint8_t *)calloc((size_t)aw * ah, 1);
    uint32_t g, end = GUEST32(font + 0x1Cu);
    unsigned x, y, i, j, r[4];
    int found = 0;
    if (!used) return 0;
    for (g = font + 0x220u; g <= end && black_pc_ram_span(g, 0x28u); g += 0x28u)
        if (bvp_cell(g, aw, ah, r))
            for (j = r[1] ? r[1] - 1 : 0; j < r[1] + r[3] + 1 && j < ah; j++)
                for (i = r[0] ? r[0] - 1 : 0; i < r[0] + r[2] + 1 && i < aw; i++) used[(size_t)j * aw + i] = 1;
    for (y = 0; y + n + 2 <= ah && !found; y++)
        for (x = 0; x + n + 2 <= aw && !found; x++) {
            int clear = 1;
            for (j = y; j < y + n + 2 && clear; j++)
                for (i = x; i < x + n + 2 && clear; i++) if (used[(size_t)j * aw + i]) clear = 0;
            if (clear) { *fx = x + 1; *fy = y + 1; found = 1; }
        }
    free(used);
    return found;
}

/* Finds the atlas of a loaded font, checks it is the disc's (Language/Fonts/<name>.bin) and draws the art. */
static void bvp_patch_font(BvpFont *pf, uint32_t font, const char *path, const char *other)
{
    uint32_t hdr, fmt, data = 0, candidates[2];
    unsigned aw, ah, i, r[4], fx, fy;
    size_t size = 0, bytes;
    uint8_t *file, *blocks;
    if (!black_pc_ram_span(font, 0x220u)) return;
    hdr = GUEST32(font + 4u);
    if (bvp_tracing())
        fprintf(stderr, "[BLACK_VR] prompts: %s: font %08X: %08X %08X %08X %08X, texture %08X: %08X %08X %08X %08X\n", path,
                font, GUEST32(font), GUEST32(font + 4u), GUEST32(font + 8u), GUEST32(font + 12u), hdr,
                black_pc_ram_span(hdr, 16u) ? GUEST32(hdr) : 0u, black_pc_ram_span(hdr, 16u) ? GUEST32(hdr + 4u) : 0u,
                black_pc_ram_span(hdr, 16u) ? GUEST32(hdr + 8u) : 0u, black_pc_ram_span(hdr, 16u) ? GUEST32(hdr + 12u) : 0u);
    pf->font = font; pf->data = 0; pf->check_at = 0;
    if (!black_pc_ram_span(hdr, 0x14u)) return;
    fmt = GUEST32(hdr + 0xCu);
    aw = 1u << ((fmt >> 20) & 15u); ah = 1u << ((fmt >> 24) & 15u);
    if (((fmt >> 8) & 0xFFu) != 0x0Eu || aw < 16u || ah < 16u || aw > 1024u || ah > 1024u) return;   /* DXT2/3 */
    bytes = (size_t)aw * ah;                                        /* DXT3: one byte per texel */
    file = bnm_read_game_file(path, &size);
    if (!file) return;
    {
        /* the two loads' order is the game's: take the file whose atlas is this texture's size */
        uint32_t tex, ffmt;
        memcpy(&tex, file + 4, 4);
        if (size >= 0x220u && tex <= size - 0x80u) {
            memcpy(&ffmt, file + tex + 0xCu, 4);
            if (ffmt != fmt && other) {
                free(file);
                path = other;
                if (!(file = bnm_read_game_file(path, &size))) return;
            }
        }
    }
    {
        uint32_t tex;
        memcpy(&tex, file + 4, 4);
        if (size < 0x220u || tex > size || size - tex < 0x80u + bytes) { free(file); return; }
        candidates[0] = GUEST32(hdr + 4u);
        candidates[1] = candidates[0] | 0x80000000u;
        for (i = 0; i < 2u && !data; i++)
            if (black_pc_ram_span(candidates[i], (uint32_t)bytes) &&
                !memcmp((const void *)((uintptr_t)g_xbox_mem_offset + candidates[i]), file + tex + 0x80u, bytes))
                data = candidates[i];
    }
    free(file);
    if (!data) {
        fprintf(stderr, "[BLACK_VR] prompts: %s's atlas is not where its texture says (or not the disc's): its glyphs keep "
                "the Xbox art\n", path);
        pf->font = font; pf->data = 0;
        return;
    }
    blocks = (uint8_t *)((uintptr_t)g_xbox_mem_offset + data);
    {
        /* every glyph gets a square cell a little taller than the font's capitals (VRP_GLYPH_CAPS), centred on
         * them: its own cell when that is big enough (A..Y, the plates, the d-pad), else a free one (smaller when
         * the atlas has no room left) */
        const uint32_t a = bvp_glyph(font, VRP_CH_A), cap = bvp_glyph(font, 'A');
        unsigned ar[4], n, done = 0, k;
        float mid;
        if (!a || !bvp_cell(a, aw, ah, ar) || !cap) { pf->font = font; return; }
        {
            const float cap_h = bvp_f32(cap + 12u) * (float)ah;
            n = (unsigned)lroundf(cap_h * VRP_GLYPH_CAPS);
            if (n > ar[2] || n > ar[3]) n = ar[2] < ar[3] ? ar[2] : ar[3];
            if (n < 8u) n = 8u;
            mid = bvp_f32(cap + 0x14u) * (float)ah + cap_h * 0.5f;           /* the capitals' middle */
        }
        for (i = 0; i < (unsigned)VRP_GLYPH_CHARS; i++) {
            const VrpGlyphChar *c = &vrp_glyph_chars[i];
            const uint32_t g = bvp_glyph(font, c->ch);
            unsigned m = n;
            if (!g) continue;
            if (c->own && bvp_cell(g, aw, ah, r) && r[2] >= n && r[3] >= n) {
                /* the old art outside the new square goes: a filtered sample at the edge must not pick it up */
                uint8_t *clear = (uint8_t *)calloc((size_t)r[2] * r[3], 4);
                if (clear) { vrp_dxt3_patch(blocks, aw, ah, r[0], r[1], r[2], r[3], clear); free(clear); }
            } else {
                for (m = n; m >= n / 2u; m--) if (bvp_free_cell(font, aw, ah, m, &fx, &fy)) break;
                if (m < n / 2u) continue;
                r[0] = fx; r[1] = fy;
            }
            r[2] = r[3] = m;
            bvp_put_f32(g, (float)r[0] / (float)aw);
            bvp_put_f32(g + 4u, (float)r[1] / (float)ah);
            bvp_put_f32(g + 8u, (float)m / (float)aw);
            bvp_put_f32(g + 12u, (float)m / (float)ah);
            bvp_put_f32(g + 0x10u, 1.0f / (float)aw);
            bvp_put_f32(g + 0x14u, (mid - (float)m * 0.5f) / (float)ah);
            bvp_put_f32(g + 0x18u, vrp_glyph_advance((float)m) / (float)aw);
            bvp_draw(blocks, aw, ah, r, c->glyph);
            done++;
            if (bvp_tracing()) fprintf(stderr, "[BLACK_VR] prompts: %s: U+%04X <- %s at %u,%u (%u)\n", path, c->ch,
                                       vqg_names[c->glyph], r[0], r[1], m);
        }
        (void)k;
        fprintf(stderr, "[BLACK_VR] prompts: %s: %u Quest glyphs drawn\n", path, done);
    }
    {
        const uint32_t g = bvp_glyph(font, VRP_CH_A);
        pf->check_at = 0;
        if (g && bvp_cell(g, aw, ah, r)) {
            pf->check_at = data + ((r[1] / 4u + 1u) * (aw / 4u) + r[0] / 4u + 1u) * 16u;
            memcpy(pf->check, (const void *)((uintptr_t)g_xbox_mem_offset + pf->check_at), 16);
        }
    }
    pf->font = font; pf->data = data;
    fprintf(stderr, "[BLACK_VR] prompts: %s: Quest glyphs drawn into its %ux%u atlas at 0x%08X\n", path, aw, ah, data);
}

/* Game thread, from the front-end update and the HUD tick: the two fonts the game loaded ([main]+2107C Big,
 * +21080 Small; sub_0020CAC0 via the path table 0x29DB04), each patched once per load. */
static void black_vr_prompts_fonts(void)
{
    static const char *const paths[2] = { "D:\\language\\fonts\\Big.bin", "D:\\language\\fonts\\Small.bin" };
    uint32_t main_obj;
    unsigned i;
    if (!black_vr_prompts_on() || !black_pc_ram_span(0x002D45F0u, 4u)) return;
    main_obj = GUEST32(0x002D45F0u);
    if (!black_pc_ram_span(main_obj, 0x21084u)) return;
    for (i = 0; i < 2u; i++) {
        const uint32_t font = GUEST32(main_obj + 0x2107Cu + i * 4u);
        BvpFont *pf = &bvp_fonts[i];
        if (!font || !black_pc_ram_span(font, 0x220u)) continue;
        if (pf->font == font) {
            /* patched and still ours; or not patchable: look again now and then (a load still in flight) */
            if (pf->data ? !pf->check_at || !memcmp((const void *)((uintptr_t)g_xbox_mem_offset + pf->check_at), pf->check, 16)
                         : (++pf->retry & 255u) || pf->retry > 256u * 16u)
                continue;
        }
        {
            const unsigned csr = _mm_getcsr();
            _mm_setcsr(0x1F80u);
            bvp_patch_font(pf, font, paths[i], paths[1u - i]);
            _mm_setcsr(csr);
        }
    }
}

/* ---- localized strings (sub_001F3A80) --------------------------------------------------------------------- */

/* Strings that change in VR: the Controls page's action names gain their Quest input, its title and its
 * "customise" item say what the page is now; "press start" names the Menu button. Copies live in the guest heap
 * for the process (one per string and handedness), as the keyboard legends do. */
typedef struct BvpString { uint32_t hash; int slot; const char *text; uint32_t guest[2]; } BvpString;
static BvpString bvp_strings[] = {
    { 0x0326CC4Fu, -1, "quest controls", { 0, 0 } },                 /* "custom setup" (the page's title) */
    { 0x97778514u, -1, "fixed in vr", { 0, 0 } },                    /* "customise" */
    { 0x2FCCE936u, 25, "press", { 0, 0 } },                          /* "press start" */
    { 0x661A3A37u, 25, "press", { 0, 0 } },
    { 0x905E4EE0u, 25, "press", { 0, 0 } },
    /* FE_ACTION_*: the names of the Controls page's rows; slot -2 = not on the controllers in VR */
    { 0x00000000u, 1, "FE_ACTION_RUNFORWARDBACK", { 0, 0 } },
    { 0x00000000u, 3, "FE_ACTION_SIDESTEPLEFTRIGHT", { 0, 0 } },
    { 0x00000000u, 5, "FE_ACTION_TURNLEFTRIGHT", { 0, 0 } },
    { 0x00000000u, 7, "FE_ACTION_LOOKUPDOWN", { 0, 0 } },
    { 0x00000000u, 13, "FE_ACTION_ACTION", { 0, 0 } },
    { 0x00000000u, 9, "FE_ACTION_PRIMARYFIRE", { 0, 0 } },
    { 0x00000000u, 11, "FE_ACTION_RELOAD", { 0, 0 } },
    { 0x00000000u, 12, "FE_ACTION_CROUCH", { 0, 0 } },
    { 0x00000000u, 14, "FE_ACTION_CHANGEWEAPONUP", { 0, 0 } },
    { 0x00000000u, 15, "FE_ACTION_CHANGEWEAPONDOWN", { 0, 0 } },
    { 0x00000000u, 16, "FE_ACTION_AIM", { 0, 0 } },
    { 0x00000000u, 27, "FE_ACTION_GRENADE", { 0, 0 } },
    { 0x00000000u, 30, "FE_ACTION_CHANGEFIRINGMODE", { 0, 0 } },
    { 0x00000000u, 31, "FE_ACTION_TOGGLESUPRESSOR", { 0, 0 } },
    { 0x00000000u, 32, "FE_ACTION_MELEE", { 0, 0 } },
    { 0x00000000u, 33, "FE_ACTION_HEALTHPACK", { 0, 0 } },
    { 0x00000000u, -2, "FE_NEXTMUSICTRACK", { 0, 0 } },
};
static uint32_t bnm_hash(const char *s);                     /* black_native_menu.inc: the game's key CRC */

/* The text of a row name in VR: the game's name, two spaces, the Quest input (no verb, no direction: the name has
 * it). */
static int bvp_row_text(const char *name, int slot, int hand, char *out, unsigned cap)
{
    uint16_t glyph[48];
    char g8[160];
    VrPromptInput in;
    if (slot == -2) return snprintf(out, cap, "%s  n/a", name) < (int)cap;        /* not on the controllers */
    in = vrp_slot_input(slot, hand);
    if (in.kind == VRP_HEAD) return snprintf(out, cap, "%s  with head", name) < (int)cap;
    if (in.kind == VRP_RAISE) return snprintf(out, cap, "%s  raise gun to eye", name) < (int)cap;
    if (slot == 1) in.kind = VRP_STICK_UPDOWN;                 /* run forward/back: the stick's up/down glyph */
    if (slot == 3) in.kind = VRP_STICK_SIDE;                   /* sidestep: left/right */
    {
        /* on a face button whose row icon (corexbox, now the Quest letters) already is this input: no second glyph */
        static const uint16_t face[4] = { VRP_CH_A, VRP_CH_B, VRP_CH_X, VRP_CH_Y };   /* controls 16..19 */
        const int control = black_pc_input_resolver(slot);
        if (control >= 16 && control <= 19 && !in.hold && vrp_input_char(in) == face[control - 16])
            return snprintf(out, cap, "%s", name) < (int)cap;
    }
    if (vrp_short(in, glyph, 48, VRP_SHORT_BARE) <= 0 || vrp_utf8(glyph, g8, sizeof(g8)) < 0) return 0;
    return snprintf(out, cap, "%s  %s", name, g8) < (int)cap;
}

/* After the native fetch: g_eax is the UTF-8 text the game will use (0 for none). Returns the VR replacement or
 * `source`. */
static uint32_t black_vr_localized(uint32_t hash, uint32_t source)
{
    static int hashed;
    size_t i;
    if (!source || !black_vr_prompts_on()) return source;
    if (!hashed) {
        for (i = 0; i < sizeof(bvp_strings) / sizeof(bvp_strings[0]); i++)
            if (!bvp_strings[i].hash) bvp_strings[i].hash = bnm_hash(bvp_strings[i].text);
        hashed = 1;
    }
    for (i = 0; i < sizeof(bvp_strings) / sizeof(bvp_strings[0]); i++) {
        BvpString *s = &bvp_strings[i];
        const int hand = black_vr_gun_hand(), k = hand == VRP_LEFT;
        char text[192], name[96];
        size_t n;
        if (s->hash != hash) continue;
        if (s->guest[k]) return s->guest[k];
        if (s->text[0] == 'F') {                                  /* a row name: the game's own word first */
            bnm_get_string(source, name, sizeof(name));
            if (!name[0] || !bvp_row_text(name, s->slot, hand, text, sizeof(text))) return source;
        } else if (s->slot == 25) {
            uint16_t glyph[16];
            char g8[48];
            if (vrp_short(vrp_slot_input(25, hand), glyph, 16, 0) <= 0 || vrp_utf8(glyph, g8, sizeof(g8)) < 0) return source;
            snprintf(text, sizeof(text), "%s %s", s->text, g8);
        } else {
            snprintf(text, sizeof(text), "%s", s->text);
        }
        n = strlen(text) + 1u;
        s->guest[k] = xbox_HeapAlloc((uint32_t)n, 16u);
        if (!s->guest[k] || !black_pc_ram_span(s->guest[k], (uint32_t)n)) { s->guest[k] = 0; return source; }
        memcpy((void *)((uintptr_t)g_xbox_mem_offset + s->guest[k]), text, n);
        if (bvp_tracing()) fprintf(stderr, "[BLACK_VR] prompts: string %08X -> \"%s\"\n", hash, text);
        return s->guest[k];
    }
    return source;
}

/* ---- the front end's pad art: the Controls page's diagram, the menus' legend buttons ------------------------- */

/* Two APT texture pages (DXT5) hold the Xbox pad art the front end draws:
 *   export/frontend/Common.bin "commonxbox": its largest image is the pad diagram of the controller pages; it becomes
 *     the user's drawing of the two Quest controllers, side by side, fitted to the box the Xbox pad's own lines fill;
 *   export/frontend/Core.bin "corexbox": the coloured A / B / X / Y buttons of every menu's legend (CONFIRM, BACK)
 *     and of the Controls page's rows; each becomes the user's glyph of the same letter (told apart by its colour:
 *     green A, red B, blue X, yellow Y).
 * A page is found in guest memory by its bytes (the bundle stays loaded: the GPU reads it there), checked whole
 * against the disc file first, patched once per load. */
typedef struct BvpPage {
    const char *file, *bundle;
    uint32_t data, check_at, tries;
    uint8_t check[16];
} BvpPage;
static BvpPage bvp_pages[2] = { { "Common.bin", "commonxbox", 0, 0, 0, { 0 } }, { "Core.bin", "corexbox", 0, 0, 0, { 0 } } };

static uint32_t bvp_rd32(const uint8_t *d, size_t n, uint32_t at) { uint32_t v = 0; if ((size_t)at + 4u <= n) memcpy(&v, d + at, 4); return v; }

typedef struct { unsigned x, y, w, h; } BvpRect;

/* Finds a page's texels in guest memory; fills its size and up to 32 image rectangles. 0 when not (yet) loaded. */
static uint32_t bvp_find_page(const BvpPage *pg, unsigned *pw, unsigned *ph, BvpRect *img, unsigned *nimg)
{
    char path[2][64];
    size_t size = 0, bytes;
    uint8_t *d = NULL;
    uint32_t count, table, i, block = 0, tex = 0, fmt = 0, w, h, map, m, entries, found = 0, va;
    const size_t blen = strlen(pg->bundle) + 1u;
    snprintf(path[0], sizeof(path[0]), "D:\\export\\frontend\\%s", pg->file);
    snprintf(path[1], sizeof(path[1]), "D:\\export\\FrontEnd\\%s", pg->file);
    *nimg = 0;
    for (i = 0; i < 2u && !d; i++) d = bnm_read_game_file(path[i], &size);
    if (!d) return 0;
    count = bvp_rd32(d, size, 8); table = bvp_rd32(d, size, 12);
    for (i = 0; i < count && i < 64u; i++) {
        const uint32_t e = bvp_rd32(d, size, table + i * 4u), name = bvp_rd32(d, size, e);
        if (name < size && size - name >= blen && !memcmp(d + name, pg->bundle, blen)) { block = bvp_rd32(d, size, e + 4u); break; }
    }
    if (block && block < size) {
        const uint32_t tb = block + bvp_rd32(d, size, block + 8u);
        for (i = 0; i < 0x100u && tb + i + 0x90u <= size && !tex; i += 4u)
            if (bvp_rd32(d, size, tb + i) == 0x00040001u && ((bvp_rd32(d, size, tb + i + 12u) >> 8) & 0xFFu) == 0x0Fu) {
                tex = tb + i; fmt = bvp_rd32(d, size, tb + i + 12u);
            }
    }
    if (!tex) { free(d); return 0; }
    w = 1u << ((fmt >> 20) & 15u); h = 1u << ((fmt >> 24) & 15u);
    bytes = (size_t)w * h;                                   /* DXT5: a byte per texel */
    if (w > 1024u || h > 1024u || (size_t)tex + 0x80u + bytes > size) { free(d); return 0; }
    map = block + bvp_rd32(d, size, block + 0x14u);
    m = bvp_rd32(d, size, map + 8u); entries = map + bvp_rd32(d, size, map + 12u);
    for (i = 0; i < m && i < 256u && *nimg < 32u; i++) {
        const uint32_t r = map + bvp_rd32(d, size, entries + i * 16u + 8u);
        float f[4];
        unsigned k;
        if ((size_t)r + 16u > size) continue;
        memcpy(f, d + r, 16);
        for (k = 0; k < 4u; k++) if (!(f[k] >= 0.0f && f[k] <= 1.0f)) break;
        if (k < 4u) continue;
        img[*nimg].x = (unsigned)lroundf(f[0] * (float)w); img[*nimg].y = (unsigned)lroundf(f[1] * (float)h);
        img[*nimg].w = (unsigned)lroundf(f[2] * (float)w); img[*nimg].h = (unsigned)lroundf(f[3] * (float)h);
        if (img[*nimg].w && img[*nimg].h && img[*nimg].x + img[*nimg].w <= w && img[*nimg].y + img[*nimg].h <= h) ++*nimg;
    }
    /* the loaded page: the same bytes in guest memory (contiguous memory, 16-byte aligned) */
    for (va = 0x80000000u; va < 0x84000000u && !found; va += 0x10u) {
        if ((va & 0xFFFFu) == 0 && !black_pc_ram_span(va, 0x10000u)) { va += 0xFFF0u; continue; }
        if (!memcmp((const void *)((uintptr_t)g_xbox_mem_offset + va), d + tex + 0x80u, 64) &&
            black_pc_ram_span(va, (uint32_t)bytes) &&
            !memcmp((const void *)((uintptr_t)g_xbox_mem_offset + va), d + tex + 0x80u, bytes)) found = va;
    }
    free(d);
    *pw = w; *ph = h;
    return found;
}

/* The box (in the image) where an image's texels are not clear, and their mean colour. */
static int bvp_image_box(const uint8_t *blocks, unsigned pw, const BvpRect *r, BvpRect *box, float rgb[3])
{
    unsigned bx, by, k, x0 = r->w, y0 = r->h, x1 = 0, y1 = 0, n = 0, sat = 0;
    uint8_t px[16][4];
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    for (by = r->y / 4u; by < (r->y + r->h + 3u) / 4u; by++)
        for (bx = r->x / 4u; bx < (r->x + r->w + 3u) / 4u; bx++) {
            vrp_dxt5_decode(blocks + ((size_t)by * (pw / 4u) + bx) * 16u, px);
            for (k = 0; k < 16u; k++) {
                const unsigned tx = bx * 4u + (k & 3u), ty = by * 4u + (k >> 2);
                if (tx < r->x || ty < r->y || tx >= r->x + r->w || ty >= r->y + r->h || px[k][3] <= 48u) continue;
                if (tx - r->x < x0) x0 = tx - r->x;
                if (tx - r->x > x1) x1 = tx - r->x;
                if (ty - r->y < y0) y0 = ty - r->y;
                if (ty - r->y > y1) y1 = ty - r->y;
                {   /* the colour: saturated texels only (not the white letter, the gray ring or the black edge) */
                    const int hi = px[k][0] > px[k][1] ? (px[k][0] > px[k][2] ? px[k][0] : px[k][2]) : (px[k][1] > px[k][2] ? px[k][1] : px[k][2]);
                    const int lo = px[k][0] < px[k][1] ? (px[k][0] < px[k][2] ? px[k][0] : px[k][2]) : (px[k][1] < px[k][2] ? px[k][1] : px[k][2]);
                    if (hi - lo > 40) { rgb[0] += px[k][0]; rgb[1] += px[k][1]; rgb[2] += px[k][2]; sat++; }
                }
                n++;
            }
        }
    if (!n) return 0;
    if (sat) { rgb[0] /= (float)sat; rgb[1] /= (float)sat; rgb[2] /= (float)sat; }
    box->x = x0; box->y = y0; box->w = x1 - x0 + 1u; box->h = y1 - y0 + 1u;
    return 1;
}

static void bvp_put_image(uint8_t *blocks, unsigned pw, unsigned ph, const BvpRect *r, const uint8_t *rgba)
{
    vrp_dxt5_patch(blocks, pw, ph, r->x, r->y, r->w, r->h, rgba);
}

/* The diagram: the drawing's lines (their box in the VQC canvas) fitted into the Xbox pad's box, centred. */
static void bvp_diagram(uint8_t *blocks, unsigned pw, unsigned ph, const BvpRect *r)
{
    BvpRect pad;
    float rgb[3], scale;
    unsigned cx0 = VQC_W, cy0 = VQC_H, cx1 = 0, cy1 = 0, x, y, cw, ch, vw, vh;
    int ox, oy;
    uint8_t *out, *pic;
    if (!bvp_image_box(blocks, pw, r, &pad, rgb)) { pad.x = 0; pad.y = 0; pad.w = r->w; pad.h = r->h; }
    for (y = 0; y < VQC_H; y++)
        for (x = 0; x < VQC_W; x++)
            if (vqc_coverage((int)x, (int)y) > 3) {
                if (x < cx0) cx0 = x; if (x > cx1) cx1 = x; if (y < cy0) cy0 = y; if (y > cy1) cy1 = y;
            }
    if (cx1 <= cx0 || cy1 <= cy0) return;
    cw = cx1 - cx0 + 1u; ch = cy1 - cy0 + 1u;
    scale = fminf((float)pad.w / (float)cw, (float)pad.h / (float)ch);
    vw = (unsigned)lroundf((float)VQC_W * scale); vh = (unsigned)lroundf((float)VQC_H * scale);
    ox = (int)pad.x + ((int)pad.w - (int)lroundf((float)cw * scale)) / 2 - (int)lroundf((float)cx0 * scale);
    oy = (int)pad.y + ((int)pad.h - (int)lroundf((float)ch * scale)) / 2 - (int)lroundf((float)cy0 * scale);
    out = (uint8_t *)calloc((size_t)r->w * r->h, 4u);
    pic = (uint8_t *)malloc((size_t)vw * vh * 4u);
    if (out && pic && vw && vh) {
        vrp_diagram_render(vw, vh, pic);
        for (y = 0; y < vh; y++)
            for (x = 0; x < vw; x++) {
                const int tx = ox + (int)x, ty = oy + (int)y;
                if (tx >= 0 && ty >= 0 && tx < (int)r->w && ty < (int)r->h)
                    memcpy(out + ((size_t)ty * r->w + (size_t)tx) * 4u, pic + ((size_t)y * vw + x) * 4u, 4u);
            }
        bvp_put_image(blocks, pw, ph, r, out);
        if (bvp_tracing()) fprintf(stderr, "[BLACK_VR] prompts: diagram: the Xbox pad's lines %u,%u %ux%u; the pair at scale %.2f\n",
                                   pad.x, pad.y, pad.w, pad.h, scale);
    }
    free(out); free(pic);
}

/* A legend button: the user's glyph of its letter, in the button's own box. */
static int bvp_legend(uint8_t *blocks, unsigned pw, unsigned ph, const BvpRect *r)
{
    BvpRect box;
    float c[3];
    int glyph;
    uint8_t *out, *pic;
    unsigned y;
    if (!bvp_image_box(blocks, pw, r, &box, c) || box.w < 8u || box.h < 8u) return 0;
    {
        /* the hue of its saturated texels: green A, red B, blue X, yellow Y */
        const float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2]));
        float hue;
        if (mx - mn < 30.0f) return 0;                                            /* gray: not a face button */
        if (mx == c[0]) hue = 60.0f * fmodf((c[1] - c[2]) / (mx - mn) + 6.0f, 6.0f);
        else if (mx == c[1]) hue = 60.0f * ((c[2] - c[0]) / (mx - mn) + 2.0f);
        else hue = 60.0f * ((c[0] - c[1]) / (mx - mn) + 4.0f);
        glyph = hue < 25.0f || hue >= 330.0f ? VQG_B : hue < 70.0f ? VQG_Y : hue < 170.0f ? VQG_A : hue < 270.0f ? VQG_X : -1;
        if (bvp_tracing()) fprintf(stderr, "[BLACK_VR] prompts: legend image %u,%u: hue %.0f -> %s\n", r->x, r->y, hue,
                                   glyph >= 0 ? vqg_names[glyph] : "-");
        if (glyph < 0) return 0;
    }
    out = (uint8_t *)calloc((size_t)r->w * r->h, 4u);
    pic = (uint8_t *)malloc((size_t)box.w * box.w * 4u);
    if (out && pic) {
        const unsigned side = box.w < box.h ? box.w : box.h;
        vrp_glyph_render(glyph, side, side, pic, 1);
        for (y = 0; y < side; y++)
            memcpy(out + ((size_t)(box.y + (box.h - side) / 2u + y) * r->w + box.x + (box.w - side) / 2u) * 4u,
                   pic + (size_t)y * side * 4u, (size_t)side * 4u);
        bvp_put_image(blocks, pw, ph, r, out);
    }
    free(out); free(pic);
    return 1;
}

/* Game thread, when a front-end page opens: each page once per load. */
static void bvp_patch_pages(void)
{
    unsigned p;
    for (p = 0; p < 2u; p++) {
        BvpPage *pg = &bvp_pages[p];
        BvpRect img[32];
        unsigned pw, ph, n, i, done = 0, biggest = 0;
        uint8_t *blocks;
        if (pg->data && pg->check_at && !memcmp((const void *)((uintptr_t)g_xbox_mem_offset + pg->check_at), pg->check, 16)) continue;
        if (pg->tries++ >= 16u) continue;
        if (!(pg->data = bvp_find_page(pg, &pw, &ph, img, &n)) || !n) { pg->data = 0; continue; }
        blocks = (uint8_t *)((uintptr_t)g_xbox_mem_offset + pg->data);
        if (p == 0) {
            for (i = 1; i < n; i++) if (img[i].w * img[i].h > img[biggest].w * img[biggest].h) biggest = i;
            bvp_diagram(blocks, pw, ph, &img[biggest]);
            done = 1;
            /* the page's other images are the Xbox control icons of the Controls page's rows (triggers, sticks,
             * d-pad, Black / White): cleared, so a row shows one input, its Quest glyph after the action's name */
            for (i = 0; i < n; i++) {
                uint8_t *clear;
                if (i == biggest || !(clear = (uint8_t *)calloc((size_t)img[i].w * img[i].h, 4u))) continue;
                bvp_put_image(blocks, pw, ph, &img[i], clear);
                free(clear);
                done++;
            }
        } else {
            for (i = 0; i < n; i++) done += (unsigned)bvp_legend(blocks, pw, ph, &img[i]);
            biggest = 0;
        }
        pg->check_at = pg->data + ((img[biggest].y / 4u + img[biggest].h / 8u) * (pw / 4u) + img[biggest].x / 4u + img[biggest].w / 8u) * 16u;
        memcpy(pg->check, (const void *)((uintptr_t)g_xbox_mem_offset + pg->check_at), 16);
        pg->tries = 0;
        fprintf(stderr, "[BLACK_VR] prompts: %s %s at 0x%08X: %u Quest image(s)\n", pg->file, pg->bundle, pg->data, done);
    }
}

/* ---- the Controls page: read-only in VR ---------------------------------------------------------------------- */

static volatile LONG bvp_readonly;            /* the CUSTOM SETUP page is shown: the OHCI filter holds its edits */
static char bvp_page[40], bvp_page_patched[40];

/* The front-end's page (sub_00124330, ui+3840) once per update: the custom controller page is read-only in VR. */
static int bvp_report(uint8_t out[20], int pad_present)
{
    (void)pad_present;
    if (!InterlockedCompareExchange(&bvp_readonly, 0, 0)) return 0;
    out[2] &= (uint8_t)~0x0Cu;               /* d-pad left / right: the layout and the rows' actions */
    out[4] = 0;                              /* A: "customise" and the row edits; B still goes back */
    out[12] = out[13] = 0;                   /* both sticks sideways (the right stick drives the d-pad in menus) */
    out[16] = out[17] = 0;
    return 0;                                /* the rest of the report goes on to the game */
}
static const PcInputFilter bvp_filter = { NULL, NULL, NULL, bvp_report, NULL, NULL };

static int bvp_is_controls_page(const char *page)
{
    /* the APT page with the controller diagram (Options > Control Setup > Custom Control Setup) */
    return !_strnicmp(page, "ControllerCustom", 16);   /* ControllerCustomXbox */
}

static void black_vr_prompts_frontend(uint32_t ui)
{
    static int installed;
    char page[40];
    if (!black_vr_prompts_on()) return;
    if (!installed) { installed = 1; pc_input_set_filter_slot(1, &bvp_filter); }   /* slot 1: pc_menu's, unused here */
    black_vr_prompts_fonts();
    if (!black_pc_ram_span(ui, 0x3860u)) return;
    bnm_get_string(ui + 0x3840u, page, sizeof(page));
    if (strcmp(page, bvp_page)) {
        snprintf(bvp_page, sizeof(bvp_page), "%s", page);
        if (bvp_tracing()) fprintf(stderr, "[BLACK_VR] prompts: front-end page \"%s\"\n", page);
    }
    if (strcmp(page, bvp_page_patched)) {
        /* a page opened: the front end's pad art (cheap once done: one block compared per page) */
        const unsigned csr = _mm_getcsr();
        _mm_setcsr(0x1F80u);
        snprintf(bvp_page_patched, sizeof(bvp_page_patched), "%s", page);
        bvp_patch_pages();
        _mm_setcsr(csr);
    }
    InterlockedExchange(&bvp_readonly, bvp_is_controls_page(page) ? 1 : 0);
}
/* END BLACK_VR_PROMPTS */
