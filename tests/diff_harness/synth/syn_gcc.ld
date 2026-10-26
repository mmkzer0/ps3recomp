/* Linker script for the synthetic PPU modules: one flat image at SYN_BASE, every
 * address below 4 GiB, and nothing that needs a loader (no PLT, no dynamic
 * section, no relocations left). The PS3 module image is the same thing: code,
 * read-only data, descriptors, data and the TOC laid out from a fixed base.
 *
 *   powerpc64-linux-gnu-ld -T synth.ld --defsym SYN_BASE=0x02000000 ...
 */
OUTPUT_FORMAT("elf64-powerpc")
OUTPUT_ARCH(powerpc:common64)
ENTRY(synObjInit)

PHDRS { image PT_LOAD FLAGS(7); }

SECTIONS
{
  . = SYN_BASE;
  .text   : { *(.text .text.*) *(.sfpr) *(.glink) } :image
  .rodata : ALIGN(16) { *(.rodata .rodata.* .rodata1) }
  .opd    : ALIGN(8)  { *(.opd) }
  .data   : ALIGN(16) { *(.data .data.* .sdata .sdata.*) }
  /* ld places the TOC base (".TOC.") 0x8000 into .got, as the PS3 toolchain does */
  .got    : ALIGN(8)  { __got_start = .; *(.got) *(.toc) *(.tocbss) }
  .bss    : ALIGN(16) { *(.bss .bss.* COMMON) *(.sbss .sbss.*) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) *(.PPC.EMB*) *(.dynamic) *(.dynsym) *(.dynstr) *(.rela*) *(.interp) *(.plt) *(.iplt) *(.branch_lt) }
}
