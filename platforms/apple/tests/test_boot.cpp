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

TEST_CASE("held key auto-repeats on the IMX schedule")
{
    // platforms/imx/source/Drivers/Keys.cpp:10,202-214: fire when elapsed > holdTime - diff[f],
    // diff = {0,100,190,235,255,270}, f saturates at 5, interval restarts at each fire.
    InputApple in;
    in.buttons().setHoldTime(333);
    in.buttons().setActionCallback(SwitchGroup::kHold, onHold);
    gHolds = 0;
    in.post({ InputEvent::Button, 43, true, 0 });
    in.dispatchAt(1000); // press at t=1000
    REQUIRE(gHolds == 0);

    // expected fire times: intervals (strictly greater than) 333, 233, 143, 98, 78, 63, 63...
    const int intervals[] = { 333, 233, 143, 98, 78, 63, 63, 63 };
    uint32_t t = 1000;
    for (int iv : intervals)
    {
        in.dispatchAt(t + iv); // exactly the interval: not yet (needs >)
        int before = gHolds;
        in.dispatchAt(t + iv + 1);
        REQUIRE(gHolds == before + 1);
        t += iv + 1;
        in.dispatchAt(t); // same instant: no double fire
        REQUIRE(gHolds == before + 1);
    }
    REQUIRE(gHoldIdx == 43);

    in.post({ InputEvent::Button, 43, false, 0 });
    in.dispatchAt(t + 1);
    REQUIRE_FALSE(in.buttons().isButtonPressed(43));
    int total = gHolds;
    in.dispatchAt(t + 1000);
    REQUIRE(gHolds == total); // released: no more repeats
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

    // Assign each synth instrument (core instrument numbering: SYNTH_INSTRUMENTS_START_IDX + part)
    // to a note on its own track, and give each part distinct settings, via the core's own
    // structures, then flag the pattern and project as changed so the save writes them.
    auto* pattern = sequencer.getActualPattern();
    for (int part = 0; part < kSynthParts; part++)
    {
        auto& step = pattern->track[part].step[0];
        step.note = 60 + part;
        step.instrument = SYNTH_INSTRUMENTS_START_IDX + part;
        mtProject.values.synthPart[part].gainDb = -3 - part;
        mtProject.values.synthPart[part].voiceCount = 1 + part % 2;
    }
    newFileManager.setActualPatternStructChanged();
    newFileManager.setProjectStructChanged();

    char path[] = "Projects";
    char name1[] = "rt1";
    REQUIRE(newFileManager.saveProject(path, name1));
    REQUIRE(pumpUntilIdle(5000));
    REQUIRE(newFileManager.getStatus() == fmSaveEnd);
    newFileManager.clearStatus();

    REQUIRE(newFileManager.openProject(path, name1));
    REQUIRE(pumpUntilIdle(5000));
    newFileManager.clearStatus();

    // the reloaded project still has the synth instruments assigned and configured
    {
        auto* reloaded = sequencer.getActualPattern();
        for (int part = 0; part < kSynthParts; part++)
        {
            REQUIRE(reloaded->track[part].step[0].instrument == SYNTH_INSTRUMENTS_START_IDX + part);
            REQUIRE(reloaded->track[part].step[0].note == 60 + part);
            REQUIRE(mtProject.values.synthPart[part].gainDb == -3 - part);
        }
    }

    const auto proj1 = slurp(root + "/Projects/rt1/project.mt");
    const auto patt1 = slurp(root + "/Projects/rt1/patterns/pattern_01.mtp");
    REQUIRE(proj1.size() == sizeof(strProjectFile));
    REQUIRE(patt1.size() > 0);

    // Re-save the re-opened project under the same name (incremental save).
    REQUIRE(newFileManager.saveProject(path, name1));
    REQUIRE(pumpUntilIdle(5000));
    REQUIRE(newFileManager.getStatus() == fmSaveEnd);
    newFileManager.clearStatus();

    const auto proj2 = slurp(root + "/Projects/rt1/project.mt");
    REQUIRE(proj2.size() == proj1.size());

    // The whole file, crc included, is byte-identical (the buffer is zero-initialised, so the
    // tails of projectName/projectPath are deterministic).
    REQUIRE(proj1 == proj2);
    REQUIRE(slurp(root + "/Projects/rt1/patterns/pattern_01.mtp") == patt1);

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
