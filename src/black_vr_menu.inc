/* BEGIN BLACK_VR_MENU
 * VR SETTINGS inside BLACK's own Options menus (BLACK_VR; port/notes/settings-panel.md). Included only by
 * recomp_manual.c, game thread only.
 *
 * The Options pages are the game's apt (Flash) movie on _level4. Under the stock buttons this adds one more button,
 * "VR SETTINGS", made from a copy of a stock button clip and joined to the page script's focus ring, so the script
 * moves the cursor onto it and selects it like its own. Selecting it makes the script load the same page again;
 * the page is then rebuilt with rows in place of the stock buttons: each row is two more copies of the stock button,
 * the setting's name and its value. The name copy is the script's button object, so up / down, the cursor, the
 * selection bar and the sounds are the game's; left / right / A on a row change it (vr_menu_activate, the shell);
 * B goes back (sections -> the list -> the stock page). The texts are the game's text fields bound to our own string
 * keys ($VRS_*), which the localized-string fetch (sub_001F3A80) answers from here (black_vr_menu_text).
 *
 * Facts about the player, its clips and the page script (addresses and layouts below) were learned from TGP482's
 * BLACK-XBERecomp (its options menu) and checked against this build's generated code; no code was copied.
 *
 * Per update, after the front-end update (sub_00124330_gen: the cursor's input at ui+44C was taken, the script ran):
 * black_vr_menu_frontend(ui). The page name is ui+3840.
 */

#define BVM_PLAYER        0x002EC988u   /* the Flash player */
#define BVM_PLAYER_READY  0x002ED408u   /* 1 once it runs */
#define BVM_INTERPRETER   0x00562C08u
#define BVM_DELETED_NAME  0x002AC7ECu   /* a deleted member's name */
#define BVM_NO_DATA       0xBAADF00Du

/* the front end (ui = [2D199C]) */
#define BVM_UI_INPUT      0x44Cu        /* this update's menu input: 1 up 2 down 3 left 4 right 5 confirm 6 back */
#define BVM_UI_INPUT_ON   0x454u
#define BVM_UI_INPUT_HELD 0x455u
#define BVM_UI_PAGE_READY 0x381Cu       /* 2: the page runs */
#define BVM_UI_PAGE_NAME  0x3840u
#define BVM_UI_ASKED      0x3884u       /* 1: a page was asked for */
#define BVM_UI_LEAVING    0x388Cu
#define BVM_UI_READY      0x390Fu
enum { BVM_IN_UP = 1, BVM_IN_DOWN, BVM_IN_LEFT, BVM_IN_RIGHT, BVM_IN_CONFIRM, BVM_IN_BACK };

/* values and clips */
#define BVM_V_FLAGS       4u            /* bit 4: defined; type in bits 25.. */
#define BVM_T_SPRITE      0xDu
#define BVM_T_TEXT        0xFu
#define BVM_C_NAME        0x08u
#define BVM_C_MATRIX      0x0Cu         /* a b c d x y */
#define BVM_C_X           0x1Cu
#define BVM_C_Y           0x20u
#define BVM_C_COLOURS     0x24u         /* 0x20 bytes */
#define BVM_C_PARENT      0x44u
#define BVM_C_DATA        0x48u
#define BVM_C_NEXT        0x50u
#define BVM_C_DEPTH       0x54u
#define BVM_C_FLAGS       0x58u
#define BVM_C_SHOWN       0x100000u
#define BVM_S_FLAGS       0x1Cu         /* a sprite's data: playing 0x2000000 */
#define BVM_S_CHILDREN    0x24u
#define BVM_F_STRING      0x1Cu         /* a text field's data: the string it shows (a name) */
#define BVM_F_FLAGS       0x6Cu

extern void sub_00100DB0(void);         /* set member: eax value (0 deletes), edx name, ecx 0; (interp, object, 1, 0, 1) */
extern void sub_000F5600(void);         /* (text): a string value */
extern void sub_00101420(void);         /* eax object path; (function, result, n, texts...) */
extern void sub_00101260(void);         /* the same with values */
extern void sub_0010F180(void);         /* (player): run the waiting frame actions */

static int bvm_trace = -1;
static int bvm_tracing(void)
{
    if (bvm_trace < 0) { const char *e = getenv("BLACKVR_MENU_TRACE"); bvm_trace = e && *e && *e != '0'; }
    return bvm_trace;
}
#define BVM_LOG(...) do { if (bvm_tracing()) { fprintf(stderr, "[BLACK_VR] menu: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint32_t bvm_r32(uint32_t va) { return black_pc_ram_span(va, 4u) ? GUEST32(va) : 0u; }
static float bvm_rf(uint32_t va) { float f = 0.0f; if (black_pc_ram_span(va, 4u)) memcpy(&f, (void *)((uintptr_t)g_xbox_mem_offset + va), 4); return f; }
static void bvm_wf(uint32_t va, float f) { if (black_pc_ram_span(va, 4u)) memcpy((void *)((uintptr_t)g_xbox_mem_offset + va), &f, 4); }
static uint32_t bvm_type(uint32_t v)
{
    const uint32_t f = bvm_r32(v + BVM_V_FLAGS);
    return v && (f & 0x10u) ? f >> 25 : 0u;
}
static int bvm_is_clip(uint32_t v) { const uint32_t t = bvm_type(v); return t >= 0xCu && t <= 0x12u; }
static const char *bvm_name_text(uint32_t name)
{
    return name && black_pc_ram_span(name + 8u, 1u) ? (const char *)((uintptr_t)g_xbox_mem_offset + name + 8u) : "";
}
static int bvm_depth(uint32_t clip) { return (int32_t)(bvm_r32(clip + BVM_C_DEPTH) << 15) >> 15; }

