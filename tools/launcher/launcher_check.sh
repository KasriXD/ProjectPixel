#!/usr/bin/env bash
# The launcher against a real platform (docs/launcher.md#testing). Starts a
# platform, publishes the platformer example with a picture, makes two
# accounts that are friends, lets one of them (a bot) play the level to the
# end (a best time on the leaderboard) and stay in the game, then opens each
# launcher page with screenshots and plays twice from the launcher: the game
# itself, and joining the friend. Every launcher run must end by itself
# ("Launcher finished") with exit code 0.
#
#   tools/launcher/launcher_check.sh BUILD_DIR OUT_DIR
#
# BUILD_DIR has px-platform, px-headless and px-player (with the launcher).
# Linux (or any Unix with curl); the player draws with SDL's dummy driver.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "$1" && pwd)
out=$2
mkdir -p "$out"
out=$(cd "$out" && pwd)
data="$out/platform"
rm -rf "$data" "$out/player-data"

platform="$build/services/platform/px-platform"
headless="$build/apps/headless/px-headless"
player="$build/apps/player/px-player"
port=8760
url="http://127.0.0.1:$port"
export SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software SDL_FRAMEBUFFER_ACCELERATION=0
export PX_PLATFORM_DIR="$out/player-data" # the launcher's login and downloads

"$platform" --data "$data" --port "$port" --no-rate-limits --server-ports 28400:20 --mail-dir "$data/outbox" \
  > "$out/platform.log" 2>&1 &
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

# api METHOD PATH TOKEN [JSON]
api() {
  curl -sf -X "$1" "$url/api/v1$2" -H "Authorization: Bearer $3" -H "Content-Type: application/json" ${4:+-d "$4"}
}
token_of() { sed -n 's/.*"token":"\([^"]*\)".*/\1/p'; }

# The game, with a picture of it (a frame of the level).
"$player" "$root/examples/platformer" --frames 40 --screenshot "$out/picture.png" > /dev/null 2>&1 ||
  fail "could not take a picture of the level"
published=$("$headless" "$root/examples/platformer" --platform "$url" --user maker --password maker-pass-123 \
  --register --publish --title "Coin Run" --thumbnail "$out/picture.png" 2>&1)
game=$(echo "$published" | sed -n 's/^published game=\([0-9]*\).*/\1/p')
[ -n "$game" ] || { echo "$published"; fail "could not publish the game"; }
echo "== published game $game"

ana=$(curl -sf "$url/api/v1/accounts" -H "Content-Type: application/json" \
  -d '{"username":"ana","password":"ana-pass-123"}' | token_of)
bob=$(curl -sf "$url/api/v1/accounts" -H "Content-Type: application/json" \
  -d '{"username":"bob","password":"bob-pass-123"}' | token_of)
[ -n "$ana" ] && [ -n "$bob" ] || fail "could not make the accounts"
api PATCH /me "$ana" '{"avatar":6,"bio":"Speedrunner in training."}' > /dev/null || fail "profile"
api PATCH /me "$bob" '{"avatar":11}' > /dev/null || fail "profile"
api POST /friends/requests "$ana" '{"username":"bob"}' > /dev/null || fail "friend request"
api POST /friends/requests/ana/accept "$bob" > /dev/null || fail "accepting the friend request"
curl -sf "$url/api/v1/accounts" -H "Content-Type: application/json" \
  -d '{"username":"cyd","password":"cyd-pass-123"}' | token_of > "$out/cyd.token"
api POST /friends/requests "$(cat "$out/cyd.token")" '{"username":"ana"}' > /dev/null || fail "friend request"
echo "== ana and bob are friends; cyd asked ana"

# Bob plays the level to the end (a best time), then stays in the game.
"$headless" --platform "$url" --user bob --password bob-pass-123 --play "$game" \
  --input-script "$root/examples/platformer/tests/full_run.input" --ticks 100000 > "$out/bob.log" 2>&1 &
bot_pid=$!
for _ in $(seq 1 300); do
  api GET "/games/$game/leaderboards" "$ana" 2> /dev/null | grep -q '"username":"bob"' && break
  kill -0 "$bot_pid" 2> /dev/null || { cat "$out/bob.log"; fail "the bot stopped"; }
  sleep 0.2
done
api GET "/games/$game/leaderboards" "$ana" | grep -q '"username":"bob"' || fail "bob's time is not on the leaderboard"
echo "== bob finished the level and is still playing"

# launch NAME ARGS...: one launcher run, logged in as ana.
launch() {
  local name=$1
  shift
  "$player" --launcher --platform "$url" --user ana --password ana-pass-123 "$@" \
    --screenshot "$out/launcher-$name.png" > "$out/launcher-$name.log" 2>&1
  local status=$?
  if [ $status -ne 0 ] || ! grep -q "Launcher finished" "$out/launcher-$name.log"; then
    tail -30 "$out/launcher-$name.log"
    fail "launcher run '$name' (exit $status)"
  fi
  echo "launcher $name: $(grep "Launcher finished" "$out/launcher-$name.log" | sed 's/.*INFO  //')"
}

launch games --frames 30
launch game --open "game:$game" --frames 30
launch friends --open friends --frames 30
launch you --open you --frames 30
launch account --open account --frames 30
grep -q "Saved screenshot" "$out/launcher-games.log" || fail "no screenshot"

# Playing from the launcher: the game, then back to the launcher.
launch play --open "play:$game" --frames 240
grep -q "Connected\|Joined" "$out/launcher-play.log" || { tail -30 "$out/launcher-play.log"; fail "the game did not join its server"; }
grep -q "Finished 240 ticks online" "$out/launcher-play.log" || fail "the game did not play online"

# Joining a friend: the same game server as bob.
launch join --open join:bob --frames 240
grep -q "joining bob" "$out/launcher-join.log" || fail "the launcher did not join bob"
grep -qE "Finished 240 ticks online.*\(most at once [2-9]\)" "$out/launcher-join.log" ||
  { tail -20 "$out/launcher-join.log"; fail "ana did not play in bob's game"; }

# A saved login (here, the one px-player --play saves): the launcher logs
# in with it, without --platform or --user.
# That run also invites bob from the game (what the menu's INVITE A FRIEND
# does); bob sees the invitation while ana is in the game.
"$player" --platform "$url" --play "$game" --user ana --password ana-pass-123 --invite bob --frames 900 \
  > "$out/saved-login.log" 2>&1 &
ana_pid=$!
invited=
for _ in $(seq 1 100); do
  api GET /me/social "$bob" | grep -q '"from":"ana"' && { invited=yes; break; }
  sleep 0.2
done
wait "$ana_pid" || { tail -20 "$out/saved-login.log"; fail "px-player --play"; }
grep -q "Invited bob" "$out/saved-login.log" || { tail -20 "$out/saved-login.log"; fail "ana could not invite bob"; }
[ -n "$invited" ] || fail "bob did not see ana's invitation"
[ -f "$PX_PLATFORM_DIR/login.json" ] || fail "no saved login"
echo "== ana invited bob from the game"
"$player" --launcher --frames 30 > "$out/launcher-saved.log" 2>&1 || { tail -20 "$out/launcher-saved.log"; fail "launcher with the saved login"; }
grep -q "logged in to $url as ana" "$out/launcher-saved.log" || { tail -20 "$out/launcher-saved.log"; fail "the saved login was not used"; }
echo "launcher saved login: $(grep "logged in" "$out/launcher-saved.log" | sed 's/.*INFO  //')"
echo "PASSED: the launcher logged in, showed games, a game page with leaderboards, friends and the profile, played the game, joined a friend, invited a friend from a game and used a saved login"
