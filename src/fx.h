/**
 * @file fx.h
 * @brief Athanor rows 2 and 3 — overdrive/wavefolder and reverb/delay.
 *
 * Pure DSP, no hardware dependencies (host-testable). Large buffers live in
 * SDRAM and are handed in by the caller via Init().
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace athanor {

constexpr float kPi = 3.14159265358979f;

inline float Clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* ── One-pole helpers ──────────────────────────────────────────────────── */

/** One-pole lowpass, coefficient from cutoff in Hz. */
struct OnePoleLp
{
    float z = 0.0f, a = 1.0f;
    void  SetHz(float hz, float sr) { a = 1.0f - std::exp(-2.0f * kPi * hz / sr); }
    float Tick(float x) { z += a * (x - z); return z; }
};

/** Parameter smoother: exponential approach, coefficient per call. */
struct Smooth
{
    float v = 0.0f, k = 0.1f;
    void  Init(float value, float coeff) { v = value; k = coeff; }
    float Tick(float target) { v += k * (target - v); return v; }
};

/** DC blocker (one-pole highpass, ~10 Hz). */
struct DcBlock
{
    float x1 = 0.0f, y1 = 0.0f, r = 0.9987f;
    float Tick(float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
};

/** Cheap white noise, -1..1 (xorshift32). */
struct Rng
{
    uint32_t s = 0x12345678u;
    inline uint32_t NextU() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline float    Next() { return (float)(int32_t)NextU() * (1.0f / 2147483648.0f); }
    inline float    Uni()  { return (float)(NextU() >> 8) * (1.0f / 16777216.0f); }   // 0..1
};

inline float Frac(float x) { return x - std::floor(x); }

/**
 * Phase accumulator LFO (0..1) that can lock to an external clock: on every
 * clock edge its phase is pulled halfway toward where it should be for
 * `cycles_per_edge` cycles per clock pulse.
 */
struct SyncLfo
{
    float    ph = 0.0f;
    uint32_t last_edges = 0;

    inline void Advance(float inc) { ph += inc; if (ph >= 1.0f) ph -= 1.0f; }
    void OnClock(uint32_t edges, float cycles_per_edge)
    {
        if (edges == last_edges) return;
        last_edges = edges;
        if (cycles_per_edge <= 0.0f) return;
        const float expect = Frac((float)(edges % 4096u) * cycles_per_edge);
        float err = ph - expect;
        err -= std::floor(err + 0.5f);          // wrap to -0.5..0.5
        ph = Frac(ph - 0.5f * err);
    }
};

/** First-order ADAA state + helper. f = the function, F = its antiderivative. */
struct Adaa
{
    float x1 = 0.0f, F1 = 0.0f;
    bool  live = false;

    template <typename Fn, typename AFn>
    inline float Run(float x, Fn f, AFn Fa)
    {
        const float Fx = Fa(x);
        if (!live) { x1 = x; F1 = Fx; live = true; return f(x); }
        const float d = x - x1;
        const float y = (std::fabs(d) > 1.0e-3f) ? (Fx - F1) / d : f(0.5f * (x + x1));
        x1 = x;
        F1 = Fx;
        return y;
    }
    inline void Idle() { live = false; }
};

/* ── Row 3: reverb / reverb + delay ───────────────────────────────────── */

/** Fractional delay line over a caller-owned buffer of any length. */
struct DelayLine
{
    float*   buf  = nullptr;
    uint32_t size = 1;
    uint32_t w    = 0;   // next write slot

