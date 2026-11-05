#pragma once
// CoreMIDI output with audio-latency-compensated timestamps (Task 8).
//
// One MidiCoreMidi object implements both UsbMidi and UartMidi; the platform owns two of them
// (usb first, uart constructed with shareWith = &usb) and both feed one shared sender: one
// CoreMIDI client, the always-published virtual source "Tracker CE (unofficial)", the optional
// selected hardware destination, one sender thread.
//
// Enqueue (any send*): lock-free, allocation-free. A thread inside an audio block (between
// TimerApple::beginBlock/endBlock: the render thread, with the tracker thread frozen) enqueues
// {blockIndex, offsetUs, bytes} on that object's render-thread SPSC ring; offsetUs is
// TimerApple::currentSubBlockOffsetUs() inside a scheduleMidiClockUs callback, else 0. Any other
// thread (the tracker thread: UI previews) enqueues on a second SPSC ring stamped "now". Two
// rings because the frozen tracker thread may be stopped in the middle of its own enqueue.
//
// The sender thread never touches core state. It converts a block index to host time with the
// latest CoreAudioOutput cycle mapping
//   hostTime(block) = hostTimeOfFirstFrame + ((block - cycleBlock) * 128 - offsetInBlock) / 44100 s
// and adds outputLatencySeconds() + offsetUs + the user offset, then calls CoreMIDI. Events
// stamped in the past go out immediately (CoreMIDI behaviour). Before the first cycle is known,
// and for tracker-thread events, the stamp is mach_absolute_time() + latency + user offset.
//
// CoreAudioOutput's AudioConverter fallback (device not at 44.1 kHz): the converter's own delay
// is not in outputLatencySeconds(); MIDI is then late by that amount (logged once).
//
// MIDI input is out of scope: isReadAvailable() is false and the receive setters are inert.
#include "Platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class CoreAudioOutput;

class MidiCoreMidi : public UsbMidi, public UartMidi
{
  public:
    static constexpr const char* kSourceName = "Tracker CE (unofficial)";
    static constexpr size_t kRingEvents = 1024; // per ring; a full ring drops the event (counted)

    // shareWith: another MidiCoreMidi whose sender (client, source, destination, offset) to use.
    explicit MidiCoreMidi(MidiCoreMidi* shareWith = nullptr);
    ~MidiCoreMidi();
    MidiCoreMidi(const MidiCoreMidi&) = delete;
    MidiCoreMidi& operator=(const MidiCoreMidi&) = delete;

    // Hardware/virtual destinations (display names) and selection. idx < 0 deselects (the
    // virtual source still publishes). false when idx is out of range.
    static std::vector<std::string> listDestinations();
    bool selectDestination(int idx);

    // User timing offset in ms, clamped to -50..+50.
    void setUserOffsetMs(int ms);
    int userOffsetMs() const;

    // Takes the timing from `out`: installs its onCycle mapping (call BEFORE out.start(): the
    // std::function is not synchronised against the IOProc) and queries its latency from the
    // sender thread. Destroy `out` before this object or call detachOutput(out) first.
    void attachOutput(CoreAudioOutput& out);
    void detachOutput(CoreAudioOutput& out); // call while out is stopped

    uint64_t droppedEvents() const;

    // Pure timestamp math (host ticks). cycle* is the latest onCycle mapping.
    static uint64_t hostTimeFor(uint64_t cycleHostTime,
                                uint64_t cycleBlock,
                                size_t cycleOffsetInBlock,
                                uint64_t eventBlock,
                                uint32_t eventOffsetUs,
                                double latencySeconds,
                                int userOffsetMs);
    static uint64_t hostNow();

    // Test seams. Real-time: the same call the IOProc makes through attachOutput.
    void noteCycle(uint64_t hostTimeOfFirstFrame, uint64_t blockIndex, size_t offsetInBlock);
    void setLatencySourceForTest(std::function<double()> fn);
    // Replaces the CoreMIDI send; runs on the sender thread. ts is the host-time stamp.
    void setTransmitHookForTest(std::function<void(uint64_t ts, const uint8_t* bytes, int len)> fn);

    // Midi
    bool isReadAvailable() override { return false; }
    void read() override {}
    void sendRealTime(MessageType messageType) override;
    void sendControlChange(uint8_t index, uint8_t value, uint8_t channel) override;
    void sendProgramChange(uint8_t programNumber, uint8_t channel) override;
    void sendAfterTouch(uint8_t velocity, uint8_t channel) override;
    void sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel) override;
    void sendPitchBend(int value, uint8_t channel) override; // -8192..8191
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override;

    // UsbMidi / UartMidi: input is out of scope.
    void setReceiveActions(UsbMidi::MidiReceiveActions*) override {}
    void setReceiveActions(UartMidi::MidiReceiveActions*) override {}
    void setReceiveNoteOnAction(void (*)(uint8_t, uint8_t, uint8_t)) override {}
    void init() override {}

    struct Hub; // opaque

  private:
    struct Ring;
    void enqueue(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t len);

    std::shared_ptr<Hub> hub_;
    std::unique_ptr<Ring> renderRing_;
    std::unique_ptr<Ring> trackerRing_;
};
