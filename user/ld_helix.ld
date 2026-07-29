ENTRY(_start)
SECTIONS
{
    /* Interpreter base — distinct from helix apps at 0x40000000 */
    . = 0x50000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
