/* The new import must report unavailable presence publishing as an API error,
 * rather than unresolved HLE or fabricated successful PSN publication.
 *
 * The error family itself is pinned on the header: the Basic module's codes
 * live at 0x8002A6xx (RPCS3 sceNp.h, tier RPCS3 -- the NP modules are not
 * available decrypted); the 0x8002AAxx values this header used to carry are
 * the sceNp core library's family, so a revert to them fails here. The
 * signaling header's constants are pinned the same way: the old 0x80022A0x
 * values match no NP family (#127).
 *
 * The import is called the way the PPU does: through the registered HLE adapter,
 * arguments in r3/r4 (the presence block is a guest effective address the
 * handler does not read), result in r3. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../libs/network/sceNpBasic.h"
#include "../libs/network/sceNpSignaling.h"
#include "ppu_context.h"
typedef void (*hle_fn)(ppu_context*);
static hle_fn handler;
uint8_t* vm_base;
void ppu_hle_register_all(void);
void ps3_hle_register(unsigned nid,const char* name,hle_fn fn){(void)name;if(nid==0xBE81C71C)handler=fn;}
static uint32_t call(uint32_t ea,uint32_t options){
 ppu_context ctx;memset(&ctx,0,sizeof ctx);
 ctx.gpr[3]=ea;ctx.gpr[4]=options;handler(&ctx);return (uint32_t)ctx.gpr[3];
}
int main(void){
 /* Header family pins: each failed against the pre-#127 header values. */
 assert(SCE_NP_BASIC_ERROR_NOT_SUPPORTED==0x8002A663);
 assert(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT==0x8002A665);
 assert(SCE_NP_BASIC_ERROR_NOT_INITIALIZED==0x8002A662);
 assert(SCE_NP_BASIC_ERROR_NOT_CONNECTED==0x8002A678);
 assert(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED==0x8002A801);
 assert(SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED==0x8002A802);
 assert(SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY==0x8002A803);
 assert(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND==0x8002A805);
 assert(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND==0x8002A80E);
 assert(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT==0x8002A815);
 ppu_hle_register_all();assert(handler);
 assert(call(0xFEFEEB0,3)==SCE_NP_BASIC_ERROR_NOT_SUPPORTED);
 assert(call(0xFEFEEB0,1)==SCE_NP_BASIC_ERROR_NOT_SUPPORTED);
 assert(call(0xFEFEEB0,2)==SCE_NP_BASIC_ERROR_NOT_SUPPORTED);
 assert(call(0,3)==SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
 assert(call(0xFEFEEB0,0)==SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
 assert(call(0xFEFEEB0,4)==SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
 puts("NP presence details: real import and explicit unavailable publication PASS");
}
