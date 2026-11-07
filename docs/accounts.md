# Accounts

Beyond a name and a password, an account on a platform can have an email
address, change or reset its password, download what the platform keeps
about it, and delete itself. All of it works on the `/play` page, in the
player's launcher (desktop and phones) and through `PlatformClient`
([ADR 0032](adr/0032-accounts-and-email.md)).

## For players

**Email address** (*You > Account* in the launcher and on the page): type
the address and your password; the platform sends a link that confirms it.
The link works once, for 24 hours. A confirmed address is what makes a
forgotten password recoverable; it is never shown to other players.
Removing the address (save it empty) works the same way.

**Forgot your password?** On the login page, give your name or your email
address. If the account has a confirmed address, a link to choose a new
password is sent there (one hour, once). The page always answers the same,
so nobody can find out from it who has an account. Choosing a new password
logs the account out everywhere.

**Changing the password** needs the current one; you stay logged in where
you changed it and are logged out everywhere else.

**Your data**: *Download your data* on the page gives a JSON file with
everything the platform keeps about the account: the account itself
(including the email address), sessions, friends and requests, blocks,
games, saved progress in each game, scores and reports you filed. The
launcher points to the page for it.

**Deleting the account** (with your password) removes, for good: your
games (their versions, saved data, scores, invitations and reports about
them; their running servers stop), your friends, requests, blocks,
invitations, saved progress and scores in other games, and your sessions.
What stays is an empty record named `deleted-<number>` so that moderation
history still makes sense; the name you had is free again. Administrators
have to give up the role first.

## For operators

### Email

Without a mail server the platform cannot send links: email addresses and
password resets are then unavailable (the page and the launcher say so),
and an operator can still help someone locked out:

```sh
px-platform --data platform-data --reset-link ada   # prints a link that works for an hour
```

To send email, give the platform an SMTP server:

```sh
PX_SMTP_PASSWORD=... px-platform --smtp smtps://smtp.example.com:465 --smtp-user pixel@example.com \
    --mail-from "Pixel <noreply@play.example.com>" --public-url https://play.example.com
```

| Option | |
|---|---|
| `--smtp smtps://HOST[:465]` | TLS from the start |
| `--smtp smtp://HOST[:587]` | STARTTLS, required: a server that does not offer it is refused |
| `--smtp smtp+plain://HOST[:25]` | no encryption: only for a relay on the same computer; never with a password |
| `--smtp-user NAME` | log in (AUTH PLAIN, only over TLS); the password comes from `PX_SMTP_PASSWORD` |
| `--smtp-ca FILE` | trusted certificates for the mail server (default: the system's) |
| `--mail-from ADDRESS` | the sender, `Name <address>` or an address (needed with `--smtp`) |
| `--mail-dir DIR` | instead of a server: every message becomes an `.eml` file in DIR (development, tests) |
| `--public-url URL` | where links point (`URL/play#verify=...`); default the HTTPS address, else HTTP |
| `--require-verified-email` | creating and publishing games needs a confirmed address |

The mail server's certificate and name are checked. Messages wait in a
queue and are sent by a worker thread, three attempts each (30 s, then
60 s apart); failures are logged and counted (`px_mail_failed_total` in
[/metrics](deployment.md#monitoring)). Messages are plain text, in English,
with `Auto-Submitted: auto-generated`.

Any mail service that accepts SMTP works (your provider, Amazon SES,
Postmark, Mailgun, a self-hosted Postfix). For delivery to big mailbox
providers, the sending domain needs SPF and DKIM records; see the service's
own guide.

### What is stored

Schema version 3 adds `users.email`, `email_verified` and `deleted`, and
`account_tokens` (the hashes of link tokens, their purpose, the address
they were sent to and when they expire). Tokens are 256-bit random values;
only their BLAKE2b hashes are stored, one per account and purpose, deleted
when used. Expired ones are removed every hour. Every reset and deletion is
in the audit log.

## HTTP API

Under `/api/v1` ([platform.md](platform.md#http-api)):

| Method and path | Auth | |
|---|---|---|
| `GET /me/account` | Bearer | `{email, emailVerified, mail, emailRequiredToPublish}` |
| `PUT /me/email` | Bearer | `{email, password}`: set (empty: remove) and send a link; 5 an hour |
| `POST /me/email/verification` | Bearer | the link again |
| `POST /email-verifications` | | `{token}` from the link → `{username, email}` |
| `POST /password-resets` | | `{name}` (a name or an address) → 202, always the same answer; 10 an hour per address, 3 per name |
| `POST /password-resets/complete` | | `{token, password}` → `{username}`; every session ends |
| `PUT /me/password` | Bearer | `{current, password}`; other sessions end |
| `DELETE /me` | Bearer | `{password}` → `{status, games}` |
| `GET /me/export` | Bearer | everything about the account (JSON, as a download) |

`GET /platform` says `"mail": true` when the platform sends email.

## Testing

| Test | |
|---|---|
| `unit_tests` (`test_accounts.cpp`) | message format and header injection; a fake SMTP server with STARTTLS, TLS from the start, an untrusted certificate, no STARTTLS (the password is never sent), a plain relay; the queue giving up; confirming addresses, resets, changes, admin links, publishing that needs a confirmed address; downloading and deleting an account |
| `tools/browser/accounts_test.sh BUILD OUT files\|smtp` | the `/play` page in headless Chromium, without a browser player: an account, its address confirmed from the mail, a password change, the download, a reset from the mail, deleting it. `smtp` sends the mail through a real SMTP server (aiosmtpd, insisting on STARTTLS) with a development certificate (CI) |
| `tools/launcher/launcher_check.sh` | the launcher's account page (`--open account`) |
