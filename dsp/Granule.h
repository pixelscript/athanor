#pragma once
#ifndef ATH_DBG
#define ATH_DBG(...)
#endif

// Derived from Capicola (c) heavylight-industries, AGPL-3.0.
// Modified for Athanor: RetriggerBehind(), GridAheadOfTap(), transient-aware
// splicing and waveform-aligned splice targets.

#include <SparseLine.h>

namespace capicola {

// Athanor: recent input onsets, in the same uniform (raw-sample) time as the
// keyframes. Written by the recorder, read by its grains.
struct TransientRing {
    static constexpr int kSize = 64;   // power of two
    double   t[kSize];
    uint32_t count;
    void Clear() { count = 0; }
    void Push(double x) { t[count & (kSize - 1)] = x; count++; }
};

// A reading cursor over the sparse buffer: the window it sits in, its uniform
// (sample-time) index, sub-window phase `tau` in [0,1], advance rate, and last
// value read. The `ideal` cursor (the stretch grid) never populates `result` —
// it only marks metric position; `actual`/`temp` do the reading.
struct Playhead {
    Window w;
    double index;    // uniform position, in samples
    float  tau;      // phase within [p2,p1]; 0 at p1, 1 at p2
    float  speed;    // advance per sample (stretch for `ideal`, pitch for the rest)
    float  result;   // last sampled value (unused for `ideal`)
};

template <int bufsz = 1024>
class Granule {
private:
    SparseLine<bufsz>* sparse_;

    Playhead ideal, actual, temp;

    // Athanor: transient-aware splicing (see Read()).
    const TransientRing* tr_;
    uint32_t trSeen_;
    double   nextT_;     // first onset after the read head (1e300 = none)
    double   lastT_;     // last onset at/behind the read head (-1e300 = none)
    static constexpr double kMaxHold = 12000.0;   // max head lead while holding splices

    void RefreshTransients() {
        nextT_ = 1e300;
        lastT_ = -1e300;
        if (!tr_) return;
        trSeen_ = tr_->count;
        const uint32_t n = (tr_->count < (uint32_t)TransientRing::kSize)
                         ? tr_->count : (uint32_t)TransientRing::kSize;
        for (uint32_t i = 0; i < n; i++) {
            const double t = tr_->t[(tr_->count - 1 - i) & (TransientRing::kSize - 1)];
            if (t > actual.index) { if (t < nextT_) nextT_ = t; }
            else if (t > lastT_)  lastT_ = t;
        }
    }

    // ── Athanor: waveform-aligned splice targets ─────────────────────────────
    // Splicing onto the grid at an arbitrary phase makes the crossfade mix two
    // unrelated waveforms (comb filtering / roughness). Like WSOLA, search
    // +-kAlignRange around the target for the point whose value and slope best
    // match where the outgoing head is about to be, and splice there instead.
    static constexpr int   kAlignRange = 192;   // samples (+-4 ms)
    static constexpr int   kAlignStep  = 2;
    bool alignOn_;

    inline float Probe(Playhead& ph, double idx) {
        if (idx > ph.w.endTime || idx < ph.w.p2->time) sparse_->StepWindow(ph.w, idx);
        ph.index = idx;
        return Sample(ph);
    }

