// Drive loudness matching: every level / flavour / amount comes out about as
// loud as the input, still clearly distorted, with no blast when engaged.
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include "drive.h"
using namespace athanor;
static const float SR = 48000;
static int fails = 0;
struct KW { float hp = 0, sh = 0; float Tick(float x) { hp += 0.013f * (x - hp); const float y = x - hp; sh += 0.2f * (y - sh); return y + 0.6f * (y - sh); } };

static float Sig(int kind, long n, float lvl)
{
    const double t = n / SR;
    if (kind == 0) return lvl * 1.414f * sinf(2 * M_PI * 220 * t);
    const double u = fmod(t, 0.5);
    const float hit = expf(-u * 18) * sinf(2 * M_PI * (60 + 140 * expf(-u * 40)) * u);
    const float hat = (fmod(t, 0.25) < 0.03) ? ((float)(((unsigned)n * 1103515245u + 12345u) >> 16 & 0x7fff) / 16384.f - 1.f) * 0.5f : 0;
    const float ch = 0.5f * sinf(2 * M_PI * 110 * t) + 0.3f * sinf(2 * M_PI * 277.2 * t) + 0.3f * sinf(2 * M_PI * 329.6 * t) + 0.25f * sinf(2 * M_PI * 440 * t);
    return lvl * 1.1f * (0.6f * ch + 0.9f * hit + 0.5f * hat);
}

int main()
{
    const char* nm[3] = {"saturation", "distortion", "fuzz      "};
    double worst = 0, worst_blast = 0, least_dist = 1e9;
    printf("loudness vs bypass (dB), amount 0.1 / 0.3 / 0.6 / 1.0; then how distorted at full\n");
    for (int level = 0; level < 3; level++) for (int side = -1; side <= 1; side += 2)
    {
        printf("  %s %s:", nm[level], side < 0 ? "analog " : "digital");
        double dist_full = 0;
        for (float a : {0.1f, 0.3f, 0.6f, 1.0f})
        {
            double sum = 0; int cnt = 0;
            for (int kind = 0; kind < 2; kind++) for (float lvl : {0.07f, 0.2f})
            {
                Drive d; d.Init(SR); const int B = 64; float l[B], r[B], x[B]; long n = 0; KW ki, ko; double ei = 0, eo = 0, ed = 0, ex = 0;
                for (long b = 0; b < (long)(SR * 4) / B; b++)
                {
                    d.SetParams(level, side, a, 0.5f);
                    for (int i = 0; i < B; i++) { x[i] = l[i] = r[i] = Sig(kind, n + i, lvl); }
                    d.Process(l, r, B);
                    for (int i = 0; i < B; i++, n++)
                    {
                        const float wi = ki.Tick(x[i]), wo = ko.Tick(l[i]);
                        if (n > SR) { ei += wi * wi; eo += wo * wo; ed += (l[i] - x[i]) * (l[i] - x[i]); ex += x[i] * x[i]; }
                    }
                }
                const double db = 10 * log10(eo / ei);
                sum += db; cnt++; if (fabs(db) > fabs(worst)) worst = db;
                if (a == 1.0f) dist_full += sqrt(ed / ex) / 4;
            }
            printf(" %+5.1f", sum / cnt);
        }
        printf("    changed by %3.0f%%\n", dist_full * 100);
        if (dist_full < least_dist) least_dist = dist_full;

        /* Slam from bypass straight to full (CV / MIDI / bypass-all can do this):
         * loudest 10 ms afterwards, against the input's own loudest 10 ms. */
        {
            Drive d; d.Init(SR); const int B = 64; float l[B], r[B]; long n = 0; double in_pk = 0, out_pk = 0, wi = 0, wo = 0; int k = 0;
            for (long b = 0; b < (long)(SR * 3) / B; b++)
            {
                const bool on = n >= (long)(SR * 2);
                d.SetParams(level, on ? side : 0, on ? 1.0f : 0.0f, 0.5f);
                float x[B]; for (int i = 0; i < B; i++) x[i] = l[i] = r[i] = Sig(1, n + i, 0.07f);
                d.Process(l, r, B);
                for (int i = 0; i < B; i++, n++)
                {
                    wi += x[i] * x[i]; wo += l[i] * l[i];
                    if (++k == 480) { if (n > SR) in_pk = fmax(in_pk, wi); if (on) out_pk = fmax(out_pk, wo); wi = wo = 0; k = 0; }
                }
            }
            const double blast = 10 * log10(out_pk / in_pk);
            if (blast > worst_blast) worst_blast = blast;
        }
    }
    printf("worst long-term loudness error: %+.1f dB\n", worst);
    printf("worst jump when slammed on:     %+.1f dB over the input's own peaks\n", worst_blast);
    if (fabs(worst) > 1.5) { fails++; printf("  FAIL: loudness not matched\n"); }
    if (worst_blast > 6.0) { fails++; printf("  FAIL: blast on engage\n"); }
    if (least_dist < 0.05) { fails++; printf("  FAIL: a drive mode is not distorting\n"); }
    printf(fails ? "%d FAIL\n" : "drive loudness OK\n", fails);
    return fails ? 1 : 0;
}
