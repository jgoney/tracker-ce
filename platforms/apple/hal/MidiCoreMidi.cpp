#include "MidiCoreMidi.h"

#include "CoreAudioOutput.h"
#include "TimerApple.h"

#include <CoreMIDI/CoreMIDI.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mutex>
#include <pthread.h>
#include <thread>

namespace
{
constexpr double kSampleRate = 44100.0;
constexpr uint64_t kBlockFrames = 128;

double ticksPerSecond()
{
    static const double v = [] {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        return 1e9 * (double)tb.denom / (double)tb.numer;
    }();
    return v;
}

uint64_t addSeconds(uint64_t base, double seconds)
{
    const double delta = seconds * ticksPerSecond();
    if (delta < 0 && (double)base < -delta)
        return 0;
    return (uint64_t)((double)base + std::llround(delta));
}

struct Event
{
    uint64_t block;
    uint64_t enqueuedHost; // tracker-thread events: when they were sent
    uint32_t offsetUs;
    uint8_t bytes[3];
    uint8_t len;
    bool inBlock;
};

std::string cfToString(CFStringRef s)
{
    if (!s)
        return {};
    char buf[256];
    if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8))
        buf[0] = 0;
    return buf;
}
} // namespace

// SPSC ring: the producer is one thread (render or tracker), the consumer the sender thread.
struct MidiCoreMidi::Ring
{
    static_assert((MidiCoreMidi::kRingEvents & (MidiCoreMidi::kRingEvents - 1)) == 0, "power of two");
    Event buf[MidiCoreMidi::kRingEvents];
    std::atomic<uint32_t> head{ 0 }; // next write
    std::atomic<uint32_t> tail{ 0 }; // next read

    bool push(const Event& e)
    {
        const uint32_t h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= MidiCoreMidi::kRingEvents)
            return false;
        buf[h % MidiCoreMidi::kRingEvents] = e;
        head.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(Event& e)
    {
        const uint32_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire))
            return false;
        e = buf[t % MidiCoreMidi::kRingEvents];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
};

// Everything the two MidiCoreMidi objects share. The sender thread only reads its own queues
// and the atomics below; it never touches core state.
struct MidiCoreMidi::Hub
{
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    MIDIPortRef outPort = 0;
    std::atomic<MIDIEndpointRef> dest{ 0 };
    std::atomic<int> userMs{ 0 };
    std::atomic<uint64_t> dropped{ 0 };

    // Latest onCycle mapping, written by the IO thread, read by the sender (seqlock).
    std::atomic<uint32_t> seq{ 0 };
    std::atomic<uint64_t> cycleHost{ 0 }, cycleBlock{ 0 }, cycleOffset{ 0 };
    std::atomic<bool> haveCycle{ false };

    std::mutex ringsMutex; // sender drain vs ring (de)registration; enqueuers never take it
    std::vector<Ring*> renderRings;  // drained before every tracker-thread event (see run)
    std::vector<Ring*> trackerRings;
    // Largest stamp sent for a render-thread event. Tracker-thread events are stamped >= this so
    // the output is a monotonic merge: a stop/panic/preview release can never overtake a note
    // already rendered into a queued block (CoreMIDI delivers by timestamp).
    uint64_t lastRenderStamp = 0;

    std::mutex ctlMutex; // sender vs attach/detach/test hooks (never taken on the enqueue path)
    CoreAudioOutput* out = nullptr;
    std::function<double()> latencyOverride;
    std::function<void(uint64_t, const uint8_t*, int)> transmitHook;
    bool converterLogged = false;

    semaphore_t sem = MACH_PORT_NULL;
    std::atomic<bool> stopping{ false };
    std::thread sender;

