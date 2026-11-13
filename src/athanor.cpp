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
 *               3 Interval: time-locked pitch shifter, snapped to musical steps
 *                        harmonic ladder (only the root and its fifth):
 *                        CCW -4th / -oct / -oct-4th / -2 oct,
 *                        CW  +5th / +oct / +oct+5th / +2 oct    mod = mix
 *   B2  Row 2   1 Saturation  analog / digital             mod = tone
 *               2 Distortion  analog / digital
 *               3 Fuzz        analog / digital
 *   B3  Row 3   1 Reverb / delay + reverb                  mod = feedback
 *               2 Chorus / tremolo                         mod = rate
 *               3 Phaser / flanger                         mod = rate
 *
 * J3: clock in, one pulse per beat or 24 ppqn (detected). Delay time follows
 * the clock; LFO rates snap to divisions.
 * J4 / J5 / J6: CV into the Time / Drive / Space rows. Each jack can move
 * both knobs of its row, each with its own depth (attenuverter): hold the
 * row's button and turn the knob you want the CV to move.
 *
 * Hold B1 + B3 together for half a second to start / stop recording the
 * output to the SD card (REC0001.WAV, ... 24-bit stereo).
 *
 * Press B2 + B3 together to step the delay's subdivision (1/2 ... 1/16 of
 * the clock's beat).
 *
 * Press B1 + B2 together to bypass all three rows (again to bring them
 * back); a gate on J7 does the same while it is high.
 *
 * USB MIDI (same cable as the web programmer): MIDI clock drives the delay
 * time and LFO rates when nothing is patched to J3; CCs 20..30 play the six
 * knobs, the variants, record and bypass. See README.
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
#include "alchemy/version.h"
#include "alchemy/host_link/diagnostics.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/control/tick_timebase.h"
#include "alchemy/midi/types.h"
#include "alchemy/midi/usb_transport.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/virtual_knob.h"

#include "age.h"
#include "clock_in.h"
#include "crashlog.h"
#include "drive.h"
#include "fx.h"
#include "midi_clock.h"
#include "modfx.h"
#include "recorder.h"
#include "shifter.h"
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

static constexpr uint8_t kNumVariants[3] = {3, 3, 3};

static constexpr VariantLook kLooks[3][3] = {
    {   /* Row 1 */
        {{0x00, 0x50, 0xFF}, {0x00, 0xFF, 0x90}, {0x40, 0xA0, 0xFF}, {0x00, 0x90, 0xFF}},  // time
        {{0xFF, 0x60, 0x10}, {0x20, 0xC0, 0xC0}, {0xFF, 0xB0, 0x60}, {0xFF, 0x70, 0x20}},  // tape/vinyl
        {{0xFF, 0x20, 0xC0}, {0xFF, 0xD0, 0x00}, {0xD0, 0x90, 0xFF}, {0xC0, 0x20, 0xFF}},  // interval
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

/* Delay subdivision: the delay time as a multiple of the beat. B2 + B3
 * (or MIDI CC 31) steps through them, longest to shortest. */
struct DelayDiv { float beats; const char* name; };
static constexpr DelayDiv kDelayDivs[] = {
    {2.0f,        "1/2"},
    {1.5f,        "dotted 1/4"},
    {1.0f,        "1/4"},
    {0.75f,       "dotted 1/8"},
    {2.0f / 3.0f, "1/4 triplet"},
    {0.5f,        "1/8"},
    {1.0f / 3.0f, "1/8 triplet"},
    {0.25f,       "1/16"},
};
static constexpr uint8_t kNumDelayDivs   = sizeof(kDelayDivs) / sizeof(kDelayDivs[0]);
static constexpr uint8_t kDefaultDelayDiv = 2;      // one beat
static volatile uint8_t  delayDiv        = kDefaultDelayDiv;
static volatile uint32_t delayDivShownMs = 0;       // ring shows the choice briefly (0 = not showing)

/* ═════════════════════════════════════════════════════════════════════════
 *  Rings
 * ═════════════════════════════════════════════════════════════════════════ */
struct AmountStyle { Rgb left, right; int row; };
struct ModStyle    { Rgb color; bool bipolar; bool clock_pip; int row; };

/* ── CV modulation ──────────────────────────────────────────────────────
 * J4 / J5 / J6 each feed one row, and can move BOTH of that row's knobs,
 * each with its own depth (a per-knob attenuverter: noon = off, CW = +,
 * CCW = inverted). Hold the row's button and turn the knob you want to
 * modulate; its ring shows the depth while you hold. +-5 V at full depth
 * moves the virtual knob +-half its travel, so a bipolar LFO on an amount
 * knob can sweep from one effect, through the bypass deadband, into the
 * other. Depths are indexed by pot: 0 1 / 2 3 / 4 5 = left/right per row. */
static constexpr uint8_t kCvJack[3] = {1u, 2u, 3u};        // cv_jacks[] index: J4, J5, J6
static constexpr uint8_t kAmtPot[3] = {0u, 2u, 4u};        // left-hand knob per row
static constexpr uint8_t kModPot[3] = {1u, 3u, 5u};        // right-hand knob per row
static volatile float cvDepth[6]  = {0, 0, 0, 0, 0, 0};    // -1..+1, per pot
static volatile float cvOffset[6] = {0, 0, 0, 0, 0, 0};    // depth * volts / 10 (norm units)
static float          cvLp[3]     = {0.0f, 0.0f, 0.0f};
static volatile float amtValue[3] = {0.5f, 0.5f, 0.5f};    // effective amount (held while editing)
static volatile float modValue[3] = {1.0f, 0.5f, 0.5f};    // effective modifier
static volatile int8_t editing[3] = {-1, -1, -1};          // pot whose depth is being set, -1 none
static volatile bool  catching[6] = {false, false, false, false, false, false}; // waiting for pickup
static float          heldVal[6]  = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};       // value held meanwhile

/* Bypass-all: B1 + B2 (or MIDI CC 30) latches it, a gate on J7 holds it.
 * Every amount knob is treated as if it sat in the deadband, so it behaves
 * exactly like throwing all three knobs to the top: tails ring out, the
 * reel spins back to normal speed. */
static bool          bypassLatch = false;
static bool          bypassGate  = false;
static volatile bool bypassAll   = false;

struct Gesture
{
    bool   down     = false;
    int8_t target   = -1;     // pot being edited this hold (-1 = none yet)
    bool   consumed = false;  // used by a chord: no variant step on release
    float  refAmt   = 0.0f;   // knob positions at press
    float  refMod   = 0.0f;
};
static Gesture gesture[3];

static inline float CvNorm(float knob, uint8_t pot)
{
    return Clamp(knob + cvOffset[pot], 0.0f, 1.0f);
}

/** An amount knob as played: knob (or MIDI) + CV, or neutral while bypassed. */
static inline float AmtNorm(int row)
{
    return bypassAll ? 0.5f : CvNorm(amtValue[row], (uint8_t)(row * 2));
}

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
        amountStyle[r].left  = l.left;
        amountStyle[r].right = l.right;
        amountStyle[r].row   = r;
        modStyle[r].color    = l.mod;
        modStyle[r].row      = r;
    }
}

