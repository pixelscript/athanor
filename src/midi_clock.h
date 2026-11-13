/**
 * @file midi_clock.h
 * @brief Beat period and beat edges from MIDI timing clock (24 ticks / beat).
 *
 * Pure logic, host-testable. Feed it the microsecond timestamp of every
 * 0xF8 tick (and Start / Continue / Stop); read the beat period in seconds
 * and a beat-edge counter.
 *
 *  - Locks after 6 ticks (a quarter of a beat), then measures across a full
 *    beat (24 ticks) so USB jitter averages out.
 *  - Start re-aligns the beat: the first tick after it is beat 1.
 *  - When ticks stop, the last tempo is HELD (the delay time shouldn't lurch
 *    because the DAW stopped); beat edges simply stop arriving.
 *  - Tempos outside 20..400 BPM are ignored.
 */
#pragma once

#include <cstdint>

namespace athanor {

class MidiClock
{
  public:
    static constexpr int kPpqn = 24;

    void Reset()
    {
        count_ = 0; head_ = 0; phase_ = 0; period_ = 0.0f; edges_ = 0;
        running_ = false; last_us_ = 0;
    }

    /** One timing-clock tick. Returns true when this tick is a beat. */
    bool Tick(uint32_t t_us)
    {
        /* A long gap means the clock restarted: measure afresh, keep tempo. */
        if (count_ > 0 && (uint32_t)(t_us - last_us_) > kGapUs) count_ = 0;
        last_us_ = t_us;
        running_ = true;

        times_[head_] = t_us;
        head_ = (head_ + 1) % (kPpqn + 1);
        if (count_ < kPpqn + 1) count_++;

        if (count_ >= kMinTicks + 1)
        {
            const int   span  = count_ - 1;                      // intervals covered
            const int   first = (head_ + (kPpqn + 1) - count_) % (kPpqn + 1);
            const float beat  = (float)(uint32_t)(t_us - times_[first]) * 1.0e-6f
                              * (float)kPpqn / (float)span;
            if (beat >= 0.15f && beat <= 3.0f)                   // 400 .. 20 BPM
            {
                /* Jump on real tempo changes, otherwise glide out the jitter. */
                const float d = beat - period_;
                if (period_ <= 0.0f || (d < 0 ? -d : d) > 0.03f * period_) period_ = beat;
                else period_ += 0.15f * d;
            }
        }

        const bool beat_now = (phase_ == 0);
        phase_ = (phase_ + 1) % kPpqn;
        if (beat_now) edges_++;
        return beat_now;
    }

    /** Start: the next tick is beat 1. Continue keeps the phase. */
    void Start()    { phase_ = 0; count_ = 0; running_ = true; }
    void Continue() { running_ = true; }
    void Stop()     { running_ = false; }

    /** Call regularly: notices that ticks have stopped. */
    void Poll(uint32_t now_us)
    {
        if (running_ && count_ > 0 && (uint32_t)(now_us - last_us_) > kGapUs) running_ = false;
    }

    /** Beat length in seconds; 0 until a tempo has been measured. Held after the clock stops. */
    float    PeriodSec() const { return period_; }
    uint32_t Edges() const { return edges_; }
    /** Ticks are arriving right now. */
    bool     Running() const { return running_; }

  private:
    static constexpr int      kMinTicks = 6;
    static constexpr uint32_t kGapUs    = 500000u;   // 0.5 s without a tick

    uint32_t times_[kPpqn + 1] = {};
    int      count_ = 0, head_ = 0, phase_ = 0;
    float    period_ = 0.0f;
    uint32_t edges_ = 0, last_us_ = 0;
    bool     running_ = false;
};

/** MIDI CC 0..127 -> knob 0..1, with 64 landing exactly on noon. */
inline float CcToKnob(uint8_t v)
{
    if (v > 127) v = 127;
    return (v <= 64) ? (float)v / 128.0f : 0.5f + (float)(v - 64) / 126.0f;
}

} // namespace athanor