    Hub()
    {
        renderRings.reserve(8);
        trackerRings.reserve(8);
        semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0);
        if (MIDIClientCreate(CFSTR("Tracker CE"), nullptr, nullptr, &client) != noErr)
        {
            std::fprintf(stderr, "MidiCoreMidi: MIDIClientCreate failed; MIDI output disabled\n");
            client = 0;
        }
        else
        {
            if (MIDISourceCreateWithProtocol(client,
                                             CFSTR("Tracker CE (unofficial)"),
                                             kMIDIProtocol_1_0,
                                             &source) != noErr)
                source = 0;
            if (MIDIOutputPortCreate(client, CFSTR("Tracker CE out"), &outPort) != noErr)
                outPort = 0;
        }
        sender = std::thread([this] { run(); });
    }

    ~Hub()
    {
        stopping.store(true);
        semaphore_signal(sem);
        sender.join();
        if (source)
            MIDIEndpointDispose(source);
        if (client)
            MIDIClientDispose(client);
        semaphore_destroy(mach_task_self(), sem);
    }

    void noteCycle(uint64_t host, uint64_t block, size_t offset)
    {
        const uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        cycleHost.store(host, std::memory_order_relaxed);
        cycleBlock.store(block, std::memory_order_relaxed);
        cycleOffset.store(offset, std::memory_order_relaxed);
        seq.store(s + 2, std::memory_order_release);
        haveCycle.store(true, std::memory_order_release);
    }

    bool readCycle(uint64_t& host, uint64_t& block, size_t& offset) const
    {
        if (!haveCycle.load(std::memory_order_acquire))
            return false;
        for (;;)
        {
            const uint32_t s0 = seq.load(std::memory_order_acquire);
            if (s0 & 1)
                continue;
            host = cycleHost.load(std::memory_order_relaxed);
            block = cycleBlock.load(std::memory_order_relaxed);
            offset = (size_t)cycleOffset.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s0)
                return true;
        }
    }

    double latencySeconds()
    {
        std::lock_guard<std::mutex> g(ctlMutex);
        if (latencyOverride)
            return latencyOverride();
        if (!out)
            return 0.0;
        if (out->usingConverter() && !converterLogged)
        {
            converterLogged = true;
            std::fprintf(stderr,
                         "MidiCoreMidi: audio output runs through a sample-rate converter; MIDI "
                         "timing is not compensated for the converter's delay\n");
        }
        return out->outputLatencySeconds();
    }

    void transmit(uint64_t ts, const Event& e)
    {
        {
            std::lock_guard<std::mutex> g(ctlMutex);
            if (transmitHook)
            {
                transmitHook(ts, e.bytes, e.len);
                return;
            }
        }
        uint32_t word;
        if (e.len == 1)
            word = (0x1u << 28) | ((uint32_t)e.bytes[0] << 16);
        else
            word = (0x2u << 28) | ((uint32_t)e.bytes[0] << 16) | ((uint32_t)e.bytes[1] << 8) |
                   (uint32_t)(e.len > 2 ? e.bytes[2] : 0);
        alignas(8) Byte storage[256];
        auto* list = (MIDIEventList*)storage;
        MIDIEventPacket* p = MIDIEventListInit(list, kMIDIProtocol_1_0);
        p = MIDIEventListAdd(list, sizeof storage, p, ts, 1, &word);
        if (!p)
            return;
        if (source)
            MIDIReceivedEventList(source, list);
        const MIDIEndpointRef d = dest.load(std::memory_order_acquire);
        if (d && outPort)
            MIDISendEventList(outPort, d, list);
    }

    void handle(const Event& e, double latency)
    {
        const int user = userMs.load(std::memory_order_relaxed);
        uint64_t ts;
        uint64_t ch, cb;
        size_t co;
        if (e.inBlock && readCycle(ch, cb, co))
            ts = MidiCoreMidi::hostTimeFor(ch, cb, co, e.block, e.offsetUs, latency, user);
        else
            ts = addSeconds(e.inBlock ? MidiCoreMidi::hostNow() : e.enqueuedHost,
                            latency + user * 1e-3);
        if (e.inBlock)
            lastRenderStamp = std::max(lastRenderStamp, ts);
        else
            ts = std::max(ts, lastRenderStamp);
        transmit(ts, e);
    }

    // Caller holds ringsMutex. Render rings first, then each tracker event after re-draining the
    // render rings: every render event enqueued before a tracker event is then already sent.
    bool drainOnce()
    {
        bool any = false;
        double latency = 0;
        bool haveLatency = false;
        auto latencyNow = [&] {
            if (!haveLatency)
            {
                latency = latencySeconds();
                haveLatency = true;
            }
            return latency;
        };
        auto drainRender = [&] {
            Event e;
            for (Ring* r : renderRings)
                while (r->pop(e))
                {
                    any = true;
                    handle(e, latencyNow());
                }
        };
        drainRender();
        for (Ring* r : trackerRings)
        {
            Event e;
            while (r->pop(e))
            {
                any = true;
                drainRender();
                handle(e, latencyNow());
            }
        }
        return any;
    }

    void run()
    {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        while (!stopping.load(std::memory_order_acquire))
        {
            semaphore_timedwait(sem, mach_timespec_t{ 0, 100 * 1000 * 1000 });
            for (;;)
            {
                std::lock_guard<std::mutex> g(ringsMutex);
                if (stopping.load(std::memory_order_relaxed) || !drainOnce())
                    break;
            }
        }
    }
};

