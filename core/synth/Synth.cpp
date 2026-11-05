#ifdef FEATURE_SYNTH

#include "Synth.h"
#include "FileSystemAdapter.h"
#include "MidiAdapter.h"
#include "config.h"

#include "fileTransfer.h"




#include "PolySynth/Instrument.h"
#include "PolySynth/Mixer.h"
#include "PolySynth/PatchPool.h"
#include "PolySynth/PatchStore.h"
#include "PolySynth/Synth.h"
#include "PolySynth/config.h"

#include "PolyUtil/misc.h"
#include "fmt/core.h"

#include "util.h"
#include "util_math.h"

#include "PolySynth/Patch.h"
#include "fileManager.h"
#include "interruptsControl.h"
#include "mtStructs.h"
#include <numeric>

static const std::array kDefaultSettings = {
    SynthPartSettings{ .gainDb = Synth::kGainDefault,
                       .voiceCount = 1,
                       .synthType = 3,
                       .panning = Synth::kPanningDefault,
                       .patchName = {},
                       .reverbSend = Synth::kSendDefaultValue,
                       .delaySend = Synth::kSendDefaultValue,
                       .macroValue = { 0 } },
    SynthPartSettings{ .gainDb = Synth::kGainDefault,
                       .voiceCount = 1,
                       .synthType = 0,
                       .panning = Synth::kPanningDefault,
                       .patchName = {},
                       .reverbSend = Synth::kSendDefaultValue,
                       .delaySend = Synth::kSendDefaultValue,
                       .macroValue = { 0 } },
    SynthPartSettings{ .gainDb = Synth::kGainDefault,
                       .voiceCount = 1,
                       .synthType = 2,
                       .panning = Synth::kPanningDefault,
                       .patchName = {},
                       .reverbSend = Synth::kSendDefaultValue,
                       .delaySend = Synth::kSendDefaultValue,
                       .macroValue = { 0 } },
    SynthPartSettings{ .gainDb = -95,
                       .voiceCount = 0,
                       .synthType = 0,
                       .panning = 0,
                       .patchName = {},
                       .reverbSend = 0.0f,
                       .delaySend = 0.0f,
                       .macroValue = { 0 } },
};

#ifdef FEATURE_SYNTH

static constexpr int kFirstSendCC = MidiAdapter::kTrackCCRange.first;
static constexpr std::array<uint8_t, kSynthCCs> defaultCCs = {
    105, 106, 102, 103, kFirstSendCC + 1, kFirstSendCC, 107, 104
};

static constexpr std::array kMacroKnobNames = { "Sample Attack", "Sample Decay",     "Bit Depth",
                                                "Filter Cutoff", "Filter Resonance", "Overdrive",
                                                "Volume" };

static constexpr int kInstrumentTypeMult = 1000;
static constexpr auto kPatchNameFormat = "%02d-%02d "; // TT-PP type-program
static constexpr auto kPatchNameTypeLen = 3;           // 2 chars of type idx + '-'

// required because of forward declarations of polysynth classes in the header
Synth::Synth() = default;
Synth::~Synth() = default;

