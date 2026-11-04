/**
 * @file crashlog.h
 * @brief Boot-stage breadcrumbs + fault capture in backup SRAM.
 *
 * Every boot step records a stage number. If the CPU faults, the handler saves
 * the stage, the faulting PC and the fault registers to backup SRAM (which
 * survives a reset but not a power cycle) and resets. On the next boot the
 * firmware sees the record and starts in SAFE MODE instead: no DSP, the rings
 * show where it died (see README "Safe mode"), and HostLink runs so the web
 * programmer can reflash it directly.
 *
 * The same pre-main hook also enables the D2 SRAM clocks (the reverb lives
 * there) and the separate Bus/Usage/MemManage fault vectors.
 */
#pragma once

#include <cstdint>

namespace athanor {
namespace crash {

enum Stage : uint32_t
{
    kStaticInit   = 1,
    kMainStart    = 2,
    kHwInit       = 3,
    kLoadVariants = 4,
    kStretchInit  = 5,
    kAgeInit      = 6,
    kDriveInit    = 7,
    kSpaceInit    = 8,
    kChorusInit   = 9,
    kPhaserInit   = 10,
    kLoopSetup    = 11,
    kAudioStarted = 12,
    kFirstTick    = 13,
    kAudioEnter   = 14,   // audio callback sub-stages
    kAudioRow1    = 15,
    kAudioAge     = 16,
    kAudioDrive   = 17,
    kAudioMod     = 18,
    kAudioSpace   = 19,
    kRunning      = 26,   // survived 5 s: healthy
};

struct Record
{
    uint32_t magic;
    uint32_t stage;
    uint32_t crashed;    // 1 = the previous boot faulted
    uint32_t kind;       // 1 hard, 2 bus, 3 usage, 4 memmanage
    uint32_t pc, lr, cfsr, hfsr, bfar;
    uint32_t count;
};

extern volatile Record* const rec;

inline void Mark(uint32_t s) { rec->stage = s; }

/** True if the previous boot crashed (record is then latched for display). */
bool PreviousBootCrashed();

/** Clears the crashed flag (call once safe mode has captured it). */
void Acknowledge();

} // namespace crash
} // namespace athanor
