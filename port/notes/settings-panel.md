# settings-panel: VR settings in the headset

Status 2026-11-12:
- **Primary: VR SETTINGS inside BLACK's own Options menu** (`src/black_vr_menu.inc`, `blackvr.ini settings_menu =
  native`, the default). A native apt (Flash) page made from the game's own button clips, font, selection bar,
  sounds and legend. **Verified in the headless game harness** (tests/game_render_x86 on SwiftShader, own build dir
  `/root/black/build-vrmenu-x86`): front end Options shows VR SETTINGS under the stock buttons; selecting it opens
  the VR SETTINGS page (sections), each section's rows change with left/right/A, B goes back, leaving saves
  blackvr.ini. Screenshots: `/root/black/build-vrmenu-x86/evidence/*.png` (listed below). The pause menu's Options
  page (PauseMenuOptionXbox) is coded the same way; see "Pause menu" for its status.
- **Fallback: the floating Dear ImGui panel** (xr_panel.c, `settings_menu = panel` or `both`): a world-locked quad
  layer opened with the left stick click in a menu. Unit- and integration-tested (tests/vr_glue), described from
  "The floating panel" below.
- Shared by both: the settings model and the blackvr.ini writer (vr_settings.h, pure, unit-tested), the state, live
  apply and Save (port/quest/src/vr_settings_menu.c).
- **Nothing has run on a headset.**

## VR SETTINGS in the game's Options menu

**Credit / provenance:** the technique (copies of a stock button clip joined to the page script's focus ring, the
page reloaded with rows in place of the stock buttons, text fields bound to our own string keys answered by the
localized-string fetch, the selection bar stretched) and the guest addresses/layouts it relies on were **learned
from TGP482's BLACK-XBERecomp** (its options menu; that repository has no license). **No code, comments or
structure were copied**: `black_vr_menu.inc` is written from our own reading of this build's generated code and
checked in the harness. Addresses used (USA XBE): Flash player `0x2EC988` (ready `0x2ED408`), interpreter
`0x562C08`, set member `0x100DB0`, new string `0xF5600`, host-to-script calls `0x101420` / `0x101260`, run frame
actions `0x10F180`, deleted-member name `0x2AC7EC`, string-key CRC table `0x2AC908`; clip offsets (+08 name, +0C
matrix, +1C/+20 x/y, +24 colours, +44 parent, +48 data, +50 next, +54 depth, +58 flags, shown `0x100000`);
front end ui = `[0x2D199C]`: +44C menu input (1 up .. 6 back), +454/+455 input on/held, +381C page ready (2),
+3840 page name, +3884 asked, +388C leaving, +390F ready.

### How it works (game thread)
- Hooks (all `#ifdef BLACK_VR`, one line each): `black_native_menu.inc` `sub_00124330`: `black_vr_menu_frontend_pre(ui)`
  before the generated front-end update (where calls into the player work: the clips are made there) and
  `black_vr_menu_frontend(ui)` after it (the update's input at ui+44C is final and the script has moved the
  focus); `recomp_manual.c` `sub_001F3A80`: `black_vr_menu_text()` answers our string keys right after the native
  fetch; the include before `black_native_menu.inc`. No generated function is newly overridden (no regeneration).
- **Entry:** on `OptionsMenuXbox` (front end) and `PauseMenuOptionXbox` (pause), a copy of a stock button clip
  (`duplicateMovieClip` through the host-to-script call) named `VRS_ENTRY`, placed 25 (27) units under the last
  stock button, its text field bound to `$VRS_1` ("VR SETTINGS"), made a script button object (movieClip,
  HighlightFrame, HighLight/UnHighLight copied from the first stock button) and linked into the focus ring
  (last stock -> ours -> first stock). Its action is "load this page"; when the confirm lands on it, the next
  build is the VR SETTINGS list.
- **Rows:** the same page reloaded with up to 8 rows (scrolling beyond): two copies of the stock button per row
  (name and value; the name copy is the script's button object, the value copy lights with it), the stock buttons
  hidden, the title field bound to `$VRS_0`. Up/down/cursor/selection bar/sounds are the script's own; left/right
  and A are read from ui+44C and handed to the shell (`vr_menu_activate`); the bar is stretched past the widest
  value. B: the script loads the page again (`mstrPageBack` = the page, its own back function cleared); we choose
  what it shows: a section -> the list, the list -> the stock page (and `vr_menu_leave` saves).
- **Pages:** VR SETTINGS (GRAPHICS, COMFORT, GESTURES, INFO, RECALIBRATE HEIGHT, RESTORE DEFAULTS) -> a section's
  rows. Values in capitals ("0.80X", "30 DEG", "SMOOTH", "N/A" where the headset lacks it, "(RESTART)" after a
  value that applies at the next start). INFO: game / XR loop fps, display rate, eye image size, the three busiest
  threads, GPU, build.
- **Shell API** (`vr_bridge.h`, appended): `vr_menu_native`, `vr_menu_enter`, `vr_menu_rows`, `vr_menu_activate`,
  `vr_menu_title`, `vr_menu_leave`; implemented in `vr_settings_menu.c` under one recursive lock. Live changes made
  on the game thread that need OpenXR (refresh rate, foveation level, performance levels) are queued and run by
  the XR thread (`vrsm_xr_frame`, every `xrs_frame`).
- **Legend / glyphs:** the page keeps the stock legend clips (▲▼ SELECT, (A) CONFIRM, (B) BACK), so the prompts
  stream's Quest glyphs in the game fonts show there unchanged; no text of ours draws a button glyph.
- `BLACKVR_MENU_TRACE=1` (env) logs pages, builds and inputs (`[BLACK_VR] menu: ...`).

### Harness verification (front end)
Own build dir `/root/black/build-vrmenu-x86` (tests/game_render_x86: the real game, the Quest shell, the Kelvin
Vulkan backend on SwiftShader, the fake OpenXR runtime), driven live through `<frames>/press`. Logs:
`evidence/run_frontend.log`, `evidence/run_frontend2.log` (`BLACKVR_MENU_TRACE=1`).

| Step | Result | Screenshot (`/root/black/build-vrmenu-x86/evidence/`) |
|---|---|---|
| Front end -> OPTIONS | CONTROL SETUP / SOUND SETUP / PICTURE SETUP / PROFILE / **VR SETTINGS** (ours, stock style, in the focus ring: up from the first button lands on it) | `preview_vr_settings_button.png` |
| A on VR SETTINGS | the page reloads as **VR SETTINGS**: GRAPHICS, COMFORT, GESTURES, INFO, RECALIBRATE HEIGHT, RESTORE DEFAULTS; the stock bar and legend | `vr_settings_list.png` |
| A on GRAPHICS | rows with values; right x2 on RESOLUTION SCALE -> `1.10X (RESTART)`; left on GAME RATE -> `30 (RESTART)`; FOVEATION / REFRESH RATE / PERFORMANCE `N/A` (the fake runtime and SwiftShader lack them) | `vr_settings_graphics.png` |
| B, COMFORT | TURNING right -> SMOOTH, SNAP ANGLE right -> 35 DEG, applied live (logged) | `vr_settings_comfort.png` (taken during the page's stock flash-in animation) |
| GESTURES | values as configured | `vr_settings_gestures.png` |
| INFO, down past the 8th row | the rows scroll (GAME scrolls off, BUILD comes in) | `vr_settings_info.png`, `vr_settings_info_scrolled.png` |
| B, B | back to the stock Options page; `settings: Saved. 2 settings apply at the next start` | `vr_settings_back_to_options.png` |
| the file | `<game root>/blackvr.ini`: `resolution_scale = 1.10`, `game_hz = 30`, `turn = smooth`, `snap_degrees = 35` (`evidence/saved_blackvr.ini`) | |

Not in the harness: the laser (the page is pad-driven), the Quest glyphs' final art in the legend (the prompts
stream's), sounds (no audio device on the host).

### Pause menu
Coded with the same mechanism (`PauseMenuOptionXbox`: holder Main, the stock ToggleCrouch button copied, our
button under MUSIC VOLUME, the "SELECT" legend prompt hidden in its place, the rows replacing the five stock rows,
value column 245). **Not verified**: the harness reached level 1 and could not open the pause menu. At ~1 XR frame
per second in gameplay on SwiftShader, any Menu press lasts a whole XR frame (about 1 s), which the VR controls read
as "Menu held 1 s" = Back (objectives), so the pause page never came up (`action tap: back->13` in the log). On a
headset (72+ Hz) a Menu tap pauses. If a build step fails on that page (a stock clip name differs), the page stays
stock (`BLACKVR_MENU_TRACE=1` says which step); a half-made entry could show as an extra inert "VR SETTINGS" line.

## The floating panel (fallback: `settings_menu = panel | both`)

## Use

- **Open** (only with `settings_menu = panel` or `both`): in a menu (BLACK's pause menu, the front end, loading, or the test scene with no game), click the
  **left thumbstick** (the off hand's: the right one with `left_handed = 1`). It opens on release, if the click
  lasted < 0.8 s and the other stick was not clicked meanwhile (that is the recentre chord). Not in gameplay
  (STEREO / FLAT_SCOPE with a camera in the last 3 s: the controls' own `in_game`) and not in a cutscene
  (FLAT_CINEMA).
- **Use:** a laser from the gun hand (right; left when left-handed); **trigger** or **A** (X when left-handed)
  clicks, either **thumbstick** up/down scrolls. A dot marks the hit on the panel.
- **Close:** **B**, **Menu**, or the panel's Close button. The buttons that closed it stay hidden from the game
  until released (so that B does not also "back" in the pause menu, nor Menu press START).
- While open, the game gets a neutral controller (it stays paused); the recentre and capture chords are off.

### Choice of input (for the lead)
- Left stick click in menus: the menu mapping (`vr_map_menu_pad`) passes it through as L3. BLACK's menus navigate
  with the d-pad / stick, A, B, X, Y and START: the PC input layer's menu bindings (`pc_input.c`, "Menus navigate
  with the D-pad and confirm/cancel with A and B") map nothing to L3, and the game's L3 is the gameplay Crouch
  action (slot 12). **Not verified in the running pause menu** (static reading only); the L3 still reaches the game
  while the click is held, as before. If a page turns out to use it, `vrp_input_step` (vr_panel_math.h) is where
  to change the open control.
- FLAT_MENU also covers **photo mode and split screen** (black_vr.inc): there the click opens the panel too.
- **Menu long-press (1.5 s) was skipped:** in gameplay Menu held 1 s is Back (objectives), so a 1.5 s hold
  would first open the objectives; in menus Menu is START from the first frame (the pause page would resume).

## Settings

| Tab | Setting (blackvr.ini) | Panel range | Applies |
|---|---|---|---|
| Graphics | Resolution scale (`resolution_scale`) | 0.6–1.4, step 0.05 | **restart**: the Kelvin raster size and every main target follow the eye swapchain size fixed at `vr_renderer_init` (`kvk_set_raster_size` before `kelvin_start`); recreating them live would need the backend (not this stream's) |
| | Foveation (`foveation`) | off / low / medium / high / hightop | **live** (`xrUpdateSwapchainFB` with a new level profile on the eye swapchain(s), under the queue lock) when the eye swapchain was created with the foveation flag; restart otherwise (foveation off at start); N/A without XR_FB_foveation_vulkan + VK_EXT_fragment_density_map (SwiftShader) |
| | Refresh rate (`refresh_hz`) | auto / 72 / 80 / 90 / 120 | **live** (`xrRequestDisplayRefreshRateFB`, nearest offered rate; auto follows the game rate the process started with) |
| | Game rate (`game_hz`) | 30 / 60 | **restart** (the game's environment) |
| | Performance (`env BLACKVR_PERF`) | sustained_high / boost / boost_cpu / boost_gpu | **live** (`xrPerfSettingsSetPerformanceLevelEXT`, `xrs_perf_apply`: the lead's session-start code, now a function) |
| | Effects debug tint (`env BLACKVR_SPRITE_DEBUG`) | on / off | **restart**: kvk_backend.c reads the variable once (static) |
| Comfort | Turning (`turn`), Snap angle (`snap_degrees`, 10–90 / 5), Smooth turn speed (`smooth_degrees_per_second`, 30–360 / 10), Move direction (`locomotion`), Left-handed (`left_handed`), World scale (`world_scale`, 0.5–2.0 / 0.05) | | **live** (read every XR frame by `vrb_set_input` / `vrb_on_xr_frame`; the game thread reads `world_scale` and the gun hand through `vr_game_config`) |
| Gestures | Swing to melee + speed (1–10 m/s), Duck to crouch + depth (0.5–0.9), Raise to scope + distance (0.03–0.5 m), Two-handed aim | | **live** (`vr_gestures.h` reads the config every update) |
| | Recalibrate height | button | `vr_request_recentre`: origin + standing height, as the chord |

No height-offset setting exists in the shell, so none is shown. `hightop` (new `foveation` value, also in the ini
parser) is level HIGH with the profile's `verticalOffset` = 10° (`VR_FOVEATION_HIGH_TOP_OFFSET_DEG`): the VrApi
"HighTop" level has no OpenXR enum; the offset value and its sign (positive = up) are **unverified on a Quest**.

The panel's slider ranges are narrower than the parser's for some keys (e.g. `resolution_scale` 0.3–2.0 in the
file): a value outside is shown and kept until the slider is moved, then clamped to the panel's range and grid.

**Badges:** LIVE / RESTART / N/A per row; a restart setting changed from what the process started with shows an
amber "RESTART to apply".

**Buttons:** Save; Cancel = back to the values at open (live ones re-applied); Defaults = every panel setting to
`vr_config_defaults` (live ones applied, Save to keep); Close keeps live changes for this run, unsaved (the footer
says "Unsaved changes").

### Save (`vrs_ini_write`, vr_settings.h)
- File: `<game root>/blackvr.ini` (the file `load_config` read; the app's files dir when no game folder was found).
- Only settings **changed in the panel** (edit vs what the panel thinks is stored: the startup config, then each
  save) are written; any other setting keeps what the file says **now** (re-read at Save), so an edit made on a PC
  while the game ran survives.
- A changed setting is rewritten on every line that sets it: the key, its spacing and a trailing `;`/`#` comment
  stay, the comment keeps its column when the value gets shorter. Not found: appended (`key = value`, or
  `env NAME=VALUE`). Comments, blank lines, unknown keys, other env lines and each line's ending (LF / CRLF) are
  copied byte for byte; a commented-out `# env ...` line is not touched.
- Written to `blackvr.ini.tmp`, fsync'd, renamed over the file.

## Design

| File | What |
|---|---|
| `port/quest/src/vr_settings.h` | pure C: the settings table (key, label, range, step, live class), get/set/clamp/format, `vrs_apply_live`, `vrs_pending_restart`, `vrs_reset_defaults`, `vrs_ini_write` |
| `port/quest/src/vr_panel_math.h` | pure C: `vrp_place` (1.2 m ahead of the head's horizontal facing, 0.1 m below the eyes, upright, 0.8 × 0.6 m), `vrp_hit` (laser ray → panel pixel), `vrp_laser_pose` (the beam as a 4 mm quad along the ray, face turned to the head), `vrp_input_step` (open click, close buttons, trigger hysteresis 0.55/0.35, scroll, what the game may see) |
| `port/quest/src/xr_panel.c` | the shell side: swapchains (panel 1024×768, laser 8×64), Vulkan (own command pool, 2 command buffers + fences, a third for the laser image), layers, live apply, Save, Info sampling |
| `port/quest/src/vr_panel_ui.cpp`, `vr_panel.h` | Dear ImGui UI (theme, tabs, rows, footer) and its Vulkan renderer (own pipeline, render pass, font texture via `ImGuiBackendFlags_RendererHasTextures`, per-slot vertex/index/staging buffers) |
| `port/quest/shaders/panel.vert`, `.frag` | ImGui vertex/fragment shaders |
| `port/quest/cmake/settings_panel.cmake`, `bin2c.cmake` | `blackvr_imgui` static library, generated shader/font includes |
| `third_party/imgui/` | Dear ImGui v1.93.0 (MIT) + Roboto-Medium.ttf (Apache 2.0), `VENDOR.txt` |

- **Threads:** everything on the XR thread. `XrsPanelOps` (xr_shell.h) is attached by `xr_main.c` after
  `blackvr.ini` was read (`xrs_panel_attach`; tests/vr_glue's `shell_setup` likewise). `xr_session.c` calls
  `input` in `input_sync` before `vrb_set_input`, `draw` after `xrBeginFrame`, `layers` under the queue lock before
  `xrEndFrame`, `destroy` before the device goes. Builds without `xr_panel.c` (tests/game_render_x86) leave
  `s->panel` NULL: no link dependency.
- **Zero cost closed:** only the open-click check runs. The swapchains, Vulkan objects and ImGui are created at
  the first open and kept; while closed nothing is acquired, recorded, submitted or sampled (the harness checks the
  panel swapchain's acquire count stays put).
- **Vulkan sync:** as the test pattern: record into the slot's command buffer after waiting its fence, submit under
  `vr_queue_lock` with that fence, release the image. Font atlas uploads are copies recorded before the render
  pass in the same command buffer (barriers order them after earlier submissions that sampled the atlas); no
  `vkQueueWaitIdle`, no extra submit. Retired textures are destroyed 3 frames later. The swapchains are created
  under the queue lock (mid-session, the renderer may be submitting).
- **Image:** sRGB swapchain written through its UNORM view (ImGui colours are display values), cleared to
  (0,0,0,0) and blended with alpha ONE / ONE_MINUS_SRC_ALPHA: premultiplied, submitted with
  `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`. 1024 px over 0.8 m at 1.2 m ≈ 28 px/degree, about the
  Quest 3's display density; body text 28 px (~1°), help text 21 px, title 40 px.
- **Layers:** after the projection / HUD / flat layers: the panel quad (world space, the pose it opened with), then
  the laser quad (only when the gun hand is tracked). `layers[]` in `xrs_frame` grew from 3 to 6.
- **Info:** game fps = (eye + flat releases) per second, XR loop fps, from `vr_diag_counters` every 0.5 s; per-thread
  CPU from a sampler thread (`bvr-panel-cpu`) that runs only while the panel is open, once a second, with the
  HEARTBEAT threads line's method (utime + stime of `/proc/self/task/*/stat`, % of one core, busiest 8). The
  heartbeat itself lives in `src/main.c` (game side, 10 s period); it was not changed, so the sampling code is a
  copy in the shell. If you want one implementation, `main.c` could call the shell's later.

## Changes outside the new files

Native page (new: `src/black_vr_menu.inc`, `port/quest/src/vr_settings_menu.c` / `.h`):
- `src/recomp_manual.c`: `#include "black_vr_menu.inc"` (BLACK_VR) before `black_native_menu.inc`; in
  `sub_001F3A80` one line after the native fetch: `if (black_vr_menu_text(hash, &g_eax)) return;`.
