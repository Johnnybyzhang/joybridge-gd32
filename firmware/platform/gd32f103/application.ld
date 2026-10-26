ENTRY(Reset_Handler)
MEMORY {
    FLASH (rx) : ORIGIN = 0x08003400, LENGTH = 0x1B800
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x4FE0
}
/* Loader POP needs 8 readable bytes above initial MSP. The top 32 bytes,
 * including boot marker 0x20004FF0, are excluded from every RAM section. */
_stack_top = ORIGIN(RAM) + LENGTH(RAM);
_stack_limit = _stack_top - 2048;
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
    .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH
    _sidata = LOADADDR(.data);
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> FLASH
    .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
    .noinit (NOLOAD) : { *(.noinit*) . = ALIGN(4); _end = .; } > RAM
    /DISCARD/ : { *(.comment*) *(.note*) }
}
ASSERT(ADDR(.isr_vector) == 0x08003400, "Wrong application vector base")
ASSERT(SIZEOF(.isr_vector) == 304, "Expected 16 core and 60 external vectors")
ASSERT(_end <= _stack_limit, "Static RAM overlaps reserved 2 KiB stack")
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0x0801EC00, "Image overlaps pairing journal")
