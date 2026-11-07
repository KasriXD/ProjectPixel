# ADR 0031: Game servers and upload checks lock themselves down

- Status: Accepted
- Date: 2026-11-07

## Context

Every game on the platform is code written by someone else, run on the
platform's machine: Luau scripts in a game server (`px-server`) and, when a
game or a picture is uploaded, a check in `px-headless`. The script sandbox
(ADR 0008, 0010) is the first wall; until now the second was only a memory
limit and a separate process running as the platform's own user, with its
environment, its files (the database, the platform's signing key) and the
whole network in reach. A bug in Luau, the engine or an image decoder would
have handed all of that to an attacker. Milestone 9 asks for game servers
isolated with CPU and memory limits before the platform goes on the
internet.

Requirements: works where the platform runs today (a plain Linux server, a
container, a developer's Windows or macOS computer); no root, no daemon,
no new dependency; each server still opens its own ports and talks to the
platform; tests must show the walls hold.

## Decision

1. **The program locks itself in** after reading what it needs and before
   it runs any game code or starts a thread (`core/Sandbox.cpp`, Linux):
   - **seccomp-bpf** refuses starting programs (`execve`), new processes
     (`fork`, `clone` without a thread), debugging other processes,
     mounting, namespaces, kernel modules and keys, socket families other
     than UNIX/IP/netlink, and signals to anything but itself.
   - **Landlock** (when the kernel has it) makes listed files read-only and
     everything else invisible, nothing writable; with ABI 4 it limits TCP
     to the server's own ports and the platform's, with ABI 6 it stops
     signals and abstract sockets leaving the process.
   The report of what was applied goes to the log ("Sandbox: system calls
   filtered; Landlock 7: ...").
2. **The platform starts them small**: a clean environment (an allowlist:
   `PATH`, `TZ`, the certificate variables, the server's own key), no core
   dumps, at most 1024 open files and 256 MiB per file written, one process
   (Windows: a job object with one process, UI restrictions and no error
   dialogs), the memory limit as before.
3. **Processor time is watched**: a server that keeps more than its share
   of a processor (default 100 %) busy in every sample for 30 seconds is
   stopped (`CpuWatch` in `Fleet.cpp`); the share is `--server-cpu`.
4. **The same for checks**: `--check-package` and `--check-thumbnail` read
   their input, open their output, then enter the sandbox before decoding
   anything.
5. **Off switch for debugging**: `px-platform --no-sandbox`.

## Alternatives considered

- **Containers per game server** (Docker, Podman, bubblewrap, nsjail):
  the strongest separation, but a daemon or setuid helper, root or user
  namespaces (often disabled), and nothing equivalent on Windows or macOS
  for local development. The platform itself can run in a container
  (ADR 0033); the walls here apply inside it too.
- **A separate user per game server**: needs root to switch users, and
  does not stop network access or reading world-readable files.
- **Only rlimits** (CPU seconds, address space): `RLIMIT_CPU` kills a
  long-running server that is merely busy over hours; a sampled share
  matches what "using too much" means for a real-time game.

## Consequences

- A compromised game server cannot read the database or the platform key,
  start a shell, connect anywhere but the platform, or signal other
  processes (tested in `test_sandbox.cpp`; the CI capture and the container
  check show it working on GitHub's kernel and inside Docker).
- Windows and macOS get the process limits but not the system call and
  file walls; production deployments are expected on Linux.
- Kernels without Landlock (before 5.13) keep the system call filter only.
- New features in game servers that need files or system calls must be
  added to the rules; the sanitizer builds allow `ptrace` and `/proc`.
