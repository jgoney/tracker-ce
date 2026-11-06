#pragma once
#include "Platform.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

struct InputEvent
{
    enum Kind
    {
        Pad,
        Button,
        Encoder
    } kind;
    uint8_t index; // pad / button index (interfacePad*, interfaceButton*)
    bool down;
    int delta; // Encoder: clicks, signed
};

// A bank of switches with press/release/hold callbacks. Driven only from dispatch().
class SwitchGroupApple : public SwitchGroup
{
  public:
    static constexpr int kMaxSwitches = 64;

    explicit SwitchGroupApple(int count) : count_(count) {}
    void setHoldTime(int timeMs) override { holdTimeMs_ = timeMs; }
    bool isButtonPressed(int idx) override;
    void setActionCallback(ActionType action, void (*callback)(uint8_t)) override;

    void press(uint8_t idx, uint32_t nowMs);
    void release(uint8_t idx);
    // Auto-repeat like the IMX Keys driver (platforms/imx/source/Drivers/Keys.cpp:202-214).
    void updateHolds(uint32_t nowMs);

  private:
    struct State
    {
        bool down = false;
        uint8_t holdFactor = 0; // index into kHoldTimeStepDiff
        uint32_t holdTimeMs = 0; // last press / last hold event
    };
    std::array<State, kMaxSwitches> state_{};
    void (*onPress_)(uint8_t) = nullptr;
    void (*onRelease_)(uint8_t) = nullptr;
    void (*onHold_)(uint8_t) = nullptr;
    // Keys.cpp:10 holdTimeStepDiff; Keys.h:98 default holdTime 333 ms.
    static constexpr int kHoldStepsCount = 6;
    static constexpr uint16_t kHoldTimeStepDiff[kHoldStepsCount] = { 0, 100, 190, 235, 255, 270 };
    int holdTimeMs_ = 333;
    int count_;
};

class EncoderApple : public Encoder
{
  public:
    void setAcceleration(int acceleration) override { acceleration_ = acceleration; }
    void setResolution(int resolution) override { resolution_ = resolution; }
    // Same shape as EncoderImpl::setChangeCallback (the core's onPotChange).
    void setChangeCallback(void (*cb)(uint8_t, int16_t)) { onChange_ = cb; }
    void change(int delta)
    {
        if (onChange_ && delta != 0)
            onChange_(0, (int16_t)delta);
    }

  private:
    void (*onChange_)(uint8_t, int16_t) = nullptr;
    int acceleration_ = 3;
    int resolution_ = 1;
};

class InputApple
{
  public:
    SwitchGroupApple& pads() { return pads_; }       // 48 indices
    SwitchGroupApple& buttons() { return buttons_; } // 44 indices (interfaceButton0..43)
    EncoderApple& encoder() { return encoder_; }

    // Any thread.
    void post(InputEvent e);
    // Any thread: runs `fn` on the tracker thread at the start of the next dispatch(), before the
    // posted input events (used to touch core state without racing the core's threads).
    void postTask(std::function<void()> fn);
    // Tracker thread, once per loop: fires callbacks and synthesizes holds.
    void dispatch();
    // Same, with an explicit millisecond clock (tests).
    void dispatchAt(uint32_t nowMs);

  private:
    std::mutex mutex_;
    std::vector<InputEvent> queue_;
    std::vector<std::function<void()>> tasks_;
    SwitchGroupApple pads_{ 48 };
    SwitchGroupApple buttons_{ 44 };
    EncoderApple encoder_;
};
