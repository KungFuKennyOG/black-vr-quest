# Foveation: our own fragment density map on the Kelvin main passes

Status: implemented, untested on a headset. SwiftShader has no FDM, so only the code paths (via the `stereo-fdm`
test mode, which fakes device support) and the generator (`kvk-fdm`) are covered in CI.

## What it does
- The shell (`xr_vulkan.c`) enables `VK_EXT_fragment_density_map` (+ `_map2` if present) when the GPU reports the
  feature and non-subsampled images, and passes `fdm`, `fdm_texel_w/h` to the backend.
- The backend (`kvk_fdm.c`, `kvk_backend.c`) builds a 2-layer R8G8_UNORM density map (one layer per eye, multiview),
  centred on each eye's projection centre from the XrFovf tangents, and uploads it when the level or FOV changes.
- Only Kelvin main passes in stereo frames get the FDM render-pass variant. HUD, offscreen chains, depth-as-colour,
  composition and flat frames are never foveated. Main targets stay non-subsampled.
- Foveated pipelines are a separate variant (pipeline key `pad = 1`); startup "twin" variants are compiled from the
  manifest and foveation switches on only once they are ready. Feature off => keys and output are identical to before.

## Knobs
| Setting | Effect |
|---|---|
| ini `foveation` off/low/medium/high/hightop | strength (tiers 1.0 / 0.5 / 0.25 of full density); hightop moves the centre up 10 deg |
| `env RECOMP_KVK_FDM=0` / `1` | force FDM off / allow it (default on when supported) |
| `env RECOMP_KVK_GPUTIME=1` | GPU timestamps per pass class, logged every 180 frames: `[KVK] gpu ms/frame over ...` |
| `env RECOMP_KVK_PASSLOG=1`, `RECOMP_KVK_PASSLOG_FRAME=N` | log pass list of frame N (default 600) |

`env NAME=VALUE` lines in blackvr.ini are exported before Vulkan device creation, so they work on the headset.

Predicted fragment shares kept at full-equivalent cost (Quest 3 FOV): low 93%, medium 83%, high 63%, hightop 67%.

## Logs to look for
- `PREFLIGHT fdm ...`: extensions, features, texel sizes, invocations, why off.
- `[KVK] fdm: ...` in the backend report: `fdm_fills`, `fdm_passes` (passes should be > 0 when active).
- Pass classes in the GPU timer line: main-first, main-rest, depth-as-colour, offscreen, hud, compose, other.

## A/B plan
resolution_scale 1.2, level 1, medium foveation, three runs of 90 s each with `env RECOMP_KVK_FDM=0` and `=1`
(plus `env RECOMP_KVK_GPUTIME=1`). Compare HEARTBEAT fps, XR loop, and `[KVK] gpu ms`. Look at a capture for
visible density seams and for a vertically flipped centre (cy orientation is unverified on device).

## Known unverified
- eye-image y orientation of the density centre;
- first foveated frames on a cold manifest may skip draws until twin pipelines are ready;
- tier ring radii are tuned on paper only.