MidiCoreMidi::MidiCoreMidi(MidiCoreMidi* shareWith)
    : hub_(shareWith ? shareWith->hub_ : std::make_shared<Hub>()),
      renderRing_(new Ring),
      trackerRing_(new Ring)
{
    std::lock_guard<std::mutex> g(hub_->ringsMutex);
    hub_->renderRings.push_back(renderRing_.get());
    hub_->trackerRings.push_back(trackerRing_.get());
}

MidiCoreMidi::~MidiCoreMidi()
{
    std::lock_guard<std::mutex> g(hub_->ringsMutex);
    auto drop = [](std::vector<Ring*>& v, Ring* r) { v.erase(std::remove(v.begin(), v.end(), r), v.end()); };
    drop(hub_->renderRings, renderRing_.get());
    drop(hub_->trackerRings, trackerRing_.get());
}

std::vector<std::string> MidiCoreMidi::listDestinations()
{
    std::vector<std::string> names;
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; i++)
    {
        CFStringRef s = nullptr;
        MIDIObjectGetStringProperty(MIDIGetDestination(i), kMIDIPropertyDisplayName, &s);
        names.push_back(cfToString(s));
        if (s)
            CFRelease(s);
    }
    return names;
}

bool MidiCoreMidi::selectDestination(int idx)
{
    if (idx < 0)
    {
        hub_->dest.store(0, std::memory_order_release);
        return true;
    }
    if ((ItemCount)idx >= MIDIGetNumberOfDestinations())
        return false;
    const MIDIEndpointRef d = MIDIGetDestination((ItemCount)idx);
    if (!d)
        return false;
    hub_->dest.store(d, std::memory_order_release);
    return true;
}

void MidiCoreMidi::setUserOffsetMs(int ms)
{
    hub_->userMs.store(std::clamp(ms, -50, 50), std::memory_order_relaxed);
}

int MidiCoreMidi::userOffsetMs() const
{
    return hub_->userMs.load(std::memory_order_relaxed);
}

void MidiCoreMidi::attachOutput(CoreAudioOutput& out)
{
    Hub* hub = hub_.get();
    {
        std::lock_guard<std::mutex> g(hub->ctlMutex);
        hub->out = &out;
        hub->converterLogged = false;
    }
    // Written unsynchronised against the IOProc: attach before out.start().
    out.setOnCycle([hub](uint64_t host, uint64_t block, size_t offset) { hub->noteCycle(host, block, offset); });
}

void MidiCoreMidi::detachOutput(CoreAudioOutput& out)
{
    out.setOnCycle(nullptr);
    std::lock_guard<std::mutex> g(hub_->ctlMutex);
    if (hub_->out == &out)
        hub_->out = nullptr;
}

uint64_t MidiCoreMidi::droppedEvents() const
{
    return hub_->dropped.load(std::memory_order_relaxed);
}

