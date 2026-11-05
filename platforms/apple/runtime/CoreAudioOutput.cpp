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
    std::atomic<bool> cycleEnabled{ true }; // cleared by the destructor: onCycle is never called after
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
// One opened device: AUHAL unit + optional converter + IOProc scratch. Built and torn down on the
// worker; the IOProc holds a raw pointer, valid until teardown() returns. A Dev is only ever
// freed after teardown() (see makeDev's deleter): no HAL call ever happens in ~Dev.
struct Dev
{
    AudioDeviceID id = kAudioObjectUnknown;
    std::string name;
    AudioUnit unit = nullptr; // null for a test fake
    AudioConverterRef conv = nullptr;
    std::atomic<AudioRuntime*> rt{ nullptr };
    std::shared_ptr<Shared> sh;
    uint32_t channels = 0, lc = 0, rc = 1, maxFrames = 0, bufFrames = 0;
    double rate = 44100, latency = 0;
    bool converter = false;
    bool fake = false;
    std::vector<float> srcL, srcR, outL, outR;
    size_t srcCap = 0;
    void* wg = nullptr;
    std::atomic<bool> started{ false };
    std::atomic<bool> torn{ false };
    std::function<void(AudioDeviceID)> onTeardown; // test hook
    void* listenCtx = nullptr; // weak_ptr<Impl>* (deliberately leaked: see addListeners)

    void removeListeners();
    void teardown()
    {
        if (torn.exchange(true))
            return;
        rt.store(nullptr);
        if (onTeardown)
            onTeardown(id);
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
};

// An untorn Dev must never be torn down inline on whatever thread drops the last reference (that
// would be an unbounded HAL call). If one is dropped untorn (its worker is wedged and abandoned)
// it is leaked on purpose.
std::shared_ptr<Dev> makeDev()
{
    return std::shared_ptr<Dev>(new Dev,
                                [](Dev* d)
                                {
                                    if (d->torn.load())
                                        delete d;
                                });
}

enum class Timed
{
    Done,
    OpTimeout,   // the op itself ran longer than its bound
    NeverStarted // never reached the worker (queue wedged behind an abandoned op)
};
} // namespace

