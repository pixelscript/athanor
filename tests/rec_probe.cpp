// SD recorder host test: the real FatFS on the SDK's RAM disk (which counts
// unaligned DMA pointers). Checks WAV header, sample-exact content across
// takes, mid-take header sync (power-cut survival), dropouts, and failures.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "ff.h"
#include "ram_diskio.h"
#include "alchemy/storage/sd_card.h"
#include "recorder.h"
using namespace athanor;
using Ev = Recorder::Event;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static bool Format()
{
    ramdisk::Reset(131072u);
    static FATFS scratch; std::memset(&scratch, 0, sizeof(scratch));
    if (f_mount(&scratch, "0:", 0) != FR_OK) return false;
    std::vector<uint8_t> work(4096u);
    const FRESULT fr = f_mkfs("0:", FM_FAT32 | FM_SFD, 512u, work.data(), (UINT)work.size());
    f_mount(nullptr, "0:", 0);
    return fr == FR_OK;
}

static std::vector<uint8_t> ReadFile(const char* p)
{
    static FIL f; std::vector<uint8_t> d;
    if (f_open(&f, p, FA_READ) != FR_OK) return d;
    d.resize(f_size(&f)); UINT br = 0;
    f_read(&f, d.data(), (UINT)d.size(), &br); f_close(&f); d.resize(br);
    return d;
}
static uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int32_t S24(const uint8_t* p) { int32_t v = p[0] | p[1] << 8 | p[2] << 16; return (v << 8) >> 8; }

static alchemy::SdCard sd;
static Recorder rec;
static uint32_t now = 1000, bump = 0;
static float Sig(long i, int ch) { return 0.8f * std::sin(0.0123f * (float)i * (ch ? 1.7f : 1.0f)); }

/* Run `sec` seconds; poll the recorder every block when `keep_up`. */
static Ev Run(long& idx, double sec, bool keep_up, Ev* last_ev = nullptr)
{
    const int B = 64; float l[B], r[B]; Ev seen = Ev::None;
    for (long b = 0; b < (long)(48000 * sec / B); b++)
    {
        for (int i = 0; i < B; i++) { l[i] = Sig(idx + i, 0); r[i] = Sig(idx + i, 1); }
        rec.Push(l, r, B); idx += B;
        now = 1000u + (uint32_t)(idx / 48) + bump;   // real time from samples
        if (keep_up) rec.Poll(now);
        const Ev e = rec.TakeEvent(); if (e != Ev::None) seen = e;
    }
    if (last_ev) *last_ev = seen;
    return seen;
}

static void Drain() { for (int i = 0; i < 100000 && rec.Active(); i++) { rec.Poll(++now); bump++; } }

static void CheckTake(const char* path, long first, long frames, const char* what)
{
    auto d = ReadFile(path);
    CHECK(d.size() >= 512, "%s: file too short (%zu)", what, d.size());
    if (d.size() < 512) return;
    CHECK(!memcmp(&d[0], "RIFF", 4) && !memcmp(&d[8], "WAVE", 4) && !memcmp(&d[504], "data", 4), "%s: bad chunk ids", what);
    const uint32_t ds = U32(&d[508]);
    CHECK(U32(&d[4]) == d.size() - 8, "%s: RIFF size %u vs file %zu", what, U32(&d[4]), d.size());
    CHECK(ds == d.size() - 512, "%s: data size %u vs %zu", what, ds, d.size() - 512);
    CHECK(U32(&d[24]) == 48000 && d[34] == 24 && d[22] == 2, "%s: bad fmt", what);
    if (frames >= 0) CHECK(ds == (uint32_t)frames * 6, "%s: %u frames, expected %ld", what, ds / 6, frames);
    long bad = 0;
    if (first >= 0) for (uint32_t k = 0; k + 6 <= ds; k += 6)
    {
        const long i = first + k / 6;
        const int32_t el = (int32_t)(Sig(i, 0) * 8388607.0f), er = (int32_t)(Sig(i, 1) * 8388607.0f);
        if (S24(&d[512 + k]) != el || S24(&d[515 + k]) != er) bad++;
    }
    CHECK(bad == 0, "%s: %ld mismatched frames", what, bad);
    printf("  %s: %s %.2f s, %u bytes, header ok, %ld bad frames\n", what, path, ds / 6 / 48000.0, ds, bad);
}

