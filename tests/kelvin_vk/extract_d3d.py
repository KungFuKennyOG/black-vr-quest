"""Extract the D3D11 backend's HLSL generators (production sources) for the Vulkan backend's parity tests.

Writes one C fragment holding, unchanged:
  kelvin_gpu_vsh.inc  kgpu_vsh_head .. vsh_generate     (NV2A vertex program -> HLSL)
  kelvin_gpu_pss.inc  ps_map .. ps_generate, ps_key_of  (register combiners -> HLSL)
The test includes it after the declarations those need (StrBuf comes with it; PSConst from gfx/kgfx_cfg_types.inc).
"""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--kelvin", required=True, help="src/nv2a/kelvin directory")
parser.add_argument("--output", required=True)
args = parser.parse_args()
kelvin = Path(args.kelvin)


def section(text, start, end):
    a = text.index(start)
    b = text.index(end, a)
    return text[a:b]


vsh = (kelvin / "kelvin_gpu_vsh.inc").read_text(encoding="utf-8")
pss = (kelvin / "kelvin_gpu_pss.inc").read_text(encoding="utf-8")
out = [
    "/* extracted by tests/kelvin_vk/extract_d3d.py: do not edit */\n",
    section(vsh, "static const char kgpu_vsh_head[]", "/* ---- environment maps"),
    section(pss, "/* The ubershader's map3/map1", "static void ps_cache_path"),
]
Path(args.output).write_text("".join(out), encoding="utf-8")