    Playhead Aligned(const Window& w0, double pos) {
        if (!alignOn_) return Head(w0, pos, pitch);
        // Where the outgoing head will be one fade-length on is unknowable
        // cheaply; match its *current* value and slope instead (the incoming
        // grain fades in from here).
        Playhead a = actual;
        const float va = Probe(a, actual.index);
        const float da = Probe(a, actual.index + 2.0) - va;
        double lo = pos - (double)kAlignRange;
        double hi = pos + (double)kAlignRange;
        if (hi > liveEdge.uniform - 4.0) hi = liveEdge.uniform - 4.0;
        const double oldest = sparse_->GetSample(sparse_->GetOldestIndex() + 2)->time;
        if (lo < oldest) lo = oldest;
        if (hi <= lo) return Head(w0, pos, pitch);
        Playhead c = Head(w0, pos, pitch);
        double best = pos;
        float  bestCost = 1e30f;
        for (double p = lo; p <= hi; p += (double)kAlignStep) {
            const float v  = Probe(c, p);
            const float dv = Probe(c, p + 2.0) - v;
            const float e1 = v - va, e2 = (dv - da) * 16.0f;
            // mild preference for staying near the requested position
            const float dist = (float)((p - pos) / (double)kAlignRange);
            const float cost = e1 * e1 + e2 * e2 + 1e-4f * dist * dist;
            if (cost < bestCost) { bestCost = cost; best = p; }
        }
        Playhead out = Head(w0, pos, pitch);
        if (best > out.w.endTime || best < out.w.p2->time) sparse_->StepWindow(out.w, best);
        out.index = best;
        return out;
    }

    // Non-preemptible splice to an arbitrary position with its own fade length
    // (transient guards). dur <= 0 = the fixed fade.
    float guardInc_;
    void ReanchorTo(double pos, float dur = 0.0f, bool align = false) {
        Window w = sparse_->WindowAt(pos);
        temp        = align ? Aligned(w, pos) : Head(w, pos, pitch);
        splicePhase = 0.0f;
        splicing    = true;
        adaptive    = false;
        guardInc_   = (dur > 0.0f) ? 1.0f / (dur < 48.0f ? 48.0f : dur) : 0.0f;
    }

    // Critical positions, kept live as convenient tuples.
    Position writePos;   // the write head              (fence, from SparseLine)
    Position liveEdge;   // furthest safe read          (fence, from SparseLine)
    Position master;     // writePos - fade, clamped    (the delayed tap; fixed-fade target)
    Window   masterW;    // window bracketing `master`, cached per block for re-anchors

    // Live distances, refreshed per sample for triggers + introspection.
    Distance distance;   // actual - ideal(grid)  : the head's lead over the stretch grid
    Distance runway;     // liveEdge - actual     : room ahead of the head
    Distance backlog;    // writePos - ideal(grid): how close the writer is to lapping the grid

    // Controls.
    float pitch;         // rate of `actual` (playback speed / transpose)
    float stretch;       // rate of `ideal` (the stretch grid)
    int   leash;         // adaptive grain size, in keyframes

    // Crossfade state. fixed* is a control (SetFade); adaptive* is computed
    // per-splice. Only one fade runs at a time; `adaptive` selects the increment.
    float fixedDuration;
    float fixedIncrement;
    float adaptiveDuration;
    float adaptiveIncrement;
    float splicePhase;
    bool  splicing;
    bool  adaptive;

    int   blockSize;

    // Re-anchor when the writer has lapped `ideal` to within this many keyframes
    // of capacity. Coarse back-stop; the front guard is the exact one.
    static constexpr int kSafetyKeyframes = bufsz - (bufsz >> 3);  // ~87.5% full

    // Package a cursor from a window that already brackets `pos`.
    static Playhead Head(const Window& w, double pos, float rate) {
        return { w, pos, 0.0f, rate, 0.0f };
    }

    // Sample a cursor at its current index (does not advance it).
    float Sample(Playhead& ph) {
        ph.tau    = (float)(ph.w.endTime - ph.index) * ph.w.invDuration;
        ph.result = sparse_->Read(ph.w, ph.tau, InterpMode::CATMULLROM);
        return ph.result;
    }

    template <typename ShapeFn>
    float Sample(Playhead& ph, ShapeFn fn) {
        ph.tau    = (float)(ph.w.endTime - ph.index) * ph.w.invDuration;
        ph.result = sparse_->Read(ph.w, ph.tau, InterpMode::CATMULLROM, fn);
        return ph.result;
    }

public:
    // Implicit trivial constructor on purpose (SDRAM-resident; see Init()).

