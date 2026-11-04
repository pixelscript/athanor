/**
 * @file modfx.h
 * @brief Athanor row 3, variants 2 and 3 — modulation effects.
 *
 *   ChorusTrem     CCW chorus   (amount = depth + mix)
 *                  CW  tremolo  (amount = depth; shape squares up, then pans)
 *   PhaserFlanger  CCW phaser   (amount = sweep width + feedback + mix)
 *                  CW  flanger  (amount = sweep width + feedback + mix)
 *
 * MODIFIER = LFO rate. With a clock on J3 the rate snaps to musical divisions
 * of the clock and the LFO phase-locks to the clock edges.
 */
#pragma once

#include "fx.h"

namespace athanor {

/** Rate for one effect: free Hz, or cycles per clock pulse when clocked. */
struct LfoRate
{
    float hz;              // cycles per second (always valid)
    float per_edge;        // cycles per clock pulse (0 = free running)
};

class ChorusTrem
{
  public:
    static constexpr uint32_t kFloats = 2048;   // per channel, ~42 ms

    /** mem: 2 * kFloats floats (caller-owned, e.g. SDRAM). */
    void Init(float sr, float* mem);
    void SetParams(int side, float amount, LfoRate rate, uint32_t clock_edges);
    void Process(float* l, float* r, std::size_t n);
    bool Busy() const { return mix_c_.v > 1.0e-5f || mix_t_.v > 1.0e-5f
                            || t_mix_c_ > 0.0f || t_mix_t_ > 0.0f; }

  private:
    float     sr_ = 48000.0f;
    float*    buf_[2] = {nullptr, nullptr};   // caller-owned (SDRAM)
    DelayLine dl_[2];
    SyncLfo   lfo_;
    float     hz_ = 1.0f;
    Smooth    mix_c_, mix_t_, amt_, hz_sm_;
    float     t_mix_c_ = 0.0f, t_mix_t_ = 0.0f, t_amt_ = 0.0f;
};

class PhaserFlanger
{
  public:
    static constexpr int      kStages = 6;
    static constexpr uint32_t kFloats = 1024;   // per channel, ~21 ms

    /** mem: 2 * kFloats floats (caller-owned, e.g. SDRAM). */
    void Init(float sr, float* mem);
    void SetParams(int side, float amount, LfoRate rate, uint32_t clock_edges);
    void Process(float* l, float* r, std::size_t n);
    bool Busy() const { return mix_p_.v > 1.0e-5f || mix_f_.v > 1.0e-5f
                            || t_mix_p_ > 0.0f || t_mix_f_ > 0.0f; }

  private:
    float     sr_ = 48000.0f;
    float*    buf_[2] = {nullptr, nullptr};   // caller-owned (SDRAM)
    DelayLine dl_[2];
    float     ap_z_[2][kStages] = {};
    float     ph_fb_[2] = {0.0f, 0.0f};
    float     ap_k_[2]  = {0.0f, 0.0f};
    float     fl_fb_[2] = {0.0f, 0.0f};
    DcBlock   fl_dc_[2];
    SyncLfo   lfo_;
    float     hz_ = 0.3f;
    Smooth    mix_p_, mix_f_, amt_, hz_sm_;
    float     t_mix_p_ = 0.0f, t_mix_f_ = 0.0f, t_amt_ = 0.0f;
};

} // namespace athanor
