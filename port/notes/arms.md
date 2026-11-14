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
  with the gun hand's orientation. While gripping, the left hand is the exact mirror of the game's right hand
  (`vra_mirror_off_hand`: P_lft = A * P_rt * S, using the bind pose's mirror symmetry), so its fingers close on the
  gun instead of keeping the game's relaxed left-hand pose. A short haptic tick on the off hand when the grip engages.
  The grip button latches: it takes the gun at 50 % and lets go under 25 % (with one 60 % threshold, a relaxed hold
  dropped the grip after 0.2–2.7 s in Kenny's device log).
- **Loose parts** (shell, magazine, clip, bullets, rocket, case...): the game's reload animations move them with its
  own left hand. A loose part more than 3 cm off its place on the gun (relative to the gun's `*_Root`) and nearer the
  game's left wrist than its right follows the moved left hand (fully from 8 cm). Before this, the shotgun shell
  floated where the game's hand had been. The game's own off-hand pose is not
  used there: for the rifles it keeps the left wrist about 0.55 m low and level with the camera, out of view
  (harness, inventory weapon 5446152380DB8000). `grip`: the game's pose, shown only while gripping. `always`: the
  game's pose. `never`: hidden.
- **Auto-grip** (`hand_auto = on`, default): each gun holds the game's right hand somewhere else, and the game's
  animations (idle sway, recoil, reload) move the wrist by up to ~10 cm and 30° (more in reloads). Two parts:
  - **Per-draw lock (position and rotation):** every bone of the weapon is moved in model space so the game's right
    wrist has a fixed pose on the controller in that very frame: position `wrist_offset` (metres, OpenXR aim space:
    right, up, back; default 0 -0.03 0.10), orientation the weapon's rest orientation (captured once the wrist has
    turned < 0.4° a frame for 15 frames). The game's sway, recoil and reload swings of the arm are locked out; the gun
    still animates relative to the hand (slide, magazine). History: ef2acc2 followed the animation slowly (the gun
    floated after shots); 5c51085 locked the position only, so the recoil's wrist rotation (~30°) spun the gun about
    the hand ("flying around").
  - **The gun is what is locked** once its rest pose is known: the gun's `*_Root` bone is held at its rest pose on the
    controller (placed so the wrist is on target at rest), and the game's hands animate on the gun (reloads, bolt
    pulls). Before the rest pose (the raise animation) the wrist is locked. Guns without a `*_Root` bone keep the
    wrist lock.
  - **Each weapon draw uses its own hand frame K:** the game draws other models with the weapon's register in the
    same frame (the ejected shell casings), so K is recorded per weapon MVP (`GlueVm.k`) and a draw finds its K by
    the MVP in its own c10 (bit for bit). Before (603a81b), the frame's last K was used: during a burst that was a
    spinning shell's, so the lock threw the gun around (Kenny's log: "moved the wrist up to 1.85 m, turned 178°").
  - **VR recoil kick** (`recoil`, 0..2, default 1): per shot (`vr_note_shot` from the muzzle flash hook) the target
    pose pitches up 5° about the wrist and moves back 2.5 cm, decaying with a 60 ms time constant.
  - The lock L (hand frame) is published (`vr_grip_lock`, 4x4) for the muzzle flash draw (w_hand * L) and main's
    muzzle effects (back = L * grip^-1). Logcat once a second: `arms: lock (last 60 frames): game moved the wrist up to
    ... m and turned it up to ... deg, held still`.
  - **Baseline:** the weapon's static offset, exact from its first 45 frames (the game thread records the correction
    each hand frame K was built with), then frozen; the game thread adds it to the grip offset
    (`vr_set_grip_correction`), so the game's own placement is already close.
  - Harness, rifle firing 30 s: the gun keeps its pose in the hand, flash at the muzzle.
- **Gripping two-handed:** when the game's own animated left hand is on the gun (its wrist ahead of the hand and near
  the gun line in the gun hand's aim frame), that hold is shown, as BLACK animates it; otherwise (the harness rifle
  keeps it ~40 cm low) the mirrored right hand on the gun line. Logged as `arms: gripping with ...`.
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
| `recoil` | 0..2, the VR kick per shot | `1.0` |

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

