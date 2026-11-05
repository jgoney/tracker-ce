// Interrupt control for the host: both masking pairs forward to AudioRuntime, which shares one
// nesting depth between them, holds the render thread (the "audio interrupt") off while it is
// > 0, and ignores them on the render thread itself. See runtime/AudioRuntime.h.
#include "AudioRuntime.h"

void platformDisableGlobalInterrupts()
{
    AudioRuntime::irqDisable();
}

void platformEnableGlobalInterrupts()
{
    AudioRuntime::irqEnable();
}

void disableAudioInterrupt()
{
    AudioRuntime::irqDisable();
}

void enableAudioInterrupt()
{
    AudioRuntime::irqEnable();
}
