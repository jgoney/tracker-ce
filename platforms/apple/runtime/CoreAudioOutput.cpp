#include "CoreAudioOutput.h"

#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dispatch/dispatch.h>
#include <mutex>
#include <os/workgroup.h>
#include <set>
#include <thread>

namespace
{
using namespace std::chrono_literals;
constexpr auto kOpTimeout = 2000ms;
constexpr UInt32 kWantedBufferFrames = 256; // ring is 4 blocks (512 frames): keep well inside it
constexpr uint64_t kStallNs = 1000000000ull; // running with no IOProc cycle for this long = dead

uint64_t nowNs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

template <typename T>
bool getProp(AudioObjectID obj,
             AudioObjectPropertySelector sel,
             T& out,
             AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    AudioObjectPropertyAddress a{ sel, scope, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(T);
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &sz, &out) == noErr;
}

uint32_t outputChannels(AudioDeviceID dev)
{
    AudioObjectPropertyAddress a{ kAudioDevicePropertyStreamConfiguration,
                                  kAudioObjectPropertyScopeOutput,
                                  kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr ||
        sz < sizeof(AudioBufferList))
        return 0;
    std::vector<uint8_t> buf(sz);
    auto* abl = (AudioBufferList*)buf.data();
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, abl) != noErr)
        return 0;
    uint32_t n = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; i++)
        n += abl->mBuffers[i].mNumberChannels;
    return n;
}

std::string deviceNameOf(AudioDeviceID dev)
{
    CFStringRef s = nullptr;
    AudioObjectPropertyAddress a{ kAudioObjectPropertyName,
                                  kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(s);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &s) != noErr || !s)
        return {};
    char buf[256] = {};
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}

double nominalRate(AudioDeviceID dev)
{
    Float64 r = 0;
    return getProp(dev, kAudioDevicePropertyNominalSampleRate, r) ? r : 0;
}

bool rateSupported(AudioDeviceID dev, double rate)
{
    AudioObjectPropertyAddress a{ kAudioDevicePropertyAvailableNominalSampleRates,
                                  kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || sz == 0)
        return true; // unknown: just try
    std::vector<AudioValueRange> v(sz / sizeof(AudioValueRange));
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, v.data()) != noErr)
        return true;
    for (auto& r : v)
        if (rate >= r.mMinimum - 0.5 && rate <= r.mMaximum + 0.5)
            return true;
    return false;
}

bool deviceAliveReal(AudioDeviceID dev)
{
    UInt32 alive = 0;
    return getProp(dev, kAudioDevicePropertyDeviceIsAlive, alive) && alive;
}

// ---- worker thread for HAL calls that can hang ----------------------------------------------

struct Worker
{
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    bool quit = false;

    static std::shared_ptr<Worker> make()
    {
        auto w = std::make_shared<Worker>();
        std::thread([w] { w->loop(); }).detach(); // detached: it may wedge in a HAL call forever
        return w;
    }
    void post(std::function<void()> f)
    {
        {
            std::lock_guard<std::mutex> g(m);
            q.push_back(std::move(f));
        }
        cv.notify_one();
    }
    void stop()
    {
        {
            std::lock_guard<std::mutex> g(m);
            quit = true;
        }
        cv.notify_one();
    }
    void loop()
    {
        for (;;)
        {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> g(m);
                cv.wait(g, [&] { return !q.empty() || quit; });
                if (quit)
                    return; // a wedged-then-released worker drops its stale queue
                f = std::move(q.front());
                q.pop_front();
            }
            f();
        }
    }
};

// ---- state shared between the IOProc, the control paths and abandoned workers ----------------

struct Shared
{
    std::atomic<uint64_t> cycles{ 0 };
    std::atomic<uint64_t> reopens{ 0 };
    std::atomic<uint64_t> lastCycleNs{ 0 };
    std::atomic<bool> freezeClock{ false }; // test: the IOProc stops reporting liveness
    std::function<void(uint64_t, uint64_t, size_t)> onCycle;
};

enum class Event
{
    DeviceDead,     // kAudioDevicePropertyDeviceIsAlive
    RateChanged,    // kAudioDevicePropertyNominalSampleRate
    DefaultChanged, // default output device
    DevicesChanged, // device list
    OpenFailed,     // open() failed on the requested device: fall back
    Watchdog        // timer tick
};
} // namespace