- `src/black_native_menu.inc` `sub_00124330`: `black_vr_menu_frontend_pre(ui)` next to the prompts stream's
  `black_vr_prompts_frontend(ui)` (kept, first) and `black_vr_menu_frontend(ui)` after `sub_00124330_gen()`. CRLF
  line endings kept as in HEAD.
- `port/quest/vr_bridge.h`: the `vr_menu_*` API (appended). `vr_config.h`: `settings_menu = native | panel | both`
  (default native: the left stick click no longer opens the floating panel unless panel/both).
- `xr_session.c`: `vrsm_xr_frame(s)` once per frame (the page's queued OpenXR side effects).
- Builds: `vr_settings_menu.c` added to port/quest, tests/vr_glue and tests/game_render_x86 (its `grx_shell`
  library: the game calls `vr_menu_*`); `tests/game_boot_arm64/boot_stubs.c` stand-ins for `vr_menu_*`.
- `tests/game_render_x86/render_main.c`: the run-time press file takes `button:hold` (released after exactly
  `hold` s) and up to 256 presses (was 64; a live session ran out).

Floating panel:

- `xr_session.c`: `choose_refresh` → `xrs_refresh_request(s, preflight_line)`; the READY branch's performance-level
  block (the lead's `BLACKVR_PERF` code, logic unchanged) → `xrs_perf_apply(s, getenv("BLACKVR_PERF"))`;
  `xrs_foveation_update` (live level) and `hightop` in `xrs_foveation_apply`; `s->foveation_swapchain`; the panel
  hooks in `input_sync`, `xrs_frame` and `xrs_session_destroy`.
- `xr_vulkan.c`: `swapchain_destroy` → `xrs_swapchain_destroy` (public).
- `vr_bridge.c`: `vrb_head_now`, `vrb_menu_context`.
- `vr_config.h`: `VR_FOVEATION_HIGH_TOP`, `foveation = hightop`. `vr_input_map.h`: casts in `vri_touch_mask` (C++
  `-Wextra` enum/int conditional warnings; no behaviour change).
- `xr_main.c`: attaches the panel; default ini mentions `hightop` and the panel.
- `tests/vr_glue`: `fake_openxr` keeps up to 4 quads per frame (with each quad's released image and layer index)
  and refuses a quad layer whose swapchain never released an image; `shell_access` panel accessors.

## Tests

- `tests/xr_shell` → `xr-shell-settings` (238 checks; the menu model: values as the page shows them, stepping
  with wrap / ends / A, sections, `settings_menu`): clamping (ranges, step grid, ini precision, NaN, every
  float's ends parse back), get/set incl. env-backed settings and left-handed → gun hand, **ini round trip**
  (comments / blank / unknown / env / commented-env lines byte for byte, in-place edits keep spacing and comment
  column, appends, last line without newline, re-save is a no-op, read-back equals the edit, CRLF kept, new file,
  duplicate keys, too-small buffer refused), **live vs restart** with each capability combination, Cancel
  restore, placement / hit / beam math, open/close/pointer state machine (gameplay refusal, chord, long hold,
  left-handed, B / Menu / Close-button blocking until release, trigger hysteresis, scroll).
- `tests/vr_glue` (multiview run): not opened in gameplay; opened by the click in FLAT_MENU; quad layer in stage
  space at the opening pose, 0.8 × 0.6 m, 1024×768, premultiplied, over the game's layers, laser last and as long as
  the hit; neutral pad while open; the laser hits a pixel within 1.5 px; laser clicks: Comfort tab → Turning
  Smooth (live), Snap angle + → 35 (live), Graphics → Game rate 30 (not live), Foveation N/A on SwiftShader; Save
  writes the ini as described; B closes, its press never reaches the game, a new press does; no layers and no
  acquires when closed; reopen, change, Cancel restores; Menu closes without START. Image checks: transparent
  rounded corners, opaque body, red accents present.
- The native page in the game harness: see "Harness verification" above.
- Regression: `/tmp/claude-0/xrt` 6/6 (math, input, hooks ×2, settings, prompts), `/root/build/vr-glue` 2/2 (the
  panel test sets `settings_menu = panel`), `/root/build/kvk-android` 2/2, `/root/black/build-boot-a64` 1/1 (after
  `build-game-android`), `build-ui-android` (arm64 shell, no warnings), `build-vrmenu-x86` (game harness, builds and
  runs).

## Not verified (no headset)

- The native page on the device, and the pause menu's Options page anywhere (above). Its row layout is tuned for
  the front end's 640-wide stage at 4:3; the longest values ("1.10X (RESTART)") fit, a longer GPU name is cut to 16
  characters.

- Everything on the device: the quad layers' look (sharpness, size at 1.2 m, the laser beam quad's orientation
  and that the compositor shows it from that side), comfort of the placement, the trigger/scroll feel.
- `xrUpdateSwapchainFB` level changes on a live Adreno FDM swapchain; `hightop`'s offset; refresh-rate switching
  mid-session; performance-level changes.
- That BLACK's pause pages ignore L3 (above), and FLAT_MENU in photo / split-screen.
- CPU/GPU cost while open (ImGui frame + one 1024×768 pass per XR frame; expected well under 1 ms, unmeasured).
- Writing `/sdcard/BlackVR/blackvr.ini` (rename over FUSE) with All files access.
