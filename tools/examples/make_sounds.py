#!/usr/bin/env python3
"""Makes the example games' sounds (docs/audio.md#the-examples).

Every sound is computed here, so the examples carry no recordings with
licences of their own. Effects are written as 16-bit mono WAV; the music is
written as WAV and turned into Ogg Vorbis with ffmpeg (libvorbis).

    python3 tools/examples/make_sounds.py            # writes examples/*/sounds
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 22050
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def write_wav(path, samples, rate=RATE):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767)))) for s in samples))


def square(phase, duty=0.5):
    return 1.0 if (phase % 1.0) < duty else -1.0


def triangle(phase):
    p = phase % 1.0
    return 4.0 * p - 1.0 if p < 0.5 else 3.0 - 4.0 * p


def envelope(t, length, attack=0.005, release=0.05):
    if t < attack:
        return t / attack
    if t > length - release:
        return max(0.0, (length - t) / release)
    return 1.0


def tone(freqs, length, wave_fn=square, volume=0.4, release=0.05):
    """Notes one after another: [(frequency, seconds), ...] with a pitch slide inside each."""
    out = []
    for f0, f1, seconds in freqs:
        n = int(seconds * RATE)
        phase = 0.0
        for i in range(n):
            t = i / RATE
            f = f0 + (f1 - f0) * (i / max(1, n - 1))
            phase += f / RATE
            out.append(wave_fn(phase) * volume * envelope(t, seconds, release=min(release, seconds / 2)))
    return out


def coin():
    # Two quick rising notes (B5, E6).
    return tone([(988, 988, 0.06), (1319, 1319, 0.22)], 0.28, lambda p: square(p, 0.5), 0.3, release=0.15)


def jump():
    # A rising sweep.
    return tone([(330, 880, 0.16)], 0.16, lambda p: square(p, 0.25), 0.28, release=0.06)


def key():
    # A bright arpeggio.
    notes = [523, 659, 784, 1047]
    return tone([(f, f, 0.07) for f in notes[:-1]] + [(notes[-1], notes[-1], 0.25)], 0.46, triangle, 0.32, release=0.12)


def door():
    # A low rumble with noise: a block sliding away.
    rnd = random.Random(7)
    out = []
    n = int(0.35 * RATE)
    phase = 0.0
    for i in range(n):
        t = i / RATE
        phase += (110 - 60 * t) / RATE
        noise = rnd.uniform(-1, 1)
        out.append((0.2 * square(phase, 0.5) + 0.12 * noise) * envelope(t, 0.35, release=0.2))
    return out


def complete():
    # A little fanfare: C E G C' (held).
    seq = [(523, 0.1), (659, 0.1), (784, 0.1), (1047, 0.45)]
    out = []
    for f, s in seq:
        out += tone([(f, f, s)], s, lambda p: square(p, 0.5), 0.3, release=0.08)
    return out


NOTE = {"C": -9, "D": -7, "E": -5, "F": -4, "G": -2, "A": 0, "B": 2}


def freq(name):
    """'A4' -> 440 Hz; '-' is a rest."""
    if name == "-":
        return 0.0
    letter, octave = name[0], int(name[-1])
    semis = NOTE[letter] + (1 if "#" in name else 0) + 12 * (octave - 4)
    return 440.0 * 2 ** (semis / 12)


def theme(melody, bass, bpm):
    """A two-voice chiptune loop: square melody, triangle bass, eighth notes."""
    step = 60.0 / bpm / 2  # an eighth note
    n = int(step * RATE)
    out = [0.0] * (n * len(melody))
    for voice, notes, fn, vol in ((0, melody, lambda p: square(p, 0.25), 0.16), (1, bass, triangle, 0.3)):
        for k, name in enumerate(notes):
            f = freq(name)
            if f == 0.0:
                continue
            phase = 0.0
            for i in range(n):
                t = i / RATE
                phase += f / RATE
                out[k * n + i] += fn(phase) * vol * envelope(t, step, attack=0.003, release=step * 0.4)
    return out


PLATFORMER_MELODY = ("E5 G5 A5 G5 E5 D5 C5 D5  E5 G5 A5 C6 B5 A5 G5 -   "
                     "F5 A5 B5 A5 F5 E5 D5 E5  G5 E5 D5 C5 D5 - C5 -").split()
PLATFORMER_BASS = ("C3 C3 G3 G3 C3 C3 G3 G3  A2 A2 E3 E3 A2 A2 E3 E3  "
                   "F2 F2 C3 C3 F2 F2 C3 C3  G2 G2 D3 D3 G2 G2 B2 B2").split()
TOPDOWN_MELODY = ("A4 C5 E5 C5 A4 C5 E5 D5  G4 B4 D5 B4 G4 B4 D5 C5  "
                  "F4 A4 C5 A4 F4 A4 C5 B4  E4 G#4 B4 G#4 E4 - E5 -").split()
TOPDOWN_BASS = ("A2 - A2 - E3 - E3 -  G2 - G2 - D3 - D3 -  "
                "F2 - F2 - C3 - C3 -  E2 - E2 - B2 - E2 -").split()


def ogg(samples, path, repeats):
    """Writes `samples` repeated `repeats` times as Ogg Vorbis (a seamless loop)."""
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "music.wav")
        write_wav(wav, samples * repeats)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", wav, "-c:a", "libvorbis", "-q:a", "2", path],
                       check=True)


def main():
    plat = os.path.join(ROOT, "examples", "platformer", "sounds")
    top = os.path.join(ROOT, "examples", "topdown", "sounds")
    write_wav(os.path.join(plat, "coin.wav"), coin())
    write_wav(os.path.join(plat, "jump.wav"), jump())
    write_wav(os.path.join(plat, "complete.wav"), complete())
    write_wav(os.path.join(top, "coin.wav"), coin())
    write_wav(os.path.join(top, "key.wav"), key())
    write_wav(os.path.join(top, "door.wav"), door())
    write_wav(os.path.join(top, "complete.wav"), complete())
    ogg(theme(PLATFORMER_MELODY, PLATFORMER_BASS, 132), os.path.join(plat, "theme.ogg"), 2)
    ogg(theme(TOPDOWN_MELODY, TOPDOWN_BASS, 112), os.path.join(top, "theme.ogg"), 2)
    for folder in (plat, top):
        for name in sorted(os.listdir(folder)):
            print(os.path.join(os.path.relpath(folder, ROOT), name), os.path.getsize(os.path.join(folder, name)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