std::vector<CoreAudioOutput::DeviceInfo> listOutputDevicesSkipping(const std::set<AudioDeviceID>& skip);
AudioDeviceID builtInSkipping(const std::set<AudioDeviceID>& skip);

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
    struct CmdState
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false, result = false;
    };
    struct CmdMsg
    {
        std::shared_ptr<Impl> self;
        std::function<bool()> fn;
        std::shared_ptr<CmdState> st;
        bool whenDying;
    };
    using TestHooks = CoreAudioOutputTestHooks;

    AudioRuntime& rt;
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    dispatch_queue_t queue = nullptr; // serial: ALL control work runs here
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
    uint64_t openGen = 0; // bumped by every public open(): older fallbacks/opens stand down
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
        if (worker)
            worker->stop(); // ends the (idle) worker thread; a wedged one is already abandoned
    }

    bool isDying()
    {
        std::lock_guard<std::mutex> g(mtx);
        return dying;
    }
    bool stale(uint64_t gen)
    {
        std::lock_guard<std::mutex> g(mtx);
        return dying || openGen != gen;
    }
    uint64_t generation()
    {
        std::lock_guard<std::mutex> g(mtx);
        return openGen;
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
                if (!m->self->dying)
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
        {
            std::lock_guard<std::mutex> g(mtx);
            if (dying)
                return;
        }
        dispatch_async_f(queue, new Msg{ shared_from_this(), Event::Watchdog, true, s, std::move(name) }, &runMsg);
    }
    static void runCmdFn(void* p)
    {
        std::unique_ptr<CmdMsg> m((CmdMsg*)p);
        bool res = false;
        if (m->whenDying || !m->self->isDying())
            res = m->fn();
        std::lock_guard<std::mutex> g(m->st->m);
        m->st->result = res;
        m->st->done = true;
        m->st->cv.notify_all();
    }
    // Runs `fn` on the control queue and waits for it at most `wait` from now. On the deadline
    // returns false; the command is still queued and completes later (its intent was recorded
    // by the caller), reporting through onStatus.
    bool runCmd(std::function<bool()> fn, std::chrono::milliseconds wait = 2500ms, bool whenDying = false)
    {
        auto st = std::make_shared<CmdState>();
        dispatch_async_f(queue, new CmdMsg{ shared_from_this(), std::move(fn), st, whenDying }, &runCmdFn);
        std::unique_lock<std::mutex> g(st->m);
        if (!st->cv.wait_for(g, wait, [&] { return st->done; }))
            return false;
        return st->result;
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
    TestHooks hooksCopy()
    {
        std::lock_guard<std::mutex> g(mtx);
        return hooks;
    }
    // Even property reads can hang on a broken virtual device, so every HAL read made on the
    // control queue goes through the worker with the same bound (def on timeout).
    template <class T>
    T bounded(T def, std::function<T()> f, AudioDeviceID blame = kAudioObjectUnknown)
    {
        auto res = std::make_shared<T>(def);
        const Timed t = runTimed([res, f] { *res = f(); });
        if (t == Timed::OpTimeout && blame != kAudioObjectUnknown)
            markUnresponsive(blame);
        return t == Timed::Done ? *res : def;
    }
    bool alive(AudioDeviceID id)
    {
        auto h = hooksCopy();
        if (h.alive)
            return h.alive(id);
        return bounded<bool>(false, [id] { return deviceAliveReal(id); }, id); // a hung read = dead
    }
    double rateOf(AudioDeviceID id)
    {
        return bounded<double>(0.0, [id] { return nominalRate(id); }, id);
    }
    AudioDeviceID defaultDev()
    {
        auto h = hooksCopy();
        if (h.defaultDevice)
            return h.defaultDevice();
        return bounded<AudioDeviceID>(kAudioObjectUnknown, [] { return CoreAudioOutput::defaultOutputDevice(); });
    }
    AudioDeviceID builtinDev()
    {
        std::set<AudioDeviceID> skip;
        {
            std::lock_guard<std::mutex> g(mtx);
            if (hooks.builtinDevice)
                return hooks.builtinDevice();
            skip = unresponsive;
        }
        return bounded<AudioDeviceID>(kAudioObjectUnknown, [skip] { return builtInSkipping(skip); });
    }
    bool present(AudioDeviceID id)
    {
        std::set<AudioDeviceID> none;
        return bounded<bool>(false,
                             [id]
                             {
                                 for (auto& i : listOutputDevicesSkipping({}))
                                     if (i.id == id)
                                         return true;
                                 return false;
                             },
                             id);
    }
    bool isUnresponsive(AudioDeviceID id)
    {
        std::lock_guard<std::mutex> g(mtx);
        return unresponsive.count(id) != 0;
    }
    void markUnresponsive(AudioDeviceID id)
    {
        std::lock_guard<std::mutex> g(mtx);
        unresponsive.insert(id);
    }

    // -- run `f` on the worker. The bound runs from when the op STARTS executing on the worker
    // (so a queued op is not blamed for time spent behind another). `cleanup` runs on the worker
    // if the caller already gave up (so a late result does not leak a device). Called from the
    // control queue only (serialised), so the worker is idle unless it was just abandoned.
    Timed runTimed(std::function<void()> f, std::function<void()> cleanup = {}, std::chrono::milliseconds bound = kOpTimeout)
    {
        struct St
        {
            std::mutex m;
            std::condition_variable cv;
            bool started = false, done = false, abandoned = false;
            std::chrono::steady_clock::time_point startedAt;
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
                st->started = true;
                st->startedAt = std::chrono::steady_clock::now();
                st->cv.notify_all();
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
        const auto preDeadline = std::chrono::steady_clock::now() + bound;
        std::unique_lock<std::mutex> g(st->m);
        for (;;)
        {
            if (st->done)
                return Timed::Done;
            const auto deadline = st->started ? st->startedAt + bound : preDeadline;
            if (std::chrono::steady_clock::now() >= deadline)
                break;
            st->cv.wait_until(g, deadline);
        }
        const Timed res = st->started ? Timed::OpTimeout : Timed::NeverStarted;
        st->abandoned = true;
        g.unlock();
        std::lock_guard<std::mutex> g2(mtx);
        if (worker == w) // the next op gets a fresh worker; this one is abandoned
            worker.reset();
        w->stop();
        return res;
    }

    // -- building a device (worker thread)
    static OSStatus renderCb(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);
    static OSStatus converterInput(AudioConverterRef, UInt32*, AudioBufferList*, AudioStreamPacketDescription**, void*);
    static OSStatus propertyCb(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*);

    Err build(AudioDeviceID id, uint8_t l, uint8_t r, bool requestRate, std::shared_ptr<Dev>& out);
    void addListeners(Dev& d);
    void addSystemListeners();
    void removeSystemListeners();

    // -- control: everything below runs on the control queue only
    std::shared_ptr<Dev> tryOpen(AudioDeviceID id, uint8_t l, uint8_t r, bool requestRate, Err& err);
    bool install(std::shared_ptr<Dev> d, bool follow); // false (nothing installed) once dying
    void discard(std::shared_ptr<Dev> d);              // tear down a device that was not installed
    void teardownCur();
    void abandonCur(const std::shared_ptr<Dev>& d);
    bool syncRunState(); // false if the device failed or hung starting/stopping
    void fallback(AudioDeviceID avoid, uint64_t gen);
    void handleEvent(Event ev);
    void reopenSame(uint64_t gen);
    bool cmdOpen(AudioDeviceID id, uint8_t l, uint8_t r, uint64_t gen);
    bool cmdSync();
    void cmdShutdown();
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

std::vector<CoreAudioOutput::DeviceInfo> listOutputDevicesSkipping(const std::set<AudioDeviceID>& skip)
{
    std::vector<CoreAudioOutput::DeviceInfo> out;
    AudioObjectPropertyAddress a{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr)
        return out;
    std::vector<AudioDeviceID> ids(sz / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) != noErr)
        return out;
    for (AudioDeviceID id : ids)
    {
        if (skip.count(id)) // known-unresponsive: even a property read can hang on it
            continue;
        const uint32_t ch = outputChannels(id);
        if (ch > 0)
            out.push_back({ id, deviceNameOf(id), ch });
    }
    return out;
}

std::vector<CoreAudioOutput::DeviceInfo> CoreAudioOutput::listOutputDevices()
{
    return listOutputDevicesSkipping({});
}

AudioDeviceID CoreAudioOutput::defaultOutputDevice()
{
    AudioDeviceID id = kAudioObjectUnknown;
    if (!getProp(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, id))
        return kAudioObjectUnknown;
    return id;
}

AudioDeviceID builtInSkipping(const std::set<AudioDeviceID>& skip)
{
    for (auto& d : listOutputDevicesSkipping(skip))
    {
        UInt32 t = 0;
        if (getProp(d.id, kAudioDevicePropertyTransportType, t) && t == kAudioDeviceTransportTypeBuiltIn)
            return d.id;
    }
    return kAudioObjectUnknown;
}

AudioDeviceID CoreAudioOutput::builtInOutputDevice()
{
    return builtInSkipping({});
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
    if (sh.cycleEnabled.load(std::memory_order_relaxed) && sh.onCycle)
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
    std::function<void(AudioDeviceID)> before, beforeTear;
    {
        auto h = hooksCopy();
        before = h.beforeOpen;
        beforeTear = h.beforeTeardown;
    }
    if (before)
        before(id);
    if (id == kAudioObjectUnknown)
        return Err::Failed;
    TestHooks th = hooksCopy();
    if (th.fakeOpen && th.fakeOpen(id))
    {
        auto d = makeDev(); // no HAL: a device object for the control logic tests
        d->id = id;
        d->name = "fake " + std::to_string(id);
        d->fake = true;
        d->sh = sh;
        d->onTeardown = th.beforeTeardown;
        d->channels = 2;
        d->maxFrames = 1024;
        d->srcCap = 1024;
        d->srcL.assign(1024, 0.f);
        d->srcR.assign(1024, 0.f);
        d->wg = (void*)(uintptr_t)id;
        d->rt.store(&rt);
        out = d;
        return Err::Ok;
    }
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

    auto d = makeDev();
    d->id = id;
    d->name = deviceNameOf(id);
    d->sh = sh;
    d->onTeardown = beforeTear;
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

// ---- control (control queue) ----------------------------------------------------------------

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
    const Timed t = runTimed(
      [this, id, l, r, requestRate, res] { res->err = build(id, l, r, requestRate, res->dev); },
      [res]
      {
          if (res->dev)
              res->dev->teardown(); // late result after the caller gave up
      });
    if (t == Timed::OpTimeout)
        markUnresponsive(id); // its own op exceeded the bound
    if (t != Timed::Done)
        return nullptr;
    err = res->err;
    return res->err == Err::Ok ? res->dev : nullptr;
}

bool CoreAudioOutput::Impl::install(std::shared_ptr<Dev> d, bool follow)
{
    // Under the lock so that ~CoreAudioOutput's `dying = true` fences it: no setWorkgroup (nor
    // any use of the runtime) starts after the destructor has set the flag.
    std::lock_guard<std::mutex> g(mtx);
    if (dying)
        return false;
    cur = d;
    following = follow;
    lc = (uint8_t)d->lc;
    rc = (uint8_t)d->rc;
    if (d->wg)
    {
        if (hooks.workgroupSink)
            hooks.workgroupSink(d->wg);
        else if (!d->fake)
            rt.setWorkgroup(d->wg); // never released: the render thread may still be joined to the old one
    }
    return true;
}

void CoreAudioOutput::Impl::discard(std::shared_ptr<Dev> d)
{
    if (!d)
        return;
    const Timed t = runTimed([d] { d->teardown(); });
    if (t != Timed::Done)
    {
        d->rt.store(nullptr, std::memory_order_release); // abandoned untorn: keep its IOProc silent
        if (t == Timed::OpTimeout)
            markUnresponsive(d->id);
    }
}

void CoreAudioOutput::Impl::abandonCur(const std::shared_ptr<Dev>& d)
{
    // The op on `d` is wedged on an abandoned worker; the Dev stays owned by that op (and is
    // leaked if it never returns). Nothing may tear it down inline. Its IOProc, if the wedged
    // start/stop later lets it run, must neither pull from the ring nor touch the runtime.
    d->rt.store(nullptr, std::memory_order_release);
    std::lock_guard<std::mutex> g(mtx);
    if (cur == d)
        cur.reset();
}

void CoreAudioOutput::Impl::teardownCur()
{
    std::shared_ptr<Dev> d;
    {
        std::lock_guard<std::mutex> g(mtx);
        d = cur;
        cur.reset();
    }
    discard(d);
}

// Makes the IOProc state match the `running` intent for the current device.
bool CoreAudioOutput::Impl::syncRunState()
{
    std::shared_ptr<Dev> d;
    bool want;
    TestHooks h;
    {
        std::lock_guard<std::mutex> g(mtx);
        d = cur;
        want = running;
        h = hooks;
    }
    if (!d || d->started.load() == want)
        return true;
    const Timed t = runTimed(
      [this, d, want, h]
      {
          if (d->torn.load())
              return;
          if (want)
          {
              if (h.beforeStart)
                  h.beforeStart(d->id);
              if (!d->unit)
              {
                  d->started = true; // test fake
                  if (h.simulateIoProcAfterStart)
                  {
                      // One real IOProc cycle, as when a wedged AudioOutputUnitStart finally
                      // returns on a device that has been abandoned meanwhile.
                      float buf[128 * 2];
                      AudioBufferList abl;
                      abl.mNumberBuffers = 1;
                      abl.mBuffers[0] = { 2, sizeof buf, buf };
                      AudioTimeStamp ts{};
                      renderCb(d.get(), nullptr, &ts, 0, 128, &abl);
                  }
              }
              else if (AudioOutputUnitStart(d->unit) == noErr)
              {
                  sh->lastCycleNs.store(nowNs());
                  d->started = true;
              }
          }
          else
          {
              if (d->unit)
                  AudioOutputUnitStop(d->unit);
              d->started = false;
          }
      });
    if (t == Timed::OpTimeout)
    {
        markUnresponsive(d->id); // a start/stop that hangs is a dead device
        abandonCur(d);
        return false;
    }
    return t == Timed::Done && d->started.load() == want;
}

void CoreAudioOutput::Impl::fallback(AudioDeviceID avoid, uint64_t gen)
{
    if (stale(gen))
        return;
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
        if (stale(gen)) // dying, or the user opened something else meanwhile: stand down
            return;
        if (c == avoid && !alive(c))
            continue;
        Err err;
        auto d = tryOpen(c, 0, 1, true, err);
        if (!d)
            continue;
        if (!install(d, true)) // dying
        {
            discard(d);
            return;
        }
        if (stale(gen)) // the user's open() wins
        {
            teardownCur();
            return;
        }
        if (!syncRunState()) // would not start (or hung): next candidate
        {
            teardownCur();
            continue;
        }
        name = d->name;
        opened = true;
        break;
    }
    if (stale(gen))
        return;
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

void CoreAudioOutput::Impl::reopenSame(uint64_t gen)
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
    if (stale(gen))
        return;
    Err err;
    auto nd = tryOpen(id, l, r, false, err); // follow the new rate through the converter
    if (!nd)
    {
        fallback(id, gen);
        return;
    }
    if (!install(nd, id == defaultDev()))
    {
        discard(nd);
        return;
    }
    if (stale(gen))
    {
        teardownCur();
        return;
    }
    if (!syncRunState())
    {
        fallback(id, gen);
        return;
    }
    sh->reopens++;
}

