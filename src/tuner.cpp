/**
 * @file tuner.cpp
 * @brief See tuner.h.
 */
#include "tuner.h"

#include <cmath>
#include <cstring>

namespace athanor {

namespace {
constexpr float kTwoPi = 6.28318530717959f;

constexpr float kGuitarHz[Tuner::kBands] = {82.407f, 110.000f, 146.832f, 195.998f, 246.942f, 329.628f};
constexpr float kCHz[Tuner::kBands]      = {32.703f, 65.406f, 130.813f, 261.626f, 523.251f, 1046.502f};

/* Detector bandwidths, as a fraction of the target frequency. Guitar is
 * narrow, to keep neighbouring strings (and most of their overtones) out;
 * the single-note modes are wider, so they respond faster and still hear a
 * note that is a long way off. */
constexpr float kBwGuitar    = 0.035f;
constexpr float kBwC         = 0.08f;
constexpr float kBwChromatic = 0.04f;
constexpr float kMinBwHz     = 6.0f;       // single-note modes: never slower than this

constexpr float kLevelFloor  = 0.0004f;    // about -68 dBFS: below this a detector is silent
constexpr float kRelFloor    = 0.03f;      // and within 30 dB of the loudest detector
constexpr float kInputFloor  = 0.0008f;    // chromatic: input peak needed to look for a note
constexpr float kClarity     = 0.5f;       // chromatic: how periodic the input must be
constexpr uint32_t kMinLag   = 5;          // 4.8 kHz at half rate
constexpr uint32_t kLagsPerPoll = 8;   // ~16k multiply-adds per poll: a full search takes ~110 ms

inline float Cents(float ratio) { return 1200.0f * std::log2(ratio); }
} // namespace

void Tuner::Tune(Detector& d, float hz, float bandwidth)
{
    d.f0 = hz;
    const float w = kTwoPi * hz / sr_;
    d.dc = std::cos(w);
    d.ds = std::sin(w);
    d.c = 1.0f; d.s = 0.0f;
    float fc = bandwidth * hz;
    if (bandwidth != kBwGuitar && fc < kMinBwHz) fc = kMinBwHz;
    d.a = 1.0f - std::exp(-kTwoPi * fc / sr_);
    for (int i = 0; i < 4; i++) d.re[i] = d.im[i] = 0.0f;
    d.pre = d.pim = d.ar = d.ai = d.mag = 0.0f;
    d.cents = 0.0f;
    d.level = 0.0f;
    /* Four poles: allow about eight time constants before believing it. */
    d.settle = (int)(8.0f / (kTwoPi * fc) * sr_ / 64.0f) + 2;
}

void Tuner::Init(float sr, float* mem)
{
    sr_   = sr;
    ring_ = mem;
    snap_ = mem + kRing;
    std::memset(mem, 0, sizeof(float) * kMemFloats);
    rw_ = 0; half_have_ = false; in_peak_ = 0.0f;
    lag_ = 0; note_ = -1; note_cents_ = 0.0f; note_hz_ = 0.0f;
    pending_note_ = -1; pending_count_ = 0; retune_hz_ = 0.0f; silent_ = 0;
    mode_ = kNumModes;      // force the first SetMode to tune the detectors
    SetMode(kCOctaves);
}

void Tuner::SetMode(Mode m)
{
    if (m >= kNumModes) m = kCOctaves;
    if (m == mode_) return;
    /* Called from the control loop while Push() may be running: retune with
     * the detectors parked (mode_ = kNumModes makes Push() skip them). */
    mode_ = kNumModes;
    for (int i = 0; i < kBands; i++)
    {
        if (m == kGuitar)         Tune(band_[i], kGuitarHz[i], kBwGuitar);
        else if (m == kCOctaves)  Tune(band_[i], kCHz[i], kBwC);
        else                      Tune(band_[i], 440.0f, kBwChromatic);
    }
    note_ = -1; pending_note_ = -1; pending_count_ = 0; lag_ = 0; retune_hz_ = 0.0f; silent_ = 0;
    mode_ = m;
}

void Tuner::Run(Detector& d, const float* mono, std::size_t n)
{
    float c = d.c, s = d.s;
    const float dc = d.dc, ds = d.ds, a = d.a;
    float r0 = d.re[0], r1 = d.re[1], r2 = d.re[2], r3 = d.re[3];
    float i0 = d.im[0], i1 = d.im[1], i2 = d.im[2], i3 = d.im[3];
    for (std::size_t k = 0; k < n; k++)
    {
        const float x = mono[k];
        r0 += a * (x * c - r0);  r1 += a * (r0 - r1);  r2 += a * (r1 - r2);  r3 += a * (r2 - r3);
        i0 += a * (-x * s - i0); i1 += a * (i0 - i1);  i2 += a * (i1 - i2);  i3 += a * (i2 - i3);
        const float nc = c * dc - s * ds;
        s = s * dc + c * ds;
        c = nc;
    }
    /* Keep the reference on the unit circle. */
    const float g = 1.5f - 0.5f * (c * c + s * s);
    d.c = c * g; d.s = s * g;
    d.re[0] = r0; d.re[1] = r1; d.re[2] = r2; d.re[3] = r3;
    d.im[0] = i0; d.im[1] = i1; d.im[2] = i2; d.im[3] = i3;

    /* The pointer's rotation since the last block, averaged (weighted by
     * level, so the loud part of a note counts most). ~120 ms. */
    const float rr = r3 * d.pre + i3 * d.pim;
    const float ri = i3 * d.pre - r3 * d.pim;
    d.pre = r3; d.pim = i3;
    const float k = 0.011f;
    d.ar += k * (rr - d.ar);
    d.ai += k * (ri - d.ai);
    const float mag = 2.0f * std::sqrt(r3 * r3 + i3 * i3);
    d.mag += 0.05f * (mag - d.mag);
    if (d.settle > 0) d.settle--;

    if (d.ar != 0.0f || d.ai != 0.0f)
    {
        const float off = std::atan2(d.ai, d.ar) * sr_ / (kTwoPi * (float)n);   // Hz from target
        const float f   = d.f0 + off;
        d.cents = (f > 1.0f) ? Cents(f / d.f0) : -1200.0f;
    }
    d.level = d.mag;
}

void Tuner::Push(const float* l, const float* r, std::size_t n)
{
    const Mode mode = mode_;
    if (mode >= kNumModes) return;
    static float mono[64];
    if (n > 64) n = 64;
    float pk = in_peak_ * 0.995f;
    for (std::size_t i = 0; i < n; i++)
    {
        const float x = 0.5f * (l[i] + r[i]);
        mono[i] = x;
        const float ax = x < 0.0f ? -x : x;
        if (ax > pk) pk = ax;
    }
    in_peak_ = pk;

    if (mode == kChromatic)
    {
        /* Half-rate copy for the pitch search. */
        uint32_t w = rw_;
        for (std::size_t i = 0; i < n; i++)
        {
            if (half_have_) { ring_[w & (kRing - 1u)] = 0.5f * (half_ + mono[i]); w++; half_have_ = false; }
            else            { half_ = mono[i]; half_have_ = true; }
        }
        rw_ = w;
        const float hz = retune_hz_;
        if (hz > 0.0f) { Tune(band_[0], hz, kBwChromatic); retune_hz_ = 0.0f; }
        Run(band_[0], mono, n);
    }
    else
        for (int b = 0; b < kBands; b++) Run(band_[b], mono, n);
}

Tuner::Reading Tuner::Band(int i) const
{
    Reading out;
    if (i < 0 || i >= kBands || (mode_ != kGuitar && mode_ != kCOctaves)) return out;
    float mx = 0.0f;
    for (int b = 0; b < kBands; b++) if (band_[b].level > mx) mx = band_[b].level;
    const Detector& d = band_[i];
    const float lv = d.level;
    out.active = d.settle == 0 && lv > kLevelFloor && lv > kRelFloor * mx;
    out.cents  = d.cents;
    /* Brightness: -68 dBFS .. -20 dBFS -> 0.2 .. 1. */
    float t = (std::log10(lv > 1.0e-6f ? lv : 1.0e-6f) + 3.4f) / 2.4f;
    out.level = t < 0.2f ? 0.2f : (t > 1.0f ? 1.0f : t);
    return out;
}

int Tuner::Fundamental() const
{
    for (int b = 0; b < kBands; b++) if (Band(b).active) return b;
    return -1;
}

/* ── Chromatic: pitch search, a few lags per call ────────────────────────
 * McLeod & Wyvill's normalised square difference: n(t) = 2 r(t) / m(t), with
 * r the autocorrelation over the window and m the energy of the two spans
 * being compared. It peaks at 1 for a perfectly periodic signal at its
 * period; the first peak nearly as high as the highest is the period (taking
 * simply the highest can land on a multiple of it). */
void Tuner::Poll()
{
    if (mode_ != kChromatic) return;

    /* Fine reading from the detector once it has settled on the note. */
    if (note_ >= 0)
    {
        const Detector& d = band_[0];
        const bool fine = d.settle == 0 && retune_hz_ == 0.0f && d.level > kLevelFloor;
        note_cents_ = fine ? (float)d.cents : rough_cents_;
    }

    if (lag_ == 0)
    {
        if (in_peak_ < kInputFloor)
        {
            if (++silent_ > 200) { note_ = -1; pending_note_ = -1; pending_count_ = 0; }   // ~0.2 s of polls
            return;
        }
        /* Snapshot the newest kWindow + kMaxLag half-rate samples. */
        const uint32_t total = kWindow + kMaxLag;
        const uint32_t start = rw_ - total;
        for (uint32_t i = 0; i < total; i++) snap_[i] = ring_[(start + i) & (kRing - 1u)];
        double e0 = 0.0;
        for (uint32_t i = 0; i < kWindow; i++) e0 += (double)snap_[i] * snap_[i];
        /* m(lag) = e0 + energy of snap[lag .. lag + W): start it at kMinLag - 1. */
        double e = 0.0;
        const uint32_t first = kMinLag - 1u;
        for (uint32_t i = 0; i < kWindow; i++) e += (double)snap_[first + i] * snap_[first + i];
        m_ = e;
        e0_ = (float)e0;
        nsdf_prev_ = nsdf_prev2_ = -1.0f;   // no peak can be claimed before two real values
        in_peak_region_ = false;
        peak_val_ = 0.0f; peak_lag_ = 0.0f;
        cand_n_ = 0;
        lag_ = kMinLag;
        return;
    }

    const float e0 = e0_;
    static bool went_negative = false;
    if (lag_ == kMinLag) went_negative = false;
    for (uint32_t k = 0; k < kLagsPerPoll && lag_ <= kMaxLag; k++, lag_++)
    {
        const uint32_t t = lag_;
        /* Slide the second span's energy from lag t-1 to t. */
        m_ += (double)snap_[t - 1u + kWindow] * snap_[t - 1u + kWindow] - (double)snap_[t - 1u] * snap_[t - 1u];
        float r = 0.0f;
        const float* a = snap_;
        const float* b = snap_ + t;
        for (uint32_t i = 0; i < kWindow; i++) r += a[i] * b[i];
        const float m = e0 + (float)m_;
        const float v = (m > 1.0e-12f) ? 2.0f * r / m : 0.0f;

        if (v < 0.0f) went_negative = true;
        if (went_negative)
        {
            /* Local maximum at t-1? Refine it with a parabola through its neighbours. */
            if (nsdf_prev_ > 0.0f && nsdf_prev_ >= nsdf_prev2_ && nsdf_prev_ > v)
            {
                const float den = nsdf_prev2_ - 2.0f * nsdf_prev_ + v;
                const float dx  = (den != 0.0f) ? 0.5f * (nsdf_prev2_ - v) / den : 0.0f;
                const float pv  = nsdf_prev_ - 0.25f * (nsdf_prev2_ - v) * dx;
                if (!in_peak_region_ || pv > peak_val_) { peak_val_ = pv; peak_lag_ = (float)(t - 1u) + dx; }
                in_peak_region_ = true;
            }
            if (v <= 0.0f && in_peak_region_)
            {
                if (cand_n_ < 16) { cand_val_[cand_n_] = peak_val_; cand_lag_[cand_n_] = peak_lag_; cand_n_++; }
                in_peak_region_ = false;
            }
        }
        nsdf_prev2_ = nsdf_prev_;
        nsdf_prev_  = v;
    }
    if (lag_ > kMaxLag)
    {
        if (in_peak_region_ && cand_n_ < 16) { cand_val_[cand_n_] = peak_val_; cand_lag_[cand_n_] = peak_lag_; cand_n_++; }
        FinishSearch();
        lag_ = 0;
    }
}

void Tuner::FinishSearch()
{
    float mx = 0.0f;
    for (int i = 0; i < cand_n_; i++) if (cand_val_[i] > mx) mx = cand_val_[i];
    float lag = 0.0f;
    if (mx >= kClarity)
        for (int i = 0; i < cand_n_; i++)
            if (cand_val_[i] >= 0.9f * mx) { lag = cand_lag_[i]; break; }

    /* A short period is only known to a fraction of a sample. Later peaks
     * are whole multiples of it: take the furthest good one and divide, which
     * sharpens the estimate by that multiple (it matters for high notes, where
     * a note sitting near a border would otherwise be named as its neighbour). */
    if (lag >= (float)kMinLag)
        for (int i = cand_n_ - 1; i >= 0; i--)
        {
            if (cand_val_[i] < 0.8f * mx) continue;
            const float mult = std::floor(cand_lag_[i] / lag + 0.5f);
            if (mult < 2.0f) break;
            const float dev = cand_lag_[i] / mult - lag;
            if (dev < 0.03f * lag && dev > -0.03f * lag) { lag = cand_lag_[i] / mult; break; }
        }

    if (lag < (float)kMinLag)
    {
        if (++silent_ > 6) { note_ = -1; pending_note_ = -1; pending_count_ = 0; }
        return;
    }
    silent_ = 0;
    const float hz   = 0.5f * sr_ / lag;
    const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
    const int   note = (int)std::floor(midi + 0.5f);
    if (note < 21 || note > 108) return;                      // A0 .. C8
    const float note_hz = 440.0f * std::exp2((float)(note - 69) / 12.0f);
    rough_cents_ = Cents(hz / note_hz);

    if (note == pending_note_) pending_count_++;
    else { pending_note_ = note; pending_count_ = 1; }
    /* Two searches in a row agreeing changes the note (no flicker between
     * neighbours when a pitch sits on the border). */
    if (note != note_ && pending_count_ >= 2)
    {
        note_       = note;
        note_hz_    = note_hz;
        note_cents_ = rough_cents_;
        retune_hz_  = note_hz;        // audio thread points the detector at it
    }
}

} // namespace athanor
