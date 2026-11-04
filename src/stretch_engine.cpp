/**
 * @file stretch_engine.cpp
 * @brief See stretch_engine.h. Derived from Capicola (AGPL-3.0).
 */
#include "stretch_engine.h"

#include "daisy_seed.h"
#include "KeyframeRecorder.h"

namespace athanor {

using namespace capicola;

/* Two keyframe rings in external SDRAM, exactly as Capicola sizes them:
 * 2^21 frames × 12 B = 24 MB per channel. Globals because DSY_SDRAM_BSS
 * cannot decorate a class member. */
static constexpr int kRingFrames = 2097152;
static KeyframeRecorder<kRingFrames> DSY_SDRAM_BSS rec_l;
static KeyframeRecorder<kRingFrames> DSY_SDRAM_BSS rec_r;

/* Capicola's tabulated keyframe waveshapers (3 x 1024 floats). */
static Shapers shapers;

namespace {
/* Capicola factory voicing (perf page defaults). */
constexpr float kFadeSamples   = 960.0f;   // 20 ms OLA fade = latency = retrigger floor
constexpr int   kGrainLeash    = 642;      // grain size, keyframes (Capicola default)
constexpr float kQualityEps    = 0.001f;   // analyzer ε — max fidelity
constexpr float kTkeoFc        = 0.0014f;  // follower smoothing
constexpr float kThreshRatio   = 2.22f;    // transient keep ratio (Capicola noon)
constexpr double kMaxDrift     = 48000.0;  // stereo re-alignment guardrail (1 s)
constexpr float kLoopFade      = 4800.0f;  // 100 ms crossfade when speed-up loops back
constexpr float kStretch       = 0.572f;   // Capicola perf default (knob 0.9 -> 0.8^2.5)
constexpr float kShaperDrive   = 1.0f;     // Capicola secondary defaults: drive 1.0,
constexpr float kShaperChar    = 1.0f;     //   character 1.0 = full sinc
constexpr float kShaperTrim    = 0.5f;     // the sinc shaper's 2x small-signal gain, undone
} // namespace

void StretchEngine::Init(float /*sample_rate*/)
{
    shapers.Init();
    for (auto* r : {&rec_l, &rec_r})
    {
        r->Init(&shapers);
        r->SetDistortDrive(kShaperDrive);
        r->SetDistortCharacter(kShaperChar);
        r->SetThreshold(kQualityEps);
        r->SetGrainPitch(1.0f);
        r->SetGrainStretch(kStretch);
        r->SetGrainLeash(kGrainLeash);
        r->SetGrainFade(kFadeSamples);
        r->SetTkeoCutoff(kTkeoFc);
        r->SetTransientThreshold(kThreshRatio);
        r->SetAutoSlice(true);          // Capicola transient snapping
        r->SetGuards(false);            // stock Capicola reader
        r->SetLoopFade(kLoopFade);
        r->SubmitRequest(Request::LIVE_EFFECT);
    }
    slice_count_ = 0;

    /* Run one silent block now, on the main thread, so the LIVE_EFFECT request
     * (which clears both 24 MB keyframe rings) never lands in the audio
     * callback. */
    float z[kMaxBlock] = {0.0f}, o[kMaxBlock];
    rec_l.ProcessBlock(z, o, kMaxBlock);
    rec_r.ProcessBlock(z, o, kMaxBlock);
}

void StretchEngine::Process(const float* in_l, const float* in_r,
                            float* out_l, float* out_r, std::size_t n)
{
    if (n > kMaxBlock) n = kMaxBlock;
    rec_l.ProcessBlock(in_l, out_l, n);
    rec_r.ProcessBlock(in_r, out_r, n);
    for (std::size_t i = 0; i < n; i++) { out_l[i] *= kShaperTrim; out_r[i] *= kShaperTrim; }

    /* Capicola's stereo guardrail: when exactly one side auto-fired and the
     * grids have drifted apart, force the quiet side to re-anchor. */
    const bool fl = rec_l.FiredThisBlock();
    const bool fr = rec_r.FiredThisBlock();
    if (fl != fr)
    {
        const double a = rec_l.GridLag(), b = rec_r.GridLag();
        if ((a > b ? a - b : b - a) > kMaxDrift)
            (fl ? rec_r : rec_l).SubmitRequest(Request::SLICE);
    }
    if (fl || fr) slice_count_++;
}

void StretchEngine::SetStretch(float s)
{
    if (s < 0.0f) s = 0.0f;
    rec_l.SetGrainStretch(s);
    rec_r.SetGrainStretch(s);
}

void StretchEngine::SetSpeedLagSamples(double lag)
{
    rec_l.SetSpeedLag(lag);
    rec_r.SetSpeedLag(lag);
}

void StretchEngine::SetGrainLeash(int k)
{
    rec_l.SetGrainLeash(k);
    rec_r.SetGrainLeash(k);
}

void StretchEngine::SetPitch(float speed)
{
    if (speed < 0.0f) speed = 0.0f;
    rec_l.SetGrainPitch(speed);
    rec_r.SetGrainPitch(speed);
}

void StretchEngine::Resync()
{
    if (rec_l.GetState() == State::LIVE_EFFECT)
    {
        rec_l.SubmitRequest(Request::SLICE);
        rec_r.SubmitRequest(Request::SLICE);
    }
}

} // namespace athanor