void Synth::init(Platform* platform, std::function<tiny_optional<float>()> getTempo)
{
    synth = polysynth::Synth::createSynth(
      [](size_t)
      {
          // We don't want to use modules requiring allocation
          assert(false);
          return nullptr;
      });

    FileSystemAdapter::staticInit();

    filesystem = platform->getFileSystem();
    synth->addFileSystem(
      polysynth::Synth::FileSystemId::kPatches,
      std::make_unique<FileSystemAdapter>(std::string(kPatchesFolder) + "/", filesystem));
    synth->addFileSystem(
      polysynth::Synth::FileSystemId::kWorkspace,
      std::make_unique<FileSystemAdapter>(std::string(kWorkspaceFolder) + "/", filesystem));

    patchStore = synth->createPatchStore(polysynth::Synth::FileSystemId::kPatches, true);
    patchStoreWorkspace =
      synth->createPatchStore(polysynth::Synth::FileSystemId::kWorkspace, false);

    midiAdapter = std::make_unique<MidiAdapter>(nullptr);

    auto instruments = std::array<std::shared_ptr<polysynth::Instrument>, kSynthParts>{};
    for (auto& part : parts)
    {
        int partIdx = std::distance(parts.begin(), &part);
        auto& instrument = part.instrument;

        part.instrumentTypeId = kSynthTypes[partIdx];
        part.patchName = kInitPatchName;
        setPatch(partIdx, patchStore->createPatch(*part.instrumentTypeId), part.patchName);

        instrument = synth->createInstrument(partIdx);
        instrument->assignPatch(part.patch);
        instrument->connect(*midiAdapter);
        instrument->setVoiceCount(1);
        instrument->setMidiChannel(partIdx);

        instruments[partIdx] = instrument;
    }

    synth->setup(instruments, {}, nullptr, std::move(getTempo));

    for (int part = 0; part < kSynthParts; part++)
    {
        Synth::initSynthPartSettings(part, mtProject.values.synthPart[part]);
    }

    updateToSettings(mtProject.values);
}

::Midi* Synth::getVirtualMidiPort()
{
    return &*midiAdapter;
}

std::vector<std::string_view> Synth::getPatchList(int part)
{
    newFileManager.createPatchesFolder();

    return patchStore->listPatchesByType(*parts[part].instrumentTypeId);
}

bool Synth::loadPatch(int part, const char* name)
{
    assert(part < kSynthParts);

    if (patchStore->loadPatch(name, *parts[part].patch, true))
    {
        setPatch(part, std::move(parts[part].patch), name);
        return true;
    }

    return false;
}

bool Synth::savePatch(int part, const char* name)
{
    assert(part < kSynthParts);

    newFileManager.createPatchesFolder();

    if (name != nullptr)
    {
        parts[part].patchName = name;
    }

    return patchStore->savePatch(parts[part].patchName, *parts[part].patch);
}

bool Synth::savePatch(int part)
{
    assert(part < kSynthParts);

    return savePatch(part, nullptr);
}

void Synth::initPatch(int part)
{
    assert(part < kSynthParts);

    std::shared_ptr<polysynth::Patch> newPatch =
      patchStore->createPatch(*parts[part].instrumentTypeId);

    DISABLE_AUDIO_INTERRUPTS();
    parts[part].instrument->assignPatch(newPatch);
    ENABLE_AUDIO_INTERRUPTS();

    setPatch(part, std::move(newPatch), kInitPatchName);
    savePatchToWorkspace(part);
}

void Synth::savePatchToWorkspace(int part)
{
    patchStoreWorkspace->savePatch(fmt::format("{}", part), *parts[part].patch);
    parts[part].patchDirty = false;
}

bool Synth::loadPatchFromWorkspace(int part, std::optional<std::string> overrideName)
{
    bool fileLoaded = true;
    if (overrideName)
    {
        parts[part].patchName = std::move(*overrideName);
    }

    if (!patchStoreWorkspace->loadPatch(fmt::format("{}", part), *parts[part].patch, true))
    {
        if (!loadPatch(part, parts[part].patchName.c_str()))
        {
            initPatch(part);
            fileLoaded = false;
        }
    }

    parts[part].patchDirty = false;
    return fileLoaded;
}

void Synth::saveDirtyWorkspacePatches()
{
    for (int part = 0; part < kSynthParts; part++)
    {
        if (parts[part].patchDirty)
        {
            savePatchToWorkspace(part);
        }
    }
}