namespace
{
// One opened device: AUHAL unit + optional converter + IOProc scratch. Built and torn down on a
// worker; the IOProc holds a raw pointer, valid until teardown() returns.
struct Dev
{
    AudioDeviceID id = kAudioObjectUnknown;
    std::string name;
    AudioUnit unit = nullptr;
    AudioConverterRef conv = nullptr;
    std::atomic<AudioRuntime*> rt{ nullptr };
    std::shared_ptr<Shared> sh;
    uint32_t channels = 0, lc = 0, rc = 1, maxFrames = 0, bufFrames = 0;
    double rate = 44100, latency = 0;
    bool converter = false;
    std::vector<float> srcL, srcR, outL, outR;
    size_t srcCap = 0;
    void* wg = nullptr;
    std::atomic<bool> started{ false };
    bool torn = false;
    void* listenCtx = nullptr; // weak_ptr<Impl>* (deliberately leaked: see addListeners)

    void removeListeners();
    void teardown()
    {
        if (torn)
            return;
        torn = true;
        rt.store(nullptr);
        removeListeners();
        if (unit)
        {
            AudioOutputUnitStop(unit); // returns after the last IOProc cycle
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            unit = nullptr;
        }
        if (conv)
        {
            AudioConverterDispose(conv);
            conv = nullptr;
        }
        started = false;
    }
    ~Dev() { teardown(); }
};
} // namespace

struct CoreAudioOutput::Impl : std::enable_shared_from_this<CoreAudioOutput::Impl>
{
    enum class Err
    {
        Ok,
        Failed,
        BadChannels
    };
    struct Msg
    {
        std::shared_ptr<Impl> self;
        Event ev;
        bool isStatus;
        Status status;
        std::string name;
    };
    using TestHooks = CoreAudioOutputTestHooks;

    AudioRuntime& rt;
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    dispatch_queue_t queue = nullptr;
    dispatch_source_t timer = nullptr;
    std::weak_ptr<Impl>* sysCtx = nullptr; // leaked: see addSystemListeners

    std::mutex mtx; // small state only, never held across a HAL call or a wait
    std::shared_ptr<Worker> worker;
    std::shared_ptr<Dev> cur;
    bool running = false; // intent
    bool wantOpen = false;
    bool following = false;
    bool dying = false;
    bool sysListening = false;
    uint8_t lc = 0, rc = 1;
    std::set<AudioDeviceID> unresponsive;
    std::function<void(Status, std::string)> onStatus;
    TestHooks hooks;

    explicit Impl(AudioRuntime& r) : rt(r)
    {
        queue = dispatch_queue_create("trackerce.coreaudio.control", DISPATCH_QUEUE_SERIAL);
    }
    ~Impl()
    {
        if (timer)
            dispatch_release(timer);
        dispatch_release(queue);
    }

