/**
 * @file fx.cpp
 * @brief See fx.h.
 */
#include "fx.h"

namespace athanor {

/* ═════════════════════════════════════════════════════════════════════════
 *  Plate
 * ═════════════════════════════════════════════════════════════════════════ */

namespace {
/* Bump allocator over the caller's block. */
struct Arena
{
    float*   base;
    uint32_t used, cap;
    float*   Take(uint32_t n)
    {
        if (used + n > cap) n = (cap > used) ? cap - used : 0;   // degrade, never overrun
        float* p = base + used;
        used += n;
        return p;
    }
};

void InitLine(DelayLine& d, Arena& a, uint32_t need)
{
    const uint32_t sz = need + 4;
    d.Init(a.Take(sz), sz);
}
} // namespace

void Plate::Init(float sr, float* mem)
{
    sr_    = sr;
    scale_ = sr / 29761.0f;
    Arena a{mem, 0, kMemFloats};
    auto S = [this](float n) { return (uint32_t)(n * scale_ + 0.5f); };

    pre_len_ = (uint32_t)(0.012f * sr);           // 12 ms pre-delay
    InitLine(pre_, a, pre_len_);

    const float in_lens[4] = {142.f, 107.f, 379.f, 277.f};
    for (int i = 0; i < 4; i++)
    {
        in_ap_[i].len = S(in_lens[i]);
        InitLine(in_ap_[i].d, a, in_ap_[i].len);
    }

    const float ap1[2] = {672.f, 908.f};
    const float d1[2]  = {4453.f, 4217.f};
    const float ap2[2] = {1800.f, 2656.f};
    const float d2[2]  = {3720.f, 3163.f};
    for (int i = 0; i < 2; i++)
    {
        len_ap1_[i] = ap1[i] * scale_;
        InitLine(tank_ap1_[i].d, a, (uint32_t)(len_ap1_[i] + 32.0f * scale_));
        len_d1_[i] = S(d1[i]);
        InitLine(tank_d1_[i], a, len_d1_[i]);
        tank_ap2_[i].len = S(ap2[i]);
        /* Dattorro's left output taps the first 1800-sample allpass at
         * 1913, past its own length. Size the line to cover the tap, or the
         * read wraps into the neighbouring buffer and clicks. */
        InitLine(tank_ap2_[i].d, a, (uint32_t)((i == 0 ? 1913.f : ap2[i]) * scale_ + 0.5f) + 8u);
        len_d2_[i] = S(d2[i]);
        InitLine(tank_d2_[i], a, len_d2_[i]);
    }

    /* Output taps (Dattorro table), left then right. */
    const float taps[14] = {266.f, 2974.f, 1913.f, 1996.f, 1990.f, 187.f, 1066.f,
                            353.f, 3627.f, 1228.f, 2673.f, 2111.f, 335.f, 121.f};
    for (int i = 0; i < 14; i++) tap_[i] = S(taps[i]);

    lfo_ph_  = 0.0f;
    lfo_inc_ = 0.6f / sr;   // ~0.6 Hz tank wander
    bw_z_ = damp_z1_ = damp_z2_ = fb_l_ = fb_r_ = 0.0f;
}

void Plate::Tick(float in, float& out_l, float& out_r)
{
    /* Input: pre-delay → bandwidth LP → four diffusers. */
    pre_.Write(in);
    float x = pre_.Tap(pre_len_);
    bw_z_ += bw_ * (x - bw_z_);
    x = bw_z_;
    x = in_ap_[0].Tick(x, 0.75f);
    x = in_ap_[1].Tick(x, 0.75f);
    x = in_ap_[2].Tick(x, 0.625f);
    x = in_ap_[3].Tick(x, 0.625f);

    /* Tank modulation: two quadrature sine-ish LFOs (parabolic approx). */
    lfo_ph_ += lfo_inc_;
    if (lfo_ph_ >= 1.0f) lfo_ph_ -= 1.0f;
    auto tri = [](float p) { p -= std::floor(p); return 4.0f * std::fabs(p - 0.5f) - 1.0f; };
    const float exc = 12.0f * scale_;
    const float m0  = len_ap1_[0] + exc * tri(lfo_ph_);
    const float m1  = len_ap1_[1] + exc * tri(lfo_ph_ + 0.25f);

    const float dd1 = 0.70f;   // decay diffusion 1
    const float dd2 = 0.50f;   // decay diffusion 2

    /* Half A (fed by half B's tail). */
    float a = x + fb_r_;
    a = tank_ap1_[0].TickMod(a, -dd1, m0);
    tank_d1_[0].Write(a);
    a = tank_d1_[0].Tap(len_d1_[0]);
    damp_z1_ += (1.0f - damp_) * (a - damp_z1_);
    a = damp_z1_ * decay_;
    a = tank_ap2_[0].Tick(a, dd2);
    tank_d2_[0].Write(a);
    const float tail_a = tank_d2_[0].Tap(len_d2_[0]) * decay_;

    /* Half B (fed by half A's tail). */
    float b = x + fb_l_;
    b = tank_ap1_[1].TickMod(b, -dd1, m1);
    tank_d1_[1].Write(b);
    b = tank_d1_[1].Tap(len_d1_[1]);
    damp_z2_ += (1.0f - damp_) * (b - damp_z2_);
    b = damp_z2_ * decay_;
    b = tank_ap2_[1].Tick(b, dd2);
    tank_d2_[1].Write(b);
    const float tail_b = tank_d2_[1].Tap(len_d2_[1]) * decay_;

    fb_l_ = tail_a;   // A → B
    fb_r_ = tail_b;   // B → A

    float l = 0.0f, r = 0.0f;
    l += tank_d1_[0].Tap(tap_[0]);
    l += tank_d1_[0].Tap(tap_[1]);
    l -= tank_ap2_[0].d.Tap(tap_[2]);
    l += tank_d2_[0].Tap(tap_[3]);
    l -= tank_d1_[1].Tap(tap_[4]);
    l -= tank_ap2_[1].d.Tap(tap_[5]);
    l -= tank_d2_[1].Tap(tap_[6]);

    r += tank_d1_[1].Tap(tap_[7]);
    r += tank_d1_[1].Tap(tap_[8]);
    r -= tank_ap2_[1].d.Tap(tap_[9]);
    r += tank_d2_[1].Tap(tap_[10]);
    r -= tank_d1_[0].Tap(tap_[11]);
    r -= tank_ap2_[0].d.Tap(tap_[12]);
    r -= tank_d2_[0].Tap(tap_[13]);

    out_l = 0.35f * l;
    out_r = 0.35f * r;
}

/* ═════════════════════════════════════════════════════════════════════════
 *  Space
 * ═════════════════════════════════════════════════════════════════════════ */

namespace {
/* Cubic soft clip: linear-ish to ±0.5, saturates to ±1 at |x| = 1.5. */
inline float LoopClip(float x)
{
    x = Clamp(x, -1.5f, 1.5f);
    return x - (x * x * x) * (1.0f / 6.75f);
}
} // namespace

void Space::Init(float sr, float* plate_mem, float* dly_l, float* dly_r)
{
    sr_ = sr;
    plate_.Init(sr, plate_mem);
    rev_hp_.SetHz(150.0f, sr);
    plate_.SetDecay(0.6f);
    plate_.SetDamping(0.3f);
    dl_[0].Init(dly_l, kDelayFloats);
    dl_[1].Init(dly_r, kDelayFloats);
    for (int i = 0; i < 2; i++)
    {
        dl_lp_[i].SetHz(5500.0f, sr);
        dl_dc_[i].r = 1.0f - 2.0f * kPi * 60.0f / sr;   // ~60 Hz HP in the loop
    }
    dly_target_ = 0.375f * sr;
    /* Delay time glides (~120 ms) so clock changes sound like tape, not clicks. */
    dly_time_.Init(dly_target_, 1.0f - std::exp(-1.0f / (0.12f * sr)));
    xf_     = 0.0f;
    xf_dec_ = 1.0f / (0.06f * sr);   // 60 ms crossfade between subdivisions

    const float k = 1.0f - std::exp(-1.0f / (0.01f * sr));   // 10 ms
    rev_send_.Init(0.0f, k);
    dly_send_.Init(0.0f, k);
    dly_to_rev_.Init(0.0f, k);
    dry_.Init(1.0f, k);
    dly_fb_.Init(0.0f, k);
}

void Space::SetDelaySeconds(float s, bool crossfade)
{
    /* Too-long clocks halve until they fit the buffer. */
    while (s > kMaxDelaySec) s *= 0.5f;
    if (s < 0.02f) s = 0.02f;
    const float target = s * sr_;
    if (crossfade && target != dly_target_)
    {
        /* Keep reading the old time while it fades out; the new tap starts
         * at the new time straight away (no glide, so no pitch swoop). */
        xf_old_     = dly_time_.v;
        xf_         = 1.0f;
        dly_time_.v = target;
    }
    dly_target_ = target;
}

void Space::SetParams(int side, float amount, float modifier)
{
    amount   = Clamp(amount, 0.0f, 1.0f);
    modifier = Clamp(modifier, 0.0f, 1.0f);
    const float engage = Clamp(amount / 0.06f, 0.0f, 1.0f);

    /* Sends: the amount fades the signal INTO the effects. Returns are always
     * open, so when you snap back to the deadband the tails ring out. */
    t_rev_send_   = (side != 0) ? engage * amount : 0.0f;
    t_dly_send_   = (side > 0)  ? engage * amount : 0.0f;
    t_dry_        = (side != 0) ? 1.0f - 0.65f * amount * amount : 1.0f;

    /* The modifier keeps following the last side used, even from the
     * deadband, so you can shape the tail while it decays. */
    if (side != 0) last_side_ = side;

    if (last_side_ < 0)
    {
        rev_decay_     = 0.30f + 0.67f * modifier;   // short room → near-freeze
        t_dly_fb_      = 0.0f;
        t_dly_to_rev_  = 0.0f;
        plate_.SetDamping(0.25f);
    }
    else
    {
        rev_decay_     = 0.40f + 0.45f * modifier;
        t_dly_fb_      = 0.98f * modifier;           // very long, but never runs away
        t_dly_to_rev_  = 1.0f;
        plate_.SetDamping(0.40f);
    }
    plate_.SetDecay(rev_decay_);
}

void Space::Process(float* l, float* r, std::size_t n)
{
    float pk = 0.0f;
    for (std::size_t i = 0; i < n; i++)
    {
        const float rs  = rev_send_.Tick(t_rev_send_);
        const float ds  = dly_send_.Tick(t_dly_send_);
        const float dtr = dly_to_rev_.Tick(t_dly_to_rev_);
        const float dry = dry_.Tick(t_dry_);
        const float fb  = dly_fb_.Tick(t_dly_fb_);
        float dt = dly_time_.Tick(dly_target_);
        if (dt > (float)(kDelayFloats - 4)) dt = (float)(kDelayFloats - 4);

        /* Stereo delay, 25% cross-feed in the loop for width. */
        float yl = dl_[0].TapF(dt);
        float yr = dl_[1].TapF(dt);
        if (xf_ > 0.0f)
        {
            const float g = xf_ * xf_ * (3.0f - 2.0f * xf_);   // smooth old-tap weight
            yl += g * (dl_[0].TapF(xf_old_) - yl);
            yr += g * (dl_[1].TapF(xf_old_) - yr);
            xf_ -= xf_dec_;
        }
        const float fl = dl_dc_[0].Tick(dl_lp_[0].Tick(0.75f * yl + 0.25f * yr));
        const float fr = dl_dc_[1].Tick(dl_lp_[1].Tick(0.75f * yr + 0.25f * yl));
        dl_[0].Write(LoopClip(l[i] * ds + fl * fb));
        dl_[1].Write(LoopClip(r[i] * ds + fr * fb));

        /* Plate: mono-summed send plus the echoes (in delay mode). */
        float rin = 0.5f * (l[i] + r[i]) * rs + 0.25f * (yl + yr) * dtr;
        rin -= rev_hp_.Tick(rin);   // low cut into the plate: no boomy build-up
        float rl, rr;
        plate_.Tick(rin, rl, rr);

        const float wl = 0.6f * yl + rl;
        const float wr = 0.6f * yr + rr;
        pk = std::fmax(pk, std::fmax(std::fabs(wl), std::fabs(wr)));
        l[i] = l[i] * dry + wl;
        r[i] = r[i] * dry + wr;
    }
    wet_peak_ = pk;
}

} // namespace athanor
