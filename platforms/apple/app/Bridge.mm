#import "Bridge.h"

#include "AudioRuntime.h"
#include "CoreAudioOutput.h"
#include "MetalRenderer.h"
#include "PlatformApple.h"
#include "Tracker.h"
#include "mtMidi.h"
#include "mtStructs.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreMIDI/CoreMIDI.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
void startDisplayNoop(Platform*) {}

// MIDI destination identity string persisted by the app: "<kMIDIPropertyUniqueID>|<display name>".
// A bare string (an older preference) is a name.
std::string midiKeyFor(const MidiCoreMidi::DestinationInfo& d)
{
    return std::to_string(d.uniqueId) + "|" + d.name;
}
MidiCoreMidi::DestinationInfo parseMidiKey(const std::string& key)
{
    const size_t bar = key.find('|');
    if (bar != std::string::npos && bar > 0)
    {
        char* end = nullptr;
        const long v = strtol(key.c_str(), &end, 10);
        if (end == key.c_str() + bar)
            return { (int32_t)v, key.substr(bar + 1) };
    }
    return { 0, key };
}
} // namespace

@implementation TCBridge
{
    std::unique_ptr<PlatformApple> _platform;
    std::unique_ptr<AudioRuntime> _rt;
    std::unique_ptr<CoreAudioOutput> _out;
    std::unique_ptr<MetalRenderer> _renderer;
    id<MTLDevice> _device;
    dispatch_queue_t _enumQueue; // device enumeration (may hang)
    dispatch_queue_t _control; // serial: start / stop / device selection
    dispatch_queue_t _listQueue; // serial: device list refreshes (HAL / CoreMIDI queries)
    dispatch_queue_t _midiQueue; // serial: MIDI destination selection
    dispatch_queue_t _notifyQueue; // device-change notifications (debounce, HAL listener)
    std::atomic<bool> _changePending;
    std::atomic<bool> _monitoring;
    AudioObjectPropertyListenerBlock _halListener;
    std::atomic<bool> _running; // platform alive and input accepted
    std::atomic<bool> _stopped;

    std::mutex _lock; // guards the lists, status and preferences below
    std::vector<CoreAudioOutput::DeviceInfo> _devices;
    NSArray<NSString*>* _audioNames;
    NSArray<NSString*>* _midiNames;
    NSArray<NSString*>* _midiKeys;
    int _defaultChannels; // output channels of the system default device (read off-main, cached)
    NSString* _status;
    std::string _prefDevice;
    int _prefL, _prefR;
    std::string _prefMidi;
    int _midiOffset;
}

- (instancetype)initWithSDRoot:(NSString*)path
{
    if (!(self = [super init]))
        return nil;
    _control = dispatch_queue_create("local.trackerce.control", DISPATCH_QUEUE_SERIAL);
    _enumQueue = dispatch_queue_create("local.trackerce.enum", DISPATCH_QUEUE_SERIAL);
    _audioNames = @[ @"System Default" ];
    _midiNames = @[];
    _midiKeys = @[];
    _defaultChannels = 2;
    _changePending = false;
    _monitoring = false;
    _listQueue = dispatch_queue_create("local.trackerce.list", DISPATCH_QUEUE_SERIAL);
    _midiQueue = dispatch_queue_create("local.trackerce.midi", DISPATCH_QUEUE_SERIAL);
    _notifyQueue = dispatch_queue_create("local.trackerce.notify", DISPATCH_QUEUE_SERIAL);
    _status = @"Starting…";
    _prefL = 0;
    _prefR = 1;
    _running = false;
    _stopped = false;

    _platform = std::make_unique<PlatformApple>(std::string(path.fileSystemRepresentation));
    _device = MTLCreateSystemDefaultDevice();
    if (_device)
        _renderer = std::make_unique<MetalRenderer>(_device, MTLPixelFormatBGRA8Unorm);
    if (_renderer && !_renderer->ok())
        _renderer.reset();
    if (_renderer)
        _renderer->attachSnapshotHook(_platform->display()); // before the tracker thread starts
    _rt = std::make_unique<AudioRuntime>(Tracker::getInstance(), _platform.get());
    _out = std::make_unique<CoreAudioOutput>(*_rt);
    // attachOutput before the first start(): the onCycle std::function is not synchronised.
    _platform->midi().attachOutput(*_out);
    __weak TCBridge* weak = self;
    _out->setOnStatus([weak](CoreAudioOutput::Status s, std::string name) {
        TCBridge* me = weak;
        if (!me)
            return;
        NSString* n = [NSString stringWithUTF8String:name.c_str()] ?: @"";
        switch (s)
        {
        case CoreAudioOutput::Status::Opened:
            [me setStatus:[NSString stringWithFormat:@"Opened: %@", n]];
            break;
        case CoreAudioOutput::Status::FellBack:
            [me setStatus:[NSString stringWithFormat:@"Fell back to: %@", n]];
            break;
        case CoreAudioOutput::Status::Failed:
            [me setStatus:@"Failed: no audio device could be opened (retrying on device changes)"];
            break;
        }
    });
    return self;
}

