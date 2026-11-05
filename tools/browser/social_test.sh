#!/usr/bin/env bash
# The /play page's social side (docs/launcher.md#the-browser): starts a
# platform that serves the WebAssembly player, publishes the top-down example
# with a picture, makes ana (the browser), bob (a bot who finishes the level,
# setting a best time, and stays in the game) and cyd (who asks ana to be
# friends), then runs social_test.js: catalog pictures, the game's page and
# leaderboards, accepting and sending friend requests, the profile, joining
# bob from the friends page and inviting cyd from the game.
#
#   tools/browser/social_test.sh NATIVE_BUILD_DIR WEB_PLAYER_DIR OUT_DIR [desktop|phone] [PICTURE]
#
# NATIVE_BUILD_DIR has px-platform, px-server and px-headless; WEB_PLAYER_DIR
# px-player.js/.wasm/.data. phone: a phone-sized touch screen. PICTURE: the
# game's picture (default: a sprite).
# Needs Node.js with playwright (tools/browser: npm install playwright).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
native=$(cd "$1" && pwd)
web=$(cd "$2" && pwd)
out=$3
phone=
[ "${4:-desktop}" = phone ] && phone=yes
picture=${5:-$root/examples/topdown/assets/hero.png}
mkdir -p "$out"
out=$(cd "$out" && pwd)
data="$out/platform-social"
rm -rf "$data"
platform="$native/services/platform/px-platform"
headless="$native/apps/headless/px-headless"
port=8770
url="http://127.0.0.1:$port"

"$platform" --data "$data" --port "$port" --no-rate-limits --web-player "$web" --server-ports 28500:20 \
  > "$out/platform-social.log" 2>&1 &
platform_pid=$!
bot_pid=
cleanup() {
  [ -n "$bot_pid" ] && kill "$bot_pid" 2> /dev/null
  kill "$platform_pid" 2> /dev/null
  wait 2> /dev/null
}
trap cleanup EXIT
for _ in $(seq 1 50); do
  curl -sf "$url/api/v1/platform" > /dev/null && break
  sleep 0.2
done
fail() {
  echo "FAILED: $*"
  exit 1
}
api() { curl -sf -X "$1" "$url/api/v1$2" -H "Authorization: Bearer $3" -H "Content-Type: application/json" ${4:+-d "$4"}; }
account() {
  curl -sf "$url/api/v1/accounts" -H "Content-Type: application/json" -d "{\"username\":\"$1\",\"password\":\"$2\"}" |
    sed -n 's/.*"token":"\([^"]*\)".*/\1/p'
}

published=$("$headless" "$root/examples/topdown" --platform "$url" --user maker --password maker-pass-123 \
  --register --publish --title "Gem Garden" --thumbnail "$picture" 2>&1)
game=$(echo "$published" | sed -n 's/^published game=\([0-9]*\).*/\1/p')
[ -n "$game" ] || { echo "$published"; fail "could not publish the game"; }
ana=$(account ana ana-pass-123)
bob=$(account bob bob-pass-123)
cyd=$(account cyd cyd-pass-123)
[ -n "$ana" ] && [ -n "$bob" ] && [ -n "$cyd" ] || fail "could not make the accounts"
api PATCH /me "$bob" '{"avatar":11}' > /dev/null
api PATCH /me "$cyd" '{"avatar":4}' > /dev/null
api POST /friends/requests "$ana" '{"username":"bob"}' > /dev/null || fail "friend request"
api POST /friends/requests/ana/accept "$bob" > /dev/null || fail "accepting"
api POST /friends/requests "$cyd" '{"username":"ana"}' > /dev/null || fail "friend request"
echo "== game $game; ana and bob are friends; cyd asked ana"

"$headless" --platform "$url" --user bob --password bob-pass-123 --play "$game" \
  --input-script "$root/examples/topdown/tests/full_run.input" --ticks 100000 > "$out/bob-social.log" 2>&1 &
bot_pid=$!
for _ in $(seq 1 300); do
  api GET "/games/$game/leaderboards" "$ana" | grep -q '"username":"bob"' && break
  kill -0 "$bot_pid" 2> /dev/null || { cat "$out/bob-social.log"; fail "the bot stopped"; }
  sleep 0.2
done
api GET "/games/$game/leaderboards" "$ana" | grep -q '"username":"bob"' || fail "bob has no score"
echo "== bob finished the level and stays in the game"

node "$root/tools/browser/social_test.js" "$url" "$out" --game "$game" --user ana --password ana-pass-123 \
  --friend bob --requester cyd --requester-password cyd-pass-123 ${phone:+--phone}
