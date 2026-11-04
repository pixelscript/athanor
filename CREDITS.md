# Credits

Athanor stands on other people's work. This file says exactly whose, and where it lives in the tree.

## Capicola: Heavylight Industries

- **Source:** <https://github.com/heavylight-industries/capicola> (imported from commit
  `8c64e85`, Capicola 1.1.0, 2026-10-08)
- **Paper:** *Keyframe Time Stretching via Extrema Sampling*, DAFx26:
  <https://github.com/heavylight-industries/dafx26-paper>
- **Licence:** GNU AGPL v3.0, which is why Athanor is AGPL v3.0 too.

The **Speed** row (B1, variant 1) is Capicola's engine, and its sound is the Capicola author's design.
Athanor's job is only to put Capicola's pitch control behind a deadbanded performance knob and add
simulated reel inertia. Capicola describes its core method as "independently authored" and
human-created; please credit it as such if you build on this.

Files in `dsp/` that come from Capicola's `lib/`:

| File | What it is |
|---|---|
| `KeyframeRecorder.h` | The keyframe record/playback engine and two-grain punch/crossfade logic |
| `Analyzer.h` | Extrema-sampling keyframe writer (the sparsifier from the paper) |
| `SparseLine.h` | Sparse keyframe ring buffer with Hermite/Catmull-Rom reads |
| `Granule.h` | Granular reader: stretch grid, adaptive splicing, boundary guards |
| `Detector.h` | TKEO-driven transient detector with adaptive thresholding |
| `DeluxeLine.h` | Delay line with Hermite/B-spline reads, derivatives and TKEO |
| `filter.h` | Zero-delay-feedback one-pole and state-variable filters (after Vadim Zavalishin) |
| `Shapers.h`, `TabulatedFunction.h`, `Tabulator.h`, `functors.h` | Tabulated waveshapers (the "sinc" keyframe shaper) |

**Athanor's modifications** are marked with `Athanor` comments:

- `Granule.h`:
  - `RetriggerBehind()` and `GridAheadOfTap()`, for speed-up beyond realtime. Unused by the current
    Speed row but kept.
  - Transient-aware splicing and waveform-aligned splice targets. These are switched **off** for the
    Speed row so it behaves exactly like Capicola's own reader.
- `KeyframeRecorder.h`:
  - A speed-up loop, an onset logger, `SetAutoSlice()`, `SetGuards()` and loop-fade control.
- `src/stretch_engine.cpp`:
  - Adapted from Capicola's `src/audio/audio_engine.cpp`: the two-channel recorder pair and the
    stereo re-alignment guardrail.
  - The voicing constants (0.572× stretch grid, 642-keyframe grain, 20 ms fade, transient keep ratio,
    sinc shaper at drive 1) are Capicola's factory perf-page defaults.

## Hermetic Modular

- **Alchemy Lab** hardware, the **Alchemy SDK** (`lib/alchemy-sdk`, MIT) and the **alchemy-template**
  project this firmware's build, linker script and board bring-up are based on:
  <https://github.com/hermetic-modular>
- `athanor.lds` is the SDK's `alchemy_stm32h750ib_sram.lds` with one added `.d2_bss` section.

## Electrosmith

- **libDaisy** (`lib/libDaisy`, MIT): <https://github.com/electro-smith/libDaisy>

## Algorithms and references

| Where | Algorithm | Reference |
|---|---|---|
| `src/fx.cpp`, `Plate` | Plate reverb topology, delay lengths and output taps | J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", *J. Audio Eng. Soc.* 45(9), 1997 |
| `src/drive.cpp`, analog levels | First-order antiderivative anti-aliasing (ADAA) | J. Parker, V. Zavalishin, E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time Convolution", DAFx-16, 2016; S. Bilbao, F. Esqueda, J. Parker, V. Välimäki, "Antiderivative Antialiasing for Memoryless Nonlinearities", *IEEE SPL* 24(7), 2017 |
| `dsp/Granule.h`, aligned splices (Athanor addition, currently off) | Waveform-similarity overlap-add idea | W. Verhelst, M. Roelands, "An Overlap-Add Technique Based on Waveform Similarity (WSOLA)", ICASSP 1993 |
| `dsp/Detector.h`, `dsp/DeluxeLine.h` | Teager-Kaiser energy operator | J. F. Kaiser, "On a Simple Algorithm to Calculate the 'Energy' of a Signal", ICASSP 1990 |
| `dsp/filter.h` | Topology-preserving / zero-delay-feedback filters | V. Zavalishin, *The Art of VA Filter Design* |

The tape/vinyl simulation, drive levels, chorus/tremolo/phaser/flanger, reel inertia, CV handling
and crash reporter are original to Athanor and use standard textbook building blocks: one-pole
filters, first-order allpasses, modulated fractional delays, a mass-spring-damper and a xorshift
noise generator.

## AI assistance

Athanor's integration code, the non-Capicola DSP and the documentation were written with AI
assistance (Claude), directed and tested on hardware by the project owner. Capicola's engine is
unchanged in substance, apart from the marked additions above.
