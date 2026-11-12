#!/bin/bash
# BLACK VR: one-command install + test run for Meta Quest 3, from a Mac.
#
# Put this script, blackvr.apk and your BLACK disc image (.iso / .xiso) in one folder, then:
#     cd that-folder && bash mac_install.sh
#
# What it does (each step is skipped when already done):
#   1. installs Homebrew tools: extract-xiso, adb (android-platform-tools)
#   2. extracts the disc image to ./game (once)
#   3. waits for the Quest over USB (developer mode on, "Allow USB debugging" accepted)
#   4. installs blackvr.apk
#   5. copies ./game to the headset (once; ~1.2 GB)
#   6. launches BLACK VR and records the full log to blackvr_log_<time>.txt until you press Ctrl+C
#
# Options:
#   --iso PATH       disc image (default: the only *.iso / *.xiso in this folder)
#   --apk PATH       APK (default: ./blackvr.apk)
#   --repush         copy the game files again even if they are already on the headset
#   --no-launch      install only
#   --log-only       skip everything, just launch and record the log
#   --pull-logs      only copy the log files the game keeps on the headset (a run without the cable) here
#
# Nothing here touches anything outside this folder and the BLACK VR app's own storage on the headset.

set -u
PKG=com.kenny.blackvr
# the app also reads /sdcard/BlackVR/game (with All files access, granted in step 4); adb cannot create
# folders under Android/data on newer Quest OS versions ("secure_mkdirs failed: Operation not permitted")
REMOTE=/sdcard/BlackVR
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

ISO=""; APK="$HERE/blackvr.apk"; REPUSH=0; LAUNCH=1; LOGONLY=0; PULLONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2;;
    --apk) APK="$2"; shift 2;;
    --repush) REPUSH=1; shift;;
    --no-launch) LAUNCH=0; shift;;
    --log-only) LOGONLY=1; shift;;
    --pull-logs) PULLONLY=1; LOGONLY=1; shift;;
    -h|--help) sed -n 2,22p "$0"; exit 0;;
    *) echo "unknown option: $1"; exit 2;;
  esac
