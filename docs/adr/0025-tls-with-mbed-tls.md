# ADR 0025: HTTPS and secure WebSockets with Mbed TLS

- Status: Accepted
- Date: 2026-11-04

## Context

The platform spoke plain HTTP: passwords, session tokens and connect
tickets crossed the network unencrypted, and browsers refuse many features
(and secure WebSockets) to pages that are not HTTPS. Clients (Studio, the
player, bots) need TLS too, on Windows, macOS and Linux, without asking
creators to install anything.

## Decision

1. **Mbed TLS 3.6** (Apache-2.0), fetched and verified by SHA-256 like the
   other dependencies and linked statically. cpp-httplib supports it
   directly (`CPPHTTPLIB_MBEDTLS_SUPPORT`). OpenSSL was not chosen: it is
   not available on Windows by default and is large to build; the operating
   systems' own TLS stacks would need three implementations.
2. **Certificates clients trust**: the system's certificate store (Windows
   store, macOS keychain, Linux CA bundles). `PX_PLATFORM_CA` (or
   `PlatformClient::setCaFile`) adds a certificate, for development
   certificates.
3. **The platform** serves the same API on a second, HTTPS listener
   (`--https-port`, `--tls-cert`, `--tls-key`); game servers on the same
   computer keep using the plain HTTP port. `px-platform --dev-certificate`
   writes a self-signed ECDSA certificate for testing.
4. **Game servers** started by a platform with a certificate offer `wss://`
   for browsers (ADR 0024). UDP game traffic stays unencrypted (planned).
5. `PX_ENABLE_TLS` (default on) can turn it off for builds that do not want
   it.

## Consequences

- About 1 MB more in programs that talk to the platform.
- A real deployment needs a certificate for its public name (for example
  from Let's Encrypt); certificate renewal means restarting px-platform.
- Mbed TLS must be kept up to date for security fixes (dependencies.md).
