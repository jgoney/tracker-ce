// Headless boot of the whole core on the Apple HAL, plus the platform bits that need no UI.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "InputApple.h"
#include "PlatformApple.h"
#include "Tracker.h"
#include "TimerApple.h"

#include "fileManager.h"
#include "fileManagerDefs.h"
#include "mtStructs.h"
#include "Synth.h"

extern int16_t sin_q15(int16_t);

namespace
{
std::string makeTempRoot()
{
    char tmpl[] = "/tmp/tracker_apple_boot_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    REQUIRE(dir != nullptr);
    return dir;
}

// The core is a process-wide singleton, so every test shares one booted instance.
struct Booted
{
    std::string root = makeTempRoot();
    PlatformApple platform{ root };
    Booted()
    {
        Tracker* t = Tracker::getInstance();
        t->getStartDisplayFunc()(&platform);
        t->init(&platform);
    }
};

Booted& booted()
{
    static Booted b;
    return b;
}

std::vector<uint8_t> slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

void pump(int n)
{
    for (int i = 0; i < n; i++)
        Tracker::getInstance()->update();
}

bool pumpUntilIdle(int maxSteps)
{
    for (int i = 0; i < maxSteps; i++)
    {
        Tracker::getInstance()->update();
        if (newFileManager.getCurrentOperation() == fmNoOperation &&
            newFileManager.getStatus() != fmIdle)
            return true;
    }
    return false;
}
} // namespace

TEST_CASE("tracker boots headless")
{
    auto& b = booted();
    Tracker* t = Tracker::getInstance();
    REQUIRE(t->getPlatform() == &b.platform);

    pump(500);

    alignas(16) int16_t l[128], r[128];
    std::memset(l, 0x55, sizeof(l));
    std::memset(r, 0x55, sizeof(r));
    for (int i = 0; i < 100; i++)
        t->updateAudioBuffers(l, r, 128);

    // every sample was overwritten (silence is fine, the 0x5555 fill is not)
    for (int i = 0; i < 128; i++)
    {
        REQUIRE(l[i] != 0x5555);
        REQUIRE(r[i] != 0x5555);
    }
}

TEST_CASE("sin_q15 matches reference")
{
    REQUIRE(sin_q15(0) == 0);
    REQUIRE(std::abs(sin_q15(0x2000) - 32767) <= 1);
    REQUIRE(std::abs(sin_q15(0x6000) - (-32768)) <= 1);
}

namespace
{
uint8_t gGotPad = 0xFF;
void onPad(uint8_t n) { gGotPad = n; }
} // namespace

TEST_CASE("pad press reaches core")
{
    InputApple in;
    in.pads().setActionCallback(SwitchGroup::kPress, onPad);
    in.post({ InputEvent::Pad, 5, true, 0 });
    in.dispatch();
    REQUIRE(gGotPad == 5);
    REQUIRE(in.pads().isButtonPressed(5));
}

namespace
{
int gHolds = 0;
uint8_t gHoldIdx = 0;
void onHold(uint8_t n)
{
    gHolds++;
    gHoldIdx = n;
}
} // namespace

TEST_CASE("buttons hold synthesizes after the hold time")
{
    InputApple in;
    in.buttons().setHoldTime(20);
    in.buttons().setActionCallback(SwitchGroup::kHold, onHold);
    in.post({ InputEvent::Button, 43, true, 0 });
    in.dispatch();
    REQUIRE(gHolds == 0);
    usleep(40 * 1000);
    in.dispatch();
    REQUIRE(gHolds == 1);
    REQUIRE(gHoldIdx == 43);
    usleep(40 * 1000);
    in.dispatch();
    REQUIRE(gHolds == 1); // only once per press
    in.post({ InputEvent::Button, 43, false, 0 });
    in.dispatch();
    REQUIRE_FALSE(in.buttons().isButtonPressed(43));
}

TEST_CASE("timer basics")
{
    TimerApple t;
    REQUIRE(t.clockCyclesPerUs() == 600);
    uint32_t a = t.micros();
    usleep(2000);
    REQUIRE(t.micros() - a >= 1500);

    static uint32_t seen;
    seen = 12345;
    t.scheduleMidiClockUs(77, []() { seen = TimerApple::currentSubBlockOffsetUs(); });
    REQUIRE(seen == 77);
}

