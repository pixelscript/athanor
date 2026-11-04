/**
 * @file age.cpp
 * @brief See age.h.
 */
#include "age.h"

namespace athanor {

namespace {
inline float Ss(float u) { return u / std::sqrt(1.0f + u * u); }
constexpr float kBaseMs = 6.0f;   // wow delay centre (latency while engaged)
} // namespace

void Age::Init(float sr, float* mem)
{
    sr_ = sr;
    buf_[0] = mem;
    buf_[1] = mem + kWowFloats;
    for (int c = 0; c < 2; c++)
    {
        ch_[c] = Chan{};
        ch_[c].wow.Init(buf_[c], kWowFloats);
        ch_[c].t_bump.SetHz(110.0f, sr);
        ch_[c].v_hiss.SetHz(7000.0f, sr);
        ch_[c].v_click_lp.SetHz(9000.0f, sr);
    }
    drift_lp_.SetHz(0.3f, sr);
    flut_noise_lp_.SetHz(25.0f, sr);
    rng_.s = 0xA7E5u;
    const float k = 1.0f - std::exp(-1.0f / (0.005f * sr));
    mix_t_.Init(0.0f, k);
    mix_v_.Init(0.0f, k);
    amt_.Init(0.0f, k);
    mod_.Init(0.0f, k);
}

void Age::SetParams(int side, float amount, float modifier)
{
    amount = Clamp(amount, 0.0f, 1.0f);
    const float engage = Clamp(amount / 0.06f, 0.0f, 1.0f);
    t_mix_t_ = (side < 0) ? engage : 0.0f;
    t_mix_v_ = (side > 0) ? engage : 0.0f;
    if (side != 0) t_amt_ = amount;
    t_mod_ = Clamp(modifier, 0.0f, 1.0f);
}

void Age::Process(float* l, float* r, std::size_t n)
{
    /* Filter corners follow age, updated once per block. */
    const float ab = amt_.v;
    for (auto& c : ch_)
    {
        const float thf = 16000.0f * std::exp2(-1.9f * ab);   // 16 k → ~4.3 k
        c.t_hf1.SetHz(thf, sr_);
        c.t_hf2.SetHz(thf * 1.3f, sr_);
        const float vhf = 15000.0f * std::exp2(-1.8f * ab);   // 15 k → ~4.3 k
        c.v_hf1.SetHz(vhf, sr_);
        c.v_hf2.SetHz(vhf * 1.2f, sr_);
        c.v_low.SetHz(25.0f + 130.0f * ab, sr_);              // rumble / bass thinning
    }

    const float inv_sr = 1.0f / sr_;
    for (std::size_t i = 0; i < n; i++)
    {
        const float mt = mix_t_.Tick(t_mix_t_);
        const float mv = mix_v_.Tick(t_mix_v_);
        const float a  = amt_.Tick(t_amt_);
        const float m  = mod_.Tick(t_mod_);

        const float xl = l[i], xr = r[i];
        ch_[0].wow.Write(xl);
        ch_[1].wow.Write(xr);
        if (mt < 1.0e-5f && mv < 1.0e-5f) continue;

        float out[2] = {xl, xr};
        const float x[2] = {xl, xr};
        float wet[2] = {0.0f, 0.0f};

        /* ── Tape ─────────────────────────────────────────────────────── */
        if (mt > 1.0e-5f)
        {
            wow_ph_  = Frac(wow_ph_ + 0.65f * inv_sr);
            flut_ph_ = Frac(flut_ph_ + 8.7f * inv_sr);
            const float drift = drift_lp_.Tick(rng_.Next()) * 6.0f;      // slow wander
            const float fn    = flut_noise_lp_.Tick(rng_.Next()) * 3.0f;
            const float wow_ms  = 0.12f * a + 2.4f * m;
            const float flut_ms = 0.02f * a + 0.22f * m;
            const float d_ms = kBaseMs
                + wow_ms  * (0.7f * std::sin(2.0f * kPi * wow_ph_) + 0.3f * Clamp(drift, -1.0f, 1.0f))
                + flut_ms * (0.7f * std::sin(2.0f * kPi * flut_ph_) + 0.3f * Clamp(fn, -1.0f, 1.0f));
            const float d = d_ms * 0.001f * sr_;
            const float g = 1.0f + 3.5f * a;
            const float comp = 0.3f / Ss(0.3f * g);
            for (int c = 0; c < 2; c++)
            {
                Chan& ch = ch_[c];
                float y = comp * Ss(g * ch.wow.TapF(d));
                y += 0.45f * a * ch.t_bump.Tick(y);                       // head bump
                y = ch.t_hf2.Tick(ch.t_hf1.Tick(y));                      // HF loss
                y += 0.0015f * a * rng_.Next();                           // hiss
                wet[c] += mt * y;
            }
        }

        /* ── Vinyl ────────────────────────────────────────────────────── */
        if (mv > 1.0e-5f)
        {
            rec_ph_ = Frac(rec_ph_ + 0.55f * inv_sr);                     // 33⅓ rpm warp
            const float d = (kBaseMs + 0.35f * a * std::sin(2.0f * kPi * rec_ph_)) * 0.001f * sr_;

            /* Big clicks hit both sides; small crackle is per channel. */
            const float big_rate = 0.08f * a + 1.6f * m;
            if (rng_.Uni() < big_rate * inv_sr)
                big_click_ = (0.15f + 0.3f * rng_.Uni()) * (rng_.Next() < 0.0f ? -1.0f : 1.0f);
            const float pop_rate = 3.0f + 14.0f * a + 140.0f * m;
            const float pop_amp  = (0.01f + 0.05f * rng_.Uni()) * (0.4f + m);

            float y[2];
            for (int c = 0; c < 2; c++)
            {
                Chan& ch = ch_[c];
                float v = ch.wow.TapF(d);
                v = v - ch.v_low.Tick(v);                                 // low cut
                v = ch.v_hf2.Tick(ch.v_hf1.Tick(v));                      // HF roll-off
                v = Ss(1.3f * v) * (0.3f / Ss(0.39f));                    // mild warmth
                if (rng_.Uni() < pop_rate * inv_sr)
                    ch.click += pop_amp * (rng_.Next() < 0.0f ? -1.0f : 1.0f);
                ch.click += big_click_;
                const float cl = ch.v_click_lp.Tick(ch.click);
                ch.click *= 0.55f;                                        // ~0.1 ms pops
                const float hiss = ch.v_hiss.Tick(rng_.Next()) * (0.0012f * a + 0.014f * m);
                y[c] = v + cl + hiss;
            }
            big_click_ = 0.0f;
            /* Mono-ing with age (old records, worn styli). */
            const float mid = 0.5f * (y[0] + y[1]), side = 0.5f * (y[0] - y[1]) * (1.0f - 0.75f * a);
            wet[0] += mv * (mid + side);
            wet[1] += mv * (mid - side);
        }

        const float mm = Clamp(mt + mv, 0.0f, 1.0f);
        for (int c = 0; c < 2; c++) out[c] = x[c] * (1.0f - mm) + wet[c];
        l[i] = out[0];
        r[i] = out[1];
    }
}

} // namespace athanor
