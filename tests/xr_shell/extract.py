"""Extract the authored BLACK_VR hooks (src/black_vr.inc) and the RAM guard they use; no game code."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests/ambient_occlusion/camera"))
from extract_camera import function  # noqa: E402

manual = (root / "src/recomp_manual.c").read_text(encoding="utf-8-sig")
vr = (root / "src/black_vr.inc").read_text(encoding="utf-8")
if manual.count('#include "black_vr.inc"') != 1:
    raise SystemExit("recomp_manual.c must include black_vr.inc exactly once")
for hook in ("black_vr_camera_begin(camera);", "black_vr_camera_restore(camera);", "black_vr_camera_after(camera);",
             "black_vr_look(camera);", "black_vr_upload(upload);", "black_vr_vm_push(", "black_vr_vm_pop();",
             "black_vr_world_mark(1);", "black_vr_world_mark(0);", "black_vr_input(self);",
             "black_vr_fx_update_begin();", "black_vr_fx_update_end();"):
    if hook not in manual:
        raise SystemExit("missing hook call in recomp_manual.c: " + hook)
start, end = vr.index("/* BEGIN BLACK_VR"), vr.index("/* END BLACK_VR */")
guard = function(manual, "static int black_pc_ram_span(uint32_t address, uint32_t bytes)")
Path(sys.argv[1]).write_text(guard + "\n" + vr[start:end], encoding="utf-8")
