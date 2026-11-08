/**
 * @file recorder.cpp
 * @brief See recorder.h.
 */
#include "recorder.h"

#include <cstdio>
#include <cstring>

#include "daisy_seed.h"
#include "ff.h"
#include "alchemy/storage/sd_card.h"

namespace athanor {

namespace {

/* SDRAM ring the audio thread packs into (CPU-only access, so SDRAM is fine).
 * A plain array: SDRAM is NOLOAD and not running during static init. */
uint8_t DSY_SDRAM_BSS s_ring[Recorder::kRingBytes];

/* Everything FatFS may DMA from: AXI SRAM, 32-byte aligned (SD-DMA rules). */
alignas(32) ALCHEMY_SDMMC_BSS FIL     s_fil;
alignas(32) ALCHEMY_SDMMC_BSS uint8_t s_stage[Recorder::kChunkBytes];

constexpr uint32_t kRingMask     = Recorder::kRingBytes - 1u;
constexpr uint32_t kBytesPerFrame = 6u;          // 24-bit stereo
constexpr uint32_t kSyncMs       = 4000u;        // header rewrite + f_sync
constexpr uint32_t kMinFreeKib   = 16u * 1024u;  // refuse to start below 16 MB
/* FAT32 files stop at 4 GiB - 1: close the take a little before that. */
constexpr uint32_t kMaxDataBytes = 0xFFFFFFFFu - Recorder::kHeaderBytes - 2u * Recorder::kChunkBytes;

inline void Put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void Put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/** 512-byte WAV header: RIFF / fmt / JUNK padding / data, so the samples
 *  begin exactly on the second sector. */
void BuildHeader(uint8_t* h, uint32_t sr, uint32_t data_bytes)
{
    std::memset(h, 0, Recorder::kHeaderBytes);
    std::memcpy(h + 0, "RIFF", 4);
    Put32(h + 4, Recorder::kHeaderBytes - 8u + data_bytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    Put32(h + 16, 16u);
    Put16(h + 20, 1u);                       // PCM
    Put16(h + 22, 2u);                       // stereo
    Put32(h + 24, sr);
    Put32(h + 28, sr * kBytesPerFrame);
    Put16(h + 32, (uint16_t)kBytesPerFrame);
    Put16(h + 34, 24u);
    std::memcpy(h + 36, "JUNK", 4);          // pad 44 .. 503
    Put32(h + 40, 504u - 44u);               // JUNK payload: 460 bytes
    std::memcpy(h + 504, "data", 4);
    Put32(h + 508, data_bytes);
}

inline int32_t ToPcm24(float x)
{
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return (int32_t)(x * 8388607.0f);
}

} // namespace

static_assert(Recorder::kHeaderBytes == 512u, "data must start on a sector boundary");
static_assert((Recorder::kRingBytes % Recorder::kChunkBytes) == 0u, "ring holds whole chunks");

void Recorder::Init(alchemy::SdCard* card, uint32_t sample_rate)
{
    card_  = card;
    sr_    = sample_rate;
    state_ = State::Idle;
    armed_ = false;
    w_ = r_ = 0;
    std::memset(&s_fil, 0, sizeof(s_fil));
    std::memset(s_stage, 0, sizeof(s_stage));
}

void Recorder::Push(const float* l, const float* r, std::size_t n)
{
    if (!armed_) return;
    const uint32_t w    = w_;
    const uint32_t need = (uint32_t)n * kBytesPerFrame;
    if (Recorder::kRingBytes - (w - r_) < need)
    {
        overflow_ = true;   // the card fell behind: drop this block
        return;
    }
    uint32_t p = w;
    for (std::size_t i = 0; i < n; i++)
    {
        const int32_t a = ToPcm24(l[i]);
        const int32_t b = ToPcm24(r[i]);
        s_ring[p++ & kRingMask] = (uint8_t)a;
        s_ring[p++ & kRingMask] = (uint8_t)(a >> 8);
        s_ring[p++ & kRingMask] = (uint8_t)(a >> 16);
        s_ring[p++ & kRingMask] = (uint8_t)b;
        s_ring[p++ & kRingMask] = (uint8_t)(b >> 8);
        s_ring[p++ & kRingMask] = (uint8_t)(b >> 16);
    }
    __asm volatile("" ::: "memory");   // bytes land before the index moves
    w_ = p;
}

bool Recorder::WriteHeader()
{
    /* Built in the aligned staging buffer (free between chunk writes). */
    BuildHeader(s_stage, sr_, data_bytes_);
    const FSIZE_t end = f_tell(&s_fil);
    UINT bw = 0;
    if (f_lseek(&s_fil, 0) != FR_OK) return false;
    if (f_write(&s_fil, s_stage, kHeaderBytes, &bw) != FR_OK || bw != kHeaderBytes) return false;
    if (end > kHeaderBytes && f_lseek(&s_fil, end) != FR_OK) return false;
    return f_sync(&s_fil) == FR_OK;
}

bool Recorder::WriteChunk(uint32_t bytes)
{
    const uint32_t from = r_ & kRingMask;
    const uint32_t a    = (from + bytes <= kRingBytes) ? bytes : kRingBytes - from;
    std::memcpy(s_stage, &s_ring[from], a);
    if (a < bytes) std::memcpy(s_stage + a, &s_ring[0], bytes - a);
    UINT bw = 0;
    const FRESULT fr = f_write(&s_fil, s_stage, bytes, &bw);
    if (fr != FR_OK || bw != bytes) return false;
    r_          = r_ + bytes;
    data_bytes_ += bytes;
    return true;
}

void Recorder::Start(uint32_t now_ms)
{
    if (state_ != State::Idle || card_ == nullptr) return;
    alchemy::SdCard::BusyGuard busy(*card_);

    if (!card_->EnsureMounted(now_ms)) { event_ = Event::Failed; return; }
    uint32_t total = 0, free_kib = 0; bool free_ok = false;
    if (card_->VolumeKib(total, free_kib, free_ok) && free_ok && free_kib < kMinFreeKib)
    {
        event_ = Event::Failed;
        return;
    }

    /* Next unused REC####.WAV (continues after the highest existing take). */
    char fspath[20];
    FILINFO fi;
    uint32_t n = (take_ > 0u) ? take_ + 1u : 1u;
    for (; n <= 9999u; n++)
    {
        std::snprintf(path_, sizeof(path_), "/REC%04u.WAV", (unsigned)n);
        std::snprintf(fspath, sizeof(fspath), "0:%s", path_);
        const FRESULT fr = f_stat(fspath, &fi);
        if (fr == FR_NO_FILE) break;
        if (fr != FR_OK) { card_->OnIoError(); event_ = Event::Failed; return; }
    }
    if (n > 9999u) { event_ = Event::Failed; return; }

    std::memset(&s_fil, 0, sizeof(s_fil));
    if (f_open(&s_fil, fspath, FA_CREATE_NEW | FA_WRITE) != FR_OK)
    {
        card_->OnIoError();
        event_ = Event::Failed;
        return;
    }
    take_       = n;
    data_bytes_ = 0;
    if (!WriteHeader())
    {
        f_close(&s_fil);
        f_unlink(fspath);
        card_->OnIoError();
        event_ = Event::Failed;
        return;
    }
    card_->Lock(path_);
    last_sync_ms_ = now_ms;
    overflow_     = false;
    r_ = w_ = 0;
    state_  = State::Recording;
    __asm volatile("" ::: "memory");
    armed_  = true;
    event_  = Event::Started;
}

void Recorder::Stop()
{
    if (state_ != State::Recording) return;
    armed_ = false;          // audio stops pushing; Poll() flushes the rest
    state_ = State::Stopping;
}

void Recorder::Finish(Event ev)
{
    armed_ = false;
    alchemy::SdCard::BusyGuard busy(*card_);
    bool ok = (ev != Event::CardError);
    if (ok) ok = WriteHeader();
    if (f_close(&s_fil) != FR_OK) ok = false;
    card_->Unlock(path_);
    if (!ok) card_->OnIoError();
    state_ = State::Idle;
    event_ = (ev == Event::Stopped && !ok) ? Event::CardError : ev;
}

void Recorder::Poll(uint32_t now_ms)
{
    if (state_ == State::Idle) return;

    if (overflow_)
    {
        overflow_ = false;
        event_    = Event::Dropout;
    }

    /* At most one card write per poll keeps the control loop responsive
     * (~10 ms for 16 KB at the bus's 12.5 MHz / 1-bit). */
    const uint32_t avail = w_ - r_;
    alchemy::SdCard::BusyGuard busy(*card_);
    if (avail >= kChunkBytes)
    {
        if (!WriteChunk(kChunkBytes)) { Finish(Event::CardError); return; }
    }
    else if (state_ == State::Stopping)
    {
        /* Tail: whatever is left (whole frames), then close the take. */
        if (avail > 0u && !WriteChunk(avail)) { Finish(Event::CardError); return; }
        Finish(Event::Stopped);
        return;
    }

    if (data_bytes_ >= kMaxDataBytes) { Stop(); return; }   // FAT32 size limit

    if (state_ == State::Recording && (now_ms - last_sync_ms_) >= kSyncMs)
    {
        last_sync_ms_ = now_ms;
        if (!WriteHeader()) Finish(Event::CardError);
    }
}

} // namespace athanor
