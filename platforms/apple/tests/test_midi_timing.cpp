// MidiCoreMidi: timestamp math with a fake clock mapping, user offset, frozen render-thread time,
// allocation-free enqueue, and a round trip through real CoreMIDI endpoints created in-process.
#include <catch2/catch_test_macros.hpp>

#include "MidiCoreMidi.h"
#include "TimerApple.h"

#include <Block.h>
#include <CoreMIDI/CoreMIDI.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mach/mach_time.h>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

// Counting operator new: only armed on the calling thread (the render-path test).
namespace
{
thread_local bool tlCountAllocs = false;
std::atomic<int> gAllocs{ 0 };
} // namespace

void* operator new(size_t size)
{
    if (tlCountAllocs)
        gAllocs.fetch_add(1);
    void* p = std::malloc(size ? size : 1);
    if (!p)
        std::abort();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace
{
using namespace std::chrono_literals;

double ticksPerSecond()
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return 1e9 * tb.denom / tb.numer;
}

struct Captured
{
    uint64_t ts;
    std::vector<uint8_t> bytes;
};

// Collects what the sender thread transmits.
struct Capture
{
    std::mutex m;
    std::vector<Captured> got;
    void install(MidiCoreMidi& midi)
    {
        midi.setTransmitHookForTest([this](uint64_t ts, const uint8_t* b, int n) {
            std::lock_guard<std::mutex> g(m);
            got.push_back({ ts, std::vector<uint8_t>(b, b + n) });
        });
    }
    bool waitFor(size_t n, std::chrono::milliseconds timeout = 2000ms)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end)
        {
            {
                std::lock_guard<std::mutex> g(m);
                if (got.size() >= n)
                    return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }
};

MidiCoreMidi* gMidi = nullptr;
void sendClockCb()
{
    gMidi->sendRealTime(Midi::MessageType::kClock);
}

// Plays the role of AudioRuntime's render thread for one block: a clock event `delayUs` into
// block `block`, sent from inside a scheduleMidiClockUs callback.
void renderBlockWithClock(MidiCoreMidi& midi, uint64_t block, uint32_t delayUs)
{
    gMidi = &midi;
    std::thread t([&] {
        TimerApple::beginBlock(block);
        TimerApple timer;
        timer.scheduleMidiClockUs(delayUs, sendClockCb);
        TimerApple::endBlock();
    });
    t.join();
}
} // namespace

TEST_CASE("test_timestamp_math", "[midi]")
{
    const double tps = ticksPerSecond();
    const uint64_t t0 = 1000000000ull;

    SECTION("pure function: block 12, 1000 us, latency 5 ms, cycle block 10")
    {
        const uint64_t got = MidiCoreMidi::hostTimeFor(t0, 10, 0, 12, 1000, 0.005, 0);
        const double expect = t0 + (256.0 / 44100.0 + 0.005 + 0.001) * tps;
        REQUIRE(std::fabs((double)got - expect) <= 1.0);
    }
    SECTION("offsetInBlock shifts the first frame back")
    {
        // cycle's first frame is frame 40 of block 10: block 10 itself starts 40 frames earlier
        const uint64_t got = MidiCoreMidi::hostTimeFor(t0, 10, 40, 10, 0, 0.0, 0);
        const double expect = t0 - (40.0 / 44100.0) * tps;
        REQUIRE(std::fabs((double)got - expect) <= 1.0);
    }
    SECTION("an earlier block than the cycle's is earlier in time")
    {
        const uint64_t got = MidiCoreMidi::hostTimeFor(t0, 10, 0, 9, 0, 0.0, 0);
        const double expect = t0 - (128.0 / 44100.0) * tps;
        REQUIRE(std::fabs((double)got - expect) <= 1.0);
    }
    SECTION("end to end through enqueue and the sender thread")
    {
        MidiCoreMidi midi;
        Capture cap;
        cap.install(midi);
        midi.setLatencySourceForTest([] { return 0.005; });
        midi.noteCycle(t0, 10, 0);
        renderBlockWithClock(midi, 12, 1000);
        REQUIRE(cap.waitFor(1));
        REQUIRE(cap.got[0].bytes == std::vector<uint8_t>{ 0xF8 });
        const double expect = t0 + (256.0 / 44100.0 + 0.005 + 0.001) * tps;
        REQUIRE(std::fabs((double)cap.got[0].ts - expect) <= 1.0);
    }
}

