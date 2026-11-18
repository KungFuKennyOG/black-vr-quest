# BLACK VR for Meta Quest 3

An unofficial, fan-made VR port of **BLACK (2006)** for standalone Meta Quest 3 / 3S.
No PC is needed to play. You aim with your hands, crouch in real life, raise scoped guns to your eye, and can hold
two-handed weapons with both hands.

It is built on [portingpete/Black-2006-Recomp](https://github.com/portingpete/Black-2006-Recomp), a static
recompilation of the original Xbox game, with a native Quest renderer (Vulkan, stereo) and an OpenXR shell.

> **Early alpha.** It runs the game from boot through the menus and into missions in stereo, with sound, on a
> Quest 3. Expect rough edges, frame drops in busy fights, and bugs. See [Known issues](#known-issues).

**This project does not include any game files.** You need your own legally acquired copy of BLACK for the
original Xbox. We do not condone piracy. BLACK belongs to Criterion Games / Electronic Arts, and this project is not
affiliated with or endorsed by them.

### Quick start

1. [Check your disc](#check-your-disc) is the supported release.
2. Turn on [developer mode](#1-turn-on-developer-mode-once) on the headset.
3. Download `blackvr.apk` from [Releases](../../releases).
4. [Install it and copy your game files](#3-install). On a Mac it's one command.
5. Play from **Library → Unknown sources**.

## VR features

- **Hand aiming** with the game's own gun models and animations in your hand, and shots that leave the barrel.
- **Two-handed guns:** grab the gun with the left grip to steady your aim.
- **Real scopes:** raise a scoped gun to your eye and the 3D view zooms, with the game's own scope picture.
- **Physical crouch and melee:** duck for real, swing the gun to melee.
- **Laser sight**, haptics when you fire, snap or smooth turning, left-handed mode.
- **VR SETTINGS** page in the game's own menus.
- Comfort: no camera shake or head bob, and cutscenes on a flat screen.

## Contents

[What you need](#what-you-need) · [Developer mode](#1-turn-on-developer-mode-once) · [Get the APK](#2-get-the-apk) ·
[Install](#3-install) · [Play](#4-play) · [Controls](#controls-touch-controllers-right-handed) ·
[VR settings](#vr-settings) · [Saves](#saves) · [FAQ](#faq) · [Problems](#problems) ·
[Known issues](#known-issues) · [Building](#building-from-source) · [Credits](#credits) · [License](#license)

---

## What you need

- A **Meta Quest 3 or 3S** with **developer mode** on (see step 1).
- Your own **original Xbox** disc of BLACK, as a disc image (`.iso` / `.xiso`) or an extracted folder.
  - Only the release this port was built from works. Its `default.xbe` has the SHA-256
    `df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6`.
  - The PS2 version and other Xbox releases won't work. The app tells you if the disc doesn't match.
  - A full dump matters: missing files mean missing levels or cutscenes.
- About **1.5 GB** free on the headset.
- Only download BLACK VR from this repository's [Releases](../../releases) page. It isn't distributed anywhere
  else, so an APK from another site, Discord server or "game included" bundle didn't come from here.
- A **USB-C data cable** and a computer (Mac, Windows or Linux) to copy things to the headset. An Android phone works
  too (see [Without a computer](#without-a-computer)).

### Check your disc

Check the `default.xbe` from your extracted disc before you start:

- Mac / Linux: `shasum -a 256 default.xbe`
- Windows (PowerShell): `Get-FileHash default.xbe`

It should print `df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6`. If it doesn't, the release
APK won't run your disc. Don't worry if you only have the image: the app checks it at startup too, and the Mac
installer extracts it for you.

## 1. Turn on developer mode (once)

1. Create a free Meta developer organisation at [developers.meta.com](https://developers.meta.com/horizon/)
   (any name works).
2. In the **Meta Horizon** phone app: **Menu → Devices → your headset → Headset settings → Developer mode → on**.
3. Restart the headset.

## 2. Get the APK

Download the latest `blackvr.apk` from the [Releases](../../releases) page.

(Or build it yourself from your own disc: see [Building from source](#building-from-source).)

## 3. Install

### On a Mac: one command

Put these three things in one folder:

- `blackvr.apk`
- [`port/quest/mac_install.sh`](port/quest/mac_install.sh)
- your BLACK disc image (`.iso` or `.xiso`)

Plug in the headset, put it on, then in Terminal:

```sh
cd that-folder
bash mac_install.sh
```

When the headset asks **"Allow USB debugging"**, tick *Always allow from this computer* and accept.

The script installs the tools it needs through [Homebrew](https://brew.sh) (install that first), extracts the disc,
installs the app, copies the game files (about 1.2 GB, a few minutes) and starts BLACK VR. Press **Ctrl+C** when
you're done; it saves a log file next to the script. Run it again later to update: steps already done are skipped.

### On Windows or Linux

1. **Extract the disc** if you have an image. Use [extract-xiso](https://github.com/XboxDev/extract-xiso/releases):
   `extract-xiso -x BLACK.iso`. You get a folder with `default.xbe` inside; rename it to `game`.
2. **Install the APK** with [SideQuest](https://sidequestvr.com/setup-howto): plug in the headset, accept USB
   debugging, and drag `blackvr.apk` onto the SideQuest window.
   (Or with adb: `adb install -r -g blackvr.apk`.)
3. **Copy the game files.** The game folder must end up as `BlackVR/game/` on the headset's storage, so
   `BlackVR/game/default.xbe` exists.
   - Windows: open *This PC → Quest 3 → Internal shared storage*, make a `BlackVR` folder and copy `game` into it.
   - adb: `adb shell mkdir -p /sdcard/BlackVR` then `adb push game /sdcard/BlackVR/`
4. **Allow file access.** Start BLACK VR once from **Library → Unknown sources**. It opens the *All files access*
   page; turn it on for BLACK VR and start the app again.
   (Or with adb: `adb shell appops set com.kenny.blackvr MANAGE_EXTERNAL_STORAGE allow`.)

### Without a computer

With an Android phone, a USB-C OTG cable and an ADB app such as **Bugjaeger**: connect the phone to the headset,
accept USB debugging in the headset, install `blackvr.apk` from the phone, and push your `game` folder to
`/sdcard/BlackVR/`. Then do step 4 above in the headset.

### Where the app looks for the game

In order (the first one that has `default.xbe` wins):

1. `Android/data/com.kenny.blackvr/files/game`
2. `/sdcard/BlackVR/game`
3. `/sdcard/Download/BlackVR/game`
4. `/sdcard/BLACK/game`

Folders 2-4 need *All files access*. Settings go in `blackvr.ini` and saves in `save/`, next to the `game` folder.

## 4. Play

Open **Library → Unknown sources → BLACK VR**. Stand in a clear space; the game uses your real height.

| What you see | What it means |
|---|---|
| A test room with a **red sky** | No game files found. Check the folder (step 3). |
| A test room with a normal sky | The game is loading. Wait a few seconds. |
| A **solid red panel** | A startup self-check failed. See [Problems](#problems). |
| An **orange panel** | The game stopped during startup. See [Problems](#problems). |

## Controls (Touch controllers, right-handed)

| Controller | In game |
|---|---|
| Right trigger | Fire |
| Right A | Reload |
| Right B | Laser sight on / off |
| Swing the gun hand fast | Melee |
| Right stick left / right | Snap turn (smooth turn in VR SETTINGS) |
| Right stick up / down | Next / previous weapon (in a scope: zoom) |
| Right stick click | Use, pick up, open doors |
| Left stick | Move (towards where you look; can be the gun hand or the left hand) |
| Left stick click, or duck for real | Crouch |
| Left trigger | Grenade |
| Left grip on the gun | Hold it with both hands (steadier aim) |
| Left X | Health pack |
| Left Y | Tap: fire mode. Hold: suppressor |
| Menu | Tap: pause. Hold 1 s: objectives |
| Raise a scoped gun to your eye | Look through the scope (two-handed guns need the left hand on them) |
| Hold both stick clicks 1 s | Recentre the view and re-measure your height |
| Any button during a movie | Skip it |

In menus: the right stick and d-pad move, **A** confirms, **B** goes back.
**Left-handed?** Turn on *left-handed* in VR SETTINGS; everything is mirrored.

## VR settings

In the game's **Options** menu (main menu, or the pause menu's Options page) there's a **VR SETTINGS** button:

- **Graphics:** resolution, foveation, display refresh rate, game rate (60 or 30 Hz), performance mode.
- **Comfort:** snap / smooth / no turning, turn angle and speed, move direction, left-handed, world scale,
  laser sight, skip cutscenes.
- **Gestures:** swing to melee, duck to crouch, raise to scope, two-handed aim, and how sensitive each one is.
- **Info:** frame rates, the busiest threads, GPU and build.

Settings marked *(RESTART)* apply the next time you start the app; the rest apply at once. Everything is also in
`blackvr.ini`.

**If it feels choppy:** the defaults are a 60 Hz game on a 120 Hz display at 0.8 resolution. Busy fights can drop
below 60. Setting **game rate to 30** is steadier, and a lower resolution helps the GPU.

**Comfort:** your view only turns with your head and the stick. The game's camera shake, recoil and head bob don't
move your view, and cutscenes play on a flat screen in front of you.

## Saves

Saves go in a `save` folder next to `game` (for example `/sdcard/BlackVR/save/`), and settings in `blackvr.ini`
in the same place.

- If your game is in `/sdcard/BlackVR/`, saves **survive uninstalling** and updating the app.
- If it's in `Android/data/com.kenny.blackvr/files/`, **uninstalling deletes them** along with the game files.
  Copy `save/` off the headset first.
- To move to a new headset, copy the whole `BlackVR` folder (`game`, `save`, `blackvr.ini`).

## FAQ

**Why do I need my own disc? Why isn't the game included?**
BLACK is Criterion's and EA's game. This project only ships the port's code: the game's files come from your disc.

**Does it work on Quest 2 / Quest Pro / Pico?**
It's made for Quest 3 and 3S. Quest 2 and Quest Pro haven't been tested and probably don't have the headroom.
Pico isn't supported.

**PCVR?**
No. It runs standalone on the headset. For a flat PC version, see upstream
[Black-2006-Recomp](https://github.com/portingpete/Black-2006-Recomp).

**My disc is a different version.**
Only one Xbox release works with the release APK (see [What you need](#what-you-need)). You can
[build an APK](#building-from-source) from your own disc, but other releases haven't been tested.

**The floor or my height is wrong.**
Hold both stick clicks for a second. That recentres the view and measures your height again. Stand up straight
while you do it.

## Problems

- **Red sky / "no game files":** the folder is wrong or the copy is incomplete. You need
  `BlackVR/game/default.xbe` on the headset, plus the rest of the disc next to it.
- **"The recompiled code matches another XBE":** your disc is a different release from the one this APK was built
  for. Only the release in [What you need](#what-you-need) works with the release APK. You can build an APK
  for your own disc (see [Building from source](#building-from-source)).
- **Red or orange panel, crash or freeze:** get the log and open an issue.
  - Mac: `bash mac_install.sh --log-only` records a run; `bash mac_install.sh --pull-logs` copies the game's own logs
    from the headset.
  - Anything else: the game keeps its logs in a `logs` folder next to `game` on the headset (`blackvr_last.log` is
    the latest run), or run `adb logcat -s BlackVR`.
- **Install fails** ("signatures do not match"): uninstall the old BLACK VR first. That also deletes the game files
  in the app's own folder, so copy them again.

Report problems in [Issues](../../issues) with your headset model, what you saw and the log file. Don't attach game files.

## Known issues

- Frame rate drops in busy scenes at 60 Hz. The game runs a little slower while it drops (it isn't frame-skipped).
- Some effects may still look wrong in VR. Reports with a log help.
- Only one disc release is supported.
- Bugs from the original game are still there. If something also happens on the Xbox, it's probably not the port.
- Not tested on Quest 2 or Quest Pro. It needs a lot of CPU and memory, so Quest 2 likely won't work.

## Building from source

The recompiled game code is generated from **your own** `default.xbe`, so it is not in this repository. To build an
APK yourself, use an **Ubuntu 24.04 x86_64** machine (a VM or WSL2 works), with ~15 GB free:

```sh
git clone https://github.com/KungFuKennyOG/black-vr-quest
cd black-vr-quest
sudo scripts/quest_setup.sh --game /path/to/BLACK.iso --skip-swiftshader --skip-vvl
```

The script installs the Android NDK/SDK and the other pinned dependencies, recompiles your disc, builds the game
for arm64 and packages `build-quest-game/blackvr.apk`. It is resumable: run it again after a failure and it continues.
The first run takes about an hour. `scripts/quest_setup.sh --list` shows each step;
[port/notes/setup.md](port/notes/setup.md) has the details.

**Don't share generated code (`src/recomp/`), shader caches, captures, logs or anything else made from the game.**

### How it's put together

- Base: [portingpete/Black-2006-Recomp](https://github.com/portingpete/Black-2006-Recomp) @ `2041e5a97f13`.
- Runtime: [portingpete/xboxrecomp](https://github.com/portingpete/xboxrecomp) @ `a40397f9baec`, vendored into
  `third_party/xboxrecomp` with this port's changes: the arm64 runtime and MMIO fault decoding, a Kelvin → Vulkan
  backend with stereo/multiview, and USB/OHCI and APU fixes.
- Quest app (OpenXR shell, preflight, install script): [`port/quest/`](port/quest/). Design notes for each area:
  [`port/notes/`](port/notes/).
- Tests: [`tests/`](tests/) (arm64 runtime, Vulkan renderer, VR glue with a fake OpenXR runtime, a headless
  full-game harness, and more).
- Upstream's GitHub Actions workflows are parked in `.github/workflows-upstream/` so they don't run here.
- The flat PC version's readme from upstream is [README-PC.md](README-PC.md).

## Credits

- [portingpete/Black-2006-Recomp](https://github.com/portingpete/Black-2006-Recomp) and
  [xboxrecomp](https://github.com/portingpete/xboxrecomp): the recompilation this port is built on.
- [xemu](https://github.com/xemu-project/xemu): the APU and Kelvin (GPU) hardware code.
- [DSP56300](https://github.com/mborgerson/dsp56300), nv2a_vsh_cpu and the other components listed in
  [NOTICE](NOTICE).
- [DrBeef](https://github.com/DrBeef)'s Quest ports, which the controls and scope are modelled on (no code from
  them is used).

## Disclaimer

This is an unofficial fan project. It is not made, endorsed or supported by Criterion Games or Electronic Arts.
BLACK and its logos are trademarks of their respective owners. No game files are included or distributed here.
Please don't ask for game files or share them in Issues.

## License

The project's own code is [MIT](LICENSE). Some components keep their own licenses: the xemu-derived APU and Kelvin
renderer (including this port's changes to them) are LGPL-2.1-or-later. [NOTICE](NOTICE) and
[LICENSES/](LICENSES/) list every component.

These licenses cover the code only. They give no rights to BLACK or its game files.
