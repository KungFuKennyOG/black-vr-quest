# prompts: BLACK's button prompts and Controls page name the Quest controllers

Status 2026-11-12. Nothing committed. Paths are relative to /root/black; `XR` = `third_party/xboxrecomp`.
Everything below is under `BLACK_VR` and only while the VR shell's session has run (`vr_active()`, latched
once true); `BLACKVR_PROMPTS=0` (env / blackvr.ini `env` line) leaves every prompt to the game. Windows and the
flat PC build are unchanged (the non-VR preprocessor branch is token-identical to before).

## 1. How BLACK builds prompts

| What | Where | How |
|---|---|---|
| Prompt text | `MainUS.bin` strings | private-use tokens U+F000 + **action slot** (" to reload your weapon"); slots as `pc_input.c` / `vr_input_map.h` (9 fire, 11 reload, 12 crouch, 13 use, 14/15 weapon, 16 aim, 27 grenade, 30 fire mode, 31 suppressor, 32 melee, 33 health pack, 35 zoom, 25 pause, 36 back). Tokens used by the shipped strings: 9, 11 (also "to continue"), 12, 14, 15, 16, 27, 30, 31, 32, 33 |
| Token -> control -> text | `sub_00113BF0` (formatter, one UTF-16 buffer, no capacity) | control = `[[0x2F3BD4] + slot*4]` (live action map; U+F025 = control 16, A); name = `[0x2AC290 + control*4]` (`BUTTON_A` ...); text = localized string of the name's CRC (table 0x2AC908) via `sub_001F3A80`, e.g. `BUTTON_A` = "PRESS ² :", `BUTTON_RIGHT_TRIGGER` = "PULL └  ♥♥R   :", `BUTTON_LEFT_THUMB` = "CLICK ┘ ♥♥♥♥L  :", `BUTTON_START` = "Press ?" (no art). Four callers: 0x118630 (256), 0x129EB7 (120), 0x12B4EF (HUD hint body, 200), 0x12B4E0 (hint "to continue", 64) |
| Glyphs | `Language/Fonts/Small.bin`, `Big.bin` (loaded by `sub_0020CAC0` through the path table 0x29DB04: `[main]+2107C` = Big, `+21080` = Small) | ordinary characters of the bitmap fonts: ² A, ¹ B, ³ X, ° Y, ┌ Black, ┐ White, └ trigger plate, ┘ stick plate, ▲▼◄► d-pad; ♥ is a 1.6-texel spacer that puts the L / R letter over a plate (plates advance 3.1). Atlas DXT3 (0x0E), Small 128x256, Big 256x256 (A/B/X/Y only); glyph records 0x28 bytes from +0x220, bucket table +0x20, last record +0x1C. The APT front end draws its text with the same fonts (callback 0x121BC0) |
| Pickup captions | `hud+0x27F0` ("hold %s to pick up", "hold %s to swap") | the face glyph of Use's control from 0x2AB72C (one byte), not the formatter; `black_pickup_text_ready` (generated hook after 0x132AB7) finds its position |
| Menu legends, "press start" | `sub_001F3A80` strings | literal glyph characters ("press ² for info") and "press start" (3 ids) |
| Controls page | front-end page `ControllerCustomXbox` (Options > Control Setup > Custom Control Setup) | APT movie: the pad diagram and per-row Xbox icons are images of the `commonxbox` texture page in `export/frontend/Common.bin`; the rows' action names are `FE_ACTION_*` strings (tables 0x5629DC / 0x562A84 / 0x562B20 filled by `sub_00214820/960/AC0`); left/right picks the layout or the action, A on CUSTOMISE starts "press the button you wish to assign" |

## 2. What VR does

Source: `port/quest/src/vr_prompts.h` (pure, header-only, unit-tested) and `src/black_vr_prompts.inc` (game glue).

- **Action -> Quest input** (`vrp_slot_input`): the VR layer's own table (`vr_input_map.h` / `vr_gestures.h`),
  not the live map, because the VR layer turns Quest inputs into actions and only then presses whatever Xbox
  control the map gives the action. Right-handed: trigger Fire, A Reload, B Melee, stick click Use, stick up/down
  weapon (zoom while scoped), stick sideways turn; left: stick move, click Crouch, trigger Grenade, X Health pack,
  Y tap Fire mode / hold Suppressor, Menu tap Pause / hold Back; Aim = raise the gun to the eye; look = the head.
  `left_handed` (read as `vr_game_config()->aim_hand`) mirrors the hands; Menu stays left. The unit test drives
  `vr_map_actions` with each input and checks it produces exactly that action (both hands, taps vs holds).
- **Text** (`vrp_prompt`, `vrp_expand`): tokens become the game's own style, "PRESS ² :", "HOLD ° :",
  "PULL <RT> :", "CLICK <L stick click> :", "PUSH <R stick up/down> UP :", "RAISE GUN TO EYE :" (one font character per glyph, section 2 Glyphs). Repeated aim tokens collapse
  ("aim sniper rifle" = one raise); the aim token before "to set sniper zoom" gets the zoom input; "to continue"
  is A. Bounded by the caller's capacity; anything that does not fit, and slots not on the controllers (34
  cycle item), are left to the native formatter. New verbs (HOLD, PUSH, UP/DOWN, RAISE ...) are English only.