int main()
{
    if (!Format()) { printf("mkfs failed\n"); return 1; }
    sd.Init(); rec.Init(&sd, 48000);

    printf("take 1: 10 s, card keeps up\n");
    long idx = 0;
    rec.Start(now); CHECK(rec.TakeEvent() == Ev::Started, "no Started event");
    CHECK(sd.IsLocked("/REC0001.WAV"), "take not locked while recording");
    const long start1 = idx;
    Ev e = Run(idx, 10.0, true);
    CHECK(e == Ev::None, "unexpected event %d", (int)e);
    rec.Stop(); Drain();
    CHECK(rec.TakeEvent() == Ev::Stopped, "no Stopped event");
    CHECK(!sd.IsLocked("/REC0001.WAV"), "still locked after stop");
    CheckTake("0:/REC0001.WAV", start1, idx - start1, "take 1");

    printf("take 2: numbering continues, power cut after 9 s (no Stop)\n");
    rec.Start(now); rec.TakeEvent();
    const long start2 = idx;
    Run(idx, 9.0, true);
    /* Power cut: nothing more is written. The last 4 s header sync must
     * have left a valid file with everything up to that point. */
    {
        auto d = ReadFile("0:/REC0002.WAV");
        const uint32_t ds = d.size() >= 512 ? U32(&d[508]) : 0;
        CHECK(ds >= 48000u * 6u * 4u, "after power cut only %u data bytes are covered by the header", ds);
        CHECK(ds <= d.size() - 512, "header claims more than the file holds");
        printf("  power cut: header covers %.2f s of %.2f s pushed\n", ds / 6 / 48000.0, (idx - start2) / 48000.0);
    }
    rec.Stop(); Drain(); rec.TakeEvent();
    CheckTake("0:/REC0002.WAV", start2, idx - start2, "take 2");

    printf("take 3: card stalls 9 s (ring is ~7 s) -> dropout, still recording\n");
    rec.Start(now); rec.TakeEvent();
    Ev last; Run(idx, 9.0, false, &last);
    rec.Poll(++now);
    CHECK(rec.TakeEvent() == Ev::Dropout, "no Dropout event");
    CHECK(rec.Recording(), "stopped on a dropout");
    rec.Stop(); Drain(); CHECK(rec.TakeEvent() == Ev::Stopped, "take 3 not stopped cleanly");
    CheckTake("0:/REC0003.WAV", -1, -1, "take 3 (structure only)");

    printf("take 4: card fails mid-take\n");
    rec.Start(now); rec.TakeEvent();
    Run(idx, 1.0, true);
    ramdisk::FailAfterWrites(10);
    Run(idx, 2.0, true, &last);
    CHECK(last == Ev::CardError, "no CardError (got %d)", (int)last);
    CHECK(!rec.Active(), "recorder still active after card error");
    CHECK(!sd.IsLocked("/REC0004.WAV"), "lock leaked after card error");
    ramdisk::ClearFaults();

    printf("no card: start fails\n");
    ramdisk::FailAll(true); sd.OnIoError();
    bump += 5000; now += 5000; rec.Start(now);
    CHECK(rec.TakeEvent() == Ev::Failed, "no Failed event with the card missing");
    CHECK(!rec.Active(), "active without a card");
    ramdisk::FailAll(false); bump += 5000; now += 5000;

    printf("card back: next take works\n");
    rec.Start(now); CHECK(rec.TakeEvent() == Ev::Started, "did not recover after card re-seat");
    const long start5 = idx; Run(idx, 1.0, true); rec.Stop(); Drain(); rec.TakeEvent();
    CheckTake("0:/REC0005.WAV", start5, idx - start5, "take 5");

    printf("unaligned disk I/O pointers: %u\n", ramdisk::UnalignedIo());
    CHECK(ramdisk::UnalignedIo() == 0, "unaligned DMA transfers");
    printf(fails ? "%d FAIL\n" : "recorder OK\n", fails);
    return fails ? 1 : 0;
}