- (void)setStatus:(NSString*)s
{
    void (^cb)(NSString*) = nil;
    {
        std::lock_guard<std::mutex> g(_lock);
        _status = [s copy];
        cb = _onAudioStatus;
    }
    if (cb)
        cb(s);
}

- (NSString*)audioStatus
{
    std::lock_guard<std::mutex> g(_lock);
    return _status;
}

- (id<MTLDevice>)device { return _device; }
- (MTLPixelFormat)pixelFormat { return MTLPixelFormatBGRA8Unorm; }

- (void)setPreferredAudioDevice:(NSString*)name left:(int)l right:(int)r
{
    std::lock_guard<std::mutex> g(_lock);
    _prefDevice = name.UTF8String ?: "";
    _prefL = l;
    _prefR = r;
}

- (void)setPreferredMidiDestination:(NSString*)name
{
    std::lock_guard<std::mutex> g(_lock);
    _prefMidi = name.UTF8String ?: "";
}

// Opens `id` (0 = system default) on l/r and starts it. Control queue / background only.
- (BOOL)openAndStart:(AudioDeviceID)devId left:(int)l right:(int)r
{
    if (_stopped)
        return NO;
    if (devId == 0)
        devId = CoreAudioOutput::defaultOutputDevice();
    if (devId == kAudioObjectUnknown)
    {
        [self setStatus:@"Failed: no audio output device found"];
        return NO;
    }
    // If open() fails on the requested device CoreAudioOutput falls back on its own and reports
    // through onStatus, so start whatever it ended up with either way.
    const BOOL ok = _out->open(devId, (uint8_t)l, (uint8_t)r);
    _out->start();
    return ok;
}

- (void)start
{
    dispatch_async(_control, ^{
        if (self->_stopped || self->_running)
            return;
        self->_running = true;
        self->_rt->start(); // boots the core on the tracker thread
        std::string dev, midi;
        int l, r;
        {
            std::lock_guard<std::mutex> g(self->_lock);
            dev = self->_prefDevice;
            midi = self->_prefMidi;
            l = self->_prefL;
            r = self->_prefR;
        }
        // Enumeration can hang on a bad driver: do it on its own queue so -stop (on _control)
        // never waits behind it, then hop back to open the device.
        dispatch_async(self->_enumQueue, ^{
#ifdef TRACKERCE_TEST_HOOKS
            if (getenv("TRACKERCE_TEST_HANG_ENUM")) // test hook: hung listOutputDevices
                for (;;)
                    sleep(1000);
#endif
            AudioDeviceID id = 0;
            if (!dev.empty())
            {
                for (auto& d : CoreAudioOutput::listOutputDevices())
                    if (d.name == dev && (uint32_t)std::max(l, r) < d.outChannels)
                        id = d.id;
            }
            dispatch_async(self->_control, ^{
                if (self->_stopped)
                    return;
                [self openAndStart:id left:id ? l : 0 right:id ? r : 1];
            });
            if (!midi.empty())
                [self selectMidiDestinationKey:[NSString stringWithUTF8String:midi.c_str()]];
            [self startMonitoring];
        });
    });
}

