// Interval's pitch shifter: is each step in tune and clean, and does the
// shifted copy stay locked in time to the input?
#include <cmath>
#include <cstdio>
#include <vector>
#include <initializer_list>
#include "shifter.h"
using namespace athanor;
static const float SR = 48000;
static float mem[Shifter::kMemFloats];
static int fails = 0;

static double Band(const std::vector<float>& o, long s0, long N, double fc, double half)
{
    double e = 0;
    for (double f = fc - half; f <= fc + half; f += 1) { double re = 0, im = 0;
        for (long k = 0; k < N; k += 2) { const double w = 0.5 - 0.5 * cos(2 * M_PI * k / N);
            re += w * o[s0 + k] * cos(2 * M_PI * f * k / SR); im += w * o[s0 + k] * sin(2 * M_PI * f * k / SR); }
        e += re * re + im * im; }
    return e;
}

int main()
{
    printf("step     note delay (ms)   held chord: on the notes / level    single note tuning   max delay\n");
    for (float p : {0.75f, 0.5f, 0.375f, 0.25f, 1.5f, 2.0f, 3.0f, 4.0f})
    {
        const int B = 64; float il[B], ir[B], ol[B], orr[B];
        /* 1. Irregular plucked notes: energy-centroid delay of each note. */
        double dmin = 1e9, dmax = -1e9, tmax = 0;
        {
            Shifter s; s.Init(SR, mem); s.SetRatio(p);
            std::vector<double> on; double t0 = 1.0; unsigned sd = 12345;
            while (t0 < 19) { on.push_back(t0); sd = sd * 1664525u + 1013904223u; t0 += 0.45 + 0.45 * ((sd >> 8) & 0xffff) / 65535.0; }
            std::vector<float> in, o; size_t ni = 0; double ph = 0; long n = 0;
            for (long b = 0; b < (long)(SR * 20) / B; b++)
            {
                for (int i = 0; i < B; i++)
                {
                    const double t = n / SR; while (ni + 1 < on.size() && t >= on[ni + 1]) ni++;
                    const double u = t - on[ni]; float x = 0;
                    if (u >= 0 && u < 0.16) { const double f = 196 * pow(2.0, ((ni * 7) % 12) / 12.0); ph += 2 * M_PI * f / SR; x = 0.5f * sin(ph) * fmin(1.0, u / 0.003) * exp(-u * 25); }
                    il[i] = ir[i] = x; n++;
                }
                s.Process(il, ir, ol, orr, B);
                tmax = fmax(tmax, s.DelaySamples());
                for (int i = 0; i < B; i++) { in.push_back(il[i]); o.push_back(ol[i]); }
            }
            for (size_t k = 1; k + 1 < on.size(); k++)
            {
                const long a = on[k] * SR, z = on[k + 1] * SR; double ci = 0, ei = 0, co = 0, eo = 0;
                for (long j = a; j < z; j++) { const double x = in[j] * in[j], y = o[j] * o[j]; ci += x * (j - a); ei += x; co += y * (j - a); eo += y; }
                const double d = (co / eo - ci / ei) / SR * 1000; dmin = fmin(dmin, d); dmax = fmax(dmax, d);
            }
        }
        /* 2. A held three-note chord: how much lands on the shifted notes, level steadiness. */
        double pur, wob, cents;
        {
            Shifter s; s.Init(SR, mem); s.SetRatio(p);
            std::vector<float> o; long n = 0;
            for (long b = 0; b < (long)(SR * 3) / B; b++)
            {
                for (int i = 0; i < B; i++) { const double t = n / SR; il[i] = ir[i] = 0.25f * sinf(2 * M_PI * 220 * t) + 0.15f * sinf(2 * M_PI * 330 * t) + 0.1f * sinf(2 * M_PI * 523 * t); n++; }
                s.Process(il, ir, ol, orr, B);
                for (int i = 0; i < B; i++) o.push_back(ol[i]);
            }
            const long N = 48000, s0 = o.size() - N; double tot = 0;
            for (long k = 0; k < N; k++) { const double w = 0.5 - 0.5 * cos(2 * M_PI * k / N); tot += w * w * o[s0 + k] * o[s0 + k]; }
            tot *= N / 4.0 * 0.5;   // same scale as Band() (stride 2)
            const double inb = Band(o, s0, N, 220 * p, 3) + Band(o, s0, N, 330 * p, 3) + Band(o, s0, N, 523 * p, 3);
            pur = 100 * inb / tot;
            double mn = 9, mx = 0;
            for (long k = s0; k + 960 <= (long)o.size(); k += 960) { double r = 0; for (int j = 0; j < 960; j++) r += o[k + j] * o[k + j]; r = sqrt(r / 960); mn = fmin(mn, r); mx = fmax(mx, r); }
            wob = 20 * log10(mx / (mn + 1e-9));
        }
        /* 3. Tuning: one harmonic-rich note (A2, 8 harmonics). */
        {
            Shifter s; s.Init(SR, mem); s.SetRatio(p);
            std::vector<float> o; long n = 0;
            for (long b = 0; b < (long)(SR * 4) / B; b++)
            {
                for (int i = 0; i < B; i++) { const double t = n / SR; double x = 0; for (int h = 1; h <= 8; h++) x += 0.3 / h * sin(2 * M_PI * 110 * h * t + h); il[i] = ir[i] = (float)x; n++; }
                s.Process(il, ir, ol, orr, B);
                for (int i = 0; i < B; i++) o.push_back(ol[i]);
            }
            const long N = 3 * 48000, s0 = o.size() - N;
            const double f2 = 220 * p;   // 2nd harmonic: better frequency resolution than the fundamental
            double best = 0, bf = 0; for (double f = f2 * 0.98; f < f2 * 1.02; f += f2 * 0.0002) { const double m = Band(o, s0, N, f, 0); if (m > best) { best = m; bf = f; } }
            cents = 1200 * log2(bf / f2);
        }
        const bool ok = dmax < 50 && dmin > 0 && pur > 85 && std::fabs(cents) < 6 && tmax / SR * 1000 < 55;
        fails += !ok;
        printf("x%.3f   %4.0f .. %3.0f       %3.0f%%  +-%.1f dB                        %+5.1f cents         %3.0f ms  %s\n",
               p, dmin, dmax, pur, wob / 2, cents, tmax / SR * 1000, ok ? "" : "<-- FAIL");
    }
    printf(fails ? "%d FAIL\n" : "shifter OK\n", fails);
    return fails ? 1 : 0;
}
