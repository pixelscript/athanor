/**
 * @file stretch_engine.h
 * @brief Athanor row 1 — Capicola's keyframe time stretcher, trimmed down.
 *
 * Derived from Capicola's AudioEngine (c) heavylight-industries, AGPL-3.0.
 * Two independent KeyframeRecorder chains (L / R) run continuously in
 * LIVE_EFFECT mode: incoming audio is sparsified into keyframes, read back by
 * a pair of trading grains on a "stretch grid", and every kept transient
 * snaps the grid back to "now" (with a crossfade) so the stretch stays locked
 * to the rhythm of the input.
 *
 * Stretch < 1 slows down (0 = freeze between transients).
 * Stretch > 1 speeds up: each grain is seated behind the tap and fast-forwards
 * through recent history until it catches up, then loops (see
 * KeyframeRecorder.h "Athanor" notes).
 *
 * Athanor v0.3 voices it exactly like Capicola's factory perf page, because the
 * Time row now plays Capicola's PITCH control (tape-speed character): stretch
 * grid at 0.57x forward, transient snapping on, 642-keyframe grains, 20 ms
 * fade, and the sinc keyframe waveshaper at drive 1 (level-matched here).
 *
 * Output is 100% wet; the caller owns the dry/wet blend.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace athanor {

class StretchEngine
{
  public:
    static constexpr std::size_t kMaxBlock = 64;

    void Init(float sample_rate);

    /** Wet-only stereo process. n <= kMaxBlock. */
    void Process(const float* in_l, const float* in_r,
                 float* out_l, float* out_r, std::size_t n);

    void SetStretch(float s);              // 0 = freeze, 1 = realtime, >1 = faster
    void SetSpeedLagSamples(double lag);   // speed-up: how far back each grain starts
    void Resync();                         // punch both channels back to "now"
    void SetGrainLeash(int keyframes);     // grain size (adaptive splice leash)
    void SetPitch(float speed);            // grain playback speed (1 = unity, 0 = stopped)

    uint32_t SliceCount() const { return slice_count_; }

  private:
    uint32_t slice_count_ = 0;
};

} // namespace athanor