void CoreAudioOutput::Impl::handleEvent(Event ev)
{
    std::shared_ptr<Dev> d;
    bool wasRunning, follow, want;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> g(mtx);
        if (dying)
            return;
        d = cur;
        wasRunning = running;
        follow = following;
        want = wantOpen;
        gen = openGen;
    }
    if (!want)
        return;
    switch (ev)
    {
        case Event::DeviceDead:
            if (d && !alive(d->id))
                fallback(d->id, gen);
            break;
        case Event::RateChanged:
            if (d && !d->fake && std::fabs(rateOf(d->id) - d->rate) >= 0.5)
                reopenSame(gen);
            break;
        case Event::DefaultChanged:
            if (!d)
                fallback(kAudioObjectUnknown, gen);
            else if (follow && defaultDev() != d->id && defaultDev() != kAudioObjectUnknown)
                fallback(kAudioObjectUnknown, gen);
            break;
        case Event::DevicesChanged:
            if (!d)
                fallback(kAudioObjectUnknown, gen);
            else
            {
                // An unplug often removes the device without a DeviceIsAlive notification.
                if (!alive(d->id) || (!d->fake && !present(d->id)))
                    fallback(d->id, gen);
            }
            break;
        case Event::OpenFailed:
            fallback(kAudioObjectUnknown, gen);
            break;
        case Event::Watchdog:
            if (d && !d->fake && wasRunning && d->started.load() && nowNs() - sh->lastCycleNs.load() > kStallNs)
                fallback(d->id, gen);
            break;
    }
}

