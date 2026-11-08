/*
 * The boot shim's entry: what Google's boot loader (ABL) jumps to after `fastboot boot`, as it would to a Linux kernel.
 *
 * ABL starts an arm64 Linux "Image" (Documentation/arch/arm64/booting.rst: the 64-byte header below, x0 = the device
 * tree's physical address, the MMU off, at EL2 on a Pixel whose Android runs pKVM). The shim:
 *
 *   1. relocates itself (it is linked at 0 and runs wherever ABL put it: R_AARCH64_RELATIVE only);
 *   2. at EL2: sets EL2 up so that EL1 is a plain AArch64 kernel's (nothing trapped, the physical timer and the GIC's
 *      system registers open to it, pointer authentication not trapped), installs el2_vectors and drops to EL1;
 *   3. at EL1: shim_main(fdt) in shim.c, which starts xnuloader as a UEFI application.
 *
 * EL2 stays the shim's: a PSCI call made with HVC (platform_frankel.psci_hvc in xnuloader/loader.c) arrives at
 * el2_vectors and is passed on to the firmware (EL3) with SMC. CPU_ON is the one changed on the way: the firmware
 * starts a core at EL2, so the shim asks it to start the core at secondary_el2, which sets that core's EL2 up the same
 * way and enters the caller's entry point at EL1, with the caller's context in x0, MMU off, as PSCI says. (CPU_SUSPEND to a
 * power-down state would need the same; neither xnuloader nor HakiOS's XNU makes one: CPU_ON, CPU_OFF, AFFINITY_INFO,
 * SYSTEM_RESET and SYSTEM_OFF are all they call.)
 *
 * Under QEMU without EL2 (no virtualization=on) the shim is entered at EL1 and step 2 is skipped: QEMU answers HVC.
 *
 * Nothing here is from another project's code: the header's layout is Linux's documented boot protocol, the registers
 * are Arm's (DDI 0487, the Arm ARM; DEN 0022, PSCI).
 */

#define PSCI_CPU_ON_32  0x84000003
#define PSCI_CPU_ON_64  0xC4000003

	.section .text.head, "ax"
	.globl	_head
_head:
	b	entry                   /* code0 */
	.long	0                       /* code1 */
	.quad	0                       /* text_offset: loaded at a 2 MB-aligned base + 0 */
	.quad	0                       /* image_size: patched by the image builder (mkxnuimg.py) to the shim and payload's size */
	.quad	0xa                     /* flags: little endian, 4 KB pages, anywhere in RAM */
	.quad	0, 0, 0                 /* res2-4 */
	.ascii	"ARM\x64"               /* magic */
	.long	0                       /* res5 */

	.text
entry:
	mov	x20, x0                 /* the device tree */
	adr	x21, _head              /* where ABL put us */

	/* relocate: every R_AARCH64_RELATIVE in .rela.dyn, *(base + offset) = base + addend */
	adrp	x9, __rela_start
	add	x9, x9, :lo12:__rela_start
	adrp	x10, __rela_end
	add	x10, x10, :lo12:__rela_end
1:	cmp	x9, x10
	b.hs	2f
	ldp	x11, x12, [x9], #16     /* r_offset, r_info */
	ldr	x13, [x9], #8           /* r_addend */
	cmp	w12, #1027              /* R_AARCH64_RELATIVE */
	b.ne	1b
	add	x13, x13, x21
	str	x13, [x21, x11]
	b	1b
2:
	/* .bss is part of the file (shim.ld), already zero: the payload lies behind it, not over it */

	mrs	x9, CurrentEL
	lsr	x9, x9, #2
	adrp	x10, shim_entry_el
	str	x9, [x10, :lo12:shim_entry_el]
	cmp	x9, #2
	b.ne	at_el1

	bl	el2_setup
	adrp	x9, boot_stack_top
	add	x9, x9, :lo12:boot_stack_top
	msr	sp_el1, x9
	adr	x9, at_el1_from_el2
	msr	elr_el2, x9
	mov	x9, #0x3c5              /* EL1h, D A I F masked */
	msr	spsr_el2, x9
	isb
	eret