/* ---- guest strings and names (the guest heap, kept for the process) ---- */
typedef struct { char text[96]; uint32_t at; } BvmName;
static BvmName bvm_names[96];
static unsigned bvm_nnames;

/* A name the player can keep: +0 its handle (itself + 4), +4 uses (0x4000: never freed), +6 length, +8 room, +10
 * hash 0, +12 the text. */
static uint32_t bvm_name(const char *text)
{
    unsigned i;
    size_t n = strlen(text);
    for (i = 0; i < bvm_nnames; i++) if (!strcmp(bvm_names[i].text, text)) return bvm_names[i].at;
    if (bvm_nnames >= 96u || n >= sizeof(bvm_names[0].text)) return 0;
    {
        const uint32_t at = xbox_HeapAlloc((uint32_t)n + 16u, 16u);
        uint8_t *p;
        if (!at || !black_pc_ram_span(at, (uint32_t)n + 13u)) return 0;
        p = (uint8_t *)((uintptr_t)g_xbox_mem_offset + at);
        GUEST32(at) = at + 4u;
        p[4] = 0x00; p[5] = 0x40;
        p[6] = (uint8_t)n; p[7] = (uint8_t)(n >> 8);
        p[8] = (uint8_t)n; p[9] = (uint8_t)(n >> 8);
        p[10] = p[11] = 0;
        memcpy(p + 12, text, n + 1);
        snprintf(bvm_names[bvm_nnames].text, sizeof(bvm_names[0].text), "%s", text);
        bvm_names[bvm_nnames].at = at;
        bvm_nnames++;
        return at;
    }
}
static uint32_t bvm_text(const char *text) { const uint32_t n = bvm_name(text); return n ? n + 12u : 0u; }