bool CoreAudioOutput::Impl::cmdOpen(AudioDeviceID id, uint8_t l, uint8_t r, uint64_t gen)
{
    if (stale(gen)) // a newer open() superseded this one
        return false;
    teardownCur();
    if (stale(gen))
        return false;
    if (!sysListening)
    {
        sysListening = true; // set before the call: a hang is not retried
        runTimed([this] { addSystemListeners(); });
    }
    Err err;
    auto d = tryOpen(id, l, r, true, err);
    if (stale(gen))
    {
        discard(d);
        return false;
    }
    if (d)
    {
        if (!install(d, id == defaultDev()))
        {
            discard(d);
            return false;
        }
        postStatus(Status::Opened, d->name);
        return true;
    }
    if (err == Err::BadChannels)
        return false; // caller error: no fallback
    fallback(kAudioObjectUnknown, gen);
    return false;
}

bool CoreAudioOutput::Impl::cmdSync()
{
    std::shared_ptr<Dev> d;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> g(mtx);
        d = cur;
        gen = openGen;
    }
    if (!d)
        return false;
    if (syncRunState())
        return true;
    fallback(d->id, gen); // a start/stop that failed or hung: the device is dead
    return false;
}

void CoreAudioOutput::Impl::cmdShutdown()
{
    teardownCur();
    runTimed([this] { removeSystemListeners(); });
}