- (void)stop
{
    dispatch_sync(_control, ^{
        if (self->_stopped)
            return;
        self->_stopped = true;
        self->_running = false;
#ifdef TRACKERCE_TEST_HOOKS
        if (getenv("TRACKERCE_TEST_HANG_STOP")) // test hook: a wedged teardown
            for (;;)
                sleep(1000);
#endif
        self->_out->stop();
        self->_rt->stop();
        if (self->_halListener)
        {
            AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
            AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &a, self->_notifyQueue, self->_halListener);
            AudioObjectPropertyAddress d = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
            AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &d, self->_notifyQueue, self->_halListener);
            self->_halListener = nil;
        }
        // After the tracker thread has stopped: the hook is a plain std::function it invokes.
        if (self->_renderer)
            MetalRenderer::detachSnapshotHook(self->_platform->display());
        self->_platform->midi().detachOutput(*self->_out);
        // Nothing is destroyed here: everything stays allocated until process exit (the Tracker
        // singleton still points at the platform, and the bridge lives as long as the app).
    });
}

- (void)postPad:(int)i down:(BOOL)d
{
    if (_running && i >= 0 && i < 48)
        _platform->input().post({ InputEvent::Pad, (uint8_t)i, (bool)d, 0 });
}
- (void)postButton:(int)i down:(BOOL)d
{
    if (_running && i >= 0 && i < 44)
        _platform->input().post({ InputEvent::Button, (uint8_t)i, (bool)d, 0 });
}
- (void)postEncoder:(int)delta
{
    if (_running && delta != 0)
        _platform->input().post({ InputEvent::Encoder, 0, false, delta });
}

- (void)refreshDevices:(void (^)(void))done
{
    // Every HAL / CoreMIDI query happens here, off the main thread and outside _lock; the results
    // (including the default device's channel count) are cached for the UI accessors.
    dispatch_async(_listQueue, ^{
        auto devs = CoreAudioOutput::listOutputDevices(); // may block on a hung driver
        const AudioDeviceID def = CoreAudioOutput::defaultOutputDevice();
        auto midi = MidiCoreMidi::listDestinationInfos();
        int defChannels = 2;
        for (auto& d : devs)
            if (d.id == def)
                defChannels = (int)d.outChannels;
        NSMutableArray* an = [NSMutableArray arrayWithObject:@"System Default"];
        for (auto& d : devs)
            [an addObject:[NSString stringWithUTF8String:d.name.c_str()] ?: @"?"];
        NSMutableArray* mn = [NSMutableArray array];
        NSMutableArray* mk = [NSMutableArray array];
        for (auto& m : midi)
        {
            [mn addObject:[NSString stringWithUTF8String:m.name.c_str()] ?: @"?"];
            [mk addObject:[NSString stringWithUTF8String:midiKeyFor(m).c_str()] ?: @"0|?"];
        }
        {
            std::lock_guard<std::mutex> g(self->_lock);
            self->_devices = devs;
            self->_audioNames = an;
            self->_midiNames = mn;
            self->_midiKeys = mk;
            self->_defaultChannels = defChannels;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (done)
                done();
        });
    });
}

// HAL device-list / default-output and CoreMIDI setup-change notifications: refresh both lists off
// the main thread (debounced: a plug event fires several), then publish on main.
- (void)devicesChanged
{
    if (_stopped || _changePending.exchange(true))
        return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), _notifyQueue, ^{
        self->_changePending = false;
        if (self->_stopped)
            return;
        [self refreshDevices:^{
            void (^cb)(void) = self.onDevicesChanged;
            if (cb)
                cb();
        }];
    });
}

- (void)startMonitoring
{
    if (_stopped || _monitoring.exchange(true))
        return;
    __weak TCBridge* weak = self;
    dispatch_async(_notifyQueue, ^{
        TCBridge* me = weak;
        if (!me || me->_stopped)
            return;
        me->_halListener = ^(UInt32, const AudioObjectPropertyAddress*) { [weak devicesChanged]; };
        AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, me->_notifyQueue, me->_halListener);
        AudioObjectPropertyAddress d = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &d, me->_notifyQueue, me->_halListener);
    });
    // CoreMIDI delivers setup-change notifications on the run loop of the thread that created the
    // client: give it a thread that spins one.
    NSThread* t = [[NSThread alloc] initWithBlock:^{
        MIDIClientRef client = 0;
        MIDIClientCreateWithBlock(CFSTR("Tracker CE device watch"), &client, ^(const MIDINotification* n) {
            if (n->messageID == kMIDIMsgSetupChanged)
                [weak devicesChanged];
        });
        // A run loop with no sources returns from CFRunLoopRunInMode at once, which turned this
        // loop into a busy spin (a full core). A port that never fires keeps it sleeping.
        [[NSRunLoop currentRunLoop] addPort:[NSMachPort port] forMode:NSDefaultRunLoopMode];
        for (;;)
        {
            TCBridge* me = weak;
            if (!me || me->_stopped)
                break;
            me = nil;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
        }
        if (client)
            MIDIClientDispose(client);
    }];
    t.name = @"local.trackerce.midi-watch";
    [t start];
}

