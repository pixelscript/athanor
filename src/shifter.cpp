/**
 * @file shifter.cpp
 * @brief See shifter.h.
 */
#include "shifter.h"

#include <cmath>
#include <cstring>

namespace athanor {

namespace {
constexpr uint32_t kMask      = Shifter::kSize - 1u;
constexpr float    kHardMin   = 32.0f;     // never read closer to the input than this
constexpr int      kSearch    = 384;       // splice search: +-8 ms (a period down to ~62 Hz)
constexpr int      kCandStep  = 4;
constexpr int      kSegPoints = 40;        // matched segment: 40 points x 6 = 5 ms
constexpr int      kSegStride = 6;
constexpr float    kGrainSec  = 0.045f;    // time between splices, nominal
constexpr float    kMaxJumpDn = 2400.0f;   // caps the delay at extreme ratios: 50 ms down,
constexpr float    kMaxJumpUp = 1150.0f;   //   24 ms up (plus the crossfade's own travel)
constexpr float    kMaxFadeS  = 0.010f;
constexpr float    kPi        = 3.14159265358979f;
} // namespace

void Shifter::Init(float sr, float* mem)
{
    sr_ = sr;
    l_  = mem;
    r_  = mem + kSize;
    m_  = mem + 2 * kSize;
    min_delay_ = kHardMin + (float)kSearch;
    ratio_ = 1.0f;
    Reset();
}

void Shifter::Reset()
{
    std::memset(l_, 0, sizeof(float) * kSize);
    std::memset(r_, 0, sizeof(float) * kSize);
    std::memset(m_, 0, sizeof(float) * kSize);
    w_      = 0;
    d_      = min_delay_ + 600.0f;
    d2_     = d_;
    fade_   = 0.0f;
    fading_ = false;
}

void Shifter::SetRatio(float r)
{
    if (r < 0.2f) r = 0.2f;
    if (r > 4.5f) r = 4.5f;
    ratio_ = r;
}

/* 4-point Hermite read, `delay` samples behind the write head. */
inline float Shifter::Read(const float* buf, float delay) const
{
    const float    pos = (float)w_ - delay;            // may be negative: wrap below
    const float    fl  = std::floor(pos);
    const float    t   = pos - fl;
    const uint32_t i   = (uint32_t)(int32_t)fl;
    const float xm1 = buf[(i - 1u) & kMask], x0 = buf[i & kMask];
    const float x1  = buf[(i + 1u) & kMask], x2 = buf[(i + 2u) & kMask];
    const float c = 0.5f * (x1 - xm1);
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + 0.5f * (x2 - x0);
    const float b = w + a;
    return ((a * t - b) * t + c) * t + x0;
}

/* Pick where the incoming tap starts, and begin the crossfade. The nominal
 * start is one "jump" away from the playing tap; within +-kSearch of it, take
 * the delay whose recent waveform best matches what the playing tap has just
 * read, so the two are in phase while they overlap. */
void Shifter::StartSplice()
{
    const float rate = 1.0f - ratio_;                   // delay change per sample
    const float mag  = std::fabs(rate);
    float jump = mag * kGrainSec * sr_;
    const float cap = (rate > 0.0f) ? kMaxJumpDn : kMaxJumpUp;
    if (jump > cap) jump = cap;
    const float grain = jump / mag;                     // samples until the next splice
    float fade = 0.4f * grain;
    if (fade > kMaxFadeS * sr_) fade = kMaxFadeS * sr_;
    if (fade < 32.0f) fade = 32.0f;

    /* Pitch down: the delay has grown, restart near the input. Pitch up: the
     * delay has shrunk, restart further back. */
    const int   d_int   = (int)(d_ + 0.5f);
    const int   nominal = (rate > 0.0f) ? (int)(min_delay_ + 0.5f)
                                        : d_int + (int)(jump + fade * mag + 0.5f);
    const int   max_c   = (int)kSize - kSegPoints * kSegStride - 8;

    const uint32_t ref = w_ - (uint32_t)d_int;
    auto score_at = [&](int c, int k) -> float {
        const uint32_t cand = w_ - (uint32_t)c;
        float corr = 0.0f, energy = 1.0e-9f;
        for (int i = 1; i <= kSegPoints; i++)
        {
            const uint32_t off = (uint32_t)(i * kSegStride);
            const float a = m_[(ref - off) & kMask];
            const float b = m_[(cand - off) & kMask];
            corr   += a * b;
            energy += b * b;
        }
        /* Normalised, with a mild pull toward the nominal spot. */
        const float dist = (float)(k < 0 ? -k : k) / (float)kSearch;
        return corr / std::sqrt(energy) * (1.0f - 0.08f * dist);
    };
    int   best = nominal;
    float best_score = -1.0e30f;
    for (int k = -kSearch; k <= kSearch; k += kCandStep)      // coarse
    {
        const int c = nominal + k;
        if (c < (int)kHardMin || c > max_c) continue;
        const float sc = score_at(c, k);
        if (sc > best_score) { best_score = sc; best = c; }
    }
    const int coarse = best;
    for (int j = -(kCandStep - 1); j <= kCandStep - 1; j++)   // fine: to the sample
    {
        const int c = coarse + j;
        if (j == 0 || c < (int)kHardMin || c > max_c) continue;
        const float sc = score_at(c, c - nominal);
        if (sc > best_score) { best_score = sc; best = c; }
    }

    d2_       = d_ + (float)(best - d_int);             // keep the fractional phase
    fade_     = 0.0f;
    fade_inc_ = 1.0f / fade;
    fading_   = true;
}

namespace { constexpr float kMonoLp = 0.25f; }   // ~2 kHz: steadier waveform matching

void Shifter::Feed(const float* in_l, const float* in_r, std::size_t n)
{
    float lp = m_[(w_ - 1u) & kMask];
    for (std::size_t i = 0; i < n; i++)
    {
        l_[w_] = in_l[i];
        r_[w_] = in_r[i];
        lp += kMonoLp * (0.5f * (in_l[i] + in_r[i]) - lp);
        m_[w_] = lp;
        w_ = (w_ + 1u) & kMask;
    }
}

void Shifter::Process(const float* in_l, const float* in_r,
                      float* out_l, float* out_r, std::size_t n)
{
    const float rate = 1.0f - ratio_;
    const float mag  = std::fabs(rate);
    float jump = mag * kGrainSec * sr_;
    const float cap = (rate > 0.0f) ? kMaxJumpDn : kMaxJumpUp;
    if (jump > cap) jump = cap;
    float fade_len = (mag > 1.0e-4f) ? 0.4f * jump / mag : 0.0f;
    if (fade_len > kMaxFadeS * sr_) fade_len = kMaxFadeS * sr_;
    if (fade_len < 32.0f) fade_len = 32.0f;
    const float travel = fade_len * mag;                // delay covered during one crossfade

    float lp = m_[(w_ - 1u) & kMask];

    for (std::size_t i = 0; i < n; i++)
    {
        const float xl = in_l[i], xr = in_r[i];
        l_[w_] = xl;
        r_[w_] = xr;
        lp += kMonoLp * (0.5f * (xl + xr) - lp);
        m_[w_] = lp;
        w_ = (w_ + 1u) & kMask;

        d_ += rate;
        if (d_ < kHardMin) d_ = kHardMin;
        if (d_ > (float)(kSize - 8u)) d_ = (float)(kSize - 8u);

        if (!fading_ && mag > 1.0e-3f)
        {
            /* Splice in time for the crossfade to finish inside the window. */
            const bool due = (rate > 0.0f) ? (d_ + travel >= min_delay_ + jump)
                                           : (d_ - travel <= min_delay_);
            if (due) StartSplice();
        }

        if (fading_)
        {
            d2_ += rate;
            if (d2_ < kHardMin) d2_ = kHardMin;
            const float g = 0.5f - 0.5f * std::cos(kPi * fade_);
            out_l[i] = Read(l_, d_) + g * (Read(l_, d2_) - Read(l_, d_));
            out_r[i] = Read(r_, d_) + g * (Read(r_, d2_) - Read(r_, d_));
            fade_ += fade_inc_;
            if (fade_ >= 1.0f)
            {
                d_      = d2_;
                fading_ = false;
            }
        }
        else
        {
            out_l[i] = Read(l_, d_);
            out_r[i] = Read(r_, d_);
        }
    }
}

} // namespace athanor
