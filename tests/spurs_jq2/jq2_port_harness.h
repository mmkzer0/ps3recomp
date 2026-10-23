/*
 * The C side of the JQ2 port tests: guest memory, the real PPU reservation
 * instructions and an SPU driven through its channel interface (spu_wrch /
 * spu_rdch, so a GETLLAR / PUTLLC here is the runtime's own MFC atomic path),
 * behind a small interface the C++ tests call without including the runtime's
 * C headers.
 */
#ifndef JQ2_PORT_HARNESS_H
#define JQ2_PORT_HARNESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zeroed, page-aligned guest memory of `bytes` bytes behind vm_base. */
void jq2h_guest_init(uint32_t bytes);
void jq2h_guest_free(void);
/* Guest words in host order; plain, for use while no other thread runs. */
uint32_t jq2h_get32(uint32_t ea);
void     jq2h_put32(uint32_t ea, uint32_t v);
uint8_t  jq2h_get8(uint32_t ea);
void     jq2h_put8(uint32_t ea, uint8_t v);

/* The PPU's lwarx/ldarx + stwcx./stdcx. (resv_*); returns the loaded value /
 * 1 when the store landed. */
typedef struct jq2h_ppu jq2h_ppu;
jq2h_ppu* jq2h_ppu_new(void);
void      jq2h_ppu_free(jq2h_ppu*);
uint32_t  jq2h_lwarx32(jq2h_ppu*, uint32_t ea);
uint64_t  jq2h_ldarx64(jq2h_ppu*, uint32_t ea);
int       jq2h_stcx32(jq2h_ppu*, uint32_t ea, uint32_t v);
int       jq2h_stcx64(jq2h_ppu*, uint32_t ea, uint64_t v);

/* One SPU through the channel path. */
typedef struct jq2h_spu jq2h_spu;
jq2h_spu* jq2h_spu_new(uint32_t id);
void      jq2h_spu_free(jq2h_spu*);
void      jq2h_getllar(jq2h_spu*, uint32_t ea);   /* into the staging line */
int       jq2h_putllc(jq2h_spu*, uint32_t ea);    /* 1 if the store landed */
uint32_t  jq2h_stage_get32(jq2h_spu*, uint32_t off);
void      jq2h_stage_put32(jq2h_spu*, uint32_t off, uint32_t v);
int       jq2h_lr_raised(jq2h_spu*);              /* SPU_EVENT_LR pending */

/* The PM's credit of word 0, as the dataflow of LS 0x3404-0x3444 computes it
 * (shli/rotmai, ai, selb 0x1FFFFFFF, andbi-masked selb), written as SPU
 * operations rather than as the port model's branches. */
uint32_t jq2h_pm_credit_word(uint32_t word0);

/* One PM completion: GETLLAR the port line, apply jq2h_pm_credit_word to word 0,
 * PUTLLC, retry on a lost reservation. Returns 1 iff the PM's release call is
 * due (the word read had bit 31 set and sext29 == -1; the PM's test at LS
 * 0x3454-0x3470). *retries is incremented per lost reservation. */
int jq2h_spu_credit(jq2h_spu*, uint32_t ea, uint64_t* retries);

#ifdef __cplusplus
}
#endif

#endif
