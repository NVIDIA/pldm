#pragma once

#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#endif

namespace pldm::test
{

inline bool runningOnValgrind()
{
#ifdef RUNNING_ON_VALGRIND
    return RUNNING_ON_VALGRIND != 0;
#else
    return false;
#endif
}

inline bool runningWithAddressSanitizer()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__clang__)
#if __has_feature(address_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

} // namespace pldm::test