at_el1:
	adrp	x9, boot_stack_top
	add	x9, x9, :lo12:boot_stack_top
	mov	sp, x9
at_el1_from_el2:
	/* FP and SIMD on at EL1 (CPACR_EL1.FPEN = 0b11), as UEFI leaves them for an application */
	mov	x9, #(3 << 20)
	msr	cpacr_el1, x9
	adrp	x9, el1_vectors
	add	x9, x9, :lo12:el1_vectors
	msr	vbar_el1, x9
	isb
	mov	x0, x20
	bl	shim_main
3:	wfe
	b	3b

/*
 * EL1's vectors while the shim runs (the loader and XNU install their own): any exception is reported,
 * shim_el1_fault(ESR, ELR, FAR, which vector), and the phone restarted.
 */
	.balign	2048
el1_vectors:
	.irp	n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	.balign	128
	mov	x3, #\n
	b	el1_fault
	.endr
el1_fault:
	mrs	x0, esr_el1
	mrs	x1, elr_el1
	mrs	x2, far_el1
	adrp	x9, boot_stack_top
	add	x9, x9, :lo12:boot_stack_top
	mov	sp, x9
	bl	shim_el1_fault
4:	wfe
	b	4b

/*
 * EL2 as a plain AArch64 kernel's EL1 wants it (Arm ARM D1 and the registers' own pages). x9-x11 only; x0-x8 kept.
 */
el2_setup:
	/* HCR_EL2: RW (EL1 is AArch64); API, APK where the core has pointer authentication (iOS's arm64e code uses it) */
	mov	x9, #(1 << 31)
	mrs	x10, ID_AA64ISAR1_EL1
	tst	x10, #0xff0             /* APA, API */
	b.ne	1f
	mrs	x10, S3_0_C0_C6_2       /* ID_AA64ISAR2_EL1: APA3 [15:12] */
	tst	x10, #0xf000
	b.eq	2f
1:	orr	x9, x9, #(1 << 40)      /* APK */
	orr	x9, x9, #(1 << 41)      /* API */
2:	msr	hcr_el2, x9
	/* the physical counter and timer open to EL1, no offset for the virtual one, EL2's own timer off */
	mov	x9, #3
	msr	cnthctl_el2, x9
	msr	cntvoff_el2, xzr
	msr	cnthp_ctl_el2, xzr
	/* the GIC's system registers open to EL1 (ICC_SRE_EL2: SRE, Enable), where the core has them */
	mrs	x9, ID_AA64PFR0_EL1
	ubfx	x9, x9, #24, #4
	cbz	x9, 3f
	mrs	x9, S3_4_C12_C9_5       /* ICC_SRE_EL2 */
	orr	x9, x9, #0x1           /* SRE */
	orr	x9, x9, #0x8           /* Enable: EL1 may set its own SRE */
	msr	S3_4_C12_C9_5, x9
	isb
3:	/* nothing trapped: FP/SIMD/SVE (CPTR_EL2: its RES1 bits 13:12, 9, 7:0 only), CP15 (HSTR_EL2), debug and PMU (MDCR_EL2.HPMN = all) */
	mov	x9, #0x32ff
	msr	cptr_el2, x9
	msr	hstr_el2, xzr
	mrs	x9, pmcr_el0
	ubfx	x9, x9, #11, #5
	msr	mdcr_el2, x9
	/* fine-grained traps (FEAT_FGT: ID_AA64MMFR0_EL1.FGT [59:56]) and HCRX_EL2 (FEAT_HCX: ID_AA64MMFR1_EL1 [43:40]) cleared */
	mrs	x9, ID_AA64MMFR0_EL1
	ubfx	x9, x9, #56, #4
	cbz	x9, 4f
	msr	S3_4_C1_C1_4, xzr       /* HFGRTR_EL2 */
	msr	S3_4_C1_C1_5, xzr       /* HFGWTR_EL2 */
	msr	S3_4_C1_C1_6, xzr       /* HFGITR_EL2 */
	msr	S3_4_C3_C1_4, xzr       /* HDFGRTR_EL2 */
	msr	S3_4_C3_C1_5, xzr       /* HDFGWTR_EL2 */
