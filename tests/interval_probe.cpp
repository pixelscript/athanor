// Interval variant: does Capicola's pitch engine land on each musical step
// (out to two octaves) cleanly with the Interval reel?
#include <cstdio>
#include <cmath>
#include <vector>
#include <initializer_list>
#include "stretch_engine.h"
using namespace athanor;
static const float SR = 48000;
int main()
{
    const float ratios[] = {1.0f, 2.0f/3, 0.5f, 1.0f/3, 0.25f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 1.0f};
    const int   N = sizeof(ratios) / sizeof(ratios[0]);
    int fails = 0;
    for (int pass = 0; pass < 1; pass++)
    {
        StretchEngine e; e.Init(SR);
        const int B = 64; float il[B], ir[B], ol[B], orr[B]; long n = 0;
        const float hz = 7.0f, z = 0.75f;   // kStepHz, kStepDamping
        const float w = 2 * M_PI * hz;
        float p = 1, v = 0, pmax = 0;
        std::vector<float> o;
        const double seg = 2.5;   // seconds per step
        for (int s = 0; s < N; s++)
            for (long b = 0; b < SR * seg / B; b++)
            {
                const float dt = B / SR;
                v += (w * w * (ratios[s] - p) - 2 * z * w * v) * dt; p += v * dt;
                if (p > 4.6f) { p = 4.6f; if (v > 0) v = 0; }
                pmax = std::fmax(pmax, p);
                e.SetPitch(p);
                for (int i = 0; i < B; i++) { double t = n / SR; il[i] = ir[i] = 0.4f * sinf(2 * M_PI * 220 * t); n++; }
                e.Process(il, ir, ol, orr, B);
                for (int i = 0; i < B; i++) { if (!std::isfinite(ol[i])) { printf("NAN\n"); return 1; } o.push_back(ol[i]); }
            }
        printf("interval reel (%.1f Hz spring, peak speed %.2f)\n", hz, pmax);
        for (int s = 0; s < N; s++)
        {
            const long s0 = (long)((s + 1) * seg * SR) - 19200;   // last 0.4 s of the step
            double best = 0, bf = 0;
            for (double f = 40; f < 1000; f += 0.5)
            {
                double re = 0, im = 0;
                for (int k = 0; k < 19200; k += 2) { re += o[s0 + k] * cos(2 * M_PI * f * k / SR); im += o[s0 + k] * sin(2 * M_PI * f * k / SR); }
                const double m = re * re + im * im; if (m > best) { best = m; bf = f; }
            }
            double rms = 0; for (int k = 0; k < 19200; k++) rms += o[s0 + k] * o[s0 + k];
            const double exp = 220.0 * ratios[s];
            const double cents = 1200.0 * std::log2(bf / exp);
            const bool ok = std::fabs(cents) < 15.0 && sqrt(rms / 19200) > 0.1;
            fails += !ok;
            printf("  step x%.3f  dominant %6.1f Hz (expect %6.1f, %+5.1f cents)  rms %.3f %s\n",
                   ratios[s], bf, exp, cents, sqrt(rms / 19200), ok ? "" : "  <-- FAIL");
        }
    }
    /* Chord: mix at noon = dry + shifted copy, both notes at equal level. */
    printf("chord (mix 50%%: dry 220 Hz + shifted)\n");
    for (float ratio : {2.0f / 3, 0.5f, 1.0f / 3, 0.25f, 1.5f, 2.0f, 3.0f, 4.0f})
    {
        StretchEngine e; e.Init(SR); e.SetPitch(ratio);
        const int B = 64; float il[B], ir[B], ol[B], orr[B]; long n = 0;
        std::vector<float> o;
        for (long b = 0; b < SR * 3 / B; b++)
        {
            for (int i = 0; i < B; i++) { il[i] = ir[i] = 0.4f * sinf(2 * M_PI * 220 * (n / SR)); n++; }
            e.Process(il, ir, ol, orr, B);
            for (int i = 0; i < B; i++) o.push_back(il[i] + 0.5f * (ol[i] - il[i]));
        }
        /* Energy in a +-4 Hz band (Hann window): the high up-shifts splice
         * often, which smears the note by a few Hz without losing it. */
        auto mag = [&](double fc) { double e = 0; const long s0 = (long)o.size() - 48000;
            for (double f = fc - 4; f <= fc + 4; f += 1) { double re = 0, im = 0;
                for (int k = 0; k < 48000; k++) { const double w = 0.5 - 0.5 * cos(2 * M_PI * k / 48000);
                    re += w * o[s0 + k] * cos(2 * M_PI * f * k / SR); im += w * o[s0 + k] * sin(2 * M_PI * f * k / SR); }
                e += re * re + im * im; }
            return sqrt(e) / 12000.0; };
        const double a = mag(220.0), b = mag(220.0 * ratio);
        const bool ok = a > 0.05 && b > 0.05 && std::fabs(20 * std::log10(a / b)) < 3.0;
        fails += !ok;
        printf("  x%.2f: dry note %.3f, shifted note %.3f (%+.1f dB) %s\n", ratio, a, b, 20 * std::log10(b / a), ok ? "" : "<-- FAIL");
    }
    printf(fails ? "%d FAIL\n" : "all steps OK\n", fails);
    return fails ? 1 : 0;
}
