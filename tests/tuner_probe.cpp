// Tuner: six strings at once, six octaves of C, and chromatic.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <initializer_list>
#include "tuner.h"
using namespace athanor;
static const float SR = 48000;
static float mem[Tuner::kMemFloats];
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

template <typename F> static void Run(Tuner& t, double seconds, F sig, long& n)
{
    const int B = 64; float l[B], r[B];
    for (long b = 0; b < (long)(SR * seconds) / B; b++)
    {
        for (int i = 0; i < B; i++) { l[i] = r[i] = sig(n / (double)SR); n++; }
        t.Push(l, r, B);
        t.Poll(); t.Poll();          // the control loop runs ~1.3x per block; 2 is close enough
    }
}
static double Saw(double f, double t, int harmonics = 12)
{
    double x = 0; for (int h = 1; h <= harmonics && f * h < 20000; h++) x += sin(2 * M_PI * f * h * t) / h; return x * 0.5;
}
static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

int main()
{
    /* ── C octaves: a sawtooth oscillator near each C ─────────────────── */
    printf("C octaves (sawtooth oscillator, level 0.25)\n");
    const double cs[6] = {32.703, 65.406, 130.813, 261.626, 523.251, 1046.502};
    double worst_c = 0;
    for (int o = 0; o < 6; o++) for (double det : {-30.0, -4.0, 0.0, 7.0, 42.0})
    {
        Tuner t; t.Init(SR, mem); t.SetMode(Tuner::kCOctaves); long n = 0;
        const double f = cs[o] * pow(2.0, det / 1200.0);
        Run(t, 2.0, [&](double s) { return (float)(0.5 * Saw(f, s)); }, n);
        const int fb = t.Fundamental();
        const double err = fb >= 0 ? t.Band(fb).cents - det : 999;
        CHECK(fb == o, "C%d %+.0f cents: fundamental reported on ring %d", o + 1, det, fb);
        if (fb == o && fabs(err) > fabs(worst_c)) worst_c = err;
    }
    printf("  right ring every time; worst reading error %+.2f cents\n", worst_c);
    CHECK(fabs(worst_c) < 1.0, "C octaves accuracy %.2f cents", worst_c);
    {   /* square wave (odd harmonics only), quiet, and silence */
        Tuner t; t.Init(SR, mem); t.SetMode(Tuner::kCOctaves); long n = 0;
        const double f = 130.813 * pow(2.0, 11.0 / 1200.0);
        Run(t, 2.0, [&](double s) { return (float)(0.004 * (fmod(f * s, 1.0) < 0.5 ? 1.0 : -1.0)); }, n);
        printf("  quiet square (-48 dB) at C3 +11: ring %d, %+.2f cents\n", t.Fundamental(), t.Fundamental() >= 0 ? t.Band(t.Fundamental()).cents : 0.0f);
        CHECK(t.Fundamental() == 2 && fabs(t.Band(2).cents - 11) < 1.0, "quiet square");
        Run(t, 2.0, [&](double) { return 0.0f; }, n);
        CHECK(t.Fundamental() == -1, "silence still shows ring %d", t.Fundamental());
    }

    /* ── Chromatic: any note ──────────────────────────────────────────── */
    printf("chromatic\n");
    double worst_ch = 0; int wrong = 0, total = 0;
    for (int midi : {24, 28, 33, 40, 45, 52, 57, 60, 64, 69, 76, 81, 88, 93, 96})
        for (double det : {-45.0, -12.0, 0.0, 3.0, 38.0})
            for (int wave = 0; wave < 2; wave++)
            {
                Tuner t; t.Init(SR, mem); t.SetMode(Tuner::kChromatic); long n = 0;
                const double f = 440.0 * pow(2.0, (midi - 69 + det / 100.0) / 12.0);
                Run(t, 1.5, [&](double s) { return (float)(wave ? 0.5 * Saw(f, s) : 0.3 * sin(2 * M_PI * f * s)); }, n);
                total++;
                const bool ok = t.HasNote() && t.NoteClass() == midi % 12 && t.Octave() == midi / 12 - 1;
                if (!ok) { wrong++; printf("  WRONG: %s%d %+.0f (%s) read as %s%d\n", kNames[midi % 12], midi / 12 - 1, det, wave ? "saw" : "sine",
                                           t.HasNote() ? kNames[t.NoteClass()] : "-", t.Octave()); continue; }
                const double err = t.NoteCents() - det;
                if (fabs(err) > fabs(worst_ch)) worst_ch = err;
            }
    printf("  %d of %d notes named correctly (C1 to C7, sine and saw); worst reading error %+.2f cents\n", total - wrong, total, worst_ch);
    CHECK(wrong == 0, "%d wrong notes", wrong);
    CHECK(fabs(worst_ch) < 1.5, "chromatic accuracy %.2f cents", worst_ch);
    {   /* a note change is followed; silence clears it */
        Tuner t; t.Init(SR, mem); t.SetMode(Tuner::kChromatic); long n = 0;
        Run(t, 1.0, [&](double s) { return (float)(0.4 * Saw(220.0, s)); }, n);
        const long n0 = n; long took = -1;
        const int B = 64; float l[B], r[B];
        for (long b = 0; b < (long)SR / B; b++)
        {
            for (int i = 0; i < B; i++) { l[i] = r[i] = (float)(0.4 * Saw(329.63, n / (double)SR)); n++; }
            t.Push(l, r, B); t.Poll(); t.Poll();
            if (took < 0 && t.HasNote() && t.NoteClass() == 4) took = n - n0;
        }
        printf("  A3 -> E4 followed after %.0f ms\n", took / SR * 1000);
        CHECK(took > 0 && took < SR * 0.4, "note change took %ld samples", took);
        Run(t, 1.0, [&](double) { return 0.0f; }, n);
        CHECK(!t.HasNote(), "note still shown in silence");
    }

    /* ── Guitar: one string, then a strum ─────────────────────────────── */
    printf("guitar\n");
    const double gs[6] = {82.407, 110.0, 146.832, 195.998, 246.942, 329.628};
    const char* gn[6] = {"E2", "A2", "D3", "G3", "B3", "E4"};
    auto string = [&](int k, double cents, double t) {
        /* Plucked string: harmonics falling off, the high ones dying faster,
         * slightly stretched (inharmonic) like a real string. */
        const double f = gs[k] * pow(2.0, cents / 1200.0); double x = 0;
        for (int h = 1; h <= 10; h++) x += sin(2 * M_PI * f * h * sqrt(1 + 0.00008 * h * h) * t + k + h) / pow(h, 1.3) * exp(-t * (0.6 + 0.35 * h));
        return 0.12 * x;
    };
    double worst_one = 0;
    for (int k = 0; k < 6; k++) for (double det : {-25.0, 0.0, 9.0})
    {
        Tuner t; t.Init(SR, mem); t.SetMode(Tuner::kGuitar); long n = 0;
        Run(t, 1.5, [&](double s) { return (float)string(k, det, s); }, n);
        CHECK(t.Band(k).active, "%s alone: its ring is not lit", gn[k]);
        const double err = t.Band(k).cents - det;
        if (fabs(err) > fabs(worst_one)) worst_one = err;
    }
    printf("  one string at a time: worst reading error %+.2f cents\n", worst_one);
    CHECK(fabs(worst_one) < 1.5, "single string accuracy %.2f", worst_one);

    double worst_strum = 0;
    for (int trial = 0; trial < 4; trial++)
    {
        const double sets[4][6] = {{0, 0, 0, 0, 0, 0}, {10, -15, 0, 20, -8, 5}, {-20, 12, 18, -6, 25, -14}, {30, 0, -30, 0, 15, -25}};
        Tuner t; t.Init(SR, mem); t.SetMode(Tuner::kGuitar); long n = 0;
        /* Strummed low to high, 25 ms between strings; read 1.2 s after. */
        Run(t, 1.4, [&](double s) { double x = 0; for (int k = 0; k < 6; k++) { const double ts = s - 0.1 - 0.025 * k; if (ts > 0) x += string(k, sets[trial][k], ts); } return (float)x; }, n);
        printf("  strum, strings off by");
        for (int k = 0; k < 6; k++) printf(" %+3.0f", sets[trial][k]);
        printf("  -> read");
        for (int k = 0; k < 6; k++)
        {
            const Tuner::Reading rd = t.Band(k);
            printf(rd.active ? " %+5.1f" : "   -- ", rd.cents);
            CHECK(rd.active, "strum: %s not lit", gn[k]);
            const double err = rd.cents - sets[trial][k];
            if (rd.active && fabs(err) > fabs(worst_strum)) worst_strum = err;
        }
        printf("\n");
    }
    printf("  strummed: worst reading error %+.1f cents\n", worst_strum);
    CHECK(fabs(worst_strum) < 8.0, "strum accuracy %.1f", worst_strum);

    printf(fails ? "%d FAIL\n" : "tuner OK\n", fails);
    return fails ? 1 : 0;
}
