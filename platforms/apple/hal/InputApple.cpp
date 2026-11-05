#include "InputApple.h"

using Clock = std::chrono::steady_clock;

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

void SwitchGroupApple::press(uint8_t idx, Clock::time_point now)
{
    if (idx >= count_ || state_[idx].down)
        return;
    state_[idx] = { true, false, now };
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

void SwitchGroupApple::updateHolds(Clock::time_point now)
{
    for (int i = 0; i < count_; i++)
    {
        State& s = state_[i];
        if (s.down && !s.holdFired &&
            now - s.since >= std::chrono::milliseconds(holdTimeMs_))
        {
            s.holdFired = true;
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
    std::vector<InputEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events.swap(queue_);
    }
    const auto now = Clock::now();
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
