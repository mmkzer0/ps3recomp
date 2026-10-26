/* synlib in hand-written PowerPC64 assembly (syn_spec.h), ELFv1 conventions:
 * arguments in r3..r10, result in r3, LR saved at 16(r1), r2 = TOC, a 48-byte
 * linkage area, callee-saved r14..r31. Guest pointers are 32-bit values
 * zero-extended in registers, so addresses are used as they are.
 *
 * Deliberately written differently from what the compiler emits (record forms,
 * the CTR loop, an absolute-address jump table fetched through a TOC entry, a
 * 64-bit constant from the TOC, rotates through rlwinm/rotld, a divdu pair)
 * so that the lifter meets a second encoding of the same behaviour.
 *
 * Build: powerpc64-linux-gnu-gcc -m64 -mcpu=cell -Wa,-mregnames -c syn_asm.S
 */
#include "syn_spec.h"

#define HI16(x) (((x) >> 16) & 0xFFFF)
#define LO16(x) ((x) & 0xFFFF)

        .macro  FUNC name
        .section ".opd","aw"
        .align  3
        .globl  \name
\name:
        .quad   .\name,.TOC.@tocbase,0
        .text
        .p2align 4
        .globl  .\name
        .type   .\name,@function
.\name:
        .endm

        .macro  ENDFUNC name
        .size   .\name,.-.\name
        .size   \name,.-.\name
        .endm

        .macro  FAIL code
        lis     r3,HI16(\code)
        ori     r3,r3,LO16(\code)
        blr
        .endm

/* ------------------------------------------------------------------------- */
/* s32 synObjInit(u32 obj, u32 name, u32 flags, u32 capacity)                  */
        FUNC    synObjInit
        cmpdi   r3,0
        beq     .Linit_null
        andi.   r0,r3,15
        bne     .Linit_align
        rlwinm. r0,r5,0,0,25            /* flags & 0xFFFFFFC0 */
        bne     .Linit_inval
        addi    r0,r6,-1                /* capacity - 1 > 0xFFF (u32): catches 0 and > 0x1000 */
        cmplwi  r0,SYN_CAP_MAX-1
        bgt     .Linit_range

        mflr    r0
        std     r0,16(r1)
        stdu    r1,-112(r1)

        lis     r9,HI16(SYN_MAGIC)
        ori     r9,r9,LO16(SYN_MAGIC)
        stw     r9,SYN_O_MAGIC(r3)
        li      r9,SYN_VERSION
        sth     r9,SYN_O_VERSION(r3)
        sth     r5,SYN_O_FLAGS(r3)
        stw     r6,SYN_O_CAPACITY(r3)
        li      r9,0
        stw     r9,SYN_O_COUNT(r3)
        xoris   r9,r5,0xFFFF            /* flags ^ 0xFFFF0000 */
        clrldi  r9,r9,32
        sldi    r10,r6,32
        or      r10,r10,r9
        std     r10,SYN_O_STAMP(r3)

        li      r7,0                    /* i */
        cmpdi   r4,0
        beq     .Linit_pad
.Linit_copy:
        cmpwi   r7,SYN_NAME_LEN-1
        bge     .Linit_pad
        lbzx    r8,r4,r7
        cmpwi   r8,0
        beq     .Linit_pad
        add     r9,r3,r7
        stb     r8,SYN_O_NAME(r9)
        addi    r7,r7,1
        b       .Linit_copy
.Linit_pad:
        cmpwi   r7,SYN_NAME_LEN
        bge     .Linit_tail
        add     r9,r3,r7
        li      r8,0
        stb     r8,SYN_O_NAME(r9)
        addi    r7,r7,1
        b       .Linit_pad
.Linit_tail:
        addi    r3,r3,SYN_O_PACK_LO     /* synHostMemset(obj + 0x30, 0, 0x20) */
        li      r4,0
        li      r5,SYN_O_SIZE-SYN_O_PACK_LO
        bl      .synHostMemset
        nop
        li      r3,0
        addi    r1,r1,112
        ld      r0,16(r1)
        mtlr    r0
        blr
.Linit_null:
        FAIL    SYN_ENULL
.Linit_align:
        FAIL    SYN_EALIGN
.Linit_inval:
        FAIL    SYN_EINVAL
.Linit_range:
        FAIL    SYN_ERANGE
        ENDFUNC synObjInit

