#!/bin/sh
# replay_ab.sh: draw each recorded backend trace (RECOMP_KVK_TRACE, see kvk_trace.h) twice through kvk-replay, once with
# the pass structure as before (RECOMP_KVK_PASS_MERGE=0, the reference) and once with the setting under test (default
# RECOMP_KVK_PASS_MERGE=1 with RECOMP_KVK_POISON=1, so a wrong DONT_CARE shows), and compare every picture of every
# flip bit for bit (port/notes/renderer.md section 13).
#
#   usage: replay_ab.sh <kvk-replay> <work dir> <trace.ktr>...
#   env:   AB_ENV_B   extra environment of the run under test (default "RECOMP_KVK_PASS_MERGE=1 RECOMP_KVK_POISON=1")
#          AB_KEEP=1  keep the raw pictures (default: only frame 0's PPMs and any difference images are kept)
#          VK_ICD_FILENAMES (SwiftShader), RECOMP_KGPU_CACHE (a warm shader cache saves time)
# Prints one line per trace; exits 1 if any picture differs.
set -u
replay=$1; work=$2; shift 2
envb=${AB_ENV_B:-"RECOMP_KVK_PASS_MERGE=1 RECOMP_KVK_POISON=1"}
mkdir -p "$work"
export RECOMP_KVK_SYNC_COMPILE=1 RECOMP_KGPU_PIPE_ASYNC=0
fail=0
for t in "$@"; do
    n=$(basename "$t" .ktr)
    a="$work/$n.a"; b="$work/$n.b"
    rm -rf "$a" "$b"
    RECOMP_KVK_PASS_MERGE=0 "$replay" "$t" "$a" --ppm > "$work/$n.a.log" 2>&1 || { echo "$n: reference run failed"; fail=1; continue; }
    env $envb "$replay" "$t" "$b" --compare "$a" > "$work/$n.b.log" 2>&1
    rc=$?
    stats() { grep "\[KVK\] PASSES" "$1" | tail -1 |
              sed -e 's/.*per frame \([0-9.]*\) passes, main target opened \([0-9.]*\)x.*traffic \([0-9.]*\) MiB.*/\1 passes, main opened \2x, \3 MiB/'; }
    verdict=$(grep "^kvk-replay: \(IDENTICAL\|DIFFERENT\)" "$work/$n.b.log" | sed 's/^kvk-replay: //')
    pics=$(grep -c "^  frame" "$work/$n.b.log")
    echo "$n: $verdict ($pics pictures) | before: $(stats "$work/$n.a.log") | after: $(stats "$work/$n.b.log")"
    [ $rc -eq 0 ] || fail=1
    if [ "${AB_KEEP:-0}" != 1 ]; then rm -f "$a"/*.raw "$b"/*.raw; fi
done
exit $fail
