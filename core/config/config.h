#ifndef CONFIG_CONFIG_H_
#define CONFIG_CONFIG_H_

#if defined(PLATFORM_APPLE)
// Host build: no CPSID/CPSIE, and Mach-O rejects ELF section names.
extern void platformDisableGlobalInterrupts();
extern void platformEnableGlobalInterrupts();
#define DISABLE_GLOBAL_INTERRUPTS() platformDisableGlobalInterrupts()
#define ENABLE_GLOBAL_INTERRUPTS()  platformEnableGlobalInterrupts()

#define DMAMEM
#define AUDIO_CORE_FLASH
#define AUDIO_CORE_RAM
#define OCRAM_BSS_SECTION
#else
#define DISABLE_GLOBAL_INTERRUPTS() __asm__ volatile("CPSID i" ::: "memory")
#define ENABLE_GLOBAL_INTERRUPTS()  __asm__ volatile("CPSIE i" ::: "memory")

#define DMAMEM            __attribute__((section(".dmabuffers"), used))
#define AUDIO_CORE_FLASH  __attribute__((section(".audio_flash_functions"), used))
#define AUDIO_CORE_RAM    __attribute__((section(".audio_ram_functions"), used))
#define OCRAM_BSS_SECTION __attribute__((section(".bss.$RAM3"), used))
#endif

static constexpr int kMaxFilesListLength = 256;
static constexpr int kMaxFilePathLen = 255;
static constexpr int kMaxFileNameLen = 80;

// !!! important: this value is supposed to be 0 if we do not include Synth
// if you need "independent" number of synth instruments use "kSynthInstrumentsCount" from
// mtStruth.h witch is always (3), this allows this constant to be used instead of the whole
// definition
#ifdef FEATURE_SYNTH
static constexpr int kSynthParts = 3;
#else
static constexpr int kSynthParts = 0;
#endif

static constexpr int kRedFrameWidth = 24;

static constexpr int kMidiChannelsPerPort = 16;
static constexpr int kSynthMaxVoices = 8;
static constexpr int kSynthCCs = 8;
static constexpr int kSends = 2;

const char* const kPatchesFolder = "Patches";
const char* const kWorkspaceFolder = "Workspace";

#endif /* CONFIG_CONFIG_H_ */