4:	mrs	x9, ID_AA64MMFR1_EL1
	ubfx	x9, x9, #40, #4
	cbz	x9, 5f
	msr	S3_4_C1_C2_2, xzr       /* HCRX_EL2 */
5:	/* no stage 2; EL1 sees the core's own MIDR and MPIDR */
	msr	vttbr_el2, xzr
	mrs	x9, midr_el1
	msr	vpidr_el2, x9
	mrs	x9, mpidr_el1
	msr	vmpidr_el2, x9
	/* EL1 starts with its MMU and caches off (SCTLR_EL1: the RES1 bits only) */
	mov	x9, #0x0800
	movk	x9, #0x30d0, lsl #16
	msr	sctlr_el1, x9
	adrp	x9, el2_vectors
	add	x9, x9, :lo12:el2_vectors
	msr	vbar_el2, x9
	isb
	ret

/*
 * A core the firmware started for a CPU_ON that came through here: x0 = its slot (cpu_slots), which holds the caller's
 * entry point and context. EL2 set up as on the boot core, then the caller's entry at EL1 with x0 = its context.
 */
	.globl	secondary_el2
secondary_el2:
	mov	x19, x0
	bl	el2_setup
	ldp	x9, x0, [x19]
	msr	elr_el2, x9
	mov	x9, #0x3c5
	msr	spsr_el2, x9
	isb
	eret

/*
 * EL2's vectors. From EL1 (AArch64, offset 0x400): an HVC is a PSCI or other SMCCC call, passed on to EL3 with SMC;
 * x0-x3 in and out as SMCCC says, x4-x17 the caller's to lose (the loader's and the kernel's psci_call say so). Anything
 * else taken to EL2 means something is trapped that should not be: the core stops there, with ESR_EL2 in x0 and
 * ELR_EL2 in x1 for a debugger (shim_el2_fault).
 */
	.balign	2048
el2_vectors:
	.rept	8                       /* current EL (EL2 itself), SP0 and SPx */
	b	el2_fault
	.balign	128
	.endr
	b	el2_sync_lower          /* lower EL, AArch64: synchronous */
	.balign	128
	.rept	7
	b	el2_fault
	.balign	128
	.endr

el2_sync_lower:
	mrs	x9, esr_el2
	lsr	x10, x9, #26
	cmp	x10, #0x16              /* HVC from AArch64 */
	b.ne	el2_fault
	mov	w10, #(PSCI_CPU_ON_64 & 0xffff)
	movk	w10, #(PSCI_CPU_ON_64 >> 16), lsl #16
	cmp	w0, w10
	b.eq	1f
	mov	w10, #(PSCI_CPU_ON_32 & 0xffff)
	movk	w10, #(PSCI_CPU_ON_32 >> 16), lsl #16
	cmp	w0, w10
	b.eq	1f
	smc	#0
	eret
1:	/* CPU_ON(x1 = target MPIDR, x2 = entry, x3 = context): the slot of Aff1.Aff0 (64 of them), seen by a core with caches off */
	ubfx	x10, x1, #8, #4
	and	x11, x1, #3
	add	x10, x11, x10, lsl #2
	adrp	x11, cpu_slots
	add	x11, x11, :lo12:cpu_slots
	add	x11, x11, x10, lsl #4
	stp	x2, x3, [x11]
	dc	civac, x11
	dsb	sy
	adrp	x2, secondary_el2
	add	x2, x2, :lo12:secondary_el2
	mov	x3, x11
	smc	#0
	eret

el2_fault:
	mrs	x0, esr_el2
	mrs	x1, elr_el2
	mrs	x2, far_el2
	adrp	x9, shim_el2_fault
	add	x9, x9, :lo12:shim_el2_fault
	stp	x0, x1, [x9]
	str	x2, [x9, #16]
	dsb	sy
1:	wfe
	b	1b

	.bss
	.balign	16
	.globl	shim_entry_el
shim_entry_el:
	.quad	0
	.globl	shim_el2_fault
shim_el2_fault:
	.quad	0, 0, 0, 0
	.balign	64
cpu_slots:
	.space	64 * 16
	.balign	16
boot_stack:
	.space	0x40000
boot_stack_top:
