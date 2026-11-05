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
/// MIDI destination (name, "" = none) applied by -start.
- (void)setPreferredMidiDestination:(NSString*)name;

/// Boots the core and opens the audio output on a background queue. Returns at once.
- (void)start;
/// Teardown (docs/apple_port.md order): stop runtime + output, detach MIDI, destroy the platform.
/// Blocks (up to ~4 s with a hung driver): call off the main thread. Idempotent.
- (void)stop;

- (void)postPad:(int)i down:(BOOL)d;
- (void)postButton:(int)i down:(BOOL)d;
- (void)postEncoder:(int)delta;

/// Re-reads the audio and MIDI device lists on a background queue, then calls `done` on the main
/// queue. Until then the accessors below return the previous (initially empty) lists.
- (void)refreshDevices:(void (^)(void))done;
/// Index 0 is always "System Default".
- (NSArray<NSString*>*)audioDevices;
- (int)audioChannelCountAtIndex:(NSInteger)i;
/// Blocking (<= ~2.5 s): call off the main thread. Opens and starts the device.
- (BOOL)selectAudioDevice:(NSInteger)i left:(int)l right:(int)r;
- (NSArray<NSString*>*)midiDestinations;
/// i < 0 deselects (the virtual source "Tracker CE (unofficial)" always publishes).
- (BOOL)selectMidiDestination:(NSInteger)i;
@property(nonatomic) int midiOffsetMs; // -50...+50

/// Latest audio output status line ("Opened: X", "Fell back to X", "Failed", ...). Thread-safe.
@property(nonatomic, readonly, copy) NSString* audioStatus;
/// Called (any thread) whenever audioStatus changes.
@property(nonatomic, copy, nullable) void (^onAudioStatus)(NSString* status);
@property(nonatomic, readonly) uint64_t underruns;
@property(nonatomic, readonly) uint64_t blocksRendered;
@property(nonatomic, readonly) double outputLatencyMs;

- (void)drawInEncoder:(id<MTLRenderCommandEncoder>)e size:(CGSize)s;
/// Renders the latest frame offscreen to a PNG (800x480 x scale). False if there is no frame.
- (BOOL)saveScreenshot:(NSString*)path;

/// Test hook (--test-clock-play): turn on USB clock out and press Play.
- (void)testClockAndPlay;

@end

NS_ASSUME_NONNULL_END
