#!/usr/bin/env bash
# The platform's container image end to end (docs/deployment.md#checking-an-image).
#
#   tools/deploy/container_check.sh IMAGE OUT_DIR
#
# Starts the image with a data volume, HTTPS (a development certificate) and
# a metrics token; waits until Docker calls it healthy; publishes the
# platformer example and plays it with a bot in another container, over the
# published UDP port (so encrypted game traffic and the sandboxed game server
# work inside the container); reads /metrics; makes a backup over the API,
# restores it into a new volume and starts a second platform from it; then
# renews the certificate and checks the platform took the new one into use.
# Needs Docker and curl. Leaves logs in OUT_DIR.
set -u
image=$1
out=$2
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
name=pixel-check
data=pixel-check-data
restored=pixel-check-restored
http=18730
https=18731
second=18740
token=check-metrics-token
url="http://127.0.0.1:$http"

cleanup() {
  docker logs "$name" > "$out/platform.log" 2>&1
  docker exec "$name" sh -c 'cat /data/logs/*.log' > "$out/game-servers.log" 2>&1
  docker logs "$name-restored" > "$out/restored.log" 2>&1
  docker rm -f "$name" "$name-restored" > /dev/null 2>&1
  docker volume rm -f "$data" "$restored" > /dev/null 2>&1
}
trap cleanup EXIT
fail() {
  echo "FAILED: $*"
  docker logs --tail 30 "$name" 2>&1
  exit 1
}
docker rm -f "$name" "$name-restored" > /dev/null 2>&1
docker volume rm -f "$data" "$restored" > /dev/null 2>&1

# A development certificate in the data volume, then the platform.
docker run --rm -v "$data:/data" "$image" --dev-certificate localhost,127.0.0.1 > "$out/certificate.log" 2>&1 ||
  { cat "$out/certificate.log"; fail "could not make a certificate"; }
docker run -d --name "$name" -v "$data:/data" -e PX_METRICS_TOKEN="$token" \
  -p "$http:8730" -p "$https:8731" -p 27960-27964:27960-27964/udp -p 27960-27964:27960-27964/tcp \
  --add-host host.docker.internal:host-gateway \
  "$image" --server-ports 27960:5 --max-servers 3 --public-host host.docker.internal --name "Pixel Check" \
  --https-port 8731 --tls-cert /data/tls/dev-cert.pem --tls-key /data/tls/dev-key.pem > /dev/null || fail "docker run"
for _ in $(seq 1 100); do
  curl -sf "$url/healthz" > /dev/null && break
  sleep 0.3
done
curl -sf "$url/healthz" | grep -q '"status":"ok"' || fail "no /healthz"
docker cp "$name:/data/tls/dev-cert.pem" "$out/cert.pem" > /dev/null
curl -sf --cacert "$out/cert.pem" "https://localhost:$https/api/v1/platform" | grep -q '"name":"Pixel Check"' ||
  fail "HTTPS does not answer"
echo "== the platform answers over HTTP and HTTPS"

# Publishing and playing, from another container (as players on other computers).
client() {
  docker run --rm --entrypoint px-headless --add-host host.docker.internal:host-gateway \
    -v "$root/examples:/examples:ro" "$image" "$@"
}
published=$(client /examples/platformer --platform "http://host.docker.internal:$http" --user maker \
  --password maker-pass-123 --register --publish --title "Coin Run" 2>&1)
game=$(echo "$published" | sed -n 's/^published game=\([0-9]*\).*/\1/p')
[ -n "$game" ] || { echo "$published"; fail "could not publish (the check runs in the sandbox inside the container)"; }
echo "== published game $game (checked by a sandboxed px-headless in the container)"
client --platform "http://host.docker.internal:$http" --user bot --password bot-pass-123 --register --play "$game" \
  --input-script /examples/platformer/tests/full_run.input --ticks 2000 > "$out/bot.log" 2>&1 ||
  { tail -20 "$out/bot.log"; fail "the bot could not play"; }