done

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die()  { printf '\n\033[31mERROR: %s\033[0m\n' "$*"; exit 1; }
ok()   { printf '   \033[32mOK\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------- 1. tools
if [ $LOGONLY -eq 0 ]; then
  say "1/6 Tools"
  if ! command -v brew >/dev/null 2>&1; then
    for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$b" ] && eval "$("$b" shellenv)"; done
  fi
  command -v brew >/dev/null 2>&1 || die "Homebrew is not installed. Install it from https://brew.sh (one command on that page), then run this script again."
  if ! command -v extract-xiso >/dev/null 2>&1; then
    # not in Homebrew: build it from source (github.com/XboxDev/extract-xiso)
    command -v cmake >/dev/null 2>&1 || brew install cmake || die "could not install cmake"
    rm -rf "$HERE/.extract-xiso" && git clone --depth 1 https://github.com/XboxDev/extract-xiso.git "$HERE/.extract-xiso" \
      && cmake -S "$HERE/.extract-xiso" -B "$HERE/.extract-xiso/build" >/dev/null \
      && cmake --build "$HERE/.extract-xiso/build" >/dev/null \
      && cp "$HERE/.extract-xiso/build/extract-xiso" "$(brew --prefix)/bin/" || die "could not build extract-xiso"
  fi
  command -v adb >/dev/null 2>&1 || brew install --cask android-platform-tools || die "could not install adb"
  ok "extract-xiso, adb"
fi
command -v adb >/dev/null 2>&1 || die "adb not found. Run without --log-only first."

# ---------------------------------------------------------------- 2. extract
if [ $LOGONLY -eq 0 ]; then
  say "2/6 Game files"
  if [ -f "$HERE/game/default.xbe" ]; then
    ok "already extracted to ./game"
  else
    if [ -z "$ISO" ]; then
      n=0
      for f in "$HERE"/*.iso "$HERE"/*.xiso "$HERE"/*.ISO; do [ -f "$f" ] && { ISO="$f"; n=$((n+1)); }; done
      [ $n -eq 0 ] && die "no disc image (.iso/.xiso) found in this folder. Put it next to this script, or pass --iso PATH."
      [ $n -gt 1 ] && die "several disc images here; pick one with --iso PATH."
    fi
    [ -f "$ISO" ] || die "disc image not found: $ISO"
    size=$(wc -c < "$ISO" | tr -d ' ')
    echo "   image: $ISO ($((size/1048576)) MB)"
    rm -rf "$HERE/.xiso_tmp"; mkdir -p "$HERE/.xiso_tmp"
    # options before the image: macOS getopt stops at the first file name
    extract-xiso -d "$HERE/.xiso_tmp" -x "$ISO" 2>&1 | tail -5
    xbe=$(find "$HERE/.xiso_tmp" -maxdepth 3 -iname default.xbe | head -1)
    [ -n "$xbe" ] || die "extraction did not produce default.xbe. Is this the BLACK disc image?"
    mv "$(dirname "$xbe")" "$HERE/game" && rm -rf "$HERE/.xiso_tmp"
    ok "extracted to ./game ($(find "$HERE/game" -type f | wc -l | tr -d ' ') files)"
  fi
  [ -d "$HERE/game/levels" ] || echo "   warning: no levels/ folder in ./game; the extraction may be incomplete"
  [ -d "$HERE/game/videos" ] || echo "   note: no videos/ folder (cutscenes will be skipped; the game still runs)"
fi

# ---------------------------------------------------------------- 3. headset
say "3/6 Headset"
echo "   Plug the Quest into this Mac with a USB-C cable and put it on."
echo "   Accept \"Allow USB debugging\" (tick \"Always allow from this computer\")."
adb start-server >/dev/null 2>&1
tries=0
while :; do
  state=$(adb devices | awk 'NR>1 && $2!="" {print $2; exit}')
  [ "$state" = "device" ] && break
  if [ $tries -eq 0 ] || [ $((tries % 10)) -eq 0 ]; then
    case "$state" in
      unauthorized) echo "   waiting: accept the USB debugging prompt inside the headset";;
      offline)      echo "   waiting: headset offline; replug the cable";;
      *)            echo "   waiting for the headset (is developer mode on? is the cable a data cable?)";;
    esac
  fi
  tries=$((tries+1)); [ $tries -gt 180 ] && die "no headset after 3 minutes."
  sleep 1
done
model=$(adb shell getprop ro.product.model 2>/dev/null | tr -d '\r')
ok "connected: $model"
free_kb=$(adb shell df /sdcard 2>/dev/null | awk 'NR==2 {print $4}' | tr -d '\r')
[ -n "$free_kb" ] && echo "   free space on headset: $((free_kb/1048576)) GB (BLACK needs ~1.3 GB + a few hundred MB for saves)"

# ---------------------------------------------------------------- 4. install
if [ $LOGONLY -eq 0 ]; then
  say "4/6 Install APK"
  [ -f "$APK" ] || die "APK not found: $APK"
  adb install -r -g "$APK" 2>&1 | tail -2 | grep -q Success || {
    echo "   install failed; retrying after uninstalling the old copy (this also removes the game files on the headset; step 5 copies them again)"
    adb uninstall $PKG >/dev/null 2>&1
    adb install -r -g "$APK" | tail -2
  }
  adb shell pm path $PKG >/dev/null 2>&1 || die "APK did not install"
  adb shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1
  ok "installed $PKG"

# ---------------------------------------------------------------- 5. push
  say "5/6 Copy game files to the headset"
  remote_xbe=$(adb shell "[ -f $REMOTE/game/default.xbe ] && echo yes" 2>/dev/null | tr -d '\r')
  local_n=$(find "$HERE/game" -type f ! -name '.DS_Store' ! -name '._*' | wc -l | tr -d ' ')
  remote_n=$(adb shell "find $REMOTE/game -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d '\r ')
  # at least as many files as here: the headset may hold a few the game or an earlier copy added
  if [ "$remote_xbe" = "yes" ] && [ "${remote_n:-0}" -ge "$local_n" ] && [ $REPUSH -eq 0 ]; then
    ok "already on the headset ($remote_n files)"
  else
    adb shell mkdir -p "$REMOTE" || die "cannot create $REMOTE"
    [ $REPUSH -eq 1 ] && adb shell rm -rf "$REMOTE/game"
    echo "   copying ~1.2 GB, a few minutes..."
    adb push "$HERE/game" "$REMOTE/" | tail -1 || die "copy failed"
    remote_n=$(adb shell "find $REMOTE/game -type f | wc -l" | tr -d '\r ')
    [ "${remote_n:-0}" -ge "$local_n" ] || die "copied $remote_n of $local_n files; run again with --repush"
    ok "copied $remote_n files"
  fi
fi

[ $LAUNCH -eq 1 ] || { say "Done (not launched)."; exit 0; }

# ---------------------------------------------------------------- 6. run + log
# The app's own verdict on the first lines it logged: the PREFLIGHT checks (port/notes/device-preflight.md), a
# crash or a stall. Shown here so a failed check is visible without opening the log.
# the game also writes its own log on the headset (survives a dropped USB connection): copy it next to the adb log
pull_device_logs() {
  local stamp; stamp=$(date +%Y%m%d_%H%M%S)
  for f in blackvr_last.log blackvr_last.1.log blackvr_prev.log; do
    adb pull "$REMOTE/logs/$f" "$HERE/device_${stamp}_$f" >/dev/null 2>&1 && echo "   headset log: device_${stamp}_$f"
  done
}
summarize() {
  echo
  pull_device_logs
  if grep -q "PREFLIGHT summary" "$LOG" 2>/dev/null; then
    echo "   app self-check: $(grep -o 'PREFLIGHT summary:.*' "$LOG" | tail -1 | cut -c1-200)"
    grep -oE 'PREFLIGHT [a-z-]+: (FAIL|WARN).*' "$LOG" | cut -c1-220 | sed 's/^/     /' | head -12
  else
    echo "   no PREFLIGHT lines in the log: the app did not get as far as its first check"
  fi
  grep -oE '(CRASH|STALL) .*' "$LOG" | head -3 | cut -c1-220 | sed 's/^/   /'
}
if [ $PULLONLY -eq 1 ]; then
  say "Copying the game's own logs from the headset"
  pull_device_logs
  echo "   Send these files back in the chat (blackvr_last = the latest run, blackvr_prev = the one before)."
  exit 0
fi
say "6/6 Launch and record the log"
LOG="$HERE/blackvr_log_$(date +%Y%m%d_%H%M%S).txt"
adb shell am force-stop $PKG >/dev/null 2>&1
adb logcat -c 2>/dev/null
adb logcat -G 16M >/dev/null 2>&1   # a bigger device log buffer, so a busy game doesn't outrun the reader
{
  echo "# BLACK VR log  $(date)"
  echo "# device: $model  build: $(adb shell getprop ro.build.fingerprint | tr -d '\r')"
  echo "# apk: $(shasum -a 256 "$APK" 2>/dev/null | cut -c1-16)"
  echo "# memory: $(adb shell cat /proc/meminfo | head -3 | tr -d '\r' | tr '\n' ' ')"
} > "$LOG"
# the reader can drop out ("logcat: Unexpected EOF") when the game logs a lot: restart it, from the newest line
(
  first=1
  while true; do
    if [ $first -eq 1 ]; then adb logcat -b main,system,crash -v threadtime 2>/dev/null
    else echo "# --- log reader restarted $(date +%H:%M:%S); a few lines may be missing (the headset's own log has them) ---"
         adb logcat -b main,system,crash -v threadtime -T 1 2>/dev/null; fi
    first=0
    adb get-state >/dev/null 2>&1 || sleep 2
    sleep 1
  done
) >> "$LOG" &
LOGPID=$!
stop_reader() { pkill -P $LOGPID 2>/dev/null; kill $LOGPID 2>/dev/null; }
trap 'stop_reader; summarize; echo; say "Log saved: $LOG"; echo "   Send this file back in the chat, plus one line on what you saw."; exit 0' INT TERM
sleep 1
adb shell am start -n $PKG/android.app.NativeActivity >/dev/null 2>&1 \
  || adb shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
echo "   BLACK VR is starting in the headset."
echo "   Play until it crashes, freezes, or you have seen enough, then press Ctrl+C here."
while kill -0 $LOGPID 2>/dev/null; do
  sleep 5
  if ! adb shell pidof $PKG >/dev/null 2>&1; then
    sleep 3
    if ! adb shell pidof $PKG >/dev/null 2>&1; then
      echo "   the app is no longer running (crash or exit)."
      sleep 2   # let the crash buffer flush into the log
      stop_reader
      summarize
      say "Log saved: $LOG"
      echo "   Send this file back in the chat, plus one line on what you saw."
      exit 0
    fi
  fi
done