/* ---- the player's objects ---- */
static uint32_t bvm_members(uint32_t v)
{
    return bvm_type(v) ? black_guest_method(v, 3u, 0u, NULL, 0u, 0x00124330u) : 0u;   /* vtable +0C */
}
static uint32_t bvm_find(uint32_t members, const char *name)
{
    const uint32_t slots = bvm_r32(members), table = bvm_r32(members + 4u);
    uint32_t i;
    if (!members || !table || slots > 0x1000u) return 0;
    for (i = 0; i < slots; i++) {
        const uint32_t key = bvm_r32(table + i * 8u), value = bvm_r32(table + i * 8u + 4u);
        if (key && key != BVM_DELETED_NAME && !_stricmp(bvm_name_text(key), name)) return bvm_type(value) ? value : 0u;
    }
    return 0;
}
static uint32_t bvm_member(uint32_t v, const char *name) { return bvm_find(bvm_members(v), name); }
/* a method of the object's class, or of a class it extends */
static uint32_t bvm_method(uint32_t object, const char *name)
{
    int up;
    for (up = 0; up < 4; up++) {
        const uint32_t m = bvm_members(object);
        uint32_t f;
        if (!m) break;
        object = bvm_r32(m + 8u);
        if ((f = bvm_member(object, name)) != 0) return f;
    }
    return 0;
}
static uint32_t bvm_level(int level)
{
    const uint32_t player = bvm_r32(BVM_PLAYER), movies = player ? bvm_r32(player + 0x18u) : 0u;
    uint32_t clip;
    int left = 64;
    for (clip = movies ? bvm_r32(movies) : 0u; clip && left; clip = bvm_r32(clip + BVM_C_NEXT), left--)
        if (bvm_depth(clip) == level) return bvm_type(clip) == 0x12u ? clip : 0u;
    return 0;
}
static uint32_t bvm_first_child(uint32_t clip)
{
    const uint32_t t = bvm_type(clip);
    uint32_t data, list, head;
    if (t != BVM_T_SPRITE && t != 0x12u) return 0;
    data = bvm_r32(clip + BVM_C_DATA);
    list = data && data != BVM_NO_DATA ? bvm_r32(data + BVM_S_CHILDREN) : 0u;
    head = list ? bvm_r32(list) : 0u;
    return head ? bvm_r32(head + BVM_C_NEXT) : 0u;
}
static uint32_t bvm_string(const char *text)
{
    BlackRegs r;
    uint32_t a[1];
    memset(&r, 0, sizeof(r));
    a[0] = bvm_text(text);
    return a[0] ? black_guest_call(sub_000F5600, &r, a, 1u, 0x00124330u) : 0u;
}
static int bvm_set(uint32_t object, const char *name, uint32_t value)
{
    BlackRegs r;
    uint32_t a[5];
    memset(&r, 0, sizeof(r));
    r.eax = value;
    r.edx = bvm_name(name);
    if (!object || !r.edx) return 0;
    a[0] = BVM_INTERPRETER; a[1] = object; a[2] = 1u; a[3] = 0u; a[4] = 1u;
    return (int)(black_guest_call(sub_00100DB0, &r, a, 5u, 0x00124330u) & 0xFFu);
}
/* path.function(texts...) through the game's host-to-script call */
static uint32_t bvm_result;              /* 64 bytes of guest heap: the call's result */
static uint32_t bvm_call(const char *path, const char *function, unsigned n, const char *const *texts)
{
    BlackRegs r;
    uint32_t a[6], rc = 0;
    unsigned i;
    if (!bvm_result && !(bvm_result = xbox_HeapAlloc(64u, 16u))) return 0;
    memset(&r, 0, sizeof(r));
    r.eax = bvm_text(path);
    GUEST32(bvm_result) = 0u;
    a[0] = bvm_text(function); a[1] = bvm_result; a[2] = n;
    for (i = 0; i < n && i < 3u; i++) a[3 + i] = bvm_text(texts[i]);
    if (r.eax && a[0]) rc = black_guest_call(sub_00101420, &r, a, 3u + n, 0x00124330u);
    BVM_LOG("call %s.%s(%u) -> %08X", path, function, n, rc);
    return rc;
}
static void bvm_call_value(const char *path, const char *function, uint32_t value)
{
    BlackRegs r;
    uint32_t a[4];
    memset(&r, 0, sizeof(r));
    r.eax = bvm_text(path);
    a[0] = bvm_text(function); a[1] = 0u; a[2] = 1u; a[3] = value;
    if (!bvm_result && !(bvm_result = xbox_HeapAlloc(64u, 16u))) return;
    a[1] = bvm_result;
    if (r.eax && a[0]) black_guest_call(sub_00101260, &r, a, 4u, 0x00124330u);
}
static void bvm_run_actions(void)
{
    BlackRegs r;
    uint32_t a[1];
    memset(&r, 0, sizeof(r));
    a[0] = bvm_r32(BVM_PLAYER);
    if (a[0]) black_guest_call(sub_0010F180, &r, a, 1u, 0x00124330u);
}

/* trace: an object's members, with their types */
static void bvm_dump(const char *what, uint32_t v)
{
    const uint32_t m = bvm_members(v), slots = bvm_r32(m), table = bvm_r32(m + 4u);
    char line[1024];
    size_t l = 0;
    uint32_t i;
    if (!bvm_tracing()) return;
    line[0] = 0;
    for (i = 0; m && table && i < slots && i < 400u && l < sizeof(line) - 48u; i++) {
        const uint32_t key = bvm_r32(table + i * 8u), val = bvm_r32(table + i * 8u + 4u);
        if (!key || key == BVM_DELETED_NAME) continue;
        l += (size_t)snprintf(line + l, sizeof(line) - l, " %.24s:%X", bvm_name_text(key), bvm_type(val));
    }
    BVM_LOG("%s %08X type %X, %u slots:%s", what, v, bvm_type(v), m ? slots : 0u, line);
}

/* the text field under a clip that shows `shows` (a string key, "$FE_OPTIONS"), or the first */
static uint32_t bvm_field(uint32_t clip, const char *shows, int depth)
{
    uint32_t c;
    int left = 256;
    for (c = bvm_first_child(clip); c && left; c = bvm_r32(c + BVM_C_NEXT), left--) {
        const uint32_t data = bvm_type(c) == BVM_T_TEXT ? bvm_r32(c + BVM_C_DATA) : 0u;
        if (data && (!shows || !strcmp(bvm_name_text(bvm_r32(data + BVM_F_STRING)), shows))) return c;
        if (depth < 4) { const uint32_t f = bvm_field(c, shows, depth + 1); if (f) return f; }
    }
    return 0;
}

/* a copy of a stock clip next to it, stopped on its frame */
static uint32_t bvm_copy(uint32_t holder, const char *from, const char *name, int depth)
{
    char d[12];
    const char *args[2];
    uint32_t clip, data;
    snprintf(d, sizeof(d), "%d", depth);
    args[0] = name; args[1] = d;
    bvm_call(from, "duplicateMovieClip", 2u, args);
    clip = bvm_member(holder, name);
    if (!clip) { bvm_run_actions(); clip = bvm_member(holder, name); BVM_LOG("copy %s after the actions: %08X", name, clip); }
    if (bvm_type(clip) != BVM_T_SPRITE) return 0;
    data = bvm_r32(clip + BVM_C_DATA);
    if (data && data != BVM_NO_DATA) GUEST32(data + BVM_S_FLAGS) &= ~0x2000000u;
    return clip;
}
static void bvm_hide(uint32_t clip) { if (bvm_is_clip(clip)) GUEST32(clip + BVM_C_FLAGS) &= ~BVM_C_SHOWN; }

