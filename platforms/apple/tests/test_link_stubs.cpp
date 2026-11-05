// TEMPORARY, test-only definitions of platform symbols that TrackerCore references but that the
// Apple platform layer (Task 4: HAL + headless boot) will provide for real. Task 4 replaces this
// file; do not link it into the shipping app.
#include <cstdint>

void platformDisableGlobalInterrupts() {}
void platformEnableGlobalInterrupts() {}
void enableAudioInterrupt() {}
void disableAudioInterrupt() {}
uint8_t isAudioOutMuted() { return 0; }
void muteAudioOutByKey() {}
void unmuteAudioOutByKey() {}
int16_t sin_q15(int16_t) { return 0; } // hardware: CMSIS-style q15 sine

// Linker-script symbols read by core/source/Profiler.cpp (stack watermark).
extern "C++" {
void _vStackBase(void) {}
void _vStackTop(void) {}
}
