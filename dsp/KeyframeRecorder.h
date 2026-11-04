#pragma once

// Derived from Capicola (c) heavylight-industries, AGPL-3.0.
// Modified for Athanor: speed-up (stretch > 1) support and onset logging for
// transient-aware splicing — see 'Athanor' notes.

#include <Analyzer.h>    // keyframe writer (raw ring + sparsifier)
#include <Granule.h>     // self-contained grain reader (own fences, self-healing)
#include <Detector.h>    // transient finder (TKEO + SVF + peak keyframer)
#include <Shapers.h>     // tabulated waveshapers (optional; Init(nullptr) = clean)
#include <cstddef>
#include <cstdint>

namespace capicola {

// Transport state of the recorder.
enum class State : uint8_t {
    STOPPED,
    RECORDING,
    PLAYBACK,
    LIVE_EFFECT,
};

// Change requests into the state machine.
enum class Request : uint8_t {
    NONE,
    ARM,
    RECORD,
    PLAY,
    RESTART_PLAYBACK,
    CLEAR,
    STOP_REC,
    STOP_PLAYING,
    LIVE_EFFECT,
    SLICE,
};

template <int bufsz = 1024>
class KeyframeRecorder {
private:
    SparseLine<bufsz> sparse;
    Analyzer<bufsz>   analyzer;

    // Two grains trading places over one gain ramp. A trigger always punches
    // through: the idle grain hard-seats at the delayed tap (Retrigger) and fades
    // in over curFade while the old grain finishes its in-flight splice at falling
    // gain. No internal fade is ever interrupted, so a punch can never click.
    Granule<bufsz> granule[2];
    int   liveG;     // index of the grain fading in / steady
    float xGain;     // gain of granule[liveG]; the other grain gets 1-xGain
    float xStep;     // per-sample ramp, 1/curFade

    Detector detector;   // transient finder over the recorded input

    // Athanor: cheap onset detector feeding the grains' transient-aware splicing.
    TransientRing onsets;
    float envFast, envSlow;
    int   onsetHold;
    inline bool Onset(float x) {
        const float a = (x < 0.0f) ? -x : x;
        envFast += ((a > envFast) ? 0.2f : 0.003f) * (a - envFast);   // ~0.1 ms / ~7 ms
        envSlow += 0.0004f * (a - envSlow);                            // ~50 ms
        if (onsetHold > 0) { onsetHold--; return false; }
        if (envFast > 2.5f * envSlow + 0.01f) { onsetHold = 2400; return true; }   // 50 ms hold-off
        return false;
    }
    bool guards = true;   // Athanor: transient guard + aligned splices (off = stock Capicola)
    void AttachOnsets() {
        for (auto& g : granule) {
            g.SetTransients(guards ? &onsets : nullptr);
            g.SetAlign(guards);
        }
    }
    uint32_t transientCount;              // monotonic; the UI polls it for edges

    State   currentState;
    Request currentRequest;

    // Current control snapshot, re-applied whenever the reader is re-Init'd.
    float curPitch;
    float curStretch;
    int   curLeash;
    float curThreshold;
    float curFade;
    double speedLag;      // Athanor: samples behind the tap a speed-up grain starts
    bool   autoSlice;     // Athanor: false = transient detector off (no snapping)
    float  loopFade;      // Athanor: crossfade for speed-up loop punches (samples)
    float  punchFade;     // Athanor: one-shot fade override for the next Punch (0 = curFade)

    bool  pendingSeed;    // spawn a fresh voice on the next render block
    bool  firedThisBlock; // detector auto-fired this block (stereo-guard edge; NOT forced slices)
    int   lastBlockSize;

    // LIVE trigger path: a kept detector event punches the block it lands in,
    // through a non-retriggerable refractory (trigGate). The refractory length is
    // curFade — a punch physically lasts one crossfade, so that's the retrigger
    // floor; dense kept bursts retrigger every fade instead of latching. B1
    // (SLICE) bypasses this.
    float trigGate;   // refractory countdown; doubles as the B1 LED state

    // Waveshaper. Applied to KEYFRAMES on the way out, never to the interpolated
    // stream — the nonlinearity never sees the sample rate, so it cannot alias.
    const Shapers* shapers;   // null = clean path, no shaping at all
    float drive;
    float character;          // 0 = quake, 0.5 = bypass, 1 = sinc

