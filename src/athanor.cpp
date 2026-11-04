/**
 * @file athanor.cpp
 * @brief Athanor — a performance end-of-chain effect for the Hermetic Modular
 *        Alchemy Lab.
 *
 * Three rows, flowing top -> bottom. In every row the LEFT knob is a bipolar
 * "amount" with a wide neutral deadband from 10 to 2 o'clock (snap it back to
 * the top and that row is bypassed). The RIGHT knob is a modifier. Each row's
 * button steps through that row's variants (remembered across power cycles):
 *
 *   B1  Row 1   1 Speed: tape slow-down / speed-up (pitch)  mod = mix
 *               2 Age:   tape / vinyl                      mod = wow+flutter / hiss+clicks
 *   B2  Row 2   1 Saturation  analog / digital             mod = tone
 *               2 Distortion  analog / digital
 *               3 Fuzz        analog / digital
 *   B3  Row 3   1 Reverb / delay + reverb                  mod = feedback
 *               2 Chorus / tremolo                         mod = rate
 *               3 Phaser / flanger                         mod = rate
 *
 * J3: clock in. Delay time follows the clock; LFO rates snap to divisions.
 *
 * Row 1 uses Capicola's keyframe time-stretch engine
 * (c) heavylight-industries — https://github.com/heavylight-industries/capicola
 * so this firmware is distributed under the GNU AGPL v3.0 (see LICENSE).
 */

#include <cmath>
#include <cstring>
#include <type_traits>

#include "daisy_seed.h"
#include "sys/dma.h"
#include "util/CpuLoadMeter.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/host.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/virtual_knob.h"

#include "age.h"
#include "crashlog.h"
#include "drive.h"
#include "fx.h"
#include "modfx.h"
#include "stretch_engine.h"

using namespace alchemy;

static_assert(std::is_same_v<AlchemyLab, AlchemyLabV2>,
              "Athanor requires Alchemy Lab V2 (build with BOARD=v2)");
using athanor::Clamp;
using athanor::LfoRate;

#ifndef ATHANOR_VERSION
#define ATHANOR_VERSION "0.3.0"
#endif
#ifndef ATHANOR_GIT_HASH
#define ATHANOR_GIT_HASH "dev"
#endif

/* ═════════════════════════════════════════════════════════════════════════
 *  Deadband geometry
 * ═════════════════════════════════════════════════════════════════════════
 * The LED arc (and the pot sweep) runs 7:30 -> 4:30, i.e. 9 clock-hours over
 * norm 0..1. The neutral zone spans 10 o'clock -> 2 o'clock. */
static constexpr float kArcStartHour = 7.5f;
static constexpr float kArcSpanHours = 9.0f;
static constexpr float kDeadLoHour   = 10.0f;
static constexpr float kDeadHiHour   = 14.0f;
static constexpr float kDeadLo = (kDeadLoHour - kArcStartHour) / kArcSpanHours;   // ~0.278
static constexpr float kDeadHi = (kDeadHiHour - kArcStartHour) / kArcSpanHours;   // ~0.722

/** Split an amount knob into side (-1 / 0 / +1) and depth 0..1 within it. */
static inline void SplitAmount(float n, int& side, float& amt)
{
    if (n < kDeadLo)      { side = -1; amt = (kDeadLo - n) / kDeadLo; }
    else if (n > kDeadHi) { side = +1; amt = (n - kDeadHi) / (1.0f - kDeadHi); }
    else                  { side = 0;  amt = 0.0f; }
    amt = Clamp(amt, 0.0f, 1.0f);
}

/* ═════════════════════════════════════════════════════════════════════════
 *  Variants and colours
 * ═════════════════════════════════════════════════════════════════════════ */
using Rgb = LedPanel::Rgb;

struct VariantLook
{
    Rgb left, right;   // amount ring: CCW / CW flavour
    Rgb mod;           // modifier ring
    Rgb button;        // row button
};

static constexpr uint8_t kNumVariants[3] = {2, 3, 3};

