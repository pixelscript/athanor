# Athanor

[![Build](../../actions/workflows/build.yml/badge.svg)](../../actions/workflows/build.yml)
[![Release](../../actions/workflows/release-please.yml/badge.svg)](../../releases/latest)

> **This repo has been made with the help of an LLM**

**A performance effects firmware for the end of your chain, for the Hermetic Modular Alchemy Lab
(V2).** Three stacked effect rows: tape speed (or tape/vinyl age, or musical intervals), drive, and space. Each row is
played from one big knob, and that knob has a **wide neutral deadband from 10 to 2 o'clock**. Throw
it back to the top and that effect is fully bypassed, so you can dive in and back out mid-set
without hunting for "off".

Athanor's layout is **inspired by Hermetic Modular's Spagyros**, the Alchemy Lab's own three-slot
end-of-chain FX firmware: three rows in series, depth on the left, parameter on the right, a button
to change the effect. No Spagyros code is used. See [Credits](#credits).

The tape-speed row runs **[Capicola](https://github.com/heavylight-industries/capicola)'s keyframe
engine by Heavylight Industries**. See [Credits](#credits).

Each row has one **amount** knob on the left, one **modifier** knob on the right, a button that steps
through that row's variants, and a CV input that can modulate either or both knobs. Hold **B1 + B3**
to record the output to the SD card, press **B1 + B2** to bypass everything, and plug in USB for
**MIDI clock and CC control**.

```
        LEFT (amount)   BUTTON   RIGHT (modifier)        CV in
Row 1   [P1] TIME       B1       [P2] MIX                J4
Row 2   [P3] DRIVE      B2       [P4] TONE               J5
Row 3   [P5] SPACE      B3       [P6] FEEDBACK / RATE    J6
                                                 clock:  J3
```

| Button | Variant | Amount CCW | Amount CW | Modifier |
|---|---|---|---|---|
| B1 | 1 Speed | tape slow-down to stop | tape speed-up to 2× | mix |
| B1 | 2 Age | tape | vinyl | wow + flutter / hiss + clicks |
| B1 | 3 Interval | down a 4th / octave / octave + 4th / 2 octaves | up a 5th / octave / octave + 5th / 2 octaves | mix |
| B2 | 1 Saturation | analog | digital | tone |
| B2 | 2 Distortion | analog | digital | tone |
| B2 | 3 Fuzz | analog | digital | tone |
| B3 | 1 Reverb | reverb | delay into reverb | feedback |
| B3 | 2 Chorus/trem | chorus | tremolo | rate |
| B3 | 3 Phaser/flanger | phaser | flanger | rate |

A **tap** moves to the row's next variant, wrapping round. The variant changes when you release the
button. The button LED and that row's rings change colour to show which one you're on. Your
selections (and CV depths) are saved about 1.5 s after the last change and come back at power-on.
Switching variants crossfades, so you can do it live.

Signal flow: **In → Speed / Age / Interval → Drive → Space → Out**, in stereo.

## The amount knobs

Each amount knob has a wide **neutral deadband from 10 o'clock to 2 o'clock**. Anywhere in that
zone the row is bypassed, so you can throw the knob back to the top mid-performance and know it's off.

- The LED ring shows the deadband as dim grey, with a white dot for the pointer while you're neutral.
- The left (CCW) and right (CW) zones are tinted in their flavour's colour. They light up from the
  deadband edge out to the pointer as you turn into them.
- Each row fades in over the first few degrees past the deadband edge, so leaving neutral never clicks.

## CV modulation (J4 / J5 / J6)

**J4**, **J5** and **J6** feed rows 1, 2 and 3. Each jack can move **either or both knobs of its
row**, each with its own depth, set like an attenuverter:

- **Hold the row's button and turn the knob you want the CV to move.** The first knob you turn is
  the one you're setting. Noon = off, CW = positive depth, CCW = inverted. While you hold, that
  knob's ring shows the depth: white for positive, cyan for inverted, with a detent at noon.
- To modulate both knobs, do it twice: hold + turn the left knob, release, then hold + turn the
  right knob. One LFO can, say, push the drive up while pulling the tone down.
- **Release.** The knob goes back to its normal job, but its value stays where it was until you turn
  the knob back to it (a dim pip shows where the knob physically is). So setting a depth never makes
  the effect, mix, tone or feedback jump.
- **Depth range:** at full depth, ±5 V moves the virtual knob half its travel. On an amount knob, a
  bipolar LFO can sweep from one effect, through the bypass deadband, into the other. Both rings
  follow the modulated position, with a dim pip at the physical knob.
- **Unpatched jacks** contribute nothing, and depths default to off. Depths saved by v0.4 move to
  the left knobs.

## Row 1, variant 1: Speed (Capicola's pitch control)

This row plays **Capicola's PITCH control**, the tape-speed sound: grains play back slower or
faster, so the pitch drops or rises like tape. Capicola's stretch grid and transient snapping keep
the timing locked to your input. Everything is voiced like Capicola's factory settings: 0.57×
stretch grid, transient snapping at the noon threshold, 642-keyframe grains, a 20 ms fade, and the
sinc keyframe shaper at drive 1. The shaper's +6 dB is trimmed back so the wet signal sits level
with the dry.

| Knob | Effect |
|---|---|
| TIME CCW | **Tape slow-down**: 1× down to a full stop. The level fades away in the last stretch, like a real tape stop. |
| TIME CW | **Tape speed-up**: 1× up to 2× (an octave up). |
| MIX | Dry/wet blend. |

**Reel inertia ("momentum").** The knob sets a *target* speed, and the speed you hear chases it like
a heavy reel. Fast moves swoop and overshoot by about 16%, then settle in roughly 0.3 s. Let go back
into the deadband and you hear the reel spin back up (or down) to normal speed before the row hands
back to the dry signal. To change the feel, edit `kReelHz` (lower = heavier reel) and `kReelDamping`
(lower = more overshoot) in `src/athanor.cpp`.

## Row 1, variant 2: Age (tape / vinyl)

AMOUNT sets the overall **age**: one knob makes everything older together. MIX becomes the
artefacts knob.

| Knob | Tape (CCW) | Vinyl (CW) |
|---|---|---|
| AMOUNT | Saturation, head bump, high-frequency loss, a touch of wow and hiss | Narrower bandwidth (bass and treble roll-off), mono-ing, warmth, off-centre warp, a light crackle floor and hiss |
| MIX | **More wow and flutter**: slow, irregular wow plus capstan flutter, with level wobble and the odd dropout near the top. Pitch swing goes from nothing, to about ±65 cents halfway, to over a semitone at full. | **More hiss, crackle and clicks** |

The Speed engine keeps recording in the background, so switching back to it is instant.

## Row 1, variant 3: Interval

The same Capicola pitch engine and tape reel as Speed, but the amount knob **snaps to musical
steps**, out to two octaves, on a **harmonic ladder**: every step is the input's own fifth or octave,
so both directions add the same two notes and the chord stays rooted on what you play. Each side of
the deadband is split into four equal zones:

| Zone | CCW (down) | CW (up) | From a C |
|---|---|---|---|
| just past the deadband | a fourth (×3/4) | a fifth (×3/2) | G |
| | an octave (×1/2) | an octave (×2) | C |
| | an octave and a fourth (×3/8) | an octave and a fifth (×3) | G |
| fully round | two octaves (×1/4) | two octaves (×4) | C |

**MIX** works as on Speed: fully CW is only the shifted signal, and around noon you hear the incoming
audio **plus** the shifted copy, so a fifth or an octave plays as a two-note chord. The intervals are
pure (just intonation), so fifths ring cleanly against the dry note.

- Moving between steps glides quickly (about 70 ms) on the tape reel, with a hint of swoop.
- The zones have a little hysteresis, so a knob or CV sitting on a border doesn't chatter. Put CV
  into J4 with some depth on the amount knob, and a stepped CV plays the intervals as a sequence.
- The shifted copy is about 20 ms behind the dry signal (the engine's lookahead), which you can hear
  as a slight thickening on sharp attacks.
- Everything down to two octaves, and up to an octave, is clean. The top two up-steps (×3, ×4) have to
  splice much more often and sound a little grainier, with a slight warble on sustained tones.

## Row 2: Drive

B2 picks the level; the amount knob picks analog (CCW) or digital (CW).

| Level (B2 colour) | Analog (CCW) | Digital (CW) |
|---|---|---|
| 1 Saturation (green) | Subtle tube-style asymmetric soft saturation | Clean digital hard clipping |
| 2 Distortion (orange) | Mid-pushed, diode-style clipping | Bit-crush (12 to 4 bits) and sample-rate reduction into a hard clip |
| 3 Fuzz (red) | Gated, sputtery fuzz with a starved bias, so decays break up | Wavefolder, heavily crushed and decimated |

TONE is a tilt EQ on the driven signal: noon is flat, CCW is darker, CW is brighter. Its ring lights
outward from noon. The analog shapers are anti-aliased; the digital ones alias deliberately. All
levels are roughly level-matched.

## Row 3, variant 1: Reverb / delay

| Knob | Effect |
|---|---|
| SPACE CCW | **Reverb** (Dattorro plate). Turning further sends more signal in and ducks the dry. |
| SPACE CW | **Delay into reverb**: stereo delay with 25% cross-feed, whose echoes also feed the plate. |
| FEEDBACK | In reverb mode it sets the reverb decay (small room up to near-freeze). In delay mode it sets the echo feedback (up to very long), and the reverb decay follows it more gently. |

Snapping SPACE back to neutral **closes the sends but leaves the returns open**, so the tails ring out
naturally. FEEDBACK keeps controlling whichever mode you used last, so you can shape a tail while
it decays.

## Row 3, variants 2 and 3: Chorus / tremolo, phaser / flanger

FEEDBACK becomes **RATE**: an LFO speed with no clock patched, or a musical division of the J3 clock
when one is patched.

| | Amount | Rate, free | Rate, clocked (cycles per clock pulse) |
|---|---|---|---|
| Chorus (CCW) | Depth and mix (two voices per side, wide stereo) | 0.05 to 3 Hz | 1/8, 1/4, 1/2, 1 |
| Tremolo (CW) | Depth. The shape squares up past halfway, and the top quarter turns into a stereo pan-trem | 0.5 to 14 Hz | 1/2, 1, 1.5, 2, 3, 4, 6, 8 |
| Phaser (CCW) | Sweep width, feedback and mix (6 stages) | 0.05 to 4 Hz | 1/16, 1/8, 1/4, 1/2, 1 |
| Flanger (CW) | Sweep width, feedback and mix | 0.03 to 2 Hz | 1/16, 1/8, 1/4, 1/2 |

When clocked, the LFO also phase-locks to the clock edges, so a trem at 2 per pulse stays on the 8ths.

Switching away from Reverb **lets the reverb and delay tails ring out** in the background. They stop
once the tail has been below -80 dB for a second.

## J3: clock in

Patch a clock or gate into **J3**. The delay time follows the clock period: rising edge above about
1.5 V, period smoothed, tempo changes over 8% jump straight to the new value. The delay glides to the
new time like tape. With no clock, or when the clock stops for 2.5 periods, the delay falls back to
**375 ms**. Clocks longer than 2.5 s are halved until they fit. P6's ring blinks white at 12 o'clock
on every clock edge. The modulation variants use the same clock for their rates.

## Buttons

Tap = next variant; hold + turn a knob = that knob's CV depth (see above). Each button LED shows its
row's current variant colour. B3 doubles as a CPU meter and overrides its colour when needed:

- **Red**: an audio block overran in the last 2 seconds (real overload; you'd hear clicks).
- **Amber**: average load is above 80% (close to the limit).

The first 3 seconds after boot are ignored.

## Bypass all

Press **B1 and B2 together** to bypass all three rows at once; press them again to bring everything
back. It acts the moment the second button goes down, and neither button changes its variant.

- A **gate on J7** does the same for as long as it is high (above 1.5 V), so a sequencer can punch
  the effects in and out.
- While bypassed, the three buttons go dim white and each amount ring shows neutral, with a dim pip
  where the knob really is.
- It behaves exactly like throwing all three amount knobs into the deadband: reverb and delay tails
  ring out, and the tape reel spins back to normal speed. Your knob positions, CV and MIDI are all
  still there when you come back.

## USB MIDI

Athanor is a USB MIDI device on the same cable the web programmer uses (they work side by side). It
listens on **all channels**.

**Clock.** MIDI clock sets the delay time and the clocked LFO rates, exactly as a clock on J3 does,
one beat per quarter note. **J3 wins** whenever it has a clock; MIDI takes over when J3 is silent.
MIDI Start re-aligns the beat. When the transport stops, the last tempo is held, so the delay time
doesn't lurch.

**Control changes.**

| CC | Controls | Values |
|---|---|---|
| 20 | Row 1 amount (TIME) | 64 = noon. Bypassed from 36 to 91; below is the CCW effect, above the CW effect |
| 21 | Row 1 modifier (MIX) | 0 to 127 |
| 22 | Row 2 amount (DRIVE) | as CC 20 |
| 23 | Row 2 modifier (TONE) | 64 = flat |
| 24 | Row 3 amount (SPACE) | as CC 20 |
| 25 | Row 3 modifier (FEEDBACK / RATE) | 0 to 127 |
| 26 / 27 / 28 | Variant of row 1 / 2 / 3 | 0 to 42 first, 43 to 85 second, 86 to 127 third |
| 29 | Record | 64 and up starts, below 64 stops |
| 30 | Bypass all | 64 and up bypassed, below 64 active |

- A knob set by CC **holds that value until you turn the physical knob to it** (a dim pip shows where
  the knob is), the same pickup used after setting a CV depth. Nothing jumps when you grab a knob.
- CV still adds on top of a value set by CC.
- Variants chosen by CC aren't written to memory, so automation doesn't wear the flash; the variant
  you last picked on the panel is what comes back at power-on.

## Recording to SD

Hold **B1 and B3 together for half a second** to start recording; do it again to stop. Neither
button changes its variant when you do this.

- Records the **final stereo output** (after the limiter) as **24-bit / 48 kHz WAV**, at the card's
  root: `REC0001.WAV`, `REC0002.WAV`, and so on. The card must be **FAT32**.
- The WAV header is rewritten every 4 seconds, so if the power goes mid-take you keep everything up to
  the last few seconds.
- Download takes over USB from the web programmer's file view (the card can stay in the module), or
  read the card on a computer. The take being recorded is locked against changes while it's open.

The buttons tell you briefly what happened, all three at once:

| Buttons | Meaning |
|---|---|
| **two short red flashes** | recording started |
| a short red blip every 3 s | still recording |
| **one short white flash** | stopped, take saved |
| **one long red flash** | couldn't start (no card, not FAT32, card full), or the card failed mid-take (the take is closed and kept) |
| one amber flash | the card fell behind and a moment was lost; recording carries on |

The recorder buffers about 7 seconds in SDRAM, so ordinary cards keep up easily; amber flashes mean
a slow or worn card. Takes stop automatically at the FAT32 4 GB limit (about 4 hours).

## Live values and log (web programmer)

With the module connected over USB, the web programmer's **Device console** shows a log and live
values from Athanor:

| Value | Meaning |
|---|---|
| CPU load | average audio load, % |
| Audio overruns | blocks that ran out of time since boot (should stay at 0) |
| J3 clock | detected tempo in BPM (0 = no clock) |
| Tape speed | the reel's current speed (1 = normal) |
| Recording / Take number / Recording dropouts | SD recorder state |

The log records each take as it starts and is saved, and says why a recording failed.

## Safe mode (crash report)

If the firmware ever crashes, it records where and resets itself. The bootloader then runs again,
and the firmware starts in **safe mode** instead of normal operation. You'll see:

- **The buttons** blinking red.
- **The rings** showing a crash report:

| Ring | Shows |
|---|---|
| P1 (top left) | Boot stage, 1 to 13 red LEDs |
| P3 (middle left) | Boot stage beyond 13, in orange (the audio callback is 14 to 19) |
| P2 (top right) | Fault type: white = hard, blue = bus, yellow = usage, magenta = memory |
| P4 / P6 (right) | Where in the code it crashed, as bits (white = 1) |
| P5 (bottom left) | Bus-fault flags, as bits (green = 1) |

Take a photo of the panel; that's enough to find the exact line. Safe mode keeps the USB link up,
so the web programmer can reflash straight away. The next power-on boots normally again.

Stages: 1 static init, 2 main, 3 hardware init, 4 load saved variants, 5 speed engine, 6 tape/vinyl,
7 drive, 8 reverb/delay, 9 chorus/trem, 10 phaser/flanger, 11 control setup, 12 audio started,
13 first control frame, 14 to 19 inside the audio callback (entry, speed row, age, drive, modulation,
space).

## Download

Prebuilt firmware is attached to every [GitHub release](../../releases/latest) as
`athanor-vX.Y.Z.bin`, along with the matching `.elf` for decoding crash reports. Every push also
builds a test binary, available under the **Actions** tab as a workflow artifact.

## Build

You need `git`, `make`, `arm-none-eabi-gcc` (13.x) and `dfu-util` (`brew install make dfu-util` and
`brew install --cask gcc-arm-embedded`).

```sh
git clone --recurse-submodules <this repo> athanor && cd athanor
make libdaisy     # once (or ./setup.sh)
make              # -> build/athanor.bin
make test         # host-side DSP tests, no hardware needed
```

The Alchemy SDK (v0.13.0) and libDaisy are git submodules pinned to the commits this firmware is
tested against. The build stamps `version.txt` and the git hash into the HostLink identity, so the
web programmer shows exactly what's running.

## Releases and versioning

Releases are automated with [release-please](https://github.com/googleapis/release-please).
Write commit messages in [Conventional Commits](https://www.conventionalcommits.org) style:

- `fix: …` gives a patch release.
- `feat: …` gives a minor release.
- `feat!: …` or a `BREAKING CHANGE:` footer gives a major release.

release-please keeps a release PR open that bumps `version.txt` and `CHANGELOG.md`. Merging it tags
the release, and the **Release** workflow builds the firmware at that tag and attaches the `.bin`,
the `.elf` and `SHA256SUMS.txt`.

Repository setting needed once: **Settings → Actions → General → Workflow permissions → "Allow GitHub
Actions to create and approve pull requests"**, so release-please can open its PR.

## Flash

Put the module in update mode: hold **B3** through the roughly 2 s boot window, until the rings breathe. Then run:

```sh
dfu-util -a 0 -s 0x90040000:leave -D athanor.bin -d ,0483:df11
# or, from source:
make program-dfu
```

You can also use the Hermetic Modular Web Programmer. The firmware speaks HostLink, so `make program-live`
works for later updates without the button dance.

## Tuning constants

| What | Where |
|---|---|
| Deadband edges (10 and 2 o'clock) | `kDeadLoHour` / `kDeadHiHour`, `src/athanor.cpp` |
| Speed range (stop to 2×) | `AudioCallback`, `src/athanor.cpp` |
| Clock thresholds and fallback | `kClkHiV`, `kClkLoV`, `kFallbackSec` |
| Drive levels | `src/drive.cpp` |
| Tape / vinyl | `src/age.cpp` |
| Chorus / trem / phaser / flanger | `src/modfx.cpp` |
| Rate ranges and clock divisions | `RateFor()`, `src/athanor.cpp` |
| Variant colours | `kLooks`, `src/athanor.cpp` |
| Reverb / delay voicing | `src/fx.cpp` |
| Saved variants | one QSPI sector at `0x907FF000` (top of the preset region) |
| Capicola voicing (stretch, grain size, transient threshold, fade, shaper) | `src/stretch_engine.cpp` |
| Reel inertia (speed row) | `kReelHz`, `kReelDamping`, `src/athanor.cpp` |
| Interval steps and glide | `kDown` / `kUp`, `kStepHz`, `kStepDamping`, `src/athanor.cpp` |
| SD recorder (buffer, chunk size, header sync) | `src/recorder.h`, `src/recorder.cpp` |
| MIDI CC numbers | `kCcKnob0`, `kCcVariant`, `kCcRecord`, `kCcBypass`, `src/athanor.cpp` |
| MIDI clock lock and smoothing | `src/midi_clock.h` |
| CV jacks, depth range, dead zone | `kCvJack`, `OnPoll()`, `src/athanor.cpp` |
| Memory layout (reverb in internal SRAM, SD DMA buffers) | `athanor.lds` (copy of the SDK script + `.d2_bss` and `.axi_bss`) |

`tests/host_test.cpp` is a desktop sanity test for all the DSP (`make test`). The probes in
`tests/` measure specific things:

- `pitch_probe`: speed tracking and reel overshoot.
- `interval_probe`: each Interval step lands in tune, and dry + shifted both come through at the
  mix midpoint (runs in `make test`).
- `midi_clock_probe`: MIDI clock lock, jitter rejection, tempo changes, Start alignment and the CC
  mapping (runs in `make test`).
- `rec_probe`: the SD recorder on the real FatFS and the SDK's RAM disk: sample-exact takes, power-cut
  header sync, dropouts, card failure and recovery, and no unaligned DMA (runs in `make test`).
- `wow_probe`: wow/flutter depth in cents.
- `rev_probe`: reverb click detection.
- `rev_balance_probe`: reverb left/right balance.
- `plate_ir`: plate impulse response.

## Credits

Full details, with file-level attribution, are in [CREDITS.md](CREDITS.md).

- **Capicola, by Heavylight Industries**
  ([github.com/heavylight-industries/capicola](https://github.com/heavylight-industries/capicola)).
  The tape-speed row is Capicola's keyframe time-stretch / pitch engine, from the author's peer-reviewed
  DAFx26 paper, *[Keyframe Time Stretching via Extrema Sampling](https://github.com/heavylight-industries/dafx26-paper)*.
  This includes the sparse keyframe analyzer, granular reader, TKEO transient detector, ZDF filters
  and tabulated waveshapers. The sound of the Speed row, including the pitch behaviour it's modelled
  on, is the author's work; Athanor just puts it behind a deadbanded knob with reel inertia. If you
  like it, go and get the full Capicola firmware.
- **Hermetic Modular's Spagyros**: the idea and panel layout Athanor builds on (three effect rows in
  series, depth + parameter per row, a button per row to change effect, per-row depth CV). Concept
  only; no Spagyros code is used.
- **Hermetic Modular**: the Alchemy Lab hardware, the Alchemy SDK and the alchemy-template project
  this firmware started from.
- **Electrosmith**: libDaisy.
- **Jon Dattorro**: the plate reverb topology (*Effect Design, Part 1*, JAES 1997).
- **Parker, Zavalishin & Le Bivic**: first-order antiderivative anti-aliasing (ADAA) used by the
  analog drive shapers (*Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time
  Convolution*, DAFx-16).

## License

**GNU AGPL v3.0** (see [LICENSE](LICENSE)). Athanor includes Capicola's engine, which is AGPL-3.0,
so the whole firmware is too. If you distribute this firmware, or hardware running it, you must
offer the corresponding source. The Alchemy SDK and libDaisy are MIT-licensed submodules under their
own terms.
