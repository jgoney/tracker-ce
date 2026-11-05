#pragma once
#include "Platform.h"

#include <cstdint>

// mach_absolute_time based clock; millis()/micros() count from construction.
class TimerApple : public Timer
{
  public:
    TimerApple();

    uint32_t millis() override;
    uint32_t micros() override;
    void delay(int ms) override;
    uint32_t clockCycles() override;
    uint32_t clockCyclesPerUs() override { return 600; } // i.MX RT1051 core clock, MHz
    // The host has no timer interrupt: the callback runs immediately and may read how far into
    // the audio sub-block it was meant to fire via currentSubBlockOffsetUs() (valid only during
    // the call, per thread).
    void scheduleMidiClockUs(uint32_t delayUs, void (*callback)()) override;
    void cancelMidiClock() override {}

    static uint32_t currentSubBlockOffsetUs();

  private:
    uint64_t nowNs() const;
    uint64_t startNs_;
};