/* The script's button object protocol on a clip of ours: the movieClip it lights, its frame, and the class's
 * HighLight / UnHighLight, as `like` (a stock button object) has them. */
static int bvm_make_button(uint32_t clip, uint32_t like)
{
    const uint32_t frame = bvm_member(like, "HighlightFrame"), on = bvm_method(like, "HighLight"),
                   off = bvm_method(like, "UnHighLight");
    return frame && on && off && bvm_set(clip, "movieClip", clip) && bvm_set(clip, "HighlightFrame", frame) &&
           bvm_set(clip, "HighLight", on) && bvm_set(clip, "UnHighLight", off);
}
static void bvm_link(uint32_t above, uint32_t below) { bvm_set(above, "buttonDown", below); bvm_set(below, "buttonUp", above); }

/* ---- our texts: string keys $VRS_<n> answered by the localized-string fetch ---- */
enum { BVM_TX_TITLE, BVM_TX_ENTRY, BVM_TX_NAME, BVM_SLOTS = 8, BVM_TX_VALUE = BVM_TX_NAME + BVM_SLOTS,
       BVM_TX_COUNT = BVM_TX_VALUE + BVM_SLOTS };
#define BVM_TEXT_BYTES 64u
typedef struct { uint32_t hash, text, data; } BvmText;
static BvmText bvm_tx[BVM_TX_COUNT];

static uint32_t bvm_hash_key(const char *s)
{
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++)
        c = ((c >> 8) | ((c & 0x80000000u) ? 0xFF000000u : 0u)) ^ GUEST32(0x002AC908u + (((c & 0xFFu) ^ (uint8_t)*s) & 0xFFu) * 4u);
    return c;
}

/* binds the field (under clip, showing `shows`, or the first) to our text n; initial text */
static int bvm_bind(int n, uint32_t clip, const char *shows, const char *text)
{
    BvmText *t = &bvm_tx[n];
    char key[16];
    uint32_t field;
    snprintf(key, sizeof(key), "$VRS_%d", n);
    if (!t->text) {
        t->hash = bvm_hash_key(key + 1);
        t->text = xbox_HeapAlloc(BVM_TEXT_BYTES, 16u);
        if (!t->text || !black_pc_ram_span(t->text, BVM_TEXT_BYTES)) { t->text = 0; return 0; }
        GUEST8(t->text) = 0;
    }
    if (!(field = bvm_field(clip, shows, 0))) return 0;
    t->data = bvm_r32(field + BVM_C_DATA);
    if (text) snprintf((char *)((uintptr_t)g_xbox_mem_offset + t->text), BVM_TEXT_BYTES, "%s", text);
    return bvm_set(field, "variable", bvm_string(key));
}
static void bvm_write(int n, const char *text)
{
    BvmText *t = &bvm_tx[n];
    char *shown;
    if (!t->text || !t->data) return;
    shown = (char *)((uintptr_t)g_xbox_mem_offset + t->text);
    if (!strncmp(shown, text, BVM_TEXT_BYTES - 1u)) return;
    snprintf(shown, BVM_TEXT_BYTES, "%s", text);
    if (black_pc_ram_span(t->data + BVM_F_FLAGS, 4u)) GUEST32(t->data + BVM_F_FLAGS) = (GUEST32(t->data + BVM_F_FLAGS) & ~1u) | 0x204u;
}

/* sub_001F3A80 (after the native fetch): our keys' texts. Returns 1 with *eax set. */
static int black_vr_menu_text(uint32_t hash, uint32_t *eax)
{
    unsigned i;
    for (i = 0; i < BVM_TX_COUNT; i++)
        if (bvm_tx[i].text && bvm_tx[i].hash == hash) { *eax = bvm_tx[i].text; return 1; }
    return 0;
}