    // -- queue plumbing
    static void runMsg(void* p)
    {
        std::unique_ptr<Msg> m((Msg*)p);
        if (m->isStatus)
        {
            std::function<void(Status, std::string)> cb;
            {
                std::lock_guard<std::mutex> g(m->self->mtx);
                cb = m->self->onStatus;
            }
            if (cb)
                cb(m->status, m->name);
        }
        else
            m->self->handleEvent(m->ev);
    }
    void postEvent(Event ev)
    {
        dispatch_async_f(queue, new Msg{ shared_from_this(), ev, false, Status::Failed, {} }, &runMsg);
    }
    void postStatus(Status s, std::string name)
    {
        dispatch_async_f(queue, new Msg{ shared_from_this(), Event::Watchdog, true, s, std::move(name) }, &runMsg);
    }
    void startTimer()
    {
        timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        auto* w = new std::weak_ptr<Impl>(shared_from_this());
        dispatch_set_context(timer, w);
        dispatch_source_set_event_handler_f(timer, [](void* p)
        {
            if (auto s = ((std::weak_ptr<Impl>*)p)->lock())
                s->handleEvent(Event::Watchdog);
        });
        dispatch_source_set_cancel_handler_f(timer, [](void* p) { delete (std::weak_ptr<Impl>*)p; });
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), 250 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
        dispatch_resume(timer);
    }

    // -- hooks
    bool alive(AudioDeviceID id)
    {
        TestHooks h;
        {
            std::lock_guard<std::mutex> g(mtx);
            h = hooks;
        }
        return h.alive ? h.alive(id) : deviceAliveReal(id);
    }
    AudioDeviceID defaultDev()
    {
        std::function<AudioDeviceID()> f;
        {
            std::lock_guard<std::mutex> g(mtx);
            f = hooks.defaultDevice;
        }
        return f ? f() : CoreAudioOutput::defaultOutputDevice();
    }
    AudioDeviceID builtinDev()
    {
        std::function<AudioDeviceID()> f;
        {
            std::lock_guard<std::mutex> g(mtx);
            f = hooks.builtinDevice;
        }
        return f ? f() : CoreAudioOutput::builtInOutputDevice();
    }
    bool isUnresponsive(AudioDeviceID id)
    {
        std::lock_guard<std::mutex> g(mtx);
        return unresponsive.count(id) != 0;
    }

    // -- run `f` on the open worker, waiting at most kOpTimeout. `cleanup` runs on the worker if
    // the caller already gave up (so a late result does not leak a device).
    bool runTimed(std::function<void()> f, std::function<void()> cleanup = {})
    {
        struct St
        {
            std::mutex m;
            std::condition_variable cv;
            bool done = false, abandoned = false;
        };
        auto st = std::make_shared<St>();
        std::shared_ptr<Worker> w;
        {
            std::lock_guard<std::mutex> g(mtx);
            if (!worker)
                worker = Worker::make();
            w = worker;
        }
        w->post([st, f, cleanup, keep = shared_from_this()]
        {
            {
                std::lock_guard<std::mutex> g(st->m);
                if (st->abandoned)
                    return;
            }
            f();
            std::lock_guard<std::mutex> g(st->m);
            if (st->abandoned)
            {
                if (cleanup)
                    cleanup();
                return;
            }
            st->done = true;
            st->cv.notify_all();
        });
        std::unique_lock<std::mutex> g(st->m);
        if (st->cv.wait_for(g, kOpTimeout, [&] { return st->done; }))
            return true;
        st->abandoned = true;
        g.unlock();
        std::lock_guard<std::mutex> g2(mtx);
        if (worker == w) // the next call gets a fresh worker; this one is abandoned
            worker.reset();
        w->stop();
        return false;
    }

    void markUnresponsive(AudioDeviceID id)
    {
        std::lock_guard<std::mutex> g(mtx);
        unresponsive.insert(id);
    }

    // -- building a device (worker thread)
    static OSStatus renderCb(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);
    static OSStatus converterInput(AudioConverterRef, UInt32*, AudioBufferList*, AudioStreamPacketDescription**, void*);
    static OSStatus propertyCb(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*);

    Err build(AudioDeviceID id, uint8_t l, uint8_t r, bool requestRate, std::shared_ptr<Dev>& out);
    void addListeners(Dev& d);
    void addSystemListeners();
    void removeSystemListeners();

    // -- control (caller threads and the control queue; never hold mtx across these)
    // Opens `id` (bounded by the timeout). null on failure.
    std::shared_ptr<Dev> tryOpen(AudioDeviceID id, uint8_t l, uint8_t r, bool requestRate, Err& err);
    void install(std::shared_ptr<Dev> d, bool follow);
    void teardownCur();
    void syncRunState();
    void fallback(AudioDeviceID avoid);
    void handleEvent(Event ev);
    void reopenSame();
};

