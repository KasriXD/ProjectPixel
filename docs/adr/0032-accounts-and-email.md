# ADR 0032: Accounts get email through the platform's own SMTP client

- Status: Accepted
- Date: 2026-11-07

## Context

Accounts had a name and a password, nothing else: a forgotten password
meant a lost account (or an operator editing the database), and there was
no way to delete an account or see what the platform keeps about you.
Players on the internet need all of that, app stores require deleting an
account from inside the app, and privacy law expects the data to be
downloadable and deletable. Email is the usual way to prove an address and
to carry reset links.

## Decision

1. **Email addresses are optional** and confirmed by a one-time link
   (24 hours). Operators can require a confirmed address before publishing
   (`--require-verified-email`).
2. **Password resets by link** (1 hour), sent only to confirmed addresses.
   The request answers the same whether or not an account matched, is
   rate-limited per address and per name, and a reset ends every session of
   the account. Operators can make a link for someone without an address
   (`--reset-link NAME`).
3. **Link tokens** are 256-bit random values stored as BLAKE2b hashes in
   `account_tokens` (schema 3), one per account and purpose, deleted when
   used; expired ones are cleaned up hourly. Links point to the `/play`
   page (`#verify=` / `#reset=`), which takes the token out of the address
   at once.
4. **The platform speaks SMTP itself** (`Mail.cpp`), over Mbed TLS (already
   used for HTTPS): `smtps://` (TLS from the start) or `smtp://` with
   STARTTLS required; certificates and host names are checked; the
   password (`PX_SMTP_PASSWORD`) is sent only encrypted (AUTH PLAIN).
   Messages go out from a queue on a worker thread with retries, so a slow
   mail server never holds up a request. `--mail-dir` writes `.eml` files
   instead, for development and tests.
5. **Deleting an account** keeps its id as a tombstone (`deleted-<id>`,
   no password, no personal data) so the audit log and moderation history
   still make sense; its games, versions, saved data, scores, friends,
   invitations and sessions go, and package and picture files nothing else
   uses are removed. Administrators must give up the role first.
6. **Downloading your data**: `GET /me/export` returns everything the
   platform keeps about the account as JSON.
7. **Every client gets the features**: the `/play` page, the launcher on
   desktop and phones (an app store requirement), and `PlatformClient`.

## Alternatives considered

- **libcurl for SMTP**: a large dependency (and its own TLS choice) for a
  conversation of a dozen commands; Mbed TLS is already built in.
- **An HTTP mail API** (SendGrid, SES, Postmark): one vendor's API in the
  core; most of them also accept SMTP, which every mail service and every
  self-hosted server speaks.
- **A sidecar sending the mail** (the platform writes files, something else
  sends them): one more thing to deploy and watch; kept as the
  `--mail-dir` mode for development.
- **Hard deletion of the user row**: breaks foreign keys from the audit log
  and reports about the account.

## Consequences

- Accounts can be recovered and deleted without an operator.
- One more secret for operators (the SMTP password) and one more thing to
  watch (`px_mail_failed_total`).
- Plain-text messages only (no HTML email), in English.
- The SMTP client has no connect timeout of its own beyond the operating
  system's; a mail server that never answers delays the queue, not requests.
