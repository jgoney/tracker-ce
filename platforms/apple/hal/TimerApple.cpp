#include "TimerApple.h"

#include <mach/mach_time.h>
#include <unistd.h>

namespace
{
thread_local uint32_t tlSubBlockOffsetUs = 0;
thread_local bool tlInBlock = false;
thread_local uint64_t tlBlockIndex = 0;
thread_local uint64_t tlFrozenNs = 0;

uint64_t machNs()
{
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
}
} // namespace

TimerApple::TimerApple() : startNs_(machNs()) {}

uint64_t TimerApple::nowNs() const
{
    if (tlInBlock)
        return tlFrozenNs;
    return machNs() - startNs_;
}

uint32_t TimerApple::millis()
{
    return (uint32_t)(nowNs() / 1000000ull);
}

uint32_t TimerApple::micros()
{
    return (uint32_t)(nowNs() / 1000ull);
}

void TimerApple::delay(int ms)
{
    if (ms > 0)
        usleep((useconds_t)ms * 1000u);
}

uint32_t TimerApple::clockCycles()
{
    return (uint32_t)(nowNs() * 600ull / 1000ull); // 600 cycles per microsecond
}

void TimerApple::scheduleMidiClockUs(uint32_t delayUs, void (*callback)())
{
    if (!callback)
        return;
    tlSubBlockOffsetUs = delayUs;
    callback();
    tlSubBlockOffsetUs = 0;
}

uint32_t TimerApple::currentSubBlockOffsetUs()
{
    return tlSubBlockOffsetUs;
}

uint64_t TimerApple::blockStartUs(uint64_t blockIndex)
{
    return blockIndex * 128ull * 1000000ull / 44100ull;
}

void TimerApple::beginBlock(uint64_t blockIndex)
{
    tlBlockIndex = blockIndex;
    tlFrozenNs = blockIndex * 128ull * 1000000000ull / 44100ull;
    tlInBlock = true;
}

void TimerApple::endBlock()
{
    tlInBlock = false;
}

bool TimerApple::inBlock()
{
    return tlInBlock;
}

uint64_t TimerApple::currentBlockIndex()
{
    return tlBlockIndex;
}