/* ------------------------------------------------------------------------- */
/* s32 synObjCommand(u32 obj, u32 cmd, u32 arg)                                */
        FUNC    synObjCommand
        cmpdi   r3,0
        beq     .Lcmd_null
        andi.   r0,r3,15
        bne     .Lcmd_align
        lwz     r9,SYN_O_MAGIC(r3)
        lis     r10,HI16(SYN_MAGIC)
        ori     r10,r10,LO16(SYN_MAGIC)
        cmpw    r9,r10
        bne     .Lcmd_state
        cmplwi  r4,7
        bgt     .Lcmd_inval
        ld      r9,.Ltoc_cmdtab@toc(r2) /* table address from a TOC entry */
        rlwinm  r10,r4,2,0,29           /* cmd * 4, upper half cleared */
        lwzx    r10,r9,r10              /* absolute case address */
        mtctr   r10
        bctr

.Lcmd_query:
        lwz     r3,SYN_O_COUNT(r3)
        extsw   r3,r3
        blr
.Lcmd_add:
        lwz     r9,SYN_O_COUNT(r3)
        add     r10,r9,r5               /* exact 64-bit sum */
        srdi.   r0,r10,32               /* carried out of 32 bits: wrapped */
        bne     .Lcmd_range
        lwz     r8,SYN_O_CAPACITY(r3)
        cmplw   r10,r8
        bgt     .Lcmd_range
        stw     r10,SYN_O_COUNT(r3)
        ld      r9,SYN_O_STAMP(r3)
        add     r9,r9,r5
        std     r9,SYN_O_STAMP(r3)
        extsw   r3,r10
        blr
.Lcmd_sub:
        lwz     r9,SYN_O_COUNT(r3)
        cmplw   r5,r9
        bgt     .Lcmd_range
        subf    r9,r5,r9
        stw     r9,SYN_O_COUNT(r3)
        extsw   r3,r9
        blr
.Lcmd_clear:
        li      r0,0
        stw     r0,SYN_O_COUNT(r3)
        std     r0,SYN_O_STAMP(r3)
        li      r3,0
        blr
.Lcmd_setflags:
        rlwinm. r0,r5,0,0,25
        bne     .Lcmd_inval
        lhz     r9,SYN_O_FLAGS(r3)
        sth     r5,SYN_O_FLAGS(r3)
        mr      r3,r9
        blr
.Lcmd_stamphi:
        ld      r9,SYN_O_STAMP(r3)
        srdi    r3,r9,32
        extsw   r3,r3
        blr
.Lcmd_pack:
        clrlwi  r9,r5,16
        sth     r9,SYN_O_PACK_LO(r3)
        srwi    r9,r5,16
        sth     r9,SYN_O_PACK_HI(r3)
        lis     r10,HI16(SYN_PACK_K)
        ori     r10,r10,LO16(SYN_PACK_K)
        mullw   r9,r5,r10
        stw     r9,SYN_O_PACK_MUL(r3)
        clrldi  r10,r10,32              /* zero-extend the constant for the full product */
        mulld   r9,r5,r10
        std     r9,SYN_O_PACK_WIDE(r3)
        li      r3,0
        blr
.Lcmd_rotate:
        ld      r9,SYN_O_STAMP(r3)
        clrlwi  r10,r5,26               /* arg & 63 */
        rotld   r9,r9,r10
        std     r9,SYN_O_STAMP(r3)
        li      r3,0
        blr
.Lcmd_null:
        FAIL    SYN_ENULL
.Lcmd_align:
        FAIL    SYN_EALIGN
.Lcmd_state:
        FAIL    SYN_ESTATE
.Lcmd_range:
        FAIL    SYN_ERANGE
.Lcmd_inval:
        FAIL    SYN_EINVAL
        ENDFUNC synObjCommand

/* ------------------------------------------------------------------------- */
/* s32 synObjPush(u32 obj, u32 n)                                              */
        FUNC    synObjPush
        mflr    r0
        std     r0,16(r1)
        stdu    r1,-128(r1)
        std     r30,112(r1)
        std     r31,120(r1)
        mr      r31,r3
        mr      r30,r4
        li      r4,SYN_CMD_ADD
        mr      r5,r30
        bl      .synObjCommand
        nop
        cmpwi   r3,0
        blt     .Lpush_out
        stw     r30,SYN_O_LAST_PUSH(r31)
        lwz     r9,SYN_O_PUSH_SEQ(r31)
        addi    r9,r9,1
        stw     r9,SYN_O_PUSH_SEQ(r31)
.Lpush_out:
        ld      r30,112(r1)
        ld      r31,120(r1)
        addi    r1,r1,128
        ld      r0,16(r1)
        mtlr    r0
        blr
        ENDFUNC synObjPush

