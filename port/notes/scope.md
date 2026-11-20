# The 3D scope view (2026-11-14)

Kenny: the flat head-locked window for the sniper scope (FLAT_SCOPE) feels bad. DrBeef's ports zoom the 3D view
instead. This is that, built from the ideas in his sources (RTCWQuest, JKXR, Lambda1VR, ioq3quest); none of their code
is used (RTCWQuest is LGPL-3, the others GPL).

## What his ports do

- **Zoom:** each eye's field of view narrowed in tangent space, `angle = atan(tan(angle) / zoom)`.
- **Aim:** the view turned to the weapon's direction, the roll kept from the head.
- **Engage:** a two-handed weapon held near the face (~0.25 m).
- **Comfort:**
  - RTCWQuest: movement at a third and stick turning off while scoped.
  - Lambda1VR: both eyes the same image and a black vignette ring.
  - ioq3quest: the zoom ramps in (0.05 per frame) and out faster; the weapon is hidden; a reticle is drawn.

## What BLACK VR does

| Piece | Where | How |
|---|---|---|
| When | `src/black_vr.inc` `black_vr_decide` | A scoped weapon zoomed to `scope_zoom` or more (2.0 for scoped weapons: the sniper's X2 is 2.33). `scope_view = zoom` keeps STEREO and publishes the game's zoom (`vr_set_scope_view`, one store per world camera; 0 otherwise); `flat` is the old FLAT_SCOPE window. |
| Raise | `vr_bridge.c` (scope raise) | The existing raise-to-eye gesture (rear sight within `scope_raise_distance`, 15 cm). With `scope_view = zoom`, a two-handed gun (class 2) also needs the off hand on it (two-hand aim engaged) to start; once up it stays 30 frames after the off hand lets go. |
| Aim | `vr_gestures.h` `vrg_scope_aim_update`, `vr_bridge.c` | Since 2026-11-15 (below): DrBeef's virtual stock. Published as `vr_scope_aim`; the gun hand's aim pose becomes it (from the eye) while the scope is up. |
| Eyes | `vr_renderer_kvk.c` `scope_eyes` | Both stereo phases (first draw, flip). Each eye: orientation the scope aim with the head's roll (`vrg_scope_view_rotation`); fields of view divided in tan by the zoom. (Before 2026-11-15: along the drawn barrel, up from the head.) `scope_mono` (default on): both eyes at their midpoint. The projection layer is submitted with the latched poses and fields of view unchanged, so the compositor spreads the narrow picture over the whole view. |
| Ease | `scope_step` | By wall time: in over 0.25 s, out over 0.125 s, smoothstep; the orientation blends with the same factor. Zoom steps (stick up/down) are smoothed over 80 ms. Log: `scope view: in, zoom 2.33 (one picture)` / `scope view: out`. |
| Weapon | `kvk_vr_viewmodel_bones` | Once the ease passes a third: every weapon draw's 192 constants zeroed (gun, arms, suppressor and flash collapse to a point). |
| Scope picture | `xr_session.c`, `xr_panel.c` `xrs_scope_layers` | The game's own: BLACK draws its sniper scope (sniperhud: the lens vignette, the reticle with its range marks, X2/X5) on the HUD layer, and while the view is in that quad is centred on the view, 10 m out, tan ±0.8 wide (the scope circle about 29° in radius), so its reticle sits on the zoomed gun line. Under it, a black surround painted once (transparent to tan 0.51, black by 0.58, out to tan ±2) covers the view beyond the quad. Kenny 2026-11-14: "has to look how it looks in the game" / "can't you just rip it". (The opaque black HUD panel of sweep issue 2 was the HUD flip bug, fixed since: the scoped HUD layer is transparent in the lens, harness 2026-11-14.) |
| Laser | `xr_panel.c` | Off while the view is in: the game's reticle is the aim. |
| Movement | `vr_input_map.h` | `ctx.scope_view`: the move stick divided by 3, stick turning off; the gun stick's up/down still steps the zoom. |
| Camera | `black_vr.inc` `black_vr_camera_after_world` | While zoomed the game's camera turns to the scope's view (the aim with the head's roll), so the game culls for what the zoomed eyes see. |
| Look and shots | `black_vr.inc` `black_vr_look`, `black_vr_shot_begin` | The look is the scope aim's forward, from the eye; the game's own shot is kept (no move to the hidden drawn barrel): it goes where the scope's centre looks. |

Options (VR SETTINGS > Comfort, live): **3D scope** (`scope_view = zoom | flat`, also `on | off`) and **Scope: one
image** (`scope_mono = on | off`).

## The aim, redone after DrBeef's ports (2026-11-15)

Kenny on the headset: "the sniper aim is AWFUL to use, it's like tilted and there's a ton of clipping", "really check
how dr beef does it". Read in RTCWQuest (`VrInputDefault.c`, `VrInputCommon.c` updateScopeAngles,
`RTCWVR_SurfaceView.c` GetMove, `cg_view.c`) and JKXR (`VrInputDefault.cpp`, `JKXR_SurfaceView.cpp`); method only.

What they do, and what we did differently:
| | DrBeef | BLACK VR before | now |
|---|---|---|---|
| Aim line | head -> off hand (a virtual stock: JKXR lifts the off hand 12 cm, RTCW lowers the eye 10 cm; the off hand averaged over 5 frames) | the drawn barrel (gun hand -> off hand two-handed, through the 33° straightened sniper) | head -> off hand + 12 cm, low-passed (50 ms) |
| Yaw | relative: the head's yaw when the scope came up + how far the aim has turned since | absolute barrel direction (the view jumped to the barrel as it eased in) | relative |
| Pitch | the aim's own | the barrel's | the aim's own (lift makes level hold level) |
| Roll | none in the aim; the head's for the view | up from the head projected on the barrel (tilted when the barrel left the gaze) | none in the aim; the head's roll for the picture, so its horizon is level |
| Game camera | the game's view angles become the scope's (RTCW `GetMove` yaw/pitch, cg_view) | stayed on the head: the zoomed view along the gun ran out of the culled frustum ("clipping") | the camera turns to the scope view while zoomed |
| Shots | from the HMD along the scope (JKXR "shoot from exactly where we are looking") | from the hidden drawn muzzle along the barrel | the game's own shot along the look (= the scope's centre) |
| Let go | distance only (0.25-0.26 m gun to HMD), or the off hand lets go | 22.5 cm or 40° off the view | distance only once up, 25 cm; the off hand off for 30 frames |
| Comfort | movement / 3, no positional movement, no stick turn | the same | the same |

