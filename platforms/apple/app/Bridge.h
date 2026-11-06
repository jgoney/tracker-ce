// Objective-C façade over the C++ Apple port (PlatformApple, AudioRuntime, CoreAudioOutput,
// MidiCoreMidi, MetalRenderer) for the Swift app. Pure Objective-C: no C++ in this header.
//
// Threading: post*/draw/underruns/status are cheap and thread-safe. Anything that talks to
// Core Audio or CoreMIDI (-refreshDevices:, -selectAudioDevice:..., -start, -stop) can block for
// seconds on a hung driver: call them OFF the main thread (-start/-stop/-refreshDevices: already
// hop to their own background queue and return at once).
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

@interface TCBridge : NSObject

- (instancetype)initWithSDRoot:(NSString*)path;

/// Metal device the renderer was created with (give it to the MTKView).
@property(nonatomic, readonly, nullable) id<MTLDevice> device;
@property(nonatomic, readonly) MTLPixelFormat pixelFormat;

/// Audio output the next -start / -selectAudioDevice should use (device name, "" = system
/// default). Set before -start.
- (void)setPreferredAudioDevice:(NSString*)name left:(int)l right:(int)r;
/// MIDI destination identity (a key from -midiDestinationKeys, a bare name, or "" = none)
/// applied by -start.
- (void)setPreferredMidiDestination:(NSString*)name;

/// Boots the core and opens the audio output on a background queue. Returns at once.
- (void)start;
/// Teardown (docs/apple_port.md order): stop runtime + output, detach MIDI. Nothing is destroyed:
/// everything stays allocated until process exit. Blocks (bounded, up to ~4 s with a hung
/// driver): call off the main thread. Idempotent.
- (void)stop;

- (void)postPad:(int)i down:(BOOL)d;
- (void)postButton:(int)i down:(BOOL)d;
- (void)postEncoder:(int)delta;

/// Re-reads the audio and MIDI device lists (and every channel count) on a background queue, then
/// calls `done` on the main queue. Until then the accessors below return the previous (initially
/// empty) lists. The same refresh runs on HAL device-list / default-output and CoreMIDI
/// setup-change notifications, followed by `onDevicesChanged` on the main queue.
- (void)refreshDevices:(void (^)(void))done;
@property(nonatomic, copy, nullable) void (^onDevicesChanged)(void);
/// Index 0 is always "System Default".
- (NSArray<NSString*>*)audioDevices;
/// Cached by -refreshDevices: (index 0 = the system default's); never queries the HAL.
- (int)audioChannelCountAtIndex:(NSInteger)i;
/// Blocking (<= ~2.5 s): call off the main thread. Opens and starts the device with this name
/// ("" = system default), resolved against the current list at call time. NO if not found.
- (BOOL)selectAudioDeviceNamed:(NSString*)name left:(int)l right:(int)r;
/// Display names and, index for index, stable identity keys of the MIDI destinations.
- (NSArray<NSString*>*)midiDestinations;
- (NSArray<NSString*>*)midiDestinationKeys;
/// Selects the destination by identity (unique id, else name), resolved at apply time on a
/// background queue; "" deselects (the virtual source "Tracker CE (unofficial)" always publishes).
- (void)selectMidiDestinationKey:(NSString*)key;
@property(nonatomic) int midiOffsetMs; // -50...+50

/// Latest audio output status line ("Opened: X", "Fell back to X", "Failed", ...). Thread-safe.
@property(nonatomic, readonly, copy) NSString* audioStatus;
/// Called (any thread) whenever audioStatus changes.
@property(nonatomic, copy, nullable) void (^onAudioStatus)(NSString* status);
@property(nonatomic, readonly) uint64_t underruns;
@property(nonatomic, readonly) uint64_t blocksRendered;
@property(nonatomic, readonly) double outputLatencyMs;
// Worst render-thread stage times (µs) since the previous call: [suspend, render, maskWait].
- (NSArray<NSNumber*>*)takeTimingMaxUs;

- (void)drawInEncoder:(id<MTLRenderCommandEncoder>)e size:(CGSize)s;
/// Renders the latest frame offscreen to a PNG (800x480 x scale). False if there is no frame.
- (BOOL)saveScreenshot:(NSString*)path;

#ifdef TRACKERCE_TEST_HOOKS
/// True while InputApple (the core's view) has the pad down. Test hook.
- (BOOL)isPadDown:(int)i;

/// Test hook (--test-clock-play): turn on USB clock out (on the tracker thread) and press Play.
- (void)testClockAndPlay;
#endif

@end

NS_ASSUME_NONNULL_END
