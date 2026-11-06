#include "StubsApple.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

// --- ConfigManagerApple -------------------------------------------------------------------

ConfigManagerApple::ConfigManagerApple(std::string path) : path_(std::move(path))
{
    int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd >= 0)
    {
        char zeros[kInitialSize] = {};
        (void)!::write(fd, zeros, sizeof(zeros));
        ::close(fd);
    }
}

bool ConfigManagerApple::readData(int address, void* data, int size)
{
    if (address < 0 || size < 0)
        return false;
    int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    std::memset(data, 0, (size_t)size);
    (void)!::pread(fd, data, (size_t)size, (off_t)address); // short read leaves zeros
    ::close(fd);
    return true;
}

void ConfigManagerApple::writeData(int address, void* data, int size)
{
    if (address < 0 || size <= 0)
        return;
    int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return;
    (void)!::pwrite(fd, data, (size_t)size, (off_t)address);
    ::close(fd);
}

// --- Symbols the core expects from the platform -------------------------------------------

namespace
{
std::atomic<bool> gAudioOutMuted{ false };
} // namespace

void muteAudioOutByKey()
{
    gAudioOutMuted = true;
}
void unmuteAudioOutByKey()
{
    gAudioOutMuted = false;
}
uint8_t isAudioOutMuted()
{
    return gAudioOutMuted ? 1 : 0;
}

// Same range and semantics as CMSIS arm_sin_q15: input is an angle in q15 where 0x0000..0x7FFF
// spans [0, 2*pi) (negative values wrap), output is sin() in q15.
int16_t sin_q15(int16_t x)
{
    const double kTwoPi = 6.283185307179586476925286766559;
    double angle = (double)(x & 0x7FFF) / 32768.0 * kTwoPi;
    long v = std::lround(std::sin(angle) * 32768.0);
    if (v > 32767)
        v = 32767;
    if (v < -32768)
        v = -32768;
    return (int16_t)v;
}

// Core's Profiler::maxStackBytes() scans the bytes between the linker-script symbols _vStackBase
// and _vStackTop for a watermark. The host has no such symbols, so a static array stands in:
// _vStackBase is the array and _vStackTop its end (asm alias, the names must stay unmangled).
extern "C" uint8_t _vStackBase[64 * 1024];
uint8_t _vStackBase[64 * 1024];
__asm__(".globl __vStackTop\n.set __vStackTop, __vStackBase + 65536\n");
