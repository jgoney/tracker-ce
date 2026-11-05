#import "Bridge.h"

#include "AudioRuntime.h"
#include "CoreAudioOutput.h"
#include "MetalRenderer.h"
#include "PlatformApple.h"
#include "Tracker.h"
#include "mtMidi.h"
#include "mtStructs.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
void startDisplayNoop(Platform*) {}
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
    std::atomic<bool> _running; // platform alive and input accepted
    std::atomic<bool> _stopped;

    std::mutex _lock; // guards the lists, status and preferences below
    std::vector<CoreAudioOutput::DeviceInfo> _devices;
    NSArray<NSString*>* _audioNames;
    NSArray<NSString*>* _midiNames;
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
    BOOL ok = devId != kAudioObjectUnknown && _out->open(devId, (uint8_t)l, (uint8_t)r);
    if (!ok && devId != kAudioObjectUnknown)
    {
        // open() failed on the requested device: CoreAudioOutput falls back on its own and reports
        // through onStatus, so just try to start whatever it ended up with.
    }
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
            if (getenv("TRACKERCE_TEST_HANG_ENUM")) // test hook: hung listOutputDevices
                for (;;)
                    sleep(1000);
            AudioDeviceID id = 0;
            if (!dev.empty())
            {
                for (auto& d : CoreAudioOutput::listOutputDevices())
                    if (d.name == dev && (uint32_t)std::max(l, r) < d.outChannels)
                        id = d.id;
            }
            int midiIdx = -1;
            if (!midi.empty())
            {
                auto dests = MidiCoreMidi::listDestinations();
                for (size_t i = 0; i < dests.size(); i++)
                    if (dests[i] == midi)
                        midiIdx = (int)i;
            }
            dispatch_async(self->_control, ^{
                if (self->_stopped)
                    return;
                [self openAndStart:id left:id ? l : 0 right:id ? r : 1];
                if (midiIdx >= 0)
                    self->_platform->midi().selectDestination(midiIdx);
            });
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
        if (getenv("TRACKERCE_TEST_HANG_STOP")) // test hook: a wedged teardown
            for (;;)
                sleep(1000);
        self->_out->stop();
        self->_rt->stop();
        // After the tracker thread has stopped: the hook is a plain std::function it invokes.
        if (self->_renderer)
            MetalRenderer::detachSnapshotHook(self->_platform->display());
        self->_platform->midi().detachOutput(*self->_out);
        // Everything stays allocated until dealloc/process exit: the Tracker singleton still
        // points at the platform. Members destroy in reverse order (output, runtime, platform
        // with its MidiCoreMidi), which is the documented teardown order.
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
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto devs = CoreAudioOutput::listOutputDevices(); // may block on a hung driver
        auto midi = MidiCoreMidi::listDestinations();
        NSMutableArray* an = [NSMutableArray arrayWithObject:@"System Default"];
        for (auto& d : devs)
            [an addObject:[NSString stringWithUTF8String:d.name.c_str()] ?: @"?"];
        NSMutableArray* mn = [NSMutableArray array];
        for (auto& m : midi)
            [mn addObject:[NSString stringWithUTF8String:m.c_str()] ?: @"?"];
        {
            std::lock_guard<std::mutex> g(self->_lock);
            self->_devices = devs;
            self->_audioNames = an;
            self->_midiNames = mn;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (done)
                done();
        });
    });
}

- (NSArray<NSString*>*)audioDevices
{
    std::lock_guard<std::mutex> g(_lock);
    return _audioNames;
}

- (int)audioChannelCountAtIndex:(NSInteger)i
{
    std::lock_guard<std::mutex> g(_lock);
    if (i <= 0)
    {
        AudioDeviceID def = CoreAudioOutput::defaultOutputDevice();
        for (auto& d : _devices)
            if (d.id == def)
                return (int)d.outChannels;
        return 2;
    }
    return i - 1 < (NSInteger)_devices.size() ? (int)_devices[i - 1].outChannels : 2;
}

- (BOOL)selectAudioDevice:(NSInteger)i left:(int)l right:(int)r
{
    __block BOOL ok = NO;
    dispatch_sync(_control, ^{
        AudioDeviceID id = 0;
        {
            std::lock_guard<std::mutex> g(self->_lock);
            if (i > 0 && i - 1 < (NSInteger)self->_devices.size())
                id = self->_devices[i - 1].id;
            else if (i > 0)
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

- (BOOL)selectMidiDestination:(NSInteger)i
{
    if (_stopped)
        return NO;
    return _platform->midi().selectDestination((int)i);
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

- (BOOL)isPadDown:(int)i
{
    return _running && i >= 0 && i < 48 && _platform->input().pads().isButtonPressed(i);
}

- (void)testClockAndPlay
{
    mtConfig.midi.clkOut = clockOut_usb;
    [self postButton:23 down:YES];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [self postButton:23 down:NO];
    });
}

@end
