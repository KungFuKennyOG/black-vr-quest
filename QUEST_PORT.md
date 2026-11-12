# BLACK VR — Meta Quest 3 port

A standalone Quest 3 VR port built on [portingpete/Black-2006-Recomp](https://github.com/portingpete/Black-2006-Recomp)
(MIT), a static recompilation of BLACK (2006) for Xbox.

- Base: `portingpete/Black-2006-Recomp` @ `2041e5a97f13`
- Runtime: `portingpete/xboxrecomp` @ `a40397f9baec`, vendored into `third_party/xboxrecomp` with this port's changes
  (arm64 runtime and MMIO fault decoding, Kelvin→Vulkan backend with stereo/multiview, USB/OHCI and APU fixes, etc.)
- Quest app (OpenXR shell, preflight, install script): `port/quest/`; design notes per area: `port/notes/`
- Tests: `tests/` (arm64 runtime, Vulkan renderer, VR glue with a fake OpenXR runtime, full-game headless harness, …)

**No game data is included.** You need your own copy of BLACK; the recompiled C (`src/recomp/`) is generated from
your own disc's XBE with `scripts/generate.py` and must not be committed or distributed. Shader caches, captures and
logs made from the game stay local too. Upstream's GitHub Actions workflows are parked in `.github/workflows-upstream/`
so they don't run here.

Install on a Quest (developer mode on): put `blackvr.apk`, `port/quest/mac_install.sh` and your disc image in one
folder on a Mac and run `bash mac_install.sh`.
