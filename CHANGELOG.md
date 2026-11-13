# Changelog

All notable changes to Athanor. Versions follow [semver](https://semver.org) and are managed by
[release-please](https://github.com/googleapis/release-please) from Conventional Commit messages.
The version in `version.txt` is stamped into the HostLink identity at build time.

## [0.5.0](https://github.com/pixelscript/athanor/compare/v0.4.0...v0.5.0) (2026-11-13)


### Features

* bypass and usb midi ([63d3774](https://github.com/pixelscript/athanor/commit/63d37749730bcfe3c1496b397c7eee32f1437b90))
* fixes from copilot feedback ([07070ff](https://github.com/pixelscript/athanor/commit/07070fffba362bd04a72c8632465e0fa43b4314d))
* gain compensated distortion ([bb20a85](https://github.com/pixelscript/athanor/commit/bb20a85d9fec7f0e8be73f7754017f1243c13ef1))
* improve ux of multi button presses ([075e4af](https://github.com/pixelscript/athanor/commit/075e4af172f8e4f411a81047b00161cfbd97601c))
* let the pitch shift effect stay in time, allow 24 ppqn and delay divisions ([842fe1b](https://github.com/pixelscript/athanor/commit/842fe1b96f12c5154252622ec032ba8fbe78dc89))
* lots of improvements ([c64b3c5](https://github.com/pixelscript/athanor/commit/c64b3c5d595d5a00ecb62d13ba45d5024959c7ec))
* new effect for stepped melodic time stretch plus recording function ([2b85c88](https://github.com/pixelscript/athanor/commit/2b85c88213a96b7dfd5210bfb87cd04db8586498))
* SDK update, adding missing interval updates ([0ce7264](https://github.com/pixelscript/athanor/commit/0ce7264021645f9327a2070fc78aaf9de065e3a8))

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
