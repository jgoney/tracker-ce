// Interrupt control for the host. There are no interrupts: the core's critical sections are
// recorded as nesting counters. Task 6 forwards these to AudioRuntime (a mutex / audio gate).
#include <atomic>

namespace
{
std::atomic<int> gGlobalIrqDisableDepth{ 0 };
std::atomic<int> gAudioIrqDisableDepth{ 0 };
} // namespace

void platformDisableGlobalInterrupts()
{
    gGlobalIrqDisableDepth++;
}

void platformEnableGlobalInterrupts()
{
    if (gGlobalIrqDisableDepth > 0)
        gGlobalIrqDisableDepth--;
}

void disableAudioInterrupt()
{
    gAudioIrqDisableDepth++;
}

void enableAudioInterrupt()
{
    if (gAudioIrqDisableDepth > 0)
        gAudioIrqDisableDepth--;
}
