/* Synthetic module layout. The image base mirrors the firmware modules the
 * harness normally consumes, so the same loader path (image at mod.base, OPDs
 * read through guest memory) runs unchanged. */
ENTRY(synth_entry_code)
SECTIONS
{
  . = 0x02000000;
  .text : { *(.text) *(.text.*) }
  .rodata : { *(.rodata) *(.rodata.*) }
  .opd : ALIGN(8) { *(.opd) }
  .data : { *(.data) *(.data.*) *(.sdata) *(.toc) }
  /* fixed so the hand-written import stub can encode it as li/oris/lwz */
  . = 0x02020000;
  .synth_import_slots : { *(.synth_import_slots) }
  .bss : { *(.bss) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.llvm_addrsig) *(.eh_frame*) }
}
