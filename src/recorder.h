/**
 * @file recorder.h
 * @brief Stream the final stereo output to the SD card as 24-bit WAV.
 *
 * The audio thread only packs samples into a 2 MB ring in SDRAM (~7 s of
 * cover for slow cards). The control loop drains it to the card in 16 KB
 * sector-aligned writes, rewrites the WAV header and syncs every few
 * seconds (so a take survives a power cut), and closes the file on stop.
 *
 * Files are REC0001.WAV, REC0002.WAV, ... at the card root (FAT32). While
 * a take is open its path is locked against HostLink writes/deletes; the
 * web programmer can still list and download finished takes over USB.
 *
 * Follows the Alchemy SDK's three SD-DMA rules (alchemy/storage/sd_card.h):
 * the FIL and the staging buffer live in AXI SRAM (.axi_bss), 32-byte
 * aligned, and the WAV header is padded to 512 bytes so every data write
 * starts on a sector boundary from an aligned pointer.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace alchemy { class SdCard; }

namespace athanor {

class Recorder
{
  public:
    enum class Event : uint8_t
    {
        None,
        Started,    ///< take opened, recording
        Stopped,    ///< take closed cleanly
        Failed,     ///< could not start (no card / not FAT32 / card full)
        CardError,  ///< write failed mid-take: take closed, recording stopped
        Dropout,    ///< card fell behind and samples were lost (still recording)
    };

    static constexpr uint32_t kRingBytes  = 2u * 1024u * 1024u;   // power of two
    static constexpr uint32_t kChunkBytes = 16u * 1024u;          // per card write
    static constexpr uint32_t kHeaderBytes = 512u;

    void Init(alchemy::SdCard* card, uint32_t sample_rate);

    /** Control thread. */
    void Start(uint32_t now_ms);
    void Stop();
    void Poll(uint32_t now_ms);
    /** Recording, or still flushing the end of a take. */
    bool Active() const { return state_ != State::Idle; }
    bool Recording() const { return state_ == State::Recording; }
    /** Most recent event since the last call (None if nothing new). */
    Event TakeEvent() { const Event e = event_; event_ = Event::None; return e; }
    /** Take number of the current / last file (REC%04u.WAV). */
    uint32_t TakeNumber() const { return take_; }

    /** Audio thread: append one block (no-op unless recording). */
    void Push(const float* l, const float* r, std::size_t n);

  private:
    enum class State : uint8_t { Idle, Recording, Stopping };

    bool WriteChunk(uint32_t bytes);
    bool WriteHeader();
    void Finish(Event ev);

    alchemy::SdCard* card_ = nullptr;
    uint32_t sr_ = 48000;

    volatile State    state_ = State::Idle;
    volatile bool     armed_ = false;      // audio thread may push
    volatile uint32_t w_ = 0;              // bytes written by audio (monotonic)
    volatile uint32_t r_ = 0;              // bytes drained to the card
    volatile bool     overflow_ = false;   // audio found the ring full
    Event    event_ = Event::None;
    uint32_t take_ = 0;
    uint32_t data_bytes_ = 0;
    uint32_t last_sync_ms_ = 0;
    char     path_[16] = {};
};

} // namespace athanor
