/**
 * @file shifter.h
 * @brief Real-time stereo pitch shifter for the Interval variant.
 *
 * A delay-line ("two tape heads") shifter: a read tap moves through a short
 * window behind the input at (1 - ratio) samples per sample, which transposes
 * what it reads. Before the tap runs out of window it crossfades to a second
 * tap at the other end. The new tap's start point is chosen by
 * cross-correlation, so the two taps are in phase during the crossfade and
 * tonal material splices without warble.
 *
 * Unlike a time-stretch engine there is no grid to drift: the output is always
 * the last few tens of milliseconds of input, only higher or lower. The delay
 * against the dry signal stays between kMinDelay and about 50 ms, whatever
 * the ratio and however long you hold it.
 *
 * Pure DSP, no hardware dependencies (host-testable).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace athanor {

class Shifter
{
  public:
    static constexpr uint32_t kSize = 8192;              // per buffer, power of two (170 ms)
    /** Floats the caller provides: left, right and a mono copy for the search. */
    static constexpr uint32_t kMemFloats = 3 * kSize;

    void Init(float sr, float* mem);
    /** Playback ratio: 1 = unison, 2 = octave up, 0.5 = octave down. 0.2 .. 4.5. */
    void SetRatio(float r);
    /** Start cleanly from "now" (call when the effect engages). */
    void Reset();
    /** Record the input without producing output: keeps the history current
     *  while the shifter isn't being heard, so it can start instantly. */
    void Feed(const float* in_l, const float* in_r, std::size_t n);
    /** Wet only. in/out may alias. */
    void Process(const float* in_l, const float* in_r, float* out_l, float* out_r, std::size_t n);

    /** Current delay of the playing tap behind the input, in samples (for tests). */
    float DelaySamples() const { return d_; }

  private:
    float Read(const float* buf, float delay) const;
    void  StartSplice();

    float* l_ = nullptr; float* r_ = nullptr; float* m_ = nullptr;
    uint32_t w_ = 0;          // next write index
    float sr_ = 48000.0f;
    float ratio_ = 1.0f;
    float d_ = 0.0f;          // playing tap delay
    float d2_ = 0.0f;         // incoming tap delay (during a crossfade)
    float fade_ = 0.0f;       // 0..1 crossfade position
    float fade_inc_ = 0.0f;
    bool  fading_ = false;
    float min_delay_ = 64.0f; // closest the tap gets to the input
};

} // namespace athanor
