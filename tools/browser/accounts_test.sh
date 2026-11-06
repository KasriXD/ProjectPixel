#!/usr/bin/env bash
# Accounts on the /play page (docs/accounts.md): starts a platform without a
# browser player, publishes the platformer example, then runs
# accounts_test.js: an account, an email address confirmed from the mail,
# changing the password, downloading the data, a password reset from the
# mail and deleting the account.
#
#   tools/browser/accounts_test.sh NATIVE_BUILD_DIR OUT_DIR [files|smtp] [desktop|phone]
#
# files: the platform writes its mail into a folder (--mail-dir).
# smtp:  the platform sends it to a real SMTP server on this computer
#        (aiosmtpd, which insists on STARTTLS) with a development
#        certificate; needs python3 -m pip install aiosmtpd.
# Needs Node.js with playwright (tools/browser: npm install playwright).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
native=$(cd "$1" && pwd)
out=$2
mode=${3:-files}
phone=
[ "${4:-desktop}" = phone ] && phone=yes
mkdir -p "$out"
out=$(cd "$out" && pwd)
data="$out/platform-accounts-$mode"
rm -rf "$data"
mkdir -p "$data/no-player"
platform="$native/services/platform/px-platform"
headless="$native/apps/headless/px-headless"
if [ "$mode" = smtp ]; then port=8781; smtp_port=8825; else port=8780; fi
url="http://127.0.0.1:$port"
args=(--data "$data" --port "$port" --no-rate-limits --web-player "$data/no-player" --server-ports 28600:10
  --name "Pixel Test")

smtp_pid=
if [ "$mode" = smtp ]; then
  "$platform" --data "$data" --dev-certificate localhost,127.0.0.1 > /dev/null || exit 1
  mail="$data/maildir"
  python3 -m aiosmtpd -n -l "127.0.0.1:$smtp_port" --tlscert "$data/tls/dev-cert.pem" --tlskey "$data/tls/dev-key.pem" \
    -c aiosmtpd.handlers.Mailbox "$mail" > "$out/smtp-server.log" 2>&1 &
  smtp_pid=$!
  args+=(--smtp "smtp://localhost:$smtp_port" --smtp-ca "$data/tls/dev-cert.pem" --mail-from "Pixel Test <noreply@example.com>")
  for _ in $(seq 1 50); do
    (exec 3<> "/dev/tcp/127.0.0.1/$smtp_port") 2> /dev/null && break
    sleep 0.2
  done
else
  mail="$data/outbox"
  args+=(--mail-dir "$mail")
fi

"$platform" "${args[@]}" > "$out/platform-accounts-$mode.log" 2>&1 &
platform_pid=$!
cleanup() {
  kill "$platform_pid" 2> /dev/null
  [ -n "$smtp_pid" ] && kill "$smtp_pid" 2> /dev/null
  wait 2> /dev/null
}
trap cleanup EXIT
for _ in $(seq 1 50); do
  curl -sf "$url/api/v1/platform" > /dev/null && break
  sleep 0.2
done
fail() {
  echo "FAILED: $*"
  echo "--- platform log"
  tail -n 40 "$out/platform-accounts-$mode.log"
  exit 1
}
curl -sf "$url/api/v1/platform" | grep -q '"mail":true' || fail "the platform does not send mail"

published=$("$headless" "$root/examples/platformer" --platform "$url" --user maker --password maker-pass-123 \
  --register --publish --title "Coin Run" 2>&1)
game=$(echo "$published" | sed -n 's/^published game=\([0-9]*\).*/\1/p')
[ -n "$game" ] || { echo "$published"; fail "could not publish the game"; }
echo "== game $game; mail: $mode ($mail)"

node "$root/tools/browser/accounts_test.js" "$url" "$out" --mail "$mail" --game "$game" ${phone:+--phone}
status=$?
if [ "$mode" = smtp ]; then
  sent=$(grep -c "Mail sent to" "$out/platform-accounts-$mode.log")
  echo "== the platform sent $sent messages through STARTTLS to aiosmtpd"
  grep -E "WARN|ERROR" "$out/platform-accounts-$mode.log" | grep -i mail && status=1
fi
[ $status -eq 0 ] || fail "accounts_test.js"
