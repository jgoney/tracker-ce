// MacTypes.h (via Core Audio) declares a global `Rect`; display/Frame.h (via AudioRuntime.h)
// declares another. Rename the system one while its headers are read.
#define Rect MacTypesRect
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#undef Rect

#include "CoreAudioOutput.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <os/workgroup.h>
#include <thread>

namespace
{
constexpr UInt32 kWantedBufferFrames = 256; // ring is 4 blocks (512 frames): keep well inside it

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
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || sz < sizeof(AudioBufferList))
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

std::string deviceName(AudioDeviceID dev)
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
} // namespace

struct CoreAudioOutput::Impl
{
    static OSStatus renderCb(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);
    static OSStatus converterInput(AudioConverterRef, UInt32*, AudioBufferList*, AudioStreamPacketDescription**, void*);
    static OSStatus propertyCb(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*);
    static void deadFn(void*);
    static void defaultFn(void*);
    static void rateFn(void*);
};

// ---- discovery ------------------------------------------------------------------------------

std::vector<CoreAudioOutput::DeviceInfo> CoreAudioOutput::listOutputDevices()
{
    std::vector<DeviceInfo> out;
    AudioObjectPropertyAddress a{ kAudioHardwarePropertyDevices,
                                  kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr)
        return out;
    std::vector<AudioDeviceID> ids(sz / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) !=
        noErr)
        return out;
    for (AudioDeviceID id : ids)
    {
        const uint32_t ch = outputChannels(id);
        if (ch > 0)
            out.push_back({ id, deviceName(id), ch });
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

// ---- lifetime -------------------------------------------------------------------------------

CoreAudioOutput::CoreAudioOutput(AudioRuntime& runtime) : rt_(runtime)
{
    queue_ = (void*)dispatch_queue_create("trackerce.coreaudio.control", DISPATCH_QUEUE_SERIAL);
}

CoreAudioOutput::~CoreAudioOutput()
{
    {
        std::lock_guard<std::mutex> g(mutex_);
        closeLocked(); // removes the listeners: nothing new gets queued
    }
    dispatch_sync((dispatch_queue_t)queue_, ^{}); // drain handlers that already hold `this`
    dispatch_release((dispatch_queue_t)queue_);
}

bool CoreAudioOutput::isOpen() const
{
    return deviceId_.load() != kAudioObjectUnknown;
}

bool CoreAudioOutput::open(AudioDeviceID id, uint8_t leftCh, uint8_t rightCh)
{
    std::lock_guard<std::mutex> g(mutex_);
    running_ = false;
    const bool ok = openLocked(id, leftCh, rightCh, true);
    followingDefault_ = ok && id == defaultOutputDevice();
    return ok;
}

bool CoreAudioOutput::start()
{
    std::lock_guard<std::mutex> g(mutex_);
    if (!unit_)
        return false;
    if (running_)
        return true;
    if (AudioOutputUnitStart((AudioUnit)unit_) != noErr)
        return false;
    running_ = true;
    return true;
}

void CoreAudioOutput::stop()
{
    std::lock_guard<std::mutex> g(mutex_);
    if (unit_ && running_)
        AudioOutputUnitStop((AudioUnit)unit_);
    running_ = false;
}

void CoreAudioOutput::closeLocked()
{
    removeListeners();
    if (unit_)
    {
        AudioOutputUnitStop((AudioUnit)unit_); // returns after the last IOProc cycle
        AudioUnitUninitialize((AudioUnit)unit_);
        AudioComponentInstanceDispose((AudioUnit)unit_);
        unit_ = nullptr;
    }
    if (converter_)
    {
        AudioConverterDispose((AudioConverterRef)converter_);
        converter_ = nullptr;
    }
    deviceId_ = kAudioObjectUnknown;
    usingConverter_ = false;
}

// ---- open -----------------------------------------------------------------------------------

bool CoreAudioOutput::openLocked(AudioDeviceID id, uint8_t l, uint8_t r, bool requestRate)
{
    closeLocked();
    if (id == kAudioObjectUnknown)
        return false;
    const uint32_t devCh = outputChannels(id);
    if (devCh == 0 || l >= devCh)
        return false;
    if (r >= devCh)
        r = l;

    // Rate: ask for the runtime's rate, else take what the device runs at.
    double rate = nominalRate(id);
    if (requestRate && std::fabs(rate - AudioRuntime::kSampleRate) > 0.5 &&
        rateSupported(id, AudioRuntime::kSampleRate))
    {
        Float64 want = AudioRuntime::kSampleRate;
        AudioObjectPropertyAddress a{ kAudioDevicePropertyNominalSampleRate,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
        AudioObjectSetPropertyData(id, &a, 0, nullptr, sizeof want, &want);
        for (int i = 0; i < 50; i++) // the change completes asynchronously
        {
            rate = nominalRate(id);
            if (std::fabs(rate - AudioRuntime::kSampleRate) <= 0.5)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    rate = nominalRate(id);
    if (rate <= 0)
        return false;

    // Small IO buffers (clamped to what the device offers).
    {
        AudioValueRange range{ 0, 0 };
        UInt32 frames = kWantedBufferFrames;
        if (getProp(id, kAudioDevicePropertyBufferFrameSizeRange, range))
            frames = (UInt32)std::clamp<double>(frames, range.mMinimum, range.mMaximum);
        AudioObjectPropertyAddress a{ kAudioDevicePropertyBufferFrameSize,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
        AudioObjectSetPropertyData(id, &a, 0, nullptr, sizeof frames, &frames);
    }
    UInt32 bufFrames = 0;
    getProp(id, kAudioDevicePropertyBufferFrameSize, bufFrames);

    AudioComponentDescription desc{ kAudioUnitType_Output,
                                    kAudioUnitSubType_HALOutput,
                                    kAudioUnitManufacturer_Apple,
                                    0,
                                    0 };
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    AudioUnit unit = nullptr;
    if (!comp || AudioComponentInstanceNew(comp, &unit) != noErr)
        return false;
    auto fail = [&]
    {
        AudioComponentInstanceDispose(unit);
        if (converter_)
        {
            AudioConverterDispose((AudioConverterRef)converter_);
            converter_ = nullptr;
        }
        return false;
    };

    UInt32 on = 1, off = 0;
    if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &on, sizeof on) != noErr ||
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &off, sizeof off) != noErr ||
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &id, sizeof id) != noErr)
        return fail();

    // Client format: float, interleaved, every device channel, at the device rate. We write
    // the mix into leftCh/rightCh and zeros elsewhere.
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    fmt.mChannelsPerFrame = devCh;
    fmt.mBitsPerChannel = 32;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = fmt.mBytesPerPacket = 4 * devCh;
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt) != noErr)
        return fail();

    UInt32 maxFrames = 0, sz = sizeof maxFrames;
    AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, &sz);
    if (maxFrames < bufFrames || maxFrames == 0)
    {
        maxFrames = std::max<UInt32>(bufFrames, 1024);
        AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
    }

    const bool convert = std::fabs(rate - AudioRuntime::kSampleRate) > 0.5;
    srcCap_ = 0;
    if (convert)
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
        dst.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
        dst.mBytesPerFrame = dst.mBytesPerPacket = 8;
        if (AudioConverterNew(&src, &dst, (AudioConverterRef*)&converter_) != noErr)
            return fail();
        srcCap_ = (size_t)std::ceil(maxFrames * AudioRuntime::kSampleRate / rate) + 64;
        srcL_.assign(srcCap_, 0.f);
        srcR_.assign(srcCap_, 0.f);
        mixOut_.assign((size_t)maxFrames * 2, 0.f);
    }
    else
    {
        srcL_.assign(maxFrames, 0.f);
        srcR_.assign(maxFrames, 0.f);
        srcCap_ = maxFrames;
    }

    channels_ = devCh;
    maxFrames_ = maxFrames;
    leftCh_ = l;
    rightCh_ = r;

    AURenderCallbackStruct cb{ &CoreAudioOutput::Impl::renderCb, this };
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
        AudioUnitInitialize(unit) != noErr)
        return fail();

    // Latency: device + safety offset + stream + buffer, in frames at the device rate.
    UInt32 devLat = 0, safety = 0, streamLat = 0;
    getProp(id, kAudioDevicePropertyLatency, devLat, kAudioObjectPropertyScopeOutput);
    getProp(id, kAudioDevicePropertySafetyOffset, safety, kAudioObjectPropertyScopeOutput);
    {
        AudioObjectPropertyAddress a{ kAudioDevicePropertyStreams,
                                      kAudioObjectPropertyScopeOutput,
                                      kAudioObjectPropertyElementMain };
        UInt32 ssz = 0;
        if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &ssz) == noErr && ssz >= sizeof(AudioStreamID))
        {
            std::vector<AudioStreamID> streams(ssz / sizeof(AudioStreamID));
            if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &ssz, streams.data()) == noErr)
                getProp(streams[0], kAudioStreamPropertyLatency, streamLat);
        }
    }
    latencySeconds_ = ((double)devLat + safety + streamLat + bufFrames) / rate;
    deviceRate_ = rate;
    bufferFrames_ = bufFrames;
    usingConverter_ = convert;

    unit_ = unit;
    deviceId_ = id;
    joinWorkgroup(id);
    addListeners();
    return true;
}