grep -q "^bot: .*levelCompleted=true" "$out/bot.log" || { tail -20 "$out/bot.log"; fail "the bot did not finish the level online"; }
grep -q '"username":"bot"' <(curl -sf "$url/api/v1/games/$game/leaderboards") || fail "no score from the bot"
echo "== a bot played the level to the end over UDP through the published port ($(grep -o 'rtt=[0-9]*ms' "$out/bot.log" | head -1))"

# Monitoring.
code=$(curl -s -o /dev/null -w '%{http_code}' "$url/metrics")
[ "$code" = 401 ] || fail "/metrics without the token answered $code"
curl -sf -H "Authorization: Bearer $token" "$url/metrics" > "$out/metrics.txt" || fail "/metrics with the token"
grep -q '^px_accounts 2$' "$out/metrics.txt" || fail "metrics: accounts"
grep -q '^px_game_servers_started_total 1$' "$out/metrics.txt" || fail "metrics: game servers"
grep -q '^px_tls_certificate_expiry_timestamp_seconds ' "$out/metrics.txt" || fail "metrics: certificate"
grep -q '^process_resident_memory_bytes ' "$out/metrics.txt" || fail "metrics: process"
for _ in $(seq 1 60); do
  [ "$(docker inspect -f '{{.State.Health.Status}}' "$name")" = healthy ] && break
  sleep 1
done
[ "$(docker inspect -f '{{.State.Health.Status}}' "$name")" = healthy ] || fail "Docker does not call it healthy"
echo "== /metrics with the token; Docker's health check says healthy"
docker exec "$name" sh -c 'grep -h "Sandbox:" /data/logs/*.log | head -1' | sed 's/^.*INFO  /== game server: /'

# A backup over the API, restored into a new volume, started.
docker exec "$name" px-platform --data /data --make-admin maker > /dev/null 2>&1 || fail "--make-admin"
login=$(curl -sf "$url/api/v1/sessions" -H "Content-Type: application/json" \
  -d '{"username":"maker","password":"maker-pass-123"}' | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
backup=$(curl -sf -X POST "$url/api/v1/admin/backups" -H "Authorization: Bearer $login" |
  sed -n 's/.*"name":"\([^"]*\)".*/\1/p')
[ -n "$backup" ] || fail "no backup"
docker run --rm -v "$data:/old:ro" -v "$restored:/data" "$image" --restore "/old/backups/$backup" > "$out/restore.log" 2>&1 ||
  { cat "$out/restore.log"; fail "restoring $backup"; }
docker run -d --name "$name-restored" -v "$restored:/data" -p "$second:8730" "$image" --server-ports 27970:5 > /dev/null ||
  fail "starting the restored platform"
for _ in $(seq 1 100); do
  curl -sf "http://127.0.0.1:$second/healthz" > /dev/null && break
  sleep 0.3
done
curl -sf "http://127.0.0.1:$second/api/v1/games" | grep -q '"title":"Coin Run"' || fail "the restored platform has no game"
curl -sf "http://127.0.0.1:$second/api/v1/sessions" -H "Content-Type: application/json" \
  -d '{"username":"bot","password":"bot-pass-123"}' > /dev/null || fail "cannot log in to the restored platform"
echo "== backup $backup restored into a new volume; the platform started from it has the game and the accounts"

# A renewed certificate: the platform notices within a minute.
docker exec "$name" sh -c 'px-platform --data /tmp/renew --dev-certificate localhost,127.0.0.1 > /dev/null 2>&1 &&
  cp /tmp/renew/tls/dev-key.pem /data/tls/dev-key.pem && cp /tmp/renew/tls/dev-cert.pem /data/tls/dev-cert.pem' ||
  fail "could not renew the certificate"
docker cp "$name:/data/tls/dev-cert.pem" "$out/renewed.pem" > /dev/null
renewed=
for _ in $(seq 1 90); do
  if curl -sf --cacert "$out/renewed.pem" "https://localhost:$https/healthz" > /dev/null; then renewed=yes; break; fi
  sleep 1
done
[ -n "$renewed" ] || fail "the renewed certificate was not taken into use"
curl -sf -H "Authorization: Bearer $token" "$url/metrics" | grep -q '^px_tls_certificate_reloads_total 1$' ||
  fail "metrics: no reload counted"
echo "== the renewed certificate was taken into use without a restart"
echo "PASSED: image $image"