void Dev::removeListeners()
{
    if (!listenCtx)
        return;
    AudioObjectPropertyAddress alive{ kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress rate{ kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectRemovePropertyListener(id, &alive, &CoreAudioOutput::Impl::propertyCb, listenCtx);
    AudioObjectRemovePropertyListener(id, &rate, &CoreAudioOutput::Impl::propertyCb, listenCtx);
    listenCtx = nullptr; // the context itself stays leaked: an in-flight callback may read it
}

// ---- discovery ------------------------------------------------------------------------------

std::vector<CoreAudioOutput::DeviceInfo> CoreAudioOutput::listOutputDevices()
{
    std::vector<DeviceInfo> out;
    AudioObjectPropertyAddress a{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr)
        return out;
    std::vector<AudioDeviceID> ids(sz / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) != noErr)
        return out;
    for (AudioDeviceID id : ids)
    {
        const uint32_t ch = outputChannels(id);
        if (ch > 0)
            out.push_back({ id, deviceNameOf(id), ch });
    }
    return out;
}

AudioDeviceID CoreAudioOutput::defaultOutputDevice()
{
    AudioDeviceID id = kAudioObjectUnknown;
    if (!getProp(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, id))
        return kAudioObjectUnknown;
    return id;
}

AudioDeviceID CoreAudioOutput::builtInOutputDevice()
{
    for (auto& d : listOutputDevices())
    {
        UInt32 t = 0;
        if (getProp(d.id, kAudioDevicePropertyTransportType, t) && t == kAudioDeviceTransportTypeBuiltIn)
            return d.id;
    }
    return kAudioObjectUnknown;
}

void CoreAudioOutput::route(const float* l, const float* r, size_t frames, float* out, uint32_t channels, uint32_t lc, uint32_t rc)
{
    std::memset(out, 0, frames * channels * sizeof(float));
    if (channels == 0)
        return;
    if (lc == rc)
    {
        if (lc < channels)
            for (size_t i = 0; i < frames; i++)
                out[i * channels + lc] = 0.5f * (l[i] + r[i]);
        return;
    }
    if (lc < channels)
        for (size_t i = 0; i < frames; i++)
            out[i * channels + lc] = l[i];
    if (rc < channels)
        for (size_t i = 0; i < frames; i++)
            out[i * channels + rc] = r[i];
}

// ---- IOProc ---------------------------------------------------------------------------------

OSStatus CoreAudioOutput::Impl::converterInput(AudioConverterRef, UInt32* packets, AudioBufferList* data, AudioStreamPacketDescription**, void* ref)
{
    auto* d = (Dev*)ref;
    AudioRuntime* rt = d->rt.load(std::memory_order_acquire);
    const size_t want = std::min<size_t>(*packets, d->srcCap);
    if (rt)
        rt->pull(d->srcL.data(), d->srcR.data(), want);
    else
    {
        std::memset(d->srcL.data(), 0, want * 4);
        std::memset(d->srcR.data(), 0, want * 4);
    }
    data->mNumberBuffers = 2;
    data->mBuffers[0] = { 1, (UInt32)(want * 4), d->srcL.data() };
    data->mBuffers[1] = { 1, (UInt32)(want * 4), d->srcR.data() };
    *packets = (UInt32)want;
    return noErr;
}

OSStatus CoreAudioOutput::Impl::renderCb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp* ts, UInt32, UInt32 frames, AudioBufferList* io)
{
    auto* d = (Dev*)ref;
    for (UInt32 i = 0; i < io->mNumberBuffers; i++)
        std::memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
    AudioRuntime* rt = d->rt.load(std::memory_order_acquire);
    if (!rt || io->mNumberBuffers != 1 || frames > d->maxFrames)
        return noErr;

    Shared& sh = *d->sh;
    sh.cycles.fetch_add(1, std::memory_order_relaxed);
    if (!sh.freezeClock.load(std::memory_order_relaxed))
        sh.lastCycleNs.store(nowNs(), std::memory_order_relaxed);
    if (sh.onCycle)
    {
        const size_t pos = rt->readPosition();
        sh.onCycle(ts->mHostTime, rt->blockIndexAt(pos), pos % AudioRuntime::kBlockFrames);
    }

    float* out = (float*)io->mBuffers[0].mData;
    if (!d->conv)
    {
        rt->pull(d->srcL.data(), d->srcR.data(), frames);
        CoreAudioOutput::route(d->srcL.data(), d->srcR.data(), frames, out, d->channels, d->lc, d->rc);
    }
    else
    {
        std::memset(d->outL.data(), 0, frames * 4);
        std::memset(d->outR.data(), 0, frames * 4);
        struct
        {
            UInt32 n;
            AudioBuffer b[2];
        } list;
        list.n = 2;
        list.b[0] = { 1, frames * 4, d->outL.data() };
        list.b[1] = { 1, frames * 4, d->outR.data() };
        UInt32 packets = frames;
        AudioConverterFillComplexBuffer(d->conv, &Impl::converterInput, d, &packets, (AudioBufferList*)&list, nullptr);
        CoreAudioOutput::route(d->outL.data(), d->outR.data(), frames, out, d->channels, d->lc, d->rc);
    }
    return noErr;
}

// ---- building a device ----------------------------------------------------------------------