/** While a row button is held and a knob is being assigned: that knob's CV
 *  depth, bipolar from noon (white = positive, cyan = inverted). */
static void DrawDepth(LedPanel& p, uint8_t pot, const ArcGeometry& g, float d)
{
    const uint8_t last = (g.arc_leds > 1) ? (uint8_t)(g.arc_leds - 1) : 1u;
    const float   half = 0.5f / (float)last;
    const float   dn   = 0.5f + 0.5f * d;
    const Rgb     dc   = (d >= 0.0f) ? kWhite : Rgb{0x00, 0xC0, 0xFF};
    for (uint8_t i = 0; i < g.arc_leds; i++)
    {
        const float ln = (float)i / (float)last;
        const bool lit = (dn >= 0.5f) ? (ln >= 0.5f - half && ln <= dn + half)
                                      : (ln <= 0.5f + half && ln >= dn - half);
        p.SetRingByHour(pot, g.start_hour + (float)i * g.step_hours,
                        p.ScaleGlobal(lit ? dc : Rgb{0x06, 0x06, 0x06}));
    }
}

/** Dim pip where the knob physically sits, when CV / pickup moves the value away. */
static void DrawPhysPip(LedPanel& p, uint8_t pot, const ArcGeometry& g, float phys, float shown)
{
    if (std::fabs(shown - phys) <= 0.02f) return;
    const uint8_t last = (g.arc_leds > 1) ? (uint8_t)(g.arc_leds - 1) : 1u;
    p.SetRingByHour(pot, g.start_hour + (float)(int)(phys * (float)last + 0.5f) * g.step_hours,
                    p.ScaleGlobal(Rgb{0x50, 0x50, 0x50}));
}

/** Amount ring: dim deadband, each side tinted, lit from the edge to the pointer. */
static void DrawAmountRing(LedPanel& p, uint8_t pot, const ArcGeometry& g,
                           float norm, uint32_t /*t_ms*/, void* ctx)
{
    const AmountStyle& s = *static_cast<const AmountStyle*>(ctx);
    if (editing[s.row] == (int8_t)pot) { DrawDepth(p, pot, g, cvDepth[pot]); return; }
    const uint8_t last  = (g.arc_leds > 1) ? (uint8_t)(g.arc_leds - 1) : 1u;
    const float   phys  = norm;
    norm = AmtNorm(s.row);   // the value actually playing
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
    DrawPhysPip(p, pot, g, phys, norm);
}

/** Modifier ring: level (or bipolar from noon) in the variant colour. */
static void DrawModRing(LedPanel& p, uint8_t pot, const ArcGeometry& g,
                        float norm, uint32_t t_ms, void* ctx)
{
    const ModStyle& s = *static_cast<const ModStyle*>(ctx);
    if (editing[s.row] == (int8_t)pot) { DrawDepth(p, pot, g, cvDepth[pot]); return; }
    const uint8_t last = (g.arc_leds > 1) ? (uint8_t)(g.arc_leds - 1) : 1u;
    const float   half = 0.5f / (float)last;

    /* Just after the delay subdivision changes, the bottom-right ring shows
     * which of the eight it is: one white LED, longest at the left. */
    if (s.row == 2 && delayDivShownMs != 0 && (t_ms - delayDivShownMs) < 1200u)
    {
        const int at = (int)((float)delayDiv * (float)last / (float)(kNumDelayDivs - 1) + 0.5f);
        for (uint8_t i = 0; i < g.arc_leds; i++)
        {
            /* Faint ticks mark the eight positions. */
            bool tick = false;
            for (uint8_t d = 0; d < kNumDelayDivs; d++)
                if ((int)((float)d * (float)last / (float)(kNumDelayDivs - 1) + 0.5f) == (int)i) tick = true;
            const Rgb c = ((int)i == at) ? kWhite : (tick ? LedPanel::Scale(s.color, 0.25f) : Rgb{0, 0, 0});
            p.SetRingByHour(pot, g.start_hour + (float)i * g.step_hours, p.ScaleGlobal(c));
        }
        return;
    }

    const float phys = norm;
    norm = CvNorm(modValue[s.row], pot);   // effective value (pickup hold + CV)
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
    DrawPhysPip(p, pot, g, phys, norm);
    if (s.clock_pip && clockPeriodSec > 0.0f && (t_ms - clockEdgeMs) < 50u)
        p.SetRingByHour(pot, 12.0f, p.ScaleGlobal(kWhite));
}

