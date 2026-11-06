#ifndef SPUEXH_BACKEND_PPU_H
#define SPUEXH_BACKEND_PPU_H
#include "spuexh_core.h"
extern spuexh_backend_t spuexh_ppu_backend;
int spuexh_ppu_init(void);     /* sys_spu_initialize and the import of the embedded SPU program; 0 on success */
#endif
