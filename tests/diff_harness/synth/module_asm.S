/* Synthetic hand-written PPC assembly: one exported function and the
 * SCE-shaped import stub the lifter must recognise as a bctrl dispatch.
 *
 * The stub is exactly the three-instruction shape tools/diff_harness/gen_tables.py
 * decodes: li r12,0 ; oris r12,r12,hi ; lwz r12,lo(r12) ; mtctr r12 ; bctrl.
 * Its slot lives at the fixed address synth.ld pins .synth_import_slots to. */
    .section .toc,"aw"
    .align 3
    .globl _synth_toc
_synth_toc:
    .quad 0

    .text
    .globl synth_entry_code
synth_entry_code:
    li 3, 0
    blr

    /* 0x91000006: a + b, by hand */
    .globl synth_nid_91000006_code
synth_nid_91000006_code:
    add 3, 3, 4
    blr

    /* 0x91000100: imported helper. The shape mirrors the firmware's SCE stub:
     * load the slot (descriptor), read {code,toc}, jump to the code with the
     * module TOC in r2. decode_slot() reads the first three instructions. */
    .globl synth_import_91000100
synth_import_91000100:
    li 12, 0
    oris 12, 12, 0x0202
    lwz 12, 0(12)
    lwz 0, 0(12)
    lwz 2, 4(12)
    mtctr 0
    bctr

    .section .opd,"aw"
    .align 3
    .globl synth_nid_91000006
synth_nid_91000006:
    .quad synth_nid_91000006_code
    .quad _synth_toc

    .section .synth_import_slots,"aw"
    .align 3
    .globl synth_import_slot_91000100
synth_import_slot_91000100:
    .quad 0
