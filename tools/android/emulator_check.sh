#!/usr/bin/env bash
# Installs the APK on a running Android emulator (or device) and starts it
# three times from the home screen (each start may turn the screen to
# landscape, which can make Android create the activity again). Each time the
# app must load its game and keep running; the last time, the back button
# must open the menu, not close the app.
# Screenshots: android-game.png, android-menu.png; logs in OUT_DIR.
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

recreated=0
for run in 1 2 3; do
  adb shell am force-stop dev.px.player
  adb shell input keyevent KEYCODE_HOME
  sleep 2
  adb logcat -c
  adb shell am start -W -n dev.px.player/.PlayerActivity > /dev/null
  for _ in $(seq 1 60); do
    adb logcat -d -s SDL/APP:I | grep -q "Loaded project" && break
    sleep 1
  done
  sleep 5
  adb logcat -d -s SDL:V SDL/APP:V > "$out/android-log-$run.txt"
  alive=$(adb shell pidof dev.px.player | tr -d '\r')
  grep -q "activity re-created" "$out/android-log-$run.txt" && recreated=$((recreated + 1))
  if ! grep -q "Loaded project" "$out/android-log-$run.txt" || [ -z "$alive" ]; then
    tail -40 "$out/android-log-$run.txt"
    echo "FAILED: start $run: the app did not load its game or stopped (pid: ${alive:-none})"
    system_log
    exit 1
  fi
  echo "start $run: loaded its game, running (pid $alive)"
done

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
echo "PASSED: three starts loaded the game (the activity was created again in $recreated of them); the back button opened the menu"
