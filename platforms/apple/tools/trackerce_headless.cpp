// trackerce-headless: boots the Tracker core on the Apple HAL without a window.
//
//   trackerce-headless --screenshot out.ppm [--updates N] [--interval-ms M] [--sd DIR]
//                      [--audio-seconds S]
//
// Runs N core update() calls (default 500), M milliseconds apart in wall-clock time (default 10:
// the UI is time driven, e.g. the start screen shows for at least ~2 s), then rasterises the
// last frame the FT812 model published with CpuRaster and writes it as a binary PPM.
// With --audio-seconds, AudioRuntime drives the core instead (tracker + render threads) for S
// seconds, pulled by a simulated real-time IOProc (no device), with Play pressed after 3 s; it
// prints blocks rendered and underruns. TRACKERCE_ALLOC_GUARD=1 aborts on any allocation in
// the audio path.
// --device ID [--channels L,R] sends --audio-seconds to a Core Audio device (see --list-devices)
// instead of the simulated IOProc. Prints the device latency, IOProc cycles and underruns.
//
//   trackerce-headless --list-devices
//   trackerce-headless --tone S [--device ID] [--channels L,R]
//
// --tone plays a 440 Hz sine (-12 dBFS) through AudioRuntime and Core Audio for S seconds on
// the given device (default output when omitted) and prints the IOProc cycle and underrun counts.
// The SD card root defaults to a fresh temporary folder.
#include "AudioRuntime.h"
#include "CoreAudioOutput.h"
#include "CpuRaster.h"
#include "PlatformApple.h"
#include "Tracker.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>

namespace
{
int usage()
{
    std::fprintf(stderr,
                 "usage: trackerce-headless --screenshot out.ppm [--updates N] [--interval-ms M] "
                 "[--sd DIR] [--audio-seconds S] [--device ID] [--channels L,R]\n"
                 "       trackerce-headless --list-devices\n"
                 "       trackerce-headless --tone S [--device ID] [--channels L,R]\n");
    return 2;
}

void startDisplayNoop(Platform*)
{
}

// A core stand-in that renders a 440 Hz sine, so --tone exercises ring + Core Audio only.
class ToneTracker final : public Tracker
{
  public:
    void init(Platform*) override {}
    void update() override { usleep(1000); }
    void updateAudioBuffers(void* l, void* r, int length) override
    {
        auto* pl = (int16_t*)l;
        auto* pr = (int16_t*)r;
        for (int i = 0; i < length; i++)
        {
            const int16_t v = (int16_t)(8192.0 * std::sin(phase_));
            phase_ += 2.0 * M_PI * 440.0 / AudioRuntime::kSampleRate;
            if (phase_ > 2.0 * M_PI)
                phase_ -= 2.0 * M_PI;
            pl[i] = pr[i] = v;
        }
    }
    void updateInputAudioBuffers(void*, void*, int) override {}
    void (*getStartDisplayFunc())(Platform*) override { return startDisplayNoop; }
    Platform* getPlatform() override { return nullptr; }

  private:
    double phase_ = 0;
};

// Opens `deviceId` (0: the default output) on the requested channels and reports it.
bool openOutput(CoreAudioOutput& out, uint32_t deviceId, int l, int r)
{
    if (deviceId == 0)
        deviceId = CoreAudioOutput::defaultOutputDevice();
    if (!out.open(deviceId, (uint8_t)l, (uint8_t)r)) // bounded: a hung driver times out after 2 s
    {
        std::fprintf(stderr, "cannot open device %u (failed or unresponsive)\n", deviceId);
        return false;
    }
    std::printf("device %u: %.0f Hz%s, %u-frame buffer, latency %.2f ms\n",
                deviceId,
                out.deviceSampleRate(),
                out.usingConverter() ? " (converted)" : "",
                out.bufferFrames(),
                out.outputLatencySeconds() * 1000.0);
    return true;
}

// Stands in for Core Audio's IO thread: time-constraint policy, one 128-frame pull per period.
void simulatedIoProc(AudioRuntime& rt, double seconds, float& peak)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double ticksPerSec = 1e9 * tb.denom / tb.numer;
    const double period = AudioRuntime::kBlockFrames / AudioRuntime::kSampleRate;
    thread_time_constraint_policy_data_t pol{ (uint32_t)(period * ticksPerSec),
                                              (uint32_t)(period * ticksPerSec / 4),
                                              (uint32_t)(period * ticksPerSec),
                                              1 };
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&pol,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    float l[AudioRuntime::kBlockFrames], r[AudioRuntime::kBlockFrames];
    const uint64_t t0 = mach_absolute_time();
    for (uint64_t cycle = 1; cycle * period < seconds; cycle++)
    {
        mach_wait_until(t0 + (uint64_t)(cycle * period * ticksPerSec));
        const size_t n = rt.pull(l, r, AudioRuntime::kBlockFrames);
        for (size_t i = 0; i < n; i++)
            peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
    }
}
} // namespace

