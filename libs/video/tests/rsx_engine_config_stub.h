/*
 * ps3recomp - the typed configuration the draw engine reads, for fixtures that do not link
 * runtime/config/config.cpp
 *
 * The engine reads eng.depth_clamp and eng.depth_bounds through ps3_cfg_eng_*() (v0.19.0 #693,
 * #694). A fixture without the real registry answers ps3_config_get() here with the registry's
 * defaults (both on, docs/CONFIG.md). A fixture that proves the off switch (test_metal_depth_clamp.m,
 * test_metal_depth_bounds.m, run a second time with PS3RECOMP_ENG_DEPTH_CLAMP=0 / _BOUNDS=0 in
 * macos.yml) gets it from the same environment variable the registry would read; the registry's own
 * parsing is covered by tests/gates/test_config.py.
 *
 * Include it once per executable, after "ps3emu/config.h"-using code is declared; a fixture that
 * defines its own ps3_config_get() does not include it and sets eng_depth_clamp / eng_depth_bounds
 * itself.
 */
#ifndef PS3RECOMP_RSX_ENGINE_CONFIG_STUB_H
#define PS3RECOMP_RSX_ENGINE_CONFIG_STUB_H

#include "ps3emu/config.h"

#include <stdlib.h>

static void rsx_engine_config_set_defaults(ps3_config* cfg)
{
    cfg->eng_depth_clamp = true;
    cfg->eng_depth_bounds = true;
}

const ps3_config* ps3_config_get(void)
{
    static ps3_config cfg;
    static int init;
    if (!init) {
        init = 1;
        rsx_engine_config_set_defaults(&cfg);
        const char* c = getenv("PS3RECOMP_ENG_DEPTH_CLAMP");
        const char* b = getenv("PS3RECOMP_ENG_DEPTH_BOUNDS");
        if (c && *c == '0') cfg.eng_depth_clamp = false;
        if (b && *b == '0') cfg.eng_depth_bounds = false;
    }
    return &cfg;
}

#endif