uint64_t MidiCoreMidi::hostNow()
{
    return mach_absolute_time();
}

uint64_t MidiCoreMidi::hostTimeFor(uint64_t cycleHostTime,
                                   uint64_t cycleBlock,
                                   size_t cycleOffsetInBlock,
                                   uint64_t eventBlock,
                                   uint32_t eventOffsetUs,
                                   double latencySeconds,
                                   int userOffsetMs)
{
    const int64_t frames = ((int64_t)eventBlock - (int64_t)cycleBlock) * (int64_t)kBlockFrames -
                           (int64_t)cycleOffsetInBlock;
    const double seconds = (double)frames / kSampleRate + latencySeconds +
                           (double)eventOffsetUs * 1e-6 + (double)userOffsetMs * 1e-3;
    return addSeconds(cycleHostTime, seconds);
}

void MidiCoreMidi::noteCycle(uint64_t hostTimeOfFirstFrame, uint64_t blockIndex, size_t offsetInBlock)
{
    hub_->noteCycle(hostTimeOfFirstFrame, blockIndex, offsetInBlock);
}

void MidiCoreMidi::setLatencySourceForTest(std::function<double()> fn)
{
    std::lock_guard<std::mutex> g(hub_->ctlMutex);
    hub_->latencyOverride = std::move(fn);
}

void MidiCoreMidi::setTransmitHookForTest(std::function<void(uint64_t, const uint8_t*, int)> fn)
{
    std::lock_guard<std::mutex> g(hub_->ctlMutex);
    hub_->transmitHook = std::move(fn);
}

void MidiCoreMidi::enqueue(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t len)
{
    Event e;
    e.bytes[0] = b0;
    e.bytes[1] = b1;
    e.bytes[2] = b2;
    e.len = len;
    e.inBlock = TimerApple::inBlock();
    e.block = e.inBlock ? TimerApple::currentBlockIndex() : 0;
    e.offsetUs = e.inBlock ? TimerApple::currentSubBlockOffsetUs() : 0;
    e.enqueuedHost = e.inBlock ? 0 : mach_absolute_time();
    Ring& r = e.inBlock ? *renderRing_ : *trackerRing_;
    if (!r.push(e))
        hub_->dropped.fetch_add(1, std::memory_order_relaxed);
    semaphore_signal(hub_->sem);
}

namespace
{
inline uint8_t status(uint8_t base, uint8_t channel)
{
    return (uint8_t)(base | ((channel - 1) & 0x0F)); // channels are 1..16 in the core
}
inline uint8_t d7(uint8_t v)
{
    return v & 0x7F;
}
} // namespace

void MidiCoreMidi::sendRealTime(MessageType t)
{
    enqueue((uint8_t)t, 0, 0, 1);
}
void MidiCoreMidi::sendControlChange(uint8_t index, uint8_t value, uint8_t channel)
{
    enqueue(status(0xB0, channel), d7(index), d7(value), 3);
}
void MidiCoreMidi::sendProgramChange(uint8_t programNumber, uint8_t channel)
{
    enqueue(status(0xC0, channel), d7(programNumber), 0, 2);
}
void MidiCoreMidi::sendAfterTouch(uint8_t velocity, uint8_t channel)
{
    enqueue(status(0xD0, channel), d7(velocity), 0, 2);
}
void MidiCoreMidi::sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel)
{
    enqueue(status(0xA0, channel), d7(note), d7(value), 3);
}
void MidiCoreMidi::sendPitchBend(int value, uint8_t channel)
{
    const int v = std::clamp(value, -8192, 8191) + 8192;
    enqueue(status(0xE0, channel), (uint8_t)(v & 0x7F), (uint8_t)(v >> 7), 3);
}
void MidiCoreMidi::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel)
{
    enqueue(status(0x90, channel), d7(note), d7(velocity), 3);
}
void MidiCoreMidi::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel)
{
    enqueue(status(0x80, channel), d7(note), d7(velocity), 3);
}