/* ------------------------------------------------------------------------- */
/* s32 synBufChecksum(u32 buf, u32 words, u32 out)                             */
        FUNC    synBufChecksum
        cmpdi   r3,0
        beq     .Lck_null
        cmpdi   r5,0
        beq     .Lck_null
        andi.   r0,r3,3
        bne     .Lck_align
        andi.   r0,r5,7
        bne     .Lck_align
        cmplwi  r4,SYN_WORDS_MAX
        bgt     .Lck_range
        li      r6,0                    /* sum */
        lis     r7,0x1234
        ori     r7,r7,0x5678            /* x */
        cmplwi  r4,0
        beq     .Lck_done
        mtctr   r4
.Lck_loop:
        lwz     r8,0(r3)
        addi    r3,r3,4
        add     r6,r6,r8
        rotlwi  r7,r7,5
        xor     r7,r7,r8
        bdnz    .Lck_loop
.Lck_done:
        std     r6,0(r5)
        stw     r7,8(r5)
        stw     r4,12(r5)
        li      r3,0
        blr
.Lck_null:
        FAIL    SYN_ENULL
.Lck_align:
        FAIL    SYN_EALIGN
.Lck_range:
        FAIL    SYN_ERANGE
        ENDFUNC synBufChecksum

/* ------------------------------------------------------------------------- */
/* u64 synBits(u64 a, u64 b, u32 c)        a = r3, b = r4, c = r5              */
        FUNC    synBits
        mulld   r6,r3,r4
        add     r6,r6,r5                /* x = a*b + c */
        srdi    r7,r6,29
        xor     r6,r6,r7
        ld      r7,.Ltoc_mixk@toc(r2)   /* 64-bit constant from the TOC */
        mulld   r6,r6,r7
        ori     r8,r4,1
        divdu   r9,r6,r8                /* q = x / (b|1) */
        ori     r10,r3,1
        divdu   r11,r6,r10
        mulld   r11,r11,r10
        subf    r11,r11,r6              /* r = x % (a|1) */
        sldi    r11,r11,3
        xor     r9,r9,r11
        sradi   r12,r6,7
        xor     r9,r9,r12               /* h */

        cntlzd  r10,r3                  /* clz64, 64 for zero */
        sldi    r10,r10,8
        xor     r9,r9,r10
        std     r4,-8(r1)               /* bswap64(b) from two word byte-reversed loads */
        addi    r11,r1,-8
        li      r12,4
        lwbrx   r10,0,r11               /* bswap32 of the high word -> low half of the result */
        lwbrx   r8,r12,r11
        sldi    r8,r8,32
        or      r10,r10,r8
        add     r9,r9,r10
        stw     r5,-16(r1)              /* bswap32(c) */
        addi    r11,r1,-16
        lwbrx   r10,0,r11
        xor     r9,r9,r10
        extsb   r10,r3
        xor     r9,r9,r10
        extsh   r10,r4
        add     r9,r9,r10
        extsw   r10,r5
        xor     r9,r9,r10
        mulhdu  r10,r3,r4
        add     r9,r9,r10
        mulhd   r10,r3,r4
        xor     r9,r9,r10
        clrldi  r11,r4,58               /* b & 63 */
        srad    r10,r3,r11
        xor     r9,r9,r10
        clrlwi  r11,r5,27               /* c & 31 */
        sraw    r10,r3,r11              /* (s32)a >> (c&31), sign-extended */
        clrldi  r10,r10,32
        add     r9,r9,r10
        clrlwi  r11,r4,27               /* b & 31 */
        rlwnm   r10,r3,r11,0,31         /* rotl32((u32)a, b&31) */
        xor     r9,r9,r10
        rlwinm  r10,r4,7,8,23           /* ((u32)b << 7) & 0x00FFFF00 */
        mr      r11,r3
        rlwimi  r11,r10,0,8,23          /* ((u32)a & ~m) | that */
        clrldi  r11,r11,32
        add     r9,r9,r11
        cmpld   cr0,r3,r4
        cmpd    cr1,r3,r4
        mfcr    r11
        rlwinm  r10,r11,1,31,31         /* cr0.LT -> bit 0 */
        rlwinm  r12,r11,6,30,30         /* cr1.LT -> bit 1 */
        or      r10,r10,r12
        rlwinm  r12,r11,5,29,29         /* cr0.EQ -> bit 2 */
        or      r10,r10,r12
        xor     r9,r9,r10
        rlwinm  r11,r4,0,17,31          /* b & 0x7FFF */
        ori     r11,r11,1
        divd    r10,r3,r11
        add     r9,r9,r10
        rlwinm  r11,r3,0,25,31          /* a & 0x7F */
        ori     r11,r11,1
        extsw   r10,r5
        divw    r10,r10,r11             /* the upper word of a divw result is undefined */
        clrldi  r10,r10,32
        xor     r9,r9,r10
        rlwinm  r11,r5,0,16,31          /* c & 0xFFFF */
        ori     r11,r11,1
        divwu   r10,r3,r11
        clrldi  r10,r10,32
        add     r9,r9,r10
        mullw   r10,r3,r4
        clrldi  r10,r10,32
        xor     r9,r9,r10
        mulhw   r10,r3,r4
        clrldi  r10,r10,32
        add     r9,r9,r10
        mulhwu  r10,r3,r4
        clrldi  r10,r10,32
        xor     r9,r9,r10
        mr      r3,r9
        blr
        ENDFUNC synBits

