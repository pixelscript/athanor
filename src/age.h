/**
 * @file age.h
 * @brief Athanor row 1, variant 2 — tape (CCW) and vinyl (CW) simulation.
 *
 * AMOUNT is overall "age": it deepens every part of the character together.
 *   Tape:  saturation, head bump, high-frequency loss, a little wow and hiss.
 *   Vinyl: bandwidth narrowing, mono-ing, mild warmth, off-centre wow,
 *          a crackle floor and a little hiss.
 * MODIFIER adds the obvious artefacts on top:
 *   Tape:  more wow and flutter.
 *   Vinyl: more hiss, crackle and clicks.
 */
#pragma once

#include "fx.h"

namespace athanor {

class Age
{
  public:
    static constexpr uint32_t kWowFloats = 2048;   // per channel, ~42 ms

    /** mem: 2 * kWowFloats floats (caller-owned, e.g. SDRAM). */
    void Init(float sr, float* mem);
    /** side -1 tape / +1 vinyl / 0 bypass; amount 0..1; modifier 0..1. */
    void SetParams(int side, float amount, float modifier);
    void Process(float* l, float* r, std::size_t n);
    bool Busy() const { return mix_t_.v > 1.0e-5f || mix_v_.v > 1.0e-5f
                            || t_mix_t_ > 0.0f || t_mix_v_ > 0.0f; }

  private:
    struct Chan
    {
        DelayLine wow;
        OnePoleLp t_hf1, t_hf2, t_bump;                       // tape
        OnePoleLp v_hf1, v_hf2, v_low, v_hiss, v_click_lp;    // vinyl
        float     click = 0.0f;
    };

    float  sr_ = 48000.0f;
    float* buf_[2] = {nullptr, nullptr};   // caller-owned (SDRAM)
    Chan   ch_[2];
    Rng    rng_;

    /* LFOs (0..1 phase) */
    float  wow_ph_ = 0.0f, flut_ph_ = 0.0f, flut2_ph_ = 0.0f, rec_ph_ = 0.0f;
    OnePoleLp flut_noise_lp_;
    float  drift_ = 0.0f, drift_tgt_ = 0.0f, drift_k_ = 0.0f;
    int    drift_timer_ = 0;
    int    drop_timer_ = 0;
    float  drop_env_ = 1.0f;

    Smooth mix_t_, mix_v_, amt_, mod_;
    float  t_mix_t_ = 0.0f, t_mix_v_ = 0.0f, t_amt_ = 0.0f, t_mod_ = 0.0f;
    float  big_click_ = 0.0f;
};

} // namespace athanor