CoreAudioOutput::Impl::Err CoreAudioOutput::Impl::build(AudioDeviceID id, uint8_t l, uint8_t r, bool requestRate, std::shared_ptr<Dev>& out)
{
    std::function<void(AudioDeviceID)> before;
    {
        std::lock_guard<std::mutex> g(mtx);
        before = hooks.beforeOpen;
    }
    if (before)
        before(id);
    if (id == kAudioObjectUnknown)
        return Err::Failed;
    const uint32_t devCh = outputChannels(id);
    if (devCh == 0)
        return Err::Failed;
    if (l >= devCh)
        return Err::BadChannels;
    if (r >= devCh)
        r = l;

    // Rate: ask for the runtime's rate, else take what the device runs at.
    double rate = nominalRate(id);
    if (requestRate && std::fabs(rate - AudioRuntime::kSampleRate) > 0.5 && rateSupported(id, AudioRuntime::kSampleRate))
    {
        Float64 want = AudioRuntime::kSampleRate;
        AudioObjectPropertyAddress a{ kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectSetPropertyData(id, &a, 0, nullptr, sizeof want, &want);
        for (int i = 0; i < 50; i++) // the change completes asynchronously: up to 0.5 s
        {
            rate = nominalRate(id);
            if (std::fabs(rate - AudioRuntime::kSampleRate) <= 0.5)
                break;
            std::this_thread::sleep_for(10ms);
        }
    }
    rate = nominalRate(id);
    if (rate <= 0)
        return Err::Failed;

    {
        AudioValueRange range{ 0, 0 };
        UInt32 frames = kWantedBufferFrames;
        if (getProp(id, kAudioDevicePropertyBufferFrameSizeRange, range))
            frames = (UInt32)std::clamp<double>(frames, range.mMinimum, range.mMaximum);
        AudioObjectPropertyAddress a{ kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectSetPropertyData(id, &a, 0, nullptr, sizeof frames, &frames);
    }
    UInt32 bufFrames = 0;
    getProp(id, kAudioDevicePropertyBufferFrameSize, bufFrames);

    auto d = std::make_shared<Dev>();
    d->id = id;
    d->name = deviceNameOf(id);
    d->sh = sh;
    d->channels = devCh;
    d->lc = l;
    d->rc = r;
    d->rate = rate;
    d->bufFrames = bufFrames;

    AudioComponentDescription desc{ kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &d->unit) != noErr)
        return Err::Failed;
    AudioUnit unit = d->unit; // Dev::teardown disposes it on every failure path below

    UInt32 on = 1, off = 0;
    if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &on, sizeof on) != noErr ||
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &off, sizeof off) != noErr ||
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &id, sizeof id) != noErr)
        return Err::Failed;

    // Client format: float, interleaved, every device channel, at the device rate.
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    fmt.mChannelsPerFrame = devCh;
    fmt.mBitsPerChannel = 32;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = fmt.mBytesPerPacket = 4 * devCh;
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt) != noErr)
        return Err::Failed;

    UInt32 maxFrames = 0, sz = sizeof maxFrames;
    AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, &sz);
    if (maxFrames < bufFrames || maxFrames == 0)
    {
        maxFrames = std::max<UInt32>(bufFrames, 1024);
        AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
    }
    d->maxFrames = maxFrames;

    d->converter = std::fabs(rate - AudioRuntime::kSampleRate) > 0.5;
    if (d->converter)
    {
        AudioStreamBasicDescription src{};
        src.mSampleRate = AudioRuntime::kSampleRate;
        src.mFormatID = kAudioFormatLinearPCM;
        src.mFormatFlags = (AudioFormatFlags)kAudioFormatFlagsNativeFloatPacked | (AudioFormatFlags)kAudioFormatFlagIsNonInterleaved;
        src.mChannelsPerFrame = 2;
        src.mBitsPerChannel = 32;
        src.mFramesPerPacket = 1;
        src.mBytesPerFrame = src.mBytesPerPacket = 4;
        AudioStreamBasicDescription dst = src;
        dst.mSampleRate = rate;
        if (AudioConverterNew(&src, &dst, &d->conv) != noErr)
            return Err::Failed;
        d->srcCap = (size_t)std::ceil(maxFrames * AudioRuntime::kSampleRate / rate) + 64;
        d->srcL.assign(d->srcCap, 0.f);
        d->srcR.assign(d->srcCap, 0.f);
        d->outL.assign(maxFrames, 0.f);
        d->outR.assign(maxFrames, 0.f);
    }
    else
    {
        d->srcL.assign(maxFrames, 0.f);
        d->srcR.assign(maxFrames, 0.f);
        d->srcCap = maxFrames;
    }

    AURenderCallbackStruct cb{ &Impl::renderCb, d.get() };
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
        AudioUnitInitialize(unit) != noErr)
        return Err::Failed;

    // Latency: device + safety offset + stream + buffer, in frames at the device rate.
    UInt32 devLat = 0, safety = 0, streamLat = 0;
    getProp(id, kAudioDevicePropertyLatency, devLat, kAudioObjectPropertyScopeOutput);
    getProp(id, kAudioDevicePropertySafetyOffset, safety, kAudioObjectPropertyScopeOutput);
    {
        AudioObjectPropertyAddress a{ kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
        UInt32 ssz = 0;
        if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &ssz) == noErr && ssz >= sizeof(AudioStreamID))
        {
            std::vector<AudioStreamID> streams(ssz / sizeof(AudioStreamID));
            if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &ssz, streams.data()) == noErr)
                getProp(streams[0], kAudioStreamPropertyLatency, streamLat);
        }
    }
    d->latency = ((double)devLat + safety + streamLat + bufFrames) / rate;

    os_workgroup_t wg = nullptr;
    if (getProp(id, kAudioDevicePropertyIOThreadOSWorkgroup, wg) && wg)
        d->wg = (void*)wg;

    addListeners(*d);
    d->rt.store(&rt);
    out = d;
    return Err::Ok;
}

// ---- listeners ------------------------------------------------------------------------------

