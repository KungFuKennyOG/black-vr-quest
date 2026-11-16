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
| Eyes | `vr_renderer_kvk.c` `scope_eyes` | Both stereo phases (first draw, flip). Each eye: orientation along the drawn barrel (`vr_gun_laser` dir in the latched gun hand `VrRenderGun.hand`; the game-time hand at the first draw; the controller ray if the barrel is unknown), up from the head; fields of view divided in tan by the zoom. `scope_mono` (default on): both eyes at their midpoint. The projection layer is submitted with the latched poses and fields of view unchanged, so the compositor spreads the narrow picture over the whole view. |
| Ease | `scope_step` | By wall time: in over 0.25 s, out over 0.125 s, smoothstep; the orientation blends with the same factor. Zoom steps (stick up/down) are smoothed over 80 ms. Log: `scope view: in, zoom 2.33 (one picture)` / `scope view: out`. |
| Weapon | `kvk_vr_viewmodel_bones` | Once the ease passes a third: every weapon draw's 192 constants zeroed (gun, arms, suppressor and flash collapse to a point). |
| Scope picture | `xr_session.c`, `xr_panel.c` `xrs_scope_layers` | The game's own: BLACK draws its sniper scope (sniperhud: the lens vignette, the reticle with its range marks, X2/X5) on the HUD layer, and while the view is in that quad is centred on the view, 10 m out, tan ±0.8 wide (the scope circle about 29° in radius), so its reticle sits on the zoomed gun line. Under it, a black surround painted once (transparent to tan 0.51, black by 0.58, out to tan ±2) covers the view beyond the quad. Kenny 2026-11-14: "has to look how it looks in the game" / "can't you just rip it". (The opaque black HUD panel of sweep issue 2 was the HUD flip bug, fixed since: the scoped HUD layer is transparent in the lens, harness 2026-11-14.) |
| Laser | `xr_panel.c` | Off while the view is in: the game's reticle is the aim. |
| Movement | `vr_input_map.h` | `ctx.scope_view`: the move stick divided by 3, stick turning off; the gun stick's up/down still steps the zoom. |
| Shots | unchanged | Along the drawn barrel from the drawn muzzle (`aim_barrel`). The view's centre is that same line seen from the eye, a few cm above it. |

Options (VR SETTINGS > Comfort, live): **3D scope** (`scope_view = zoom | flat`, also `on | off`) and **Scope: one
image** (`scope_mono = on | off`).

## Limits

- **Culling.** The game culls with its own camera, which follows the head: its window covers both eyes x 1.10. The
  zoomed view is narrow (about 31° half width at X2), so it stays inside while the gun is within ~20-25° of the gaze.
  The raise needs 25° to start and lets go at 40°, so near the edge of that range the view can lose far objects.
- **Stereo (`scope_mono = off`).** Each eye's own view is magnified with the picture, so the depth is too: like
  binoculars, far things look nearer.
- **Reprojection.** The compositor treats the zoomed picture as the head's view. A head turn between the render and the
  display moves it as if unzoomed (zoom x too little). At 72 Hz this is a fraction of a frame.