- (NSArray<NSString*>*)audioDevices
{
    std::lock_guard<std::mutex> g(_lock);
    return _audioNames;
}

// Cached by -refreshDevices: (never queries the HAL: this is called from SwiftUI body evaluation).
- (int)audioChannelCountAtIndex:(NSInteger)i
{
    std::lock_guard<std::mutex> g(_lock);
    if (i <= 0)
        return _defaultChannels;
    return i - 1 < (NSInteger)_devices.size() ? (int)_devices[i - 1].outChannels : 2;
}

- (BOOL)selectAudioDeviceNamed:(NSString*)name left:(int)l right:(int)r
{
    __block BOOL ok = NO;
    dispatch_sync(_control, ^{
        AudioDeviceID id = 0;
        const std::string want = name.UTF8String ?: "";
        if (!want.empty())
        {
            // Resolved against the current list at apply time, by name.
            std::lock_guard<std::mutex> g(self->_lock);
            for (auto& d : self->_devices)
                if (d.name == want)
                    id = d.id;
            if (!id)
                return;
        }
        ok = [self openAndStart:id left:l right:r];
    });
    return ok;
}

- (NSArray<NSString*>*)midiDestinations
{
    std::lock_guard<std::mutex> g(_lock);
    return _midiNames;
}

- (NSArray<NSString*>*)midiDestinationKeys
{
    std::lock_guard<std::mutex> g(_lock);
    return _midiKeys;
}

- (void)selectMidiDestinationKey:(NSString*)key
{
    const std::string k = key.UTF8String ?: "";
    dispatch_async(_midiQueue, ^{
        if (self->_stopped)
            return;
        if (k.empty())
        {
            self->_platform->midi().selectDestination(-1);
            return;
        }
        // Resolved against the live CoreMIDI list now, by unique id then name.
        const auto id = parseMidiKey(k);
        self->_platform->midi().selectDestinationByIdentity(id.uniqueId, id.name);
    });
}

- (void)setMidiOffsetMs:(int)ms
{
    if (_stopped)
        return;
    _platform->midi().setUserOffsetMs(ms);
    _midiOffset = _platform->midi().userOffsetMs();
}
- (int)midiOffsetMs { return _midiOffset; }

- (uint64_t)underruns { return _rt ? _rt->underruns.load() : 0; }
- (uint64_t)blocksRendered { return _rt ? _rt->blocksRendered.load() : 0; }
- (NSArray<NSNumber*>*)takeTimingMaxUs
{
    if (!_rt)
        return @[ @0, @0, @0 ];
    return @[ @(AudioRuntime::takeMax(_rt->maxSuspendUs)), @(AudioRuntime::takeMax(_rt->maxRenderUs)),
              @(AudioRuntime::takeMax(_rt->maxMaskWaitUs)) ];
}
- (double)outputLatencyMs { return _out ? _out->outputLatencySeconds() * 1000.0 : 0; }

- (void)drawInEncoder:(id<MTLRenderCommandEncoder>)e size:(CGSize)s
{
    if (!_renderer || !_renderer->ok() || _stopped)
        return;
    if (auto frame = _platform->display().latestFrame())
        _renderer->draw(*frame, e, s);
}

- (BOOL)saveScreenshot:(NSString*)path
{
    if (!_renderer || _stopped)
        return NO;
    auto frame = _platform->display().latestFrame();
    return frame && _renderer->renderToPNG(*frame, path.fileSystemRepresentation);
}

#ifdef TRACKERCE_TEST_HOOKS
- (BOOL)isPadDown:(int)i
{
    return _running && i >= 0 && i < 48 && _platform->input().pads().isButtonPressed(i);
}

- (void)testClockAndPlay
{
    // The config write runs on the tracker thread (drained by InputApple::dispatch), not here.
    _platform->input().postTask([] { mtConfig.midi.clkOut = clockOut_usb; });
    [self postButton:23 down:YES];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [self postButton:23 down:NO];
    });
}
#endif

@end
