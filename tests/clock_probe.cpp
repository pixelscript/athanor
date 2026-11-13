// J3 clock input: 1 / 24 ppqn auto-detect, steadiness under polling jitter,
// and how fast a real tempo change is followed.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "clock_in.h"
using namespace athanor;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* What the firmware sees: the true edge, noticed at the next 1 ms poll, with
 * the poll itself up to 2 ms late and now and then stalled 10 ms (SD write). */
static uint32_t Seen(double t_us)
{
    double p = std::ceil(t_us / 1000.0) * 1000.0 + (rand() % 2001);
    if (rand() % 40 == 0) p += 10000.0;
    return (uint32_t)p;
}

struct Rig
{
    PulseClock pc; TempoLock lock; double t = 3.0e6; float old_p = 0; uint32_t old_last = 0;
    float lo = 9, hi = 0, old_lo = 9, old_hi = 0;
    void Mark() { lo = 9; hi = 0; old_lo = 9; old_hi = 0; }
    /* Run `beats` beats at `bpm` with `ppqn` pulses per beat. */
    int Run(double bpm, int ppqn, int beats)
    {
        int nb = 0;
        for (int i = 0; i < beats * ppqn; i++)
        {
            t += 60.0e6 / bpm / ppqn;
            const uint32_t s = Seen(t);
            const bool beat = pc.Edge(s);
            if (beat) nb++;
            if (pc.RawPeriod() > 0 && (beat || lock.Period() <= 0)) lock.Update(pc.RawPeriod());
            if (lock.Period() > 0) { lo = std::fmin(lo, lock.Period()); hi = std::fmax(hi, lock.Period()); }
            /* The previous firmware, for comparison (one pulse = one beat). */
            if (old_last) { const float p = (s - old_last) * 1e-6f;
                if (old_p <= 0 || std::fabs(p - old_p) > 0.08f * old_p) old_p = p; else old_p += 0.3f * (p - old_p);
                old_lo = std::fmin(old_lo, old_p); old_hi = std::fmax(old_hi, old_p); }
            old_last = s;
        }
        return nb;
    }
};

int main()
{
    srand(11);
    for (int ppqn : {1, 24})
    {
        Rig r; r.pc.Reset(); r.lock.Reset();
        r.Run(120, ppqn, 4);
        CHECK(r.pc.Ppqn() == ppqn, "detected %d ppqn, sent %d", r.pc.Ppqn(), ppqn);
        CHECK(std::fabs(60.0f / r.lock.Period() - 120.0f) < 2.0f, "%d ppqn: reads %.1f BPM", ppqn, 60.0f / r.lock.Period());
        r.Run(120, ppqn, 4); r.Mark();
        const int nb = r.Run(120, ppqn, 64);
        CHECK(nb == 64, "%d ppqn: %d beats in 64", ppqn, nb);
        const float wob = (r.hi - r.lo) / 0.5f * 100.0f, old_wob = (r.old_hi - r.old_lo) / (0.5f / (ppqn == 1 ? 1 : 24)) * 100.0f;
        printf("%2d ppqn at 120 BPM: reads %.2f BPM, tempo moves %.3f%% over 64 beats (before: %.1f%%%s)\n",
               ppqn, 60.0f / r.lock.Period(), wob, old_wob, ppqn == 24 ? ", and read 2880 BPM" : "");
        CHECK(wob < 0.5f, "%d ppqn: locked tempo wobbles %.3f%%", ppqn, wob);
        CHECK(std::fabs(60.0f / r.lock.Period() - 120.0f) < 0.5f, "%d ppqn: settled at %.2f BPM", ppqn, 60.0f / r.lock.Period());

        /* Real tempo change: how many beats until within 1%? */
        int took = -1;
        for (int b = 1; b <= 12; b++) { r.Run(140, ppqn, 1); if (took < 0 && std::fabs(60.0f / r.lock.Period() - 140.0f) < 1.4f) took = b; }
        printf("            120 -> 140 BPM followed after %d beats (now %.2f)\n", took, 60.0f / r.lock.Period());
        CHECK(took > 0 && took <= 5, "%d ppqn: tempo change took %d beats", ppqn, took);

        /* Slow tempos and fast ones still detect correctly. */
        for (double bpm : {40.0, 90.0, 180.0, 280.0})
        {
            Rig q; q.pc.Reset(); q.lock.Reset(); q.Run(bpm, ppqn, 12);
            CHECK(q.pc.Ppqn() == ppqn && std::fabs(60.0f / q.lock.Period() - bpm) < bpm * 0.012,
                  "%d ppqn at %.0f BPM: detected %d ppqn, reads %.1f", ppqn, bpm, q.pc.Ppqn(), 60.0f / q.lock.Period());
        }
    }
    /* Switching the source from 1 to 24 ppqn re-detects. */
    { Rig r; r.pc.Reset(); r.lock.Reset(); r.Run(100, 1, 8); r.Run(100, 24, 8);
      CHECK(r.pc.Ppqn() == 24 && std::fabs(60.0f / r.lock.Period() - 100.0f) < 1.5f, "1 -> 24 ppqn switch: %d ppqn, %.1f BPM", r.pc.Ppqn(), 60.0f / r.lock.Period()); }
    /* Clock removed. */
    { PulseClock p; p.Reset(); uint32_t t = 1000000; for (int i = 0; i < 6; i++) { p.Edge(t); t += 500000; }
      CHECK(!p.Lost(t), "lost while running"); CHECK(p.Lost(t + 2000000), "not lost 2.5 s after the last pulse"); }

    printf(fails ? "%d FAIL\n" : "clock in OK\n", fails);
    return fails ? 1 : 0;
}