/* ═════════════════════════════════════════════════════════════════════════
 *  Knobs (pot indices: 0 1 / 2 3 / 4 5 = left/right per row)
 * ═════════════════════════════════════════════════════════════════════════ */
static VirtualKnob timeAmt  = VirtualKnob(kPotTopLeft, "Time")
    .Ident("time.amount")
    .Help("Speed: CCW slows the tape to a stop, CW speeds it up to 2x (with reel inertia). Age: CCW tape, CW vinyl. Interval: CCW down a 4th / octave / octave+4th / 2 octaves, CW up a 5th / octave / octave+5th / 2 octaves (always the root's fifth or octave). 10-2 o'clock: off.")
    .Ring(Custom(DrawAmountRing, &amountStyle[0]));
static VirtualKnob timeMix  = VirtualKnob(kPotTopRight, "Mix")
    .Ident("time.mix")
    .Help("Speed: dry/wet. Tape: wow and flutter. Vinyl: hiss, crackle and clicks. Interval: dry/wet (noon = dry + shifted, a two-note chord).")
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
/* SD card: the recorder writes takes; HostLink's filesystem block lets the
 * web programmer list / download them over USB. */
static alchemy::SdCard               sdCard;
static hostlink::FsExtension         fsExt(sdCard);
static athanor::Recorder             recorder;

/* Live values and a log in the web programmer's Device console (same USB
 * connection; nothing waits on a client). */
static hostlink::Diagnostics     debug;
static hostlink::Gauge<float>    gCpu("cpu.load", "CPU load");
static hostlink::Gauge<uint32_t> gOverruns("cpu.overruns", "Audio overruns");
static hostlink::Gauge<float>    gBpm("clock.bpm", "Clock tempo");
static hostlink::Gauge<uint32_t> gPpqn("clock.ppqn", "J3 pulses per beat");
static hostlink::Gauge<float>    gSpeed("tape.speed", "Tape speed");
static hostlink::Gauge<bool>     gRec("rec.active", "Recording");
static hostlink::Gauge<uint32_t> gTake("rec.take", "Take number");
static hostlink::Gauge<uint32_t> gDrops("rec.dropouts", "Recording dropouts");
static volatile uint32_t         overrunCount = 0;
static volatile float            reelShown    = 1.0f;

/* Recording feedback on the three button LEDs (see OnRender). */
enum class Flash : uint8_t { None, Started, Problem, Stopped, Dropout };
static volatile Flash    flashKind = Flash::None;
static volatile uint32_t flashMs   = 0;

static constexpr uint16_t kBlock = 64;

static athanor::StretchEngine stretch;
static athanor::Shifter       shifter;      // Interval: time-locked pitch shifter
static athanor::Age           age;
static athanor::Drive         drive;
static athanor::Space         space;
static athanor::ChorusTrem    chorusTrem;
static athanor::PhaserFlanger phaserFlanger;
static athanor::Limiter       limiter;

/* Reverb in internal D2 SRAM (see athanor.lds); delays stay in SDRAM. */
static float __attribute__((section(".d2_bss"), aligned(4))) plateMem[athanor::Plate::kMemFloats];
static float DSY_SDRAM_BSS delayL[athanor::Space::kDelayFloats];
static float DSY_SDRAM_BSS delayR[athanor::Space::kDelayFloats];
static float DSY_SDRAM_BSS ageMem[2 * athanor::Age::kWowFloats];
static float DSY_SDRAM_BSS chorusMem[2 * athanor::ChorusTrem::kFloats];
static float DSY_SDRAM_BSS phaserMem[2 * athanor::PhaserFlanger::kFloats];
static float DSY_SDRAM_BSS shiftMem[athanor::Shifter::kMemFloats];

namespace crash = athanor::crash;

/* Row 1 smoothing / state (audio thread only). */
static athanor::Smooth engageSm, mixSm;
static bool            timeActive = false;
static float           reelPitch  = 1.0f;     // playback speed actually played
static float           reelVel    = 0.0f;     // its rate of change (per second)
static constexpr float kReelHz      = 1.6f;   // reel "spring": ~0.3 s to settle
static constexpr float kReelDamping = 0.5f;   // < 1: overshoots ~16%, tape-like
static constexpr float kMinPitch    = 0.02f;  // never quite 0 (fades still finish)
static constexpr float kMaxPitch    = 4.6f;   // room for +2 oct (4x) plus overshoot
static constexpr float kStepHz      = 7.0f;   // Interval: a quick reel, ~70 ms per step,
static constexpr float kStepDamping = 0.75f;  //   with a hint of tape swoop
static float           reelHz       = kReelHz;
static float           reelDamping  = kReelDamping;
static int             intervalStep = -1;     // current Interval step 0..3 (-1 = none)

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
    int8_t   depth[3];   // v2: CV depth per row's LEFT knob, -100..+100
    uint8_t  pad;
    uint32_t check2;     // v2
    int8_t   depthR[3];  // v3: CV depth per row's RIGHT knob
    uint8_t  delayDiv;   // v4: delay subdivision index (was padding)
    uint32_t check3;     // v3
};