/* ------------------------------------------------------------------------- */
/* s32 synFpu(u32 in, u32 out)             in = r3, out = r4                   */
        FUNC    synFpu
        cmpdi   r3,0
        beq     .Lfpu_null
        cmpdi   r4,0
        beq     .Lfpu_null
        or      r0,r3,r4
        andi.   r0,r0,7
        bne     .Lfpu_align
        lfd     f1,0(r3)                /* a */
        lfd     f2,8(r3)                /* b */
        lfs     f3,16(r3)               /* c */
        lfs     f4,20(r3)               /* d */
        fsub    f0,f2,f2                /* +0.0 for finite b */
        fcmpu   cr0,f2,f0
        beq     .Lfpu_range             /* b == 0 */
        ld      r9,.Ltoc_two31@toc(r2)
        lfd     f5,0(r9)                /* 2147483648.0 */
        fabs    f6,f1
        fcmpu   cr0,f6,f5
        bge     .Lfpu_range             /* fabs(a) >= 2^31 */

        fadd    f7,f1,f2
        stfd    f7,SYN_F_SUM(r4)
        fsub    f7,f1,f2
        stfd    f7,SYN_F_DIFF(r4)
        fmul    f8,f1,f2
        stfd    f8,SYN_F_PROD(r4)
        fdiv    f7,f1,f2
        stfd    f7,SYN_F_QUOT(r4)
        fmadd   f7,f1,f2,f3             /* a*b + (double)c, one rounding */
        stfd    f7,SYN_F_FMA(r4)
        frsp    f9,f8
        stfs    f9,SYN_F_PRODF(r4)
        fctiwz  f10,f1                  /* (s32)a, truncating; the integer is in the low word */
        stfd    f10,-8(r1)
        lwz     r9,-4(r1)
        stw     r9,SYN_F_TRUNC(r4)
        fcmpu   cr0,f1,f2
        mfcr    r9
        rlwinm  r10,r9,1,31,31          /* LT -> bit 0 */
        rlwinm  r11,r9,4,30,30          /* EQ -> bit 1 */
        rlwinm  r12,r9,4,29,29          /* GT -> bit 2 */
        or      r10,r10,r11
        or      r10,r10,r12
        stw     r10,SYN_F_CMP(r4)
        fadds   f11,f3,f4
        stfs    f11,SYN_F_ADDS(r4)
        fmuls   f12,f3,f4
        stfs    f12,SYN_F_MULS(r4)
        stfd    f6,SYN_F_ABS(r4)        /* fabs(a), already in f6 */
        fneg    f13,f2
        stfd    f13,SYN_F_NEGB(r4)
        li      r3,0
        blr
.Lfpu_null:
        FAIL    SYN_ENULL
.Lfpu_align:
        FAIL    SYN_EALIGN
.Lfpu_range:
        FAIL    SYN_ERANGE
        ENDFUNC synFpu

/* ------------------------------------------------------------------------- */
        .section ".rodata"
        .align  3
.Lcmdtab:
        .long   .Lcmd_query, .Lcmd_add, .Lcmd_sub, .Lcmd_clear
        .long   .Lcmd_setflags, .Lcmd_stamphi, .Lcmd_pack, .Lcmd_rotate

.Lfc_two31:
        .double 2147483648.0

        .section ".toc","aw"
.Ltoc_cmdtab:
        .quad   .Lcmdtab
.Ltoc_mixk:
        .quad   0xBF58476D1CE4E5B9
.Ltoc_two31:
        .quad   .Lfc_two31
