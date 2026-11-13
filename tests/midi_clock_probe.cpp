// MidiClock: tempo lock, jitter rejection, tempo change, Start alignment,
// tempo hold after stop, and the CC -> knob mapping.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "midi_clock.h"
using namespace athanor;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main()
{
    MidiClock c; c.Reset();
    double t = 5.0e6;   // us
    srand(7);
    auto run = [&](double bpm, int ticks, double jitter_us) {
        const double step = 60.0e6 / bpm / 24.0;
        int beats = 0;
        for (int i = 0; i < ticks; i++)
        {
            t += step;
            const double j = jitter_us * ((rand() % 2001) / 1000.0 - 1.0);
            if (c.Tick((uint32_t)(t + j))) beats++;
            c.Poll((uint32_t)t);
        }
        return beats;
    };

    CHECK(c.PeriodSec() == 0.0f, "period before any tick");
    run(120, 7, 0);
    CHECK(std::fabs(c.PeriodSec() - 0.5f) < 0.002f, "quick lock after 7 ticks: %.4f", c.PeriodSec());
    printf("lock after 7 ticks: %.4f s (120 BPM = 0.5)\n", c.PeriodSec());

    /* 1 ms of USB frame jitter, 8 beats. */
    float lo = 9, hi = 0; int beats = 0;
    for (int b = 0; b < 8; b++) { beats += run(120, 24, 1000); lo = std::fmin(lo, c.PeriodSec()); hi = std::fmax(hi, c.PeriodSec()); }
    CHECK(beats == 8, "beats %d", beats);
    CHECK(hi - lo < 0.004f, "jitter leaks into the tempo: %.4f..%.4f", lo, hi);
    printf("with +-1 ms jitter: %.4f..%.4f s\n", lo, hi);

    run(140, 48, 300);
    CHECK(std::fabs(c.PeriodSec() - 60.0f / 140.0f) < 0.003f, "tempo change to 140: %.4f", c.PeriodSec());
    printf("after change to 140 BPM: %.4f s (%.4f)\n", c.PeriodSec(), 60.0 / 140.0);

    /* Start mid-beat: the next tick must be a beat. */
    run(140, 5, 0);
    c.Start();
    const uint32_t e0 = c.Edges();
    t += 60.0e6 / 140 / 24; CHECK(c.Tick((uint32_t)t), "first tick after Start is not a beat");
    CHECK(c.Edges() == e0 + 1, "edge count after Start");
    CHECK(run(140, 47, 0) == 1, "beat spacing after Start");

    /* Transport stops: tempo is held, clock reports not running. */
    const float held = c.PeriodSec();
    t += 3.0e6; c.Poll((uint32_t)t);
    CHECK(!c.Running(), "still running 3 s after the last tick");
    CHECK(c.PeriodSec() == held, "tempo not held after stop");
    /* ...and resumes cleanly at a new tempo without a bogus long interval. */
    run(90, 30, 0);
    CHECK(std::fabs(c.PeriodSec() - 60.0f / 90.0f) < 0.003f, "resume at 90: %.4f", c.PeriodSec());

    /* Timestamp wrap (2^32 us ~ 71 min). */
    MidiClock w; w.Reset(); t = 4294967296.0 - 200000.0;
    for (int i = 0; i < 60; i++) { t += 60.0e6 / 120 / 24; w.Tick((uint32_t)(uint64_t)t); }
    CHECK(std::fabs(w.PeriodSec() - 0.5f) < 0.002f, "across the timestamp wrap: %.4f", w.PeriodSec());

    /* Out-of-range tempo is ignored. */
    MidiClock s; s.Reset(); t = 1e6;
    for (int i = 0; i < 30; i++) { t += 60.0e6 / 900 / 24; s.Tick((uint32_t)t); }
    CHECK(s.PeriodSec() == 0.0f, "accepted 900 BPM");

    CHECK(CcToKnob(0) == 0.0f && CcToKnob(64) == 0.5f && CcToKnob(127) == 1.0f, "CC mapping ends / centre");
    for (int v = 1; v < 128; v++) CHECK(CcToKnob(v) > CcToKnob(v - 1), "CC mapping not monotonic at %d", v);
    /* Deadband 0.278..0.722 in CC terms. */
    int dlo = 0, dhi = 0; for (int v = 0; v < 128; v++) { const float k = CcToKnob(v); if (k < 0.2778f) dlo = v + 1; if (k <= 0.7222f) dhi = v; }
    printf("amount knob by CC: bypassed for CC %d..%d\n", dlo, dhi);

    printf(fails ? "%d FAIL\n" : "midi clock OK\n", fails);
    return fails ? 1 : 0;
}
