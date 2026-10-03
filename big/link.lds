/* Derived from RT-Thread's SDK nncase/examples/cmake/link.lds.
 * Copyright (c) 2006-2020, RT-Thread Development Team
 * SPDX-License-Identifier: Apache-2.0
 * Keep the SDK startup symbols/address; do not match .rela.got as GOT data.
 */
OUTPUT_ARCH("riscv")
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
    stack PT_GNU_STACK FLAGS(6);
}
SECTIONS
{
    . = 0x200000000;
    .text : {
        __text_start__ = .;
        *(.start)
        *(.text .text.*)
        KEEP(*(.init))
        KEEP(*(.fini))
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*)) *(.ctors)
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*)) *(.dtors)
        *(.rodata .rodata.* .srodata .srodata.*)
        KEEP(*(.eh_frame*))
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        _etext = .; __text_end__ = .;
    } :text
    . = ALIGN(0x1000);
    /* RT-Smart treats non-PROGBITS allocated sections as writable mappings.
     * Keep the linker's R_RISCV_NONE metadata off the final text page. */
    .rela.dyn : { *(.rela .rela.*) } :data
    .plt : { *(.plt .plt.* .iplt) } :data
    .got : {
        __got_start__ = .;
        *(.got .got.* .igot .igot.*)
        __got_end__ = .;
    } :data
    .data : {
        __data_start__ = .;
        *(vtable) *(.data .data.* .sdata .sdata.*)
        . = ALIGN(8);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array .preinit_array.*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        . = ALIGN(8);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
        . = ALIGN(8);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
        . = ALIGN(8);
        __data_end__ = .;
    } :data
    .tdata : { *(.tdata .tdata.*) } :data :tls
    .tbss : { *(.tbss .tbss.*) } :data :tls
    .bss : {
        __bss_start__ = .;
        *(.bss .bss.* .sbss .sbss.*) *(COMMON) *(.stack)
        . = ALIGN(8);
        __bss_end__ = .;
    } :data
    _end = .;
}
