# BLACK VR for Meta Quest 3 / 3S

A standalone Quest app (`com.kenny.blackvr`) that runs the BLACK static recompilation in
VR. You supply your own original Xbox BLACK disc files; nothing from the game is in the APK.

> **Status (2026-11-02): smoke build.** The APK installs and starts the VR shell, and with
> no game linked (or no game files) it shows a **test scene**: head tracking, stereo,
> controllers, buttons, snap turn and the flat screen (quad) layer. The game itself needs
> the Vulkan renderer and the ARM64 runtime from the other port streams. See
> [What works](#what-works-and-what-is-untested).

## 1. Build the APK

On the build machine (Linux, with the Android NDK r27c and build-tools 34):

```sh
. /root/xtool/env.sh
port/quest/build_apk.sh                # smoke build: shell + test scene
port/quest/build_apk.sh --with-game    # links black_game / xboxrecomp / black_vk_renderer when they exist
```

The output is `build-quest/blackvr.apk`, signed with a debug key. To sign it with your own key,
set `BLACKVR_KEYSTORE`, `BLACKVR_KEYSTORE_PASS` and `BLACKVR_KEY_ALIAS`. The script builds the
Khronos OpenXR loader from source once (`/root/deps/OpenXR-SDK`). No Gradle or Maven is needed.
For a PC with Android Studio, `port/quest/gradle/` is a Gradle project with the same manifest
and CMake. It gets the loader from Maven Central.

## 2. Put the APK on the headset

**Developer mode is required for every method.** Set it up once in the Meta Horizon phone app:
Menu → Devices → your headset → Headset settings → Developer mode → on. You need a
(free) Meta developer organisation for this.

### Without a PC

- **From an Android phone (Bugjaeger or a similar ADB app):** connect the phone to the headset
  with a USB-C cable (OTG). Accept "Allow USB debugging" in the headset, then install
  `blackvr.apk` from the phone's storage.
- **On the headset itself (unverified):** download `blackvr.apk` in the Quest browser and open
  it from *Files* → Downloads. Whether Horizon OS offers to install it depends on the OS
  version. If it does not, use the phone method above.

### With a PC

- **SideQuest** (Windows/macOS/Linux): plug in the headset, accept USB debugging, drag
  `blackvr.apk` onto SideQuest.
- **adb**: `adb install -r build-quest/blackvr.apk`

The app appears under **Library → Unknown sources** as *BLACK VR*.

## 3. Your game files

Use your own retail BLACK disc (original Xbox, the release the build was generated from, with
`default.xbe` SHA-256 `df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6`). Extract it as you would for the PC version. The launcher
or any XISO extractor gives a folder that contains `default.xbe`.

Start BLACK VR once. It creates its folder and a `WHERE_TO_PUT_GAME_FILES.txt` there:

```
/sdcard/Android/data/com.kenny.blackvr/files/
    blackvr.ini                     VR settings (created on first start)
    game/default.xbe                <- your disc files go here (the whole disc in game/)
    game/...
    save/                           saves
```

- **With a PC:** plug in the headset and open *Quest 3 → Internal shared storage → Android →
  data → com.kenny.blackvr → files* (Windows Explorer / Android File Transfer). Copy your
  `game` folder in. Or run `adb push game /sdcard/Android/data/com.kenny.blackvr/files/`.
- **Without a PC:** Android blocks other apps from writing to `Android/data`. Put the disc in
  **`/sdcard/BLACK/game/`** instead (e.g. copy it from a USB-C stick with a file manager in the
  headset). When BLACK VR finds `/sdcard/BLACK` but cannot read it, it opens the system page
  *All files access*. Allow it for BLACK VR and start the app again.

The app looks in this order, and logs every folder it tried on its `PREFLIGHT gamedir` line:
`Android/data/com.kenny.blackvr/files/game`, `/sdcard/BlackVR/game`, `/sdcard/Download/BlackVR/game`,
`/sdcard/BLACK/game` (all but the first need *All files access*).

If no folder has `default.xbe`, the app shows the test scene with a **red sky**. With the game
found, the test scene (normal sky) shows while the game boots, until its first frame. A **solid red
panel** in front of you means a startup self-check failed and the game was not started; an **orange
panel** means the game stopped during startup. The log says why: every check writes one
`PREFLIGHT <check>: OK|WARN|FAIL` line, explained in `port/notes/device-preflight.md`.

## 4. Controls (Touch Plus)

In play, the controllers press BLACK's own actions on whatever buttons the game's controller layout
gives them (any of the four layouts, or your own from the Controls page). Right-handed
(`left_handed = 1` mirrors everything):

| Touch | Game |
|---|---|
| Right trigger | fire |
| Right A / B | reload / melee (a fast swing of the gun hand also melees) |
| Right stick left/right | snap turn 30° (`turn = smooth` for smooth turning) |
| Right stick up / down | next / previous weapon (one per push); in a scope: the scope's zoom |
| Right stick click | use / pick up / open doors |
| Right grip | nothing (`scope_raise = off`: hold for the scope) |
| Left stick | move (where you look; `locomotion = aim`: where the gun points, `offhand`: where the left controller points) |
| Left stick click | crouch (as the game's Toggle Crouch option says) |
| Duck / stand up | crouch / stand (`duck_crouch`, `duck_ratio`) |
| Left trigger | grenade |
| Left grip near the gun | hold the gun with both hands (steadier aim; `two_hand`) |
| Left X | health pack |
| Left Y tap / hold | suppressor / fire mode |
| Menu (left) tap / hold 1 s | pause / objectives |
| Raise a scoped weapon to your eye | look through the scope (`scope_raise`; other weapons never zoom) |
| Both stick clicks (hold 1 s) | recentre the view and re-measure your standing height (or the Meta button long-press) |
| Menus | right stick and the d-pad navigate; A confirms, B goes back, Menu = Start |

You aim with the right controller. The game's own shot crosses the controller's ray 10 m
ahead (`aim_converge`), and the first-person weapon is moved into your hand. `aim = head`
aims with your head instead. BLACK's bullet spread does not depend on aiming down the sights,
so hip-fired shots are as accurate as the game ever makes them. All settings are in `blackvr.ini`.

## 5. Comfort

- The view turns only with your head and with snap/smooth turning. The game's camera shake,
  recoil kick and roll do not move your view, vertical head bob is smoothed, and camera
  motion blur and the full-screen reload/aim blur are off in VR.
- Aiming down a scope switches to a flat, head-locked scope window at high zoom
  (`scope_zoom`). Lower zoom does not change the field of view in VR.
- Cutscenes and scripted cameras play on a flat screen 2.2 m in front of you. Menus, pause and
  movies do the same.
- The game's HUD (ammo, health, prompts) floats 2 m in front of your eyes, 2.4 m wide, and moves
  with your head (`hud_distance`, `hud_width`).
- The game runs at its own ~60 Hz. The headset shows 72 Hz (`refresh_hz` = 90 is available)
  and reprojects every frame to your current head pose, so head rotation stays smooth when the
  game drops frames. Moving objects animate at the game's rate.

## What works and what is untested

| | Status |
|---|---|
| APK builds, packages (aapt2 / zipalign -p 16 / apksigner v3) and verifies | **built here** |
| OpenXR loader built from source for arm64, `XR_KHR_loader_init_android` | **built here** |
| Shell math, input mapping, settings file, game camera/aim/weapon hooks | **unit-tested** natively (`tests/xr_shell`) |
| Session, stage space, Vulkan through `XR_KHR_vulkan_enable2`, multiview swapchain, test scene, controllers, quad layer, 72/90 Hz | written, **not yet run on a headset** |
| Renderer glue: Kelvin Vulkan backend on the shell's device, per-eye late latch, HUD quad layer, flat modes on the quad layer | **integration-tested** on SwiftShader with a fake OpenXR runtime (`tests/vr_glue`, port/notes/vr-glue.md) |
| Game on the headset (needs the ARM64 runtime stream) | **not yet possible** |
| Game camera in stereo, hand aiming, weapon in hand, disabled effects | implemented against documented hook points; **untested with the real game** |

On the first headset run, check: the test scene's floor is at your real floor (stage space;
in local space it sits 1.6 m below your head), the yellow pillar is ahead of you, the green
cube 1.5 m ahead looks solid in stereo, both controller beams follow your hands, A/B/X/Y
recolour the cube, holding X shows the colour-cycling flat panel, and the right stick snap-turns
(the test scene does not rotate yet; the turn reaches the game only). Logs: `adb logcat -s BlackVR`.
