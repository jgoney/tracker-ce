#include "InputApple.h"

#include <chrono>


bool SwitchGroupApple::isButtonPressed(int idx)
{
    return idx >= 0 && idx < count_ && state_[idx].down;
}

void SwitchGroupApple::setActionCallback(ActionType action, void (*callback)(uint8_t))
{
    switch (action)
    {
        case kPress: onPress_ = callback; break;
        case kRelease: onRelease_ = callback; break;
        case kHold: onHold_ = callback; break;
    }
}

constexpr uint16_t SwitchGroupApple::kHoldTimeStepDiff[];

void SwitchGroupApple::press(uint8_t idx, uint32_t now)
{
    if (idx >= count_ || state_[idx].down)
        return;
    state_[idx] = { true, 0, now };
    if (onPress_)
        onPress_(idx);
}

void SwitchGroupApple::release(uint8_t idx)
{
    if (idx >= count_ || !state_[idx].down)
        return;
    state_[idx].down = false;
    if (onRelease_)
        onRelease_(idx);
}

void SwitchGroupApple::updateHolds(uint32_t now)
{
    for (int i = 0; i < count_; i++)
    {
        State& s = state_[i];
        // Keys.cpp:203-214: fire when elapsed > holdTime - step diff, then advance the step
        // (clamped) and restart the interval, so a held key repeats faster and faster.
        if (s.down && (uint32_t)(now - s.holdTimeMs) >
                        (uint32_t)(holdTimeMs_ - kHoldTimeStepDiff[s.holdFactor]))
        {
            if (s.holdFactor < kHoldStepsCount - 1)
                s.holdFactor++;
            s.holdTimeMs = now;
            if (onHold_)
                onHold_((uint8_t)i);
        }
    }
}

void InputApple::post(InputEvent e)
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(e);
}

void InputApple::dispatch()
{
    dispatchAt((uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count());
}

void InputApple::dispatchAt(uint32_t now)
{
    std::vector<InputEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events.swap(queue_);
    }
    for (const InputEvent& e : events)
    {
        switch (e.kind)
        {
            case InputEvent::Pad: e.down ? pads_.press(e.index, now) : pads_.release(e.index); break;
            case InputEvent::Button:
                e.down ? buttons_.press(e.index, now) : buttons_.release(e.index);
                break;
            case InputEvent::Encoder: encoder_.change(e.delta); break;
        }
    }
    pads_.updateHolds(now);
    buttons_.updateHolds(now);
}