    void Init(SparseLine<bufsz>& sparse, double startPos) {
        sparse_ = &sparse;
        tr_     = nullptr;
        trSeen_ = 0;
        guardInc_ = 0.0f;
        alignOn_  = true;
        pitch   = 1.0f;
        stretch = 1.0f;
        leash   = 128;
        SetFade(2400.0f);   // 50 ms @ 48 kHz
        adaptive  = false;
        blockSize = 0;
        writePos = sparse_->GetWritePosition();
        liveEdge = writePos;
        masterW  = sparse_->WindowAtLatest();
        Spawn(startPos);
    }

    void SetPitch(float s) { pitch = s; actual.speed = s; temp.speed = s; }
    void SetStretch(float t) { stretch = t; ideal.speed = t; }
    void SetLeash(int k)   { leash = k; }
    void SetFade(float f)  { fixedDuration = (f < 1.0f) ? 1.0f : f;
                             fixedIncrement = 1.0f / fixedDuration; }

    float  GetPitch()   const { return pitch; }
    float  GetStretch() const { return stretch; }
    int    GetLeash()   const { return leash; }
    Position GetIdeal() const { return { ideal.w.index, ideal.index }; }

    // Uniform samples the stretch grid sits behind the writer (the read lag);
    // the engine diffs L vs R for the stereo guardrail.
    double   GridLag()   const { return backlog.uniform; }
    Position GetActual() const { return { actual.w.index, actual.index }; }

    Position GetMaster()   const { return master; }
    Position GetLiveEdge() const { return liveEdge; }
    Distance GetRunway()   const { return runway; }
    Distance GetBacklog()  const { return backlog; }
    Distance GetDistance() const { return distance; }
    bool     IsSplicing()  const { return splicing; }
    bool     IsAdaptive()  const { return adaptive; }

    // (Re)seat both cursors at `pos` and clear any in-flight crossfade.
    void Spawn(double pos) {
        Window w = sparse_->WindowAtLatest();
        sparse_->StepWindow(w, pos);
        ideal    = Head(w, pos, stretch);
        actual   = Head(w, pos, pitch);
        splicing    = false;
        splicePhase = 0.0f;
        RefreshTransients();
    }

    // Recompute the delayed tap: master = writePos - fade, clamped to
    // [0, liveEdge]. The fade length is the minimum lead, so a full crossfade
    // fits in front of an anchor before the fence. `masterW` steps incrementally
    // (the tap advances ~one block per block); re-seed from the latest window
    // only if the writer lapped the cached window off the ring.
    void UpdateMaster() {
        double m = writePos.uniform - (double)fixedDuration;
        if (m > liveEdge.uniform) m = liveEdge.uniform;
        if (m < 0.0)              m = 0.0;
        master.uniform = m;
        if (masterW.index <= sparse_->GetOldestIndex())
            masterW = sparse_->WindowAtLatest();
        sparse_->StepWindow(masterW, m);
        master.sparse = masterW.index;
    }

    // Snapshot the fences once per block and recompute the derived landmarks.
    void BeginBlock(int size) {
        blockSize = size;
        if (tr_ && tr_->count != trSeen_) RefreshTransients();
        writePos  = sparse_->GetWritePosition();
        liveEdge  = sparse_->GetMinReadPosition(size);
        UpdateMaster();
    }

    // Begin a FIXED-duration crossfade re-anchoring to the delayed tap. Reuses
    // the cached `masterW` — no walk, no search.
    void ReanchorMaster() {
        temp        = Head(masterW, master.uniform, pitch);
        splicePhase = 0.0f;
        splicing    = true;
        adaptive    = false;
        guardInc_   = 0.0f;
    }

