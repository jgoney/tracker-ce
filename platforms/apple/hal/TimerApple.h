#pragma once
#include "Platform.h"

#include <cstdint>

// mach_absolute_time based clock; millis()/micros() count from construction.
//
// Frozen time on the render thread: between beginBlock(n) and endBlock() on a thread,
// millis()/micros()/clockCycles() on THAT thread return the nominal start of audio block n
// (n * 128 / 44100 s from the stream origin) and do not advance. Core code that measures
// "elapsed since the block started" (mtMidi.cpp sendMidiClock) then sees 0 elapsed, so its delay
// is exactly the sub-block offset. Other threads (tracker thread) see real time.
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

    // AudioRuntime's render thread, around each block's core calls (see above).
    static void beginBlock(uint64_t blockIndex);
    static void endBlock();
    // True on a thread between beginBlock and endBlock; currentBlockIndex() is that block.
    static bool inBlock();
    static uint64_t currentBlockIndex();
    // Nominal start of a block in microseconds from the stream origin (not wrapped to 32 bits).
    static uint64_t blockStartUs(uint64_t blockIndex);

  private:
    uint64_t nowNs() const;
    uint64_t startNs_;
};
