/* Registers the reference implementation as the HLE handlers of the synthetic
 * module, the way tools/gen_hle_nids.py registers a library's handlers: one
 * ps3_hle_register(NID, name, &ps3_hle_adapt<&fn>) per export, so the register
 * marshalling of the real HLE adapter is part of what is exercised. The NIDs
 * come from the same manifest the module is packaged with (synth_pack.py). */
#include "ppu_hle_adapt.hpp"
#include "ref_native.h"
#include "syn_nids.h"

extern "C" void ppu_hle_register_all(void)
{
    ps3_hle_register(SYN_NID_synObjInit,     "synObjInit",     &ps3_hle_adapt<&synObjInit>);
    ps3_hle_register(SYN_NID_synObjCommand,  "synObjCommand",  &ps3_hle_adapt<&synObjCommand>);
    ps3_hle_register(SYN_NID_synObjPush,     "synObjPush",     &ps3_hle_adapt<&synObjPush>);
    ps3_hle_register(SYN_NID_synBufChecksum, "synBufChecksum", &ps3_hle_adapt<&synBufChecksum>);
    ps3_hle_register(SYN_NID_synBits,        "synBits",        &ps3_hle_adapt<&synBits>);
    ps3_hle_register(SYN_NID_synFpu,         "synFpu",         &ps3_hle_adapt<&synFpu>);
    /* the import the module makes: a host helper, reachable only through the import policy */
    ps3_hle_register(SYN_NID_synHostMemset,  "synHostMemset",  &ps3_hle_adapt<&synHostMemset>);
}
