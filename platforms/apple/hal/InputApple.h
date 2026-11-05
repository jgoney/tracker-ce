#pragma once
#include "Platform.h"

#include <array>
#include <chrono>
#include <cstdint>
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

    void press(uint8_t idx, std::chrono::steady_clock::time_point now);
    void release(uint8_t idx);
    void updateHolds(std::chrono::steady_clock::time_point now);

  private:
    struct State
    {
        bool down = false;
        bool holdFired = false;
        std::chrono::steady_clock::time_point since;
    };
    std::array<State, kMaxSwitches> state_{};
    void (*onPress_)(uint8_t) = nullptr;
    void (*onRelease_)(uint8_t) = nullptr;
    void (*onHold_)(uint8_t) = nullptr;
    int holdTimeMs_ = 500;
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
    // Tracker thread, once per loop: fires callbacks and synthesizes holds.
    void dispatch();

  private:
    std::mutex mutex_;
    std::vector<InputEvent> queue_;
    SwitchGroupApple pads_{ 48 };
    SwitchGroupApple buttons_{ 44 };
    EncoderApple encoder_;
};