void Synth::setVoiceCount(int part, int voiceCount)
{
    if (parts[part].instrument->getVoiceCount() != voiceCount)
    {
        DISABLE_AUDIO_INTERRUPTS();
        parts[part].instrument->setVoiceCount(voiceCount);
        ENABLE_AUDIO_INTERRUPTS();
    }

    assert(std::accumulate(parts.begin(),
                           parts.end(),
                           0,
                           [](int acc, const Part& part)
                           { return acc + part.instrument->getVoiceCount(); }) <= kSynthMaxVoices);
}

int Synth::getMaxVoiceCount(int part)
{
    int otherPartsVoiceCount = std::accumulate(parts.begin(),
                                               parts.end(),
                                               -parts[part].instrument->getVoiceCount(),
                                               [](int acc, const Part& part)
                                               { return acc + part.instrument->getVoiceCount(); });

    assert(parts[part].patch);
    return std::min(kSynthMaxVoices - otherPartsVoiceCount,
                    parts[part].patch->getMetadata().maxVoices);
}

void Synth::setInstrumentType(int partIdx, polysynth::InstrumentTypeId typeId)
{
    auto& part = parts[partIdx];
    if (part.instrumentTypeId != typeId)
    {
        part.instrumentTypeId = typeId;
        setPatch(partIdx, patchStore->createPatch(*typeId), kInitPatchName);

        if (part.instrument->getVoiceCount() > getMaxVoiceCount(partIdx))
        {
            setVoiceCount(partIdx, getMaxVoiceCount(partIdx));
        }

        DISABLE_AUDIO_INTERRUPTS();
        part.instrument->assignPatch(part.patch);
        ENABLE_AUDIO_INTERRUPTS();
    }
}

void Synth::setPatch(int part, std::shared_ptr<polysynth::Patch> patch, std::string name)
{
    parts[part].patch = std::move(patch);
    parts[part].patchName = std::move(name);

    //    updateCCDefaults(part);
}

std::string Synth::getInstrumentTypeName(int typeIdx)
{
    return synth->getInstrumentName(*kSynthTypes.at(typeIdx));
}

int Synth::getInstrumentTypeIdx(int part)
{
    auto it = std::ranges::find(kSynthTypes, parts[part].instrumentTypeId);
    assert(it != kSynthTypes.end());

    return std::distance(kSynthTypes.begin(), it);
}

int Synth::getSend(int part, int sendIdx)
{
    return midiAdapter->getTrackedCCValue(part, kFirstSendCC + sendIdx) * kSendMaxValue /
           polysynth::MidiEvent::Data::ControlChange::kMaxValue;
}

polysynth::Patch& Synth::getPatch(int part)
{
    return *parts[part].patch;
}

std::string Synth::getPatchName(int part)
{
    return parts[part].patchName;
}

std::string Synth::getSynthName(int part)
{
    assert(part < kSynthParts);
    std::string synthName = parts[part].instrument->getVoiceCount() > 0
                              ? synth->getInstrumentName(*parts[part].instrumentTypeId)
                              : "Off";
    return fmt::format("Synth {} ({})", part + 1, synthName);
}

std::string Synth::getParamFullName(int part, int paramId)
{
    const auto& metadata = parts[part].patch->getMetadata();
    const auto& paramName = !metadata.params[paramId].displayName.empty()
                              ? metadata.params[paramId].displayName
                              : "Empty";

    for (const auto& section : metadata.layoutFlat.sections)
    {
        for (const auto& element : section.elements)
        {
            if (element.id == paramId &&
                element.type ==
                  polysynth::Patch::Metadata::EditorLayout::EditorElement::Type::kParam)
            {
                return (section.shortName.empty() ? section.name : section.shortName) + ' ' +
                       paramName;
            }
        }
        for (const auto& subsection : section.subsections)
        {
            for (const auto& element : subsection.elements)
            {
                if (element.id == paramId &&
                    element.type ==
                      polysynth::Patch::Metadata::EditorLayout::EditorElement::Type::kParam)
                {
                    return section.name + '>' + subsection.name + ' ' + paramName;
                }
            }
        }
    }

    return paramName;
}

