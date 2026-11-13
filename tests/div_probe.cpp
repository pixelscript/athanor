// Delay subdivision change: crossfades to the new time without a click or a
// pitch swoop, and echoes then land at the new time.
#include <cmath>
#include <cstdio>
#include <vector>
#include "fx.h"
using namespace athanor;
static const float SR = 48000;
static float plate[Plate::kMemFloats], dl[Space::kDelayFloats], dr[Space::kDelayFloats];
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Strongest frequency in a short window (zero crossings of a clean sine). */
static double Freq(const std::vector<float>& o, long a, long n)
{
    int z = 0; for (long k = a + 1; k < a + n; k++) if ((o[k - 1] < 0) != (o[k] < 0)) z++;
    return z * SR / (2.0 * n);
}

int main()
{
    for (int xf = 1; xf >= 0; xf--)
    {
        Space s; s.Init(SR, plate, dl, dr);
        s.SetParams(+1, 1.0f, 0.0f);          // delay side, full send, no feedback
        s.SetDelaySeconds(0.5f);
        const int B = 64; float l[B], r[B]; long n = 0; std::vector<float> o;
        double worst = 0, flo = 1e9, fhi = 0;
        for (long b = 0; b < (long)(SR * 3) / B; b++)
        {
            if (b == (long)(SR * 1.5) / B) s.SetDelaySeconds(0.375f, xf != 0);   // 1/4 -> dotted 1/8
            /* The note stops just before the change, so only its echo is heard through it. */
            for (int i = 0; i < B; i++) { l[i] = r[i] = (n < (long)(SR * 1.45)) ? 0.3f * sinf(2 * M_PI * 440 * n / SR) : 0.0f; n++; }
            s.Process(l, r, B);
            for (int i = 0; i < B; i++) o.push_back(l[i]);
        }
        /* Around the switch: biggest sample-to-sample step, and pitch range. */
        const long sw = (long)(SR * 1.5);
        for (long k = sw; k < sw + 24000; k++) worst = std::fmax(worst, std::fabs(o[k] - o[k - 1]));
        for (long a = sw; a < sw + 12000; a += 960) { const double f = Freq(o, a, 960); flo = flo < f ? flo : f; fhi = fhi > f ? fhi : f; }
        printf("%s: biggest step %.3f, pitch %.0f..%.0f Hz around the change\n", xf ? "crossfade" : "tape glide", worst, flo, fhi);
        if (xf)
        {
            CHECK(worst < 0.08, "click on crossfade (step %.3f)", worst);
            CHECK(flo > 410 && fhi < 470, "pitch moved during a crossfade: %.0f..%.0f", flo, fhi);
        }
        else
            CHECK(fhi > 470, "tape glide lost its pitch swoop (%.0f)", fhi);
    }
    /* After a crossfade, an impulse echoes at the new time. */
    {
        Space s; s.Init(SR, plate, dl, dr);
        s.SetParams(+1, 1.0f, 0.0f); s.SetDelaySeconds(0.5f);
        const int B = 64; float l[B], r[B]; long n = 0; long peak_at = 0; float peak = 0;
        const long hit = (long)(SR * 1.0);
        for (long b = 0; b < (long)(SR * 2) / B; b++)
        {
            if (b == (long)(SR * 0.5) / B) s.SetDelaySeconds(0.25f, true);
            for (int i = 0; i < B; i++) { l[i] = r[i] = (n == hit) ? 1.0f : 0.0f; n++; }
            s.Process(l, r, B);
            for (int i = 0; i < B; i++) { const long t = n - B + i; if (t > hit + 2400 && fabsf(l[i]) > peak) { peak = fabsf(l[i]); peak_at = t; } }
        }
        const double ms = (peak_at - hit) / SR * 1000;
        printf("echo after switching 0.5 s -> 0.25 s: %.1f ms\n", ms);
        CHECK(fabs(ms - 250) < 3, "echo at %.1f ms, expected 250", ms);
    }
    printf(fails ? "%d FAIL\n" : "subdivision change OK\n", fails);
    return fails ? 1 : 0;
}