/* ---- the pages ---- */
typedef struct {
    const char *page;          /* ui+3840 */
    const char *holder;        /* the clip under Main its buttons are in, NULL: Main */
    const char *button;        /* the stock button copied for ours (a path) */
    const char *under;         /* our button goes under this stock clip (in the holder) */
    float drop;                /* that far below it */
    const char *first, *last;  /* the ring's first and last button objects (on _level4) */
    float pitch, value_x, scale;
    const char *hide_prompt;   /* a prompt our button takes the place of (its string key), NULL none */
    float bar_box, bar_tick, bar_line, bar_scan;   /* the stock selection bar at rest: box x scale, right tick, line, scan x */
    const char *stock[14];     /* the stock clips the rows replace */
} BvmHost;
static const BvmHost bvm_hosts[] = {
    { "OptionsMenuXbox", "TextAnim", "_level4.Main.TextAnim.ControllerSetup_Text", "Playerprofile_Text", 25.0f,
      "controllerButton", "Playerprofile", 25.0f, 170.0f, 0.9f, NULL, 1.0f, 245.0f, 445.0f, -5.0f,
      { "ControllerSetup_Text", "SoundSetup_Text", "PictureSetup_Text", "Playerprofile_Text" } },
    /* Not on the pause menu's Options page (PauseMenuOptionXbox), by the user's choice (2026-11-12): its volume bars
     * leave no free row, and placed under MUSIC VOLUME the button covered the bar. The front end's Options is the
     * one way in. The entry that was here: holder Main, copy of _level4.Main.ToggleCrouch under MusicVolume, ring
     * toggleCrouchButton..musicVolumeButton, pitch 27, value x 245, hiding $FE_SELECT. In the harness its page and
     * GRAPHICS rows opened; a drop of about 66.5 (not 27) would put the button in the hidden SELECT prompt's slot. */
};

#define BVM_DEPTH 120           /* our clips' depths in the holder */
enum { BVM_MODE_STOCK = 0, BVM_MODE_LIST = 1, BVM_MODE_SECTION0 = 2 };   /* SECTION0 + n: the rows of section n */

typedef struct {
    const BvmHost *host;
    int mode, failed, built;
    uint32_t first;            /* our first clip: a new page instance does not have it */
    uint32_t lead;             /* the stock clip ours follow (the page slides and fades it in) */
    float lead_dx, lead_dy;    /* ours relative to it */
    uint32_t entry;
    uint32_t name[BVM_SLOTS], value[BVM_SLOTS];
    int slots, rows, top;
    uint32_t prev_focus;
    VrMenuRow row[32];
} BvmPage;
static BvmPage bvm;
static int bvm_next = BVM_MODE_STOCK;     /* the mode of the page the script loads next */
static int bvm_focus_row;                 /* the row the next page focuses (back from a section: its row) */
static int bvm_entered;                   /* vr_menu_enter was called, vr_menu_leave not yet */

static int bvm_section(void) { return bvm.mode >= BVM_MODE_SECTION0 ? bvm.mode - BVM_MODE_SECTION0 : -1; }

static void bvm_follow(void)
{
    const float lx = bvm_rf(bvm.lead + BVM_C_X), ly = bvm_rf(bvm.lead + BVM_C_Y);
    int i;
    #define BVM_PLACE(clip, dx, dy) do { if (clip) { \
        bvm_wf((clip) + BVM_C_X, lx + bvm.lead_dx + (dx)); bvm_wf((clip) + BVM_C_Y, ly + bvm.lead_dy + (dy)); \
        if (black_pc_ram_span((clip) + BVM_C_COLOURS, 0x20u) && black_pc_ram_span(bvm.lead + BVM_C_COLOURS, 0x20u)) \
            memcpy((void *)((uintptr_t)g_xbox_mem_offset + (clip) + BVM_C_COLOURS), \
                   (void *)((uintptr_t)g_xbox_mem_offset + bvm.lead + BVM_C_COLOURS), 0x20u); } } while (0)
    if (!bvm.lead) return;
    BVM_PLACE(bvm.entry, 0.0f, 0.0f);
    for (i = 0; i < bvm.slots; i++) {
        BVM_PLACE(bvm.name[i], 0.0f, bvm.host->pitch * (float)i);
        BVM_PLACE(bvm.value[i], bvm.host->value_x, bvm.host->pitch * (float)i);
    }
    #undef BVM_PLACE
}

/* the stock page with our button under its last one */
static int bvm_build_entry(uint32_t root, uint32_t holder)
{
    const BvmHost *h = bvm.host;
    uint32_t top = bvm_member(root, h->first), last = bvm_member(root, h->last);
    bvm.lead = bvm_member(holder, h->under);
    if (!top || !last || !bvm_is_clip(bvm.lead)) { BVM_LOG("%s: ring %08X..%08X, lead %s %08X", h->page, top, last, h->under, bvm.lead); return 0; }
    bvm.lead_dx = 0.0f;
    bvm.lead_dy = h->drop;
    if (!(bvm.entry = bvm_copy(holder, h->button, "VRS_ENTRY", BVM_DEPTH))) {
        BVM_LOG("copy of %s failed (front end movie path \"%.40s\")", h->button,
                (const char *)((uintptr_t)g_xbox_mem_offset + GUEST32(0x002D199Cu) + 0x38E7u));
        bvm_dump("root", root);
        bvm_dump("holder", holder);
        bvm_dump("lead", bvm.lead);
        return 0;
    }
    if (!bvm_bind(BVM_TX_ENTRY, bvm.entry, NULL, "VR SETTINGS")) { BVM_LOG("entry text not bound"); return 0; }
    bvm.first = bvm.entry;
    bvm_follow();
    bvm_run_actions();
    if (!bvm_make_button(bvm.entry, top) || !bvm_set(bvm.entry, "actionSelectPage", bvm_string(h->page))) {
        BVM_LOG("entry: not a button");
        return 0;
    }
    bvm_link(last, bvm.entry);
    bvm_link(bvm.entry, top);
    if (h->hide_prompt) {
        uint32_t c;
        for (c = bvm_field(holder, h->hide_prompt, 0); c; c = bvm_r32(c + BVM_C_PARENT))
            if (bvm_r32(c + BVM_C_PARENT) == holder) { bvm_hide(c); break; }
    }
    return 1;
}

