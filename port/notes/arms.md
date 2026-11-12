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
- `left_hand = grip` (default): the game's left hand shows only while the two-handed grip is engaged (`g_two.engaged`,
  the existing two_hand gesture). `always` / `never` force it.
- Gun bones, the right hand and all other constants are never touched. The cut end tapers into a cuff.

## Settings (blackvr.ini)
| key | values | default |
|---|---|---|
| `arms` | `hands` / `full` | `hands` |
| `left_hand` | `grip` / `always` / `never` | `grip` |

## Tests
- `tests/xr_shell/test_arms.c` (50 checks): name roles in the SMG order, joints from inverse binds, the cut is seamless
  at the wrist, upper arms fold to the wrist, a hidden hand folds to one point, nothing else changes, no-arm models
  untouched.
- Harness (level 1 pistol): one-handed, the forearms and the left hand are gone and the right hand grips the gun;
  `left_hand = always`, both hands on the gun, both cut.

## Not verified
- Guns other than the pistol in the harness: the test-weapon hook (`RECOMP_BLACK_TEST_WEAPON`, now any 16-hex-digit
  inventory ID) was refused by the game's own check in level 1. Check the UZI, rifles, shotgun and RPG on the headset.
- The left hand during reloads (`grip` hides it unless the two-handed grip is on; `left_hand = always` shows the reload
  hand).
- The model of a left-handed player (`left_handed = 1`): the model's right arm is always treated as the gun hand.

## Debug switches added
- `RECOMP_KVK_VMSPLIT=1`: a different tint per weapon draw.
- `RECOMP_KVK_VMATTR=<n>`: the first n weapon draws' vertex attributes, constants and bone-index histograms.
- `RECOMP_BLACK_FPSCAN=<n>`: on the n-th first-person submission, bone-matrix arrays reachable from the actor and the
  inventory table.
