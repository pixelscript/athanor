/**
 * @file drive.h
 * @brief Athanor row 2 — three drive levels, each with an analog (CCW) and a
 *        digital (CW) flavour. B2 picks the level.
 *
 *   Level   Analog (CCW)                         Digital (CW)
 *   ─────   ──────────────────────────────────   ──────────────────────────────
 *   1 Sat   tube-ish asymmetric soft saturation   clean digital hard clipping
 *   2 Dist  mid-pushed diode-style clipping       bit-crush + sample-rate reduction
 *   3 Fuzz  gated, sputtery asymmetric fuzz       wavefolder + heavy crush
 *
 * Analog shapers are ADAA anti-aliased; the digital ones alias on purpose.
 * TONE is a tilt EQ on the driven signal (noon flat). Level changes crossfade.
 */
#pragma once

#include "fx.h"

namespace athanor {

class Drive
{
  public:
    static constexpr int kLevels = 3;

    void Init(float sr);

    /** side -1 analog / +1 digital / 0 bypass; amount 0..1; tone 0..1. */
    void SetParams(int level, int side, float amount, float tone);

    void Process(float* l, float* r, std::size_t n);

  private:
    struct Chan
    {
        Adaa      sat, dist, fuzz, clip, fold;
        OnePoleLp pre_hp, dist_lp, fuzz_lp, fuzz_env;
        OnePoleLp tilt;
        DcBlock   dc, fuzz_dc;
        float     hold[kLevels] = {0.0f, 0.0f, 0.0f};   // sample & hold (digital)
    };

    float Analog(int level, Chan& c, float x, float a);
    float Digital(int level, Chan& c, float x, float a, bool take);

    Chan   ch_[2];
    float  sr_ = 48000.0f;
    float  sh_ph_[kLevels] = {0.0f, 0.0f, 0.0f};   // decimator phases (shared L/R)

    Smooth lvl_w_[kLevels];
    Smooth mix_a_, mix_d_, amt_, gl_, gh_;
    float  t_lvl_w_[kLevels] = {1.0f, 0.0f, 0.0f};
    float  t_mix_a_ = 0.0f, t_mix_d_ = 0.0f, t_amt_ = 0.0f;
    float  t_gl_ = 1.0f, t_gh_ = 1.0f;
};

} // namespace athanor
