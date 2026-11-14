/* Compile-time SPU context layout pin check.
 *
 * The signature is a constant expression over the members generated code and
 * its inline helpers touch. It must match the pin for this data model at build
 * time, on every host the toolkit supports. Cross-compiling this file for
 * Windows x64 with `_WIN32` and `_WIN64` defined checks the Win64 pin without
 * running a Windows binary; the Windows t0 job runs the runtime probe natively.
 */
#include "ps3emu/lifter_abi.h"
#include "spu_context.h"

_Static_assert(SPU_RUNTIME_LIFTER_CONTEXT_LAYOUT_SIGNATURE(spu_context) ==
               SPU_RUNTIME_LIFTER_CONTEXT_LAYOUT_SIGNATURE_PIN,
               "spu_context layout used by lifted code changed: bump SPU_LIFTER_ABI "
               "and update the data-model layout pin");
