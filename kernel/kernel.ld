OUTPUT_FORMAT("pei-i386")
ENTRY(_entry)
SECTIONS {
  . = 0xC0000000 +0x100000;
  .text : AT(ADDR(.text)-0xC0000000) {
    *(.text)
    *(.text.*)
  }
  . = ALIGN(4);
  .rodata : AT(ADDR(.rodata)-0xC0000000) {
    *(.rodata)
    *(.rodata.*)
    . = ALIGN(4);
    *(.eh_frame)
  }
  .rdata : AT(ADDR(.rdata)-0xC0000000) {
    *(.rdata)
    *(.rdata.*)
    . = ALIGN(4);
    *(.eh_frame)
  }
  .data : AT(ADDR(.data)-0xC0000000) {
    *(.data)
    *(.data.*)
  }
  . = ALIGN(4);
  _edata = .;
  .bss : AT(ADDR(.bss)-0xC0000000) {
    *(.bss)
    *(.bss.*)
    *(COMMON)
  }
  . = ALIGN(4);
  _end = .;
}