- **Hooks** (recomp_manual.c, black_hud_input.inc, black_native_menu.inc): `sub_00113BF0` expands for the four
  callers before the native formatter (as keyboard prompts do); the HUD hint cache re-formats with the VR text
  when the prompt "device" changes (2 = VR right-handed, 3 = VR left-handed, so a handedness change re-formats a
  shown hint); `black_pickup_refresh` splices Use's glyph (the R stick click) for the face glyph; `sub_001F3A80`
  returns VR copies (guest heap, one per string and handedness) of "press start" ("press" + Menu glyph), the
  Controls page's title ("quest controls"), its "customise" item ("fixed in vr") and the 17 row names
  ("grenade  <LT>", "change firing mode  HOLD <Y>", "next music track  n/a", "aim  raise gun to eye", "look up/down  with head"; a row whose action is on a face button showing the same Quest letter gets no second glyph).
- **Glyphs**: the **user-provided** Quest glyph sheet `port/quest/assets/quest_glyphs.png` (4 x 9, white line
  art; the system button is not used) and controller drawing `port/quest/assets/quest_controllers.png`.
  `port/quest/tools/make_quest_glyphs.py` (Python 3 + Pillow + NumPy) turns them into the committed, generated
  header `port/quest/src/vr_quest_glyphs.h`: 34 glyphs as 48x48 masters with two 4-bit channels (line coverage
  from luminance, keeping the anti-aliasing; the backdrop the art encloses, found by a gap-closing flood fill)
  and the diagram as 256x128 4-bit coverage. Builds never run the script; run it again after changing the PNGs.
  At run time `vrp_glyph_render` area-filters a master to the font's cell (Small 25, Big 48) and shades it like
  the game's own pad glyphs (gray only, so DXT3 keeps it exact: art lit light at the top to mid gray at the
  bottom, a dark backdrop inside, a dark rim just outside). One font character per Quest input
  (`vrp_glyph_chars`): ² A, ¹ B, ³ X, ° Y, └ LT, ┘ RT, ┌ RG, ┐ LG, ▲ R stick up/down, ▼ L stick up/down,
  ► R stick left/right, ◄ L stick left/right, and five characters no game text uses in cells of their own
  (found free from the glyph records): ¥ Menu, € R stick click, £ L stick click, » R stick all directions,
  « L stick all directions. Each record gets a square cell of A's size, offset +1 and advance cell + 2 texels,
  so text never runs into a glyph. Written once into each loaded font's atlas **in guest memory**
  (`vrp_dxt3_patch`: decode the 4x4 blocks a cell touches, replace its texels, re-encode). The atlas is found
  from the font's texture header and
  accepted only if it is byte-identical to the disc file's atlas. U+00A5 (no game text uses it) is pointed at a
  free atlas cell (found from the glyph records) holding the Menu glyph, with A's metrics. The Kelvin Vulkan
  texture cache already re-signs texture memory once per frame epoch, so the new content is uploaded once; no
  backend change and no per-draw cost. A reload of a font at the same address is noticed (one patched block is
  compared per call) and patched again.
- **Front-end pad art** (APT texture pages, DXT5, found in guest memory by their bytes and checked whole against the
  disc file, patched once per load when a front-end page opens; `vrp_dxt5_patch`): `Common.bin` "commonxbox": the
  largest image (the Xbox pad diagram) becomes the user's controller drawing, the two controllers side by side a
  small gap apart, fitted into the box the Xbox pad's own lines fill (so the page's L / R labels stay over them);
  the page's other images (the rows' Xbox trigger / stick / d-pad / Black / White icons) are cleared.
  `Core.bin` "corexbox": the coloured face buttons of every menu legend and of the rows become the user's A / B / X
  / Y glyphs, each told apart by the hue of its saturated texels (green A, red B, blue X, yellow Y).
