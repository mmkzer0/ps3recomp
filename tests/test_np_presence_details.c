/* The new import must report unavailable presence publishing as an API error,
 * rather than unresolved HLE or fabricated successful PSN publication.
 *
 * The import is called the way the PPU does: through the registered HLE adapter,
 * arguments in r3/r4 (the presence block is a guest effective address the
 * handler does not read), result in r3. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
 ppu_hle_register_all();assert(handler);
 assert(call(0xFEFEEB0,3)==0x8002a663);
 assert(call(0xFEFEEB0,1)==0x8002a663);
 assert(call(0xFEFEEB0,2)==0x8002a663);
 assert(call(0,3)==0x8002a665);
 assert(call(0xFEFEEB0,0)==0x8002a665);
 assert(call(0xFEFEEB0,4)==0x8002a665);
 puts("NP presence details: real import and explicit unavailable publication PASS");
}
