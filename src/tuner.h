/**
 * @file tuner.h
 * @brief Tuner for the six rings: six strings at once, six octaves of C, or
 *        any single note.
 *
 * Three modes:
 *
 *   Guitar     six narrow detectors on E2 A2 D3 G3 B3 E4. Strum, and every
 *              string that is ringing reports how flat or sharp it is.
 *   C octaves  six detectors on C1..C6, for tuning oscillators to C. The
 *              lowest one that hears something is the fundamental (higher
 *              ones would only be hearing its overtones) and is the one
 *              reported.
 *   Chromatic  finds the pitch of a single note from about 30 Hz to 2 kHz
 *              (C1 to C7; bright waveforms above that can read an octave
 *              low), names the nearest of the 12 notes and its octave, and
 *              measures the offset from it.
 *
 * How the offset is measured: each detector multiplies the input by a sine
 * and cosine at its target frequency and low-passes the result. What is left
 * is a slowly rotating pointer whose rotation speed IS the tuning error, so
 * the reading is as fine as you like without a long FFT. Chromatic mode
 * first finds the note with an autocorrelation pitch detector (McLeod's
 * NSDF), then points one such detector at that note for the fine reading.
 *
 * Threads: Push() runs on the audio thread and does the detectors. Poll()
 * runs on the control loop and does the pitch search a slice at a time, so
 * it never holds the loop up. Readings are plain values read by either.
 *
 * Pure DSP, no hardware dependencies (host-testable).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace athanor {

class Tuner
{
  public:
    /** In the order B1 steps through them. */
    enum Mode : uint8_t { kCOctaves = 0, kChromatic, kGuitar, kNumModes };

    static constexpr int kBands = 6;
    /** Pitch-search buffer, at half the sample rate. */
    static constexpr uint32_t kWindow  = 2048;                // samples correlated
    static constexpr uint32_t kMaxLag  = 880;                 // down to ~27 Hz at 24 kHz
    static constexpr uint32_t kRing    = 4096;                // power of two, > kWindow + kMaxLag
    /** Floats the caller provides: the ring and the snapshot the search works on. */
    static constexpr uint32_t kMemFloats = kRing + kWindow + kMaxLag + 8;

    /** One ring's worth of reading. */
    struct Reading
    {
        bool  active = false;   // something is sounding here
        float cents  = 0.0f;    // offset from the target, + = sharp
        float level  = 0.0f;    // 0..1, for brightness
    };

    void Init(float sr, float* mem);
    void SetMode(Mode m);
    Mode GetMode() const { return mode_; }

    /** Audio thread: one block of input (mono sum is taken inside). */
    void Push(const float* l, const float* r, std::size_t n);
    /** Control loop: a slice of the pitch search (chromatic mode). */
    void Poll();

    /** Guitar / C octaves: band 0..5, low to high. */
    Reading Band(int i) const;
    /** C octaves: the band that holds the fundamental, or -1. */
    int     Fundamental() const;

    /** Chromatic: is there a note, which (0 = C .. 11 = B), which octave
     *  (4 = the octave of middle C / A440), and how far off. */
    bool  HasNote() const { return note_ >= 0; }
    int   NoteClass() const { return note_ >= 0 ? note_ % 12 : -1; }
    int   Octave() const { return note_ >= 0 ? note_ / 12 - 1 : 0; }
    float NoteCents() const { return note_cents_; }
    float NoteHz() const { return note_hz_; }

    /** Frequency of a band's target (for tests / display). */
    float TargetHz(int i) const { return band_[i].f0; }

  private:
    struct Detector
    {
        float f0 = 0.0f;
        float c = 1.0f, s = 0.0f, dc = 1.0f, ds = 0.0f;     // rotating reference
        float a = 0.0f;                                      // low-pass coefficient
        float re[4] = {0, 0, 0, 0}, im[4] = {0, 0, 0, 0};
        float pre = 0.0f, pim = 0.0f;                        // pointer one block ago
        float ar = 0.0f, ai = 0.0f;                          // averaged rotation
        float mag = 0.0f;
        volatile float cents = 0.0f, level = 0.0f;
        int   settle = 0;                                    // blocks until trustworthy
    };

    void Tune(Detector& d, float hz, float bandwidth);
    void Run(Detector& d, const float* mono, std::size_t n);
    void FinishSearch();

    float sr_ = 48000.0f;
    volatile Mode mode_ = kCOctaves;
    Detector band_[kBands];

    /* Chromatic. */
    float*   ring_ = nullptr;         // half-rate input
    float*   snap_ = nullptr;         // copy the search works on
    volatile uint32_t rw_ = 0;
    float    half_ = 0.0f; bool half_have_ = false;
    volatile float in_peak_ = 0.0f;
    uint32_t lag_ = 0;                // next lag to compute (0 = take a new snapshot)
    float    nsdf_prev_ = 0.0f, nsdf_prev2_ = 0.0f;
    float    e0_ = 0.0f;              // energy of the fixed span
    float    peak_val_ = 0.0f, peak_lag_ = 0.0f; bool in_peak_region_ = false;
    float    cand_val_[16]; float cand_lag_[16]; int cand_n_ = 0;
    double   m_ = 0.0;                // running energy term
    int      pending_note_ = -1, pending_count_ = 0;
    volatile int   note_ = -1;        // MIDI note number, or -1
    volatile float note_cents_ = 0.0f, note_hz_ = 0.0f;
    float    rough_cents_ = 0.0f;
    volatile float retune_hz_ = 0.0f; // control loop -> audio thread
    int      silent_ = 0;
};

} // namespace athanor
