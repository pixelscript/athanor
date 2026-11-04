/**
 * @file crashlog.cpp
 * @brief See crashlog.h.
 */
#include "crashlog.h"

#include "stm32h7xx.h"

namespace athanor {
namespace crash {

static constexpr uint32_t kMagic = 0xA7A0C0DEu;

/* Backup SRAM (0x38800000, 4 KB) survives a reset but not a power cycle.
 * Its first bytes hold the bootloader's boot_info handoff record, so ours
 * sits at a fixed 1 KB offset, well clear of it. */
static constexpr uintptr_t kRecordAddr = 0x38800400u;
volatile Record* const rec = reinterpret_cast<volatile Record*>(kRecordAddr);
#define g_record (*athanor::crash::rec)

bool PreviousBootCrashed()
{
    return g_record.magic == kMagic && g_record.crashed == 1u;
}

void Acknowledge()
{
    g_record.crashed = 0u;
}

} // namespace crash
} // namespace athanor



extern "C" {

/* Called from the naked fault stubs with the stacked exception frame. */
__attribute__((used)) void AthanorFaultC(uint32_t* frame, uint32_t kind)
{
    g_record.magic   = athanor::crash::kMagic;
    g_record.crashed = 1u;
    g_record.kind    = kind;
    g_record.pc      = frame ? frame[6] : 0u;
    g_record.lr      = frame ? frame[5] : 0u;
    g_record.cfsr    = SCB->CFSR;
    g_record.hfsr    = SCB->HFSR;
    g_record.bfar    = SCB->BFAR;
    g_record.count   = g_record.count + 1u;
    SCB_CleanDCache();   // backup SRAM is cacheable: push the record out first
    __DSB();
    NVIC_SystemReset();
    for (;;) {}
}

#define ATHANOR_FAULT_STUB(name, kind)                       \
    __attribute__((naked)) void name(void)                   \
    {                                                        \
        __asm volatile("tst lr, #4      \n"                  \
                       "ite eq          \n"                  \
                       "mrseq r0, msp   \n"                  \
                       "mrsne r0, psp   \n"                  \
                       "movs r1, #" #kind "\n"               \
                       "b AthanorFaultC \n");                \
    }

ATHANOR_FAULT_STUB(BusFault_Handler, 2)
ATHANOR_FAULT_STUB(UsageFault_Handler, 3)
ATHANOR_FAULT_STUB(MemManage_Handler, 4)
ATHANOR_FAULT_STUB(AthanorHardFault_Handler, 1)

/* Runs from __libc_init_array's .preinit_array, i.e. before any C++ static
 * constructor, so even a fault during static init is caught. */
static void AthanorPreInit(void)
{
    /* Backup SRAM access (clocks/regs were reset by the system reset). */
    PWR->CR1 |= PWR_CR1_DBP;
    while ((PWR->CR1 & PWR_CR1_DBP) == 0u) {}
    RCC->AHB4ENR |= RCC_AHB4ENR_BKPRAMEN;

    /* D2 SRAM1/2/3 clocks: the reverb buffer sits in SRAM1+2. The bootloader
     * skips SystemInit (BOOT_APP), so nothing else guarantees these. */
    RCC->AHB2ENR |= (RCC_AHB2ENR_D2SRAM1EN | RCC_AHB2ENR_D2SRAM2EN | RCC_AHB2ENR_D2SRAM3EN);
    (void)RCC->AHB2ENR;

    /* Fresh power-on: backup SRAM holds garbage. */
    if (g_record.magic != athanor::crash::kMagic)
    {
        g_record.magic   = athanor::crash::kMagic;
        g_record.crashed = 0u;
        g_record.count   = 0u;
    }

    /* Separate fault vectors, so bus/usage faults reach our handlers instead
     * of escalating into libDaisy's HardFault (which just halts). */
    SCB->SHCSR |= SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk
                | SCB_SHCSR_MEMFAULTENA_Msk;

    /* The vector table is in SRAM (BOOT_SRAM app): point HardFault at ours
     * too, overriding libDaisy's halt-forever handler. */
    volatile uint32_t* vt = reinterpret_cast<volatile uint32_t*>(SCB->VTOR);
    vt[3] = reinterpret_cast<uint32_t>(&AthanorHardFault_Handler);
    __DSB();
    __ISB();

    if (!(g_record.crashed == 1u))
        g_record.stage = athanor::crash::kStaticInit;
}

__attribute__((section(".preinit_array"), used))
static void (*const athanor_preinit)(void) = AthanorPreInit;

} // extern "C"