const char* Synth::getMacroKnobName(int macroId)
{
    return macroId < std::ssize(kMacroKnobNames) ? kMacroKnobNames[macroId] : nullptr;
}

void Synth::updateToSettings(const strMtValues& settings, bool resetAllVoices)
{
    if (resetAllVoices)
    {
        for (int part = 0; part < kSynthParts; part++)
        {
            setVoiceCount(part, 0);
        }
    }

    for (int part = 0; part < kSynthParts; part++)
    {
        setVoiceCount(part, settings.synthPart[part].voiceCount);
        setInstrumentType(part, kSynthTypes.at(settings.synthPart[part].synthType));
        parts[part].patchName = settings.synthPart[part].patchName;
    }
}

#endif

void Synth::initSynthPartSettings(int part, SynthPartSettings& settings)
{
    settings = kDefaultSettings.at(part);

    initSynthPartMacrosValues(part, settings);

    snprintf(settings.patchName, std::size(settings.patchName), "%s", kInitPatchName);
}

void Synth::initSynthPartMacrosValues(int part, SynthPartSettings& settings)
{
    for (int macroIdx = 0; macroIdx < kSynthMacrosCount; macroIdx++)
    {
        initSynthPartMacroValue(part, macroIdx, settings);
    }
}

void Synth::initSynthPartMacroValue(int part, int macroIdx, SynthPartSettings& settings)
{
    const polysynth::Patch::Metadata& metadata = getPatch(part).getMetadata();
    settings.macroValue[macroIdx] = metadata.macros[macroIdx].bipolar ? kCCMidValue : kCCMinValue;
}

void Synth::validateSynthPartSettings(int part, SynthPartSettings& settings)
{

    bool anyMacroValueIsOurRange = false;
    for (auto macroValue : settings.macroValue)
    {
        if (isOutOfRange<int>(macroValue, 0, kCCMaxValue))
        {
            anyMacroValueIsOurRange = true;
            break;
        }
    }

    if (isOutOfRange<int>(settings.synthType, 0, std::ssize(kSynthTypes) - 1) ||
        isOutOfRange<int>(settings.voiceCount, 0, kSynthMaxVoices) ||
        isOutOfRange<float>(settings.gainDb, kGainSilence, kGainMax) ||
        isOutOfRange<float>(settings.panning, -kPanningMax, kPanningMax) ||
        isOutOfRange<float>(settings.reverbSend, 0, kSendMaxValue) ||
        isOutOfRange<float>(settings.delaySend, 0, kSendMaxValue) || anyMacroValueIsOurRange)
    {
        initSynthPartSettings(part, settings);
    }
}

void Synth::triggerTestNote(int part, int note, bool noteOn)
{
    DISABLE_AUDIO_INTERRUPTS();
    if (noteOn)
    {
        midiAdapter->sendNoteOn(note, 100, part + 1);
    }
    else
    {
        midiAdapter->sendNoteOff(note, 100, part + 1);
    }

    ENABLE_AUDIO_INTERRUPTS();
}

void Synth::copyWorkspacePatches(const char* sourceProjectPath, const char* destinationProjectPath)
{
    for (int part = 0; part < kSynthParts; part++)
    {
        std::string srcFile =
          fmt::format("{}{}{}{}", sourceProjectPath, FileSystem::kPathSeparator, part, polysynth::PatchStore::kFileExtension);
        std::string dstFile = fmt::format(
          "{}{}{}{}", destinationProjectPath, FileSystem::kPathSeparator, part, polysynth::PatchStore::kFileExtension);
        cFileTransfer::copyFile(filesystem, srcFile.c_str(), dstFile.c_str());
    }
}

bool Synth::isAnyPatchDirty()
{
    for (int part = 0; part < kSynthParts; part++)
    {
        if (parts[part].patchDirty)
        {
            return true;
        }
    }
    return false;
}

#endif