void CoreAudioOutput::joinWorkgroup(AudioDeviceID id)
{
    os_workgroup_t wg = nullptr;
    if (!getProp(id, kAudioDevicePropertyIOThreadOSWorkgroup, wg) || !wg)
        return;
    workgroup_ = (void*)wg;
    rt_.setWorkgroup(workgroup_);
}

// ---- IOProc ---------------------------------------------------------------------------------

OSStatus CoreAudioOutput::Impl::converterInput(AudioConverterRef,
                                         UInt32* packets,
                                         AudioBufferList* data,
                                         AudioStreamPacketDescription**,
                                         void* ref)
{
    auto* self = (CoreAudioOutput*)ref;
    const size_t want = std::min<size_t>(*packets, self->srcCap_);
    self->rt_.pull(self->srcL_.data(), self->srcR_.data(), want);
    data->mNumberBuffers = 2;
    data->mBuffers[0] = { 1, (UInt32)(want * 4), self->srcL_.data() };
    data->mBuffers[1] = { 1, (UInt32)(want * 4), self->srcR_.data() };
    *packets = (UInt32)want;
    return noErr;
}

OSStatus CoreAudioOutput::Impl::renderCb(void* ref,
                                   AudioUnitRenderActionFlags*,
                                   const AudioTimeStamp* ts,
                                   UInt32,
                                   UInt32 frames,
                                   AudioBufferList* io)
{
    auto* self = (CoreAudioOutput*)ref;
    if (io->mNumberBuffers != 1)
    {
        for (UInt32 i = 0; i < io->mNumberBuffers; i++)
            std::memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
        return noErr;
    }
    const uint32_t ch = self->channels_;
    float* out = (float*)io->mBuffers[0].mData;
    std::memset(out, 0, (size_t)frames * ch * sizeof(float));
    if (frames > self->maxFrames_)
        return noErr;

    self->cycles.fetch_add(1, std::memory_order_relaxed);
    if (self->onCycle)
    {
        const size_t pos = self->rt_.readPosition();
        self->onCycle(ts->mHostTime,
                      self->rt_.blockIndexAt(pos),
                      pos % AudioRuntime::kBlockFrames);
    }

    const uint32_t lc = self->leftCh_, rc = self->rightCh_;
    if (!self->converter_)
    {
        float* sl = self->srcL_.data();
        float* sr = self->srcR_.data();
        self->rt_.pull(sl, sr, frames);
        for (UInt32 i = 0; i < frames; i++)
        {
            out[(size_t)i * ch + lc] = sl[i];
            out[(size_t)i * ch + rc] = sr[i]; // rc == lc on a mono device: right wins
        }
    }
    else
    {
        AudioBufferList outList;
        outList.mNumberBuffers = 1;
        outList.mBuffers[0] = { 2, frames * 8, self->mixOut_.data() };
        UInt32 packets = frames;
        std::memset(self->mixOut_.data(), 0, (size_t)frames * 8);
        AudioConverterFillComplexBuffer((AudioConverterRef)self->converter_, &CoreAudioOutput::Impl::converterInput, self, &packets, &outList, nullptr);
        const float* m = self->mixOut_.data();
        for (UInt32 i = 0; i < packets; i++)
        {
            out[(size_t)i * ch + lc] = m[2 * i];
            out[(size_t)i * ch + rc] = m[2 * i + 1];
        }
    }
    return noErr;
}

