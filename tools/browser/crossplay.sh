#!/usr/bin/env bash
# Browser cross-play test (docs/platform.md#browsers). Starts a platform that
# serves the WebAssembly player, publishes the top-down example, lets headless
# Chromium play its route through the /play page (over WebSockets) while a
# desktop bot joins the same game server (over UDP), and checks that each saw
# the other and the level completed.
#
#   tools/browser/crossplay.sh NATIVE_BUILD_DIR WEB_PLAYER_DIR OUT_DIR [http|https]
#
# NATIVE_BUILD_DIR has px-platform, px-server and px-headless (any preset with
# the platform), WEB_PLAYER_DIR px-player.js/.wasm/.data (the wasm preset).
# https: a development certificate; the browser uses wss:// to the game server.
# Needs Node.js with playwright (tools/browser: npm install playwright).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
native=$(cd "$1" && pwd)
web=$(cd "$2" && pwd)
out=$3
mode=${4:-http}
mkdir -p "$out"
out=$(cd "$out" && pwd)
data="$out/platform-$mode"
rm -rf "$data"

platform="$native/services/platform/px-platform"
headless="$native/apps/headless/px-headless"
if [ "$mode" = https ]; then port=8751; first=28300; else port=8750; first=28200; fi
args=(--data "$data" --port "$port" --no-rate-limits --web-player "$web" --server-ports "$first:20")
url="http://127.0.0.1:$port"
browser_extra=()
curl_extra=()
if [ "$mode" = https ]; then
  "$platform" --data "$data" --dev-certificate 127.0.0.1,localhost > /dev/null || exit 1
  args+=(--https-port $((port + 100)) --tls-cert "$data/tls/dev-cert.pem" --tls-key "$data/tls/dev-key.pem")
  url="https://127.0.0.1:$((port + 100))"
  export PX_PLATFORM_CA="$data/tls/dev-cert.pem" # for the desktop bot
  browser_extra=(--ignore-https-errors)
  curl_extra=(--cacert "$PX_PLATFORM_CA")
fi

"$platform" "${args[@]}" > "$out/platform-$mode.log" 2>&1 &
platform_pid=$!
browser_pid=
cleanup() {
  [ -n "$browser_pid" ] && kill "$browser_pid" 2> /dev/null
  kill "$platform_pid" 2> /dev/null
  wait 2> /dev/null
}
trap cleanup EXIT

for _ in $(seq 1 50); do
  curl -sf "${curl_extra[@]}" "$url/api/v1/platform" > /dev/null && break
  sleep 0.2
done
echo "== platform at $url"

published=$("$headless" "$root/examples/topdown" --platform "$url" --user maker --password maker-pass-123 \
  --register --publish --title "Gem Garden" 2>&1)
game=$(echo "$published" | sed -n 's/^published game=\([0-9]*\).*/\1/p')
if [ -z "$game" ]; then
  echo "$published"
  echo "FAILED: could not publish the game"
  exit 1
fi
echo "== published game $game"

# The browser joins first: it plays the route as player 1.
node "$root/tools/browser/platform_test.js" "$url" "$out/browser-$mode.png" --game "$game" \
  --user webplayer --password web-pass-123 --frames 1500 \
  --input-script /examples/topdown/tests/full_run.input \
  --expect-level-complete --expect-players 2 "${browser_extra[@]}" > "$out/browser-$mode.log" 2>&1 &
browser_pid=$!
for _ in $(seq 1 600); do
  grep -q "Joined" "$out/browser-$mode.log" && break
  kill -0 "$browser_pid" 2> /dev/null || break
  sleep 0.2
done
grep -q "Joined" "$out/browser-$mode.log" && echo "== the browser joined; now the desktop bot"

"$headless" --platform "$url" --user deskbot --password desk-pass-123 --register --play "$game" \
  --ticks 1500 --expect-players 2 --expect-level-complete > "$out/bot-$mode.log" 2>&1
bot_status=$?
wait "$browser_pid"
browser_status=$?
browser_pid=
echo "== browser (end):"
cat "$out/browser-$mode.log"
echo "== desktop bot (exit $bot_status):"
cat "$out/bot-$mode.log"
if [ "$bot_status" -ne 0 ] || [ "$browser_status" -ne 0 ]; then
  echo "== platform log:"
  tail -40 "$out/platform-$mode.log"
  echo "FAILED: cross-play over $mode (browser exit $browser_status, desktop bot exit $bot_status)"
  exit 1
fi
echo "PASSED: a browser and a desktop player played together over $mode"