    // Begin a FIXED-duration crossfade back onto the stretch grid, leaving the
    // grid's own time intact. The front guard uses this: when the head races the
    // live edge, only the HEAD needs rescuing — hauling the grid to the tap
    // destroys the stretch timer. At stretch = 1 grid ≈ tap, so this matches the
    // classic OLA delay-span loop.
    void ReanchorGrid() {
        temp        = Head(ideal.w, ideal.index, pitch);
        splicePhase = 0.0f;
        splicing    = true;
        adaptive    = false;
        guardInc_   = 0.0f;
    }

    // Hard-seat both cursors at the delayed tap with NO internal fade — for grain
    // trading, where the caller crossfades between two Granules one layer up (see
    // KeyframeRecorder). BeginBlock has already refreshed master this block; any
    // in-flight splice is simply abandoned, since this grain enters at zero gain.
    void Retrigger() {
        ideal  = Head(masterW, master.uniform, stretch);
        actual = Head(masterW, master.uniform, pitch);
        splicing    = false;
        splicePhase = 0.0f;
        RefreshTransients();
    }

    // ── Athanor addition (speed-up support) ──────────────────────────────────
    // Hard-seat both cursors `lag` samples BEHIND the delayed tap (same contract
    // as Retrigger: no internal fade, the caller crossfades one layer up). With
    // stretch > 1 the grid then races forward through that recent history and
    // catches the tap after lag / (stretch - 1) samples: a pitch-preserved
    // fast-forward replay. Clamped to the oldest frame still live in the ring.
    void RetriggerBehind(double lag) {
        if (lag <= 0.0) { Retrigger(); return; }
        double pos = master.uniform - lag;
        const double oldest = sparse_->GetSample(sparse_->GetOldestIndex() + 2)->time;
        if (pos < oldest) pos = oldest;
        Window w = sparse_->WindowAt(pos);
        ideal  = Head(w, pos, stretch);
        actual = Head(w, pos, pitch);
        splicing    = false;
        splicePhase = 0.0f;
        RefreshTransients();
    }

    // Uniform samples the stretch grid sits AHEAD of the delayed tap (negative =
    // behind). The recorder uses it to loop the fast-forward before the grid can
    // run into the live edge.
    double GridAheadOfTap() const { return ideal.index - master.uniform; }

    // Athanor: attach the recorder's onset ring (enables transient-aware splicing).
    void SetTransients(const TransientRing* r) { tr_ = r; RefreshTransients(); }
    void SetAlign(bool on) { alignOn_ = on; }

    // Produce one output sample, unshaped. Kept as the generic entry point for
    // ports that don't want the waveshaper; delegates through an identity
    // functor so there is only one implementation.
    float Read() { return Read([](float v) { return v; }); }