TEST_CASE("config manager is file backed and zero filled")
{
    std::string root = makeTempRoot();
    PlatformApple p(root);
    ConfigManager* cm = p.getConfigManager();
    uint8_t buf[16];
    std::memset(buf, 0xAA, sizeof(buf));
    REQUIRE(cm->readData(100, buf, 16));
    for (uint8_t v : buf)
        REQUIRE(v == 0);
    uint8_t w[4] = { 1, 2, 3, 4 };
    cm->writeData(8, w, 4);
    uint8_t rd[4] = {};
    REQUIRE(cm->readData(8, rd, 4));
    REQUIRE(std::memcmp(rd, w, 4) == 0);
    REQUIRE(slurp(root + "/.config.bin").size() >= 12);
    REQUIRE(p.getCrcCalculator()->calculate32bitCrc(w, 4) == 0);
    REQUIRE(p.getBattery()->getLevel() == 100);
    REQUIRE_FALSE(p.getBattery()->getState());
}

TEST_CASE("test_synth_project_roundtrip_identical")
{
    auto& b = booted();
    const std::string root = b.root;

    // let whatever the UI started at boot (workspace project load) finish
    for (int i = 0; i < 5000 && newFileManager.getCurrentOperation() != fmNoOperation; i++)
        Tracker::getInstance()->update();
    newFileManager.clearStatus();

    // A synth patch the way hardware leaves it in the workspace: opaque bytes under
    // Workspace/<part>.patch. The stub keeps them verbatim; the point is that the core's
    // save/open/save cycle neither drops nor alters them.
    REQUIRE(newFileManager.createNewProjectInWorkspace());
    REQUIRE(newFileManager.openProjectFromWorkspace());
    pumpUntilIdle(2000);
    newFileManager.clearStatus();

    std::vector<std::vector<uint8_t>> blobs;
    for (int part = 0; part < kSynthParts; part++)
    {
        std::vector<uint8_t> blob(96 + part);
        for (size_t i = 0; i < blob.size(); i++)
            blob[i] = (uint8_t)(i * 7 + part * 31 + 1);
        blobs.push_back(blob);
        std::string p = root + "/Workspace/" + std::to_string(part) + ".patch";
        {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write((const char*)blob.data(), (std::streamsize)blob.size());
        }
        REQUIRE(newFileManager.getSynth()->loadPatchFromWorkspace(part));
        newFileManager.getSynth()->setPatchDirty(part);
    }

    char path[] = "Projects";
    char name1[] = "rt1";
    REQUIRE(newFileManager.saveProject(path, name1));
    REQUIRE(pumpUntilIdle(5000));
    REQUIRE(newFileManager.getStatus() == fmSaveEnd);
    newFileManager.clearStatus();

    REQUIRE(newFileManager.openProject(path, name1));
    REQUIRE(pumpUntilIdle(5000));
    newFileManager.clearStatus();

    const auto proj1 = slurp(root + "/Projects/rt1/project.mt");
    REQUIRE(proj1.size() == sizeof(strProjectFile));

    // Re-save the re-opened project under the same name (incremental save).
    REQUIRE(newFileManager.saveProject(path, name1));
    REQUIRE(pumpUntilIdle(5000));
    REQUIRE(newFileManager.getStatus() == fmSaveEnd);
    newFileManager.clearStatus();

    const auto proj2 = slurp(root + "/Projects/rt1/project.mt");
    REQUIRE(proj2.size() == proj1.size());

    // Everything up to projectName (header, song, values) must be byte-identical. projectName
    // and projectPath are strcpy'd into an uninitialized stack buffer by the core (hardware
    // does the same), so only their string content is deterministic; the trailing crc is 0.
    strProjectFile probe;
    const size_t nameOff = (const char*)probe.projectDataAndHeader.project.projectName -
                           (const char*)&probe;
    const size_t pathOff = (const char*)probe.projectDataAndHeader.project.projectPath -
                           (const char*)&probe;
    REQUIRE(nameOff > 1000);
    REQUIRE(std::memcmp(proj1.data(), proj2.data(), nameOff) == 0);
    REQUIRE(std::strcmp((const char*)&proj1[nameOff], (const char*)&proj2[nameOff]) == 0);
    REQUIRE(std::strcmp((const char*)&proj1[pathOff], (const char*)&proj2[pathOff]) == 0);
    REQUIRE(std::strcmp((const char*)&proj1[nameOff], "rt1") == 0);

    for (int part = 0; part < kSynthParts; part++)
    {
        std::string f = "/" + std::to_string(part) + ".patch";
        auto w = slurp(root + "/Workspace" + f);
        auto p = slurp(root + "/Projects/rt1" + f);
        REQUIRE(w == blobs[part]);
        REQUIRE(p == w);
    }
}

extern "C" uint8_t _vStackBase[];
extern "C" void _vStackTop(void);

TEST_CASE("stack watermark symbols bound a real region")
{
    const uint8_t* top = (const uint8_t*)&_vStackTop;
    REQUIRE(top - _vStackBase == 64 * 1024);
    // Profiler walks down from top-1; it must be readable
    volatile uint8_t probe = top[-1];
    (void)probe;
}
