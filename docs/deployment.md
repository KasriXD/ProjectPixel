# Running a platform on the internet

This guide puts a platform on a server where anyone can reach it: HTTPS
with a certificate that renews itself, game servers players connect to from
anywhere, email, backups and monitoring
([ADR 0033](adr/0033-deployment-and-operations.md)). For a platform on your
own computer, [platform.md](platform.md) is enough.

What you need: a Linux server (2 processors and 4 GB of memory carry a few
dozen players; each game server is a process with up to 1 GiB), a DNS name
pointing at it, and these ports open in its firewall:

| Port | For |
|---|---|
| 443/tcp | the platform: its API (apps, Studio), the `/play` page and the browser player |
| 80/tcp | Let's Encrypt checking the name when it issues and renews the certificate |
| 27960-27979/udp | game servers, for the apps |
| 27960-27979/tcp | the same game servers, for browsers (secure WebSockets) |

## With Docker Compose

```sh
git clone https://github.com/KasriXD/ProjectPixel.git && cd ProjectPixel
cat > deploy/.env <<'EOF'
PIXEL_DOMAIN=play.example.com
PIXEL_EMAIL=you@example.com
PIXEL_NAME=My Pixel Platform
PX_METRICS_TOKEN=a-long-random-string
EOF
docker compose -f deploy/compose.yaml up -d --build
docker compose -f deploy/compose.yaml logs -f platform
```

The build takes a while (the platform, then Emscripten and the browser
player). [deploy/compose.yaml](../deploy/compose.yaml) runs two containers:

- **certbot** gets a Let's Encrypt certificate for `PIXEL_DOMAIN` (on port
  80), then looks twice a day whether it is due and renews it, copying it to
  a volume the platform reads;
- **platform** runs the image built from the [Dockerfile](../Dockerfile),
  with HTTPS on port 443 and 20 game servers at most on ports 27960-27979.
  Until the first certificate exists it cannot start HTTPS and is restarted
  (a minute or so the first time).

Then make yourself an administrator (after creating your account in the
launcher or at `https://play.example.com/play`):

```sh
docker compose -f deploy/compose.yaml exec platform px-platform --data /data --make-admin YourName
```

and point Studio and the player at `https://play.example.com`.

