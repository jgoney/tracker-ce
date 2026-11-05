// Silent stand-in for the proprietary libPolySynth.a / libPolyDSP.a.
//
// Written only from the MIT-licensed headers in synth/include/PolySynth/ (and sdk/).
// The real libraries are never linked, inspected or disassembled.
//
//  * Rendering produces silence (zero-filled buffers).
//  * Patch persistence is byte preserving: loadPatch() keeps the raw file bytes inside the
//    Patch object and savePatch() writes them back unchanged, so patches copied from/to a
//    hardware SD card survive a round trip through the Mac build untouched. The stub does not
//    understand the blob; parameter edits made on the Mac live only in memory and are NOT
//    serialized. A patch created on the Mac with no source file serializes as a zero-length
//    file ("created on Mac, not loadable on hardware").
//  * On-disk patch layout is a guess (the header does not specify it):
//      folder-structured store: <typeId>/<name>.patch    flat store: <name>.patch
//    where <typeId> is the decimal InstrumentTypeId.

#include "PolySynth/FileSystem.h"
#include "PolySynth/Grid.h"
#include "PolySynth/GridMapper.h"
#include "PolySynth/GridMapperStore.h"
#include "PolySynth/Instrument.h"
#include "PolySynth/Mixer.h"
#include "PolySynth/Patch.h"
#include "PolySynth/PatchPool.h"
#include "PolySynth/PatchStore.h"
#include "PolySynth/SampleStore.h"
#include "PolySynth/Sequencer.h"
#include "PolySynth/Synth.h"
#include "PolySynth/sdk/InstrumentType.h"
#include "PolySynth/sdk/InstrumentVoice.h"
#include "PolySynth/sdk/InstrumentTypeRegistry.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace polysynth
{

// ---------------------------------------------------------------------------------------------
// Engine factories (referenced by core/synth/registry.cpp). Metadata is minimal: no params.
// ---------------------------------------------------------------------------------------------
namespace
{
std::unique_ptr<InstrumentType> makeStubType(const char* name)
{
    return std::make_unique<InstrumentType>(InstrumentType{
      .name = name,
      .createMetadata = [](Patch&, Patch::Metadata& metadata)
      { metadata.outputChannels = Patch::Metadata::OutputChannels::kStereo; },
      .createVoice = [](int) -> std::unique_ptr<InstrumentVoice> { return nullptr; } });
}
} // namespace

std::unique_ptr<InstrumentType> createMidiOut()
{
    auto t = makeStubType("MIDI");
    t->createMetadata = [](Patch&, Patch::Metadata& metadata)
    { metadata.outputChannels = Patch::Metadata::OutputChannels::kNone; };
    return t;
}
std::unique_ptr<InstrumentType> createSimpleAnalogSynth() { return makeStubType("SIMPLE"); }
std::unique_ptr<InstrumentType> createFatAnalogSynth() { return makeStubType("FAT"); }
std::unique_ptr<InstrumentType> createVASynth() { return makeStubType("VA"); }
std::unique_ptr<InstrumentType> createFMSynth() { return makeStubType("FM"); }
std::unique_ptr<InstrumentType> createDrumSynth() { return makeStubType("DRUM"); }
std::unique_ptr<InstrumentType> createShapingSynth() { return makeStubType("SHAPING"); }

// ---------------------------------------------------------------------------------------------
// InstrumentTypeRegistry (members the core does not define itself)
// ---------------------------------------------------------------------------------------------
std::shared_ptr<const InstrumentType> InstrumentTypeRegistry::getInstrumentType(int typeId) const
{
    auto it = instrumentTypes.find(typeId);
    return it == instrumentTypes.end() ? nullptr : it->second;
}

void InstrumentTypeRegistry::addInstrumentType(int typeId,
                                               std::unique_ptr<InstrumentType> instrumentType)
{
    instrumentTypes[typeId] = std::shared_ptr<InstrumentType>(std::move(instrumentType));
}

namespace
{

// ---------------------------------------------------------------------------------------------
// Patch
// ---------------------------------------------------------------------------------------------
class StubPatch final : public Patch
{
  public:
    StubPatch(std::shared_ptr<const InstrumentType> type, int typeId)
      : type(std::move(type))
    {
        if (this->type && this->type->createMetadata)
        {
            this->type->createMetadata(*this, metadata);
        }
        if (metadata.instrumentTypeId < 0)
        {
            metadata.instrumentTypeId = typeId;
        }
        if (metadata.initDefaultMacros)
        {
            metadata.initDefaultMacros(metadata.macros);
        }
        for (size_t i = 0; i < metadata.macros.size(); i++)
        {
            auto& m = metadata.macros[i];
            if (i >= kUserMacroCount)
            {
                m.system = true;
                m.perVoice = true;
            }
            if (m.displayName.empty())
            {
                m.displayName = i < kUserMacroCount ? "Macro " + std::to_string(i + 1)
                                : i == static_cast<size_t>(SystemMacroId::kVelocity) ? "Velocity"
                                                                                     : "Pressure";
            }
        }
        paramValues.resize(metadata.params.size());
        for (size_t i = 0; i < paramValues.size(); i++)
        {
            paramValues[i] = metadata.params[i].defaultValue;
        }
        macroValues.assign(metadata.macros.size(), optional_paramvalue_t(std::nullopt));
    }

    // Raw blob handed over by PatchStore::loadPatch, returned verbatim by savePatch.
    std::vector<uint8_t> rawBytes;

    optional_paramvalue_t getParamValue(int paramId) const override
    {
        if (paramId < 0 || paramId >= static_cast<int>(paramValues.size()))
        {
            return std::nullopt;
        }
        return paramValues[paramId];
    }

    tiny_optional<float> getParamValueEditMapped(int paramId) const override
    {
        auto v = getParamValue(paramId);
        if (!v.has_value())
        {
            return std::nullopt;
        }
        const auto& p = metadata.params[paramId];
        float lo = static_cast<float>(p.minValue), hi = static_cast<float>(p.maxValue);
        if (!(hi > lo))
        {
            return 0.0f;
        }
        return std::clamp((*v - lo) / (hi - lo), 0.0f, 1.0f); // linear for every curve
    }

    void setParamValueEditMapped(int paramId,
                                 tiny_optional<float> value,
                                 ParamChangeSource source) override
    {
        if (paramId < 0 || paramId >= static_cast<int>(paramValues.size()))
        {
            return;
        }
        if (!value.has_value())
        {
            setParamValue(paramId, std::nullopt, source);
            return;
        }
        const auto& p = metadata.params[paramId];
        float lo = static_cast<float>(p.minValue), hi = static_cast<float>(p.maxValue);
        setParamValue(paramId, lo + std::clamp(*value, 0.0f, 1.0f) * (hi - lo), source);
    }

    optional_paramvalue_t getParamFinalValue(int paramId) const override
    {
        return getParamValue(paramId);
    }

    void setParamValue(int paramId, optional_paramvalue_t value, ParamChangeSource source) override
    {
        if (paramId < 0 || paramId >= static_cast<int>(paramValues.size()))
        {
            return;
        }
        paramValues[paramId] = value;
        if (batching)
        {
            return;
        }
        notify(paramId, value, source);
    }

    void subscribeParamUpdates(std::weak_ptr<ParamChangeListener> listener) override
    {
        listeners.push_back(std::move(listener));
    }

    void unsubscribeParamUpdates(const ParamChangeListener& listener) override
    {
        std::erase_if(listeners,
                      [&](const std::weak_ptr<ParamChangeListener>& w)
                      {
                          auto s = w.lock();
                          return !s || s.get() == &listener;
                      });
    }

    void startBatchUpdate() override { batching = true; }

    void finishBatchUpdate() override
    {
        batching = false;
        for (int i = 0; i < static_cast<int>(paramValues.size()); i++)
        {
            notify(i, paramValues[i], ParamChangeSource::kPatchAssign);
        }
    }

    optional_paramvalue_t getMacroValue(int macroId) const override
    {
        if (macroId < 0 || macroId >= static_cast<int>(macroValues.size()))
        {
            return std::nullopt;
        }
        return macroValues[macroId];
    }

    void setMacroValue(int macroId, optional_paramvalue_t value, tiny_optional<int>) override
    {
        if (macroId >= 0 && macroId < static_cast<int>(macroValues.size()))
        {
            macroValues[macroId] = value;
        }
    }

    void setMacroModifier(int macroId, int modifierIdx, tiny_optional<int> paramId) override
    {
        auto* m = macro(macroId);
        if (!m || modifierIdx < 0 || modifierIdx > static_cast<int>(m->modifiers.size()))
        {
            return;
        }
        if (!paramId.has_value())
        {
            if (modifierIdx < static_cast<int>(m->modifiers.size()))
            {
                m->modifiers.erase(m->modifiers.begin() + modifierIdx);
            }
        }
        else if (modifierIdx == static_cast<int>(m->modifiers.size()))
        {
            m->modifiers.push_back({ paramId, 0.0f });
        }
        else
        {
            m->modifiers[modifierIdx].paramId = paramId;
        }
    }

    void setMacroModifierAmount(int macroId, int modifierIdx, float amount) override
    {
        auto* m = macro(macroId);
        if (m && modifierIdx >= 0 && modifierIdx < static_cast<int>(m->modifiers.size()))
        {
            m->modifiers[modifierIdx].amount = amount;
        }
    }

    tiny_optional<float> getMacroModifierAmount(int macroId, int modifierIdx) override
    {
        auto* m = macro(macroId);
        if (!m || modifierIdx < 0 || modifierIdx >= static_cast<int>(m->modifiers.size()))
        {
            return std::nullopt;
        }
        return m->modifiers[modifierIdx].amount;
    }

    void clearMacroModifiers() override
    {
        for (auto& m : metadata.macros)
        {
            m.modifiers.clear();
        }
    }

    void changeMacroModifierAmount(int macroId, int modifierIdx, paramvalue_t value) override
    {
        auto* m = macro(macroId);
        if (!m || modifierIdx < 0 || modifierIdx >= static_cast<int>(m->modifiers.size()))
        {
            return;
        }
        auto& mod = m->modifiers[modifierIdx];
        float range = 1.0f;
        if (mod.paramId.has_value() && *mod.paramId >= 0 &&
            *mod.paramId < static_cast<int>(metadata.params.size()))
        {
            const auto& p = metadata.params[*mod.paramId];
            range = static_cast<float>(p.maxValue) - static_cast<float>(p.minValue);
            if (!(range > 0.0f))
            {
                range = 1.0f;
            }
        }
        mod.amount = std::clamp(mod.amount + value / range,
                                -Metadata::Macro::ParamModifier::kMaxAmount,
                                Metadata::Macro::ParamModifier::kMaxAmount);
    }

    void setMacroBipolar(int macroId, bool bipolar) override
    {
        if (auto* m = macro(macroId))
        {
            m->bipolar = bipolar;
        }
    }

    void setMacroDisplayName(int macroId, std::string name) override
    {
        if (auto* m = macro(macroId))
        {
            m->displayName = std::move(name);
        }
    }

    const Sample* getSample(int) const override { return nullptr; }
    LoadStatus loadSample(int, const std::string&) override { return LoadStatus::kFileError; }

    const Metadata& getMetadata() const override { return metadata; }
    const Sequencer& getSequencer() const override { return sequencer; }

    void setSequence(const Sequencer::Sequence& steps, int idx) override
    {
        if (idx >= 0 && idx < static_cast<int>(sequencer.sequences.size()))
        {
            sequencer.sequences[idx] = steps;
        }
    }
    void setSequencerConfig(const Sequencer::Config& config) override
    {
        sequencer.config = config;
    }

  private:
    Metadata::Macro* macro(int id)
    {
        return id >= 0 && id < static_cast<int>(metadata.macros.size()) ? &metadata.macros[id]
                                                                        : nullptr;
    }

    void notify(int paramId, optional_paramvalue_t value, ParamChangeSource source)
    {
        std::erase_if(listeners, [](const auto& w) { return w.expired(); });
        for (auto& w : listeners)
        {
            if (auto l = w.lock())
            {
                l->onParamChange(paramId, value, source, std::nullopt);
            }
        }
    }

    std::shared_ptr<const InstrumentType> type;
    Metadata metadata;
    Sequencer sequencer;
    std::vector<optional_paramvalue_t> paramValues;
    std::vector<optional_paramvalue_t> macroValues;
    std::vector<std::weak_ptr<ParamChangeListener>> listeners;
    bool batching = false;
};

// ---------------------------------------------------------------------------------------------
// PatchStore: raw-blob persistence on top of polysynth::FileSystem
// ---------------------------------------------------------------------------------------------
class StubPatchStore final : public PatchStore
{
  public:
    StubPatchStore(std::shared_ptr<FileSystem> fs,
                   std::shared_ptr<InstrumentTypeRegistry> registry,
                   bool typeFolders)
      : fs(std::move(fs))
      , registry(std::move(registry))
      , typeFolders(typeFolders)
    {
    }

    std::unique_ptr<Patch> createPatch(int instrumentTypeId) override
    {
        auto type = registry->getInstrumentType(instrumentTypeId);
        if (!type)
        {
            return nullptr;
        }
        return std::make_unique<StubPatch>(std::move(type), instrumentTypeId);
    }

    std::vector<std::pair<std::string_view, int>> listPatchesFilter(
      std::function<bool(InstrumentTypeId)> filter) override
    {
        listed.clear();
        std::vector<std::pair<size_t, int>> index; // (position in listed, type)
        for (const auto& [typeId, _] : sortedTypes())
        {
            if (filter && !filter(static_cast<InstrumentTypeId>(typeId)))
            {
                continue;
            }
            for (auto& n : scan(typeId))
            {
                index.emplace_back(listed.size(), typeId);
                listed.push_back(std::move(n));
            }
        }
        std::vector<std::pair<std::string_view, int>> out;
        for (auto [i, typeId] : index)
        {
            out.emplace_back(listed[i], typeId);
        }
        return out;
    }

    std::vector<std::string_view> listPatchesByType(int typeId) override
    {
        listed = scan(typeId);
        return { listed.begin(), listed.end() };
    }

    bool loadPatch(const std::string& name, Patch& patch, bool) override
    {
        auto& p = static_cast<StubPatch&>(patch); // StubPatch is the only Patch we create
        auto path = pathFor(name, p.getMetadata().instrumentTypeId);
        auto handle = fs->open(path, FileSystem::OpenOptions::kNone);
        if (!handle)
        {
            return false;
        }
        std::vector<uint8_t> bytes;
        uint8_t chunk[4096];
        for (;;)
        {
            int32_t n = fs->read(*handle, std::span<uint8_t>(chunk, sizeof(chunk)));
            if (n <= 0)
            {
                break;
            }
            bytes.insert(bytes.end(), chunk, chunk + n);
        }
        fs->close(*handle);
        p.rawBytes = std::move(bytes);
        return true;
    }

    bool savePatch(const std::string& name, const Patch& patch) override
    {
        const auto& p = static_cast<const StubPatch&>(patch);
        int typeId = p.getMetadata().instrumentTypeId;
        if (typeFolders)
        {
            fs->createFolder(std::to_string(typeId));
        }
        auto handle = fs->open(pathFor(name, typeId), FileSystem::OpenOptions::kWrite);
        if (!handle)
        {
            return false;
        }
        bool ok = p.rawBytes.empty() || fs->write(*handle, std::span<const uint8_t>(p.rawBytes));
        fs->close(*handle);
        return ok;
    }

    std::optional<InstrumentTypeId> loadPatchInstrumentType(const std::string& name) override
    {
        if (typeFolders)
        {
            for (const auto& [typeId, _] : sortedTypes())
            {
                if (auto h = fs->open(pathFor(name, typeId), FileSystem::OpenOptions::kNone))
                {
                    fs->close(*h);
                    return static_cast<InstrumentTypeId>(typeId);
                }
            }
        }
        return std::nullopt; // the blob format is opaque to the stub
    }

  private:
    std::string pathFor(const std::string& name, int typeId) const
    {
        return (typeFolders ? std::to_string(typeId) + "/" : std::string()) + name + kFileExtension;
    }

    std::map<int, std::shared_ptr<InstrumentType>> sortedTypes() const
    {
        const auto& m = registry->getInstrumentTypes();
        return { m.begin(), m.end() };
    }

    std::vector<std::string> scan(int typeId)
    {
        std::vector<std::string> names;
        auto files = fs->listFiles(typeFolders ? std::to_string(typeId) : std::string(),
                                   [](const std::string& n)
                                   {
                                       return n.size() > kFileExtension.size() &&
                                              n.compare(n.size() - kFileExtension.size(),
                                                        kFileExtension.size(),
                                                        kFileExtension) == 0;
                                   },
                                   std::nullopt);
        for (auto f : files)
        {
            names.emplace_back(f.substr(0, f.size() - kFileExtension.size()));
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    std::shared_ptr<FileSystem> fs;
    std::shared_ptr<InstrumentTypeRegistry> registry;
    bool typeFolders;
    std::vector<std::string> listed;
};

// ---------------------------------------------------------------------------------------------
// Remaining abstract components: inert but state-keeping
// ---------------------------------------------------------------------------------------------
class StubPatchPool final : public PatchPool
{
  public:
    bool getPatch(int, Patch*, bool) override { return false; }
    std::optional<std::string> getPatchName(int) override { return std::nullopt; }
};

class StubInstrument final : public Instrument
{
  public:
    void assignPatch(std::shared_ptr<Patch> p) override { patch = std::move(p); }
    void connect(MidiSource&) override {}
    void setMidiChannel(int c) override { channel = c; }
    int getMidiChannel() const override { return channel; }
    void setLocalMode(LocalMode m) override { mode = m; }
    LocalMode getLocalMode() const override { return mode; }
    int getVoiceCount() const override { return voices; }
    void setVoiceCount(int v) override { voices = v; }

  private:
    std::shared_ptr<Patch> patch;
    int channel = 0;
    LocalMode mode = LocalMode::kOn;
    int voices = 0;
};

class StubMixer final : public Mixer
{
  public:
    float getChannelGain(int c) override { return gain[c]; }
    void setChannelGain(int c, float g) override { gain[c] = g; }
    float getChannelPanning(int c) override { return pan[c]; }
    void setChannelPanning(int c, float p) override { pan[c] = p; }
    float getChannelSend(int c, int s) override { return send[{ c, s }]; }
    void setChannelSend(int c, int s, float g) override { send[{ c, s }] = g; }
    float getReturnChannelSend(int c, int s) override { return ret[{ c, s }]; }
    void setReturnChannelSend(int c, int s, float g) override { ret[{ c, s }] = g; }
    void connectInput(int, std::shared_ptr<Instrument>) override {}
    void disconnectInput(int) override {}
    void connectSend(int, std::shared_ptr<Instrument>) override {}

  private:
    std::map<int, float> gain, pan;
    std::map<std::pair<int, int>, float> send, ret;
};

class StubGrid final : public Grid
{
  public:
    void enqueuePadEvent(const PadEvent&) override {}
    void subscribePadEvents(std::weak_ptr<PadEventCallback>) override {}
};

class StubGridMapper final : public GridMapper
{
  public:
    void connect(Grid&) override {}
    const std::optional<PadConfig>& getPadConfig(int r, int c) const override
    {
        static const std::optional<PadConfig> kNone;
        auto it = pads.find({ r, c });
        return it == pads.end() ? kNone : it->second;
    }
    void setPadConfig(int r, int c, const PadConfig& cfg) override { pads[{ r, c }] = cfg; }
    void clearPadConfig(int r, int c) override { pads.erase({ r, c }); }
    void clearAllPadsConfig() override { pads.clear(); }
    void setScale(const Musicology::Scale& s) override { scale = s; }
    const Musicology::Scale& getScale() const override { return scale; }
    void subscribeMidiEvents(int, std::weak_ptr<MidiEventCallback>) override {}

  private:
    std::map<std::pair<int, int>, std::optional<PadConfig>> pads;
    Musicology::Scale scale{ Musicology::kChromaticScale, 0 };
};

class StubGridMapperStore final : public GridMapperStore
{
  public:
    bool loadPreset(GridMapper&, const std::string&) override { return false; }
    bool savePreset(GridMapper&, const std::string&) override { return false; }
    std::vector<std::string_view> listPresets() override { return {}; }
};

class StubSampleStore final : public SampleStore
{
  public:
    std::optional<Sample> loadSample(const std::string&, std::span<sample_store_t>) override
    {
        return std::nullopt;
    }
    tiny_optional<int> getSampleSize(const std::string&) override { return std::nullopt; }
    std::vector<std::string_view> listSamples() override { return {}; }
};

class StubSequencer final : public Sequencer
{
  public:
    void setTempo(float t) override { tempo = t; }
    TempoProvider getTempoProvider() const override
    {
        float t = tempo;
        return [t] { return tiny::optional<float>(t); };
    }
    void setClockInternal(bool) override {}
    void setTransportInternal(bool) override {}
    void setClockOut(bool) override {}
    void setTransportOut(bool) override {}
    void setWaitForExternalStart(bool) override {}
    void startSequence(int, int, int trackId, uint8_t) override { started[trackId] = true; }
    bool isStarted(int trackId) const override
    {
        auto it = started.find(trackId);
        return it != started.end() && it->second;
    }
    void stopSequence(int trackId) override { started[trackId] = false; }

  private:
    float tempo = kDefaultTempo;
    std::map<int, bool> started;
};

// ---------------------------------------------------------------------------------------------
// Synth
// ---------------------------------------------------------------------------------------------
class StubSynth final : public Synth
{
  public:
    explicit StubSynth(const Allocator& alloc)
      : registry(std::make_shared<InstrumentTypeRegistry>())
    {
        InstrumentTypeRegistry::registerInstrumentTypes(*registry, alloc);
    }

    std::unique_ptr<PatchStore> createPatchStore(FileSystemId id,
                                                 bool instrumentTypeFolderStructure) override
    {
        return std::make_unique<StubPatchStore>(fileSystem(id), registry,
                                                instrumentTypeFolderStructure);
    }
    std::unique_ptr<PatchPool> createPatchPool(
      std::shared_ptr<PatchStore>,
      int,
      std::function<tiny::optional<int>(const std::string&)>) override
    {
        return std::make_unique<StubPatchPool>();
    }
    std::shared_ptr<Instrument> createInstrument(int) override
    {
        return std::make_shared<StubInstrument>();
    }
    std::unique_ptr<Mixer> createMixer(int, int) override { return std::make_unique<StubMixer>(); }
    std::unique_ptr<Grid> createGrid() override { return std::make_unique<StubGrid>(); }
    std::unique_ptr<GridMapperStore> createGridMapperStore(FileSystemId) override
    {
        return std::make_unique<StubGridMapperStore>();
    }
    std::unique_ptr<GridMapper> createGridMapper() override
    {
        return std::make_unique<StubGridMapper>();
    }
    std::unique_ptr<SampleStore> createSampleStore(FileSystemId) override
    {
        return std::make_unique<StubSampleStore>();
    }
    std::shared_ptr<Sequencer> createSequencer() override
    {
        return std::make_shared<StubSequencer>();
    }

    void addMidiSource(const std::string&, std::shared_ptr<MidiSource>) override {}
    void addMidiSink(const std::string&, std::unique_ptr<MidiSink>) override {}
    void addFileSystem(FileSystemId id, std::unique_ptr<FileSystem> fs) override
    {
        fileSystems[id] = std::shared_ptr<FileSystem>(std::move(fs));
    }
    void initSamplePool(size_t, std::shared_ptr<SampleStore>) override {}

    std::string getInstrumentName(int typeId) override
    {
        auto t = registry->getInstrumentType(typeId);
        return t ? t->name : std::string();
    }
    std::vector<std::string_view> getInstrumentTypeNames() override
    {
        std::vector<std::string_view> out;
        for (const auto& [_, t] : sorted())
        {
            out.emplace_back(t->name);
        }
        return out;
    }
    std::vector<int> getInstrumentTypeIds() override
    {
        std::vector<int> out;
        for (const auto& [id, _] : sorted())
        {
            out.push_back(id);
        }
        return out;
    }

    void setup(std::span<std::shared_ptr<Instrument>>,
               std::span<std::shared_ptr<Instrument>>,
               std::shared_ptr<Mixer>,
               TempoProvider) override
    {
    }
    void disconnectInstrument(int) override {}

    void renderAudioBuffer(AudioBufferStereoRef buffer) override
    {
        for (auto& ch : buffer)
        {
            std::fill(ch.begin(), ch.end(), sample_t(0));
        }
    }
    void renderAudioBuffer(AudioBufferStereoRef bufferMain,
                           std::span<AudioBufferRef> bufferAux,
                           int) override
    {
        renderAudioBuffer(bufferMain);
        for (auto& ch : bufferAux)
        {
            std::fill(ch.begin(), ch.end(), sample_t(0));
        }
    }
    void updateAudio() override {}
    void updateMain() override {}

  private:
    std::shared_ptr<FileSystem> fileSystem(FileSystemId id) const
    {
        auto it = fileSystems.find(id);
        return it == fileSystems.end() ? nullptr : it->second;
    }
    std::map<int, std::shared_ptr<InstrumentType>> sorted() const
    {
        const auto& m = registry->getInstrumentTypes();
        return { m.begin(), m.end() };
    }

    std::shared_ptr<InstrumentTypeRegistry> registry;
    std::map<FileSystemId, std::shared_ptr<FileSystem>> fileSystems;
};

} // namespace

std::shared_ptr<Synth> Synth::createSynth(Allocator largeBuffersAlloc)
{
    return std::make_shared<StubSynth>(largeBuffersAlloc);
}

} // namespace polysynth