TEST_CASE("test_user_offset_applied", "[midi]")
{
    const double tps = ticksPerSecond();
    const uint64_t t0 = 1000000000ull;
    const uint64_t base = MidiCoreMidi::hostTimeFor(t0, 10, 0, 11, 0, 0.002, 0);
    REQUIRE(std::fabs((double)MidiCoreMidi::hostTimeFor(t0, 10, 0, 11, 0, 0.002, 7) -
                      (double)base - 0.007 * tps) <= 1.0);
    REQUIRE(std::fabs((double)MidiCoreMidi::hostTimeFor(t0, 10, 0, 11, 0, 0.002, -7) -
                      (double)base + 0.007 * tps) <= 1.0);

    MidiCoreMidi midi;
    Capture cap;
    cap.install(midi);
    midi.setLatencySourceForTest([] { return 0.0; });
    midi.noteCycle(t0, 10, 0);
    midi.setUserOffsetMs(9);
    REQUIRE(midi.userOffsetMs() == 9);
    renderBlockWithClock(midi, 10, 0);
    REQUIRE(cap.waitFor(1));
    REQUIRE(std::fabs((double)cap.got[0].ts - (t0 + 0.009 * tps)) <= 1.0);

    midi.setUserOffsetMs(500);
    REQUIRE(midi.userOffsetMs() == 50);
    midi.setUserOffsetMs(-500);
    REQUIRE(midi.userOffsetMs() == -50);
}

TEST_CASE("tracker-thread sends are stamped now plus latency", "[midi]")
{
    const double tps = ticksPerSecond();
    MidiCoreMidi midi;
    Capture cap;
    cap.install(midi);
    midi.setLatencySourceForTest([] { return 0.010; });
    midi.noteCycle(1, 0, 0); // a mapping exists, but is irrelevant outside a block
    const uint64_t before = MidiCoreMidi::hostNow();
    midi.sendNoteOn(60, 100, 1);
    const uint64_t after = MidiCoreMidi::hostNow();
    REQUIRE(cap.waitFor(1));
    REQUIRE(cap.got[0].bytes == std::vector<uint8_t>{ 0x90, 60, 100 });
    REQUIRE((double)cap.got[0].ts >= (double)before + 0.010 * tps - 1);
    REQUIRE((double)cap.got[0].ts <= (double)after + 0.010 * tps + 1);
}

TEST_CASE("message encoding", "[midi]")
{
    MidiCoreMidi midi;
    Capture cap;
    cap.install(midi);
    midi.sendNoteOff(61, 5, 2);
    midi.sendControlChange(7, 99, 16);
    midi.sendProgramChange(12, 3);
    midi.sendAfterTouch(33, 4);
    midi.sendPolyPressure(70, 44, 5);
    midi.sendPitchBend(-8192, 1);
    midi.sendPitchBend(8191, 1);
    midi.sendPitchBend(0, 1);
    midi.sendRealTime(Midi::MessageType::kStart);
    REQUIRE(cap.waitFor(9));
    using V = std::vector<uint8_t>;
    REQUIRE(cap.got[0].bytes == V{ 0x81, 61, 5 });
    REQUIRE(cap.got[1].bytes == V{ 0xBF, 7, 99 });
    REQUIRE(cap.got[2].bytes == V{ 0xC2, 12 });
    REQUIRE(cap.got[3].bytes == V{ 0xD3, 33 });
    REQUIRE(cap.got[4].bytes == V{ 0xA4, 70, 44 });
    REQUIRE(cap.got[5].bytes == V{ 0xE0, 0, 0 });
    REQUIRE(cap.got[6].bytes == V{ 0xE0, 0x7F, 0x7F });
    REQUIRE(cap.got[7].bytes == V{ 0xE0, 0, 0x40 });
    REQUIRE(cap.got[8].bytes == V{ 0xFA });
    REQUIRE_FALSE(midi.isReadAvailable());
}

TEST_CASE("test_render_thread_time_frozen", "[midi]")
{
    TimerApple timer;
    const uint64_t block = 777;
    uint32_t a = 0, b = 0, ms = 0;
    bool inside = false;
    uint32_t otherThreadSees = 0;
    std::thread render([&] {
        TimerApple::beginBlock(block);
        inside = TimerApple::inBlock() && TimerApple::currentBlockIndex() == block;
        a = timer.micros();
        const auto end = std::chrono::steady_clock::now() + 100us;
        while (std::chrono::steady_clock::now() < end)
        {
        }
        b = timer.micros();
        ms = timer.millis();
        // the tracker thread keeps seeing real time meanwhile
        std::thread other([&] { otherThreadSees = timer.micros(); });
        other.join();
        TimerApple::endBlock();
    });
    render.join();
    const uint32_t nominal = (uint32_t)TimerApple::blockStartUs(block);
    REQUIRE(inside);
    REQUIRE(a == b);
    REQUIRE(a == nominal);
    REQUIRE(nominal == (uint32_t)(777ull * 128 * 1000000 / 44100));
    REQUIRE(ms == nominal / 1000);
    REQUIRE(otherThreadSees != nominal);
    REQUIRE_FALSE(TimerApple::inBlock());
    const uint32_t r0 = timer.micros();
    std::this_thread::sleep_for(2ms);
    REQUIRE(timer.micros() > r0);
}

