#!/usr/bin/env python3
"""Proves game traffic is encrypted on the wire (docs/multiplayer.md#encryption).

Starts px-server on 127.0.0.1, captures every UDP packet to and from its port
on the loopback interface (a raw socket: Linux, root or CAP_NET_RAW), lets a
px-headless bot with a distinctive name play the level, and checks that the
packets carry neither the bot's name nor the server's game name, which both
cross the network in the game's Hello and Welcome messages. A plain datagram
with the name is sent first, to show the capture would see it.

    sudo tools/net/capture_check.py BUILD_DIR PROJECT_DIR [INPUT_SCRIPT]
"""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

NAME = "Zebracorn"


def main():
    build, project = sys.argv[1], sys.argv[2]
    script = sys.argv[3] if len(sys.argv) > 3 else None
    port = 27790
    server_exe = os.path.join(build, "apps/server/px-server")
    headless_exe = os.path.join(build, "apps/headless/px-headless")

    sniffer = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
    sniffer.bind(("lo", 0))
    sniffer.settimeout(0.2)
    payloads = []
    stop = threading.Event()

    def capture():
        while not stop.is_set():
            try:
                frame = sniffer.recv(65535)
            except socket.timeout:
                continue
            if len(frame) < 14 + 20 + 8 or struct.unpack("!H", frame[12:14])[0] != 0x0800:
                continue
            ip = frame[14:]
            header = (ip[0] & 0x0F) * 4
            if ip[9] != 17:  # UDP
                continue
            src, dst = struct.unpack("!HH", ip[header:header + 4])
            if port in (src, dst):
                payloads.append(bytes(ip[header + 8:]))

    thread = threading.Thread(target=capture, daemon=True)
    thread.start()

    server = subprocess.Popen([server_exe, project, "--port", str(port), "--no-websocket", "--ticks", "1500"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.7)
    # The control: a plain datagram with the name (the server ignores it).
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.sendto(b"control " + NAME.encode(), ("127.0.0.1", port))
    time.sleep(0.2)
    control_seen = any(NAME.encode() in p for p in payloads)
    payloads.clear()

    bot = [headless_exe, project, "--connect", f"127.0.0.1:{port}", "--name", NAME, "--expect-joined"]
    if script:
        bot += ["--input-script", script]
    else:
        bot += ["--ticks", "300"]
    result = subprocess.run(bot, capture_output=True, text=True, timeout=120)
    server.terminate()
    out, _ = server.communicate(timeout=10)
    time.sleep(0.3)
    stop.set()
    thread.join()

    game_name = None
    for line in out.splitlines():
        if "server " in line and "'" in line:
            game_name = line.split("'")[1]
            break
    total = sum(len(p) for p in payloads)
    leaks = [w for w in (NAME, game_name or "") if w and any(w.encode() in p for p in payloads)]
    print(f"captured {len(payloads)} packets, {total} bytes; bot exit {result.returncode}; "
          f"control seen: {control_seen}; game name: {game_name!r}")
    print(result.stdout.strip().splitlines()[-1] if result.stdout.strip() else result.stderr.strip()[-500:])
    if not control_seen:
        print("FAILED: the capture did not see the plain control datagram")
        return 1
    if result.returncode != 0 or len(payloads) < 100:
        print("FAILED: the bot did not play")
        return 1
    if not game_name:
        print("FAILED: no game name in the server's log")
        return 1
    if leaks:
        print(f"FAILED: readable on the wire: {leaks}")
        return 1
    print(f"PASSED: {len(payloads)} packets of a real game, neither '{NAME}' nor '{game_name}' readable on the wire")
    return 0


if __name__ == "__main__":
    sys.exit(main())
