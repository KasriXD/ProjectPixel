# Sound

Games play sound effects and music. Sounds are assets like images: imported
in Studio, named in scripts, carried in the game's package. This page covers
making sounds play, how they are heard, what happens in multiplayer, and how
it is tested. Text on the screen is in [text.md](text.md); the decision
behind the design is [ADR 0034](adr/0034-audio.md).

## Sounds in a project

Import `.wav`, `.ogg` (Ogg Vorbis) or `.qoa` files in Studio (**Import...**
in the Assets panel, or drag them onto the window). They are copied to the
project's `sounds/` folder and checked first: a file that does not decode is
refused with the reason. Double-click a sound in the Assets panel to listen;
its tooltip shows the format, rate, channels and length.

| | Supported |
|---|---|
| WAV | PCM 8, 16, 24 and 32 bit, 32-bit float, `WAVE_FORMAT_EXTENSIBLE` |
| Ogg Vorbis | any quality |
| QOA | [Quite OK Audio](https://qoaformat.org), as the platform stores sounds |
| Channels | mono or stereo |
| Sample rate | 8 to 96 kHz (mixed at 48 kHz) |
| Length | up to 10 minutes each; 60 minutes for a whole game on the platform |

Sounds up to 15 seconds are decoded once and kept in memory; longer ones
(music) are decoded while they play, so a long song costs little memory.

Scripts name sounds by their asset name, as the Assets panel shows it
(rename it there). Names are not file names: `"Coin"`, not `"sounds/coin.wav"`.

## Playing sounds from scripts

Only authority scripts start sounds (the server in multiplayer, the session
when playing alone): what is heard is decided where the game is decided.

```lua
Sound.play("Coin")                            -- the same everywhere, for everyone
Sound.playAt("Boom", self.entity.position)    -- heard from a place in the world
self.entity:playSound("Jump")                 -- from an entity: follows it while it plays
Sound.playFor(player, "Secret", 0.8)          -- only that player hears it
Sound.playMusic("Theme", 0.6, 2)              -- music for everyone: volume, seconds to fade
Sound.playMusicFor(player, "Boss")            -- this player's own music instead
Sound.playMusicFor(player, nil)               -- back to everyone's music
Sound.stopMusic(1)                            -- fade out over a second
print(Sound.music())                          -- the name of the music playing ("" for none)
```

Every function takes an optional volume (1 plays the sound as recorded, up
to 4) and the effects an optional pitch (0.25 to 4; 2 is an octave up and
twice as fast). An unknown name is an error that suggests the closest name.
At most 32 sounds start per tick; more are an error (and the 32 still play).

Starting music that is already playing only changes its volume. Starting
other music fades the old song out while the new one fades in over the
seconds given (default 1).

## Sound sources

The **AudioSource** component plays a sound from its entity for as long as
`playing` is on: a waterfall, a machine's hum, the music of one room. Studio
makes one when a sound is dropped in the Scene view (**Create > Sound
Source** too).

| Field | Meaning |
|---|---|
| `sound` | the sound asset |
| `volume`, `pitch` | as above (changing the pitch starts the sound again) |
| `loop` | start again at the end (on by default) |
| `playing` | scripts start and stop it with `entity.soundPlaying` |
| `positional` | heard from the entity (on by default) or the same everywhere |
| `range` | positional: silent this far away; 0 means "what is on screen" |

A source that does not loop plays once each time `playing` turns on.

## How sounds are heard

A positional sound (an entity's sound, `playAt`, a positional source) is
placed by where it is relative to the camera:

- With `range` 0 (the default) it plays at full volume while it is on screen
  and fades out over half a screen beyond the edge, so things just off
  screen are still heard and the whole level is not.
- With a `range` it fades with distance and is silent at that distance.
- It is panned to its side: hard left or right never, so it stays in both ears.

Everything else plays in the middle at its own volume.

### How sounds are mixed

The mixer (`engine/audio`, `px::audio::Mixer`) plays up to 32 sounds at
once. When all 32 are busy, the oldest effect that does not loop makes room;
music and loops are never cut off. Effects and music have their own volume
(the *bus*), under the player's overall volume. Volume changes are ramped
over a few milliseconds and stopping always fades for at least 5 ms, so
nothing clicks; a soft limiter keeps loud moments from distorting. The mixer
has no device of its own: the player and Studio feed SDL's audio device
from it, and tests render it directly.

## Multiplayer

Sounds are part of the game state the server sends:

- Each tick's sounds go to each player in a reliable message, without the
  sounds meant for someone else (`playFor`).
- A player hears a sound when their view reaches the tick it was played on
  (the view runs a little behind the server, see
  [multiplayer.md](multiplayer.md#how-it-works)), so a coin is heard when it
  is seen to disappear. Sounds of the player's own character are played
  straight away, because that character is shown ahead of the rest.
- Sounds far older than the view (after a stall) are dropped, not played late.
- Music is part of the game state, everyone's and each player's own, and so
  is every AudioSource: a player who joins late hears what is playing.

## Players' settings

The player's menu (Esc, or the pause button) has **Volume**, **Music** and
**Effects**, in steps of 10%, remembered on the device
([input-devices.md](input-devices.md#settings)). Without a sound device the
menu says so and the game plays silently.

### Browsers and phones

Browsers only let a page make sound after the player has clicked, tapped or
pressed a key on it. The browser player starts its sound at the first such
input (SDL does this); until then the game plays silently, and nothing is
lost but the sounds of those first moments. Phones and tablets stop the
sound while the app is in the background and pause the game behind the
menu. On iOS, game sound follows the silent switch and mixes with music
from other apps.

## In Studio

Sounds play while you test a game (**Play**), from the Scene view's game
camera. Stopping the game fades them out. In the Assets panel,
double-click a sound to listen to it (again to stop), or use its menu.

## On the platform

The upload check ([platform.md](platform.md#publishing)) decodes every sound
with the limits above and stores it again as QOA written by the engine, like
images are stored again as PNG. Players' devices then only ever decode
sounds the platform wrote, in the simplest format, and never a WAV or Vorbis
file from the internet. The check's report counts the sounds and their length.

## Checking sound without a speaker

`px-headless` mixes what a player would hear, tick by tick, with the same
code as the player:

    px-headless examples/platformer --input-script examples/platformer/tests/full_run.input \
        --audio-out run.wav --expect-sound

It prints how many sounds started and the loudest moment; `--audio-out`
writes a 48 kHz stereo WAV file to listen to; `--expect-sound` fails if the
run was silent. The example playthrough tests in CTest use both. Unit tests
check decoding (every format, damaged and hostile files), the mixer (volume,
pan, pitch, fades, voices, limits), placement, streaming, scripts, the
network messages and sounds reaching the right players.

## The examples

The examples' sounds are made by `tools/examples/make_sounds.py`: short
square and triangle wave effects (coin, jump, key, gate, fanfare) and two
chiptune loops encoded as Ogg Vorbis, all computed, so they carry no
recordings with licences of their own. The scripts take sound names as
properties (`Coin.sound`, `Level Rules.music`, `Player Sounds.jump`, ...),
empty for silence, so the same scripts work in projects without sounds.

## Limitations

- No effects on sounds (echo, filters), no recording, no speech.
- Pitch is chosen when a sound starts; a source's pitch does not slide
  (changing it starts the sound again).
- Positional sound is left and right only (a 2D game has no behind).
- Studio has no waveform view or trimming; edit sounds in an audio editor.
