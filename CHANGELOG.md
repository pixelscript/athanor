# Changelog

All notable changes to Athanor. Versions follow [semver](https://semver.org) and are managed by
[release-please](https://github.com/googleapis/release-please) from Conventional Commit messages.
The version in `version.txt` is stamped into the HostLink identity at build time.

## [0.4.0](https://github.com/pixelscript/athanor/compare/v0.3.0...v0.4.0) (2026-11-04)


### Features

* first commit ([d37069f](https://github.com/pixelscript/athanor/commit/d37069fd636a5135234f5c19eebd0219c6b6a3ca))
* first release ([4fbcf20](https://github.com/pixelscript/athanor/commit/4fbcf207eb905a570ecdef297c9a00daece2e815))
* more intense wow and flutter ([f205e15](https://github.com/pixelscript/athanor/commit/f205e1562cb7cc3f03c2bdeb0bf0b0c0cba7202c))

## 0.3.0 (pre-GitHub)

### Features

* Speed row plays Capicola's pitch control (tape slow-down to stop / speed-up to 2×) with simulated
  reel inertia.
* Button variants: B1 Speed ↔ Tape/Vinyl age, B2 Saturation / Distortion / Fuzz (analog CCW,
  digital CW), B3 Reverb+Delay / Chorus+Tremolo / Phaser+Flanger. Saved to QSPI.
* J3 clock: delay time and clock-synced LFO divisions.
* Crash reporter / safe mode.

### Bug Fixes

* Boot crash: SDRAM-resident engine had a default member initialiser (now a compile-time check).
* Boot crash: D2 SRAM clocks not enabled under the bootloader; DTCM stack headroom.
* Reverb clicking: an output tap read past its delay line; low cut and a linked limiter on the output.

## 0.2.0 (pre-GitHub)

* Variants, tape/vinyl, drive levels, modulation effects; CPU meter on B3.

## 0.1.0 (pre-GitHub)

* First version: time stretch, overdrive/wavefold, reverb/delay; deadbanded amount knobs.