    // Produce one output sample, shaping each keyframe on the way out.
    template <typename ShapeFn>
    float Read(ShapeFn fn) {
        // Keep every cursor's keyframe bracket AROUND its index. Two-sided: at
        // negative stretch the grid walks backward out of its window, which a
        // past-the-end test never notices, and the window then stays stale for
        // the rest of the pass. StepWindow always handled both directions; only
        // the call was one-sided.
        if (ideal.index  > ideal.w.endTime  || ideal.index  < ideal.w.p2->time)
            sparse_->StepWindow(ideal.w,  ideal.index);
        if (actual.index > actual.w.endTime || actual.index < actual.w.p2->time)
            sparse_->StepWindow(actual.w, actual.index);
        if (splicing && (temp.index > temp.w.endTime || temp.index < temp.w.p2->time))
            sparse_->StepWindow(temp.w,   temp.index);

        // Refresh live distances. Indices are monotonic, so these are plain
        // subtractions — signed and correct across any number of ring wraps.
        // distance > 0: actual leads the grid; < 0: it lags (pitch down).
        distance.sparse  = actual.w.index - ideal.w.index;
        distance.uniform = actual.index    - ideal.index;
        runway.sparse    = liveEdge.sparse - actual.w.index;
        runway.uniform   = liveEdge.uniform - actual.index;
        backlog.sparse   = writePos.sparse - ideal.w.index;
        backlog.uniform  = writePos.uniform - ideal.index;

        // ── boundary guards (fixed fade → master) ────────────────────────────
        // Safety outranks the adaptive splice: a guard may preempt an adaptive
        // fade, never a fixed one. The front guard only arms when the head is
        // closing on the fence (pitch > 1); it fires when the runway left is less
        // than the fade will consume. One block of slack covers the per-block
        // fence snapshot.
        if (!splicing || adaptive) {
            bool frontBreach = false;
            if (pitch > 1.0f) {
                // In-flight adaptive fade: compare the runway against what the
                // REMAINDER of that fade consumes; idle, against a full fixed fade.
                double closing = splicing
                    ? (double)((1.0f - splicePhase) * adaptiveDuration)
                          * (1.0 - 1.0 / (double)pitch)
                    : (double)fixedDuration * (1.0 - 1.0 / (double)pitch);
                frontBreach = runway.uniform <= closing + (double)blockSize;
            }
            // Back edge, from either side: the writer lapping the grid, or the
            // grid reversing onto the oldest frame still live. Both mean the
            // same thing — there is nothing behind us — and both loop to the
            // delayed tap over a full fixed fade.
            bool backBreach = backlog.sparse >= kSafetyKeyframes
                           || ideal.w.index <= sparse_->GetOldestIndex() + 1;
            if (backBreach) {
                // The GRID is about to fall off the ring: full reset to the tap.
                ReanchorMaster();
                ideal = Head(masterW, master.uniform, stretch);
            } else if (frontBreach) {
                // Only the HEAD is in danger: fade it back onto the stretch grid,
                // leaving the grid's time (the stretch timer) alone.
                ReanchorGrid();
            }
        }

        // ── Athanor: transient-aware splicing ────────────────────────────────
        // Plain OLA stretching replays every attack: at 0.5x the head (always
        // at pitch 1) runs ahead of the grid, plays the hit, gets spliced back,
        // and plays it again. Here each onset is played once, when the GRID
        // reaches it:
        //  - slow (stretch < 1): if the head is about to play an onset the grid
        //    is still more than a fade away from, pull it back onto the grid
        //    first (the fade completes before the attack). When the grid gets
        //    within a fade, seat the head one fade before the onset so the
        //    attack lands at full gain. Once played, hold adaptive splices
        //    until the grid has passed it, so they can't land in front of it.
        //  - fast (stretch > 1): if the grid is about to pass an onset the head
        //    hasn't reached, splice the head to just before it, so attacks
        //    aren't skipped.
        bool holdSplice = false;
        if (tr_) {
            if (actual.index > nextT_) RefreshTransients();     // head crossed an onset
            if (!splicing || adaptive) {
                const double fade = (double)fixedDuration;
                if (stretch < 1.0f) {
                    // "Arriving" = the grid reaches the onset within ~4 fades of
                    // output time; near freeze it never does, so attacks are
                    // never replayed out of a frozen grain.
                    const bool arriving = (nextT_ - ideal.index) <= (double)stretch * 4.0 * fade;
                    if (!arriving) {
                        // grid still far: don't let the head reach the attack.
                        // Look two fades ahead; the fade is shortened if needed
                        // so the outgoing grain never reaches the onset.
                        const double room = nextT_ - actual.index - (double)blockSize;
                        if (actual.index < nextT_ && room <= 2.0 * fade) {
                            ATH_DBG('G', actual.index, ideal.index, nextT_);
                            // Target the grid, but never closer than 3 fades to the
                            // onset, so a grid parked just before a hit still loops
                            // a usable stretch of audio rather than buzzing.
                            double target = ideal.index;
                            if (target > nextT_ - 3.0 * fade) target = nextT_ - 3.0 * fade;
                            ReanchorTo(target, (float)((room < fade) ? (room < 48.0 ? 48.0 : room) : fade), true);
                        }
                    } else if (actual.index < nextT_ - fade) {
                        // grid has arrived: seat the head so its fade-in ends
                        // exactly on the attack (outgoing grain stays short of it)
                        { ATH_DBG('T', actual.index, ideal.index, nextT_); ReanchorTo(nextT_ - fade); }
                    }
                } else if (stretch > 1.0f) {
                    if (actual.index < nextT_ - fade && ideal.index >= nextT_ - fade
                        && nextT_ < liveEdge.uniform)
                        ReanchorTo(nextT_ - fade);
                }
            }
            if (stretch < 1.0f && lastT_ > ideal.index
                && actual.index - ideal.index < kMaxHold)
                holdSplice = true;
        }

        // ── adaptive splice (adaptive fade → ideal grid) ─────────────────────
        // The head has drifted a full grain off the grid: crossfade back onto it.
        // Symmetric — a lead (pitch up) or lag (pitch down) both trigger. Lowest
        // priority; never interrupts a fade in progress.
        long long lead = distance.sparse < 0 ? -distance.sparse : distance.sparse;
        if (!splicing && !holdSplice && lead > leash) {
            ATH_DBG('A', actual.index, ideal.index, nextT_);
            temp = Aligned(ideal.w, ideal.index);   // Athanor: phase-aligned near the grid
            temp.speed = pitch;

            // Budget = raw-sample span of `leash` keyframes, measured in the
            // direction the splice crosses. A lagging head (pitch down) fades
            // across the keyframes BEHIND the grid; a leading head (pitch up)
            // fades across the span ahead, then CLAMPS to 90% of the runway so
            // the fade lands with room to spare.
            long long sparseIndex = ideal.w.index;
            // Budget behind when the head lags the grid, and also whenever the
            // grid itself is reversing — that is the side it is heading into.
            // The sign of `stretch` already says so; no extra state for it.
            if (distance.sparse < 0 || stretch < 0.0f)
                adaptiveDuration = (float)(sparse_->GetSample(sparseIndex - 1)->time
                                         - sparse_->GetSample(sparseIndex - leash)->time);
            else                       // head leads the grid: budget ahead
                adaptiveDuration = (float)(sparse_->GetSample(sparseIndex + leash - 1)->time
                                         - sparse_->GetSample(sparseIndex - 1)->time);
            float limit = 0.9f * (float)runway.uniform;
            if (adaptiveDuration > limit) adaptiveDuration = limit;
            if (adaptiveDuration < 48.0f) adaptiveDuration = 48.0f;   // 1 ms floor — never a click

            adaptiveIncrement = 1.0f / adaptiveDuration;
            splicePhase       = 0.0f;
            splicing          = true;
            adaptive          = true;
        }

        // Advance the Now marker (non-reading).
        ideal.index += ideal.speed;

        // ── crossfade in progress ────────────────────────────────────────────
        if (splicing && splicePhase <= 1.0f) {
            temp.speed = pitch;                    // stay locked to live pitch
            float in  = Sample(temp, fn);
            float out = Sample(actual, fn);
            temp.index   += temp.speed;
            actual.index += actual.speed;

            const float w = SmoothStep(splicePhase);
            float mixed = (in * w) + (out * (1.0f - w));
            // |pitch|: the fade spans buffer samples, crossed in either direction.
            splicePhase += (adaptive ? adaptiveIncrement
                                     : (guardInc_ > 0.0f ? guardInc_ : fixedIncrement))
                         * (pitch < 0.0f ? -pitch : pitch);
            return mixed;
        }
        // ── crossfade complete: adopt the incoming cursor ────────────────────
        else if (splicing && splicePhase > 1.0f) {
            actual      = temp;
            guardInc_   = 0.0f;
            RefreshTransients();
            splicePhase = 0.0f;
            splicing    = false;
        }

        // ── steady read ──────────────────────────────────────────────────────
        float out = Sample(actual, fn);
        actual.index += actual.speed;
        return out;
    }
};

} // namespace capicola