    static constexpr float kBypassEps = 1e-4f;

    inline float Shape(float x) const {
        if (!shapers) return x;
        const float d = character - 0.5f;
        if (d > -kBypassEps && d < kBypassEps) return x;
        const float xd = x * drive;
        if (d < 0.0f) {
            const float t = character * 2.0f;
            return (1.0f - t) * shapers->ReadQuake(xd) + t * x;
        }
        const float t = d * 2.0f;
        return (1.0f - t) * x + t * shapers->ReadSinc(xd);
    }

    // Athanor: above realtime only the LIVE grain may run fast; the retiring
    // grain is held at realtime so it cannot race into the live edge while it
    // fades out.
    float RetireStretch() const { return (curStretch > 1.0f) ? 1.0f : curStretch; }

    void ApplyParams() {
        for (int i = 0; i < 2; i++) {
            Granule<bufsz>& g = granule[i];
            g.SetPitch(curPitch);
            g.SetStretch(i == liveG ? curStretch : RetireStretch());
            g.SetLeash(curLeash);
            g.SetFade(curFade);
        }
        analyzer.SetThreshold(curThreshold);
    }

    // Bind fresh readers over the (already Init'd) buffer and re-apply controls.
    void ResetReader() {
        granule[0].Init(sparse, 0.0);
        granule[1].Init(sparse, 0.0);
        liveG = 0;
        xGain = 1.0f;
        xStep = 0.0f;
        ApplyParams();
        AttachOnsets();
    }

    // Unconditional punch-through (button / transient). Swaps to the idle grain,
    // seats it at the tap, and starts the gain ramp. If a punch lands mid-ramp
    // the half-faded grain is reused and its gain jumps — rare given the fire-side
    // hold (>=50 ms) and curFade-length ramps.
    void Punch() {
        liveG ^= 1;
        // Athanor: stretch > 1 = speed-up. Seat the new grain behind the tap so
        // it can fast-forward through recent history; the outgoing grain drops
        // to realtime for its fade-out.
        granule[liveG].SetStretch(curStretch);
        granule[liveG ^ 1].SetStretch(RetireStretch());
        if (curStretch > 1.0f) granule[liveG].RetriggerBehind(speedLag);
        else                   granule[liveG].Retrigger();
        granule[liveG].SetPitch(curPitch);
        xGain = 0.0f;
        const float f = (punchFade > 0.0f) ? punchFade : curFade;
        punchFade = 0.0f;
        xStep = 1.0f / ((f < 1.0f) ? 1.0f : f);
    }

    // Direction change. Reversing a moving head in place is a corner in the
    // waveform, so the idle grain takes a copy of the live one — same cursors,
    // same in-flight splice — and plays on in the new direction while the old
    // one fades out in the old. Same ramp as a punch, and the same refractory,
    // so a transient can't punch into it.
    void Flip() {
        granule[liveG ^ 1] = granule[liveG];
        liveG ^= 1;
        xGain    = 0.0f;
        xStep    = 1.0f / ((curFade < 1.0f) ? 1.0f : curFade);
        trigGate = curFade;
    }

    // Once per block, after any punch: only the live grain follows the knob;
    // the outgoing one finishes its ramp at the rate it had. A sign change
    // trades grains, and waits for a running ramp to land rather than reuse a
    // grain that is still audible.
    void TrackPitch() {
        if ((curPitch < 0.0f) != (granule[liveG].GetPitch() < 0.0f)) {
            if (xGain < 1.0f) return;
            Flip();
        }
        granule[liveG].SetPitch(curPitch);
    }

    // Mixed read of the trading pair; the retired grain is skipped once the ramp
    // completes.
    inline float ReadPair() {
        auto shape = [this](float v) { return Shape(v); };
        if (xGain >= 1.0f) return granule[liveG].Read(shape);
        const float in  = granule[liveG].Read(shape);
        const float out = granule[liveG ^ 1].Read(shape);
        const float w   = SmoothStep(xGain);
        const float y   = in * w + out * (1.0f - w);
        xGain += xStep;
        if (xGain > 1.0f) xGain = 1.0f;
        return y;
    }

public:
    // Trivially constructible on purpose: instances are DSY_SDRAM_BSS globals, so
    // any ctor code would run during static init — before main() configures the
    // SDRAM controller — and bus-fault on the first write. Everything initializes
    // in Init(), called from AudioEngine::Init() after hw.Init().

