# ADR 0033: One container image; the platform does its own TLS, backups and metrics

- Status: Accepted
- Date: 2026-11-07

## Context

Until Milestone 9 the platform ran from a build folder on a developer's
computer. Putting it on the internet needs a way to install it, a TLS
certificate that renews itself, backups that can actually be restored,
and numbers to watch. The platform is one process that starts game servers
as child processes on a range of ports (UDP for apps, TCP WebSockets for
browsers) and keeps everything in one data folder (SQLite, packages,
pictures, its signing key).

## Decision

1. **One container image** (`Dockerfile`, Ubuntu 24.04): the platform with
   `px-server` and `px-headless` next to it, built from source in the
   image build, running as an unprivileged user (uid 10001) with `/data` as
   a volume. Game servers run as processes inside the same container (they
   still lock themselves down, ADR 0031). A second target adds the
   WebAssembly player. `deploy/compose.yaml` runs it with certbot; a
   systemd unit covers installs without containers.
2. **The platform terminates TLS itself** (as since ADR 0025) and takes a
   renewed certificate into use without restarting: it looks at the files
   every minute and swaps them in with cpp-httplib's `update_certs_pem`
   once the new certificate and key belong together. Game servers started
   afterwards read the new files. A reverse proxy in front stays possible:
   `--trusted-proxy` makes the platform believe its `X-Forwarded-For` for
   rate limits.
3. **Backups are made by the platform**, online: SQLite's backup API copies
   the database a few pages at a time while requests continue; then the
   packages and pictures that copy refers to are hard-linked (or copied)
   next to it with the platform key and a `backup.json`. A backup is a
   self-contained folder, made daily by default, on demand by an
   administrator (`POST /api/v1/admin/backups`) or from the command line,
   with the newest seven kept. Removing files waits while a backup collects
   them. `px-platform --restore` puts one into an empty data folder and
   checks it (`integrity_check`, every referenced file present).
4. **Monitoring over HTTP**: `/healthz` (the database answers) for load
   balancers and Docker's health check (`px-platform --check-health`, so
   the image needs no curl); `/metrics` in Prometheus' text format —
   requests by status, rate limiting, accounts, games, game servers and
   players, how game servers ended, mail, backups, the certificate's expiry
   and the process. `/metrics` answers only on the same computer (not
   through a proxy) unless `PX_METRICS_TOKEN` is set.

## Alternatives considered

- **A reverse proxy for TLS** (Caddy, nginx): automatic certificates, but
  game servers need the certificate too for `wss://` on their own ports,
  which a proxy cannot cover without routing every game port through it.
- **One container per game server**: see ADR 0031; it needs access to the
  Docker daemon from the platform, which is root on the host.
- **Backups with `sqlite3 .backup` or file copies by cron**: copying the
  database file while it is written can give a broken copy, and the
  packages must match the database; doing it inside the platform gets both
  right and reports it in `/metrics`.
- **OpenTelemetry**: a large dependency; Prometheus' text format is a few
  lines of code and every monitoring system reads it.

## Consequences

- `docker compose up` on a server with a DNS name gives a working platform
  with HTTPS, game servers and the browser player; CI builds the image and
  plays a level through it.
- The database is still one SQLite file on one machine: the platform scales
  up (more game servers per machine) but not out. Backups live next to the
  data until the operator copies them elsewhere (the guide shows how).
- Game servers inside one container share its limits; the compose file
  gives it 8 GiB by default.
