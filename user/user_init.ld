/* User ELF link script — init at 0x40000000 */
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    .text : {
        *(.text*)
    }
    .rodata : {
        *(.rodata*)
    }
    .data : {
        *(.data*)
    }
    .bss : {
        *(.bss*)
        *(COMMON)
    }
}
