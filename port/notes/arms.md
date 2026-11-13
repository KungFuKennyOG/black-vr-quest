# arms: first-person hands cut at the wrist (VR)

Status 2026-11-12. Harness-verified on the level 1 pistol; not run on a headset yet.

## How BLACK draws the first-person weapon
- One model per gun: `levels/<level>/fpguns/bg1_<gun>.db`. All 32 gun models found on the disc have the same arm
  skeleton (`Shoulder_`, `Elbow_`, `Wrist_`, `Finger1..4_*`, `Thumb_*` with `_lft` / `_rt`, plus `Joint_wrist_twist`
  on the left), but in different orders (the SMG's right shoulder is bone 20, most guns' 21, the magnums' arms start at
  bone 10).
- Two draws per frame: the gun mesh and the shared arms mesh (`bg1_arm`). Both arms are in the one arms draw.
- Skinned on the GPU (vertex program 463DBDF0). The vertex data never changes between frames. The bone palette is in
  the vertex constants: bone k of the model's bone-name table is three rows at c[60 + 3k] (x, y, z rows, w =
  translation). Checked: with the model's inverse-bind matrices (MODEL root +54), the captured palette puts every
  parent/child joint pair at the same point (0.0 m error) for the pistol models.

## What the port does (`port/quest/src/vr_arms.h`, glue in `vr_renderer_kvk.c`, hook in `kvk_backend.c`)
- For a draw already classified as the weapon (the gun-latch classification), the backend hands a copy of its 192
  constants and its vertex pointer to `kvk_vr_viewmodel_bones()`.
- The glue finds the MODEL root before the vertex data (GtID MODEL 0x95820AD4B8000000, version 0x23), reads the bone
  names and inverse binds (cached per vertex pointer, revalidated each use), and builds the rig by name.
- `vra_apply` collapses palette matrices to a single point (the posed wrist joint):
  - `arms = hands` (default): Shoulder/Elbow/twist of both arms fold into their wrist. The hand stays.
  - the off hand hidden: every bone of the left arm folds to its wrist, so its triangles have no area.
- `left_hand = tracked` (default): the game's left hand follows the off controller; while the two-handed grip is
  engaged it snaps onto the gun line: straight ahead of the gun hand at the off controller's distance (at least 8 cm),
  with the gun hand's orientation, so it mirrors the right hand's grip further along the gun. A short haptic tick on
  the off hand when the grip engages. The game's own off-hand pose is not
  used there: for the rifles it keeps the left wrist about 0.55 m low and level with the camera, out of view
  (harness, inventory weapon 5446152380DB8000). `grip`: the game's pose, shown only while gripping. `always`: the
  game's pose. `never`: hidden.
- **Auto-grip** (`hand_auto = on`, default): each gun model holds the game's right hand somewhere else relative to the
  fixed `vm_offset`, so the right hand did not sit on the player's hand (rifles: far back). The renderer measures the
  game's right wrist in the gun hand's aim frame (via K, below) and integrates the error to `wrist_offset` (metres,
  OpenXR aim space, default 0 -0.03 0.10) into a per-weapon grip correction; the game thread adds it to the grip
  offset (`vr_set_grip_correction` / `vr_grip_correction`). Fast for the first ~2 s of a weapon (gain 0.08), then slow
  (0.01, error limited to 5 cm a step) so recoil and reload animations do not drag the gun. Harness, rifle: the wrist
  went from 11 cm right / 7 cm low of target to exactly on it within 120 draws. `vm_offset` is now only the
  starting guess.
- How the tracked hand is placed (render thread, from the late latch, so it moves at display rate): the stock arms
  are mirror images across model x = 0 (bind pose checked on the gun models). The game thread records
  K = hand_from_model (`vgm_weapon_hand_frame`: the gun hand's aim frame from the weapon's model space, the same chain
  the weapon re-target uses) with the weapon MVP (`vr_mark_viewmodel_offhand`). The latch locates the off hand at the
  eyes' display time (`VrRenderGun.off`). The renderer forms rel = gun hand from off hand (`vgm_off_hand_rel`, snapped
  while gripping) and A = K^-1 * rel * S * K (`vgm_off_hand_mirror`, S = flip x), moves the left wrist to
  A * (posed right wrist) * mirror, where mirror = bind_rt^-1 * S * bind_lft, and the whole left arm rigidly with it,
  so the game's own finger pose is kept. Then the cut is applied as usual.
- The right hand and the gun bones are never edited (auto-grip moves the whole weapon through the grip offset). The cut end tapers into a cuff.

## Settings (blackvr.ini)
| key | values | default |
|---|---|---|
| `arms` | `hands` / `full` | `hands` |
| `left_hand` | `tracked` / `grip` / `always` / `never` | `tracked` |
| `hand_auto` | `on` / `off` | `on` |
| `wrist_offset` | metres from the controller's aim pose to your wrist: right, up, back | `0 -0.03 0.10` |

## Tests
- `tests/xr_shell/test_arms.c` (50 checks): name roles in the SMG order, joints from inverse binds, the cut is seamless
  at the wrist, upper arms fold to the wrist, a hidden hand folds to one point, nothing else changes, no-arm models
  untouched.
- `test_arms.c` also checks the placement (87 checks in all): a mirror-image controller puts the left wrist exactly on
  its bind frame, a moved controller moves the wrist by the same amount, the fingers keep their pose, the right arm
  and gun bones are untouched.
- Harness, `GRX_CONTROLS=1` (both controllers tracked): the game's left hand shows at the left controller, cut at the
  wrist, beside the pistol.
- Harness (level 1 pistol): one-handed, the forearms and the left hand are gone and the right hand grips the gun;
  `left_hand = always`, both hands on the gun, both cut.

## Not verified
- Guns other than the pistol in the harness: the test-weapon hook (`RECOMP_BLACK_TEST_WEAPON`, now any 16-hex-digit
  inventory ID) was refused by the game's own check in level 1. Check the UZI, rifles, shotgun and RPG on the headset.
- The left hand during reloads: with `tracked` it stays at your controller (the game's reload animation moves its
  fingers but not its place); `always` shows the game's own reload hand.
- The tracked hand's exact fit on the controller is a mirror of the right hand's fit (set by the grip offset); it is
  not tuned on the headset yet.
- The model of a left-handed player (`left_handed = 1`): the model's right arm is always treated as the gun hand.

## Debug switches added
- `RECOMP_KVK_VMSPLIT=1`: a different tint per weapon draw.
- `RECOMP_KVK_VMATTR=<n>`: the first n weapon draws' vertex attributes, constants and bone-index histograms.
- `RECOMP_BLACK_FPSCAN=<n>`: on the n-th first-person submission, bone-matrix arrays reachable from the actor and the
  inventory table.