    void Init(float* b, uint32_t n)
    {
        buf = b; size = n ? n : 1; w = 0;
        for (uint32_t i = 0; i < size; i++) buf[i] = 0.0f;
    }
    inline void Write(float x)
    {
        buf[w] = x;
        if (++w >= size) w = 0;
    }
    /** Integer tap: d >= 1 is the sample written d writes ago (d < size). */
    inline float Tap(uint32_t d) const
    {
        int32_t i = (int32_t)w - (int32_t)d;
        if (i < 0) i += (int32_t)size;
        return buf[i];
    }
    /** Fractional tap (linear), 1 <= d < size - 1. */
    inline float TapF(float d) const
    {
        const uint32_t di = (uint32_t)d;
        const float    f  = d - (float)di;
        int32_t i = (int32_t)w - (int32_t)di;
        if (i < 0) i += (int32_t)size;
        int32_t j = i - 1;
        if (j < 0) j += (int32_t)size;
        return buf[i] + f * (buf[j] - buf[i]);
    }
};

/** Allpass diffuser built on a DelayLine (Schroeder form). */
struct Allpass
{
    DelayLine d;
    uint32_t  len = 1;
    inline float Tick(float x, float g)
    {
        const float z = d.Tap(len);
        const float v = x + g * z;
        d.Write(v);
        return z - g * v;
    }
    inline float TickMod(float x, float g, float len_f)
    {
        const float z = d.TapF(len_f);
        const float v = x + g * z;
        d.Write(v);
        return z - g * v;
    }
};

/**
 * Dattorro plate reverb (1997 "Effect Design Part 1" topology), delay lengths
 * rescaled from 29.76 kHz to the running rate, with slow modulation on the
 * two tank input allpasses to keep long tails from ringing metallic.
 */
class Plate
{
  public:
    /** Total floats of SDRAM the plate needs (call before Init). */
    static constexpr uint32_t kMemFloats = 40000;   // 160 KB: exact fit at 48 kHz (internal SRAM)

    void Init(float sr, float* mem);
    void SetDecay(float d) { decay_ = Clamp(d, 0.0f, 0.985f); }
    void SetDamping(float d) { damp_ = Clamp(d, 0.0f, 0.95f); }
    /** Mono in, stereo out. */
    void Tick(float in, float& out_l, float& out_r);

  private:
    float sr_ = 48000.0f, scale_ = 1.0f;
    float decay_ = 0.6f, damp_ = 0.3f, bw_ = 0.9995f;
    float bw_z_ = 0.0f, damp_z1_ = 0.0f, damp_z2_ = 0.0f;
    float lfo_ph_ = 0.0f, lfo_inc_ = 0.0f;
    DelayLine pre_;
    uint32_t  pre_len_ = 1;
    Allpass   in_ap_[4];
    Allpass   tank_ap1_[2];          // modulated
    DelayLine tank_d1_[2];
    Allpass   tank_ap2_[2];
    DelayLine tank_d2_[2];
    uint32_t  len_d1_[2], len_d2_[2];
    float     len_ap1_[2];
    uint32_t  tap_[14];
    float     fb_l_ = 0.0f, fb_r_ = 0.0f;
};

/**
 * Space: the end-of-chain ambience.
 *
 *   side -1  reverb only.   amount = send/wet,  modifier = reverb decay
 *   side +1  delay → reverb amount = send/wet,  modifier = delay feedback
 *                           (reverb decay follows it more gently)
 *   side  0  neutral — sends close, but tails keep ringing out naturally.
 *
 * The delay time is set by the caller (J3 clock period, or a fallback).
 */
class Space
{
  public:
    static constexpr uint32_t kDelayFloats = 131072;   // per channel, 2.73 s @ 48 kHz
    static constexpr float    kMaxDelaySec = 2.5f;

    void Init(float sr, float* plate_mem, float* dly_l, float* dly_r);
    void SetParams(int side, float amount, float modifier);
    void SetDelaySeconds(float s);
    void Process(float* l, float* r, std::size_t n);
    /** Peak of the wet (reverb + echo) output over the last Process(). */
    float WetPeak() const { return wet_peak_; }

  private:
    float wet_peak_ = 0.0f;
    float sr_ = 48000.0f;
    Plate plate_;
    DelayLine dl_[2];
    OnePoleLp dl_lp_[2];
    DcBlock   dl_dc_[2];
    float     dly_target_ = 18000.0f;
    Smooth    dly_time_;

    Smooth rev_send_, dly_send_, dly_to_rev_, dry_, dly_fb_;
    float  t_rev_send_ = 0.0f, t_dly_send_ = 0.0f, t_dly_to_rev_ = 0.0f;
    float  t_dry_ = 1.0f, t_dly_fb_ = 0.0f;
    float  rev_decay_ = 0.6f;
    int    last_side_ = -1;
};

/** Final output safety: transparent below 0.8, soft knee above. */
inline float OutputLimit(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.8f) return x;
    const float y = 0.8f + 0.2f * std::tanh((a - 0.8f) * 5.0f);
    return x < 0.0f ? -y : y;
}

} // namespace athanor
