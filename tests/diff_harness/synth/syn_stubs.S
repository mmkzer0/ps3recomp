/* Import stubs in the shape the firmware's import glue has, so that the
 * harness patches the slot of a stub that really is a stub:
 *
 *     li    r12,0
 *     oris  r12,r12,slot@ha        ; the pair names the slot's address
 *     lwz   r12,slot@l(r12)        ; the slot holds the address of a {u32 code, u32 toc}
 *     std   r2,0x28(r1)            ; descriptor; the loader (here: the harness) fills it in
 *     lwz   r0,0(r12)
 *     lwz   r2,4(r12)
 *     mtctr r0
 *     bctr
 *
 * One stub per import. The slot is an uninitialised data word. The 24-byte ELFv1
 * descriptor emitted for each stub is linker plumbing only (ld insists on one
 * for every called function); the 8-byte {code,toc} descriptors the PS3 ABI
 * calls through are produced by tools/diff_harness/synth_pack.py.
 */
        .macro  IMPORT name
        .section ".text"
        .align  2
        .globl  .\name
        .type   .\name,@function
.\name:
        li      12,0
        oris    12,12,.Lslot_\name@ha
        lwz     12,.Lslot_\name@l(12)
        std     2,0x28(1)
        lwz     0,0(12)
        lwz     2,4(12)
        mtctr   0
        bctr
        .size   .\name,.-.\name
        .section ".opd","aw"
        .align  3
        .globl  \name
\name:
        .quad   .\name,.TOC.@tocbase,0
        .section ".data"
        .align  2
.Lslot_\name:
        .long   0
        .endm

        IMPORT  synHostMemset
