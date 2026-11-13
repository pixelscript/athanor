/**
 * @file drive.cpp
 * @brief See drive.h.
 */
#include "drive.h"

namespace athanor {

namespace {
constexpr float kLn2 = 0.69314718f;

/* L1 analog: asymmetric soft saturation s(u+b) - s(b), s(u) = u/sqrt(1+u^2). */
constexpr float kSatBias = 0.15f;
inline float Ss(float u)  { return u / std::sqrt(1.0f + u * u); }
inline float SsF(float u) { return std::sqrt(1.0f + u * u); }
const float kSsB = 0.14834f;   // Ss(kSatBias)
inline float SatF(float u)  { return Ss(u + kSatBias) - kSsB; }
inline float SatFA(float u) { return SsF(u + kSatBias) - kSsB * u; }

/* L2 analog: tanh, antiderivative log(cosh(u)) computed overflow-safe. */
inline float TanhF(float u)  { return std::tanh(u); }
inline float TanhFA(float u)
{
    const float a = std::fabs(u);
    return a + std::log1p(std::exp(-2.0f * a)) - kLn2;
}

/* Hard clip to [lo, hi] and its antiderivative (continuous at the knees). */
inline float Clip(float u, float lo, float hi) { return u < lo ? lo : (u > hi ? hi : u); }
inline float ClipA(float u, float lo, float hi)
{
    if (u < lo) return lo * u - 0.5f * lo * lo;
    if (u > hi) return hi * u - 0.5f * hi * hi;
    return 0.5f * u * u;
}

/* Sine folder. */
constexpr float kFoldK = 0.5f * kPi;
inline float FoldF(float u)  { return std::sin(kFoldK * u); }
inline float FoldFA(float u) { return -std::cos(kFoldK * u) * (1.0f / kFoldK); }

/* Rough ear weighting for the loudness match: lose the lows below ~100 Hz,
 * lift the highs above ~1.7 kHz by about 4 dB (harsh harmonics sound louder
 * than their RMS suggests). */
inline float Weigh(float x, float& hp, float& sh)
{
    hp += 0.013f * (x - hp);
    const float y = x - hp;
    sh += 0.2f * (y - sh);
    return y + 0.6f * (y - sh);
}

constexpr float kMakeupMin = 0.03f;   // -30 dB
constexpr float kMakeupMax = 1.4f;    // +3 dB
constexpr float kEnergyEps = 1.0e-7f;

inline float Crush(float x, float bits)
{
    const float q = std::exp2(bits - 1.0f);
    return std::round(x * q) / q;
}
} // namespace

void Drive::Init(float sr)
{
    sr_ = sr;
    for (auto& c : ch_)
    {
        c = Chan{};
        c.pre_hp.SetHz(700.0f, sr);
        c.dist_lp.SetHz(6000.0f, sr);
        c.fuzz_lp.SetHz(3800.0f, sr);
        c.fuzz_env.SetHz(25.0f, sr);
        c.tilt.SetHz(800.0f, sr);
    }
    const float k = 1.0f - std::exp(-1.0f / (0.005f * sr));    // 5 ms
    const float kl = 1.0f - std::exp(-1.0f / (0.03f * sr));    // 30 ms level xfade
    for (int i = 0; i < kLevels; i++) lvl_w_[i].Init(i == 0 ? 1.0f : 0.0f, kl);
    mix_a_.Init(0.0f, k);
    mix_d_.Init(0.0f, k);
    amt_.Init(0.0f, k);
    gl_.Init(1.0f, k);
    gh_.Init(1.0f, k);

    e_k_      = 1.0f - std::exp(-1.0f / (0.08f * sr));    // 80 ms loudness window
    makeup_k_ = 1.0f - std::exp(-1.0f / (0.02f * sr));    // 20 ms gain smoothing
    e_in_ = e_out_ = 0.0f;
    makeup_ = 1.0f;
    was_on_ = false;
}

void Drive::SetParams(int level, int side, float amount, float tone)
{
    if (level < 0 || level >= kLevels) level = 0;
    for (int i = 0; i < kLevels; i++) t_lvl_w_[i] = (i == level) ? 1.0f : 0.0f;

    amount = Clamp(amount, 0.0f, 1.0f);
    const float engage = Clamp(amount / 0.06f, 0.0f, 1.0f);
    t_mix_a_ = (side < 0) ? engage : 0.0f;
    t_mix_d_ = (side > 0) ? engage : 0.0f;
    if (side != 0) t_amt_ = amount;   // hold the last amount while fading out

    const float t = Clamp(tone, 0.0f, 1.0f);
    if (t < 0.5f)
    {
        const float k = (0.5f - t) * 2.0f;
        t_gl_ = 1.0f + 0.5f * k;
        t_gh_ = 1.0f - 0.85f * k;
    }
    else
    {
        const float k = (t - 0.5f) * 2.0f;
        t_gl_ = 1.0f - 0.5f * k;
        t_gh_ = 1.0f + 1.0f * k;
    }
}

float Drive::Analog(int level, Chan& c, float x, float a)
{
    switch (level)
    {
        case 0:   /* subtle tube-ish saturation, up to ~+16 dB of drive */
        {
            const float g = 1.0f + 5.0f * a;
            const float comp = Ss(0.25f) / Ss(0.25f * g);
            return comp * c.sat.Run(g * x, SatF, SatFA);
        }
        case 1:   /* mid-pushed diode-style distortion (tanh), up to ~+38 dB */
        {
            const float hp = x - c.pre_hp.Tick(x);
            const float pre = x + 1.5f * hp;
            const float g = 2.0f + 78.0f * a * a;
            const float y = c.dist.Run(g * pre, TanhF, TanhFA);
            return 0.26f * c.dist_lp.Tick(y);
        }
        default:  /* gated, sputtery asymmetric fuzz */
        {
            const float env = c.fuzz_env.Tick(std::fabs(x));
            const float g = 20.0f + 380.0f * a * a;
            /* Starved bias: the clip window shifts with the envelope, so
             * decays sputter and break up like a dying-battery fuzz. */
            const float bias = 0.25f - 0.6f * Clamp(env * 4.0f, 0.0f, 0.4f);
            const float y = c.fuzz.Run(g * x + bias,
                [](float u) { return Clip(u, -1.0f, 0.55f); },
                [](float u) { return ClipA(u, -1.0f, 0.55f); });
            const float gate = Clamp((env - 0.002f) * 400.0f, 0.0f, 1.0f);
            return 0.28f * gate * c.fuzz_dc.Tick(c.fuzz_lp.Tick(y));
        }
    }
}

float Drive::Digital(int level, Chan& c, float x, float a, bool take)
{
    switch (level)
    {
        case 0:   /* clean digital hard clipping */
        {
            const float g = 1.0f + 6.0f * a;
            const float y = c.clip.Run(g * x,
                [](float u) { return Clip(u, -1.0f, 1.0f); },
                [](float u) { return ClipA(u, -1.0f, 1.0f); });
            return y / (1.0f + 0.35f * (g - 1.0f));
        }
        case 1:   /* bit-crush + decimation into a hard clip */
        {
            if (take) c.hold[1] = x;
            const float bits = 12.0f - 8.0f * a;            // 12 → 4 bits
            const float g = 2.0f + 10.0f * a;
            const float y = Clip(Crush(c.hold[1] * g, bits), -1.0f, 1.0f);
            return 0.5f * y / (1.0f + 0.6f * a);
        }
        default:  /* wavefold + heavy crush */
        {
            const float g = 2.0f + 7.0f * a;
            const float f = c.fold.Run(g * x, FoldF, FoldFA);
            if (take) c.hold[2] = f;
            const float bits = 6.0f - 3.0f * a;             // 6 → 3 bits
            return 0.3f * Crush(c.hold[2], bits);
        }
    }
}

void Drive::Process(float* l, float* r, std::size_t n)
{
    for (std::size_t i = 0; i < n; i++)
    {
        float w[kLevels];
        for (int k = 0; k < kLevels; k++) w[k] = lvl_w_[k].Tick(t_lvl_w_[k]);
        const float ma = mix_a_.Tick(t_mix_a_);
        const float md = mix_d_.Tick(t_mix_d_);
        const float a  = amt_.Tick(t_amt_);
        const float gl = gl_.Tick(t_gl_);
        const float gh = gh_.Tick(t_gh_);

        /* Decimator clocks for the two crushing levels (shared L/R). */
        bool take[kLevels] = {true, false, false};
        {
            const float hz1 = 24000.0f * std::exp2(-2.6f * a);   // 24 k → ~4 k
            const float hz2 = 8000.0f * std::exp2(-2.4f * a);    //  8 k → ~1.5 k
            sh_ph_[1] += hz1 / sr_;
            sh_ph_[2] += hz2 / sr_;
            for (int k = 1; k < kLevels; k++)
                if (sh_ph_[k] >= 1.0f) { sh_ph_[k] -= std::floor(sh_ph_[k]); take[k] = true; }
        }

        float* io[2] = {&l[i], &r[i]};
        float  wet[2] = {0.0f, 0.0f};
        float  mm[2]  = {0.0f, 0.0f};
        float  ein = 0.0f, eout = 0.0f;
        for (int c = 0; c < 2; c++)
        {
            Chan&       ch = ch_[c];
            const float x  = *io[c];
            const float xi = Weigh(x, ch.wi_hp, ch.wi_sh);   // input loudness: always tracked
            ein += xi * xi;
            float shaped = 0.0f, m = 0.0f;
            for (int k = 0; k < kLevels; k++)
            {
                const float wa = w[k] * ma, wd = w[k] * md;
                if (wa > 1.0e-4f) { shaped += wa * Analog(k, ch, x, a); m += wa; }
                else if (k == 0) ch.sat.Idle();
                else if (k == 1) ch.dist.Idle();
                else             ch.fuzz.Idle();
                if (wd > 1.0e-4f) { shaped += wd * Digital(k, ch, x, a, take[k]); m += wd; }
                else if (k == 0) ch.clip.Idle();
                else if (k == 2) ch.fold.Idle();
            }
            mm[c] = m;
            if (m <= 1.0e-4f) { ch.tilt.Tick(0.0f); ch.dc.Tick(0.0f); continue; }
            const float lo = ch.tilt.Tick(shaped);
            /* `shaped` carries the mix weight m; measure the full-strength
             * driven signal so the match doesn't chase the fade-in. */
            wet[c] = ch.dc.Tick(lo * gl + (shaped - lo) * gh);
            const float full = wet[c] / (m > 1.0f ? 1.0f : m);
            const float yo   = Weigh(full, ch.wo_hp, ch.wo_sh);
            eout += yo * yo;
        }

        /* Loudness match: one gain for both channels. */
        e_in_ += e_k_ * (ein - e_in_);
        const bool on = (mm[0] > 1.0e-4f) || (mm[1] > 1.0e-4f);
        if (on)
        {
            if (!was_on_)
            {
                /* Engaging: assume the drive is louder than the input until
                 * it has been measured, so it fades up rather than blasts. */
                e_out_  = e_in_ * (t_lvl_w_[0] > 0.5f ? 2.5f : 16.0f);
                makeup_ = std::sqrt((e_in_ + kEnergyEps) / (e_out_ + kEnergyEps));
            }
            e_out_ += e_k_ * (eout - e_out_);
            const float want = Clamp(std::sqrt((e_in_ + kEnergyEps) / (e_out_ + kEnergyEps)),
                                     kMakeupMin, kMakeupMax);
            makeup_ += makeup_k_ * (want - makeup_);
        }
        was_on_ = on;

        for (int c = 0; c < 2; c++)
        {
            if (mm[c] <= 1.0e-4f) continue;
            const float m = mm[c] > 1.0f ? 1.0f : mm[c];
            *io[c] = *io[c] * (1.0f - m) + makeup_ * wet[c];
        }
    }
}

} // namespace athanor