// ---- device events --------------------------------------------------------------------------

void CoreAudioOutput::addListeners()
{
    const AudioObjectID dev = deviceId_.load();
    AudioObjectPropertyAddress alive{ kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress rate{ kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress def{ kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(dev, &alive, &CoreAudioOutput::Impl::propertyCb, this);
    AudioObjectAddPropertyListener(dev, &rate, &CoreAudioOutput::Impl::propertyCb, this);
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &def, &CoreAudioOutput::Impl::propertyCb, this);
    listening_ = true;
    listenedDevice_ = dev;
}

void CoreAudioOutput::removeListeners()
{
    if (!listening_)
        return;
    AudioObjectPropertyAddress alive{ kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress rate{ kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress def{ kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectRemovePropertyListener(listenedDevice_, &alive, &CoreAudioOutput::Impl::propertyCb, this);
    AudioObjectRemovePropertyListener(listenedDevice_, &rate, &CoreAudioOutput::Impl::propertyCb, this);
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &def, &CoreAudioOutput::Impl::propertyCb, this);
    listening_ = false;
}

// HAL notification thread: only hop to our queue (tearing the unit down here could deadlock).
OSStatus CoreAudioOutput::Impl::propertyCb(AudioObjectID,
                                     UInt32 n,
                                     const AudioObjectPropertyAddress* addrs,
                                     void* ref)
{
    auto* self = (CoreAudioOutput*)ref;
    for (UInt32 i = 0; i < n; i++)
    {
        switch (addrs[i].mSelector)
        {
            case kAudioDevicePropertyDeviceIsAlive:
                dispatch_async_f((dispatch_queue_t)self->queue_, self, &CoreAudioOutput::Impl::deadFn);
                break;
            case kAudioDevicePropertyNominalSampleRate:
                dispatch_async_f((dispatch_queue_t)self->queue_, self, &CoreAudioOutput::Impl::rateFn);
                break;
            case kAudioHardwarePropertyDefaultOutputDevice:
                dispatch_async_f((dispatch_queue_t)self->queue_, self, &CoreAudioOutput::Impl::defaultFn);
                break;
        }
    }
    return noErr;
}

void CoreAudioOutput::Impl::deadFn(void* ctx)
{
    auto* self = (CoreAudioOutput*)ctx;
    UInt32 alive = 1;
    const AudioDeviceID dev = self->deviceId_.load();
    // A failed read means the device is gone too.
    if (dev != kAudioObjectUnknown && getProp(dev, kAudioDevicePropertyDeviceIsAlive, alive) && alive)
        return;
    self->handleDeviceDead();
}

void CoreAudioOutput::Impl::defaultFn(void* ctx)
{
    static_cast<CoreAudioOutput*>(ctx)->handleDefaultChanged();
}

void CoreAudioOutput::Impl::rateFn(void* ctx)
{
    static_cast<CoreAudioOutput*>(ctx)->handleRateChanged();
}

// Reopens the default output (never the avoided device); keeps running_ as it was.
void CoreAudioOutput::reopenDefault(AudioDeviceID avoid)
{
    const bool wasRunning = running_.load();
    const AudioDeviceID def = defaultOutputDevice();
    closeLocked();
    followingDefault_ = true; // also when this fails: the next default change retries
    // The default can still be the dead device for a moment: the default-change listener
    // retries once the system moves on. (A device that is alive, e.g. a simulated loss or a
    // stream failure, is simply reopened.)
    UInt32 alive = 1;
    const bool avoidDead = avoid != kAudioObjectUnknown &&
      (!getProp(avoid, kAudioDevicePropertyDeviceIsAlive, alive) || !alive);
    if (def == kAudioObjectUnknown || (def == avoid && avoidDead))
        return;
    if (!openLocked(def, 0, 1, true))
        return;
    reopens++;
    if (wasRunning && AudioOutputUnitStart((AudioUnit)unit_) == noErr)
        running_ = true;
    else
        running_ = false;
}

void CoreAudioOutput::handleDeviceDead()
{
    std::lock_guard<std::mutex> g(mutex_);
    const AudioDeviceID dead = deviceId_.load();
    const bool wasRunning = running_.load();
    reopenDefault(dead);
    if (!isOpen())
        running_ = wasRunning; // nothing to play on yet; the default-change listener retries
}

void CoreAudioOutput::handleDefaultChanged()
{
    std::lock_guard<std::mutex> g(mutex_);
    const AudioDeviceID def = defaultOutputDevice();
    if (!followingDefault_ || def == kAudioObjectUnknown || def == deviceId_.load())
        return;
    reopenDefault(kAudioObjectUnknown);
}

void CoreAudioOutput::handleRateChanged()
{
    std::lock_guard<std::mutex> g(mutex_);
    const AudioDeviceID dev = deviceId_.load();
    if (dev == kAudioObjectUnknown || std::fabs(nominalRate(dev) - deviceRate_.load()) < 0.5)
        return;
    // Another app (or the user) changed the rate: follow it through the converter rather than
    // fighting for it.
    const bool wasRunning = running_.load();
    const uint8_t l = leftCh_, r = rightCh_;
    if (openLocked(dev, l, r, false))
    {
        reopens++;
        if (wasRunning && AudioOutputUnitStart((AudioUnit)unit_) == noErr)
            running_ = true;
        else
            running_ = false;
    }
}