static void bvm_texts(void)
{
    int i;
    bvm.rows = vr_menu_rows(bvm_section(), bvm.row, 32);
    bvm_write(BVM_TX_TITLE, vr_menu_title(bvm_section()));
    for (i = 0; i < bvm.slots; i++) {
        const int r = bvm.top + i;
        bvm_write(BVM_TX_NAME + i, r < bvm.rows ? bvm.row[r].name : " ");
        bvm_write(BVM_TX_VALUE + i, r < bvm.rows && bvm.row[r].value[0] ? bvm.row[r].value : " ");
        /* the value lights with its name: the label clip's matrix and colours */
        {
            const uint32_t from = bvm_first_child(bvm.name[i]), to = bvm_first_child(bvm.value[i]);
            if (from && to && black_pc_ram_span(from + BVM_C_MATRIX, 0x38u) && black_pc_ram_span(to + BVM_C_MATRIX, 0x38u))
                memcpy((void *)((uintptr_t)g_xbox_mem_offset + to + BVM_C_MATRIX),
                       (void *)((uintptr_t)g_xbox_mem_offset + from + BVM_C_MATRIX), 0x38u);
        }
    }
}


/* The selection bar (Main.ButtonSelector, at rest on its frame 10 = data frame 9) reaches past the values: its box
 * stretched, its right tick and line moved, the scan line scaled with it. `right`: the tick's x in the bar. */
static void bvm_stretch_bar(uint32_t root, float right)
{
    const BvmHost *h = bvm.host;
    const uint32_t bar = bvm_member(bvm_member(root, "Main"), "ButtonSelector");
    uint32_t data, part;
    float k;
    int left = 64;
    if (bvm_type(bar) != BVM_T_SPRITE || right <= h->bar_tick) return;
    data = bvm_r32(bar + BVM_C_DATA);
    if (!data || data == BVM_NO_DATA || bvm_r32(data + 0x18u) != 9u) return;
    k = (right + 5.0f) / (h->bar_tick + 5.0f);
    for (part = bvm_first_child(bar); part && left; part = bvm_r32(part + BVM_C_NEXT), left--) {
        const int d = bvm_depth(part);
        if (d == 1) bvm_wf(part + BVM_C_MATRIX, h->bar_box * k);
        else if (d == 5) bvm_wf(part + BVM_C_X, h->bar_line + right - h->bar_tick);
        else if (d == 7) bvm_wf(part + BVM_C_X, right);
        else if (d == 11) { bvm_wf(part + BVM_C_MATRIX, k); bvm_wf(part + BVM_C_X, -5.0f + (h->bar_scan + 5.0f) * k); }
    }
}