static constexpr VariantLook kLooks[3][3] = {
    {   /* Row 1 */
        {{0x00, 0x50, 0xFF}, {0x00, 0xFF, 0x90}, {0x40, 0xA0, 0xFF}, {0x00, 0x90, 0xFF}},  // time
        {{0xFF, 0x60, 0x10}, {0x20, 0xC0, 0xC0}, {0xFF, 0xB0, 0x60}, {0xFF, 0x70, 0x20}},  // tape/vinyl
        {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
    },
    {   /* Row 2: analog warm / digital cool; level shown on button + tone ring */
        {{0xFF, 0x60, 0x00}, {0x00, 0xA0, 0xFF}, {0x40, 0xFF, 0x40}, {0x40, 0xFF, 0x40}},  // sat
        {{0xFF, 0x60, 0x00}, {0x00, 0xA0, 0xFF}, {0xFF, 0xA0, 0x00}, {0xFF, 0xA0, 0x00}},  // dist
        {{0xFF, 0x60, 0x00}, {0x00, 0xA0, 0xFF}, {0xFF, 0x10, 0x10}, {0xFF, 0x10, 0x10}},  // fuzz
    },
    {   /* Row 3 */
        {{0x90, 0x30, 0xFF}, {0xFF, 0xC0, 0x10}, {0xC0, 0x80, 0xFF}, {0x90, 0x30, 0xFF}},  // verb/delay
        {{0x00, 0xC0, 0xFF}, {0xFF, 0x30, 0x80}, {0x80, 0xD0, 0xFF}, {0x00, 0xC0, 0xFF}},  // chorus/trem
        {{0x40, 0xFF, 0x60}, {0xFF, 0xF0, 0x40}, {0xC0, 0xFF, 0x80}, {0x80, 0xFF, 0x40}},  // phaser/flanger
    },
};

static constexpr Rgb kNeutral {0x30, 0x30, 0x30};
static constexpr Rgb kWhite   {0xFF, 0xFF, 0xFF};

static volatile uint8_t variant[3] = {0, 0, 0};

/* ═════════════════════════════════════════════════════════════════════════
 *  Rings
 * ═════════════════════════════════════════════════════════════════════════ */
struct AmountStyle { Rgb left, right; };
struct ModStyle    { Rgb color; bool bipolar; bool clock_pip; };

static AmountStyle amountStyle[3];
static ModStyle    modStyle[3];

static volatile float    clockPeriodSec = 0.0f;   // 0 = no clock
static volatile uint32_t clockEdgeMs    = 0;
static volatile uint32_t clockEdges     = 0;

static void ApplyLooks()
{
    for (int r = 0; r < 3; r++)
    {
        const VariantLook& l = kLooks[r][variant[r]];
        amountStyle[r] = {l.left, l.right};
        modStyle[r].color = l.mod;
    }
}

/** Amount ring: dim deadband, each side tinted, lit from the edge to the pointer. */
static void DrawAmountRing(LedPanel& p, uint8_t pot, const ArcGeometry& g,
                           float norm, uint32_t /*t_ms*/, void* ctx)
{
    const AmountStyle& s = *static_cast<const AmountStyle*>(ctx);
    const uint8_t last  = (g.arc_leds > 1) ? (uint8_t)(g.arc_leds - 1) : 1u;
    const int  ptr     = (int)(norm * (float)last + 0.5f);
    const bool neutral = (norm >= kDeadLo && norm <= kDeadHi);

    for (uint8_t i = 0; i < g.arc_leds; i++)
    {
        const float hour = g.start_hour + (float)i * g.step_hours;
        const float ln   = (float)i / (float)last;
        Rgb c;
        if (ln >= kDeadLo - 1e-3f && ln <= kDeadHi + 1e-3f)
            c = (neutral && i == ptr) ? kWhite : LedPanel::Scale(kNeutral, 0.5f);
        else if (ln < kDeadLo)
            c = (norm < kDeadLo && ln >= norm - 0.5f / (float)last)
                    ? s.left : LedPanel::Scale(s.left, 0.08f);
        else
            c = (norm > kDeadHi && ln <= norm + 0.5f / (float)last)
                    ? s.right : LedPanel::Scale(s.right, 0.08f);
        p.SetRingByHour(pot, hour, p.ScaleGlobal(c));
    }
}

/** Modifier ring: level (or bipolar from noon) in the variant colour. */
static void DrawModRing(LedPanel& p, uint8_t pot, const ArcGeometry& g,
                        float norm, uint32_t t_ms, void* ctx)
{
    const ModStyle& s = *static_cast<const ModStyle*>(ctx);
    const uint8_t last = (g.arc_leds > 1) ? (uint8_t)(g.arc_leds - 1) : 1u;
    const float   half = 0.5f / (float)last;
    for (uint8_t i = 0; i < g.arc_leds; i++)
    {
        const float hour = g.start_hour + (float)i * g.step_hours;
        const float ln   = (float)i / (float)last;
        bool lit;
        if (s.bipolar)
            lit = (norm >= 0.5f) ? (ln >= 0.5f - half && ln <= norm + half)
                                 : (ln <= 0.5f + half && ln >= norm - half);
        else
            lit = ln <= norm + half;
        p.SetRingByHour(pot, hour, p.ScaleGlobal(lit ? s.color : LedPanel::Scale(s.color, 0.08f)));
    }
    if (s.clock_pip && clockPeriodSec > 0.0f && (t_ms - clockEdgeMs) < 50u)
        p.SetRingByHour(pot, 12.0f, p.ScaleGlobal(kWhite));
}

/* ═════════════════════════════════════════════════════════════════════════
 *  Knobs (pot indices: 0 1 / 2 3 / 4 5 = left/right per row)
 * ═════════════════════════════════════════════════════════════════════════ */
static VirtualKnob timeAmt  = VirtualKnob(kPotTopLeft, "Time")
    .Ident("time.amount")
    .Help("Speed: CCW slows the tape to a stop, CW speeds it up to 2x (with reel inertia). Age: CCW tape, CW vinyl. 10-2 o'clock: off.")
    .Ring(Custom(DrawAmountRing, &amountStyle[0]));
static VirtualKnob timeMix  = VirtualKnob(kPotTopRight, "Mix")
    .Ident("time.mix")
    .Help("Speed: dry/wet. Tape: wow and flutter. Vinyl: hiss, crackle and clicks.")
    .Ring(Custom(DrawModRing, &modStyle[0]));
static VirtualKnob driveAmt = VirtualKnob(kPotMiddleLeft, "Drive")
    .Ident("drive.amount")
    .Help("CCW: analog. CW: digital. B2 picks saturation / distortion / fuzz. 10-2 o'clock: off.")
    .Ring(Custom(DrawAmountRing, &amountStyle[1]));
static VirtualKnob driveTone = VirtualKnob(kPotMiddleRight, "Tone")
    .Ident("drive.tone")
    .Help("Tilt EQ on the drive: noon flat, CCW darker, CW brighter.")
    .Ring(Custom(DrawModRing, &modStyle[1]));
static VirtualKnob spaceAmt = VirtualKnob(kPotBottomLeft, "Space")
    .Ident("space.amount")
    .Help("Reverb/delay, chorus/tremolo or phaser/flanger (B3). 10-2 o'clock: off.")
    .Ring(Custom(DrawAmountRing, &amountStyle[2]));
static VirtualKnob spaceFb  = VirtualKnob(kPotBottomRight, "Feedback")
    .Ident("space.feedback")
    .Help("Reverb/delay: feedback. Chorus/trem/phaser/flanger: rate (clock-synced on J3).")
    .Ring(Custom(DrawModRing, &modStyle[2]));

static Page mainPage = Page(0).Name("Athanor").Knobs(timeAmt, timeMix,
                                                    driveAmt, driveTone,
                                                    spaceAmt, spaceFb);

/* ═════════════════════════════════════════════════════════════════════════
 *  Hardware, DSP
 * ═════════════════════════════════════════════════════════════════════════ */
static AlchemyLab     hw;
static ControlLoop    loop(hw);
static hostlink::Host host("athanor", "Athanor", ATHANOR_VERSION, ATHANOR_GIT_HASH);

static constexpr uint16_t kBlock = 64;

static athanor::StretchEngine stretch;
static athanor::Age           age;
static athanor::Drive         drive;
static athanor::Space         space;
static athanor::ChorusTrem    chorusTrem;
static athanor::PhaserFlanger phaserFlanger;

/* Reverb in internal D2 SRAM (see athanor.lds); delays stay in SDRAM. */
static float __attribute__((section(".d2_bss"), aligned(4))) plateMem[athanor::Plate::kMemFloats];
static float DSY_SDRAM_BSS delayL[athanor::Space::kDelayFloats];
static float DSY_SDRAM_BSS delayR[athanor::Space::kDelayFloats];
static float DSY_SDRAM_BSS ageMem[2 * athanor::Age::kWowFloats];
static float DSY_SDRAM_BSS chorusMem[2 * athanor::ChorusTrem::kFloats];
static float DSY_SDRAM_BSS phaserMem[2 * athanor::PhaserFlanger::kFloats];

namespace crash = athanor::crash;

/* Row 1 smoothing / state (audio thread only). */
static athanor::Smooth engageSm, mixSm;
static bool            timeActive = false;
static float           reelPitch  = 1.0f;     // playback speed actually played
static float           reelVel    = 0.0f;     // its rate of change (per second)
static constexpr float kReelHz      = 1.6f;   // reel "spring": ~0.3 s to settle
static constexpr float kReelDamping = 0.5f;   // < 1: overshoots ~16%, tape-like
static constexpr float kMinPitch    = 0.02f;  // never quite 0 (fades still finish)

/* Row 3 state (audio thread only). */
static float    spaceFbHeld   = 0.5f;   // reverb/delay feedback while another variant plays
static uint32_t spaceQuiet    = 0;      // blocks of silent tail
static bool     spaceAsleep   = false;
static int      lastModSide[2] = {-1, -1};

/* CPU meter. B3: red = an audio block overran in the last 2 s,
 * amber = average load above 80%. The first 3 s after boot are ignored. */
static daisy::CpuLoadMeter cpu;
static volatile float      cpuAvg      = 0.0f;
static volatile uint32_t   overrunMs   = 0;
static uint32_t            blockStart  = 0;
static float               ticksPerBlockInv = 0.0f;

/* ═════════════════════════════════════════════════════════════════════════
 *  Variant memory: one QSPI sector at the very top of the preset region
 *  (well clear of the SDK's preset slots and the calibration record).
 * ═════════════════════════════════════════════════════════════════════════ */
static constexpr uint32_t kSaveAddr  = 0x907FF000u;
static constexpr uint32_t kSaveMagic = 0x4E485441u;   // "ATHN"

struct SavedState
{
    uint32_t magic;
    uint8_t  v[3];
    uint8_t  version;
    uint32_t check;
};

static uint32_t SaveCheck(const SavedState& s)
{
    return s.magic ^ (0x9E3779B9u * (1u + s.v[0] + 7u * s.v[1] + 49u * s.v[2] + 343u * s.version));
}

static void LoadVariants()
{
    dsy_dma_invalidate_cache_for_buffer(reinterpret_cast<uint8_t*>(kSaveAddr), sizeof(SavedState));
    SavedState s;
    std::memcpy(&s, reinterpret_cast<const void*>(kSaveAddr), sizeof(s));
    if (s.magic != kSaveMagic || s.version != 1 || s.check != SaveCheck(s)) return;
    for (int r = 0; r < 3; r++)
        if (s.v[r] < kNumVariants[r]) variant[r] = s.v[r];
}

static void SaveVariants()
{
    SavedState s{};
    s.magic   = kSaveMagic;
    for (int r = 0; r < 3; r++) s.v[r] = variant[r];
    s.version = 1;
    s.check   = SaveCheck(s);
    hw.seed.qspi.Erase(kSaveAddr, kSaveAddr + 4096u);
    hw.seed.qspi.Write(kSaveAddr, sizeof(s), reinterpret_cast<uint8_t*>(&s));
    dsy_dma_invalidate_cache_for_buffer(reinterpret_cast<uint8_t*>(kSaveAddr), sizeof(s));
}

static uint32_t dirtyMs = 0;   // last variant change (0 = saved)
static volatile bool healthy = false;   // survived the first 5 s

/* ═════════════════════════════════════════════════════════════════════════
 *  J3 clock + buttons (1 ms poll)
 * ═════════════════════════════════════════════════════════════════════════ */
static constexpr float    kClkHiV      = 1.5f;
static constexpr float    kClkLoV      = 0.6f;
static constexpr float    kFallbackSec = 0.375f;
static bool               clkHigh      = false;
static uint32_t           clkLastUs    = 0;
static float              clkPeriod    = 0.0f;

static void OnPoll(uint32_t t_ms)
{
    if (!healthy && t_ms > 5000u) { healthy = true; crash::Mark(crash::kRunning); }

    /* Buttons: each press steps that row to its next variant. */
    for (int b = 0; b < 3; b++)
    {
        if (hw.buttons[b].RisingEdge())
        {
            variant[b] = (uint8_t)((variant[b] + 1u) % kNumVariants[b]);
            ApplyLooks();
            dirtyMs = t_ms ? t_ms : 1u;
        }
    }
    /* Save 1.5 s after the last change (one sector erase, ~50 ms). */
    if (dirtyMs != 0 && (t_ms - dirtyMs) > 1500u)
    {
        dirtyMs = 0;
        SaveVariants();
    }

    const uint32_t now = loop.PollTimeUs();
    const float    v   = hw.j3.Volts();

    if (!clkHigh && v > kClkHiV)
    {
        clkHigh = true;
        if (clkLastUs != 0)
        {
            const float p = (float)(now - clkLastUs) * 1.0e-6f;
            if (p > 0.02f && p < 6.0f)
            {
                /* Jump on tempo changes > 8%, otherwise average out jitter. */
                if (clkPeriod <= 0.0f || std::fabs(p - clkPeriod) > 0.08f * clkPeriod)
                    clkPeriod = p;
                else
                    clkPeriod += 0.3f * (p - clkPeriod);
                clockPeriodSec = clkPeriod;
            }
        }
        clkLastUs   = now;
        clockEdgeMs = t_ms;
        clockEdges  = clockEdges + 1u;
    }
    else if (clkHigh && v < kClkLoV)
        clkHigh = false;

    /* Clock lost: no edge for 2.5 periods (min 1 s). */
    if (clkPeriod > 0.0f)
    {
        float timeout = 2.5f * clkPeriod;
        if (timeout < 1.0f) timeout = 1.0f;
        if ((float)(now - clkLastUs) * 1.0e-6f > timeout)
        {
            clkPeriod      = 0.0f;
            clockPeriodSec = 0.0f;
            clkLastUs      = 0;
        }
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 *  Button LEDs: the row's current variant colour (B3 doubles as CPU meter)
 * ═════════════════════════════════════════════════════════════════════════ */
static void OnRender(uint32_t /*t_ms*/)
{
    for (int b = 0; b < 3; b++)
    {
        Rgb c = LedPanel::Scale(kLooks[b][variant[b]].button, 0.35f);
        if (b == 2)
        {
            const uint32_t now = daisy::System::GetNow();
            if (cpuAvg > 0.8f) c = Rgb{0xFF, 0x80, 0x00};
            if (overrunMs != 0 && (now - overrunMs) < 2000u) c = Rgb{0xFF, 0x00, 0x00};
        }
        hw.leds.SetButtonPair((uint8_t)b, hw.leds.ScaleGlobal(c));
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 *  LFO rates for the modulation variants
 * ═════════════════════════════════════════════════════════════════════════
 * Free: exponential sweep of the modifier knob. Clocked (J3): the knob picks
 * a division, as LFO cycles per clock pulse (a pulse is usually a beat). */
static LfoRate RateFor(int fx, float m, float clk)
{
    static constexpr float kLo[4] = {0.05f, 0.5f, 0.05f, 0.03f};
    static constexpr float kHi[4] = {3.0f, 14.0f, 4.0f, 2.0f};
    static constexpr float kChorus[]  = {0.125f, 0.25f, 0.5f, 1.0f};
    static constexpr float kTrem[]    = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
    static constexpr float kPhaser[]  = {0.0625f, 0.125f, 0.25f, 0.5f, 1.0f};
    static constexpr float kFlanger[] = {0.0625f, 0.125f, 0.25f, 0.5f};
    m = Clamp(m, 0.0f, 1.0f);
    if (clk > 0.0f)
    {
        const float* t; int n;
        switch (fx)
        {
            case 0:  t = kChorus;  n = 4; break;
            case 1:  t = kTrem;    n = 8; break;
            case 2:  t = kPhaser;  n = 5; break;
            default: t = kFlanger; n = 4; break;
        }
        int i = (int)(m * (float)n);
        if (i >= n) i = n - 1;
        return {t[i] / clk, t[i]};
    }
    return {kLo[fx] * std::pow(kHi[fx] / kLo[fx], m), 0.0f};
}

/* ═════════════════════════════════════════════════════════════════════════
 *  Audio
 * ═════════════════════════════════════════════════════════════════════════ */
static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           n)
{
    crash::Mark(crash::kAudioEnter);
    cpu.OnBlockStart();
    blockStart = daisy::System::GetTick();
    if (n > kBlock) n = kBlock;

    const float    sr      = hw.SampleRate();
    const float    clk     = clockPeriodSec;
    const float    beatSec = (clk > 0.0f) ? clk : kFallbackSec;
    const uint32_t edges   = clockEdges;
    const int      v1 = variant[0], v2 = variant[1], v3 = variant[2];

    crash::Mark(crash::kAudioRow1);
    /* ── Row 1: tape speed (Capicola pitch) / tape + vinyl ─────────── */
    int   tSide; float tAmt;
    SplitAmount(timeAmt.Norm(), tSide, tAmt);
    const int   stSide = (v1 == 0) ? tSide : 0;
    const float mod1   = timeMix.Norm();

    /* Capicola's PITCH control with tape-reel inertia. The knob sets a target
     * playback speed (CCW 1x -> stopped, CW 1x -> 2x); the speed actually
     * played is a mass-spring-damper chasing it, so moves swoop like a heavy
     * reel spinning up or down and overshoot a little. Timing stays locked to
     * the input (Capicola's stretch grid and transient snapping do that).
     * Releasing the knob back to neutral lets the reel spin back to 1x
     * audibly before the row hands back to the dry signal. */
    float pitchTarget = 1.0f;
    if (stSide < 0)      pitchTarget = 1.0f - tAmt;          // 1 -> 0 (tape stop)
    else if (stSide > 0) pitchTarget = 1.0f + tAmt;          // 1 -> 2 (octave up)
    {
        const float dt = (float)n / sr;
        const float w  = 2.0f * athanor::kPi * kReelHz;
        reelVel += (w * w * (pitchTarget - reelPitch) - 2.0f * kReelDamping * w * reelVel) * dt;
        reelPitch += reelVel * dt;
        if (reelPitch < kMinPitch) { reelPitch = kMinPitch; if (reelVel < 0.0f) reelVel = 0.0f; }
        if (reelPitch > 2.4f)      { reelPitch = 2.4f;      if (reelVel > 0.0f) reelVel = 0.0f; }
    }
    stretch.SetPitch(reelPitch);

    const bool spinning = std::fabs(reelPitch - 1.0f) > 0.004f || std::fabs(reelVel) > 0.02f;
    const bool active   = (v1 == 0) && (stSide != 0 || spinning);
    if (active && !timeActive) stretch.Resync();     // engage from "now"
    timeActive = active;

    /* Near a full stop the tape runs out of level, like a real tape stop. */
    const float stopGain = Clamp(reelPitch / 0.12f, 0.0f, 1.0f);
    const float engage   = engageSm.Tick(active ? 1.0f : 0.0f);
    const float wet      = engage * mixSm.Tick(v1 == 0 ? mod1 : mixSm.v);

    static float wl[kBlock], wr[kBlock];
    float*       bl = out[0];
    float*       br = out[1];
    stretch.Process(in[0], in[1], wl, wr, n);   // always runs: keeps its history live
    for (size_t i = 0; i < n; i++)
    {
        bl[i] = in[0][i] + wet * (stopGain * wl[i] - in[0][i]);
        br[i] = in[1][i] + wet * (stopGain * wr[i] - in[1][i]);
    }

    crash::Mark(crash::kAudioAge);
    age.SetParams((v1 == 1) ? tSide : 0, tAmt, mod1);
    if (age.Busy()) age.Process(bl, br, n);

    crash::Mark(crash::kAudioDrive);
    /* ── Row 2: drive ────────────────────────────────────────────────── */
    int   dSide; float dAmt;
    SplitAmount(driveAmt.Norm(), dSide, dAmt);
    drive.SetParams(v2, dSide, dAmt, driveTone.Norm());
    drive.Process(bl, br, n);

    crash::Mark(crash::kAudioMod);
    /* ── Row 3: space / chorus + trem / phaser + flanger ─────────────── */
    int   sSide; float sAmt;
    SplitAmount(spaceAmt.Norm(), sSide, sAmt);
    const float mod3 = spaceFb.Norm();

    {
        const int side = (v3 == 1) ? sSide : 0;
        if (side != 0) lastModSide[0] = side;
        chorusTrem.SetParams(side, sAmt, RateFor(lastModSide[0] < 0 ? 0 : 1, mod3, clk), edges);
        if (chorusTrem.Busy()) chorusTrem.Process(bl, br, n);
    }
    {
        const int side = (v3 == 2) ? sSide : 0;
        if (side != 0) lastModSide[1] = side;
        phaserFlanger.SetParams(side, sAmt, RateFor(lastModSide[1] < 0 ? 2 : 3, mod3, clk), edges);
        if (phaserFlanger.Busy()) phaserFlanger.Process(bl, br, n);
    }

    crash::Mark(crash::kAudioSpace);
    space.SetDelaySeconds(beatSec);
    if (v3 == 0)
    {
        spaceFbHeld = mod3;
        space.SetParams(sSide, sAmt, mod3);
        spaceAsleep = false;
        spaceQuiet  = 0;
    }
    else
        space.SetParams(0, 0.0f, spaceFbHeld);   // sends closed, tails ring out
    if (!spaceAsleep)
    {
        space.Process(bl, br, n);
        if (v3 != 0)
        {
            /* Sleep once the tail has been below -80 dB for a second. */
            spaceQuiet = (space.WetPeak() < 1.0e-4f) ? spaceQuiet + 1u : 0u;
            if (spaceQuiet > (uint32_t)(sr / (float)kBlock)) spaceAsleep = true;
        }
    }

    for (size_t i = 0; i < n; i++)
    {
        bl[i] = athanor::OutputLimit(bl[i]);
        br[i] = athanor::OutputLimit(br[i]);
    }

    crash::Mark(healthy ? (uint32_t)crash::kRunning : (uint32_t)crash::kFirstTick);
    cpu.OnBlockEnd();
    cpuAvg = cpu.GetAvgCpuLoad();
    const float    load = (float)(daisy::System::GetTick() - blockStart) * ticksPerBlockInv;
    const uint32_t ms   = daisy::System::GetNow();
    if (load > 0.98f && ms > 3000u) overrunMs = ms ? ms : 1u;
}

/* ═════════════════════════════════════════════════════════════════════════
 *  Main
 * ═════════════════════════════════════════════════════════════════════════ */
/* ═════════════════════════════════════════════════════════════════════════
 *  Safe mode: the previous boot crashed. Show where, and keep HostLink up so
 *  the web programmer can reflash without the B3 dance. See README.
 * ═════════════════════════════════════════════════════════════════════════ */
static crash::Record crashSnap;

static void DrawBits(uint8_t pot, uint32_t bits, uint8_t n, Rgb on, Rgb off)
{
    for (uint8_t i = 0; i < 13; i++)
    {
        Rgb c = (i < n) ? (((bits >> i) & 1u) ? on : off) : Rgb{0, 0, 0};
        hw.leds.SetRingByHour(pot, 7.5f + 0.75f * (float)i, hw.leds.ScaleGlobal(c));
    }
}

static void DrawCount(uint8_t pot, uint32_t count, Rgb on)
{
    for (uint8_t i = 0; i < 13; i++)
        hw.leds.SetRingByHour(pot, 7.5f + 0.75f * (float)i,
                              hw.leds.ScaleGlobal(i < count ? on : Rgb{0x08, 0x08, 0x08}));
}

static void SafeRender(uint32_t t_ms)
{
    const uint32_t st = crashSnap.stage;
    DrawCount(kPotTopLeft, st > 13 ? 13 : st, Rgb{0xFF, 0x00, 0x00});
    DrawCount(kPotMiddleLeft, st > 13 ? st - 13 : 0, Rgb{0xFF, 0x80, 0x00});
    static constexpr Rgb kKind[5] = {{0x40, 0x40, 0x40}, {0xFF, 0xFF, 0xFF}, {0x00, 0x40, 0xFF},
                                     {0xFF, 0xFF, 0x00}, {0xFF, 0x00, 0xFF}};
    DrawCount(kPotTopRight, 13, kKind[crashSnap.kind < 5 ? crashSnap.kind : 0]);
    const uint32_t off = crashSnap.pc - 0x24000000u;
    DrawBits(kPotMiddleRight, off >> 1, 13, Rgb{0xFF, 0xFF, 0xFF}, Rgb{0x00, 0x00, 0x30});
    DrawBits(kPotBottomRight, off >> 14, 6, Rgb{0xFF, 0xFF, 0xFF}, Rgb{0x00, 0x00, 0x30});
    DrawBits(kPotBottomLeft, crashSnap.cfsr >> 8, 8, Rgb{0x00, 0xFF, 0x00}, Rgb{0x00, 0x20, 0x00});
    const Rgb b = ((t_ms / 400u) & 1u) ? Rgb{0xFF, 0x00, 0x00} : Rgb{0x20, 0x00, 0x00};
    for (uint8_t i = 0; i < 3; i++) hw.leds.SetButtonPair(i, hw.leds.ScaleGlobal(b));
}

[[noreturn]] static void SafeMode()
{
    crashSnap.stage = crash::rec->stage;
    crashSnap.kind  = crash::rec->kind;
    crashSnap.pc    = crash::rec->pc;
    crashSnap.lr    = crash::rec->lr;
    crashSnap.cfsr  = crash::rec->cfsr;
    crashSnap.hfsr  = crash::rec->hfsr;
    crashSnap.bfar  = crash::rec->bfar;
    crashSnap.count = crash::rec->count;
    crash::Acknowledge();   // next power-on boots normally again

    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlock);
    loop.Use(host).OnRender(SafeRender);
    for (;;) loop.Tick();
}

int main()
{
    if (crash::PreviousBootCrashed()) SafeMode();
    crash::Mark(crash::kMainStart);

    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlock);
    const float sr = hw.SampleRate();
    crash::Mark(crash::kHwInit);

    LoadVariants();
    crash::Mark(crash::kLoadVariants);
    modStyle[0] = {{0, 0, 0}, false, false};
    modStyle[1] = {{0, 0, 0}, true, false};    // tone: bipolar from noon
    modStyle[2] = {{0, 0, 0}, false, true};    // blinks on J3 clock
    ApplyLooks();

    stretch.Init(sr);
    crash::Mark(crash::kStretchInit);
    age.Init(sr, ageMem);
    crash::Mark(crash::kAgeInit);
    drive.Init(sr);
    crash::Mark(crash::kDriveInit);
    space.Init(sr, plateMem, delayL, delayR);
    crash::Mark(crash::kSpaceInit);
    chorusTrem.Init(sr, chorusMem);
    crash::Mark(crash::kChorusInit);
    phaserFlanger.Init(sr, phaserMem);
    crash::Mark(crash::kPhaserInit);

    /* Per-block smoothers (64-sample blocks ~ 1.33 ms). */
    engageSm.Init(0.0f, 0.08f);     // ~17 ms
    mixSm.Init(1.0f, 0.08f);

    cpu.Init(sr, kBlock);
    ticksPerBlockInv = sr / ((float)daisy::System::GetTickFreq() * (float)kBlock);

    loop.Use(mainPage)
        .Use(host)
        .OnPoll(OnPoll)
        .OnRender(OnRender);
    crash::Mark(crash::kLoopSetup);

    hw.StartAudio(AudioCallback);
    crash::Mark(crash::kAudioStarted);

    loop.Tick();
    crash::Mark(crash::kFirstTick);
    for (;;) loop.Tick();
}
