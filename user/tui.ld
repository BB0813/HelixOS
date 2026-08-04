/* HelixOS TUI shell — load at classic Linux ET_EXEC address (same as helixbox) */
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
