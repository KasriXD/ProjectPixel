# ADR 0034: Our own mixer on SDL's audio device; the platform stores sounds as QOA

- Status: Accepted
- Date: 2026-11-07

## Context

Milestone 10 gives games sound effects and music. Sounds come from creators
(any WAV or Ogg file they have) and reach players' devices through the
platform, so the decoders on players' devices read files from strangers.
Sound must work the same in px-player (desktop, phones, the browser),
Studio's play mode, multiplayer clients and tests without a speaker, and it
must not affect the simulation, which stays deterministic.

## Decision

1. **The engine mixes, SDL only plays.** `px::audio::Mixer` mixes up to 32
   voices (effects and music buses, pan, pitch by resampling, fades, a soft
   limiter) into stereo floats; `platform::SdlAudio` hands it to SDL3's
   audio stream callback. Without a device (servers, CI, a blocked browser)
   the game plays silently. Tests and `px-headless --audio-out` render the
   same mixer offline.
2. **Sounds are events and state of the game, decided by authority
   scripts.** A tick's sounds (`Sound.play`, `entity:playSound`, ...) are
   reported by the session; music and AudioSource components are state.
   `GameAudio` turns them into voices for whoever shows the game. In
   multiplayer the server sends each player their sounds in a reliable
   message (protocol 4) and clients play them when their view reaches that
   tick, the own character's at once.
3. **Formats: WAV, Ogg Vorbis and QOA**, decoded by our WAV reader,
   stb_vorbis and our own bounds-checked QOA decoder (64-bit arithmetic,
   checks on every frame header), with limits checked before decoding
   (8-96 kHz, mono or stereo, 10 minutes). Short sounds are decoded once;
   long ones stream.
4. **The platform stores every sound as QOA it wrote**: the upload check
   decodes each sound and encodes it again with qoa.h, as it already does
   for images (PNG). Players then only decode QOA files written by the
   engine; the Vorbis and WAV decoders only see files on creators' own
   computers and in the upload check (which runs sandboxed, ADR 0031).

## Alternatives considered

- **SDL_mixer or miniaudio**: complete libraries, but they bring decoders
  we do not want on players' devices (MP3, FLAC, MOD) or their own device
  layers next to SDL's, and do not know about entities, players or ticks.
  The mixer is a few hundred lines with tests.
- **Ship Ogg Vorbis to players**: several times smaller than QOA, but stb_vorbis
  is a large decoder of a complex format, and its history has fuzzing
  findings. QOA (3.2 bits per sample) decodes with a few lines of integer
  arithmetic that are easy to check. Packages stay under their size limit
  for the amounts of sound games use; the limit of 60 minutes of sound per
  game keeps the check quick.
- **Sounds as replicated components only** (no events): one-shot sounds
  would need an entity each and arrive with snapshots (unreliable, every
  other tick). Events in their own reliable message are simpler and exact.

## Consequences

- Games sound the same everywhere the engine runs, and sound is testable
  without hardware (CTest mixes the example playthroughs and checks they
  are not silent).
- Published packages with long music grow: about 1 MB per minute of mono
  44.1 kHz sound.
- No effects (reverb, filters) and no 3D sound: later work if games need it.
- Browsers only start sound after the player's first input; phones stop it
  in the background (docs/audio.md).