## Shots from the drawn gun, straight guns, the reload hand (Nov 13, after the headset video)
- **Shots left the flat gun, not the drawn one.** Bullets: 1DBD50 takes its origin from the actor's eye matrix
  (vtable +4C; for the player this sits at the flat muzzle, ~10-16 cm from the drawn one in the harness) and 1CEF40
  aims it at the flat crosshair. Rockets: 1E8540 launches through 1E35F0(a0 = launch matrix at the flat muzzle record,
  at = toward the crosshair; a1 = velocity). Wrapped in recomp_manual.c: for the player's own shot (shot+4 -> +F0 ==
  [2D45D4]+30), with `aim_barrel` on, the origin / launch matrix go through X (bvr_fx_x_now, the flat gun onto the
  drawn one) and the direction becomes X's turn of the flat barrel (the muzzle record's at, else the aim forward) with
  the game's spread kept. Log: `[BLACK_VR] shot: bullet|rocket from the drawn muzzle along the drawn barrel (...)`.
- **Angled guns are straightened.** The RPG's tube (gun root z = muzzle record at) is drawn 36 deg off straight ahead
  (toward the flat crosshair); held as drawn it pointed 36 deg off the controller, and the game's rocket flew across it.
  At the rest capture, a gun root 8..50 deg off the hand's forward is turned about the wrist target onto it
  (`arms: the gun is drawn N deg off straight ahead: turned to point along the controller`). RPG after: barrel 0.1 deg
  off the controller ray, rocket turned 0.8 deg. The UZI (1.6 deg) is untouched.
- **Reload hand.** The game tilts the gun 15+ deg in a reload (firing stays under 10); the left hand switches to the
  game's animation on that tilt or a loose part >3 cm off, until 0.3 s after both settle. Before, it switched ~22
  frames late, carrying nothing while the game's hand worked the magazine.
- The "gun points down in the reload" captures were from before the gun-root lock: a trace (`BLACKVR_ARMS_TRACE=1`)
  shows the game turning the SMG root 60 deg in the reload and the lock holding it.

## The left hand, Kenny's rules (Nov 13 evening)
- Not gripping: the left hand floats at the controller. (d6cfd4f broke this on straightened guns: the reload test
  measured the gun's turn against the straightened lock, ~36 deg always, so the game's hand stayed on.)
- One-handed guns: the UZI (left wrist at rest -0.26 -0.41 -0.34 m from the controller, > 25 cm): the grip does nothing.
  The pistol (left hand cupped under the grip, 0.14 m): the grip shows the game's hold, the look only, no two-handed
  aim (vr_set_gun_two_handed 1). Earlier text: one-handed guns (the game's left wrist off the gun at the rest capture,
  the pistol): the grip never takes the gun. No two-handed aim either (`vr_set_gun_two_handed`, gates the bridge's
  two-hand update). The mirrored right hand is gone.
- Two-handed guns: gripping near the gun shows the game's own two-handed hold ("the original two hands on the gun").
- The game's own left-hand animations (reloads): shown for the whole animation, then back to the controller. Busy while
  the game's left wrist is 4+ cm off its rest both on the gun (root frame) and in the view (model), the gun is turned
  15+ deg from the game's rest, and 0.35 s after (not loose parts: firing throws the AK's 14 cm every shot; their
  check is against their rest places now, the AK keeps one 22 cm from its bind place). Firing does not trip it (two-handed:
  the hand moves with the gun; one-handed: the hand stays in the view).

## Every gun checked (harness, level 1, Nov 13-14)
| gun | hold | straightened |
|---|---|---|
| AK (ak1), assault rifle (asr), heavy MG (hvy), shotgun (shg) | two-handed (grip takes it, two-handed aim) | no |
| pistol (pst) | look only (cupped hand, no two-handed aim) | no |
| RPG | look only (left hand 18 cm beside the tube) | 36 deg |
| sniper (snr) | look only (left wrist at rest 15 cm up-left of the hand) | 33 deg |
| UZI (smg) | one-handed | no |
| *5 variants (ak5, as5, hv5, ps5, sh5, sm5, sn5) | not in level 1's gun table: untested (same models as the base guns) | |
| bns | accepted, but never held still for the rest capture (wrist lock only) | |
- Straightening now uses the game's muzzle record (game thread -> `vr_gun_barrel`), not the gun root's z: same
  answer for the RPG (36) and the sniper (33).
- The left-wrist trigger for the game's animations is 8 cm (the sniper's idle moves it 4.6 cm).