`vrg_scope_aim_update` tests: `tests/xr_shell/test_input.c` `test_scope_aim`. Log: `scope aim: on (the stock: head to
the off hand, lift 0.12 m, smoothing 50 ms)`.

## Limits

- **Culling.** Fixed 2026-11-15: the game's camera turns with the scope view. During the 0.25 s ease the eyes are
  between the head and the scope while the camera is already on the scope (its window is the unzoomed eyes' x 1.10,
  so far apart directions can lose an edge for that moment).
- **Stereo (`scope_mono = off`).** Each eye's own view is magnified with the picture, so the depth is too: like
  binoculars, far things look nearer.
- **Reprojection.** The compositor treats the zoomed picture as the head's view. A head turn between the render and the
  display moves it as if unzoomed (zoom x too little). At 72 Hz this is a fraction of a frame.

## Headset, 2026-11-15 23:29 (27f777f): "the aiming for the sniper is just stuck" -> fixed
The log: `scope aim: on (the stock ...)`, then 0.7 s later `two-hand off (... gun two-handed 0)`. Once the zoom hides the
gun there are no weapon draws, and the renderer's "ten flips without the gun" rule published the gun as not
two-handed: the two-hand hold dropped, the stock read as gone and the aim held its last value for good.
- The renderer no longer counts flips while the scope hides the gun (`scope_hides_weapon`).
- The stock is DrBeef's `weapon_stabilised`: the off hand tracked, its grip held (50 %, kept to 15 %), within 60 cm
  of the gun hand (`scope_stock_held`), not the two-hand aim's own test.
- Losing the off hand for more than 3 updates hands the aim to the gun hand's ray from where it is (no jump); the
  stock takes over again when the hand comes back (`vrg_scope_rebase`). Tests: `test_scope_aim`.

## Headset, 2026-11-16: "not aligned with the cross air when I aim and shoot", "doesn't feel like it's at the center of the screen (like the whole thing)"

**Shots.** Scoped, the game's own shot was kept ("it goes where the scope's centre looks"). It does not: the game
starts the shot at the actor's eye point and aims it at the weapon's aim point. Harness (level 5, the SVD, raise +
twohand, `[BLACK_VR] scoped shot` lines): the shot left 0.7-0.95 m ahead of the camera and up to 0.4 m beside it,
4.4 and 7.1 degrees off the camera's axis, the reticle's line.
- Now (`black_vr_shot_aimed`, scoped branch): the player's scoped shot starts at the camera (`w_head`) and goes along
  its axis; the game's spread (the shot's turn away from the direction toward weapon+10) is kept. The camera is the
  scope's view (`black_vr_camera_after_world`), so the hit is what the reticle's centre shows.
- Log: `scoped shot N: from the scope's centre along it; the game's left ...` (first 8, then every 50th;
  `BLACKVR_AIM_LOG=1`: all).

**Centre.** The picture, the game's scope (HUD quad) and the surround were centred on the head's forward. A Quest's
field of view reaches further down than up, so that point is above the middle of what the wearer sees.
- Now all three sit `drop` above the head's forward: `vrg_scope_centre_pitch`, the mean of the eyes' up and down
  angles (negative on a Quest), or `scope_height = <degrees>` in blackvr.ini (`auto` by default, -20..20).
- Eyes (`scope_eyes`): turned so the aim lies at `drop` in the eye, and the field of view narrowed about that point
  (`c + (t - c) / zoom`), not about the eye's axis: the aim is shown exactly at `drop`.
- Shell: `xrs_scope_drop` (xr_shell.h) places the HUD quad (xr_session.c) and the surround (xr_panel.c).
- Log, once: `scope view: centred N deg below straight ahead (...; the eyes see ...)`: the headset's real angles.

**Harness notes.** Aiming is refused while the level's opening objective banner is up. SwiftShader runs about one frame
a second in stereo, so the game's zoom takes a long time to come in.
