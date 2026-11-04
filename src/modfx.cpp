/**
 * @file modfx.cpp
 * @brief See modfx.h.
 */
#include "modfx.h"

namespace athanor {

namespace {
inline float Sin1(float ph) { return std::sin(2.0f * kPi * ph); }   // phase 0..1

inline float LoopClip(float x)
{
    x = Clamp(x, -1.5f, 1.5f);
    return x - (x * x * x) * (1.0f / 6.75f);
}

void InitCommon(Smooth& a, Smooth& b, Smooth& amt, Smooth& hz, float sr, float hz0)
{
    const float k = 1.0f - std::exp(-1.0f / (0.02f * sr));   // 20 ms
    a.Init(0.0f, k);
    b.Init(0.0f, k);
    amt.Init(0.0f, k);
    hz.Init(hz0, 0.2f);   // per block
}
} // namespace

/* ═════════════════════════════════════════════════════════════════════════
 *  Chorus / tremolo
 * ═════════════════════════════════════════════════════════════════════════ */

void ChorusTrem::Init(float sr, float* mem)
{
    sr_ = sr;
    buf_[0] = mem;
    buf_[1] = mem + kFloats;
    dl_[0].Init(buf_[0], kFloats);
    dl_[1].Init(buf_[1], kFloats);
    InitCommon(mix_c_, mix_t_, amt_, hz_sm_, sr, 1.0f);
    lfo_ = SyncLfo{};
}

void ChorusTrem::SetParams(int side, float amount, LfoRate rate, uint32_t edges)
{
    amount = Clamp(amount, 0.0f, 1.0f);
    const float engage = Clamp(amount / 0.06f, 0.0f, 1.0f);
    t_mix_c_ = (side < 0) ? engage : 0.0f;
    t_mix_t_ = (side > 0) ? engage : 0.0f;
    if (side != 0) t_amt_ = amount;
    hz_ = hz_sm_.Tick(rate.hz);
    lfo_.OnClock(edges, rate.per_edge);
}

void ChorusTrem::Process(float* l, float* r, std::size_t n)
{
    const float inc = hz_ / sr_;
    const float ms  = 0.001f * sr_;
    for (std::size_t i = 0; i < n; i++)
    {
        const float mc = mix_c_.Tick(t_mix_c_);
        const float mt = mix_t_.Tick(t_mix_t_);
        const float a  = amt_.Tick(t_amt_);
        lfo_.Advance(inc);
        const float p = lfo_.ph;

        float xl = l[i], xr = r[i];
        dl_[0].Write(xl);
        dl_[1].Write(xr);

        /* Chorus: two voices per side in anti-phase, sides 90° apart. */
        if (mc > 1.0e-5f)
        {
            const float base  = 8.0f * ms;
            const float depth = (1.0f + 6.0f * a) * ms;
            const float wl = 0.5f * (dl_[0].TapF(base + depth * (0.5f + 0.5f * Sin1(p)))
                                   + dl_[0].TapF(base + depth * (0.5f + 0.5f * Sin1(p + 0.5f))));
            const float wr = 0.5f * (dl_[1].TapF(base + depth * (0.5f + 0.5f * Sin1(p + 0.25f)))
                                   + dl_[1].TapF(base + depth * (0.5f + 0.5f * Sin1(p + 0.75f))));
            const float mix = mc * (0.35f + 0.4f * a);
            xl += mix * (wl - xl);
            xr += mix * (wr - xr);
        }

        /* Tremolo: sine that squares up with depth, then spreads into a
         * stereo pan-trem over the top quarter of the throw. */
        if (mt > 1.0e-5f)
        {
            const float sharp = 1.0f + 6.0f * Clamp((a - 0.5f) * 2.0f, 0.0f, 1.0f);
            const float norm  = 1.0f / std::tanh(sharp);
            const float pan   = 0.5f * Clamp((a - 0.75f) * 4.0f, 0.0f, 1.0f);
            const float sl = std::tanh(sharp * Sin1(p)) * norm;
            const float sr = std::tanh(sharp * Sin1(p + pan)) * norm;
            const float depth = mt * a;
            xl *= 1.0f - depth * (0.5f - 0.5f * sl);
            xr *= 1.0f - depth * (0.5f - 0.5f * sr);
        }
        l[i] = xl;
        r[i] = xr;
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 *  Phaser / flanger
 * ═════════════════════════════════════════════════════════════════════════ */

void PhaserFlanger::Init(float sr, float* mem)
{
    sr_ = sr;
    buf_[0] = mem;
    buf_[1] = mem + kFloats;
    dl_[0].Init(buf_[0], kFloats);
    dl_[1].Init(buf_[1], kFloats);
    for (auto& d : fl_dc_) d.r = 1.0f - 2.0f * kPi * 20.0f / sr;
    InitCommon(mix_p_, mix_f_, amt_, hz_sm_, sr, 0.3f);
    lfo_ = SyncLfo{};
}

void PhaserFlanger::SetParams(int side, float amount, LfoRate rate, uint32_t edges)
{
    amount = Clamp(amount, 0.0f, 1.0f);
    const float engage = Clamp(amount / 0.06f, 0.0f, 1.0f);
    t_mix_p_ = (side < 0) ? engage : 0.0f;
    t_mix_f_ = (side > 0) ? engage : 0.0f;
    if (side != 0) t_amt_ = amount;
    hz_ = hz_sm_.Tick(rate.hz);
    lfo_.OnClock(edges, rate.per_edge);
}

void PhaserFlanger::Process(float* l, float* r, std::size_t n)
{
    const float inc = hz_ / sr_;
    const float ms  = 0.001f * sr_;
    for (std::size_t i = 0; i < n; i++)
    {
        const float mp = mix_p_.Tick(t_mix_p_);
        const float mf = mix_f_.Tick(t_mix_f_);
        const float a  = amt_.Tick(t_amt_);
        lfo_.Advance(inc);
        const float p = lfo_.ph;

        float io[2] = {l[i], r[i]};

        /* Flanger delay lines always see the input so a switch is seamless. */
        if (mf > 1.0e-5f)
        {
            const float width = (0.5f + 5.5f * a) * ms;
            const float fb    = 0.3f + 0.6f * a;
            for (int c = 0; c < 2; c++)
            {
                const float s = 0.5f - 0.5f * std::cos(2.0f * kPi * (p + 0.25f * (float)c));
                const float d = 0.3f * ms + width * s;
                const float y = dl_[c].TapF(d);
                dl_[c].Write(LoopClip(io[c] + fb * fl_dc_[c].Tick(y)));
                io[c] += 0.5f * mf * (y - io[c]);
            }
        }
        else
        {
            dl_[0].Write(io[0]);
            dl_[1].Write(io[1]);
        }

        /* Phaser: 6 first-order allpasses swept exponentially, with feedback. */
        if (mp > 1.0e-5f)
        {
            const float span = 0.4f + 0.6f * a;       // sweep width (octaves scale)
            const float fb   = 0.15f + 0.45f * a;
            for (int c = 0; c < 2; c++)
            {
                /* Coefficient refreshed every 8 samples (the sweep is slow). */
                if ((i & 7u) == 0u)
                {
                    const float s = 0.5f - 0.5f * std::cos(2.0f * kPi * (p + 0.25f * (float)c));
                    const float f = 180.0f * std::exp2(span * 4.5f * s);   // 180 Hz -> ~4 kHz
                    const float t = std::tan(kPi * Clamp(f, 20.0f, 0.45f * sr_) / sr_);
                    ap_k_[c] = (t - 1.0f) / (t + 1.0f);
                }
                const float k = ap_k_[c];
                float y = io[c] + fb * ph_fb_[c];
                for (int st = 0; st < kStages; st++)
                {
                    const float out = k * y + ap_z_[c][st];
                    ap_z_[c][st] = y - k * out;
                    y = out;
                }
                ph_fb_[c] = Clamp(y, -1.5f, 1.5f);
                io[c] += 0.5f * mp * (y - io[c]);
            }
        }
        l[i] = io[0];
        r[i] = io[1];
    }
}

} // namespace athanor