    void Init(const Shapers* sh = nullptr) {
        shapers   = sh;
        drive     = 1.0f;
        character = 0.5f;   // bypass
        sparse.Init();
        analyzer.Init(sparse);
        granule[0].Init(sparse, 0.0);
        granule[1].Init(sparse, 0.0);
        liveG = 0;
        xGain = 1.0f;
        xStep = 0.0f;
        detector.Init(48000.0f);
        transientCount = 0;
        onsets.Clear();
        envFast = envSlow = 0.0f;
        onsetHold = 0;
        AttachOnsets();

        curPitch     = 1.0f;
        curStretch   = 1.0f;
        curLeash     = 128;
        curThreshold = 0.001f;
        curFade      = 2400.0f;   // 50 ms @ 48 kHz
        speedLag     = 24000.0;   // 0.5 s @ 48 kHz
        autoSlice    = true;
        loopFade     = 4800.0f;   // 100 ms
        punchFade    = 0.0f;
        ApplyParams();

        currentState   = State::STOPPED;
        currentRequest = Request::NONE;
        pendingSeed    = false;
        firedThisBlock = false;
        lastBlockSize  = 0;
        trigGate       = 0.0f;
    }

    void SetGrainPitch(float s)   { curPitch = s; }   // signed; applied per block (TrackPitch)
    void SetGrainStretch(float t) {
        curStretch = t;
        granule[liveG].SetStretch(t);
        granule[liveG ^ 1].SetStretch(RetireStretch());
    }
    // Athanor: how far behind the tap a speed-up grain starts (samples).
    void SetSpeedLag(double samples) { speedLag = (samples < 0.0) ? 0.0 : samples; }
    // Athanor: enable/disable transient snapping (skips the detector entirely).
    void SetAutoSlice(bool on) { autoSlice = on; }
    // Athanor: crossfade length used when a speed-up pass loops back.
    void SetLoopFade(float samples) { loopFade = (samples < 1.0f) ? 1.0f : samples; }
    // Athanor: enable/disable the transient guard and aligned splices.
    // Off reproduces Capicola's own reader exactly.
    void SetGuards(bool on) { guards = on; AttachOnsets(); }
    void SetGrainLeash(int k)     { curLeash = k; for (auto& g : granule) g.SetLeash(k); }
    void SetThreshold(float v)    { curThreshold = v; analyzer.SetThreshold(v); }
    void SetGrainFade(float f)    { curFade = f; for (auto& g : granule) g.SetFade(f); }

    void SetTransientThreshold(float t) { detector.SetThreshold(t); }
    void SetTkeoCutoff(float fc)        { detector.SetCutoff(fc); }
    void SetTkeoResonance(float r)      { detector.SetResonance(r); }
    void SetDistortDrive(float d)       { drive = d; }
    void SetDistortCharacter(float c)   { character = c; }

    State GetState() { return currentState; }

    uint32_t TransientCount() const { return transientCount; }
    float    PairGain()       const { return xGain; }             // takeover ramp state
    float    TkeoEnvelope()   const { return detector.Envelope(); }

    // Stereo re-alignment guardrail (see AudioEngine::ProcessBlock): did the
    // detector auto-fire this block (forced slices excluded), and how far behind
    // the writer the live grid sits right now.
    bool     FiredThisBlock() const { return firedThisBlock; }
    double   GridLag()        const { return granule[liveG].GridLag(); }

    void SubmitRequest(Request r) { currentRequest = r; }

