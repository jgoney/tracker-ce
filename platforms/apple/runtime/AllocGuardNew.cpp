// operator new hook for AudioRuntime's allocation guard (the IMX post-init malloc check,
// PlatformTrackerIMX.cpp:66-77, moved to "no allocation on the audio thread").
// An object library of its own so a binary can link it, or define its own operator new
// (e.g. a counting one in a test) without a duplicate symbol.
#include "AudioRuntime.h"

#include <cstdlib>
#include <new>

namespace
{
void* guardedAlloc(size_t size)
{
    AudioRuntime::allocGuardCheck(size);
    return std::malloc(size ? size : 1);
}

void* guardedAlignedAlloc(size_t size, size_t align)
{
    AudioRuntime::allocGuardCheck(size);
    if (align < sizeof(void*))
        align = sizeof(void*);
    void* p = nullptr;
    if (posix_memalign(&p, align, size ? size : 1) != 0)
        return nullptr;
    return p; // freed with free(), like every other block here
}
} // namespace

void* operator new(size_t size)
{
    void* p = guardedAlloc(size);
    if (!p)
        std::abort(); // -fno-exceptions: no bad_alloc
    return p;
}

void* operator new[](size_t size)
{
    return operator new(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept
{
    return guardedAlloc(size);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
    return guardedAlloc(size);
}

void* operator new(size_t size, std::align_val_t align)
{
    void* p = guardedAlignedAlloc(size, size_t(align));
    if (!p)
        std::abort();
    return p;
}

void* operator new[](size_t size, std::align_val_t align)
{
    return operator new(size, align);
}

void* operator new(size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
    return guardedAlignedAlloc(size, size_t(align));
}

void* operator new[](size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
    return guardedAlignedAlloc(size, size_t(align));
}

void operator delete(void* p, std::align_val_t) noexcept
{
    std::free(p);
}

void operator delete[](void* p, std::align_val_t) noexcept
{
    std::free(p);
}

void operator delete(void* p, size_t, std::align_val_t) noexcept
{
    std::free(p);
}

void operator delete[](void* p, size_t, std::align_val_t) noexcept
{
    std::free(p);
}

void operator delete(void* p) noexcept
{
    std::free(p);
}

void operator delete[](void* p) noexcept
{
    std::free(p);
}

void operator delete(void* p, size_t) noexcept
{
    std::free(p);
}

void operator delete[](void* p, size_t) noexcept
{
    std::free(p);
}