To change options, edit the `command:` list in the compose file (any
[platform option](platform.md#running-a-platform) works; `--data`, `--bind`
and `--port` are set by the image). Email: replace `--mail-dir` with the
`--smtp` lines shown there and put `PX_SMTP_PASSWORD` in `deploy/.env`
([accounts.md](accounts.md#email)).

### The image by itself

```sh
docker build -t pixel-platform .                          # without the browser player
docker build --target platform-web -t pixel-platform .    # with it
docker run -d --name pixel -v pixel-data:/data -p 8730:8730 \
  -p 27960-27979:27960-27979/udp -p 27960-27979:27960-27979/tcp \
  pixel-platform --public-host play.example.com --server-ports 27960:20
```

The image runs `px-platform --data /data --bind 0.0.0.0 --port 8730` as
user 10001; the arguments after the image name are added. Everything the
platform keeps is in `/data`: the database, the platform's key, packages,
pictures, game server logs and backups. Docker's health check runs
`px-platform --check-health` (`/healthz` on port 8730).

Game servers are processes inside the platform's container, so the
container's memory limit is shared by all of them (the compose file sets
8 GiB). They still lock themselves down inside it
([security.md](security.md#game-servers)); Docker's default seccomp profile
allows that.

## Without containers

Build on the server (or on a computer with the same Linux distribution):

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPX_BUILD_CLIENT=OFF -DPX_BUILD_TESTS=OFF
cmake --build build/release --target px_platform px_server px_headless
sudo install -d /opt/pixel
sudo install build/release/services/platform/px-platform build/release/apps/server/px-server \
  build/release/apps/headless/px-headless /opt/pixel/
```

For the browser player, build the `wasm` preset with Emscripten
([platform.md](platform.md#browsers)) and copy `px-player.js`, `.wasm` and
`.data` to `/opt/pixel/web-player/`.

Then use the systemd unit [deploy/systemd/pixel-platform.service](../deploy/systemd/pixel-platform.service);
its comments show the user to create and the `/etc/pixel/platform.env` file
with the options and secrets. It keeps the data in `/var/lib/pixel`, lets
the platform bind port 443 without root, and locks the service down
(read-only system, no new privileges, private `/tmp`, ...).

Certificates: with certbot on the server, a deploy hook copies the renewed
files where the platform's user can read them:

```sh
sudo install -d -m 700 -o pixel /etc/pixel/certs
sudo certbot certonly --standalone -d play.example.com \
  --deploy-hook 'install -m 600 -o pixel /etc/letsencrypt/live/play.example.com/privkey.pem /etc/pixel/certs/privkey.pem &&
                 install -m 644 -o pixel /etc/letsencrypt/live/play.example.com/fullchain.pem /etc/pixel/certs/fullchain.pem'
```

## Certificates

The platform serves HTTPS itself (`--https-port`, `--tls-cert`,
`--tls-key`) and gives the same certificate to the game servers it starts,
for browsers' secure WebSockets. **Renewals need no restart**: every minute
the platform looks at the two files and, when they changed and the key
belongs to the certificate, takes them into use for new connections (the
log says so, and `px_tls_certificate_reloads_total` counts it). A
certificate written before its key is tried again a minute later. Game
servers started afterwards use the new files; running ones keep the old
certificate until they stop (at the latest when their last player leaves).

`px_tls_certificate_expiry_timestamp_seconds` in `/metrics` is when the
certificate in use expires: alert when it is less than two weeks away.

## Behind a reverse proxy

If nginx, Caddy or a load balancer sits in front of the platform's HTTP
API, tell the platform which addresses are proxies, so that rate limits
count clients and not the proxy:

```sh
px-platform --trusted-proxy 127.0.0.1,10.0.0.0/8 ...
```

For a request from one of those addresses, the client is the right-most
address in `X-Forwarded-For` that is not a trusted proxy (what clients put
there themselves is ignored). Game servers cannot go through an HTTP
proxy: their UDP and TCP ports must be reachable directly, and for
browsers on an HTTPS page they need the certificate (`--tls-cert`) for
`wss://`. Keep `/metrics` away from the proxy (see below).

## Backups

The platform makes a backup **every 24 hours** while it runs (the first a
minute after starting, unless a recent one exists) and keeps the newest
seven, in `<data>/backups` (`--backup-hours`, `--backup-keep`,
`--backup-dir`). A backup is a folder:

```
backups/20261107-031500/
  platform.db       the database, copied while the platform kept working
  platform.key      the platform's signing key (secret: the folder is readable only by the platform's user)
  packages/         every game package the database refers to
  thumbnails/       every game picture it refers to
  backup.json       when, which versions, how many files and bytes, anything missing
```

Packages and pictures are hard links when the backup folder is on the same
disk (they never change, so this costs no space) and copies otherwise. On
the same disk a backup protects against mistakes and broken databases, not
against losing the disk: **copy the backups elsewhere**, for example every
night with

```sh
rsync -a --delete /var/lib/docker/volumes/pixel_pixel-data/_data/backups/ backup-host:pixel-backups/
```

(or `restic`, `borg`, a cloud bucket). Also on demand:

```sh
curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" https://play.example.com/api/v1/admin/backups   # an administrator
docker compose -f deploy/compose.yaml exec platform px-platform --data /data --backup
```

**Restoring** goes into an empty data folder (a new volume), checks the
database and that every package and picture it needs is there, then you
start the platform on it:

```sh
docker volume create pixel-restored
docker run --rm -v pixel_pixel-data:/old:ro -v pixel-restored:/data pixel-platform --restore /old/backups/20261107-031500
# then use pixel-restored as the platform's /data volume
px-platform --data /var/lib/pixel-restored --restore /path/to/backups/20261107-031500   # without containers
```

Accounts, sessions, games and their files, saved data, scores and friends
come back as they were at the backup; the platform key too, so tickets and
logins keep working. Game servers are not in a backup (they start again on
demand). A restored platform with an older schema is upgraded when it
starts.

## Monitoring

**`GET /healthz`** answers 200 `{"status": "ok", "version", "uptimeSeconds",
"gameServers", "lastBackup"}` when the platform and its database work, 503
otherwise. It needs no login: use it for load balancers and uptime checks.

**`GET /metrics`** is in Prometheus' text format. It answers requests from
the same computer that did not come through a proxy, or anyone with
`Authorization: Bearer <PX_METRICS_TOKEN>` when that environment variable
is set:

```yaml
# prometheus.yml
scrape_configs:
  - job_name: pixel
    scheme: http
    static_configs: [{targets: ["play.example.com:8730"]}]   # or 127.0.0.1:8730 on the server
    authorization: {credentials: a-long-random-string}
```

| Metric | |
|---|---|
| `px_build_info{version}`, `px_start_time_seconds` | which version, since when |
| `px_http_requests_total{code="2xx"...}`, `px_http_request_seconds_total` | requests answered by status class; time spent |
| `px_rate_limited_total` | requests refused by a rate limit |
| `px_accounts`, `px_accounts_confirmed_email`, `px_sessions` | accounts (not deleted), with a confirmed address; login sessions |
| `px_games`, `px_game_versions`, `px_reports_open` | published games and versions; reports waiting for a moderator |
| `px_database_bytes` | the database file |
| `px_game_servers{state}`, `px_players` | game servers starting and ready; players on them |
| `px_game_servers_started_total`, `px_game_servers_ended_total{how}` | how they ended: `finished`, `stopped`, `failed`, `exited`, `cpu_limit` |
| `px_mail_sent_total`, `px_mail_failed_total`, `px_mail_queued` | email |
| `px_backups_total`, `px_backup_failures_total`, `px_backup_last_success_timestamp_seconds`, `px_backup_last_seconds`, `px_backup_last_bytes` | backups made by this process |
| `px_tls_certificate_expiry_timestamp_seconds`, `px_tls_certificate_reloads_total`, `px_tls_certificate_reload_failures_total` | the certificate in use |
| `process_cpu_seconds_total`, `process_resident_memory_bytes`, `process_open_fds`, ... | the platform process (not its game servers) |

Alerts worth having: `/healthz` failing; `time() - px_backup_last_success_timestamp_seconds > 2 * 86400`;
`px_tls_certificate_expiry_timestamp_seconds - time() < 14 * 86400`;
`rate(px_game_servers_ended_total{how=~"failed|exited"}[1h]) > 0`;
`increase(px_mail_failed_total[1h]) > 0`; `rate(px_http_requests_total{code="5xx"}[5m]) > 0`.

Logs: the platform logs to standard output (`docker compose logs platform`,
`journalctl -u pixel-platform`); each game server writes
`<data>/logs/server-<session>.log`.

## Upgrading

Make a backup, then rebuild and restart (`git pull`, then
`docker compose -f deploy/compose.yaml up -d --build`). The platform
upgrades its database when it starts; players with an older player are
told to update when they join a game (protocol version 3 since 0.9.0).

## Checking an image

[tools/deploy/container_check.sh](../tools/deploy/container_check.sh)
`IMAGE OUT_DIR` runs an image the way a server would and plays through it:
HTTPS with a development certificate, a game published (checked by the
sandboxed checker inside the container) and played to the end by a bot
from another container over the published UDP port, `/metrics` with a
token, Docker's health check, a backup restored into a new volume and
started, and a renewed certificate taken into use without a restart. CI
runs it on every change (*Container image*), and builds the image with the
browser player too.
