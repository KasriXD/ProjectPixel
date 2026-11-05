#!/usr/bin/env bash
# Installs the APK on a running Android emulator (or device) and checks it:
#  1. started three times from the home screen (each start may turn the
#     screen to landscape, which can make Android create the activity
#     again), the app opens the launcher and keeps running;
#  2. started with the game on this device (--open local:0), the game loads
#     and the back button opens its menu, not close the app;
#  3. a test run (--launcher --open local:0 --frames N) plays the game, goes
#     back to the launcher in the same window and ends by itself.
# Screenshots: android-launcher.png, android-game.png, android-menu.png; logs in OUT_DIR.
#   tools/android/emulator_check.sh pixel-player.apk OUT_DIR
set -u
apk=$1
out=${2:-.}
mkdir -p "$out"
adb wait-for-device
adb install -r "$apk" || exit 1

system_log() {
  adb logcat -d -s ActivityTaskManager:I ActivityManager:I WindowManager:W AndroidRuntime:E DEBUG:I \
    > "$out/android-system.txt" 2> /dev/null || true
  echo "== Android's activity log:"
  grep -iE "px.player|dev\.px|config|relaunch" "$out/android-system.txt" | tail -40
  adb logcat -d -b crash 2> /dev/null | tail -40
}

# start [ARGS]: a fresh start of the app (with player options as the intent's "args").
start() {
  adb shell am force-stop dev.px.player
  adb shell input keyevent KEYCODE_HOME
  sleep 2
  adb logcat -c
  if [ $# -gt 0 ]; then
    adb shell am start -W -n dev.px.player/.PlayerActivity --es args "\"$*\"" > /dev/null
  else
    adb shell am start -W -n dev.px.player/.PlayerActivity > /dev/null
  fi
}

# wait_for TEXT SECONDS: until the app's log has TEXT.
wait_for() {
  for _ in $(seq 1 "$2"); do
    adb logcat -d -s SDL/APP:I | grep -q "$1" && return 0
    sleep 1
  done
  return 1
}

recreated=0
for run in 1 2 3; do
  start
  wait_for "Launcher open" 60
  sleep 5
  adb logcat -d -s SDL:V SDL/APP:V > "$out/android-log-$run.txt"
  alive=$(adb shell pidof dev.px.player | tr -d '\r')
  grep -q "activity re-created" "$out/android-log-$run.txt" && recreated=$((recreated + 1))
  if ! grep -q "Launcher open" "$out/android-log-$run.txt" || [ -z "$alive" ]; then
    tail -40 "$out/android-log-$run.txt"
    echo "FAILED: start $run: the app did not open the launcher or stopped (pid: ${alive:-none})"
    system_log
    exit 1
  fi
  echo "start $run: launcher open, running (pid $alive)"
done
adb exec-out screencap -p > "$out/android-launcher.png"

start --open local:0
wait_for "Loaded project" 60
sleep 5
alive=$(adb shell pidof dev.px.player | tr -d '\r')
adb logcat -d -s SDL:V SDL/APP:V > "$out/android-log-game.txt"
if ! grep -q "Loaded project" "$out/android-log-game.txt" || [ -z "$alive" ]; then
  tail -40 "$out/android-log-game.txt"
  echo "FAILED: the launcher did not start the game on this device (pid: ${alive:-none})"
  system_log
  exit 1
fi
echo "the launcher started the game on this device (pid $alive)"
adb exec-out screencap -p > "$out/android-game.png"
adb shell input keyevent KEYCODE_BACK
sleep 2
adb exec-out screencap -p > "$out/android-menu.png"
alive=$(adb shell pidof dev.px.player | tr -d '\r')
adb logcat -d -s SDL:V SDL/APP:V > "$out/android-log.txt"
tail -25 "$out/android-log.txt"
if [ -z "$alive" ] || ! grep -q "Menu opened" "$out/android-log.txt"; then
  echo "FAILED: the back button did not open the menu (app pid: ${alive:-none})"
  system_log
  exit 1
fi

# The round trip, in one window: launcher -> game (300 ticks) -> launcher -> quit.
start --launcher --open local:0 --frames 300
wait_for "Launcher finished" 90
adb logcat -d -s SDL:V SDL/APP:V > "$out/android-log-roundtrip.txt"
if ! grep -q "Launcher finished" "$out/android-log-roundtrip.txt" || ! grep -q "Loaded project" "$out/android-log-roundtrip.txt"; then
  tail -40 "$out/android-log-roundtrip.txt"
  echo "FAILED: the game did not go back to the launcher"
  system_log
  exit 1
fi
echo "PASSED: three starts opened the launcher (the activity was created again in $recreated of them); it started the game on this device, whose back button opened the menu; a game went back to the launcher in the same window"
