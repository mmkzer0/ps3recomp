/* iOS libsystem_platform returns ENOTSUP; override only the selected call. */
#if defined(__APPLE__)
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE 1
#endif
#include <errno.h>
#include <ucontext.h>
#if HF_FAIL_getcontext
int getcontext(ucontext_t* uc) { (void)uc; errno = ENOTSUP; return -1; }
#elif HF_FAIL_swapcontext
int swapcontext(ucontext_t* from, const ucontext_t* to)
{ (void)from; (void)to; errno = ENOTSUP; return -1; }
#elif HF_FAIL_setcontext
int setcontext(const ucontext_t* uc) { (void)uc; errno = ENOTSUP; return -1; }
#else
#error "Select exactly one failing host context call"
#endif
