#include "Profiler.h"
#include "config.h"
#include "limits.h"
#include "log.h"
#ifndef PLATFORM_APPLE
#include "malloc.h"
#endif
#include <polyassert.h>

#include "Platform.h"
#include "Tracker.h"
#include "math.h"
#include <algorithm>
#include <vector>

// extern uint32_t _estack;
extern "C"
{
    extern void _vStackTop(void); // symbols from linker script
    extern void _vStackBase(void);
}
Profiler* Profiler::instance = nullptr;

void Profiler::init()
{
#ifndef PLATFORM_APPLE
    // allocate and then free all available memory for malloc to claim all space
    // from sbrk, so that mallinfo data includes all of it

    std::vector<void*> allocations;
    allocations.reserve(128);
    size_t allocSize = 4096;
    do
    {
        void* buf = malloc(allocSize);
        if (buf != nullptr)
        {
            allocations.push_back(buf);
        }
        else
        {
            allocSize /= 2;
        }
    } while (allocSize > 16);

    std::for_each(std::rbegin(allocations), std::rend(allocations), [](void* buf) { free(buf); });
    allocations.clear();
    allocations.shrink_to_fit();

    struct mallinfo mi = mallinfo();
    (void)mi;
    LOGF(
      "memory on init: arena: %d, allocated: %d, free: %d\r\n", mi.arena, mi.uordblks, mi.fordblks);
#endif // PLATFORM_APPLE (no mallinfo/sbrk on the host)

    resetMeasuringAudio();
}

size_t Profiler::freeRamBytes()
{
#ifdef PLATFORM_APPLE
    return 0;
#else
    return mallinfo().fordblks;
#endif
}

size_t Profiler::maxStackBytes()
{
#ifdef PLATFORM_IMX
    const uint8_t* base = (uint8_t*)&_vStackBase;
    const uint8_t* top = (uint8_t*)&_vStackTop;

    const uint8_t* addr = top - 1;

    int watermarksCounter = 0;
    static constexpr int kConsecutiveWatermarksToSuccess = 3;
    while (addr >= base)
    {

        if (*addr-- == kStackWatermark)
        {
            watermarksCounter++;
        }
        else
        {
            watermarksCounter = 0;
        }

        if (watermarksCounter == kConsecutiveWatermarksToSuccess)
        {
            break;
        }
    }

    return (size_t)(top - addr) - watermarksCounter;
#else
    return 0;
#endif
}

void Profiler::resetMeasuringAudio()
{
    lastSampleUs = 0;
    sumOfSamples = 0;
    minSampleUs = INT_MAX;
    maxSampleUs = 0;
    samples = 0;
    tempMaxSampleUs = 0;
}

void Profiler::startMeasuringAudio()
{
    if (Tracker::getInstance()->getPlatform() == nullptr)
    {
        return;
    }

    audioUpdateTimer = Tracker::getInstance()->getPlatform()->getTimer()->clockCycles();
}

void Profiler::stopMeasuringAudio()
{
    if (Tracker::getInstance()->getPlatform() == nullptr)
    {
        return;
    }

    lastSampleUs =
      std::max<int>(
        Tracker::getInstance()->getPlatform()->getTimer()->clockCycles() - audioUpdateTimer, 0) /
      (float)Tracker::getInstance()->getPlatform()->getTimer()->clockCyclesPerUs();

    minSampleUs = std::min(minSampleUs, lastSampleUs);
    maxSampleUs = std::max(maxSampleUs, lastSampleUs);
    tempMaxSampleUs = std::max(tempMaxSampleUs, lastSampleUs);

    sumOfSamples += lastSampleUs;
    samples++;
}

void Profiler::getAudioProcessingUsageUs(AudioProcessorUsage& usage, bool resetTempMaximum)
{
    usage.average = sumOfSamples / samples;
    usage.last = lastSampleUs;
    usage.maximum = maxSampleUs;
    usage.minimum = minSampleUs;
    usage.tempMaximum = tempMaxSampleUs;

    if (resetTempMaximum)
    {
        tempMaxSampleUs = 0;
    }
}

void Profiler::getAudioProcessingUsagePercent(AudioProcessorUsage& usage, bool resetTempMaximum)
{
    getAudioProcessingUsageUs(usage, resetTempMaximum);

    usage.average = calculatePercentProcessorUsage(usage.average);
    usage.last = calculatePercentProcessorUsage(usage.last);
    usage.maximum = calculatePercentProcessorUsage(usage.maximum);
    usage.minimum = calculatePercentProcessorUsage(usage.minimum);
    usage.tempMaximum = calculatePercentProcessorUsage(usage.tempMaximum);
}

int Profiler ::calculatePercentProcessorUsage(int processorUsageUs)
{
    return (100 * processorUsageUs) / kMaxAudioUpdateTime;
}
