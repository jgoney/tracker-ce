#ifndef PLATFORM_TRAITS_H_
#define PLATFORM_TRAITS_H_

namespace platform
{
#if defined(PLATFORM_APPLE)
inline constexpr bool kIsApple = true;
#else
inline constexpr bool kIsApple = false;
#endif
} // namespace platform

#endif /* PLATFORM_TRAITS_H_ */