/* the rows in place of the stock buttons */
static int bvm_build_rows(uint32_t root, uint32_t holder, uint32_t menu)
{
    const BvmHost *h = bvm.host;
    const uint32_t like = bvm_member(root, h->first);
    const uint32_t nothing = bvm_method(like, "Select");
    const char *dot = strrchr(h->button, '.');
    int i;
    bvm.lead = bvm_member(holder, dot ? dot + 1 : h->button);
    if (!like || !nothing || !bvm_is_clip(bvm.lead)) { BVM_LOG("rows: like %08X select %08X lead %08X", like, nothing, bvm.lead); return 0; }
    bvm.lead_dx = bvm.lead_dy = 0.0f;
    bvm.rows = vr_menu_rows(bvm_section(), bvm.row, 32);
    bvm.slots = bvm.rows < BVM_SLOTS ? bvm.rows : BVM_SLOTS;
    bvm.top = 0;
    for (i = 0; i < bvm.slots; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "VRS_N%d", i);
        bvm.name[i] = bvm_copy(holder, h->button, nm, BVM_DEPTH + i);
        snprintf(nm, sizeof(nm), "VRS_V%d", i);
        bvm.value[i] = bvm_copy(holder, h->button, nm, BVM_DEPTH + BVM_SLOTS + i);
        if (!bvm.name[i] || !bvm.value[i]) { BVM_LOG("rows: copy %d failed", i); return 0; }
        bvm_wf(bvm.name[i] + BVM_C_MATRIX, h->scale); bvm_wf(bvm.name[i] + BVM_C_MATRIX + 12u, h->scale);
        bvm_wf(bvm.value[i] + BVM_C_MATRIX, h->scale); bvm_wf(bvm.value[i] + BVM_C_MATRIX + 12u, h->scale);
        if (!bvm_bind(BVM_TX_NAME + i, bvm.name[i], NULL, bvm.row[i].name) ||
            !bvm_bind(BVM_TX_VALUE + i, bvm.value[i], NULL, bvm.row[i].value[0] ? bvm.row[i].value : " ")) {
            BVM_LOG("rows: text %d not bound", i);
            return 0;
        }
    }
    bvm.first = bvm.name[0];
    bvm_follow();
    bvm_run_actions();
    for (i = 0; i < bvm.slots; i++) {
        /* a section opens with the page loaded again; anything else stays (Select does nothing) */
        const int opens = bvm.mode == BVM_MODE_LIST;
        if (!bvm_make_button(bvm.name[i], like)) { BVM_LOG("rows: %d not a button", i); return 0; }
        if (!(opens ? bvm_set(bvm.name[i], "actionSelectPage", bvm_string(h->page))
                    : bvm_set(bvm.name[i], "actionSelectFunction", nothing))) return 0;
        if (i) bvm_link(bvm.name[i - 1], bvm.name[i]);
    }
    for (i = 0; i < 14 && h->stock[i]; i++) bvm_hide(bvm_member(holder, h->stock[i]));
    bvm_bind(BVM_TX_TITLE, holder, "$FE_OPTIONS", vr_menu_title(bvm_section()));
    /* back: the same page (we decide what it shows), not the page's own back */
    bvm_set(menu, "mfuncFunctionBack", 0u);
    bvm_set(menu, "mstrPageBack", bvm_string(h->page));
    {
        const int f = bvm_focus_row > 0 && bvm_focus_row < bvm.slots ? bvm_focus_row : 0;
        bvm_call_value("_level4.MyMenu", "SetFocusButton", bvm.name[f]);
        bvm.prev_focus = bvm.name[f];
        bvm_focus_row = 0;
    }
    bvm_texts();
    return 1;
}

static void bvm_sound(void) { bvm_call("_level4.MyMenu", "PlayChangeSound", 0u, NULL); }

static void bvm_build(uint32_t root, uint32_t holder, uint32_t menu, const BvmHost *h)
{
    const int mode = bvm_next;
    memset(&bvm, 0, sizeof(bvm));
    { int i; for (i = 0; i < BVM_TX_COUNT; i++) bvm_tx[i].data = 0; }
    bvm.host = h;
    bvm.mode = mode;
    bvm_next = BVM_MODE_STOCK;
    if (mode != BVM_MODE_STOCK && !bvm_entered) { vr_menu_enter(); bvm_entered = 1; }
    bvm.failed = !(mode == BVM_MODE_STOCK ? bvm_build_entry(root, holder) : bvm_build_rows(root, holder, menu));
    bvm.built = 1;
    BVM_LOG("%s: %s%s", h->page, mode == BVM_MODE_STOCK ? "stock + VR SETTINGS" : mode == BVM_MODE_LIST ? "VR SETTINGS" :
            vr_menu_title(mode - BVM_MODE_SECTION0), bvm.failed ? " FAILED" : "");
}

static const BvmHost *bvm_find_host(const char *page)
{
    size_t i;
    for (i = 0; i < sizeof(bvm_hosts) / sizeof(bvm_hosts[0]); i++)
        if (!strcmp(page, bvm_hosts[i].page)) return &bvm_hosts[i];
    return NULL;
}

/* One row's input (left / right / confirm) */
static int bvm_want_sound;
static void bvm_row_input(int slot, uint32_t in)
{
    const int r = bvm.top + slot, sec = bvm_section();
    int res;
    if (r >= bvm.rows) return;
    if (in == BVM_IN_CONFIRM) {
        res = vr_menu_activate(sec, r, 0);
        if (res >= VR_MENU_OPEN_SECTION_0) bvm_next = BVM_MODE_SECTION0 + (res - VR_MENU_OPEN_SECTION_0);
        else if (res) bvm_want_sound = 1;
    } else if (in == BVM_IN_LEFT || in == BVM_IN_RIGHT) {
        if (vr_menu_activate(sec, r, in == BVM_IN_LEFT ? -1 : 1)) bvm_want_sound = 1;
    }
}