    void HandleRequest() {
        switch (currentRequest) {
            case Request::RECORD:
                sparse.Init();
                analyzer.Init(sparse);
                ResetReader();
                currentState = State::RECORDING;
                break;

            case Request::LIVE_EFFECT:
                sparse.Init();
                analyzer.Init(sparse);
                onsets.Clear();
                ResetReader();
                pendingSeed  = true;
                currentState = State::LIVE_EFFECT;
                break;

            case Request::SLICE:
                if (!analyzer.IsEmpty()) pendingSeed = true;
                break;

            case Request::STOP_REC:
                analyzer.ArmFinalFrame();
                ResetReader();
                currentState = State::STOPPED;
                break;

            case Request::PLAY:
            case Request::RESTART_PLAYBACK:
                ResetReader();
                pendingSeed  = true;
                currentState = State::PLAYBACK;
                break;

            case Request::STOP_PLAYING:
                currentState = State::STOPPED;
                break;

            case Request::CLEAR:
                sparse.Init();
                analyzer.Init(sparse);
                ResetReader();
                currentState = State::STOPPED;
                break;

            default:
                break;
        }
        currentRequest = Request::NONE;
    }

    inline void ProcessBlock(const float* in, float* out, size_t size) {
        lastBlockSize  = (int)size;
        firedThisBlock = false;
        if (currentRequest != Request::NONE) HandleRequest();

        switch (currentState) {
            case State::STOPPED: {
                for (size_t i = 0; i < size; i++) out[i] = in[i];
                break;
            }

            case State::RECORDING: {
                for (size_t i = 0; i < size; i++) {
                    if (analyzer.IsFull()) { SubmitRequest(Request::STOP_REC); out[i] = in[i]; continue; }
                    analyzer.Analyze(in[i]);
                    out[i] = in[i];
                }
                break;
            }

            case State::PLAYBACK: {
                granule[0].BeginBlock((int)size);
                granule[1].BeginBlock((int)size);
                if (pendingSeed) { Punch(); pendingSeed = false; }
                TrackPitch();
                for (size_t i = 0; i < size; i++) out[i] = ReadPair();
                break;
            }

            case State::LIVE_EFFECT: {
                // Capture the whole block first so keyframes exist ahead of the
                // reader, then render behind them. A kept detector event fires
                // live: with delay pinned at 0 a punch seats its grain one fade
                // behind the transient, so the fade-in completes exactly as the
                // transient reaches the playhead — full gain at the hit.
                bool fire = false;
                for (size_t i = 0; i < size; i++) {
                    analyzer.Analyze(in[i]);
                    if (autoSlice && detector.Analyze(in[i])) fire = true;
                    // Athanor: log onsets ~1 ms before detection (pre-roll).
                    if (Onset(in[i])) onsets.Push((double)analyzer.WriteHead() - 49.0);
                }

                // Refractory: expire first, then punch only from an expired state,
                // never re-arm on top of a running hold (else dense bursts latch).
                if (trigGate > 0.0f) trigGate -= (float)size;
                if (fire && trigGate <= 0.0f) {
                    transientCount++;
                    pendingSeed    = true;
                    firedThisBlock = true;   // stereo-guard edge (natural fire only)
                    trigGate       = curFade;
                }

                granule[0].BeginBlock((int)size);
                granule[1].BeginBlock((int)size);
                // Athanor: speed-up loop. A grid running faster than realtime
                // closes on the delayed tap; before it gets there, punch a fresh
                // grain back behind the tap (a normal crossfaded punch). Only
                // from a settled pair, so a running ramp is never reused.
                if (curStretch > 1.0f && !pendingSeed && xGain >= 1.0f &&
                    granule[liveG].GridAheadOfTap() >= -(double)curStretch * (double)size)
                {
                    pendingSeed = true;
                    punchFade   = loopFade;   // long, smooth loop-back
                }
                if (pendingSeed) { Punch(); pendingSeed = false; }
                TrackPitch();
                for (size_t i = 0; i < size; i++) out[i] = ReadPair();

                // Gap-gated boundary keyframe. Safe because every reader fence
                // derives from the newest keyframe's time, and read heads sit
                // >= curFade behind it — so bounding coverage staleness by `gap`
                // keeps every legal read >= (curFade - gap) clear of the newest
                // frame. Half the fade is a comfortable gap; clamp so it degrades
                // to per-block cadence for degenerate tiny fades.
                float gap = 0.5f * curFade;
                const float cap = curFade - 2.0f * (float)size;
                if (gap > cap) gap = cap;
                if (gap < 1.0f) gap = 1.0f;
                analyzer.GuardKeyframe((double)gap);
                break;
            }
        }
    }
};

} // namespace capicola
