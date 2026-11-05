// Tests for the silent PolySynth / ptfx stand-ins (platforms/apple/synth_stub).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "PolySynth/FileSystem.h"
#include "PolySynth/Instrument.h"
#include "PolySynth/Mixer.h"
#include "PolySynth/Patch.h"
#include "PolySynth/PatchStore.h"
#include "PolySynth/Synth.h"
#include "PolySynth/sdk/InstrumentTypeRegistry.h"
#include "ptfx_loader.h"

using namespace polysynth;

namespace
{
// In-memory polysynth::FileSystem, keyed by path.
class MemFileSystem : public FileSystem
{
  public:
    struct Handle
    {
        std::string path;
        size_t pos = 0;
        bool write = false;
    };
    std::map<std::string, std::vector<uint8_t>> files;
    std::vector<std::string> listing;

    std::vector<std::string_view> listFiles(const std::string& path,
                                            std::function<bool(const std::string&)> matcher,
                                            tiny_optional<int>) override
    {
        listing.clear();
        const std::string prefix = path.empty() || path.back() == '/' ? path : path + "/";
        for (const auto& [name, _] : files)
        {
            if (name.rfind(prefix, 0) != 0)
                continue;
            std::string rest = name.substr(prefix.size());
            if (rest.find('/') == std::string::npos && matcher(rest))
                listing.push_back(rest);
        }
        return { listing.begin(), listing.end() };
    }
    std::optional<FileHandle> open(const std::string& path, OpenOptions options) override
    {
        bool w = (static_cast<int>(options) & static_cast<int>(OpenOptions::kWrite)) != 0;
        if (!w && !files.count(path))
            return std::nullopt;
        if (w)
            files[path].clear();
        return new Handle{ path, 0, w };
    }
    void close(FileHandle f) override { delete static_cast<Handle*>(f); }
    bool write(FileHandle f, std::span<const uint8_t> d) override
    {
        auto* h = static_cast<Handle*>(f);
        files[h->path].insert(files[h->path].end(), d.begin(), d.end());
        return true;
    }
    int32_t read(FileHandle f, std::span<uint8_t> buf) override
    {
        auto* h = static_cast<Handle*>(f);
        auto& data = files[h->path];
        size_t n = std::min(buf.size(), data.size() - h->pos);
        std::memcpy(buf.data(), data.data() + h->pos, n);
        h->pos += n;
        return static_cast<int32_t>(n);
    }
    bool seek(FileHandle f, int off) override
    {
        static_cast<Handle*>(f)->pos += off;
        return true;
    }
    void createFolder(const std::string&) override {}
};

struct Fixture
{
    std::shared_ptr<Synth> synth = Synth::createSynth([](size_t) { return nullptr; });
    MemFileSystem* patchesFs = nullptr;
    MemFileSystem* workspaceFs = nullptr;
    Fixture()
    {
        auto p = std::make_unique<MemFileSystem>();
        auto w = std::make_unique<MemFileSystem>();
        patchesFs = p.get();
        workspaceFs = w.get();
        synth->addFileSystem(Synth::FileSystemId::kPatches, std::move(p));
        synth->addFileSystem(Synth::FileSystemId::kWorkspace, std::move(w));
    }
};
} // namespace

TEST_CASE("stub synth renders silence")
{
    Fixture f;
    auto store = f.synth->createPatchStore(Synth::FileSystemId::kPatches, true);
    std::array<std::shared_ptr<Instrument>, 3> instruments;
    for (int i = 0; i < 3; i++)
    {
        auto patch = store->createPatch(static_cast<int>(InstrumentTypeId::kSimpleAnalogSynth));
        REQUIRE(patch);
        instruments[i] = f.synth->createInstrument(i);
        instruments[i]->assignPatch(std::move(patch));
        instruments[i]->setVoiceCount(1);
    }
    f.synth->setup(instruments, {}, nullptr, [] { return tiny::optional<float>(120.f); });

    std::array<sample_t, kAudioBufferSize> l, r;
    l.fill(0.5f);
    r.fill(-0.5f);
    f.synth->updateAudio();
    f.synth->renderAudioBuffer(AudioBufferStereoRef{ AudioBufferRef(l), AudioBufferRef(r) });
    REQUIRE(std::all_of(l.begin(), l.end(), [](float s) { return s == 0.0f; }));
    REQUIRE(std::all_of(r.begin(), r.end(), [](float s) { return s == 0.0f; }));

    // single-instrument render with aux sends
    l.fill(0.5f);
    r.fill(0.5f);
    std::array<sample_t, kAudioBufferSize> a0, a1;
    a0.fill(0.5f);
    a1.fill(0.5f);
    std::array<AudioBufferRef, 2> aux{ AudioBufferRef(a0), AudioBufferRef(a1) };
    f.synth->renderAudioBuffer(AudioBufferStereoRef{ AudioBufferRef(l), AudioBufferRef(r) }, aux, 0);
    for (auto* b : { &l, &r, &a0, &a1 })
        REQUIRE(std::all_of(b->begin(), b->end(), [](float s) { return s == 0.0f; }));
}

TEST_CASE("stub patch bytes round-trip unchanged")
{
    Fixture f;
    auto store = f.synth->createPatchStore(Synth::FileSystemId::kWorkspace, false);
    auto patch = store->createPatch(static_cast<int>(InstrumentTypeId::kFMSynth));

    std::vector<uint8_t> blob(70001);
    uint32_t x = 12345;
    for (auto& b : blob)
    {
        x = x * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(x >> 24); // includes zeros and 0xFF
    }
    f.workspaceFs->files["0.patch"] = blob;

    REQUIRE(store->loadPatch("0", *patch, true));
    REQUIRE(store->savePatch("1", *patch));
    REQUIRE(f.workspaceFs->files["1.patch"] == blob);

    REQUIRE_FALSE(store->loadPatch("missing", *patch, true));
}

TEST_CASE("registry registers every engine")
{
    InstrumentTypeRegistry reg;
    InstrumentTypeRegistry::registerInstrumentTypes(reg, [](size_t) { return nullptr; });
    for (auto id : { InstrumentTypeId::kMidiOut, InstrumentTypeId::kSimpleAnalogSynth,
                     InstrumentTypeId::kFatAnalogSynth, InstrumentTypeId::kVASynth,
                     InstrumentTypeId::kFMSynth, InstrumentTypeId::kDrumSynth,
                     InstrumentTypeId::kShapingSynth, InstrumentTypeId::kChiptuneSynth })
        REQUIRE(reg.getInstrumentType(static_cast<int>(id)));
}

TEST_CASE("ptfx plugin load fails on this platform")
{
    PtfxHeader h{};
    PtfxInitFunc i = nullptr;
    PtfxSetParamFunc s = nullptr;
    PtfxProcessBlockFunc p = nullptr;
    REQUIRE_FALSE(PtfxLoader::readHeader("/Effects/x.ptfx", h));
    REQUIRE_FALSE(PtfxLoader::loadPlugin("/Effects/x.ptfx", h, i, s, p));
}
