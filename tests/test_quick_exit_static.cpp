/*
 * ps3recomp - the C++ static whose destructor must NOT run on a guest exit
 * (see tests/test_quick_exit.c).
 *
 * A guest exit ends the host process while other threads still use the
 * runtime's global state, so the host uses quick_exit() and this destructor is
 * never called. exit() would call it, teardown order and all. The marker goes
 * to stderr, which is unbuffered, so finding it (or not) cannot depend on a
 * flush.
 */
#include <cstdio>

namespace {

struct StaticMarker {
    ~StaticMarker()
    {
        std::fputs("[quick-exit] static destructor ran\n", stderr);
    }
};

StaticMarker g_static_marker;

} // namespace
