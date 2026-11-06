/* A local account has no purchased entitlements. Exercise the real import,
 * initialization guard, count query and preservation of unused output.
 *
 * The import is called the way the PPU does: through the registered HLE adapter,
 * with the guest's arguments in r3 and r4 and the result in r3 (a guest effective
 * address for the list, so the "unused output" is a guest-memory span). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppu_context.h"
#include "../libs/network/sceNp.h"
typedef void (*hle_fn)(ppu_context*);
static hle_fn handler;
uint8_t* vm_base;
void ppu_hle_register_all(void);
const char* np_psnr_online_id(void){return "PlayerRetail";}
void ps3_hle_register(unsigned nid,const char* name,hle_fn fn){(void)name;if(nid==0xB66D1C46)handler=fn;}
static int32_t call(uint32_t ea,uint32_t n){
 ppu_context ctx;memset(&ctx,0,sizeof ctx);
 ctx.gpr[3]=ea;ctx.gpr[4]=n;handler(&ctx);return (int32_t)ctx.gpr[3];
}
int main(void){
 vm_base=calloc(1,0x10000);assert(vm_base);
 ppu_hle_register_all();assert(handler);
 assert(call(0,0)==(int32_t)SCE_NP_ERROR_NOT_INITIALIZED);
 assert(sceNpInit(0,0)==0);
 assert(call(0,0)==0);
 const uint32_t ea=0x1000;
 for(unsigned i=0;i<32;i++)vm_base[ea+i]=0xa5;
 assert(call(ea,1)==0);
 for(unsigned i=0;i<32;i++)assert(vm_base[ea+i]==0xa5);
 assert(sceNpTerm()==0);
 assert(call(0,0)==(int32_t)SCE_NP_ERROR_NOT_INITIALIZED);
 puts("NP entitlements: real import, lifecycle and empty list PASS");
}
