#pragma once
// Shared by the Core Audio tests: a fake core and small wait helpers.
#include "AudioRuntime.h"
#include "Tracker.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace ca_test
{
using namespace std::chrono_literals;

inline void startDisplayNoop(Platform*)
{
}

class FakeTracker final : public Tracker
{
  public:
    void init(Platform*) override {}
    void update() override { std::this_thread::sleep_for(1ms); }
    void updateAudioBuffers(void* l, void* r, int length) override
    {
        auto* pl = (int16_t*)l;
        auto* pr = (int16_t*)r;
        for (int i = 0; i < length; i++)
        {
            phase = (phase + 1) % 100;
            pl[i] = pr[i] = (int16_t)(phase * 100);
        }
    }
    void updateInputAudioBuffers(void*, void*, int) override {}
    void (*getStartDisplayFunc())(Platform*) override { return startDisplayNoop; }
    Platform* getPlatform() override { return nullptr; }
    int phase = 0;
};

inline bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
} // namespace ca_test