- **Controls page read-only**: while `ControllerCustomXbox` is the front-end page, a pad filter (pc_input slot 1,
  pc_menu's, which is a stub on non-Windows hosts) takes d-pad left/right, both sticks' x and A from the report;
  B (back), up/down and Menu still work. Why: the VR layer resolves its actions through the live map, so any
  layout the page can save (the game itself refuses unassigned or duplicated actions) leaves every Quest input on
  the same action. The page therefore only explains: rows name their Quest input, the title says QUEST CONTROLS,
  CUSTOMISE reads FIXED IN VR.

## 3. Coverage

| Prompt / page | VR | Evidence (`build-ui-x86/evidence/`, local, game-derived: never commit) |
|---|---|---|
| HUD hints from tokens (fire, reload, crouch, weapon switch, aim, zoom, grenade, fire mode, suppressor, melee, health pack, "to continue") | Quest text and glyphs, glyphs 1.45 x cap height, advance = cell + 3 | `preview_hint.png` / `hint_text0_after_hud.png`; native `hint_text0_before_hud.png` if present |
| Pickup / swap captions | R stick click glyph spliced for the face glyph | **not captured** (no pickup reached in the harness); code path shared with the keyboard splice |
| Title "PRESS START" | "PRESS (Menu glyph)" | `title_before.png` / `title_after.png` |
| Font-drawn ² ¹ ³ ° anywhere | the user's A B X Y | |
| Menu legends (A CONFIRM, B BACK) | the user's A / B (corexbox page) | `main_menu_before.png` / `main_menu_after.png`, `controls_after.png` |
| Controls page | title QUEST CONTROLS, CUSTOMISE -> FIXED IN VR, the user's controller drawing, row names with their Quest input (face-button rows show it as the row icon only), Xbox trigger/stick/d-pad icons cleared; read-only (left/right/A taken; B back) | `controls_before*.png` / `controls_after*.png`, `preview_controls.png` |
| Known limits | the trailing row glyph uses the font's 16-texel glyph (one size for HUD and menus), small in the rows; the Xbox X row (default: pick up weapon) shows the Quest X icon plus the R stick click glyph (the face icons are shared with the legends, so they cannot differ per row); the d-pad "SELECT" legend arrows stay (plain arrows, not Xbox art); new words (HOLD, PUSH, UP/DOWN, RAISE GUN TO EYE, WITH HEAD, N/A) are English only | |

## 4. Verification

- Harness: `build-ui-x86` (configured as build-perf-x86), eyes 416x448, `GRX_AUDIO_WAV=`; menus by `GRX_INPUT`
  (`60:menu,110:down,125:a,150:a,175:down,180:down,185:down,195:a,225:left,235:right,245:a,260:b`), level 1 by
  the default script. Hints with tokens are not reachable early in level 1, so `BLACKVR_PROMPT_TEST=N` (harness
  aid, never set on the headset) replaces the text of the first hint the game formats ("FIND THE SHOTGUN") with
  a text holding the shipped strings' tokens; it goes through the real formatter, HUD and fonts.
  `BLACKVR_PROMPTS=0` gives the Xbox baseline with the same binary. `BLACKVR_PROMPT_TRACE=1` logs pages, strings,
  formatter calls and patches.
- Captures in `build-ui-x86/evidence/`: `title_*`, `main_menu_*`, `controls_*` (+ `_after_left_right`: layout
  unchanged in VR; `_after_A`: Xbox leaves the page, VR stays), `hint_text0_before_hud.png` /
  `hint_text0_after_hud.png` / `hint_before_after_crop.png`, `preview_*`.
- Tests: tests/xr_shell `xr-shell-prompts` (the slot -> input table drives `vr_map_actions` and must produce that
  action, both hands, taps vs holds; text; expansion and capacities; DXT3/DXT5 round trips; glyph and diagram
  rendering), /tmp/claude-0/xrt 6/6, /root/build/kvk-tests 5/5, kvk-android 2/2, vr-glue 2/2, nv2a-x86 10/10;
  build-game-android rebuilt; build-boot-a64 boots to the first GPU touch when linked with stand-ins for the
  settings agent's new shell functions (its own boot_stubs.c lacks `vr_menu_*`, so the ctest link fails until
  those are stubbed there).
- Not verified: pickup / swap captions in a capture (no pickup reached); `left_handed = 1` in the harness (unit
  tests only); the headset.

## 5. Files

| File | Change |
|---|---|
| `port/quest/src/vr_prompts.h` | new: mapping, text, glyph shading/filtering, diagram, DXT3/DXT5 writers (pure C) |
| `port/quest/assets/quest_glyphs.png`, `quest_controllers.png` | new: the user-provided art (sources) |
| `port/quest/tools/make_quest_glyphs.py` | new: PNGs -> `vr_quest_glyphs.h` (run by hand; Pillow, NumPy) |
| `port/quest/src/vr_quest_glyphs.h` | new, generated (committed): 34 glyph masters 48x48 + the 256x128 diagram; API `VQG_*`, `vqg_names`, `vqg_coverage/backdrop`, `vqc_coverage`. A settings page can include it with `vr_prompts.h` and call `vrp_glyph_render(glyph, w, h, rgba, shade)` / `vrp_diagram_render(w, h, rgba)` |
| `src/black_vr_prompts.inc` | new: VR prompt device, font patching, string overrides, read-only Controls page |
| `src/recomp_manual.c` | includes black_vr_prompts.inc before black_hud_input.inc; `BLACK_PROMPT_DEVICE()`; VR expansion in `sub_00113BF0`; VR strings in `sub_001F3A80`; font check in `sub_001314E0` (all `#ifdef BLACK_VR`) |
| `src/black_hud_input.inc` | hint device via `BLACK_PROMPT_DEVICE()`; VR expansion in `black_hint_format`; VR glyph splice in `black_pickup_refresh` |
| `src/black_native_menu.inc` | `sub_00124330`: `black_vr_prompts_frontend(ui)` first (`#ifdef BLACK_VR`) |
| `tests/xr_shell/test_prompts.c`, `CMakeLists.txt` | new test `xr-shell-prompts` (added to the foreach lists) |
