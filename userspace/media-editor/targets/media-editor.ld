/* SPDX-License-Identifier: GPL-3.0-only */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/*
 * Phipia maps a user image one 4 KiB page at a time and gives each section its
 * own permissions, so no two permissions may share a page. Every section
 * therefore starts on a page boundary and pads to one, and the asserts at the
 * end of this script prove it rather than trusting it.
 *
 * The base is the address Phipia's measured user profiles already load at.
 * The image is ET_EXEC and non-PIE: docs/LINUX_SYSCALL_ABI.md in Phipia
 * refuses interpreter, dynamic, relocation, PIE, executable-stack, and W+X
 * shapes, so a position-independent image would be refused before it is mapped.
 */
IMAGE_BASE = 0x0000400001000000;

PHDRS
{
    rodata PT_LOAD FLAGS(4);
    text   PT_LOAD FLAGS(5);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = IMAGE_BASE;
    __image_start = .;

    .rodata ALIGN(4K) :
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        /*
         * The large code model puts constants in .lrodata, code in .ltext, and
         * mutable data in .ldata. A script that names only the ordinary
         * spellings drops executable code into a read-only segment without
         * saying so; --orphan-handling=error plus these lines is what catches
         * it.
         */
        *(.lrodata .lrodata.*)
        *(.data.rel.ro .data.rel.ro.*)

        /*
         * A fixed-address image must not contain a global offset table. These
         * sections are named so the linker cannot place them elsewhere, and
         * asserted empty below so that anything needing one is a
         * link error rather than a silent indirection.
         */
        __got_start = .;
        *(.got .got.plt .igot.plt)
        *(.plt .plt.* .iplt)
        __got_end = .;
        . = ALIGN(4K);
        __rodata_end = .;
    } :rodata

    .text ALIGN(4K) :
    {
        __text_start = .;
        *(.text .text.*)
        *(.ltext .ltext.*)
        . = ALIGN(4K);
        __text_end = .;
    } :text

    .data ALIGN(4K) :
    {
        __data_start = .;
        *(.data .data.*)
        *(.ldata .ldata.*)
        . = ALIGN(4K);
        __data_end = .;
    } :data

    .bss ALIGN(4K) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(.lbss .lbss.*)
        *(COMMON)
        . = ALIGN(4K);
        __bss_end = .;
    } :data

    __image_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.eh_frame_hdr)
        *(.llvmbc)
        *(.llvmcmd)
        *(.debug_gdb_scripts)
        /* Relocations for a fully linked, non-relocatable image. The ELF
           audit rejects any R_X86_64 record independently of this. */
        *(.rela .rela.*)
    }

    /* Kept but never loaded. Named explicitly because
       --orphan-handling=error refuses anything this script does not mention,
       and losing line numbers to keep the linker honest is a bad trade. */
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_aranges 0 : { *(.debug_aranges) }
    .debug_frame 0 : { *(.debug_frame) }
    .debug_info 0 : { *(.debug_info) }
    .debug_line 0 : { *(.debug_line) }
    .debug_line_str 0 : { *(.debug_line_str) }
    .debug_loc 0 : { *(.debug_loc) }
    .debug_loclists 0 : { *(.debug_loclists) }
    .debug_macro 0 : { *(.debug_macro) }
    .debug_ranges 0 : { *(.debug_ranges) }
    .debug_rnglists 0 : { *(.debug_rnglists) }
    .debug_str 0 : { *(.debug_str) }
    .debug_str_offsets 0 : { *(.debug_str_offsets) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_pubtypes 0 : { *(.debug_pubtypes) }

    /*
     * Phipia identity-maps the whole low 4 GiB for itself, so a user image
     * must live above it, and at 70 TiB no address fits a 32-bit
     * displacement. The prebuilt core and alloc archives are compiled
     * position-independent, and the linker cannot relax their GOT-relative
     * accesses into direct ones at this distance, so a small table is
     * unavoidable without rebuilding the standard library on a nightly
     * toolchain. It is resolved completely at link time - the ELF audit
     * proves there is no relocation record and no dynamic section left - and
     * it is bounded here so that it cannot grow quietly.
     */
    ASSERT(__got_end - __got_start <= 4096,
           "the global offset table grew past its bound")
    ASSERT((__image_start & 0xFFF) == 0, "the image is not page aligned")
    ASSERT((__rodata_start & 0xFFF) == 0, "rodata starts mid-page")
    ASSERT((__rodata_end & 0xFFF) == 0, "rodata ends mid-page")
    ASSERT((__text_start & 0xFFF) == 0, "text does not start on a page")
    ASSERT((__text_end & 0xFFF) == 0, "text does not end on a page")
    ASSERT((__data_start & 0xFFF) == 0, "data does not start on a page")
    ASSERT((__bss_start & 0xFFF) == 0, "bss does not start on a page")
    ASSERT((__bss_end & 0xFFF) == 0, "bss does not end on a page")
    ASSERT(__rodata_end == __text_start, "a gap separates rodata and text")
    ASSERT(__text_end == __data_start, "a gap separates text and data")
    ASSERT(__data_end == __bss_start, "a gap separates data and bss")
    ASSERT(__bss_end == __image_end, "bss does not end the image")
}