static uint32_t SaveCheck(const SavedState& s)
{
    return s.magic ^ (0x9E3779B9u * (1u + s.v[0] + 7u * s.v[1] + 49u * s.v[2] + 343u * s.version));
}

static uint32_t SaveCheck2(const SavedState& s)
{
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < 3; i++) h = (h ^ (uint8_t)s.depth[i]) * 16777619u;
    return h ^ s.check;
}

static uint32_t SaveCheck3(const SavedState& s)
{
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < 3; i++) h = (h ^ (uint8_t)s.depthR[i]) * 16777619u;
    if (s.version >= 4) h = (h ^ s.delayDiv) * 16777619u;
    return h ^ s.check2 ^ 0xA5A5A5A5u;
}

static inline int8_t DepthToSave(float d)
{
    return (int8_t)(d * 100.0f + (d >= 0.0f ? 0.5f : -0.5f));
}

static void LoadVariants()
{
    dsy_dma_invalidate_cache_for_buffer(reinterpret_cast<uint8_t*>(kSaveAddr), sizeof(SavedState));
    SavedState s;
    std::memcpy(&s, reinterpret_cast<const void*>(kSaveAddr), sizeof(s));
    if (s.magic != kSaveMagic || s.check != SaveCheck(s)) return;
    if (s.version < 1 || s.version > 4) return;
    for (int r = 0; r < 3; r++)
        if (s.v[r] < kNumVariants[r]) variant[r] = s.v[r];
    if (s.version >= 2 && s.check2 == SaveCheck2(s))
    {
        /* v2 depths were the amount (left) knobs. */
        for (int r = 0; r < 3; r++)
            cvDepth[kAmtPot[r]] = Clamp((float)s.depth[r] / 100.0f, -1.0f, 1.0f);
        if (s.version >= 3 && s.check3 == SaveCheck3(s))
        {
            for (int r = 0; r < 3; r++)
                cvDepth[kModPot[r]] = Clamp((float)s.depthR[r] / 100.0f, -1.0f, 1.0f);
            if (s.version >= 4 && s.delayDiv < kNumDelayDivs) delayDiv = s.delayDiv;
        }
    }
}

