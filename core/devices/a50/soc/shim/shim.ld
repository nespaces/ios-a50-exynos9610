/*
 * The shim, linked at 0 and run wherever the boot loader puts it (start.S applies .rela.dyn). Everything, .bss included,
 * is in the file (the Makefile's objcopy makes .bss's zeros contents), so that the payload appended behind the file is
 * behind __image_end, not under the .bss.
 */
ENTRY(_head)

SECTIONS
{
	. = 0;
	.text : {
		*(.text.head)
		*(.text .text.*)
	}
	.rodata : ALIGN(16) {
		*(.rodata .rodata.*)
	}
	.rela.dyn : ALIGN(8) {
		__rela_start = .;
		*(.rela .rela.*)
		__rela_end = .;
	}
	.data : ALIGN(16) {
		*(.data .data.*)
		*(.got .got.*)
	}
	.dynamic : { *(.dynamic) }
	.dynsym : { *(.dynsym) }
	.dynstr : { *(.dynstr) }
	.hash : { *(.hash) }
	.gnu.hash : { *(.gnu.hash) }
	.bss : ALIGN(4096) {
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4096);
		__image_end = .;
	}
	/DISCARD/ : {
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame_hdr)
		*(.interp)
	}
}
