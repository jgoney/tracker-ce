// trackerce-headless: boots the Tracker core on the Apple HAL without a window.
//
//   trackerce-headless --screenshot out.ppm [--updates N] [--interval-ms M] [--sd DIR]
//
// Runs N core update() calls (default 500), M milliseconds apart in wall-clock time (default 10:
// the UI is time driven, e.g. the start screen shows for at least ~2 s), then rasterises the
// last frame the FT812 model published with CpuRaster and writes it as a binary PPM.
// The SD card root defaults to a fresh temporary folder.
#include "CpuRaster.h"
#include "PlatformApple.h"
#include "Tracker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace
{
int usage()
{
    std::fprintf(stderr,
                 "usage: trackerce-headless --screenshot out.ppm [--updates N] [--interval-ms M] "
                 "[--sd DIR]\n");
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    const char* screenshot = nullptr;
    long updates = 500;
    long intervalMs = 10;
    std::string sdRoot;

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
        else
            return usage();
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
    tracker->getStartDisplayFunc()(&platform);
    tracker->init(&platform);

    for (long i = 0; i < updates; i++)
    {
        tracker->update();
        if (intervalMs > 0)
            usleep(useconds_t(intervalMs * 1000));
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
