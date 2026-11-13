/**
 * @file clock_in.h
 * @brief A steady beat from a jittery pulse clock.
 *
 * Pure logic, host-testable. Two parts:
 *
 *   PulseClock  turns rising-edge timestamps into a raw beat period. It
 *               works out by itself whether the source sends one pulse per
 *               beat or 24 per beat (pulses closer than ~150 ms can't be
 *               beats), and measures across several pulses so timestamp
 *               jitter averages out.
 *
 *   TempoLock   turns raw periods into the tempo the effects actually use.
 *               While measurements stay within 1.5% of the locked tempo it
 *               barely moves (a slow drift toward the long-term average), so
 *               a delay following it sits still instead of scrubbing. Two
 *               measurements in a row outside that band are a real tempo
 *               change, and it re-locks to them at once.
 */
#pragma once

#include <cstdint>

namespace athanor {

class TempoLock
{
  public:
    void  Reset() { period_ = 0.0f; out_ = 0; n_ = 0; }
    float Period() const { return period_; }

    /** Feed one raw beat-period measurement (seconds). Call once per beat. */
    void Update(float raw)
    {
        if (raw <= 0.0f) return;
        if (period_ <= 0.0f) { period_ = raw; out_ = 0; n_ = 1; return; }
        const float dev = (raw - period_) / period_;
        if (dev < kBand && dev > -kBand)
        {
            /* A plain running mean for the first beats after locking (the
             * first measurement is the roughest), then a slow long-term
             * average: no audible motion. */
            if (n_ < kSettle) n_++;
            period_ += (raw - period_) / (float)n_;
            out_ = 0;
        }
        else if (++out_ >= kConfirm)
        {
            period_ = raw;                          // a real tempo change
            out_ = 0;
            n_   = 1;
        }
    }

  private:
    static constexpr float kBand    = 0.015f;
    static constexpr int   kSettle  = 20;      // then each beat moves it 5% of the way
    static constexpr int   kConfirm = 2;
    float period_ = 0.0f;
    int   out_ = 0, n_ = 0;
};

class PulseClock
{
  public:
    void Reset()
    {
        count_ = 0; head_ = 0; phase_ = 0; ppqn_ = 0; raw_ = 0.0f; last_us_ = 0; have_last_ = false;
    }

    /** One rising edge. Returns true when this pulse is a beat. */
    bool Edge(uint32_t t_us)
    {
        if (have_last_)
        {
            const uint32_t gap = t_us - last_us_;
            if (gap > kLostUs) { Reset(); }
            else
            {
                /* One pulse per beat or 24? Beats can't be closer than
                 * ~150 ms (400 BPM); 24 ppqn pulses can't be further apart
                 * than 125 ms (20 BPM). Hysteresis between the two. */
                const float g = (float)gap * 1.0e-6f;
                int want = ppqn_;
                if (g < 0.150f)      want = 24;
                else if (g > 0.180f) want = 1;
                else if (want == 0)  want = 1;
                if (want != ppqn_) { ppqn_ = want; count_ = 0; phase_ = 0; raw_ = 0.0f; }
            }
        }
        last_us_   = t_us;
        have_last_ = true;

        times_[head_] = t_us;
        head_ = (head_ + 1) % kRing;
        if (count_ < kRing) count_++;

        if (ppqn_ != 0)
        {
            /* Measure across up to a beat of pulses (24 ppqn) or up to four
             * beats (1 ppqn). If the newest stretch disagrees with the whole
             * window, the tempo has changed: keep only the newest stretch. */
            const int full  = (ppqn_ == 24) ? 24 : 4;
            const int quick = (ppqn_ == 24) ? 6 : 1;
            const int need  = (ppqn_ == 24) ? 6 : 1;
            int span = count_ - 1;
            if (span > full) span = full;
            if (span >= need)
            {
                float beat = Span(span) * (float)ppqn_ / (float)span;
                if (span > quick)
                {
                    const float recent = Span(quick) * (float)ppqn_ / (float)quick;
                    const float d = (recent - beat) / beat;
                    if (d > 0.04f || d < -0.04f) { beat = recent; count_ = quick + 1; }
                }
                if (beat >= 0.15f && beat <= 6.0f) raw_ = beat;
            }
        }

        if (ppqn_ == 0) return false;
        const bool beat_now = (phase_ == 0);
        phase_ = (phase_ + 1) % ppqn_;
        return beat_now;
    }

    /** No pulse for 2.5 beats (at least 1 s): the clock has gone. */
    bool Lost(uint32_t now_us) const
    {
        if (!have_last_) return false;
        float limit = 2.5f * raw_;
        if (limit < 1.0f) limit = 1.0f;
        return (float)(uint32_t)(now_us - last_us_) * 1.0e-6f > limit;
    }

    /** Raw beat period in seconds (0 until measured). */
    float RawPeriod() const { return raw_; }
    /** 1 or 24 once known, 0 before the second pulse. */
    int   Ppqn() const { return ppqn_; }

  private:
    static constexpr int      kRing   = 25;
    static constexpr uint32_t kLostUs = 6500000u;

    /** Seconds covered by the newest `n` intervals. */
    float Span(int n) const
    {
        const int newest = (head_ + kRing - 1) % kRing;
        const int oldest = (newest + kRing - n) % kRing;
        return (float)(uint32_t)(times_[newest] - times_[oldest]) * 1.0e-6f;
    }

    uint32_t times_[kRing] = {};
    int      count_ = 0, head_ = 0, phase_ = 0, ppqn_ = 0;
    float    raw_ = 0.0f;
    uint32_t last_us_ = 0;
    bool     have_last_ = false;
};

} // namespace athanor