TEST_CASE("test_render_thread_send_does_not_lock", "[midi]")
{
    MidiCoreMidi usb;
    MidiCoreMidi uart(&usb);
    Capture cap;
    cap.install(usb);
    gMidi = &usb;
    int allocs = -1;
    std::thread render([&] {
        TimerApple::beginBlock(5);
        TimerApple timer;
        gAllocs = 0;
        tlCountAllocs = true;
        for (int i = 0; i < 200; i++)
        {
            usb.sendNoteOn(60, 100, 1);
            uart.sendNoteOff(60, 0, 1);
            usb.sendRealTime(Midi::MessageType::kClock);
            timer.scheduleMidiClockUs(500, sendClockCb);
        }
        tlCountAllocs = false;
        allocs = gAllocs.load();
        TimerApple::endBlock();
    });
    render.join();
    REQUIRE(allocs == 0);
    // the tracker-thread path too
    gAllocs = 0;
    tlCountAllocs = true;
    for (int i = 0; i < 100; i++)
        usb.sendNoteOn(60, 100, 1);
    tlCountAllocs = false;
    REQUIRE(gAllocs.load() == 0);
}

TEST_CASE("sender is shared by both instances and a full ring drops, never blocks", "[midi]")
{
    MidiCoreMidi usb;
    MidiCoreMidi uart(&usb);
    Capture cap;
    cap.install(usb); // hook lives on the shared hub
    uart.sendNoteOn(1, 1, 1);
    usb.sendNoteOn(2, 2, 1);
    REQUIRE(cap.waitFor(2));
}

// Real CoreMIDI: an in-process virtual destination and a client listening to our source.
TEST_CASE("CoreMIDI round trip: virtual destination and published source", "[midi][coremidi]")
{
    struct Rx
    {
        std::mutex m;
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> events;
    } destRx, srcRx;

    auto collect = [](Rx* rx) {
        return (MIDIReceiveBlock)Block_copy(^(const MIDIEventList* list, void*) {
          const MIDIEventPacket* p = &list->packet[0];
          for (UInt32 i = 0; i < list->numPackets; i++)
          {
              for (UInt32 w = 0; w < p->wordCount; w++)
              {
                  const uint32_t word = p->words[w];
                  std::vector<uint8_t> bytes;
                  if ((word >> 28) == 0x1)
                      bytes = { (uint8_t)(word >> 16) };
                  else if ((word >> 28) == 0x2)
                      bytes = { (uint8_t)(word >> 16), (uint8_t)(word >> 8), (uint8_t)word };
                  std::lock_guard<std::mutex> g(rx->m);
                  rx->events.push_back({ p->timeStamp, bytes });
              }
              p = MIDIEventPacketNext(p);
          }
        });
    };

    MIDIClientRef client = 0;
    REQUIRE(MIDIClientCreate(CFSTR("test client"), nullptr, nullptr, &client) == noErr);
    MIDIEndpointRef dest = 0;
    auto destBlock = collect(&destRx);
    REQUIRE(MIDIDestinationCreateWithProtocol(client,
                                              CFSTR("TrackerCE test destination"),
                                              kMIDIProtocol_1_0,
                                              &dest,
                                              destBlock) == noErr);

    MidiCoreMidi midi;
    midi.setLatencySourceForTest([] { return 0.0; });

    // our virtual source is published
    MIDIEndpointRef ourSource = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++)
    {
        MIDIEndpointRef s = MIDIGetSource(i);
        CFStringRef name = nullptr;
        MIDIObjectGetStringProperty(s, kMIDIPropertyName, &name);
        if (name)
        {
            char buf[128];
            CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
            CFRelease(name);
            if (std::string(buf) == MidiCoreMidi::kSourceName)
                ourSource = s;
        }
    }
    REQUIRE(ourSource != 0);

    MIDIPortRef inPort = 0;
    auto srcBlock = collect(&srcRx);
    REQUIRE(MIDIInputPortCreateWithProtocol(client, CFSTR("in"), kMIDIProtocol_1_0, &inPort, srcBlock) == noErr);
    REQUIRE(MIDIPortConnectSource(inPort, ourSource, nullptr) == noErr);

    int idx = -1;
    auto names = MidiCoreMidi::listDestinations();
    for (size_t i = 0; i < names.size(); i++)
        if (names[i].find("TrackerCE test destination") != std::string::npos)
            idx = (int)i;
    REQUIRE(idx >= 0);
    REQUIRE_FALSE(midi.selectDestination((int)names.size() + 5));
    REQUIRE(midi.selectDestination(idx));

    midi.sendNoteOn(64, 90, 3);
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    auto have = [&](Rx& rx) {
        std::lock_guard<std::mutex> g(rx.m);
        return !rx.events.empty();
    };
    while (std::chrono::steady_clock::now() < deadline && !(have(destRx) && have(srcRx)))
        std::this_thread::sleep_for(2ms);
    REQUIRE(have(destRx));
    REQUIRE(have(srcRx));
    REQUIRE(destRx.events[0].second == std::vector<uint8_t>{ 0x92, 64, 90 });
    REQUIRE(srcRx.events[0].second == std::vector<uint8_t>{ 0x92, 64, 90 });

    REQUIRE(midi.selectDestination(-1));
    MIDIPortDispose(inPort);
    MIDIEndpointDispose(dest);
    MIDIClientDispose(client);
}
