#!/usr/bin/env bash
# Installs the APK on a running Android emulator (or device), starts it, and
# checks that it loads its game and keeps running; the back button must open
# the menu, not close the app. Screenshots: android-game.png, android-menu.png.
#   tools/android/emulator_check.sh pixel-player.apk OUT_DIR
set -u
apk=$1
out=${2:-.}
mkdir -p "$out"
adb wait-for-device
adb install -r "$apk" || exit 1
adb logcat -c
adb shell am start -W -n dev.px.player/.PlayerActivity
for _ in $(seq 1 60); do
  adb logcat -d -s SDL/APP:I | grep -q "Loaded project" && break
  sleep 1
done
sleep 5
adb exec-out screencap -p > "$out/android-game.png"
adb shell input keyevent KEYCODE_BACK
sleep 2
adb exec-out screencap -p > "$out/android-menu.png"
alive=$(adb shell pidof dev.px.player | tr -d '\r')
adb logcat -d -s SDL:V SDL/APP:V > "$out/android-log.txt"
adb logcat -d -b crash > "$out/android-crash.txt" 2> /dev/null || true
cat "$out/android-log.txt" | tail -40
if ! grep -q "Loaded project" "$out/android-log.txt"; then
  echo "FAILED: the app did not load its game"
  exit 1
fi
if [ -z "$alive" ]; then
  echo "FAILED: the app is not running after the back button"
  cat "$out/android-crash.txt"
  exit 1
fi
echo "PASSED: the app loaded its game and is running (pid $alive)"