/* The Options page's objects this update, or 0 when it is not one of ours / not running. */
static const BvmHost *bvm_page(uint32_t ui, uint32_t *root, uint32_t *holder, uint32_t *menu, int *leaving)
{
    const char *page;
    const BvmHost *h;
    if (!ui || !black_pc_ram_span(ui, 0x3960u) || !vr_menu_native()) return NULL;
    page = (const char *)((uintptr_t)g_xbox_mem_offset + ui + BVM_UI_PAGE_NAME);
    if (bvm_tracing()) {
        static char shown[48];
        if (strncmp(shown, page, sizeof(shown) - 1)) { snprintf(shown, sizeof(shown), "%.47s", page); BVM_LOG("page %s", shown); }
    }
    h = bvm_find_host(page);
    if (!h || bvm_r32(BVM_PLAYER_READY) != 1u || !bvm_r32(BVM_PLAYER) || bvm_r32(ui + BVM_UI_PAGE_READY) != 2u) {
        if (!h && page[0]) {
            if (bvm_entered) { vr_menu_leave(); bvm_entered = 0; }   /* left the Options pages altogether */
            bvm_next = BVM_MODE_STOCK;
        }
        if (!h) memset(&bvm, 0, sizeof(bvm));
        else bvm.built = 0;
        return NULL;
    }
    *leaving = bvm_r32(ui + BVM_UI_LEAVING) || bvm_r32(ui + BVM_UI_ASKED) == 1u;
    *root = bvm_level(4);
    *holder = bvm_member(*root, "Main");
    *menu = bvm_member(*root, "MyMenu");
    if (h->holder) *holder = bvm_member(*holder, h->holder);
    if (!*holder || !*menu) return NULL;
    return h;
}
static int bvm_current(const BvmHost *h, uint32_t holder)
{
    return bvm.built && bvm.host == h && bvm_member(holder, bvm.mode == BVM_MODE_STOCK ? "VRS_ENTRY" : "VRS_N0") == bvm.first;
}

/* Before the front-end update (the script and the player run in it, and our calls into them work there): the page's
 * clips made when missing, and what the last update's input asked for. */
static void black_vr_menu_frontend_pre(uint32_t ui)
{
    uint32_t root, holder, menu;
    int leaving = 0;
    const BvmHost *h = bvm_page(ui, &root, &holder, &menu, &leaving);
    if (!h) return;
    if (!bvm_current(h, holder)) {
        if (bvm.failed && bvm.host == h && bvm.built) return;     /* failed on this page instance: leave it stock */
        if (!leaving) bvm_build(root, holder, menu, h);
        return;
    }
    if (bvm_want_sound) { bvm_want_sound = 0; bvm_sound(); }
    bvm_follow();
}

/* After the front-end update: this update's input on our rows (ui+44C, the script has moved the focus already). */
static void black_vr_menu_frontend(uint32_t ui)
{
    uint32_t root, holder, menu, focus, in;
    int leaving = 0;
    const BvmHost *h = bvm_page(ui, &root, &holder, &menu, &leaving);
    if (!h || !bvm_current(h, holder) || bvm.failed) return;
    bvm_follow();
    in = GUEST8(ui + BVM_UI_READY) && GUEST8(ui + BVM_UI_INPUT_ON) && !GUEST8(ui + BVM_UI_INPUT_HELD) ? bvm_r32(ui + BVM_UI_INPUT) : 0u;
    focus = bvm_member(menu, "mFocusButton");
    if (in) BVM_LOG("input %u, focus %08X (entry %08X, row 0 %08X)", in, focus, bvm.entry, bvm.name[0]);
    if (bvm.mode == BVM_MODE_STOCK) {
        /* the script loads the page again for our button: then with the list of sections */
        if (in == BVM_IN_CONFIRM && focus == bvm.entry) bvm_next = BVM_MODE_LIST;
        return;
    }
    if (in == BVM_IN_BACK) {
        /* the script loads the page again (mstrPageBack): a section goes back to the list, the list to the stock page */
        bvm_next = bvm.mode >= BVM_MODE_SECTION0 ? BVM_MODE_LIST : BVM_MODE_STOCK;
        bvm_focus_row = bvm.mode >= BVM_MODE_SECTION0 ? bvm.mode - BVM_MODE_SECTION0 : 0;
        if (bvm_next == BVM_MODE_STOCK && bvm_entered) { vr_menu_leave(); bvm_entered = 0; }
        return;
    }
    {
        int slot;
        for (slot = 0; slot < bvm.slots && bvm.name[slot] != focus; slot++) {}
        /* scrolling: up on the first slot / down on the last (the script had nowhere to go) */
        if (!leaving && slot < bvm.slots && focus == bvm.prev_focus) {
            if (in == BVM_IN_UP && slot == 0 && bvm.top > 0) { bvm.top--; bvm_want_sound = 1; }
            else if (in == BVM_IN_DOWN && slot == bvm.slots - 1 && bvm.top + bvm.slots < bvm.rows) { bvm.top++; bvm_want_sound = 1; }
        }
        /* a confirm on a section row has the script load the page again already (leaving): still ours to read */
        if (slot < bvm.slots) bvm_row_input(slot, in);
        bvm.prev_focus = focus;
    }
    bvm_texts();
    {
        /* the bar past the widest value of the page (Eurostile caps ~0.72 of the 15-unit text height) */
        size_t widest = 0;
        int r;
        for (r = 0; r < bvm.rows; r++) if (strlen(bvm.row[r].value) > widest) widest = strlen(bvm.row[r].value);
        if (widest > 1) bvm_stretch_bar(root, bvm.host->value_x + (2.0f + (float)widest * 15.0f * 0.72f) * bvm.host->scale + 10.0f);
    }
}
/* END BLACK_VR_MENU */
