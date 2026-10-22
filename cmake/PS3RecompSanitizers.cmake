# Sanitizer builds.
#
# The flags are directory-wide compile and link options, set before any target
# exists. That way they reach the runtime, the tests and the host harnesses
# alike: an uninstrumented runtime under an instrumented test misses exactly
# the races and overflows the test is there to find.
#
# A title that builds this tree through add_subdirectory() includes this file
# from its own top level first. The guard then skips this tree's include, and
# the title's directory options reach both trees once.
#
# ASan/UBSan and TSan cannot share a process, so they cannot share a build tree.
# UBSan does not recover: the first undefined operation fails the test, so a
# report cannot scroll by unnoticed in a passing run.

include_guard(GLOBAL)

option(PS3RECOMP_SANITIZE "Instrument with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PS3RECOMP_SANITIZE_THREAD "Instrument with ThreadSanitizer" OFF)

if(PS3RECOMP_SANITIZE AND PS3RECOMP_SANITIZE_THREAD)
    message(FATAL_ERROR "PS3RECOMP_SANITIZE and PS3RECOMP_SANITIZE_THREAD need separate build trees")
endif()

if(PS3RECOMP_SANITIZE OR PS3RECOMP_SANITIZE_THREAD)
    if(MSVC)
        message(FATAL_ERROR "Sanitizer builds need Clang or GCC")
    endif()
    add_compile_options(-fno-omit-frame-pointer)
endif()

if(PS3RECOMP_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(PS3RECOMP_SANITIZE_THREAD)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()
