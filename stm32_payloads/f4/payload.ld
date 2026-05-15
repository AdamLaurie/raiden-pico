ENTRY(_start)

/* Specify the memory areas */
MEMORY
{
    RAM (rwx)   : ORIGIN = 0x20004000, LENGTH = 20K
}

SECTIONS
{
    .text :
    {
        /* ARM-specific vectors and entrypoint */
        KEEP(*(.isr_vector))

        . = ALIGN(8);

        /* Code */
        *(.text)
        *(.text*)

        . = ALIGN(8);

        /* Data -- this is RAM so there is no relocation needed */
        *(.data)
        *(.data*)
    } >RAM

    .bss :
    {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss)
        *(.bss*)
        . = ALIGN(8);
        _bss_end = .;
    } >RAM = 0xa5
}