static void SaveVariants()
{
    SavedState s{};
    s.magic   = kSaveMagic;
    for (int r = 0; r < 3; r++) s.v[r] = variant[r];
    s.version = 4;
    s.check   = SaveCheck(s);
    for (int r = 0; r < 3; r++) s.depth[r] = DepthToSave(cvDepth[kAmtPot[r]]);
    s.check2  = SaveCheck2(s);
    for (int r = 0; r < 3; r++) s.depthR[r] = DepthToSave(cvDepth[kModPot[r]]);
    s.delayDiv = delayDiv;
    s.check3  = SaveCheck3(s);
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
/* J3: 1 or 24 pulses per beat (detected), measured across several pulses and
 * then tempo-locked, so polling jitter never reaches the delay time. */
static athanor::PulseClock j3Clock;
static athanor::TempoLock  j3Lock;
static athanor::TempoLock  midiLock;

/* ═════════════════════════════════════════════════════════════════════════
 *  USB MIDI (shares the USB connection with HostLink)
 * ═════════════════════════════════════════════════════════════════════════
 * Clock: 24 ppqn timing clock becomes the beat for the delay time and the
 * LFO divisions whenever J3 has no clock of its own.
 *
 * Control changes, any channel:
 *   CC 20..25  the six knobs, top-left to bottom-right (64 = noon, so an
 *              amount knob is bypassed around the middle of the CC range)
 *   CC 26..28  variant of row 1 / 2 / 3 (0-42 first, 43-85 second, 86-127 third)
 *   CC 29      record: 64+ start, below 64 stop
 *   CC 30      bypass all: 64+ bypassed, below 64 active
 *   CC 31      delay subdivision: the range split into eight, 1/2 ... 1/16
 * A knob set by CC holds that value until the physical knob is turned to it
 * (the same pickup as after a depth edit), so nothing jumps. */
static constexpr uint8_t kCcKnob0   = 20;
static constexpr uint8_t kCcVariant = 26;
static constexpr uint8_t kCcRecord  = 29;
static constexpr uint8_t kCcBypass  = 30;
static constexpr uint8_t kCcDelayDiv = 31;

static midi::DaisyUsbPort<> midiPort;
static TickTimebase         midiTime;
static athanor::MidiClock   midiClock;
static bool                 midiReady = false;
static uint32_t             midiNowMs = 0;
static volatile uint32_t    midiSeenMs = 0;    // last MIDI message (0 = never)

static void ApplyLooks();

static bool OnMidiEvent(const midi::Event& e)
{
    using midi::EventType;
    midiSeenMs = midiNowMs | 1u;
    switch (e.type)
    {
        case EventType::TimingClock: midiClock.Tick(e.time_us); break;
        case EventType::Start:       midiClock.Start(); break;
        case EventType::Continue:    midiClock.Continue(); break;
        case EventType::Stop:        midiClock.Stop(); break;
        case EventType::ControlChange:
        {
            const uint8_t cc = e.data1;
            const uint8_t v  = (uint8_t)(e.value > 127 ? 127 : e.value);
            if (cc >= kCcKnob0 && cc < kCcKnob0 + 6)
            {
                const uint8_t pot = (uint8_t)(cc - kCcKnob0);
                heldVal[pot]  = athanor::CcToKnob(v);
                catching[pot] = true;
            }
            else if (cc >= kCcVariant && cc < kCcVariant + 3)
            {
                const int r = cc - kCcVariant;
                uint8_t   n = (uint8_t)((v * 3u) / 128u);
                if (n >= kNumVariants[r]) n = (uint8_t)(kNumVariants[r] - 1u);
                if (n != variant[r]) { variant[r] = n; ApplyLooks(); }   // not saved to flash
            }
            else if (cc == kCcRecord)
            {
                if (v >= 64) { if (!recorder.Active()) recorder.Start(midiNowMs); }
                else if (recorder.Recording()) recorder.Stop();
            }
            else if (cc == kCcBypass)
                bypassLatch = (v >= 64);
            else if (cc == kCcDelayDiv)
            {
                const uint8_t n = (uint8_t)((v * kNumDelayDivs) / 128u);
                if (n != delayDiv) { delayDiv = n; delayDivShownMs = midiNowMs | 1u; }   // not saved to flash
            }
            break;
        }
        default: break;
    }
    return true;
}

static void OnPoll(uint32_t t_ms)
{
    if (!healthy && t_ms > 5000u) { healthy = true; crash::Mark(crash::kRunning); }

    const float* phys = loop.Phys();

    if (midiReady)
    {
        midiNowMs = t_ms;
        midiTime.Update(daisy::System::GetTick());
        midiPort.Poll(midiTime.GetSample(), OnMidiEvent);
        midiClock.Poll(midiTime.NowUs());
    }

    /* Buttons. A tap (release without turning anything) steps that row to its
     * next variant. Hold + turn either knob of the row to set that knob's CV
     * depth: the first knob moved becomes the target for this hold. Its own
     * value is held meanwhile, and afterwards stays held until the knob is
     * brought back to it (pickup), so nothing jumps. */
    static VirtualKnob* const kAmtKnob[3] = {&timeAmt, &driveAmt, &spaceAmt};
    for (int b = 0; b < 3; b++)
    {
        Gesture&      gs = gesture[b];
        const uint8_t pa = kAmtPot[b], pm = kModPot[b];
        const float   ka = phys[pa], km = phys[pm];
        if (hw.buttons[b].RisingEdge())
        {
            gs.down      = true;
            gs.target    = -1;
            gs.consumed  = false;
            gs.refAmt    = ka;
            gs.refMod    = km;
            if (!catching[pa]) heldVal[pa] = amtValue[b];
            if (!catching[pm]) heldVal[pm] = modValue[b];
        }
        if (gs.down && gs.target < 0 && !gs.consumed)
        {
            if (std::fabs(km - gs.refMod) > 0.02f)      gs.target = (int8_t)pm;
            else if (std::fabs(ka - gs.refAmt) > 0.02f) gs.target = (int8_t)pa;
            if (gs.target >= 0)
            {
                editing[b] = gs.target;
            }
        }
        if (gs.down && gs.target >= 0)
        {
            float d = (phys[gs.target] - 0.5f) * 2.0f;    // -1..+1, noon = 0
            if (std::fabs(d) < 0.06f) d = 0.0f;           // detent at noon
            else d = (d > 0.0f) ? (d - 0.06f) / 0.94f : (d + 0.06f) / 0.94f;
            cvDepth[gs.target] = Clamp(d, -1.0f, 1.0f);
        }
        if (hw.buttons[b].FallingEdge() && gs.down)
        {
            gs.down = false;
            if (gs.target >= 0)
            {
                editing[b]          = -1;
                catching[gs.target] = true;
                dirtyMs = t_ms ? t_ms : 1u;
            }
            else if (!gs.consumed)
            {
                variant[b] = (uint8_t)((variant[b] + 1u) % kNumVariants[b]);
                ApplyLooks();
                dirtyMs = t_ms ? t_ms : 1u;
            }
        }

        /* Effective values: frozen while assigning / until picked up. */
        const bool holdA = (editing[b] == (int8_t)pa) || catching[pa];
        const bool holdM = (editing[b] == (int8_t)pm) || catching[pm];
        amtValue[b] = holdA ? heldVal[pa] : kAmtKnob[b]->Norm();
        modValue[b] = holdM ? heldVal[pm] : km;
        if (editing[b] != (int8_t)pa && catching[pa] && std::fabs(ka - heldVal[pa]) < 0.025f) catching[pa] = false;
        if (editing[b] != (int8_t)pm && catching[pm] && std::fabs(km - heldVal[pm]) < 0.025f) catching[pm] = false;
    }

    /* Two-button actions fire each time the pair becomes pressed together, so
     * you can keep one button held and tap the other to repeat the action
     * (a button that took part in one never steps its variant on release).
     *
     * B1 + B2 together: bypass all three rows / bring them back. Acts the
     * moment the second button goes down. J7 gate: bypassed while high. */
    {
        Gesture& g1 = gesture[0];
        Gesture& g2 = gesture[1];
        static bool chordHeld = false;
        const bool both = g1.down && g2.down && g1.target < 0 && g2.target < 0 && !gesture[2].down;
        if (both)
        {
            g1.consumed = g2.consumed = true;
            if (!chordHeld) { chordHeld = true; bypassLatch = !bypassLatch; }
        }
        else
            chordHeld = false;   // either button up: the next press together fires again

        const float gv = hw.j7.Volts();
        if (!bypassGate && gv > kClkHiV)      bypassGate = true;
        else if (bypassGate && gv < kClkLoV)  bypassGate = false;
        bypassAll = bypassLatch || bypassGate;
    }

    /* B2 + B3 together: next delay subdivision (wraps). The bottom-right ring
     * shows which one for a moment. */
    {
        Gesture& g2 = gesture[1];
        Gesture& g3 = gesture[2];
        static bool chordHeld = false;
        const bool both = g2.down && g3.down && g2.target < 0 && g3.target < 0 && !gesture[0].down;
        if (both)
        {
            g2.consumed = g3.consumed = true;
            if (!chordHeld)
            {
                chordHeld       = true;
                delayDiv        = (uint8_t)((delayDiv + 1u) % kNumDelayDivs);
                delayDivShownMs = t_ms | 1u;
                dirtyMs         = t_ms ? t_ms : 1u;
                debug.Info("Delay: %s", kDelayDivs[delayDiv].name);
            }
        }
        else
            chordHeld = false;   // either button up: the next press together fires again
        if (delayDivShownMs != 0 && (t_ms - delayDivShownMs) >= 1200u) delayDivShownMs = 0;
    }

    /* B1 + B3 held together for 0.5 s: start / stop SD recording. Neither
     * button steps its variant on release. */
    {
        Gesture& g1 = gesture[0];
        Gesture& g3 = gesture[2];
        static uint32_t chordSinceMs = 0;
        static bool     chordFired   = false;
        const bool both = g1.down && g3.down && g1.target < 0 && g3.target < 0;
        if (both)
        {
            g1.consumed = g3.consumed = true;   // a chord is never a tap
            if (chordSinceMs == 0) chordSinceMs = t_ms ? t_ms : 1u;
            if (!chordFired && (t_ms - chordSinceMs) >= 500u)
            {
                chordFired = true;
                if (recorder.Active()) recorder.Stop();
                else                   recorder.Start(t_ms);
            }
        }
        else   // either button up: pressing together again starts a new hold
        {
            chordSinceMs = 0;
            chordFired   = false;
        }
    }
    recorder.Poll(t_ms);
    Flash fl = Flash::None;
    static uint32_t recDrops = 0;
    const unsigned  take = (unsigned)recorder.TakeNumber();
    switch (recorder.TakeEvent())
    {
        case athanor::Recorder::Event::Started:
            fl = Flash::Started; recDrops = 0;
            debug.Info("Recording REC%04u.WAV", take);
            break;
        case athanor::Recorder::Event::Stopped:
            fl = Flash::Stopped;
            debug.Info("Saved REC%04u.WAV (%u dropouts)", take, (unsigned)recDrops);
            break;
        case athanor::Recorder::Event::Failed:
            fl = Flash::Problem;
            debug.Warn("Recording could not start: no card, not FAT32, or card full");
            break;
        case athanor::Recorder::Event::CardError:
            fl = Flash::Problem;
            debug.Error("SD card error: REC%04u.WAV closed early", take);
            break;
        case athanor::Recorder::Event::Dropout:
            fl = Flash::Dropout; recDrops++;
            debug.Warn("SD card fell behind: audio lost in REC%04u.WAV", take);
            break;
        default: break;
    }
    /* Gauges, ~10 Hz. */
    if ((t_ms % 100u) == 0u)
    {
        gCpu.Set(cpuAvg * 100.0f);
        gOverruns.Set(overrunCount);
        gBpm.Set(clockPeriodSec > 0.0f ? 60.0f / clockPeriodSec : 0.0f);
        gPpqn.Set((uint32_t)j3Clock.Ppqn());
        gSpeed.Set(reelShown);
        gRec.Set(recorder.Recording());
        gTake.Set(recorder.TakeNumber());
        gDrops.Set(recDrops);
    }
    if (fl != Flash::None)
    {
        flashMs   = daisy::System::GetNow() | 1u;
        flashKind = fl;
    }

    /* CV inputs J4 / J5 / J6 -> offsets for both knobs of their row (5 ms
     * smoothing, small dead zone so an unpatched jack's noise can't nudge
     * anything). */
    for (int r = 0; r < 3; r++)
    {
        float v = hw.cv_jacks[kCvJack[r]].Volts();
        v = Clamp(v, -5.5f, 5.5f);
        cvLp[r] += 0.2f * (v - cvLp[r]);
        float vv = cvLp[r];
        if (std::fabs(vv) < 0.04f) vv = 0.0f;
        cvOffset[kAmtPot[r]] = cvDepth[kAmtPot[r]] * vv * 0.1f;
        cvOffset[kModPot[r]] = cvDepth[kModPot[r]] * vv * 0.1f;
    }
    /* Save 1.5 s after the last change (one sector erase, ~50 ms). */
    if (dirtyMs != 0 && (t_ms - dirtyMs) > 1500u)
    {
        dirtyMs = 0;
        SaveVariants();
    }

    const uint32_t now = loop.PollTimeUs();
    const float    v   = hw.j3.Volts();
    bool           j3Edge = false;

    if (!clkHigh && v > kClkHiV)
    {
        clkHigh = true;
        j3Edge  = j3Clock.Edge(now);                 // true on beats only
        /* Lock on the first measurement, then refine once per beat. */
        if (j3Clock.RawPeriod() > 0.0f && (j3Edge || j3Lock.Period() <= 0.0f))
            j3Lock.Update(j3Clock.RawPeriod());
    }
    else if (clkHigh && v < kClkLoV)
        clkHigh = false;

    if (j3Clock.Lost(now))
    {
        j3Clock.Reset();
        j3Lock.Reset();
    }

    /* MIDI clock gets the same tempo lock. Its tempo is held after the
     * transport stops. */
    static uint32_t midiEdgesSeen = 0;
    const uint32_t  me       = midiClock.Edges();
    const bool      midiEdge = (me != midiEdgesSeen);
    midiEdgesSeen = me;
    if (midiClock.PeriodSec() > 0.0f && (midiEdge || midiLock.Period() <= 0.0f))
        midiLock.Update(midiClock.PeriodSec());

    /* Which clock plays: J3 whenever it is running, otherwise MIDI. */
    const bool useJ3 = j3Lock.Period() > 0.0f;
    clockPeriodSec = useJ3 ? j3Lock.Period() : midiLock.Period();
    if (useJ3 ? j3Edge : midiEdge)
    {
        clockEdgeMs = t_ms;
        clockEdges  = clockEdges + 1u;
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 *  Button LEDs: the row's current variant colour (B3 doubles as CPU meter)
 * ═════════════════════════════════════════════════════════════════════════ */
/* Recording status, briefly, on all three buttons:
 *   two short red flashes   recording started
 *   one long red flash      couldn't start (no card / not FAT32 / full) or
 *                           the card failed mid-take (take closed)
 *   one short white flash   stopped, take saved
 *   one amber flash         the card fell behind and a moment was lost
 *   a red blip every 3 s    still recording */
static bool FlashColour(uint32_t now, Rgb& c)
{
    const Flash k = flashKind;
    if (k != Flash::None)
    {
        const uint32_t t = now - flashMs;
        bool on = false; uint32_t len = 0; Rgb col{0xFF, 0x00, 0x00};
        switch (k)
        {
            case Flash::Started: on = (t < 130u) || (t >= 260u && t < 390u); len = 520u; break;
            case Flash::Problem: on = t < 700u; len = 900u; break;
            case Flash::Stopped: on = t < 160u; len = 300u; col = kWhite; break;
            case Flash::Dropout: on = t < 400u; len = 600u; col = Rgb{0xFF, 0x80, 0x00}; break;
            default: break;
        }
        if (t >= len) { flashKind = Flash::None; flashMs = 0; }
        else { c = on ? col : Rgb{0, 0, 0}; return true; }
    }
    if (recorder.Recording() && ((now / 100u) % 30u) == 0u)
    {
        c = Rgb{0xFF, 0x00, 0x00};
        return true;
    }
    return false;
}

static void OnRender(uint32_t /*t_ms*/)
{
    const uint32_t now = daisy::System::GetNow();
    Rgb flash;
    const bool flashing = FlashColour(now, flash);
    for (int b = 0; b < 3; b++)
    {
        Rgb c = LedPanel::Scale(kLooks[b][variant[b]].button, 0.35f);
        if (b == 2)
        {
            if (cpuAvg > 0.8f) c = Rgb{0xFF, 0x80, 0x00};
            if (overrunMs != 0 && (now - overrunMs) < 2000u) c = Rgb{0xFF, 0x00, 0x00};
        }
        if (bypassAll) c = Rgb{0x30, 0x30, 0x30};   // all rows bypassed: buttons go dim white
        if (flashing) c = flash;
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
    /* ── Row 1: tape speed (Capicola pitch) / tape + vinyl / interval ─ */
    int   tSide; float tAmt;
    SplitAmount(AmtNorm(0), tSide, tAmt);
    const bool  reelRow = (v1 == 0) || (v1 == 2);     // variants on the pitch engine
    const int   stSide  = reelRow ? tSide : 0;
    const float mod1    = CvNorm(modValue[0], 1);

    /* Capicola's PITCH control with tape-reel inertia. The knob sets a target
     * playback speed; the speed actually played is a mass-spring-damper
     * chasing it, so moves swoop like a heavy reel spinning up or down and
     * overshoot a little. Timing stays locked to the input (Capicola's
     * stretch grid and transient snapping do that). Releasing the knob back
     * to neutral lets the reel spin back to 1x audibly before the row hands
     * back to the dry signal.
     *   Speed:    continuous. CCW 1x -> stopped, CW 1x -> 2x.
     *   Interval: four steps per side, out to two octaves, on a harmonic
     *             ladder: every step is the input's own fifth or octave, so
     *             both directions add the same two notes (from a C: G and C).
     *             Up: x3/2, x2, x3, x4. Down: x3/4, x1/2, x3/8, x1/4. The modifier
     *             stays MIX, so the dry input plus the shifted copy plays
     *             as a two-note chord. */
    float pitchTarget = 1.0f;
    if (v1 == 2)
    {
        static constexpr float kDown[4] = {0.75f, 0.5f, 0.375f, 0.25f};
        static constexpr float kUp[4]   = {1.5f, 2.0f, 3.0f, 4.0f};
        if (stSide != 0)
        {
            /* Four equal zones per side, with a little hysteresis so a knob
             * (or CV) sitting on a border doesn't chatter between steps. */
            const float z = tAmt * 4.0f;
            int s = intervalStep;
            if (s < 0 || z < (float)s - 0.08f || z > (float)(s + 1) + 0.08f)
                s = (int)z;
            if (s > 3) s = 3;
            intervalStep = s;
            pitchTarget  = (stSide < 0) ? kDown[s] : kUp[s];
        }
        else
            intervalStep = -1;
        reelHz      = kStepHz;
        reelDamping = kStepDamping;
    }
    else
    {
        if (stSide < 0)      pitchTarget = 1.0f - tAmt;          // 1 -> 0 (tape stop)
        else if (stSide > 0) pitchTarget = 1.0f + tAmt;          // 1 -> 2 (octave up)
        if (v1 == 0) { reelHz = kReelHz; reelDamping = kReelDamping; }
        intervalStep = -1;
    }
    {
        const float dt = (float)n / sr;
        const float w  = 2.0f * athanor::kPi * reelHz;
        reelVel += (w * w * (pitchTarget - reelPitch) - 2.0f * reelDamping * w * reelVel) * dt;
        reelPitch += reelVel * dt;
        if (reelPitch < kMinPitch) { reelPitch = kMinPitch; if (reelVel < 0.0f) reelVel = 0.0f; }
        if (reelPitch > kMaxPitch) { reelPitch = kMaxPitch; if (reelVel > 0.0f) reelVel = 0.0f; }
    }
    /* Speed plays the reel through Capicola's engine (tape feel: timing
     * drifts and snaps back on transients). Interval plays it through the
     * delay-line shifter instead, which has no time grid to drift: the
     * shifted copy stays within ~50 ms of the input however long it's held,
     * so dry + shifted land together as a chord. */
    const bool interval = (v1 == 2);
    static bool wasInterval = false;
    if (interval && !wasInterval) shifter.Reset();   // fresh history on entering the variant
    wasInterval = interval;
    stretch.SetPitch(interval ? 1.0f : reelPitch);
    shifter.SetRatio(reelPitch);
    reelShown = reelPitch;

    const bool spinning = std::fabs(reelPitch - 1.0f) > 0.004f || std::fabs(reelVel) > 0.02f;
    const bool active   = reelRow && (stSide != 0 || spinning);
    if (active && !timeActive && !interval) stretch.Resync();     // engage from "now"
    timeActive = active;

    /* Near a full stop the tape runs out of level, like a real tape stop. */
    const float stopGain = interval ? 1.0f : Clamp(reelPitch / 0.12f, 0.0f, 1.0f);
    const float engage   = engageSm.Tick(active ? 1.0f : 0.0f);
    const float mixT     = (v1 == 0 || v1 == 2) ? mod1 : mixSm.v;   // Speed / Interval: mix
    const float wet      = engage * mixSm.Tick(mixT);

    static float wl[kBlock], wr[kBlock];
    float*       bl = out[0];
    float*       br = out[1];
    stretch.Process(in[0], in[1], wl, wr, n);   // always runs: keeps its history live
    if (interval) shifter.Process(in[0], in[1], wl, wr, n);
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
    SplitAmount(AmtNorm(1), dSide, dAmt);
    drive.SetParams(v2, dSide, dAmt, CvNorm(modValue[1], 3));
    drive.Process(bl, br, n);

    crash::Mark(crash::kAudioMod);
    /* ── Row 3: space / chorus + trem / phaser + flanger ─────────────── */
    int   sSide; float sAmt;
    SplitAmount(AmtNorm(2), sSide, sAmt);
    const float mod3 = CvNorm(modValue[2], 5);

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
    {
        /* Tempo changes glide like tape; a new subdivision crossfades in. */
        static uint8_t lastDiv = 0xFF;
        const uint8_t  div     = delayDiv < kNumDelayDivs ? delayDiv : kDefaultDelayDiv;
        space.SetDelaySeconds(beatSec * kDelayDivs[div].beats, lastDiv != 0xFF && div != lastDiv);
        lastDiv = div;
    }
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
        limiter.Process(bl[i], br[i]);
    }
    recorder.Push(bl, br, n);   // final output -> SD ring (no-op when idle)

    crash::Mark(healthy ? (uint32_t)crash::kRunning : (uint32_t)crash::kFirstTick);
    cpu.OnBlockEnd();
    cpuAvg = cpu.GetAvgCpuLoad();
    const float    load = (float)(daisy::System::GetTick() - blockStart) * ticksPerBlockInv;
    const uint32_t ms   = daisy::System::GetNow();
    if (load > 0.98f && ms > 3000u) { overrunMs = ms ? ms : 1u; overrunCount = overrunCount + 1u; }
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
    modStyle[0] = {{0, 0, 0}, false, false, 0};
    modStyle[1] = {{0, 0, 0}, true, false, 1};    // tone: bipolar from noon
    modStyle[2] = {{0, 0, 0}, false, true, 2};    // blinks on J3 clock
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
    shifter.Init(sr, shiftMem);
    crash::Mark(crash::kPhaserInit);

    /* Per-block smoothers (64-sample blocks ~ 1.33 ms). */
    engageSm.Init(0.0f, 0.08f);     // ~17 ms
    mixSm.Init(1.0f, 0.08f);

    limiter.Init(sr);
    sdCard.Init();                      // owns volume "0:" (lazy, no disk I/O)
    recorder.Init(&sdCard, (uint32_t)(sr + 0.5f));
    host.Extend(fsExt);
    gCpu.Unit("%");
    gBpm.Unit("BPM");
    gSpeed.Unit("x");
    debug.Watch(gCpu);   debug.Watch(gOverruns); debug.Watch(gBpm); debug.Watch(gPpqn); debug.Watch(gSpeed);
    debug.Watch(gRec);   debug.Watch(gTake);     debug.Watch(gDrops);
    host.Extend(debug);
    /* USB MIDI on the same composite device as HostLink. If it can't start,
     * everything else carries on without it. */
    midiClock.Reset();
    j3Clock.Reset();
    if (daisy::System::GetTickFreq() != 0u)
    {
        midiTime.Init(daisy::System::GetTickFreq(), daisy::System::GetTick());
        /* Name the shared USB device before either service starts it. */
        usb::Device::Shared().Configure(daisy::UsbHandle::FS_EXTERNAL, "Athanor");
        midiReady = midiPort.Init({}, {0, 1});
    }
    debug.Info("Athanor " ATHANOR_VERSION " (" ATHANOR_GIT_HASH "), Alchemy SDK " ALCHEMY_SDK_VERSION);
    if (!midiReady) debug.Warn("USB MIDI did not start");
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