// Contexts are weak_ptrs that are deliberately leaked (16 bytes per device open): the HAL may
// still deliver a callback after the listener is removed, and the weak_ptr keeps that safe.
void CoreAudioOutput::Impl::addListeners(Dev& d)
{
    auto* ctx = new std::weak_ptr<Impl>(shared_from_this());
    AudioObjectPropertyAddress alive{ kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress rate{ kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(d.id, &alive, &Impl::propertyCb, ctx);
    AudioObjectAddPropertyListener(d.id, &rate, &Impl::propertyCb, ctx);
    d.listenCtx = ctx;
}

void CoreAudioOutput::Impl::addSystemListeners()
{
    if (sysListening)
        return;
    sysListening = true;
    sysCtx = new std::weak_ptr<Impl>(shared_from_this());
    AudioObjectPropertyAddress def{ kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress devs{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &def, &Impl::propertyCb, sysCtx);
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &devs, &Impl::propertyCb, sysCtx);
}

void CoreAudioOutput::Impl::removeSystemListeners()
{
    if (!sysListening)
        return;
    sysListening = false;
    AudioObjectPropertyAddress def{ kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress devs{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &def, &Impl::propertyCb, sysCtx);
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &devs, &Impl::propertyCb, sysCtx);
}

// HAL notification thread: only hop to the control queue.
OSStatus CoreAudioOutput::Impl::propertyCb(AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* addrs, void* ref)
{
    auto self = ((std::weak_ptr<Impl>*)ref)->lock();
    if (!self)
        return noErr;
    for (UInt32 i = 0; i < n; i++)
        switch (addrs[i].mSelector)
        {
            case kAudioDevicePropertyDeviceIsAlive: self->postEvent(Event::DeviceDead); break;
            case kAudioDevicePropertyNominalSampleRate: self->postEvent(Event::RateChanged); break;
            case kAudioHardwarePropertyDefaultOutputDevice: self->postEvent(Event::DefaultChanged); break;
            case kAudioHardwarePropertyDevices: self->postEvent(Event::DevicesChanged); break;
        }
    return noErr;
}

// ---- control --------------------------------------------------------------------------------

std::shared_ptr<Dev> CoreAudioOutput::Impl::tryOpen(AudioDeviceID id, uint8_t l, uint8_t r, bool requestRate, Err& err)
{
    err = Err::Failed;
    if (isUnresponsive(id))
        return nullptr;
    struct Result
    {
        std::shared_ptr<Dev> dev;
        Err err = Err::Failed;
    };
    auto res = std::make_shared<Result>();
    const bool ok = runTimed(
      [this, id, l, r, requestRate, res]
      {
          // `this` stays valid: runTimed's task holds a shared_ptr to the Impl.
          res->err = build(id, l, r, requestRate, res->dev);
      },
      [res]
      {
          if (res->dev)
              res->dev->teardown(); // late result after the caller gave up
      });
    if (!ok)
    {
        markUnresponsive(id);
        return nullptr;
    }
    err = res->err;
    return res->err == Err::Ok ? res->dev : nullptr;
}

void CoreAudioOutput::Impl::install(std::shared_ptr<Dev> d, bool follow)
{
    {
        std::lock_guard<std::mutex> g(mtx);
        cur = d;
        following = follow;
        lc = (uint8_t)d->lc;
        rc = (uint8_t)d->rc;
    }
    if (d->wg)
        rt.setWorkgroup(d->wg); // never released: the render thread may still be joined to the old one
}

void CoreAudioOutput::Impl::teardownCur()
{
    std::shared_ptr<Dev> d;
    {
        std::lock_guard<std::mutex> g(mtx);
        d = cur;
        cur.reset();
    }
    if (!d)
        return;
    if (!runTimed([d] { d->teardown(); }))
        markUnresponsive(d->id); // abandoned: the zombie task still owns the Dev
}

// Makes the IOProc state match the `running` intent for the current device.
void CoreAudioOutput::Impl::syncRunState()
{
    for (int i = 0; i < 3; i++)
    {
        std::shared_ptr<Dev> d;
        bool want;
        {
            std::lock_guard<std::mutex> g(mtx);
            d = cur;
            want = running;
        }
        if (!d || d->started.load() == want)
            return;
        const bool ok = runTimed(
          [this, d, want]
          {
              if (d->torn)
                  return;
              if (want && !d->started.load())
              {
                  if (AudioOutputUnitStart(d->unit) == noErr)
                  {
                      sh->lastCycleNs.store(nowNs());
                      d->started = true;
                  }
              }
              else if (!want && d->started.load())
              {
                  AudioOutputUnitStop(d->unit);
                  d->started = false;
              }
          });
        if (!ok)
        {
            markUnresponsive(d->id);
            std::lock_guard<std::mutex> g(mtx);
            if (cur == d)
                cur.reset(); // abandoned
            return;
        }
        if (d->started.load() != want)
            return; // the start failed: the watchdog / device events deal with it
    }
}

void CoreAudioOutput::Impl::fallback(AudioDeviceID avoid)
{
    teardownCur();
    // Order: the system default, then the built-in output. The default can still be the dead
    // device for a moment: it is skipped, and the default-change notification retries.
    const AudioDeviceID def = defaultDev();
    const AudioDeviceID builtin = builtinDev();
    std::vector<AudioDeviceID> cands;
    if (def != kAudioObjectUnknown)
        cands.push_back(def);
    if (builtin != kAudioObjectUnknown && builtin != def)
        cands.push_back(builtin);
    std::string name;
    bool opened = false;
    for (AudioDeviceID c : cands)
    {
        if (c == avoid && !alive(c))
            continue;
        Err err;
        auto d = tryOpen(c, 0, 1, true, err);
        if (!d)
            continue;
        install(d, true);
        syncRunState();
        bool wantRun, started;
        {
            std::lock_guard<std::mutex> g(mtx);
            wantRun = running;
            started = d->started.load();
        }
        if (wantRun && !started) // would not start: next candidate
        {
            teardownCur();
            continue;
        }
        name = d->name;
        opened = true;
        break;
    }
    {
        std::lock_guard<std::mutex> g(mtx);
        following = true;
    }
    if (opened)
    {
        sh->reopens++;
        postStatus(Status::FellBack, name);
    }
    else
        postStatus(Status::Failed, {});
}

void CoreAudioOutput::Impl::reopenSame()
{
    std::shared_ptr<Dev> d;
    uint8_t l, r;
    {
        std::lock_guard<std::mutex> g(mtx);
        d = cur;
        l = lc;
        r = rc;
    }
    if (!d)
        return;
    const AudioDeviceID id = d->id;
    teardownCur();
    Err err;
    auto nd = tryOpen(id, l, r, false, err); // follow the new rate through the converter
    if (!nd)
    {
        fallback(id);
        return;
    }
    install(nd, id == defaultDev());
    syncRunState();
    sh->reopens++;
}

void CoreAudioOutput::Impl::handleEvent(Event ev)
{
    std::shared_ptr<Dev> d;
    bool wasRunning, follow, want;
    {
        std::lock_guard<std::mutex> g(mtx);
        if (dying)
            return;
        d = cur;
        wasRunning = running;
        follow = following;
        want = wantOpen;
    }
    if (!want)
        return;
    switch (ev)
    {
        case Event::DeviceDead:
            if (d && !alive(d->id))
                fallback(d->id);
            break;
        case Event::RateChanged:
            if (d && std::fabs(nominalRate(d->id) - d->rate) >= 0.5)
                reopenSame();
            break;
        case Event::DefaultChanged:
            if (!d && want)
                fallback(kAudioObjectUnknown);
            else if (d && follow && defaultDev() != d->id && defaultDev() != kAudioObjectUnknown)
                fallback(kAudioObjectUnknown);
            break;
        case Event::DevicesChanged:
            if (!d)
                fallback(kAudioObjectUnknown);
            else
            {
                // An unplug often removes the device without a DeviceIsAlive notification.
                bool present = false;
                for (auto& i : CoreAudioOutput::listOutputDevices())
                    present |= i.id == d->id;
                if (!alive(d->id) || !present)
                    fallback(d->id);
            }
            break;
        case Event::OpenFailed:
            fallback(kAudioObjectUnknown);
            break;
        case Event::Watchdog:
            if (d && wasRunning && d->started.load() &&
                nowNs() - sh->lastCycleNs.load() > kStallNs)
                fallback(d->id);
            break;
    }
}

// ---- public API -----------------------------------------------------------------------------

CoreAudioOutput::CoreAudioOutput(AudioRuntime& runtime) : impl_(std::make_shared<Impl>(runtime))
{
    impl_->startTimer();
}

CoreAudioOutput::~CoreAudioOutput()
{
    Impl& i = *impl_;
    std::shared_ptr<Dev> d;
    {
        std::lock_guard<std::mutex> g(i.mtx);
        i.dying = true; // handlers and the watchdog stop acting
        d = i.cur;
        i.cur.reset();
        i.running = false;
    }
    if (i.timer)
        dispatch_source_cancel(i.timer);
    const bool ok = i.runTimed(
      [&i, d]
      {
          if (d)
              d->teardown();
          i.removeSystemListeners();
      });
    if (d)
        d->rt.store(nullptr); // an IOProc that is still alive (wedged teardown) goes silent
    if (!ok)
    {
        // A HAL call is wedged on the worker. Do not wait for it at quit: the wedged task keeps
        // the Impl, the Dev and its thread alive (a deliberate leak) and nothing else blocks.
    }
}

void CoreAudioOutput::setOnCycle(std::function<void(uint64_t, uint64_t, size_t)> fn)
{
    impl_->sh->onCycle = std::move(fn);
}

void CoreAudioOutput::setOnStatus(std::function<void(Status, std::string)> fn)
{
    std::lock_guard<std::mutex> g(impl_->mtx);
    impl_->onStatus = std::move(fn);
}

bool CoreAudioOutput::open(AudioDeviceID id, uint8_t leftCh, uint8_t rightCh)
{
    Impl& i = *impl_;
    {
        std::lock_guard<std::mutex> g(i.mtx);
        i.running = false;
        i.wantOpen = true;
    }
    i.teardownCur();
    {
        // System listeners go on the worker too (they are HAL calls).
        i.runTimed([&i] { i.addSystemListeners(); });
    }
    Impl::Err err;
    auto d = i.tryOpen(id, leftCh, rightCh, true, err);
    if (d)
    {
        i.install(d, id == defaultOutputDevice());
        i.postStatus(Status::Opened, d->name);
        return true;
    }
    if (err == Impl::Err::BadChannels)
        return false; // caller error: no fallback
    i.postEvent(Event::OpenFailed);
    return false;
}

bool CoreAudioOutput::start()
{
    Impl& i = *impl_;
    {
        std::lock_guard<std::mutex> g(i.mtx);
        if (!i.cur)
            return false;
        i.running = true;
    }
    i.syncRunState();
    return isRunning();
}

void CoreAudioOutput::stop()
{
    Impl& i = *impl_;
    {
        std::lock_guard<std::mutex> g(i.mtx);
        i.running = false;
    }
    i.syncRunState();
}

bool CoreAudioOutput::isOpen() const
{
    std::lock_guard<std::mutex> g(impl_->mtx);
    return impl_->cur != nullptr;
}

bool CoreAudioOutput::isRunning() const
{
    std::lock_guard<std::mutex> g(impl_->mtx);
    return impl_->running && impl_->cur && impl_->cur->started.load();
}

AudioDeviceID CoreAudioOutput::deviceId() const
{
    std::lock_guard<std::mutex> g(impl_->mtx);
    return impl_->cur ? impl_->cur->id : kAudioObjectUnknown;
}

std::string CoreAudioOutput::deviceName() const
{
    std::lock_guard<std::mutex> g(impl_->mtx);
    return impl_->cur ? impl_->cur->name : std::string();
}

double CoreAudioOutput::outputLatencySeconds() const
{
    std::lock_guard<std::mutex> g(impl_->mtx);
    return impl_->cur ? impl_->cur->latency : 0.0;
}

double CoreAudioOutput::deviceSampleRate() const
{
    std::lock_guard<std::mutex> g(impl_->mtx);
    return impl_->cur ? impl_->cur->rate : AudioRuntime::kSampleRate;
}

bool CoreAudioOutput::usingConverter() const
{
    std::lock_guard<std::mutex> g(impl_->mtx);
    return impl_->cur && impl_->cur->converter;
}

uint32_t CoreAudioOutput::bufferFrames() const
{
    std::lock_guard<std::mutex> g(impl_->mtx);
    return impl_->cur ? impl_->cur->bufFrames : 0;
}

uint64_t CoreAudioOutput::cycles() const
{
    return impl_->sh->cycles.load();
}

uint64_t CoreAudioOutput::reopens() const
{
    return impl_->sh->reopens.load();
}

// ---- test seams -----------------------------------------------------------------------------

void CoreAudioOutputTestAccess::setHooks(CoreAudioOutput& o, CoreAudioOutputTestHooks h)
{
    std::lock_guard<std::mutex> g(o.impl_->mtx);
    o.impl_->hooks = std::move(h);
}

void CoreAudioOutputTestAccess::postDeviceDead(CoreAudioOutput& o)
{
    o.impl_->postEvent(Event::DeviceDead);
}

void CoreAudioOutputTestAccess::postDevicesChanged(CoreAudioOutput& o)
{
    o.impl_->postEvent(Event::DevicesChanged);
}

void CoreAudioOutputTestAccess::freezeWatchdogClock(CoreAudioOutput& o, bool freeze)
{
    o.impl_->sh->freezeClock = freeze;
}

bool CoreAudioOutputTestAccess::isUnresponsive(CoreAudioOutput& o, AudioDeviceID id)
{
    return o.impl_->isUnresponsive(id);
}