int main(int argc, char** argv)
{
    const char* screenshot = nullptr;
    long updates = 500;
    long intervalMs = 10;
    std::string sdRoot;
    double audioSeconds = 0;
    double toneSeconds = 0;
    bool listDevices = false;
    uint32_t deviceId = 0;
    bool haveDevice = false;
    int leftCh = 0, rightCh = 1;

    for (int i = 1; i < argc; i++)
    {
        const bool hasValue = i + 1 < argc;
        if (!std::strcmp(argv[i], "--screenshot") && hasValue)
            screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--updates") && hasValue)
            updates = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--interval-ms") && hasValue)
            intervalMs = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--sd") && hasValue)
            sdRoot = argv[++i];
        else if (!std::strcmp(argv[i], "--audio-seconds") && hasValue)
            audioSeconds = std::strtod(argv[++i], nullptr);
        else if (!std::strcmp(argv[i], "--tone") && hasValue)
            toneSeconds = std::strtod(argv[++i], nullptr);
        else if (!std::strcmp(argv[i], "--list-devices"))
            listDevices = true;
        else if (!std::strcmp(argv[i], "--device") && hasValue)
        {
            deviceId = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
            haveDevice = true;
        }
        else if (!std::strcmp(argv[i], "--channels") && hasValue)
        {
            if (std::sscanf(argv[++i], "%d,%d", &leftCh, &rightCh) != 2 || leftCh < 0 || rightCh < 0)
                return usage();
        }
        else
            return usage();
    }

    if (listDevices)
    {
        const uint32_t def = CoreAudioOutput::defaultOutputDevice();
        for (auto& d : CoreAudioOutput::listOutputDevices())
            std::printf("%u\t%u ch\t%s%s\n", d.id, d.outChannels, d.name.c_str(), d.id == def ? "\t(default)" : "");
        return 0;
    }
    if (toneSeconds > 0)
    {
        ToneTracker tone;
        AudioRuntime rt(&tone, nullptr);
        CoreAudioOutput out(rt);
        if (!openOutput(out, deviceId, leftCh, rightCh))
            return 1;
        rt.start();
        out.start();
        std::this_thread::sleep_for(std::chrono::duration<double>(toneSeconds));
        out.stop();
        rt.stop();
        std::printf("tone: %.1f s, %llu IOProc cycles, %llu blocks rendered, %llu underruns\n",
                    toneSeconds,
                    (unsigned long long)out.cycles(),
                    (unsigned long long)rt.blocksRendered.load(),
                    (unsigned long long)rt.underruns.load());
        return out.cycles() > 0 ? 0 : 1;
    }
    if (!screenshot || updates < 0 || intervalMs < 0)
        return usage();

    if (sdRoot.empty())
    {
        const char* tmp = std::getenv("TMPDIR");
        std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp/") + "/trackerce_sd_XXXXXX";
        if (!mkdtemp(tmpl.data()))
        {
            std::perror("mkdtemp");
            return 1;
        }
        sdRoot = tmpl;
    }

    static PlatformApple platform(sdRoot);
    Tracker* tracker = Tracker::getInstance();
    if (audioSeconds > 0)
    {
        AudioRuntime rt(tracker, &platform);
        rt.start(); // boots the core on the tracker thread
        float peak = 0;
        CoreAudioOutput out(rt);
        std::thread io;
        if (haveDevice)
        {
            if (!openOutput(out, deviceId, leftCh, rightCh))
                return 1;
            out.start();
            io = std::thread([&] { std::this_thread::sleep_for(std::chrono::duration<double>(audioSeconds)); });
        }
        else
            io = std::thread([&] { simulatedIoProc(rt, audioSeconds, peak); });
        if (audioSeconds > 3.5)
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            platform.input().post({ InputEvent::Button, 23, true, 0 }); // interfaceButtonPlay
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            platform.input().post({ InputEvent::Button, 23, false, 0 });
        }
        io.join();
        out.stop();
        rt.stop();
        if (haveDevice)
            std::printf("device: %llu IOProc cycles\n", (unsigned long long)out.cycles());
        std::printf("audio: %.1f s, %llu blocks rendered, %llu underruns, peak %.4f\n",
                    audioSeconds,
                    (unsigned long long)rt.blocksRendered.load(),
                    (unsigned long long)rt.underruns.load(),
                    peak);
    }
    else
    {
        tracker->getStartDisplayFunc()(&platform);
        tracker->init(&platform);

        for (long i = 0; i < updates; i++)
        {
            tracker->update();
            if (intervalMs > 0)
                usleep(useconds_t(intervalMs * 1000));
        }
    }

    auto frame = platform.display().latestFrame();
    if (!frame)
    {
        std::fprintf(stderr, "no frame was published\n");
        return 1;
    }
    if (!CpuRaster::toPPM(*frame, platform.display(), screenshot))
    {
        std::fprintf(stderr, "cannot write %s\n", screenshot);
        return 1;
    }
    std::printf("frame %llu: %zu prims -> %s\n",
                (unsigned long long)frame->serial,
                frame->prims.size(),
                screenshot);
    return 0;
}
