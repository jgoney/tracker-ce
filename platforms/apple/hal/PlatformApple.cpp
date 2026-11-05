#include "PlatformApple.h"

#include <sys/stat.h>

// Core input handlers (defined in core/, declared the same way platforms/imx/source/hardware.cpp does).
void onPotChange(uint8_t n, int16_t value);
void onButtonPush(uint8_t n);
void onButtonRelease(uint8_t n);
void onButtonHold(uint8_t n);
void onPadPush(uint8_t n);
void onPadRelease(uint8_t n);
void onPadHold(uint8_t n);

static std::string ensureRoot(const std::string& root)
{
    ::mkdir(root.c_str(), 0755);
    return root;
}

PlatformApple::PlatformApple(const std::string& sdRootPath)
    : fileSystem_(ensureRoot(sdRootPath)), config_(sdRootPath + "/.config.bin")
{
    // Folders a fresh Tracker+ card has / the core assumes exist.
    fileSystem_.mkdir(0, "/Projects");
    fileSystem_.mkdir(0, "/Patches");
    fileSystem_.mkdir(1, "/Workspace");
    connectInputToCore();
}

void PlatformApple::connectInputToCore()
{
    input_.buttons().setActionCallback(SwitchGroup::kPress, onButtonPush);
    input_.buttons().setActionCallback(SwitchGroup::kRelease, onButtonRelease);
    input_.buttons().setActionCallback(SwitchGroup::kHold, onButtonHold);
    input_.pads().setActionCallback(SwitchGroup::kPress, onPadPush);
    input_.pads().setActionCallback(SwitchGroup::kRelease, onPadRelease);
    input_.pads().setActionCallback(SwitchGroup::kHold, onPadHold);
    input_.encoder().setChangeCallback(onPotChange);
}
