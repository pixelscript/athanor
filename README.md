# Athanor

A performance end-of-chain effect firmware for the Hermetic Modular **Alchemy Lab** (V2 hardware).
It has three effect rows that feed into each other from top to bottom. Each row has one
**amount** knob on the left, one **modifier** knob on the right, and a button that steps through
that row's variants.

```
        LEFT (amount)   BUTTON   RIGHT (modifier)
Row 1   [P1] TIME       B1       [P2] MIX
Row 2   [P3] DRIVE      B2       [P4] TONE
Row 3   [P5] SPACE      B3       [P6] FEEDBACK / RATE
```

| Button | Variant | Amount CCW | Amount CW | Modifier |
|---|---|---|---|---|
| B1 | 1 Speed | tape slow-down to stop | tape speed-up to 2× | mix |
| B1 | 2 Age | tape | vinyl | wow + flutter / hiss + clicks |
| B2 | 1 Saturation | analog | digital | tone |
| B2 | 2 Distortion | analog | digital | tone |
| B2 | 3 Fuzz | analog | digital | tone |
| B3 | 1 Reverb | reverb | delay into reverb | feedback |
| B3 | 2 Chorus/trem | chorus | tremolo | rate |
| B3 | 3 Phaser/flanger | phaser | flanger | rate |

Each press moves to the row's next variant, wrapping round. The button LED and that row's rings
change colour to show which one you're on. Your selections are saved about 1.5 s after the last
press and come back at power-on. Switching variants crossfades, so you can do it live.

Signal flow: **In → Speed / Age → Drive → Space → Out**, in stereo.

## The amount knobs

Each amount knob has a wide **neutral deadband from 10 o'clock to 2 o'clock**. Anywhere in that
zone the row is bypassed, so you can throw the knob back to the top mid-performance and know it's off.

- The LED ring shows the deadband as dim grey, with a white dot for the pointer while you're neutral.
- The left (CCW) and right (CW) zones are tinted in their flavour's colour. They light up from the
  deadband edge out to the pointer as you turn into them.
- Each row fades in over the first few degrees past the deadband edge, so leaving neutral never clicks.

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
| MIX | **More wow and flutter** (slow wander, up to seasick) | **More hiss, crackle and clicks** |

The Speed engine keeps recording in the background, so switching back to it is instant.

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

See the variant table at the top. Each button LED shows its row's current variant colour. B3 doubles
as a CPU meter and overrides its colour when needed:

- **Red**: an audio block overran in the last 2 seconds (real overload; you'd hear clicks).
- **Amber**: average load is above 80% (close to the limit).

The first 3 seconds after boot are ignored.

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

## Build

You need `git`, `make`, `arm-none-eabi-gcc` and `dfu-util` (`brew install make dfu-util` and
`brew install --cask gcc-arm-embedded`).

```sh
./setup.sh        # clones pinned alchemy-sdk v0.12.0 + libDaisy into lib/, builds libDaisy
make              # -> build/athanor.bin
```

A prebuilt `athanor.bin` is included in this folder.

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
| Memory layout (reverb in internal SRAM) | `athanor.lds` (copy of the SDK script + `.d2_bss`) |

`tests/host_test.cpp` is a desktop sanity test for the DSP. The probes in `tests/` render the
stretcher for inspection: `speed_probe` and `glide_probe` check the rate, `trans_probe` counts
attacks, and `tone_probe` measures roughness on a sustained chord.

```sh
g++ -O2 -std=c++17 -I tests/stub -I src -I dsp tests/host_test.cpp src/fx.cpp src/drive.cpp \
    src/age.cpp src/modfx.cpp src/stretch_engine.cpp -o host_test && ./host_test
```

## License

**GNU AGPL v3.0.** The time-stretch engine in `dsp/` is Capicola by heavylight-industries
(https://github.com/heavylight-industries/capicola), lightly modified. The modifications are marked
"Athanor" in `Granule.h` and `KeyframeRecorder.h` and add speed-up (stretch > 1) support. If you
distribute this firmware or hardware running it, you must offer the source.