// ---- public API -----------------------------------------------------------------------------

CoreAudioOutput::CoreAudioOutput(AudioRuntime& runtime) : impl_(std::make_shared<Impl>(runtime))
{
    impl_->startTimer();
}

CoreAudioOutput::~CoreAudioOutput()
{
    Impl& i = *impl_;
    i.sh->cycleEnabled = false; // first: onCycle is never called again, whatever the waits below do
    std::shared_ptr<Dev> d;
    {
        std::lock_guard<std::mutex> g(i.mtx);
        i.dying = true; // fences install()/status/handlers: nothing new starts after this
        i.onStatus = nullptr;
        i.running = false;
        i.wantOpen = false;
        d = i.cur;
    }
    if (d)
        d->rt.store(nullptr); // the IOProc goes silent now, whatever happens to the teardown
    if (i.timer)
        dispatch_source_cancel(i.timer);
    // Fence: the shutdown command runs after whatever the control queue is doing (which stands
    // down at its next step because `dying` is set). Wait for it at most 2 s.
    const bool ok = i.runCmd([&i] { i.cmdShutdown(); return true; }, kOpTimeout, true);
    (void)ok;
    // else: a HAL call is wedged (or the queue is slow). Do not wait for it at quit: the queued
    // work only touches the Impl, which stays alive until it is done (a deliberate leak if wedged).
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
    uint64_t gen;
    {
        // Intent first: it stands even if the wait below expires.
        std::lock_guard<std::mutex> g(i.mtx);
        if (i.dying)
            return false;
        i.running = false;
        i.wantOpen = true;
        gen = ++i.openGen;
    }
    return i.runCmd([&i, id, leftCh, rightCh, gen] { return i.cmdOpen(id, leftCh, rightCh, gen); });
}

bool CoreAudioOutput::start()
{
    Impl& i = *impl_;
    {
        std::lock_guard<std::mutex> g(i.mtx);
        if (i.dying || !i.cur)
            return false;
        i.running = true;
    }
    i.runCmd([&i] { return i.cmdSync(); });
    return isRunning();
}

void CoreAudioOutput::stop()
{
    Impl& i = *impl_;
    {
        std::lock_guard<std::mutex> g(i.mtx);
        if (i.dying)
            return;
        i.running = false;
    }
    i.runCmd([&i] { return i.cmdSync(); });
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
