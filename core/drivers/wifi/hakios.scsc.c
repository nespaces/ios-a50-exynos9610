/*
 * The Galaxy A50's Wi-Fi and Bluetooth: Samsung's SCSC S620 ("Leman", Maxwell mx140), a companion chip on the
 * Exynos 9610's own buses -- not SDIO, not PCIe -- powered and reset through the PMU, talking to the AP through a
 * mailbox (0x119C0000) and shared DRAM. Boot-arg scsc=N:
 *   1  a look: the PMU's WLBT registers and the mailbox, read and said; nothing written
 *   3  power, mx140.bin, the Maxwell manager and the WLAN service; 4 one ADD_VIF; 5 then one passive scan
 *   6  stays up: en0 and the station (power, passive scan, the networks). A join is WPA2-PSK
 *      with CCMP, or an open network; other protection is refused
 *   7  one Bluetooth HCI Reset; 8 BLE scan and experimental iOS HCI command/event transport
 * Facts: the A50 kernel's drivers/misc/samsung/scsc (GPL-2.0, read for facts, not copied):
 * mif_reg_S5E9610.h (offsets, bits), platform_mif_9610.c (the sequences); the phone's device tree.
 */
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <mach/vm_param.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <sys/errno.h>
#include <sys/hakios.net.h>
#include <sys/hakios.sta.h>
#include <sys/hakios.macaddr.h>
#include <sys/random.h>
#ifdef HAKIOS_BCM4361_BT
#include <kern/clock.h>
#include <sys/kern_control.h>
#include <sys/proc.h>
#include <sys/tty.h>
#include <sys/ttycom.h>
#endif
#include "hakios.lx_wpa.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_line(const char *line);
extern void delay(int usec);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
#ifndef VM_WIMG_WCOMB
#define VM_WIMG_WCOMB           0x6
#endif
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern int hakios_partition_file(const char *partition, const char *mount, const char *path, uint64_t max, const uint8_t **data,
    uint64_t *size);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define PMU_PA                  0x11860000u     /* system-controller@11860000 */
#define MAILBOX_WLBT_PA         0x119C0000u     /* the AP<->WLBT mailbox (device tree, 0x180 long) */
#define CMU_APM_PA              0x11800000u
#define CMU_APM_GATE_MBOX_WLBT  0x205C
#define CMU_APM_QCH_MBOX_WLBT   0x3068

/* PMU (mif_reg_S5E9610.h) */
#define WLBT_CTRL_NS            0x0050          /* PWRON bit 1, RESET_SET bit 0, MASK_PWR_REQ bit 18 */
#define WLBT_CTRL_S             0x0054
#define WLBT_DEBUG              0x005C
#define CENTRAL_SEQ_WLBT_CONFIG 0x0180
#define CENTRAL_SEQ_WLBT_STATUS 0x0184          /* [23:16] the state machine */
#define RESET_AHEAD_WLBT        0x1360
#define CLEANY_BUS_WLBT         0x1364
#define LOGIC_RESET_WLBT        0x1368
#define TCXO_GATE_WLBT          0x136C
#define WLBT_DISABLE_ISO        0x1370
#define WLBT_RESET_ISO          0x1374

/* the mailbox */
#define INTGR0                  0x008
#define INTMR0                  0x010
#define INTSR0                  0x014
#define INTGR1                  0x01C
#define INTMR1                  0x024
#define INTSR1                  0x028
#define ISSR(n)                 (0x080 + 4 * (n))

static uint64_t
scsc_smc(uint64_t function, uint64_t first, uint64_t second, uint64_t third)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = first;
	register uint64_t x2 __asm__("x2") = second;
	register uint64_t x3 __asm__("x3") = third;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : : "x4", "x5", "x6", "x7", "x8", "x9", "x10",
	    "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void
scsc_look(void)
{
	vm_offset_t pmu = ml_io_map(PMU_PA, 0x2000), mbox = ml_io_map(MAILBOX_WLBT_PA, 0x1000);    /* (mapping is harmless) */
	static const struct {
		const char *name;
		uint32_t off;
	} regs[] = {
		{ "WLBT_CTRL_NS", WLBT_CTRL_NS }, { "WLBT_CTRL_S", WLBT_CTRL_S }, { "WLBT_DEBUG", WLBT_DEBUG },
		{ "CENTRAL_SEQ_WLBT_CONFIGURATION", CENTRAL_SEQ_WLBT_CONFIG }, { "CENTRAL_SEQ_WLBT_STATUS", CENTRAL_SEQ_WLBT_STATUS },
		{ "RESET_AHEAD_WLBT_SYS_PWR_REG", RESET_AHEAD_WLBT }, { "CLEANY_BUS_WLBT_SYS_PWR_REG", CLEANY_BUS_WLBT },
		{ "LOGIC_RESET_WLBT_SYS_PWR_REG", LOGIC_RESET_WLBT }, { "TCXO_GATE_WLBT_SYS_PWR_REG", TCXO_GATE_WLBT },
		{ "WLBT_DISABLE_ISO_SYS_PWR_REG", WLBT_DISABLE_ISO }, { "WLBT_RESET_ISO_SYS_PWR_REG", WLBT_RESET_ISO },
	};

	if (pmu == 0 || mbox == 0) {
		say("scsc: 1: the PMU or the mailbox could not be mapped\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		say("scsc: 1: PMU %s (%#x) = %#x\n", regs[i].name, PMU_PA + regs[i].off,
		    *(volatile uint32_t *)(pmu + regs[i].off));
	}
	uint32_t ns = *(volatile uint32_t *)(pmu + WLBT_CTRL_NS);
	say("scsc: 1: WLBT is %s, its reset %s (WLBT_CTRL_NS PWRON %u RESET_SET %u MASK_PWR_REQ %u)\n",
	    (ns & 2) ? "powered" : "not powered", (ns & 1) ? "held" : "released", (ns >> 1) & 1, ns & 1, (ns >> 18) & 1);
	/*
	 * The mailbox only with scsc_mbox=1: read with WLBT off it stopped the phone (9 November: the run never came
	 * back, pstore empty) -- its block has no clock then, and the bus waits for ever.
	 */
	uint32_t want_mbox = 0;

	if (!PE_parse_boot_argn("scsc_mbox", &want_mbox, sizeof(want_mbox)) || want_mbox == 0) {
		say("scsc: 1: the mailbox is not read (scsc_mbox=1 reads it)\n");
		return;
	}
	/*
	 * Its clock first: the mailbox is APM's block (cmucal-sfr.c: CMU_APM 0x11800000, gate MAILBOX_AP2WLBT PCLK +0x205C,
	 * CG_VAL bit 21, MANUAL bit 20; Q-channel +0x3068, ENABLE bit 0, CLOCK_REQ bit 1)
	 */
	vm_offset_t cmu = ml_io_map(CMU_APM_PA, 0x4000);

	if (cmu == 0) {
		say("scsc: 1: CMU_APM could not be mapped: the mailbox is not read\n");
		return;
	}
	volatile uint32_t *gate = (volatile uint32_t *)(cmu + CMU_APM_GATE_MBOX_WLBT), *qch = (volatile uint32_t *)(cmu + CMU_APM_QCH_MBOX_WLBT);
	uint32_t gate_was = *gate, qch_was = *qch;

	say("scsc: 1: CMU_APM MAILBOX_AP2WLBT gate %#x, Q-channel %#x (read; next, written)\n", gate_was, qch_was);

	*gate = gate_was | (1u << 21) | (1u << 20);
	*qch = (qch_was & ~1u) | (1u << 1);
	__builtin_arm_dsb(0xf);
	delay(10);
	say("scsc: 1: CMU_APM MAILBOX_AP2WLBT gate %#x -> %#x, Q-channel %#x -> %#x\n", gate_was, *gate, qch_was, *qch);
	if (want_mbox < 2) {
		say("scsc: 1: the mailbox itself is not read (scsc_mbox=2)\n");
		return;
	}
	/*
	 * Its clock was already on (gate 0x200000, the Q-channel asking): what stopped the bus is more likely the TZPC,
	 * which keeps WLBT's side secure until Samsung's CFG_REQ handler asks EL3 for it (platform_mif_9610.c:
	 * exynos_smc(SMC_CMD_CONN_IF 0x82000710, EXYNOS_WLBT 1 << 32 | EXYNOS_SET_CONN_TZPC 0)). Asked first here.
	 */
	uint64_t answer = scsc_smc(0x82000710u, (1ull << 32) | 0, 0, 0);

	say("scsc: 1: SMC CONN_IF (WLBT, SET_CONN_TZPC) answered %#llx\n", answer);
	if (want_mbox < 3) {
		say("scsc: 1: the mailbox itself is not read (scsc_mbox=3)\n");
		return;
	}
	say("scsc: 1: mailbox INTGR0 %#x INTMR0 %#x INTSR0 %#x INTGR1 %#x INTMR1 %#x INTSR1 %#x\n",
	    *(volatile uint32_t *)(mbox + INTGR0), *(volatile uint32_t *)(mbox + INTMR0), *(volatile uint32_t *)(mbox + INTSR0),
	    *(volatile uint32_t *)(mbox + INTGR1), *(volatile uint32_t *)(mbox + INTMR1), *(volatile uint32_t *)(mbox + INTSR1));
	say("scsc: 1: mailbox ISSR0-7 %#x %#x %#x %#x %#x %#x %#x %#x\n", *(volatile uint32_t *)(mbox + ISSR(0)),
	    *(volatile uint32_t *)(mbox + ISSR(1)), *(volatile uint32_t *)(mbox + ISSR(2)), *(volatile uint32_t *)(mbox + ISSR(3)),
	    *(volatile uint32_t *)(mbox + ISSR(4)), *(volatile uint32_t *)(mbox + ISSR(5)), *(volatile uint32_t *)(mbox + ISSR(6)),
	    *(volatile uint32_t *)(mbox + ISSR(7)));
}

/*
 * The guard of a step that may stop the phone for good (9 November: a mailbox read did, and the watchdog's reset
 * does not keep INFORM3, so the phone came back into the same image and stopped again, over and over, until its
 * battery was unplugged). A word in DRAM that S-Boot leaves alone (the last page of abox_rmem, kept out of XNU by
 * the loader) is set before the step and cleared after it: found set at the start, the last try never came back,
 * and the step is not tried again (the word is cleared, so the boot after that tries once more).
 */
#define GUARD_PA                (0xE9400000u + 0x2800000u - 0x1000u)
#define GUARD_MAGIC             0x5C5C0000u

static volatile uint32_t *guard;

static bool
guard_enter(uint32_t step)
{
	if (guard == NULL) {
		vm_offset_t page = ml_io_map(GUARD_PA, 0x1000);

		guard = page ? (volatile uint32_t *)page : NULL;
	}
	if (guard == NULL) {
		say("scsc: the guard word could not be mapped: step %u not tried\n", step);
		return false;
	}
	if ((guard[0] & 0xFFFF0000u) == GUARD_MAGIC) {
		say("scsc: step %u: the last boot's step %u never came back (guard word %#x): not tried this time\n", step,
		    guard[0] & 0xFFFF, guard[0]);
		guard[0] = 0;
		__builtin_arm_dsb(0xf);
		return false;
	}
	guard[0] = GUARD_MAGIC | step;
	__builtin_arm_dsb(0xf);
	return true;
}

static void
guard_leave(void)
{
	if (guard != NULL) {
		guard[0] = 0;
		__builtin_arm_dsb(0xf);
	}
}

/*
 * Step 2: WLBT's power taken by the AP and its reset released, as platform_mif_pmu_reset_release() does it
 * (platform_mif_power(): WLBT_CTRL_NS MASK_PWR_REQ 1; platform_mif_hold_reset(): RESET_SET 0) -- but not
 * WLBT_CTRL_S WLBT_START, so its CPU does not run. Then the PMU and, guarded, the mailbox.
 */
static void
scsc_wlbt_power(void)
{
	vm_offset_t pmu = ml_io_map(PMU_PA, 0x2000), mbox = ml_io_map(MAILBOX_WLBT_PA, 0x1000);
	volatile uint32_t *ns = (volatile uint32_t *)(pmu + WLBT_CTRL_NS);

	if (pmu == 0 || mbox == 0) {
		return;
	}
	say("scsc: 2: WLBT_CTRL_NS %#x: MASK_PWR_REQ (bit 18) set, RESET_SET (bit 0) cleared; WLBT_START not written\n", *ns);
	*ns |= 1u << 18;
	__builtin_arm_dsb(0xf);
	delay(1000);
	*ns &= ~1u;
	__builtin_arm_dsb(0xf);
	delay(5000);
	say("scsc: 2: WLBT_CTRL_NS %#x WLBT_CTRL_S %#x CENTRAL_SEQ_WLBT_STATUS %#x WLBT_DEBUG %#x\n", *ns,
	    *(volatile uint32_t *)(pmu + WLBT_CTRL_S), *(volatile uint32_t *)(pmu + CENTRAL_SEQ_WLBT_STATUS),
	    *(volatile uint32_t *)(pmu + WLBT_DEBUG));
	if (!guard_enter(2)) {
		return;
	}
	say("scsc: 2: reading the mailbox now (guarded)\n");
	say("scsc: 2: mailbox INTMR0 %#x INTSR0 %#x INTMR1 %#x INTSR1 %#x ISSR0-3 %#x %#x %#x %#x\n",
	    *(volatile uint32_t *)(mbox + INTMR0), *(volatile uint32_t *)(mbox + INTSR0), *(volatile uint32_t *)(mbox + INTMR1),
	    *(volatile uint32_t *)(mbox + INTSR1), *(volatile uint32_t *)(mbox + ISSR(0)), *(volatile uint32_t *)(mbox + ISSR(1)),
	    *(volatile uint32_t *)(mbox + ISSR(2)), *(volatile uint32_t *)(mbox + ISSR(3)));
	guard_leave();
	say("scsc: 2: the mailbox answers with WLBT powered\n");
}

/*
 * Step 3: the chip started, as Samsung's mxman_start() and platform_mif_9610.c do it, short of the Maxwell manager:
 *   - 16 MB of shared DRAM (the device tree's wifibt_if, 0x1000000), mx140.bin copied to its start verbatim
 *     (mx140_file.c), cleaned to memory
 *   - the mailbox reset as platform_mif_map() leaves it; ISSR0 the firmware's entry point (its header, +40), ISSR1 the
 *     configuration's address (0 here: no mxconf yet), ISSR2 0xbcdeedcb, ISSR3 the start flags (0) (mxman.c mbox_init)
 *   - WLBT_CTRL_S WLBT_START (the power and the reset as step 2), then the chip's boot ROM raises CFG_REQ (SPI 73,
 *     GIC INTID 105, polled at the distributor's pending bits) and waits for the AP to configure it
 *     (platform_cfg_req_isr): the TZPC through SMC CONN_IF, WLBT_REMAP, CHIP_VERSION_ID's EMA bits, the DBUS window
 *     (WLBT 0x80000000.. -> the shared DRAM) and PBUS windows (the mailboxes, USI_CMG00, SYSREG_CMGP2WLBT, GPIO_CMGP,
 *     SHUB), the keep-alive patch into BOOT_CFG +0x1000 (fw/scsc_ka.bin, tools/a50-scsc-ka.py), BOOT_CFG_ACK, and the
 *     power given back (MASK_PWR_REQ 0)
 * then for 5 s what the firmware does: the mailbox, the PMU, the first words of the panic record (header +176).
 */
#define GICD_PA                 0x12301000u
#define CFG_REQ_INTID           105u            /* SPI 73 (interrupts = <0 0x49 4>) */
#define WLBT_REMAP_PA           0x14C50000u
#define DBUS_BAAW_PA            0x14C20000u
#define PBUS_BAAW_PA            0x14C30000u
#define BOOT_CFG_PA             0x14C60000u
#define SHARED_BYTES            0x1000000u
#define SHARED_PA               (0xE9400000u + 0x1000000u)
#define BAAW_ACCESS             0x80000003u     /* INIT_DONE | EN_WRITE | EN_READ */
#define EMA_MASK                0x003FFFFFu
#define EMA_VALUE               ((1u << 20) | (1u << 18) | (1u << 13) | (1u << 11) | (1u << 5) | (1u << 2))

static void
w32(vm_offset_t base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__builtin_arm_dsb(0xf);
}

static uint32_t
r32(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

/*
 * Step 4: Maxwell's configuration (mxman.c transports_init(), mxconf.h, mifstream.c, cpacket_buffer.c, the 9610's
 * mifintrbit): struct mxconf (magic 0x79828486, version 0.5) and the circular packet buffers it names, laid out in the
 * shared DRAM after the firmware's runtime (miframman's heap starts at fw_runtime_length), every address an offset
 * from the shared DRAM's start (platform_mif_get_mifram_ref). The streams, sized as Linux sizes them:
 *   mxmgmt   to the AP and from it: 4 packets of 128 bytes (struct mxmgr_message: channel id, then 127 bytes)
 *   gdb      R4's and M4's monitors, both ways: 512 packets of 4 bytes; M4_1 not in use (buffer 0)
 *   mxlog    to the AP: 4096 packets of 4 bytes
 * A stream to the AP is filled with 0xff first (mif_stream_init). Its read bit is the AP's to the chip, its write bit
 * the chip's to the AP; from the AP, the other way round. The AP's bits to the chip go to the mailbox's INTGR1 (bit 0
 * kept for forcing a panic), the chip's to the AP come in INTSR0's top half: not used here, the indices are polled.
 */
struct mx_cbuf {
	uint32_t buffer, num_packets, packet_size, read_index, write_index;
};
struct mx_stream {
	struct mx_cbuf buf;
	uint8_t read_bit, write_bit, pad[2];
};
struct mx_trans {
	struct mx_stream to_ap, from_ap;
};
struct mx_conf {
	uint32_t magic;
	uint16_t major, minor;
	struct mx_trans mgmt, gdb_r4, gdb_m4;
	struct mx_stream log;
	uint32_t soc_revision, flags;
	uint32_t fwconfig_offset, fwconfig_size;
	struct mx_trans gdb_m4_1;
};
_Static_assert(sizeof(struct mx_conf) == 240, "struct mxconf as Linux lays it out (not packed)");

static uint32_t heap_next;

static uint32_t
heap_alloc(vm_offset_t mem, uint32_t bytes, uint8_t fill)
{
	uint32_t at = (heap_next + 63) & ~63u;

	heap_next = at + bytes;
	memset((void *)(mem + at), fill, bytes);
	return at;
}

static void
stream_make(vm_offset_t mem, struct mx_stream *st, uint32_t packets, uint32_t size, bool to_ap, uint8_t ap_bit,
    uint8_t chip_bit)
{
	st->buf.buffer = heap_alloc(mem, packets * size, to_ap ? 0xFF : 0);
	st->buf.num_packets = packets;
	st->buf.packet_size = size;
	st->buf.read_index = heap_alloc(mem, 4, 0);
	st->buf.write_index = heap_alloc(mem, 4, 0);
	/* (to the AP: the reader is the AP, it tells the chip with a bit to the chip; from the AP: the other way) */
	st->read_bit = to_ap ? chip_bit : ap_bit;
	st->write_bit = to_ap ? ap_bit : chip_bit;
}

static uint32_t mgmt_in_ref;            /* the mxmgmt stream to the AP: where its buffer and indices are */
static uint32_t mgmt_out_ref;           /* ... and the one from the AP */
static uint32_t log_ref;                /* the mxlog stream to the AP */

static uint32_t
mxconf_make(vm_offset_t mem, uint32_t runtime)
{
	uint32_t at;
	struct mx_conf *c;

	heap_next = runtime;
	at = heap_alloc(mem, sizeof(struct mx_conf), 0);
	c = (struct mx_conf *)(mem + at);
	c->magic = 0x79828486u;
	c->major = 0;
	c->minor = 5;
	/* bits: to the AP 0..6 (INTSR0's top half); to the chip R4 1..4, M4 1 (0 is each one's panic bit) */
	stream_make(mem, &c->mgmt.to_ap, 4, 128, true, 0, 1);
	stream_make(mem, &c->mgmt.from_ap, 4, 128, false, 1, 2);
	stream_make(mem, &c->gdb_r4.to_ap, 512, 4, true, 2, 3);
	stream_make(mem, &c->gdb_r4.from_ap, 512, 4, false, 3, 0);
	stream_make(mem, &c->gdb_m4.to_ap, 512, 4, true, 4, 1);
	stream_make(mem, &c->gdb_m4.from_ap, 512, 4, false, 5, 0);
	stream_make(mem, &c->log, 4096, 4, true, 6, 4);
	c->soc_revision = 0;
	c->flags = 0;
	c->fwconfig_offset = 0;
	c->fwconfig_size = 0;
	memset(&c->gdb_m4_1, 0, sizeof(c->gdb_m4_1));
	mgmt_in_ref = at + __builtin_offsetof(struct mx_conf, mgmt.to_ap);
	mgmt_out_ref = at + __builtin_offsetof(struct mx_conf, mgmt.from_ap);
	log_ref = at + __builtin_offsetof(struct mx_conf, log);
	say("scsc: 4: mxconf at +%#x (%u bytes), the heap after it to +%#x; mxmgmt to the AP: buffer +%#x, indices +%#x +%#x\n",
	    at, (unsigned)sizeof(struct mx_conf), heap_next, c->mgmt.to_ap.buf.buffer, c->mgmt.to_ap.buf.read_index,
	    c->mgmt.to_ap.buf.write_index);
	return at;
}

static bool wlan_started;
static uint8_t bt_service_cfm;

/* what the chip has put in the mxmgmt stream to the AP, said and taken */
static unsigned
mgmt_take(vm_offset_t mem)
{
	struct mx_stream *st = (struct mx_stream *)(mem + mgmt_in_ref);
	volatile uint32_t *ri = (volatile uint32_t *)(mem + st->buf.read_index), *wi = (volatile uint32_t *)(mem + st->buf.write_index);
	unsigned taken = 0;

	while (*ri != *wi && taken < 8) {
		const uint8_t *m = (const uint8_t *)(mem + st->buf.buffer + (*ri % st->buf.num_packets) * st->buf.packet_size);

		say("scsc: 4: from the chip: channel %u: %02x %02x %02x %02x %02x %02x %02x %02x%s\n", m[0], m[1], m[2], m[3], m[4],
		    m[5], m[6], m[7], m[8], m[0] == 0 && m[1] == 0 ? " -- MM_START_IND: Maxwell's firmware runs" :
		    m[0] == 1 && m[2] == 1 ? " -- SM_MSG_START_CFM: the service started" : "");
		if (m[0] == 1 && m[1] == 1 && m[2] == 1) { wlan_started = true; }
		if (m[0] == 1 && m[1] == 2 && (m[2] == 1 || m[2] == 3)) {
			bt_service_cfm = m[2];
		}
		*ri = (*ri + 1) % st->buf.num_packets;
		__builtin_arm_dsb(0xf);
		taken++;
	}
	return taken;
}

/*
 * A message to the chip on the mxmgmt stream from the AP (mxmgmt_transport_send(): the channel id, then the payload,
 * in one 128-byte packet; cpacketbuffer_write(), then the stream's write bit to R4 through the mailbox's INTGR1).
 */
static bool
mgmt_send(vm_offset_t mem, vm_offset_t mbox, uint8_t channel, const void *payload, uint32_t length)
{
	struct mx_stream *st = (struct mx_stream *)(mem + mgmt_out_ref);
	volatile uint32_t *ri = (volatile uint32_t *)(mem + st->buf.read_index), *wi = (volatile uint32_t *)(mem + st->buf.write_index);
	uint32_t next = (*wi + 1) % st->buf.num_packets;
	uint8_t *packet;

	if (next == *ri || length > st->buf.packet_size - 1) {
		return false;
	}
	packet = (uint8_t *)(mem + st->buf.buffer + *wi * st->buf.packet_size);
	memset(packet, 0, st->buf.packet_size);
	packet[0] = channel;
	memcpy(packet + 1, payload, length);
	__builtin_arm_dsb(0xf);
	*wi = next;
	__builtin_arm_dsb(0xf);
	w32(mbox, INTGR1, 1u << st->write_bit);
	return true;
}

/*
 * Step 6: the WLAN service (id 1) started with its HIP4 area, as the SLSI driver does (cm_if.c: 2.5 MB of the service's
 * shared memory, 4 KB aligned; hip4.c hip4_init(): the control block at its start; then SM_MSG_START_REQ with the
 * control block's address). The layout (hip4.h, this firmware's kernel: no SMAPPER, no RX NAPI):
 *   +0x000000  struct hip4_hip_control: init { 0xcaaa0400, 0, config_v4's address, config_v5's }, config_v5 and
 *              config_v4 (both 0xcaba0401: TX at +0x0A000 (1 MB data + 64 KB control), RX at +0x11A000 (1 MB), the
 *              MIB, one interrupt bit each way, the scoreboard, 6 queues of 256 entries)
 *   +0x002000  the MIB: the firmware's wlan.hcf, then wlan_sw.hcf, as they are (mgt.c, the files' bytes)
 * Every address is a mifram reference: an offset from the shared DRAM's start.
 */
#define HIP_TX_OFFSET           0x0A000u
#define HIP_TX_SIZE             (0x100000u + 0x10000u)
#define HIP_RX_OFFSET           (HIP_TX_OFFSET + HIP_TX_SIZE)
#define HIP_RX_SIZE             0x100000u
#define HIP_MIB_OFFSET          0x02000u
#define HIP_MIB_SIZE            0x08000u
#define HIP_AREA_SIZE           0x280000u       /* 2.5 MB */

struct hip_cfg_common {
	uint32_t magic;
	uint16_t version, length;
	uint32_t compat_flag;
	uint16_t sap_mlme, sap_ma, sap_debug, sap_test;
	uint32_t fw_build_id, fw_patch_id;
	uint8_t unidat_headroom, unidat_tailroom, bulk_align, host_cache_line;
	uint32_t host_buf_loc, host_buf_sz, fw_buf_loc, fw_buf_sz, mib_loc, mib_sz, log_config_loc, log_config_sz;
} __attribute__((packed));
struct hip_cfg_v5 {
	struct hip_cfg_common c;
	uint8_t fh_int, th_int, reserved[2];
	uint32_t scbrd_loc;
	uint16_t q_num, q_len, q_idx_sz;
	uint8_t reserved2[2];
	uint32_t q_loc[6];
	uint8_t reserved3[16];
} __attribute__((packed));
struct hip_cfg_v4 {
	struct hip_cfg_common c;
	uint8_t fh_int, reserved1[3];
	uint8_t th_int[6], reserved2[2];
	uint32_t scbrd_loc;
	uint16_t q_num, q_len, q_idx_sz;
	uint8_t reserved3[2];
	uint32_t q_loc[6];
	uint8_t reserved_nosmapper[99];
	uint8_t reserved4[16];
} __attribute__((packed));
struct hip_q {
	uint32_t array[256];
	uint8_t idx_read, idx_write, total;
} __attribute__((aligned(64)));
struct hip_control {
	struct { uint32_t magic, version, v4_ref, v5_ref; } __attribute__((packed)) init;
	struct hip_cfg_v5 v5 __attribute__((aligned(32)));
	struct hip_cfg_v4 v4 __attribute__((aligned(32)));
	uint32_t scoreboard[256] __attribute__((aligned(64)));
	struct hip_q q[6] __attribute__((aligned(64)));
} __attribute__((aligned(4096)));
_Static_assert(sizeof(struct hip_cfg_v5) == 120, "hip4_hip_config_version_5");
_Static_assert(sizeof(struct hip_cfg_v4) == 227, "hip4_hip_config_version_4 without SMAPPER");
_Static_assert(__builtin_offsetof(struct hip_control, v5) == 32 && __builtin_offsetof(struct hip_control, v4) == 160 &&
    __builtin_offsetof(struct hip_control, scoreboard) == 448 && __builtin_offsetof(struct hip_control, q) == 1472,
    "struct hip4_hip_control as the SLSI driver lays it out");

static uint32_t hip_ref;

static void
hip_common(struct hip_cfg_common *c, uint16_t version, uint16_t length, uint32_t mib_len)
{
	c->magic = 0xcaba0401u;
	c->version = version;
	c->length = length;
	c->host_cache_line = 64;
	c->host_buf_loc = hip_ref + HIP_TX_OFFSET;
	c->host_buf_sz = HIP_TX_SIZE;
	c->fw_buf_loc = hip_ref + HIP_RX_OFFSET;
	c->fw_buf_sz = HIP_RX_SIZE;
	c->mib_loc = mib_len ? hip_ref + HIP_MIB_OFFSET : 0;
	c->mib_sz = mib_len;
	c->log_config_loc = 0;
}

static bool
hip_make(vm_offset_t mem, uint8_t fh_bit, uint8_t th_bit)
{
	struct hip_control *h;
	uint32_t mib_len = 0;
	const char *const mibs[] = { "mx140_wlan.hcf", "mx140_wlan_sw.hcf" };

	heap_next = (heap_next + 4095) & ~4095u;
	hip_ref = heap_alloc(mem, HIP_AREA_SIZE, 0);
	if (hip_ref & 4095) {                           /* (heap_alloc aligns to 64 only) */
		hip_ref = (hip_ref + 4095) & ~4095u;
	}
	h = (struct hip_control *)(mem + hip_ref);
	for (int i = 0; i < 2; i++) {
		uint64_t size = 0;
		const uint8_t *f = hakios_image_file("fw", mibs[i], &size, NULL);

		if (f == NULL) {
			say("scsc: 6: no fw/%s\n", mibs[i]);
			continue;
		}
		/*
		 * As the SLSI driver hands it over (mgt.c slsi_mib_open_file(), slsi_mib_get_platform()): the file's 8-byte
		 * header off (byte 7, the format, must be 1), then the records the firmware does not want -- PSID 0xFFFE
		 * (the version) and 0xFFFF (the platform's name) -- each { u16 PSID, u16 length, the value padded to even }.
		 * Given whole, the header reads as a record 0x42C1 long and the firmware panics copying it (9 November)
		 */
		const uint8_t *d = f;
		uint64_t left = size;

		if (left < 8 || d[7] != 1) {
			say("scsc: 6: %s: not an HCF file of format 1\n", mibs[i]);
			continue;
		}
		d += 8;
		left -= 8;
		while (left >= 4 && (uint16_t)(d[0] | d[1] << 8) >= 0xFFFE) {
			uint32_t rec = 4 + (((uint32_t)(d[2] | d[3] << 8) + 1) & ~1u);

			if (rec > left) {
				break;
			}
			d += rec;
			left -= rec;
		}
		if (mib_len + left > HIP_MIB_SIZE) {
			say("scsc: 6: %s does not fit in the MIB area\n", mibs[i]);
			continue;
		}
		memcpy((void *)(mem + hip_ref + HIP_MIB_OFFSET + mib_len), d, (size_t)left);
		mib_len += (uint32_t)left;
		say("scsc: 6: MIB file %s: %llu bytes, %llu of records after its header (first PSID %#x)\n", mibs[i], size, left,
		    left >= 2 ? d[0] | d[1] << 8 : 0);
	}
	hip_common(&h->v4.c, 4, sizeof(struct hip_cfg_v4), mib_len);
	h->v4.fh_int = fh_bit;
	for (int i = 0; i < 6; i++) {
		h->v4.th_int[i] = th_bit;
	}
	h->v4.scbrd_loc = hip_ref + __builtin_offsetof(struct hip_control, scoreboard);
	h->v4.q_num = 6;
	h->v4.q_len = 256;
	h->v4.q_idx_sz = 1;
	hip_common(&h->v5.c, 5, sizeof(struct hip_cfg_v5), mib_len);
	h->v5.fh_int = fh_bit;
	h->v5.th_int = th_bit;
	h->v5.scbrd_loc = h->v4.scbrd_loc;
	h->v5.q_num = 6;
	h->v5.q_len = 256;
	h->v5.q_idx_sz = 1;
	for (int i = 0; i < 6; i++) {
		h->v4.q_loc[i] = h->v5.q_loc[i] = hip_ref + (uint32_t)__builtin_offsetof(struct hip_control, q) +
		    (uint32_t)(i * sizeof(struct hip_q));
	}
	h->init.magic = 0xcaaa0400u;
	h->init.version = 0;
	h->init.v4_ref = hip_ref + __builtin_offsetof(struct hip_control, v4);
	h->init.v5_ref = hip_ref + __builtin_offsetof(struct hip_control, v5);
	__builtin_arm_dsb(0xf);
	say("scsc: 6: HIP4 at +%#x (2.5 MB), the MIB %u bytes; bits: from the AP %u, to the AP %u\n", hip_ref, mib_len, fh_bit,
	    th_bit);
	return true;
}


/* Bounded, polling HIP4 diagnostic. Queue indices are bytes in the scoreboard,
 * not the legacy fields at the end of hip_q. Protocol facts: Samsung HIP4/FAPI.
 * scsc=4 sends one ADD_VIF; scsc=5 then scans passively.
 * scsc=6 does not use this probe: hip_station() keeps the queues and serves hakios_sta_ops.
 * No association, keys or packet data yet. */
static const uint8_t hip_indices[6][2] = {
	{0, 64}, {1, 65}, {66, 2}, {67, 3}, {68, 4}, {5, 69}
};
static uint16_t hip_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void hip_put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static uint8_t
hip_index(struct hip_control *h, unsigned q, unsigned rw)
{
	uint8_t v = ((volatile uint8_t *)h->scoreboard)[hip_indices[q][rw]];
	__builtin_arm_dsb(0xf);
	return v;
}

static void
hip_index_set(struct hip_control *h, unsigned q, unsigned rw, uint8_t v)
{
	__builtin_arm_dsb(0xf);
	((volatile uint8_t *)h->scoreboard)[hip_indices[q][rw]] = v;
	__builtin_arm_dsb(0xf);
}

static bool
hip_push(struct hip_control *h, vm_offset_t mbox, unsigned q, uint32_t ref)
{
	uint8_t w = hip_index(h, q, 0), r = hip_index(h, q, 1);
	if ((uint8_t)(w + 1) == r) { return false; }
	((volatile uint32_t *)h->q[q].array)[w] = ref;
	hip_index_set(h, q, 0, (uint8_t)(w + 1));
	w32(mbox, INTGR1, 1u << 5);
	return true;
}


static bool
hip_scan_send(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, uint32_t ref, uint32_t version)
{
	const struct hip_cfg_common *c = version == 4 ? &h->v4.c : &h->v5.c;
	uint8_t *b = (uint8_t *)(mem + ref), *f = b + 24;
	/* 36-byte FAPI + 4-byte prefix; payload: timing IE + 11 passive channels. */
	uint16_t payload = 19 + 7 + 11 * 3, head = 40 + c->unidat_headroom;
	uint16_t dat = payload + c->unidat_headroom + c->unidat_tailroom;
	if (20u + 40 + dat > 1024) { return false; }
	memset(b, 0, 1024);
	b[4] = 5; b[5] = 5; b[6] = 1; b[7] = 1;
	hip_put16(b + 8, dat); hip_put16(b + 10, 40);
	hip_put16(b + 12, payload); hip_put16(b + 14, head);
	hip_put16(f, 0x2005); hip_put16(f + 4, 0xc002);
	hip_put16(f + 12, 0x100); hip_put16(f + 14, 2);
	const uint8_t mac[6] = {2, 0x50, 0, 0, 0, 1};
	memcpy(f + 16, mac, 6); hip_put16(f + 22, 4);
	uint8_t *d = b + 20 + head;
	const uint8_t timing[19] = {0xdd, 17, 0, 0x16, 0x32, 1, 1};
	const uint8_t channels[7] = {0xdd, 38, 0, 0x16, 0x32, 1, 2};
	memcpy(d, timing, sizeof(timing)); memcpy(d + 19, channels, sizeof(channels));
	for (unsigned i = 0; i < 11; i++) {
		hip_put16(d + 26 + i * 3, 2 * (2412 + i * 5)); d[28 + i * 3] = 1;
	}
	say("scsc: MLME_ADD_SCAN_REQ passive channels 1-11, headroom %u tailroom %u\n",
	    c->unidat_headroom, c->unidat_tailroom);
	return hip_push(h, mbox, 0, ref);
}

static void
hip_scan_result(const uint8_t *sig, const uint8_t *data, unsigned len)
{
	if (len < 36 || (data[0] != 0x80 && data[0] != 0x50)) { return; }
	char ssid[129] = {0}; unsigned used = 0;
	for (unsigned at = 36; at + 2 <= len;) {
		unsigned id = data[at], n = data[at + 1]; at += 2;
		if (n > len - at) { return; }
		if (id == 0) {
			if (n > 32) { return; }
			for (unsigned i = 0; i < n; i++) {
				unsigned c = data[at + i];
				if (c >= 32 && c <= 126 && c != '\\' && c != '"') { ssid[used++] = c; }
				else { used += snprintf(ssid + used, sizeof(ssid) - used, "\\x%02x", c); }
			}
			break;
		}
		at += n;
	}
	say("scsc: SCAN network BSSID %02x:%02x:%02x:%02x:%02x:%02x freq %u MHz RSSI %d SSID \"%s\"\n",
	    data[16], data[17], data[18], data[19], data[20], data[21], hip_u16(sig + 12) / 2,
	    (int16_t)hip_u16(sig + 14), ssid);
}


/* Single diagnostic worker owns these buffers. Validate the entire chain before
 * publishing any returned ref: malformed/cyclic chains keep their ownership. */
static uint8_t hip_rx_data[32768];          /* a chain of up to 16 buffers of 2028 bytes; 8192 made a long chain "invalid" and its buffers were never returned */
static unsigned hip_rx_why, hip_rx_a, hip_rx_b, hip_rx_c;
static bool
hip_rx_collect(vm_offset_t mem, uint32_t ref, uint32_t refs[16], unsigned *count,
    const uint8_t **signal, unsigned *signal_len, unsigned *data_len)
{
	uint32_t lo = hip_ref + HIP_RX_OFFSET, hi = lo + HIP_RX_SIZE;
	*count = 0; *data_len = 0;
	if (ref == 0) { hip_rx_why = 1; return false; }
	while (ref) {
		if (*count == 16 || ref < lo || ref > hi - 20 || (ref & 3)) { hip_rx_why = 2; hip_rx_a = *count; hip_rx_b = ref; hip_rx_c = hi; return false; }
		for (unsigned i = 0; i < *count; i++) { if (refs[i] == ref) { hip_rx_why = 3; hip_rx_a = *count; return false; } }
		const uint8_t *m = (const uint8_t *)(mem + ref);
		uint16_t sig = hip_u16(m + 10), len = hip_u16(m + 12), head = hip_u16(m + 14);
		if (sig > hi - ref - 20) { hip_rx_why = 4; hip_rx_a = sig; hip_rx_b = ref; return false; }
		if (*count == 0) {
			if (!(m[4] & 1) || sig < 14) { hip_rx_why = 5; hip_rx_a = m[4]; hip_rx_b = sig; return false; }
			*signal = m + 24; *signal_len = sig - 4;
		}
		/* Signal-only RX has head rounded beyond sig_bufsz, dat_bufsz == 0.
		 * Check payload ranges only when a payload actually exists. */
		if (len) {
			if (head < sig || (uint32_t)head + len > hi - ref - 20 || len > sizeof(hip_rx_data) - *data_len) { hip_rx_why = 6; hip_rx_a = head << 16 | len; hip_rx_b = sig; hip_rx_c = *data_len; return false; }
			memcpy(hip_rx_data + *data_len, m + 20 + head, len); *data_len += len;
		}
		refs[(*count)++] = ref;
		bool chained = (m[4] & 0x80) && (*count > 1 || (m[4] & 0x40));
		ref = chained ? (uint32_t)hip_u16(m + 16) | (uint32_t)hip_u16(m + 18) << 16 : 0;
	}
	return true;
}

static void
hip_probe(vm_offset_t mem, vm_offset_t mbox, bool scan)
{
	struct hip_control *h = (struct hip_control *)(mem + hip_ref);
	uint32_t version = ((volatile struct hip_control *)h)->init.version;
	uint32_t tx = hip_ref + HIP_TX_OFFSET + 0x100000;
	uint8_t *b = (uint8_t *)(mem + tx), *f = b + 24;
	bool returned = false, confirmed = false, vif_ok = false;
	bool scan_sent = false, scan_returned = false, scan_done = false;
	if (!wlan_started || (version != 4 && version != 5)) {
		say("scsc: HIP4 probe skipped: WLAN ready %u, version %u\n", wlan_started, version);
		return;
	}
	say("scsc: HIP4 probe: negotiated v%u; ADD_VIF station 1, diagnostic MAC 02:50:00:00:00:01\n", version);
	/* A single non-reused control slot: packed 20-byte mbulk, 4-byte prefix,
	 * 38-byte FAPI signal, padded to 44 bytes including the prefix. */
	memset(b, 0, 64);
	b[4] = 1 | 4; b[5] = 5; b[6] = 1;
	hip_put16(b + 10, 44); hip_put16(b + 14, 44);
	hip_put16(f, 0x2007); hip_put16(f + 4, 0xc001);
	hip_put16(f + 10, 1);
	const uint8_t mac[6] = {2, 0x50, 0, 0, 0, 1};
	memcpy(f + 12, mac, 6); hip_put16(f + 18, 2); memcpy(f + 20, mac, 6);
	if (!hip_push(h, mbox, 0, tx)) { say("scsc: HIP4 FH_CTRL full\n"); return; }
	for (unsigned tick = 0; tick < (scan ? 3000u : 1000u); tick++) {
		/* Bound each drain so a broken producer cannot monopolise the thread. */
		for (unsigned q = 2; q <= 4; q++) {
			for (unsigned n = 0; n < 256; n++) {
				uint8_t r = hip_index(h, q, 1), w = hip_index(h, q, 0);
				if (r == w) { break; }
				uint32_t ref = ((volatile uint32_t *)h->q[q].array)[r];
				if (q == 2) {
					/* Some firmware revisions tag returned refs with colour bits. Log
					 * unknown values and stop; never turn a ref into an unchecked pointer. */
					if (!((ref == tx && !returned) || (scan_sent && ref == tx + 1024 && !scan_returned))) { say("scsc: HIP4 unexpected TX return %#x\n", ref); return; }
					if (ref == tx) { returned = true; } else { scan_returned = true; }
					say("scsc: HIP4 TX buffer returned +%#x\n", ref);
				} else {
					uint32_t refs[16]; unsigned count, siglen, len;
					const uint8_t *s = NULL;
					if (!hip_rx_collect(mem, ref, refs, &count, &s, &siglen, &len)) {
						say("scsc: HIP4 invalid RX chain +%#x q%u\n", ref, q); return;
					}
					unsigned sig = siglen + 4;
					uint8_t free_slots = (uint8_t)(hip_index(h, 5, 1) - hip_index(h, 5, 0) - 1);
					if (count > free_slots) { break; }
					uint16_t id = hip_u16(s);
					say("scsc: HIP4 RX q%u id %#x sig %u data %u receiver %#x\n", q, id, sig - 4, len, hip_u16(s + 2));
					if (id == 0x2107 && sig >= 30 && hip_u16(s + 2) == 0xc001 && hip_u16(s + 10) == 1) {
						confirmed = true; vif_ok = hip_u16(s + 12) == 0;
						say("scsc: MLME_ADD_VIF_CFM result %u (0 = success)\n", hip_u16(s + 12));
					}
					if (scan_sent && id == 0x2105 && sig >= 32 && hip_u16(s + 2) == 0xc002) {
						say("scsc: MLME_ADD_SCAN_CFM result %u scan_id %#x\n", hip_u16(s + 12), hip_u16(s + 14));
					}
					if (scan_sent && id == 0x2300 && sig >= 42 && hip_u16(s + 16) == 0x100) {
						hip_scan_result(s, hip_rx_data, len);
					}
					if (scan_sent && id == 0x2301 && sig >= 30 && hip_u16(s + 12) == 0x100) {
						scan_done = true; say("scsc: MLME_SCAN_DONE_IND scan_id 0x100\n");
					}
					for (unsigned i = 0; i < count; i++) {
						if (!hip_push(h, mbox, 5, refs[i])) { return; }
					}
				}
				hip_index_set(h, q, 1, (uint8_t)(r + 1));
			}
		}
		if (confirmed && returned) {
			if (!scan || !vif_ok || (scan_done && scan_returned)) { break; }
			if (!scan_sent) { scan_sent = hip_scan_send(mem, mbox, h, tx + 1024, version); }
		}
		delay(10000);
	}
	say("scsc: HIP4 probe done: CFM %u TX returned %u; FH_CTRL %u/%u TH_CTRL %u/%u TH_RFB %u/%u\n",
	    confirmed, returned, hip_index(h, 0, 0), hip_index(h, 0, 1), hip_index(h, 3, 0), hip_index(h, 3, 1),
	    hip_index(h, 5, 0), hip_index(h, 5, 1));
}

/*
 * scsc=6: one thread owns the HIP4 queues for the rest of the run. The station thread
 * (hakios.sta.c) only posts a request and waits. A join is WPA2-PSK with CCMP, or an open
 * network; anything else is refused. Scan stays passive on 2.4 GHz channels 1-11.
 */
static void log_dump(vm_offset_t mem, uint32_t panic_off);
static uint32_t hip_panic_off;          /* for the firmware's log at a failed join */

#define HIP_SLOTS               4
#define HIP_SLOT_BYTES          2048u
#define HIP_BSS_MOST            32
#define HIP_SCAN_ID             0x0100u
#define HIP_EAPOL_MOST          4

static uint8_t scsc_mac[6] = { 0x02, 0x50, 0x00, 0x00, 0x00, 0x01 };
static uint32_t hip_version;
static uint32_t hip_slot_ref[HIP_SLOTS];
static uint8_t hip_slot_busy[HIP_SLOTS];
static uint16_t hip_sender_next = 0xc010;
static uint16_t hip_scan_ch[11];
static unsigned hip_scan_n;
static struct hakios_sta_bss hip_bss[HIP_BSS_MOST];
static unsigned hip_bss_n;
static volatile int hip_phase_v = HAKIOS_STA_OFF;
static volatile int hip_vif_v;
static volatile int hip_joined_v;
static volatile int hip_error_v;
static unsigned hip_debug_inds, hip_other_log, hip_bad_log;

static LCK_GRP_DECLARE(hip_lock_group, "scsc-hip");
static LCK_MTX_DECLARE(hip_lock, &hip_lock_group);
enum { HIP_REQ_ON = 1, HIP_REQ_OFF = 2, HIP_REQ_SCAN = 3, HIP_REQ_JOIN = 4, HIP_REQ_TX = 5, HIP_REQ_LEAVE = 6 };
static bool hip_rx_other(uint16_t id, const uint8_t *s, unsigned siglen, const uint8_t *data, unsigned len);
static int hip_req, hip_job, hip_status;
static bool hip_alive;

/* Filled on the station thread before HIP_REQ_JOIN. The HIP thread only reads it. */
static struct {
	uint8_t bssid[6];
	uint8_t ssid[32];
	uint8_t ssid_length;
	uint16_t frequency;
	int16_t rssi;
	uint16_t beacon_interval;
	uint16_t capability;
	uint8_t rsn[22];
	bool wpa;
	uint16_t elements_length;
	uint8_t elements[HAKIOS_STA_ELEMENTS_MOST];
} hip_target;
static struct lx_wpa hip_wpa;
static struct {
	unsigned len;
	uint8_t body[384];
} hip_eapol_q[HIP_EAPOL_MOST];
static unsigned hip_eapol_n;
static bool hip_ind_seen, hip_lost, hip_m4_armed, hip_m4_tx_back, hip_m4_acked;
static int hip_ind_code, hip_m4_status;
static uint16_t hip_m4_tag, hip_tag_next = 1;
static uint8_t hip_tx_frame[HAKIOS_NET_FRAME_MAX];
static unsigned hip_tx_len;
static volatile int hip_tx_busy;
static unsigned hip_tx_said, hip_rx_said, hip_cfm_said;

/* What the command now in flight is waiting to hear. Only the HIP thread touches this. */
static struct {
	uint16_t want_cfm, sender, scan_id;
	uint32_t tx_ref;
	bool cfm_seen, cfm_ok, scan_done, tx_back;
	int result;
} hip_wait;

static uint16_t
hip_sender(void)
{
	uint16_t sender = hip_sender_next++;

	if (hip_sender_next < 0xc010) {
		hip_sender_next = 0xc010;
	}
	return sender;
}

static void
hip_slots_init(void)
{
	uint32_t base = hip_ref + HIP_TX_OFFSET + 0x100000u;

	for (int i = 0; i < HIP_SLOTS; i++) {
		hip_slot_ref[i] = base + (uint32_t)i * HIP_SLOT_BYTES;
		hip_slot_busy[i] = 0;
	}
}

static int
hip_slot_get(void)
{
	for (int i = 0; i < HIP_SLOTS; i++) {
		if (!hip_slot_busy[i]) {
			hip_slot_busy[i] = 1;
			return i;
		}
	}
	return -1;
}

static void
hip_slot_release_ref(uint32_t ref)
{
	for (int i = 0; i < HIP_SLOTS; i++) {
		if (hip_slot_busy[i] && (ref == hip_slot_ref[i] || (ref & 0x00ffffffu) == hip_slot_ref[i])) {
			hip_slot_busy[i] = 0;
		}
	}
}

static void
hip_bss_note(const uint8_t *sig, unsigned siglen, const uint8_t *data, unsigned len)
{
	uint16_t freq;
	int16_t rssi;
	unsigned ssid_n = 0, ie_n, at;
	uint8_t ssid[32];
	struct hakios_sta_bss *slot = NULL;
	bool known = false;

	if (siglen < 18 || len < 36 || (data[0] != 0x80 && data[0] != 0x50)) {
		return;
	}
	if ((data[16] | data[17] | data[18] | data[19] | data[20] | data[21]) == 0) {
		return;
	}
	freq = hip_u16(sig + 12) / 2;
	rssi = (int16_t)hip_u16(sig + 14);
	if (freq < 2400 || freq > 6000) {
		return;
	}
	for (at = 36; at + 2 <= len; ) {
		unsigned id = data[at], n = data[at + 1];

		at += 2;
		if (n > len - at) {
			break;
		}
		if (id == 0) {
			if (n > 32) {
				return;
			}
			memcpy(ssid, data + at, n);
			ssid_n = n;
			break;
		}
		at += n;
	}
	for (unsigned i = 0; i < hip_bss_n; i++) {
		if (memcmp(hip_bss[i].bssid, data + 16, 6) == 0) {
			slot = &hip_bss[i];
			known = true;
			break;
		}
	}
	if (slot == NULL) {
		if (hip_bss_n >= HIP_BSS_MOST) {
			return;
		}
		slot = &hip_bss[hip_bss_n++];
		memset(slot, 0, sizeof(*slot));
		memcpy(slot->bssid, data + 16, 6);
	}
	if (!known || rssi > slot->rssi) {
		slot->rssi = rssi;
		slot->frequency = freq;
		slot->beacon_interval = hip_u16(data + 32);
		slot->capability = hip_u16(data + 34);
		ie_n = len - 36;
		if (ie_n > HAKIOS_STA_ELEMENTS_MOST) {
			ie_n = HAKIOS_STA_ELEMENTS_MOST;
		}
		memcpy(slot->elements, data + 36, ie_n);
		slot->elements_length = (uint16_t)ie_n;
	}
	if (ssid_n != 0 && slot->ssid_length == 0) {
		memcpy(slot->ssid, ssid, ssid_n);
		slot->ssid_length = (uint8_t)ssid_n;
		known = false;
	}
	if (!known) {
		char name[96];
		unsigned used = 0;

		name[0] = 0;
		for (unsigned i = 0; i < slot->ssid_length && used + 4 < sizeof(name); i++) {
			unsigned c = slot->ssid[i];

			if (c >= 32 && c <= 126 && c != '\\' && c != '"') {
				name[used++] = (char)c;
			} else {
				used += (unsigned)snprintf(name + used, sizeof(name) - used, "\\x%02x", c);
			}
		}
		name[used] = 0;
		say("scsc: SCAN network BSSID %02x:%02x:%02x:%02x:%02x:%02x freq %u MHz RSSI %d SSID \"%s\"\n",
		    slot->bssid[0], slot->bssid[1], slot->bssid[2], slot->bssid[3], slot->bssid[4], slot->bssid[5],
		    slot->frequency, slot->rssi, name);
	}
}

static void
hip_drain(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	for (unsigned q = 2; q <= 4; q++) {
		for (unsigned n = 0; n < 128; n++) {
			uint8_t r = hip_index(h, q, 1), w = hip_index(h, q, 0);
			uint32_t ref;

			if (r == w) {
				break;
			}
			ref = ((volatile uint32_t *)h->q[q].array)[r];
			if (q == 2) {
				hip_slot_release_ref(ref);
				if (hip_wait.tx_ref != 0 && (ref == hip_wait.tx_ref || (ref & 0x00ffffffu) == hip_wait.tx_ref)) {
					hip_wait.tx_back = true;
					say("scsc: HIP4 TX buffer returned +%#x\n", ref);
				}
			} else {
				uint32_t refs[16];
				unsigned count, siglen, len;
				const uint8_t *s = NULL;
				uint8_t free_slots;
				uint16_t id;

				if (!hip_rx_collect(mem, ref, refs, &count, &s, &siglen, &len)) {
					if (hip_bad_log++ < 8) {
						say("scsc: HIP4 invalid RX chain +%#x q%u (kept, not returned): why %u a %#x b %#x c %#x\n", ref, q, hip_rx_why, hip_rx_a, hip_rx_b, hip_rx_c);
					}
					hip_index_set(h, q, 1, (uint8_t)(r + 1));
					continue;
				}
				free_slots = (uint8_t)(hip_index(h, 5, 1) - hip_index(h, 5, 0) - 1);
				if (count > free_slots) {
					break;
				}
				id = hip_u16(s);
				if (id == 0x8301) {
					hip_debug_inds++;
				} else if (hip_wait.want_cfm != 0 && id == hip_wait.want_cfm && siglen >= 14 &&
				    hip_u16(s + 2) == hip_wait.sender) {
					/* SEND_FRAME_CFM: vif, host_tag, result_code. The rest: result at +12. */
					hip_wait.cfm_seen = true;
					hip_wait.result = (id == 0x2110 && siglen >= 16) ? (int)hip_u16(s + 14) : (int)hip_u16(s + 12);
					hip_wait.cfm_ok = hip_wait.result == 0;
					say("scsc: HIP4 CFM id %#x result %d\n", id, hip_wait.result);
				} else if (id == 0x2300 && hip_wait.scan_id != 0 && siglen >= 18 && hip_u16(s + 16) == hip_wait.scan_id) {
					hip_bss_note(s, siglen, hip_rx_data, len);
				} else if (id == 0x2301 && hip_wait.scan_id != 0 && siglen >= 14 && hip_u16(s + 12) == hip_wait.scan_id) {
					hip_wait.scan_done = true;
					say("scsc: MLME_SCAN_DONE_IND scan_id %#x\n", hip_wait.scan_id);
				} else if (hip_rx_other(id, s, siglen, hip_rx_data, len)) {
					/* join, EAPOL, or a data frame */
				} else if (id != 0x8301 && hip_other_log < 12) {
					hip_other_log++;
					say("scsc: HIP4 RX q%u id %#x sig %u data %u\n", q, id, siglen, len);
				}
				for (unsigned i = 0; i < count; i++) {
					if (!hip_push(h, mbox, 5, refs[i])) {
						say("scsc: HIP4 TH_RFB refused a returned buffer\n");
						return;
					}
				}
			}
			hip_index_set(h, q, 1, (uint8_t)(r + 1));
		}
	}
}

static bool
hip_send_vif(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, uint32_t ref, uint16_t sender, bool add)
{
	uint8_t *b = (uint8_t *)(mem + ref), *f = b + 24;

	memset(b, 0, 64);
	b[4] = 1 | 4;
	b[5] = 5;
	b[6] = 1;
	hip_put16(b + 10, 44);
	hip_put16(b + 14, 44);
	hip_put16(f, add ? 0x2007 : 0x2008);
	hip_put16(f + 4, sender);
	hip_put16(f + 10, 1);
	if (add) {
		memcpy(f + 12, scsc_mac, 6);
		hip_put16(f + 18, 2);            /* FAPI_VIFTYPE_STATION */
		memset(f + 20, 0xff, 6);        /* device_address: broadcast, as slsi_connect() */
	}
	say("scsc: MLME_%s_VIF_REQ vif 1 sender %#x\n", add ? "ADD" : "DEL", sender);
	return hip_push(h, mbox, 0, ref);
}

static bool
hip_send_scan(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, uint32_t ref, uint16_t sender)
{
	const struct hip_cfg_common *c = hip_version == 4 ? &h->v4.c : &h->v5.c;
	unsigned n = hip_scan_n;
	unsigned payload = 19 + 7 + n * 3;
	uint16_t head = (uint16_t)(40 + c->unidat_headroom);
	uint16_t dat = (uint16_t)(payload + c->unidat_headroom + c->unidat_tailroom);
	uint8_t *b = (uint8_t *)(mem + ref), *f = b + 24, *d;
	uint8_t timing[19] = { 0xdd, 17, 0, 0x16, 0x32, 1, 1 };
	uint8_t channels[7];

	if (n == 0 || n > 11 || 20u + 40u + dat > HIP_SLOT_BYTES) {
		return false;
	}
	memset(b, 0, HIP_SLOT_BYTES);
	b[4] = 5;
	b[5] = 5;
	b[6] = 1;
	b[7] = 1;
	hip_put16(b + 8, dat);
	hip_put16(b + 10, 40);
	hip_put16(b + 12, (uint16_t)payload);
	hip_put16(b + 14, head);
	hip_put16(f, 0x2005);
	hip_put16(f + 4, sender);
	hip_put16(f + 12, HIP_SCAN_ID);
	hip_put16(f + 14, 2);
	memcpy(f + 16, scsc_mac, 6);
	hip_put16(f + 22, 4);
	d = b + 20 + head;
	memcpy(d, timing, sizeof(timing));
	channels[0] = 0xdd;
	channels[1] = (uint8_t)(5 + n * 3);
	channels[2] = 0;
	channels[3] = 0x16;
	channels[4] = 0x32;
	channels[5] = 1;
	channels[6] = 2;
	memcpy(d + 19, channels, sizeof(channels));
	for (unsigned i = 0; i < n; i++) {
		hip_put16(d + 26 + i * 3, (uint16_t)(2 * hip_scan_ch[i]));
		d[28 + i * 3] = 0;              /* active: country is set, so the chip may transmit */
	}
	say("scsc: MLME_ADD_SCAN_REQ active, %u channel(s), sender %#x, headroom %u\n", n, sender, c->unidat_headroom);
	return hip_push(h, mbox, 0, ref);
}

static int
hip_await(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, uint32_t ref, bool scan, unsigned ticks)
{
	hip_wait.tx_ref = ref;
	hip_wait.tx_back = false;
	hip_wait.cfm_seen = false;
	hip_wait.cfm_ok = false;
	hip_wait.scan_done = false;
	hip_wait.result = -1;
	unsigned since_cfm = 0;

	for (unsigned t = 0; t < ticks; t++) {
		hip_drain(mem, mbox, h);
		if (hip_wait.cfm_seen && hip_wait.tx_back && (!scan || hip_wait.scan_done || !hip_wait.cfm_ok)) {
			break;
		}
		/*
		 * Confirmed, its buffer not back yet: the firmware keeps a request's mbulk while it works on it (a CONNECT's
		 * until the connection is made; 10 November: CFM after 10 ms, the buffer not in 5 s, and the join was given up
		 * for that). Half a second more, then the confirmation stands; hip_drain() frees the slot when q2 returns it.
		 */
		if (!scan && hip_wait.cfm_seen && !hip_wait.tx_back && ++since_cfm >= 50) {
			break;
		}
		delay(10000);
	}
	hip_wait.tx_ref = 0;
	if (!hip_wait.cfm_seen || (!hip_wait.tx_back && scan)) {
		return ETIMEDOUT;
	}
	if (!hip_wait.cfm_ok) {
		return EIO;
	}
	if (scan && !hip_wait.scan_done) {
		return ETIMEDOUT;
	}
	return 0;
}

static int
hip_vif_cmd(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, bool add)
{
	int slot, err;
	uint16_t sender;
	uint32_t ref;

	if (add && hip_vif_v) {
		return 0;
	}
	if (!add && !hip_vif_v) {
		hip_phase_v = HAKIOS_STA_OFF;
		return 0;
	}
	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	hip_wait.want_cfm = add ? 0x2107 : 0x2108;
	hip_wait.sender = sender;
	hip_wait.scan_id = 0;
	if (!hip_send_vif(mem, mbox, h, ref, sender, add)) {
		hip_slot_busy[slot] = 0;
		hip_wait.want_cfm = 0;
		return EIO;
	}
	err = hip_await(mem, mbox, h, ref, false, 500);
	hip_wait.want_cfm = 0;
	if (err == 0) {
		hip_vif_v = add ? 1 : 0;
		hip_phase_v = add ? HAKIOS_STA_IDLE : HAKIOS_STA_OFF;
		if (!add) {
			hip_bss_n = 0;
			hip_joined_v = 0;
			hakios_net_link_state(false);
			memset(&hip_wpa, 0, sizeof(hip_wpa));
		}
	}
	say("scsc: MLME_%s_VIF %s\n", add ? "ADD" : "DEL", err == 0 ? "ok" : "failed");
	return err;
}

static int
hip_scan_cmd(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	int slot, err;
	uint16_t sender;
	uint32_t ref;

	if (!hip_vif_v) {
		return ENETDOWN;
	}
	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	{
		unsigned kept = hip_bss_n;

		hip_bss_n = 0;
		hip_phase_v = HAKIOS_STA_SCANNING;
		hip_wait.want_cfm = 0x2105;
		hip_wait.sender = sender;
		hip_wait.scan_id = HIP_SCAN_ID;
		if (!hip_send_scan(mem, mbox, h, ref, sender)) {
			hip_slot_busy[slot] = 0;
			hip_wait.want_cfm = 0;
			hip_wait.scan_id = 0;
			hip_phase_v = HAKIOS_STA_IDLE;
			hip_bss_n = kept;
			return EIO;
		}
		err = hip_await(mem, mbox, h, ref, true, 2000);
		hip_wait.want_cfm = 0;
		hip_wait.scan_id = 0;
		if (hip_phase_v == HAKIOS_STA_SCANNING) {
			hip_phase_v = HAKIOS_STA_IDLE;
		}
		if (err != 0 && hip_bss_n == 0) {
			hip_bss_n = kept;
		}
		hip_error_v = err;
		say("scsc: scan %s, %u network(s), debug indications %u\n", err == 0 ? "done" : "failed", hip_bss_n, hip_debug_inds);
		return err;
	}
}

static int
hip_call(int req)
{
	int status;

	lck_mtx_lock(&hip_lock);
	if (!hip_alive) {
		lck_mtx_unlock(&hip_lock);
		return ENXIO;
	}
	if (hip_req != 0 || hip_job != 0) {
		lck_mtx_unlock(&hip_lock);
		return EBUSY;
	}
	hip_req = req;
	hip_status = 0;
	lck_mtx_unlock(&hip_lock);
	lck_mtx_lock(&hip_lock);
	while (hip_status == 0) {
		lck_mtx_sleep(&hip_lock, LCK_SLEEP_DEFAULT, (event_t)&hip_status, THREAD_UNINT);
	}
	status = hip_status;
	hip_req = 0;
	lck_mtx_unlock(&hip_lock);
	return status < 0 ? -status : 0;
}

static int
hip_pull(void)
{
	int req = 0;

	lck_mtx_lock(&hip_lock);
	if (hip_job == 0 && hip_req != 0 && hip_status == 0) {
		hip_job = req = hip_req;
	}
	lck_mtx_unlock(&hip_lock);
	return req;
}

static void
hip_finish(int err)
{
	lck_mtx_lock(&hip_lock);
	hip_job = 0;
	hip_status = err == 0 ? 1 : -err;
	thread_wakeup((event_t)&hip_status);
	lck_mtx_unlock(&hip_lock);
}

static int
scsc_power(bool on)
{
	return hip_call(on ? HIP_REQ_ON : HIP_REQ_OFF);
}

static int
scsc_scan(const struct hakios_sta_scan *what)
{
	static const uint16_t all[11] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462 };
	uint16_t chosen[11];
	unsigned n = 0;

	if (what != NULL && what->frequency_count != 0) {
		for (unsigned i = 0; i < what->frequency_count && i < 64; i++) {
			bool known = false, dup = false;

			for (unsigned k = 0; k < 11; k++) {
				known = known || all[k] == what->frequencies[i];
			}
			if (!known) {
				continue;
			}
			for (unsigned k = 0; k < n; k++) {
				dup = dup || chosen[k] == what->frequencies[i];
			}
			if (!dup && n < 11) {
				chosen[n++] = what->frequencies[i];
			}
		}
		if (n == 0) {
			hip_error_v = EINVAL;
			return EINVAL;
		}
	} else {
		memcpy(chosen, all, sizeof(all));
		n = 11;
	}
	if (hip_joined_v) {
		say("scsc: sta scan: already joined, last list kept (%u)\n", hip_bss_n);
		return 0;
	}
	memcpy(hip_scan_ch, chosen, n * sizeof(chosen[0]));
	hip_scan_n = n;
	say("scsc: sta scan: %u channel(s), passive%s, a name of %u bytes\n", n,
	    what != NULL && !what->passive ? " (an active scan was asked)" : "",
	    what != NULL ? what->ssid_length : 0);
	return hip_call(HIP_REQ_SCAN);
}

static void
scsc_heard(void (*each)(const struct hakios_sta_bss *bss, void *context), void *context)
{
	for (unsigned i = 0; i < hip_bss_n; i++) {
		each(&hip_bss[i], context);
	}
}

static void
hip_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static int
hip_await_tx(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, uint32_t ref, unsigned ticks)
{
	hip_wait.tx_ref = ref;
	hip_wait.tx_back = false;
	for (unsigned t = 0; t < ticks; t++) {
		hip_drain(mem, mbox, h);
		if (hip_wait.tx_back) {
			break;
		}
		delay(10000);
	}
	hip_wait.tx_ref = 0;
	return hip_wait.tx_back ? 0 : ETIMEDOUT;
}

/* Control mbulk, or one with a payload placed after the negotiated headroom. Signal is the
 * 4-byte prefix plus the FAPI bytes, rounded up to 4. Does not push. */
static bool
hip_fill(vm_offset_t mem, struct hip_control *h, uint32_t ref, uint16_t id, uint16_t sender,
    unsigned fapi_len, const uint8_t *payload, unsigned plen, uint8_t **fapi)
{
	const struct hip_cfg_common *c = hip_version == 4 ? &h->v4.c : &h->v5.c;
	unsigned sig = (fapi_len + 4u + 3u) & ~3u;
	unsigned head = sig, dat = 0, room;
	uint8_t *b;

	if (plen != 0) {
		dat = plen + c->unidat_headroom + c->unidat_tailroom;
		head = sig + c->unidat_headroom;
	}
	room = 20u + sig + dat;
	if (fapi_len < 12 || room > HIP_SLOT_BYTES || plen > HAKIOS_NET_FRAME_MAX) {
		return false;
	}
	b = (uint8_t *)(mem + ref);
	memset(b, 0, room);
	b[4] = 1 | 4;
	b[5] = 5;
	b[6] = 1;
	if (plen != 0) {
		b[7] = 1;
		hip_put16(b + 8, (uint16_t)dat);
		hip_put16(b + 12, (uint16_t)plen);
	}
	hip_put16(b + 10, (uint16_t)sig);
	hip_put16(b + 14, (uint16_t)head);
	*fapi = b + 24;
	hip_put16(*fapi, id);
	hip_put16(*fapi + 4, sender);
	hip_put16(*fapi + 10, 1);
	if (plen != 0) {
		memcpy(b + 20 + head, payload, plen);
	}
	return true;
}

static int
hip_tx_wait(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, int slot, uint32_t ref,
    uint16_t sender, uint16_t cfm, unsigned ticks)
{
	int err;

	if (!hip_push(h, mbox, 0, ref)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	if (cfm != 0) {
		hip_wait.want_cfm = cfm;
		hip_wait.sender = sender;
		hip_wait.scan_id = 0;
		err = hip_await(mem, mbox, h, ref, false, ticks);
		hip_wait.want_cfm = 0;
	} else {
		err = hip_await_tx(mem, mbox, h, ref, ticks);
	}
	return err;
}

static const uint8_t *
hip_ie(const uint8_t *ie, unsigned len, uint8_t id)
{
	unsigned at = 0;

	while (at + 2 <= len) {
		unsigned n = ie[at + 1];

		if (n > len - (at + 2)) {
			break;
		}
		if (ie[at] == id) {
			return ie + at;
		}
		at += 2 + n;
	}
	return NULL;
}

/* 802.11g non-HT rates in 500 kbps, the default of UnifiSupportedDataRates (mib.h 0x07F9). */
static const uint8_t hip_g_rates[12] = { 2, 4, 11, 12, 18, 22, 24, 36, 48, 72, 96, 108 };

static unsigned
hip_bss_rates(const uint8_t *elements, unsigned elen, uint8_t *out, unsigned max)
{
	unsigned n = 0, k;
	const uint8_t *ie;
	uint8_t id;

	for (id = 1; id != 0; id = (id == 1) ? 50 : 0) {
		ie = hip_ie(elements, elen, id);
		if (ie == NULL) {
			continue;
		}
		for (k = 0; k < ie[1] && n < max; k++) {
			uint8_t r = ie[2 + k] & 0x7f;

			if (r >= 2 && r < 127) {
				out[n++] = r;
			}
		}
	}
	return n;
}

static int
hip_set_rates(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, const uint8_t *rates, unsigned n)
{
	uint8_t mib[32], *f;
	unsigned dlen, i;
	int slot, err;
	uint16_t sender;
	uint32_t ref;

	if (rates == NULL || n == 0 || n > 12) {
		rates = hip_g_rates;
		n = 12;
	}
	dlen = n + 1;
	mib[0] = 0xf9;
	mib[1] = 0x07;
	hip_put16(mib + 2, (uint16_t)(2 + dlen));
	mib[4] = 0x80 | 0x20 | 1;
	mib[5] = (uint8_t)dlen;
	mib[6] = (uint8_t)n;
	for (i = 0; i < n; i++) {
		mib[7 + i] = rates[i];
	}
	i = 7 + n;
	if (i & 1) {
		mib[i++] = 0;
	}
	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x2002, sender, 24, mib, i, &f)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	hip_put16(f + 10, 0);
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x2102, 300);
	say("scsc: MLME_SET UnifiSupportedDataRates %u rate(s): %d\n", n, err);
	return err;
}

static int
hip_set_bool(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, uint16_t psid, int on, const char *name)
{
	uint8_t mib[8], *f;
	int slot, err;
	uint16_t sender;
	uint32_t ref;

	mib[0] = (uint8_t)psid;
	mib[1] = (uint8_t)(psid >> 8);
	hip_put16(mib + 2, 1);
	mib[4] = on ? 1 : 0;
	mib[5] = 0;
	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x2002, sender, 24, mib, 6, &f)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	hip_put16(f + 10, 0);
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x2102, 300);
	say("scsc: MLME_SET %s %u: %d\n", name, on ? 1 : 0, err);
	return err;
}

static int
hip_add_info(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, const uint8_t *ie, unsigned len)
{
	uint8_t *f;
	int slot, err;
	uint16_t sender;
	uint32_t ref;

	if (len == 0) {
		return 0;
	}
	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x2004, sender, 26, ie, len, &f)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	hip_put16(f + 12, 0x0010);
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x2104, 300);
	say("scsc: MLME_ADD_INFO_ELEMENTS %u bytes: %d\n", len, err);
	return err;
}

static int
hip_register_action(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	uint8_t *f;
	int slot, err;
	uint16_t sender;
	uint32_t ref;

	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x200f, sender, 32, NULL, 0, &f)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	hip_put32(f + 12, 0x00020632u);
	hip_put32(f + 16, 0x00020612u);
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x210f, 300);
	say("scsc: MLME_REGISTER_ACTION_FRAME: %d\n", err);
	return err;
}

static int
hip_vif_reset(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	unsigned kept = hip_bss_n;
	int err;

	(void)hip_vif_cmd(mem, mbox, h, false);
	hip_bss_n = kept;
	err = hip_vif_cmd(mem, mbox, h, true);
	hip_bss_n = kept;
	return err;
}

/* The join request already prepared PMK, addresses, SNonce and RSN on the
 * station thread. DEL_VIF deliberately wipes the old handshake. Preserve only
 * this new request across that reset; never restore it when the reset fails. */
static int
hip_join_vif_reset(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	struct lx_wpa pending = hip_wpa;
	int err = hip_vif_reset(mem, mbox, h);
	if (err == 0) {
		hip_wpa = pending;
	} else {
		memset_s(&hip_wpa, sizeof(hip_wpa), 0, sizeof(hip_wpa));
	}
	memset_s(&pending, sizeof(pending), 0, sizeof(pending));
	return err;
}

/* NULL when the join may proceed. For WPA2, *group is the access point's group cipher. */
static const char *
hip_protection(const struct hakios_sta_bss *bss, bool want_wpa, unsigned *group)
{
	const uint8_t *rsn = hip_ie(bss->elements, bss->elements_length, 48);
	const uint8_t *at, *end;
	bool ccmp = false, psk = false;
	unsigned count;

	if (!want_wpa) {
		return ((bss->capability & 0x10) != 0 || rsn != NULL) ? "the network is protected" : NULL;
	}
	if (rsn == NULL) {
		return (bss->capability & 0x10) != 0 ? "it is protected by something older than WPA2" : "it is not protected";
	}
	if (rsn[1] < 8) {
		return "its RSN element is not one this reads";
	}
	at = rsn + 2;
	end = rsn + 2 + rsn[1];
	if ((unsigned)(end - at) < 8 || at[2] != 0x00 || at[3] != 0x0f || at[4] != 0xac) {
		return "its RSN element is not one this reads";
	}
	*group = at[5];
	count = at[6] | (unsigned)at[7] << 8;
	at += 8;
	for (; count != 0 && end - at >= 4; count--, at += 4) {
		ccmp = ccmp || (at[0] == 0x00 && at[1] == 0x0f && at[2] == 0xac && at[3] == 4);
	}
	if (end - at < 2) {
		return "its RSN element names no kind of key management";
	}
	count = at[0] | (unsigned)at[1] << 8;
	at += 2;
	for (; count != 0 && end - at >= 4; count--, at += 4) {
		psk = psk || (at[0] == 0x00 && at[1] == 0x0f && at[2] == 0xac && at[3] == 2);
	}
	if (*group != 4) {
		return "it ciphers frames to everybody with something other than CCMP";
	}
	if (!ccmp) {
		return "it does not offer CCMP";
	}
	if (!psk) {
		return "it does not take a WPA2 passphrase";
	}
	if (end - at >= 2 && (at[0] & 0x40) != 0) {
		return "it requires protected management frames";
	}
	return NULL;
}

static const struct hakios_sta_bss *
hip_bss_pick(const struct hakios_sta_join *what)
{
	const struct hakios_sta_bss *best = NULL;
	bool have_bssid = false;

	for (unsigned i = 0; i < 6; i++) {
		have_bssid = have_bssid || what->bssid[i] != 0;
	}
	for (unsigned i = 0; i < hip_bss_n; i++) {
		const struct hakios_sta_bss *b = &hip_bss[i];
		bool bssid_set = false;

		for (unsigned k = 0; k < 6; k++) {
			bssid_set = bssid_set || b->bssid[k] != 0;
		}
		if (!bssid_set) {
			continue;
		}
		if (have_bssid) {
			if (memcmp(b->bssid, what->bssid, 6) != 0) {
				continue;
			}
		} else if (b->ssid_length != what->ssid_length || memcmp(b->ssid, what->ssid, what->ssid_length) != 0) {
			continue;
		}
		if (best == NULL || b->rssi > best->rssi) {
			best = b;
		}
	}
	return best;
}

static bool
hip_bad_pass(const char *why)
{
	static const char mark[] = "its signature";

	if (why == NULL) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(mark) - 1; i++) {
		if (why[i] != mark[i]) {
			return false;
		}
	}
	return true;
}

static int
hip_set_key(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, bool pairwise)
{
	uint8_t *f, addr[6];
	const uint8_t *key;
	int slot, err;
	uint16_t sender, key_id;
	uint32_t ref;

	if (pairwise) {
		key = hip_wpa.pairwise;
		key_id = 0;
		memcpy(addr, hip_target.bssid, 6);
	} else {
		if (hip_wpa.group_length != 16) {
			say("scsc: group key is %u bytes, not CCMP\n", hip_wpa.group_length);
			return ENOTSUP;
		}
		key = hip_wpa.group;
		key_id = (uint16_t)hip_wpa.group_index;
		/* Samsung cfg80211_ops.c slsi_add_key: station GTK is associated
		 * with its AP peer, not the broadcast address (firmware rejects it). */
		memcpy(addr, hip_target.bssid, sizeof(addr));
	}
	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x2018, sender, 56, key, 16, &f)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	hip_put16(f + 12, 16 * 8);
	hip_put16(f + 14, key_id);
	hip_put16(f + 16, pairwise ? 1 : 0);
	memcpy(f + 18, addr, 6);
	if (!pairwise) {
		for (unsigned i = 0; i < 8; i += 2) {
			hip_put16(f + 24 + i, (uint16_t)(hip_wpa.group_counter[i] | (uint16_t)hip_wpa.group_counter[i + 1] << 8));
		}
	}
	hip_put32(f + 40, 0x000fac04);
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x2118, 300);
	say("scsc: SETKEYS %s id %u %s (cfm %d)\n", pairwise ? "pairwise" : "group", key_id,
	    err == 0 ? "ok" : "failed", hip_wait.result);
	return err;
}

static int
hip_send_eapol(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, const uint8_t *eapol, unsigned elen,
    uint16_t message_type, bool m4)
{
	uint8_t frame[14 + LX_WPA_REPLY_MOST], *f;
	int slot, err;
	uint16_t sender, tag;
	uint32_t ref;

	if (elen > LX_WPA_REPLY_MOST) {
		return EIO;
	}
	memcpy(frame, hip_target.bssid, 6);
	memcpy(frame + 6, scsc_mac, 6);
	frame[12] = 0x88;
	frame[13] = 0x8e;
	memcpy(frame + 14, eapol, elen);
	tag = hip_tag_next++;
	if (hip_tag_next == 0) {
		hip_tag_next = 1;
	}
	if (m4) {
		hip_m4_tag = tag;
		hip_m4_armed = true;
		hip_m4_acked = false;
		hip_m4_tx_back = false;
		hip_m4_status = -1;
	}
	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x2010, sender, 40, frame, 14 + elen, &f)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	hip_put16(f + 12, tag);
	hip_put16(f + 14, 1);
	hip_put16(f + 16, message_type);
	hip_put32(f + 20, 50000);               /* dwell: stay for M3 / retries */
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x2110, 300);
	if (m4 && err == 0) {
		hip_m4_tx_back = true;
	}
	say("scsc: EAPOL TX message %u, %u bytes, %s (cfm %d)\n", message_type, elen, err == 0 ? "sent" : "failed", err);
	return err;
}

static int
hip_eapol_work(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	while (hip_eapol_n != 0) {
		uint8_t body[384], reply[LX_WPA_REPLY_MOST];
		unsigned len = hip_eapol_q[0].len, reply_len = 0;
		const char *why = NULL;
		int kind, err;

		if (len > sizeof(body)) {
			len = sizeof(body);
		}
		memcpy(body, hip_eapol_q[0].body, len);
		for (unsigned i = 1; i < hip_eapol_n; i++) {
			hip_eapol_q[i - 1] = hip_eapol_q[i];
		}
		hip_eapol_n--;
		if (!hip_target.wpa) {
			say("scsc: EAPOL dropped (open network)\n");
			continue;
		}
		kind = lx_wpa_message(&hip_wpa, body, len, reply, &reply_len, &why);
		if (kind == LX_WPA_NOTHING) {
			continue;
		}
		if (kind == LX_WPA_BAD) {
			say("scsc: handshake: %s\n", why != NULL ? why : "rejected");
			return hip_bad_pass(why) ? EAUTH : EIO;
		}
		say("scsc: handshake %s, reply %u bytes\n", kind == LX_WPA_ANSWER ? "message 1" : kind == LX_WPA_KEYS ?
		    "message 3" : "group key", reply_len);
		if (kind == LX_WPA_KEYS) {
			err = hip_set_key(mem, mbox, h, true);
			if (err != 0) {
				return err;
			}
		}
		if (kind == LX_WPA_KEYS || kind == LX_WPA_GROUP) {
			err = hip_set_key(mem, mbox, h, false);
			if (err != 0) {
				return err;
			}
		}
		err = hip_send_eapol(mem, mbox, h, reply, reply_len, kind == LX_WPA_KEYS ? 3 : 2, kind == LX_WPA_KEYS);
		if (err != 0) {
			return err;
		}
	}
	return 0;
}

static int
hip_connect_res(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	uint8_t *f;
	int slot, err;
	uint16_t sender;
	uint32_t ref;

	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x2200, sender, 24, NULL, 0, &f)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0, 300);
	say("scsc: MLME_CONNECT_RES %s\n", err == 0 ? "sent" : "failed");
	return err;
}

static int
hip_disconnect(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, unsigned ticks)
{
	uint8_t *f;
	int slot, err;
	uint16_t sender;
	uint32_t ref;

	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x200e, sender, 32, NULL, 0, &f)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	memcpy(f + 12, hip_target.bssid, 6);
	hip_put16(f + 18, 3);
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x210e, ticks);
	say("scsc: MLME_DISCONNECT %s\n", err == 0 ? "ok" : "not confirmed");
	return err;
}

static void
hip_join_clear(void)
{
	hip_joined_v = 0;
	hip_m4_armed = false;
	hip_phase_v = hip_vif_v ? HAKIOS_STA_IDLE : HAKIOS_STA_OFF;
	memset(&hip_wpa, 0, sizeof(hip_wpa));
	hakios_net_link_state(false);
}

static int
hip_join_fail(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, int err, bool ind_ok)
{
	/*
	 * DISCONNECT after CONNECT_IND NOT_ALLOWED made the firmware discard it (fault 0x2c76 arg 0x200e,
	 * CFM 0xFF50) and every later scan timed out (10 November). Recreate the interface instead, as
	 * slsi_connect() does on a failed connect (del_vif + add_vif). DISCONNECT only if CONNECT_IND
	 * itself succeeded and the handshake then failed.
	 */
	if (ind_ok) {
		(void)hip_disconnect(mem, mbox, h, 100);
	}
	hip_join_clear();
	(void)hip_vif_reset(mem, mbox, h);
	hip_error_v = err;
	say("scsc: join failed (%d)\n", err);
	log_dump(mem, hip_panic_off);           /* what the firmware itself says of it (host-tools/mxlog-decode.py) */
	return err;
}

static int
hip_join_up(void)
{
	hip_joined_v = 1;
	hip_phase_v = HAKIOS_STA_JOINED;
	hip_error_v = 0;
	hakios_net_link_state(true);
	say("scsc: joined (%s), link up\n", hip_target.wpa ? "WPA2" : "open");
	return 0;
}

static int
hip_join_cmd(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	uint8_t body[2 + 32 + 22], *f;
	unsigned n = 0, quiet = 0;
	int slot, err;
	uint16_t sender;
	uint32_t ref;
	bool ind_ok = false;

	hip_eapol_n = 0;
	hip_ind_seen = false;
	hip_ind_code = -1;
	hip_lost = false;
	hip_m4_armed = false;
	hip_m4_tx_back = false;
	hip_m4_acked = false;
	hip_joined_v = 0;
	/*
	 * The interface made anew for the join, as Samsung's slsi_connect() makes it (slsi_mlme_add_vif() right before
	 * slsi_mlme_connect(); its scans need no interface). Then the rates the firmware intersected as empty
	 * (DEBUG_FAULT 0x2c1f MLME_VIFCTRL_NO_VALID_RATES_INTERSECTION, CONNECT_IND 48) and the action-frame bitmap.
	 */
	log_dump(mem, hip_panic_off);
	err = hip_join_vif_reset(mem, mbox, h);
	if (err != 0) {
		return hip_join_fail(mem, mbox, h, err, false);
	}
	{
		uint8_t rates[12], info[128];
		unsigned nr, infon = 0;
		const uint8_t *ie;

		{
			uint8_t heard[12];
			unsigned nh, i, j;

			nh = hip_bss_rates(hip_target.elements, hip_target.elements_length, heard, 12);
			nr = 12;
			memcpy(rates, hip_g_rates, 12);
			for (i = 0; i < nh && nr < 12; i++) {
				for (j = 0; j < nr; j++) {
					if (rates[j] == heard[i]) {
						break;
					}
				}
				if (j == nr) {
					rates[nr++] = heard[i];
				}
			}
			{
				char line[160];
				unsigned o = (unsigned)snprintf(line, sizeof(line), "scsc: access point rates %u, station %u:", nh, nr);

				for (i = 0; i < nr && o + 8u < sizeof(line); i++) {
					o += (unsigned)snprintf(line + o, sizeof(line) - o, " %u", rates[i]);
				}
				say("%s\n", line);
			}
		}
		(void)hip_set_rates(mem, mbox, h, rates, nr);
		(void)hip_set_bool(mem, mbox, h, 0x07fe, 1, "UnifiHtActivated");
		ie = hip_ie(hip_target.elements, hip_target.elements_length, 1);
		if (ie != NULL && infon + 2u + ie[1] <= sizeof(info)) {
			memcpy(info + infon, ie, 2u + ie[1]);
			infon += 2u + ie[1];
		}
		ie = hip_ie(hip_target.elements, hip_target.elements_length, 50);
		if (ie != NULL && infon + 2u + ie[1] <= sizeof(info)) {
			memcpy(info + infon, ie, 2u + ie[1]);
			infon += 2u + ie[1];
		}
		ie = hip_ie(hip_target.elements, hip_target.elements_length, 45);
		if (ie != NULL && infon + 2u + ie[1] <= sizeof(info)) {
			memcpy(info + infon, ie, 2u + ie[1]);
			infon += 2u + ie[1];
		}
		if (hip_target.wpa && infon + 22u <= sizeof(info)) {
			memcpy(info + infon, hip_target.rsn, 22);
			infon += 22;
		}
		(void)hip_add_info(mem, mbox, h, info, infon);
		(void)hip_register_action(mem, mbox, h);
	}
	hip_phase_v = HAKIOS_STA_JOINING;
	body[n++] = 0;
	body[n++] = hip_target.ssid_length;
	memcpy(body + n, hip_target.ssid, hip_target.ssid_length);
	n += hip_target.ssid_length;
	if (hip_target.wpa) {
		memcpy(body + n, hip_target.rsn, 22);
		n += 22;
	}
	slot = hip_slot_get();
	if (slot < 0) {
		return hip_join_fail(mem, mbox, h, EAGAIN, false);
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x200b, sender, 34, body, n, &f)) {
		hip_slot_busy[slot] = 0;
		return hip_join_fail(mem, mbox, h, EIO, false);
	}
	memcpy(f + 12, hip_target.bssid, 6);
	hip_put16(f + 18, 0);
	hip_put16(f + 20, (uint16_t)(2 * hip_target.frequency));
	say("scsc: MLME_CONNECT_REQ BSSID %02x:%02x:%02x:%02x:%02x:%02x %s freq %u sender %#x\n",
	    hip_target.bssid[0], hip_target.bssid[1], hip_target.bssid[2], hip_target.bssid[3], hip_target.bssid[4],
	    hip_target.bssid[5], hip_target.wpa ? "WPA2" : "open", hip_target.frequency, sender);
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x210b, 500);
	if (err != 0 || hip_lost) {
		return hip_join_fail(mem, mbox, h, hip_lost ? ECONNRESET : err, false);
	}
	ind_ok = false;
	for (unsigned t = 0; t < 2000; t++) {
		hip_drain(mem, mbox, h);
		if (hip_lost) {
			return hip_join_fail(mem, mbox, h, ECONNRESET, ind_ok);
		}
		if (hip_ind_seen) {
			int code = hip_ind_code;

			hip_ind_seen = false;
			if (code != 0) {
				say("scsc: CONNECT_IND result %d%s\n", code, code == 48 ? " (NOT_ALLOWED)" : "");
				return hip_join_fail(mem, mbox, h, EIO, false);
			}
			say("scsc: CONNECT_IND success\n");
			ind_ok = true;
			/* WPA completes CONNECT only after the four-way handshake, as Samsung
			 * does. scsc23's early RES did not repair the wiped WPA context. */
			if (!hip_target.wpa) {
				err = hip_connect_res(mem, mbox, h);
				return err == 0 ? hip_join_up() : hip_join_fail(mem, mbox, h, err, true);
			}
		}
		err = hip_eapol_work(mem, mbox, h);
		if (err != 0) {
			return hip_join_fail(mem, mbox, h, err, ind_ok);
		}
		if (hip_m4_tx_back) {
			if (hip_m4_acked && hip_m4_status == 0) {
				err = hip_connect_res(mem, mbox, h);
				return err == 0 ? hip_join_up() : hip_join_fail(mem, mbox, h, err, true);
			}
			if (hip_m4_acked && hip_m4_status != 0) {
				say("scsc: M4 transmission failed (%d)\n", hip_m4_status);
				return hip_join_fail(mem, mbox, h, EIO, true);
			}
			if (++quiet >= 200) {
				say("scsc: no transmission indication\n");
				err = hip_connect_res(mem, mbox, h);
				return err == 0 ? hip_join_up() : hip_join_fail(mem, mbox, h, err, true);
			}
		}
		delay(10000);
	}
	say("scsc: join timed out\n");
	return hip_join_fail(mem, mbox, h, ETIMEDOUT, ind_ok);
}

static int
hip_leave_cmd(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	if (hip_joined_v || hip_phase_v == HAKIOS_STA_JOINED) {
		(void)hip_disconnect(mem, mbox, h, 100);
	}
	hip_join_clear();
	hip_error_v = 0;
	say("scsc: left the network\n");
	return 0;
}

static uint16_t
hip_eth_kind(const uint8_t *f, unsigned len)
{
	unsigned ihl, sp, dp;

	if (len >= 14 && f[12] == 0x08 && f[13] == 0x06) {
		return 4;                       /* FAPI_MESSAGETYPE_ARP */
	}
	if (len < 38 || f[12] != 0x08 || f[13] != 0x00 || (f[14] & 0xf0) != 0x40) {
		return 0;
	}
	ihl = (unsigned)(f[14] & 0x0f) * 4;
	if (ihl < 20 || f[23] != 17 || 14 + ihl + 4 > len) {
		return 0;
	}
	sp = ((unsigned)f[14 + ihl] << 8) | f[15 + ihl];
	dp = ((unsigned)f[16 + ihl] << 8) | f[17 + ihl];
	if ((sp == 68 && dp == 67) || (sp == 67 && dp == 68)) {
		return 5;                       /* FAPI_MESSAGETYPE_DHCP */
	}
	return 0;
}

/*
 * The access point ACKs the lease unicast before Darwin has that address (STATE_INIT REQUEST).
 * ip_input then drops the packet. Rewrite as Ethernet/IP broadcast so UDP :68 is delivered.
 */
static void
hip_dhcp_bcast(uint8_t *f, unsigned len)
{
	unsigned ihl, i, sum;
	uint8_t *ip, *udp;
	uint16_t dp;

	if (len < 42 || f[12] != 0x08 || f[13] != 0x00 || (f[14] & 0xf0) != 0x40) {
		return;
	}
	ihl = (unsigned)(f[14] & 0x0f) * 4;
	if (ihl < 20 || f[23] != 17 || 14 + ihl + 8 > len) {
		return;
	}
	ip = f + 14;
	udp = ip + ihl;
	dp = ((unsigned)udp[2] << 8) | udp[3];
	if (dp != 68) {
		return;
	}
	if (ip[16] == 255 && ip[17] == 255 && ip[18] == 255 && ip[19] == 255) {
		say("scsc: DHCP RX %u bytes broadcast\n", len);
		return;
	}
	/* Only the AP's unicast ACK to us. A foreign lease (seen on one run) must not be broadcast in. */
	if (memcmp(f, scsc_mac, 6) != 0) {
		return;
	}
	say("scsc: DHCP RX %u bytes dest %u.%u.%u.%u -> broadcast\n", len, ip[16], ip[17], ip[18], ip[19]);
	memset(f, 0xff, 6);
	ip[16] = ip[17] = ip[18] = ip[19] = 255;
	ip[10] = ip[11] = 0;
	sum = 0;
	for (i = 0; i < ihl; i += 2) {
		sum += ((unsigned)ip[i] << 8) | ip[i + 1];
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	sum = ~sum & 0xffff;
	ip[10] = (uint8_t)(sum >> 8);
	ip[11] = (uint8_t)sum;
	udp[6] = udp[7] = 0;
}

static uint8_t hip_rx_copy[HAKIOS_NET_FRAME_MAX];

static void
hip_net_in(const uint8_t *data, unsigned len)
{
	if (len < 14 || len > HAKIOS_NET_FRAME_MAX) {
		return;
	}
	memcpy(hip_rx_copy, data, len);
	hip_dhcp_bcast(hip_rx_copy, len);
	hakios_net_input(hip_rx_copy, len);
}

/*
 * DHCP and ARP go through MLME_SEND_FRAME (tx.c slsi_tx_dhcp / slsi_tx_arp), not MA_UNITDATA: without that the
 * firmware associated an open network (the access point saw our MAC) and Darwin never got an address (10 November).
 */
static int
hip_send_eth(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, const uint8_t *frame, unsigned len,
    uint16_t message_type)
{
	uint8_t *f;
	int slot, err;
	uint16_t sender, tag;
	uint32_t ref;

	slot = hip_slot_get();
	if (slot < 0) {
		return EAGAIN;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	tag = hip_tag_next++;
	if (hip_tag_next == 0) {
		hip_tag_next = 1;
	}
	if (!hip_fill(mem, h, ref, 0x2010, sender, 40, frame, len, &f)) {
		hip_slot_busy[slot] = 0;
		return EIO;
	}
	hip_put16(f + 12, tag);
	hip_put16(f + 14, 1);
	hip_put16(f + 16, message_type);
	hip_put32(f + 20, 100000);              /* dwell, microseconds: hear the offer / ARP reply */
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x2110, 300);
	say("scsc: SEND_FRAME type %u, %u bytes eth %02x%02x: %d\n", message_type, len, frame[12], frame[13], err);
	return err;
}

static int
hip_tx_cmd(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	uint8_t *f;
	unsigned len = hip_tx_len;
	int slot, err = ENETDOWN;
	uint16_t sender, tag, kind;
	uint32_t ref;

	if (!hip_joined_v || len < 14 || len > HAKIOS_NET_FRAME_MAX) {
		goto out;
	}
	hip_drain(mem, mbox, h);
	kind = hip_eth_kind(hip_tx_frame, len);
	if (kind != 0) {
		err = hip_send_eth(mem, mbox, h, hip_tx_frame, len, kind);
		goto out;
	}
	slot = hip_slot_get();
	if (slot < 0) {
		err = EAGAIN;
		goto out;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	tag = hip_tag_next++;
	if (hip_tag_next == 0) {
		hip_tag_next = 1;
	}
	if (!hip_fill(mem, h, ref, 0x1000, sender, 34, hip_tx_frame, len, &f)) {
		hip_slot_busy[slot] = 0;
		err = EIO;
		goto out;
	}
	hip_put16(f + 12, tag);
	hip_put16(f + 14, 0x8000);
	hip_put16(f + 16, 1);
	hip_put16(f + 18, 1);
	/*
	 * Do not wait for the TX buffer: that serialized every TCP segment on HIP (scsc31 RTT hundreds of ms).
	 * The slot is freed when q2 returns the ref in hip_drain.
	 */
	if (!hip_push(h, mbox, 0, ref)) {
		hip_slot_busy[slot] = 0;
		err = EIO;
	} else {
		err = 0;
	}
	if (hip_tx_said < 6 || (hip_tx_said % 64) == 0) {
		say("scsc: data TX %u bytes type %02x%02x %s\n", len, hip_tx_frame[12], hip_tx_frame[13], err == 0 ? "queued" : "failed");
	}
	hip_tx_said++;
out:
	hip_tx_busy = 0;
	return err;
}

static bool
hip_rx_other(uint16_t id, const uint8_t *s, unsigned siglen, const uint8_t *data, unsigned len)
{
	if (id == 0x8302 && siglen >= 28) {
		/* DEBUG_FAULT_IND (fapi.h debug_fault_ind): vif, faultid, count, timestamp, arg, cpu (0x8000 MAC, 0x4000 PHY) */
		{
			uint16_t fid = hip_u16(s + 12);
			const char *name = fid == 0x2c1f ? " NO_VALID_RATES_INTERSECTION" :
			    fid == 0x2551 ? " CONNECTION_ATTEMPT_ABORTED" :
			    fid == 0x2c76 ? " INVALID_SIGNAL_DISCARDED" : "";

			say("scsc: FIRMWARE FAULT id %#x%s count %u time %#x arg %#x cpu %#x seq %u\n", fid, name, hip_u16(s + 14),
			    (unsigned)(hip_u16(s + 16) | hip_u16(s + 18) << 16), (unsigned)(hip_u16(s + 20) | hip_u16(s + 22) << 16),
			    hip_u16(s + 24), hip_u16(s + 26));
		}
		return true;
	}
	if (id == 0x2303 && siglen >= 14) {
		hip_ind_seen = true;
		hip_ind_code = (int)hip_u16(s + 12);
		return true;
	}
	if (id == 0x2304) {
		say("scsc: CONNECTED_IND (station join does not use it)\n");
		return true;
	}
	if (id == 0x230a && siglen >= 16) {
		say("scsc: procedure started type %u aid %u\n", hip_u16(s + 12), hip_u16(s + 14));
		return true;
	}
	if (id == 0x230c && siglen >= 16) {
		uint16_t tag = hip_u16(s + 12), status = hip_u16(s + 14);

		if (hip_m4_armed && tag == hip_m4_tag) {
			hip_m4_acked = true;
			hip_m4_status = status;
			say("scsc: M4 transmission status %u\n", status);
		}
		return true;
	}
	if (id == 0x2308 || id == 0x2309) {
		bool was = hip_joined_v != 0;

		say("scsc: disconnect indication %#x\n", id);
		hip_lost = true;
		hip_joined_v = 0;
		if (hip_phase_v == HAKIOS_STA_JOINED || hip_phase_v == HAKIOS_STA_JOINING) {
			hip_phase_v = hip_vif_v ? HAKIOS_STA_IDLE : HAKIOS_STA_OFF;
		}
		if (was) {
			memset(&hip_wpa, 0, sizeof(hip_wpa));
			hakios_net_link_state(false);
			hakios_sta_lost(ECONNRESET);
		}
		return true;
	}
	if (id == 0x1100 || id == 0x2110) {
		if (siglen >= 14 && hip_u16(s + 12) != 0 && hip_cfm_said++ < 6) {
			say("scsc: TX confirm id %#x status %u\n", id, hip_u16(s + 12));
		}
		return true;
	}
	if (id == 0x1301 || id == 0x2313) {
		return true;                    /* MA_BLOCKACK_IND, MLME_AC_PRIORITY_UPDATE_IND */
	}
	if (id == 0x230d && siglen >= 16) {
		/*
		 * MLME_RECEIVED_FRAME_IND: EAPOL and DHCP (Samsung rx.c slsi_rx_received_frame_ind).
		 * Discover went out (the access point offered); we only kept 0x888e and dropped the offer.
		 */
		uint16_t desc = hip_u16(s + 12);

		if (desc == 1 && len >= 14 && data[12] == 0x88 && data[13] == 0x8e) {
			unsigned elen = len - 14;

			say("scsc: received frame descriptor %u, %u bytes\n", desc, len);
			if (elen > sizeof(hip_eapol_q[0].body) || hip_eapol_n >= HIP_EAPOL_MOST) {
				say("scsc: EAPOL dropped (%u bytes, queue %u)\n", elen, hip_eapol_n);
				return true;
			}
			memcpy(hip_eapol_q[hip_eapol_n].body, data + 14, elen);
			hip_eapol_q[hip_eapol_n].len = elen;
			hip_eapol_n++;
			say("scsc: EAPOL RX %u bytes (received-frame)\n", elen);
			return true;
		}
		if (desc == 1 && hip_joined_v && len >= 14 && len <= HAKIOS_NET_FRAME_MAX) {
			if (hip_rx_said < 8 || (hip_rx_said % 16) == 0) {
				say("scsc: data RX %u bytes type %02x%02x (received-frame)\n", len, data[12], data[13]);
			}
			hip_rx_said++;
			hip_net_in(data, len);
		}
		return true;
	}
	if (id == 0x1300 && siglen >= 24) {
		unsigned skip = 0;
		uint16_t desc = hip_u16(s + 12);
		uint16_t extra = hip_u16(s + 20);

		if (desc != 1) {
			if (hip_rx_said++ < 6) {
				say("scsc: data indication descriptor %u, %u bytes\n", desc, len);
			}
			return true;
		}
		if (extra != 0 && extra < len) {
			skip = extra;
		}
		if (len < skip + 14) {
			return true;
		}
		data += skip;
		len -= skip;
		if (data[12] == 0x88 && data[13] == 0x8e) {
			unsigned elen = len - 14;

			if (elen > sizeof(hip_eapol_q[0].body) || hip_eapol_n >= HIP_EAPOL_MOST) {
				say("scsc: EAPOL dropped (%u bytes, queue %u)\n", elen, hip_eapol_n);
				return true;
			}
			memcpy(hip_eapol_q[hip_eapol_n].body, data + 14, elen);
			hip_eapol_q[hip_eapol_n].len = elen;
			hip_eapol_n++;
			say("scsc: EAPOL RX %u bytes\n", elen);
			return true;
		}
		if (hip_joined_v && len <= HAKIOS_NET_FRAME_MAX) {
			if (hip_rx_said < 6 || (hip_rx_said % 32) == 0) {
				say("scsc: data RX %u bytes type %02x%02x\n", len, data[12], data[13]);
			}
			hip_rx_said++;
			hip_net_in(data, len);
		}
		return true;
	}
	return false;
}

static int
scsc_join(const struct hakios_sta_join *what)
{
	const struct hakios_sta_bss *bss;
	const char *why;
	unsigned group = 4;
	bool want_wpa;
	uint8_t pmk[32];
	int err;

	if (what == NULL || what->ssid_length == 0 || what->ssid_length > 32) {
		hip_error_v = EINVAL;
		return EINVAL;
	}
	bss = hip_bss_pick(what);
	if (bss == NULL) {
		say("scsc: sta join: the network is not in the last scan\n");
		hip_error_v = ENOENT;
		return ENOENT;
	}
	if (bss->ssid_length != 0 && (bss->ssid_length != what->ssid_length || memcmp(bss->ssid, what->ssid, what->ssid_length) != 0)) {
		say("scsc: sta join: the name does not match the beacon\n");
		hip_error_v = ENOENT;
		return ENOENT;
	}
	want_wpa = what->security == HAKIOS_STA_WPA2_PSK;
	why = hip_protection(bss, want_wpa, &group);
	if (why != NULL) {
		say("scsc: sta join refused: %s\n", why);
		hip_error_v = ENOTSUP;
		return ENOTSUP;
	}
	memset(&hip_target, 0, sizeof(hip_target));
	memcpy(hip_target.bssid, bss->bssid, 6);
	memcpy(hip_target.ssid, what->ssid, what->ssid_length);
	hip_target.ssid_length = what->ssid_length;
	hip_target.frequency = bss->frequency;
	hip_target.rssi = bss->rssi;
	hip_target.beacon_interval = bss->beacon_interval;
	hip_target.capability = bss->capability;
	hip_target.wpa = want_wpa;
	hip_target.elements_length = bss->elements_length;
	if (hip_target.elements_length > HAKIOS_STA_ELEMENTS_MOST) {
		hip_target.elements_length = HAKIOS_STA_ELEMENTS_MOST;
	}
	memcpy(hip_target.elements, bss->elements, hip_target.elements_length);
	memset(&hip_wpa, 0, sizeof(hip_wpa));
	if (want_wpa) {
		static const uint8_t tmpl[22] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };

		memcpy(hip_target.rsn, tmpl, 22);
		hip_target.rsn[7] = (uint8_t)group;
		if (what->passphrase_length != 0) {
			if (what->passphrase_length < 8 || what->passphrase_length > 63) {
				hip_error_v = EINVAL;
				return EINVAL;
			}
			say("scsc: sta join deriving the key (%u bytes)\n", what->passphrase_length);
			lx_wpa_pmk(what->passphrase, what->passphrase_length, what->ssid, what->ssid_length, pmk);
			memcpy(hip_wpa.pmk, pmk, 32);
			memset(pmk, 0, sizeof(pmk));
		} else {
			memcpy(hip_wpa.pmk, what->pmk, 32);
		}
		memcpy(hip_wpa.own, scsc_mac, 6);
		memcpy(hip_wpa.peer, hip_target.bssid, 6);
		read_random(hip_wpa.own_nonce, 32);
		hip_wpa.rsn = hip_target.rsn;
		hip_wpa.rsn_length = 22;
		{
			unsigned any = 0, i;

			for (i = 0; i < 32; i++) {
				any |= hip_wpa.pmk[i];
			}
			say("scsc: sta join PMK %s, STA %02x:%02x:%02x:..\n", any != 0 ? "present" : "all zeros",
			    scsc_mac[0], scsc_mac[1], scsc_mac[2]);
		}
	}
	say("scsc: sta join BSSID %02x:%02x:%02x:%02x:%02x:%02x %s, ssid %u bytes, passphrase %u bytes\n",
	    hip_target.bssid[0], hip_target.bssid[1], hip_target.bssid[2], hip_target.bssid[3], hip_target.bssid[4],
	    hip_target.bssid[5], want_wpa ? "WPA2" : "open", what->ssid_length, what->passphrase_length);
	err = hip_call(HIP_REQ_JOIN);
	hip_error_v = err;
	return err;
}

static int
scsc_leave(void)
{
	if (!hip_joined_v && hip_phase_v != HAKIOS_STA_JOINED) {
		return 0;
	}
	return hip_call(HIP_REQ_LEAVE);
}

static void
scsc_state(struct hakios_sta_state *state)
{
	memset(state, 0, sizeof(*state));
	state->phase = (enum hakios_sta_phase)hip_phase_v;
	state->last_error = hip_error_v;
	if (state->phase == HAKIOS_STA_JOINED || state->phase == HAKIOS_STA_JOINING) {
		memcpy(state->bss.bssid, hip_target.bssid, 6);
		memcpy(state->bss.ssid, hip_target.ssid, hip_target.ssid_length);
		state->bss.ssid_length = hip_target.ssid_length;
		state->bss.frequency = hip_target.frequency;
		state->bss.rssi = hip_target.rssi;
		state->bss.beacon_interval = hip_target.beacon_interval;
		state->bss.capability = hip_target.capability;
	}
}

static unsigned
scsc_channels(struct hakios_sta_channel *channels, unsigned most)
{
	unsigned n = 0;

	for (unsigned i = 0; i < 11 && n < most; i++) {
		channels[n++] = (struct hakios_sta_channel){ .frequency = (uint16_t)(2412 + i * 5), .flags = HAKIOS_STA_CHANNEL_PASSIVE };
	}
	return n;
}

/*
 * Frames from the stack wait here for the HIP thread, which sends them one after the other. Before, a frame was handed to the
 * thread by a one-job rendezvous: a second sender found it busy and its frame was dropped (3500 of 17000 in a run), and each
 * handover waited for the thread's 10 ms nap.
 */
#define HIP_TXQ 128
static struct { uint16_t length; uint8_t bytes[HAKIOS_NET_FRAME_MAX]; } hip_txq[HIP_TXQ];
static unsigned hip_txq_head, hip_txq_tail;           /* both under hip_lock */

static bool
scsc_transmit(mbuf_t packet, size_t length)
{
	bool queued = false;

	if (!hip_joined_v || packet == NULL || length < 14 || length > HAKIOS_NET_FRAME_MAX) {
		return false;
	}
	lck_mtx_lock(&hip_lock);
	if (hip_alive && hip_txq_tail - hip_txq_head < HIP_TXQ) {
		unsigned at = hip_txq_tail % HIP_TXQ;

		if (mbuf_copydata(packet, 0, length, hip_txq[at].bytes) == 0) {
			hip_txq[at].length = (uint16_t)length;
			hip_txq_tail++;
			queued = true;
		}
	}
	lck_mtx_unlock(&hip_lock);
	if (!queued && hip_tx_said < 8) {
		say("scsc: data TX %u bytes not queued\n", (unsigned)length);
		hip_tx_said++;
	}
	return queued;
}

/* The HIP thread: send what is queued; false when the head had to wait (no free slot). */
static bool
hip_txq_run(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h, unsigned * sent_n)
{
	for (unsigned budget = 0; budget < 32; budget++) {
		int err;

		lck_mtx_lock(&hip_lock);
		if (hip_txq_head == hip_txq_tail) {
			lck_mtx_unlock(&hip_lock);
			return true;
		}
		unsigned at = hip_txq_head % HIP_TXQ;

		hip_tx_len = hip_txq[at].length;
		memcpy(hip_tx_frame, hip_txq[at].bytes, hip_tx_len);
		lck_mtx_unlock(&hip_lock);
		err = hip_tx_cmd(mem, mbox, h);
		if (err == EAGAIN) {
			return false;
		}
		lck_mtx_lock(&hip_lock);
		hip_txq_head++;
		lck_mtx_unlock(&hip_lock);
		(*sent_n)++;
	}
	return true;
}

static struct hakios_net_link scsc_link = {
	.name = "the SCSC S620",
	.wireless = true,
	.transmit = scsc_transmit,
};

static struct hakios_sta_ops scsc_ops = {
	.power = scsc_power,
	.scan = scsc_scan,
	.heard = scsc_heard,
	.join = scsc_join,
	.leave = scsc_leave,
	.state = scsc_state,
	.channels = scsc_channels,
};

static bool
scsc_mac_take(const char *from, const char *text, size_t length)
{
	uint8_t addr[6];

	if (text == NULL || hakios_macaddr_parse(text, length, addr) == 0) {
		return false;
	}
	memcpy(scsc_mac, addr, 6);
	say("scsc: the phone's address, from %s: %02x:%02x:%02x:..:..:..\n", from, scsc_mac[0], scsc_mac[1], scsc_mac[2]);
	return true;
}

/*
 * The A50's own Wi-Fi address, as Samsung's driver reads it (mgt.c slsi_get_hw_mac_address()): boot-arg wifi_mac,
 * then fw/mac.info of the small image (the phone's /efs/wifi/.mac.info), then that file on the EFS partition itself.
 * Only the maker's three bytes are said. The locally-administered 02:50:00:00:00:01 is diagnostic; the access point
 * already associated it (10 November), and Darwin then had no IP.
 */
static void
scsc_mac_load(void)
{
	char text[32] = "";
	const uint8_t *file = NULL;
	uint64_t size = 0;
	static const char *const parts[] = { "efs", "EFS", "sec_efs", "SEC_EFS", NULL };

	if (PE_parse_boot_arg_str("wifi_mac", text, sizeof(text)) && text[0] != 0) {
		if (scsc_mac_take("boot-arg wifi_mac", text, strlen(text))) {
			return;
		}
		say("scsc: wifi_mac is not an address\n");
	}
	file = hakios_image_file("fw", "mac.info", &size, NULL);
	if (file != NULL && scsc_mac_take("fw/mac.info", (const char *)file, size > 64 ? 64 : (size_t)size)) {
		return;
	}
	for (unsigned i = 0; parts[i] != NULL; i++) {
		if (hakios_partition_file(parts[i], "/efs", "/wifi/.mac.info", 64, &file, &size) == 0 &&
		    file != NULL && scsc_mac_take("EFS /wifi/.mac.info", (const char *)file, (size_t)size)) {
			return;
		}
	}
	say("scsc: no factory address (wifi_mac, fw/mac.info, EFS /wifi/.mac.info): diagnostic 02:50:00:00:00:01\n");
}

static void
scsc_register(void)
{
	memcpy(scsc_link.address, scsc_mac, 6);
	memcpy(scsc_ops.address, scsc_mac, 6);
	if (!hakios_net_attach(&scsc_link)) {
		say("scsc: en0 did not attach\n");
	}
	hakios_sta_attach(&scsc_ops);
	say("scsc: station registered (en0, 2.4 GHz). A join is WPA2-PSK with CCMP, or an open network.\n");
}

/*
 * What Samsung's driver tells the firmware once the WLAN service runs and before any interface (mgt.c slsi_start()):
 * the host's state (mlme.c slsi_mlme_set_host_state(): MLME_HOST_STATE_REQ 0x2028, LCD and cellular active) and the
 * country with its regulatory rules (slsi_mlme_set_country(): MLME_SET_COUNTRY_REQ 0x2033, the code as two letters in
 * one word, the DFS region, then 7 octets a rule). Without a country the firmware only listens: scans are passive and
 * a join faults some 20 ms after its CONNECT is confirmed (10 November). The rules are the phone's own, made by
 * tools/a50-scsc-country.py from /vendor/etc/wifi/slsi_reg_database.bin into fw/scsc_country.bin.
 */
static void
hip_regulatory(vm_offset_t mem, vm_offset_t mbox, struct hip_control *h)
{
	uint64_t size = 0;
	const uint8_t *c = hakios_image_file("fw", "scsc_country.bin", &size, NULL);
	uint8_t *f;
	uint16_t sender;
	uint32_t ref;
	int slot, err;

	slot = hip_slot_get();
	if (slot >= 0) {
		ref = hip_slot_ref[slot];
		sender = hip_sender();
		if (hip_fill(mem, h, ref, 0x2028, sender, 26, NULL, 0, &f)) {
			hip_put16(f + 10, 0);                   /* (no interface) */
			hip_put16(f + 12, 0x0001 | 0x0002);     /* LCD_ACTIVE | CELLULAR_ACTIVE */
			err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x2128, 300);
			say("scsc: MLME_HOST_STATE_REQ (3): %d\n", err);
		} else {
			hip_slot_busy[slot] = 0;
		}
	}
	if (c == NULL || size < 4 || size < 4u + 7u * c[3]) {
		say("scsc: no fw/scsc_country.bin (tools/a50-scsc-country.py): the firmware stays without a country, and "
		    "will only listen\n");
		return;
	}
	slot = hip_slot_get();
	if (slot < 0) {
		return;
	}
	ref = hip_slot_ref[slot];
	sender = hip_sender();
	if (!hip_fill(mem, h, ref, 0x2033, sender, 24, c + 4, 7u * c[3], &f)) {
		hip_slot_busy[slot] = 0;
		say("scsc: MLME_SET_COUNTRY_REQ could not be made\n");
		return;
	}
	hip_put16(f + 10, 0);
	hip_put16(f + 12, (uint16_t)(c[0] << 8 | c[1]));
	hip_put16(f + 14, c[2]);
	err = hip_tx_wait(mem, mbox, h, slot, ref, sender, 0x2133, 300);
	say("scsc: MLME_SET_COUNTRY_REQ %c%c, DFS region %u, %u rules: %d\n", c[0], c[1], c[2], c[3], err);
}

static void
hip_station(vm_offset_t mem, vm_offset_t mbox, uint32_t panic_off)
{
	struct hip_control *h = (struct hip_control *)(mem + hip_ref);
	uint32_t version = ((volatile struct hip_control *)h)->init.version;
	static const uint16_t all[11] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462 };
	int err;

	if (!wlan_started || (version != 4 && version != 5)) {
		say("scsc: station skipped: WLAN ready %u, HIP version %u\n", wlan_started, version);
		return;
	}
	hip_version = version;
	hip_slots_init();
	say("scsc: HIP4 station: negotiated v%u\n", version);
	hip_panic_off = panic_off;
	scsc_mac_load();
	scsc_register();                                /* en0 + IOKit before the first scan: configd can make Wi-Fi */
	hip_regulatory(mem, mbox, h);
	err = hip_vif_cmd(mem, mbox, h, true);
	if (err != 0) {
		say("scsc: station not registered (ADD_VIF failed); RX is still drained\n");
	} else {
		memcpy(hip_scan_ch, all, sizeof(all));
		hip_scan_n = 11;
		(void)hip_scan_cmd(mem, mbox, h);
		log_dump(mem, panic_off);
		lck_mtx_lock(&hip_lock);
		hip_alive = true;
		lck_mtx_unlock(&hip_lock);
	}
	for (;;) {
		int req = 0;

		hip_drain(mem, mbox, h);
		if (hip_joined_v) {
			int rekey = hip_eapol_work(mem, mbox, h);

			if (rekey != 0) {
				say("scsc: group handshake failed (%d)\n", rekey);
			}
		}
		if (hip_alive) {
			req = hip_pull();
		}
		if (req == HIP_REQ_ON || req == HIP_REQ_OFF) {
			hip_finish(hip_vif_cmd(mem, mbox, h, req == HIP_REQ_ON));
		} else if (req == HIP_REQ_SCAN) {
			hip_finish(hip_scan_cmd(mem, mbox, h));
		} else if (req == HIP_REQ_JOIN) {
			hip_finish(hip_join_cmd(mem, mbox, h));
		} else if (req == HIP_REQ_TX) {
			hip_finish(hip_tx_cmd(mem, mbox, h));
		} else if (req == HIP_REQ_LEAVE) {
			hip_finish(hip_leave_cmd(mem, mbox, h));
		} else {
			unsigned sent_n = 0;
			bool all_sent = hip_alive ? hip_txq_run(mem, mbox, h, &sent_n) : true;

			delay(sent_n != 0 || !all_sent ? 200 : 2000);
		}
	}
}

/*
 * The firmware's own log (the mxlog stream: 32-bit words, phase 5 records behind a 0x9669 sync, format strings as
 * offsets into the firmware's debug/common/log-strings.bin): its raw words, said 8 to a line, for decoding on the host;
 * and the R4 panic record whole. Taken, so that the next call says only what is new.
 */
static void
log_dump(vm_offset_t mem, uint32_t panic_off)
{
	struct mx_stream *st = (struct mx_stream *)(mem + log_ref);
	volatile uint32_t *ri = (volatile uint32_t *)(mem + st->buf.read_index), *wi = (volatile uint32_t *)(mem + st->buf.write_index);
	volatile uint32_t *words = (volatile uint32_t *)(mem + st->buf.buffer);
	uint32_t r = *ri, w = *wi, n = (w + st->buf.num_packets - r) % st->buf.num_packets;

	say("scsc: log: %u words (read %u, write %u)\n", n, r, w);
	while (r != w) {
		char line[160];
		int used = snprintf(line, sizeof(line), "scsc: log %04x:", r);

		for (int k = 0; k < 8 && r != w; k++) {
			used += snprintf(line + used, sizeof(line) - used, " %08x", words[r]);
			r = (r + 1) % st->buf.num_packets;
		}
		say("%s\n", line);
	}
	*ri = r;
	__builtin_arm_dsb(0xf);
	volatile uint32_t *panic = (volatile uint32_t *)(mem + (panic_off & 0xFFFFFF));
	if (panic[0] != 0) {
		for (int i = 0; i < 56; i += 8) {
			say("scsc: panic record [%2d] %08x %08x %08x %08x %08x %08x %08x %08x\n", i, panic[i], panic[i + 1], panic[i + 2],
			    panic[i + 3], panic[i + 4], panic[i + 5], panic[i + 6], panic[i + 7]);
		}
	}
}


/* Samsung BHCS v2 and BSMHCP offsets, verified against the A50 headers.
 * Reserve the full protocol region even though this probe only uses HCI. */
#define BT_PROTOCOL_SIZE       0x165c0u
#define BT_PROTOCOL_MAGIC      0x155160c2u
#define BT_CMD_WRITE           0x24
#define BT_CMD_READ            0x44
#define BT_EVT_READ            0x28
#define BT_EVT_WRITE           0x48
#define BT_CMD_BASE            0x100
#define BT_EVT_BASE            0xa2c0
#define BT_EVT_COUNT           32u
#define BT_EVT_STRIDE          264u
#define BT_R4_BG               6

static bool
bt_service_request(vm_offset_t mem, vm_offset_t mbox, uint8_t request, uint32_t ref)
{
	uint8_t packet[6] = { 2, request };

	hip_put32(packet + 2, ref);
	bt_service_cfm = 0;
	if (!mgmt_send(mem, mbox, 1, packet, sizeof(packet))) {
		return false;
	}
	for (unsigned i = 0; i < 200; i++) {
		(void)mgmt_take(mem);
		if (bt_service_cfm == request + 1) {
			return true;
		}
		delay(10000);
	}
	return false;
}


/* One host writer at a time. The controller owns CMD_READ and EVT_WRITE. */
static bool
bt_hci_send(vm_offset_t protocol, vm_offset_t mbox, const uint8_t *data, unsigned length)
{
	uint32_t r = r32(protocol, BT_CMD_READ), w = r32(protocol, BT_CMD_WRITE);
	if (length < 3 || length > 258 || length != data[2] + 3u || r >= 8 || w >= 8 || (w + 1) % 8 == r) {
		return false;
	}
	uint8_t *slot = (uint8_t *)(protocol + BT_CMD_BASE + w * 260);
	memcpy(slot + 2, data, length);
	hip_put16(slot, length);
	__builtin_arm_dsb(0xf);
	w32(protocol, BT_CMD_WRITE, (w + 1) % 8);
	__builtin_arm_dsb(0xf);
	w32(mbox, INTGR1, 1u << BT_R4_BG);
	return true;
}

/* 0: empty, -1: invalid indices, -2: malformed slot, positive: HCI event length. */
static int
bt_hci_peek(vm_offset_t protocol, uint8_t *data, uint32_t *index)
{
	uint32_t r = r32(protocol, BT_EVT_READ), w = r32(protocol, BT_EVT_WRITE);
	if (r >= BT_EVT_COUNT || w >= BT_EVT_COUNT) { return -1; }
	if (r == w) { return 0; }
	__builtin_arm_dsb(0xf);
	const uint8_t *slot = (const uint8_t *)(protocol + BT_EVT_BASE + r * BT_EVT_STRIDE);
	unsigned length = hip_u16(slot);
	*index = r;
	if (length < 2 || length > 258 || length != slot[7] + 2u) { return -2; }
	memcpy(data, slot + 6, length);
	return length;
}

static void
bt_hci_pop(vm_offset_t protocol, vm_offset_t mbox, uint32_t index)
{
	__builtin_arm_dsb(0xf);
	w32(protocol, BT_EVT_READ, (index + 1) % BT_EVT_COUNT);
	__builtin_arm_dsb(0xf);
	w32(mbox, INTGR1, 1u << BT_R4_BG);
}

static bool
bt_hci_command(vm_offset_t protocol, vm_offset_t mbox, const uint8_t *cmd, unsigned size, uint8_t *address)
{
	uint16_t opcode = hip_u16(cmd);
	if (!bt_hci_send(protocol, mbox, cmd, size)) { return false; }
	for (unsigned i = 0; i < 200; i++) {
		uint8_t event[258];
		uint32_t index;
		int n = bt_hci_peek(protocol, event, &index);
		if (n == -1) { break; }
		if (n == 0) { delay(10000); continue; }
		bt_hci_pop(protocol, mbox, index);
		if (n >= 6 && event[0] == 0x0e && hip_u16(event + 3) == opcode) {
			say("scsc-bt: HCI %#x Command Complete status %#x\n", opcode, event[5]);
			if (address != NULL) {
				if (n != 12 || event[5] != 0) { return false; }
				memcpy(address, event + 6, 6);
			}
			return event[5] == 0;
		}
		if (n == 6 && event[0] == 0x0f && hip_u16(event + 4) == opcode && event[2] != 0) {
			say("scsc-bt: HCI %#x Command Status %#x\n", opcode, event[2]);
			return false;
		}
	}
	say("scsc-bt: HCI %#x timed out, cmd read %u, event write %u\n", opcode, r32(protocol, BT_CMD_READ), r32(protocol, BT_EVT_WRITE));
	return false;
}

static bool
bt_ble_probe(vm_offset_t protocol, vm_offset_t mbox)
{
	/* Passive scanning. Interval 60 ms, window 30 ms, duplicate filtering. */
	static const uint8_t event_mask[] = {0x01, 0x0c, 8, 0, 0, 0, 0, 0, 0, 0, 0x20}; /* LE Meta */
	static const uint8_t le_mask[] = {0x01, 0x20, 8, 2, 0, 0, 0, 0, 0, 0, 0}; /* Advertising Report */
	static const uint8_t params[] = {0x0b, 0x20, 7, 0, 0x60, 0, 0x30, 0, 0, 0};
	static const uint8_t enable[] = {0x0c, 0x20, 2, 1, 1}, disable[] = {0x0c, 0x20, 2, 0, 0};
	if (!bt_hci_command(protocol, mbox, event_mask, sizeof(event_mask), NULL) ||
	    !bt_hci_command(protocol, mbox, le_mask, sizeof(le_mask), NULL) ||
	    !bt_hci_command(protocol, mbox, params, sizeof(params), NULL)) {
		return false;
	}
	if (!bt_hci_command(protocol, mbox, enable, sizeof(enable), NULL)) {
		(void)bt_hci_command(protocol, mbox, disable, sizeof(disable), NULL);
		return false;
	}
	unsigned reports = 0;
	for (unsigned tick = 0; tick < 300; tick++) {
		for (unsigned budget = 0; budget < 32; budget++) {
			uint8_t event[258];
			uint32_t index;
			int n = bt_hci_peek(protocol, event, &index);
			if (n == -1) { return false; }
			if (n == 0) { break; }
			bt_hci_pop(protocol, mbox, index);
			if (n < 4 || event[0] != 0x3e || event[2] != 2) { continue; }
			unsigned at = 4;
			for (unsigned j = 0; j < event[3] && at + 10 <= (unsigned)n; j++) {
				unsigned bytes = event[at + 8];
				if (at + 10 + bytes > (unsigned)n) { break; }
				if (reports++ < 8) {
					say("scsc-bt: BLE report, address type %u, RSSI %d dBm, %u data bytes\n",
					    event[at + 1], (int8_t)event[at + 9 + bytes], bytes);
				}
				at += 10 + bytes;
			}
		}
		delay(10000);
	}
	say("scsc-bt: passive BLE scan received %u reports\n", reports);
	return bt_hci_command(protocol, mbox, disable, sizeof(disable), NULL);
}

#ifdef HAKIOS_BCM4361_BT
/* The existing BlueTool/IORegistry path expects H4 over a kernel control.
 * BSMHCP supplies its own ACL buffer ownership and completion rings. */
extern void HakiosBTNodeStart(const uint8_t *address, uint32_t speed);
static LCK_GRP_DECLARE(scsc_bt_group, "scsc-bt");
static LCK_MTX_DECLARE(scsc_bt_lock, &scsc_bt_group);
static vm_offset_t bt_protocol, bt_mbox;
static kern_ctl_ref bt_ctl;
static uint32_t bt_unit;
#define BT_ACL_COUNT 32u
#define BT_ACL_BUFFERS 30u
#define BT_ACL_BYTES 1024u
#define BT_ACL_HANDLES 4096u
static uint8_t bt_fragment[BT_ACL_BYTES + 5];
static uint32_t bt_session;
static struct { uint32_t session; uint16_t handle; bool busy; } bt_buffers[BT_ACL_BUFFERS];
static struct { uint16_t cid; unsigned left; uint16_t rx_credits; } bt_links[BT_ACL_HANDLES];
static unsigned bt_host_mtu, bt_host_count, bt_host_used;
static bool bt_host_flow;
static uint8_t bt_reply[8], bt_reporting;
static unsigned bt_reply_len;
static bool bt_reply_pending;
static unsigned bt_used, bt_tx_said, bt_rx_said, bt_backpressure_said;
static uint16_t bt_vs_opcode;              /* the vendor command whose completion is forged */
static uint8_t bt_vs_first;                 /* its first parameter: the subopcode iOS matches */


/* The kernel control terminates HCI host flow control. The firmware's ACL
 * RX ring is separate, so never forward these host buffer commands to R4. */
static errno_t
bt_host_command(const uint8_t *cmd, unsigned length)
{
	uint16_t opcode = hip_u16(cmd);
	uint8_t status = 0;
	if (opcode == 0x0c35) {                    /* Host Number of Completed Packets */
		if (length < 4 || length != 4u + 4u * cmd[3]) { return EINVAL; }
		for (unsigned i = 0; i < cmd[3]; i++) {
			uint16_t handle = hip_u16(cmd + 4 + 4 * i);
			unsigned count = hip_u16(cmd + 6 + 4 * i);
			if (handle >= BT_ACL_HANDLES) { return EINVAL; }
			for (unsigned j = 0; j < i; j++) {
				if (hip_u16(cmd + 4 + 4 * j) == handle) { count += hip_u16(cmd + 6 + 4 * j); }
			}
			if (count > bt_links[handle].rx_credits) { return EINVAL; }
		}
		for (unsigned i = 0; i < cmd[3]; i++) {
			unsigned count = hip_u16(cmd + 6 + 4 * i);
			bt_links[hip_u16(cmd + 4 + 4 * i)].rx_credits -= count;
			bt_host_used -= count;
		}
		return 0;                         /* no completion event on success */
	}
	if (bt_reply_pending) { return ENOBUFS; }
	if (opcode == 0x0c33) {                    /* Host Buffer Size */
		if (length != 10 || hip_u16(cmd + 3) == 0 || hip_u16(cmd + 6) == 0 || bt_host_used != 0) {
			status = 0x12;
		} else {
			bt_host_mtu = hip_u16(cmd + 3);
			bt_host_count = hip_u16(cmd + 6);
			say("scsc-bt: host ACL buffers %u x %u bytes\n", bt_host_count, bt_host_mtu);
		}
	} else if (opcode == 0x0c5a || opcode == 0x0c5b) {
		/* Local setting for synchronous data reporting. BSMHCP has no
		 * synchronous H4 ring in this firmware, so no SCO packets use it. */
		if (opcode == 0x0c5a) {
			if (length != 3) { status = 0x12; }
		} else if (length != 4 || cmd[3] > 1) {
			status = 0x12;
		} else {
			bt_reporting = cmd[3];
		}
	} else {                                  /* Set Controller To Host Flow Control */
		if (length != 4 || cmd[3] > 3 || bt_host_used != 0) {
			status = 0x12;
		} else {
			/* Bit 0 controls ACL. This transport has no synchronous HCI data path. */
			bt_host_flow = (cmd[3] & 1u) != 0;
		}
		say("scsc-bt: host flow mode %u, status %#x\n", length == 4 ? cmd[3] : 0xff, status);
	}
	bool read_reporting = opcode == 0x0c5a && status == 0;
	uint8_t reply[] = {4, 0x0e, read_reporting ? 5 : 4, 1, (uint8_t)opcode, (uint8_t)(opcode >> 8), status, bt_reporting};
	bt_reply_len = read_reporting ? 8 : 7;
	memcpy(bt_reply, reply, sizeof(reply));
	say("scsc-bt: local HCI %#x status %#x\n", opcode, status);
	bt_reply_pending = true;
	return 0;
}

static void
bt_acl_doorbell(void)
{
	__builtin_arm_dsb(0xf);
	w32(bt_mbox, INTGR1, 1u << 7);            /* R4 foreground (M4 mode is excluded) */
}

static errno_t
bt_acl_send(const uint8_t *h4, unsigned length)
{
	uint16_t handle = hip_u16(h4 + 1) & 0xfff;
	uint8_t flags = h4[2] & 0xf0;
	unsigned bytes = length - 5;
	uint32_t r = r32(bt_protocol, 0x60), w = r32(bt_protocol, 0x2c);
	if (r >= BT_ACL_COUNT || w >= BT_ACL_COUNT) { return EIO; }
	if ((w + 1) % BT_ACL_COUNT == r) { return ENOBUFS; }
	unsigned slot = 0;
	while (slot < BT_ACL_BUFFERS && bt_buffers[slot].busy) { slot++; }
	if (slot == BT_ACL_BUFFERS) { return ENOBUFS; }
	unsigned left = bt_links[handle].left;
	uint16_t cid = bt_links[handle].cid;
	switch (flags & 0x30) {
	case 0x00:
	case 0x20:
		if (bytes < 4) { return EINVAL; }
		left = 4u + hip_u16(h4 + 5);
		cid = hip_u16(h4 + 7);
		break;
	case 0x10:
		if (left == 0) { return EINVAL; }
		break;
	default:
		return EINVAL;
	}
	if (bytes > left) { return EINVAL; }
	left -= bytes;
	bt_links[handle].left = left;
	bt_links[handle].cid = cid;
	bt_buffers[slot].session = bt_session;
	bt_buffers[slot].handle = handle;
	bt_buffers[slot].busy = true;
	uint8_t *td = (uint8_t *)(bt_protocol + 0x29a0 + w * 8);
	hip_put16(td, bytes);
	td[2] = slot;
	td[3] = flags | (left == 0 ? 1 : 0);
	hip_put16(td + 4, handle);
	hip_put16(td + 6, cid);
	memcpy((void *)(bt_protocol + 0x2aa0 + slot * BT_ACL_BYTES), h4 + 5, bytes);
	__builtin_arm_dsb(0xf);
	w32(bt_protocol, 0x2c, (w + 1) % BT_ACL_COUNT);
	bt_acl_doorbell();
	return 0;
}

/* Return TX credits only for buffers the firmware has actually freed. */
static void
bt_acl_reap(void)
{
	uint32_t r = r32(bt_protocol, 0x30), w = r32(bt_protocol, 0x64);
	if (r >= BT_ACL_COUNT || w >= BT_ACL_COUNT || r == w) { return; }
	__builtin_arm_dsb(0xf);
	while (r != w) {
		const uint8_t *td = (const uint8_t *)(bt_protocol + 0x16540 + r * 4);
		unsigned slot = td[0];
		uint16_t handle = hip_u16(td + 2) & 0xfff;
		lck_mtx_lock(&scsc_bt_lock);
		bool valid = slot < BT_ACL_BUFFERS && bt_buffers[slot].busy && bt_buffers[slot].handle == handle;
		uint32_t session = bt_session;
		uint32_t unit = valid && bt_buffers[slot].session == session ? bt_unit : 0;
		lck_mtx_unlock(&scsc_bt_lock);
		uint8_t h4[8] = {4, 0x13, 5, 1, (uint8_t)handle, (uint8_t)(handle >> 8), 1, 0};
		if (unit != 0 && ctl_enqueuedata(bt_ctl, unit, h4, sizeof(h4), 0) != 0) { break; }
		lck_mtx_lock(&scsc_bt_lock);
		if (valid) { bt_buffers[slot].busy = false; }
		r = (r + 1) % BT_ACL_COUNT;
		w32(bt_protocol, 0x30, r);
		lck_mtx_unlock(&scsc_bt_lock);
	}
	bt_acl_doorbell();
}

/* Deliver one fragment, holding the RX slot until its full payload is read. */
static bool
bt_acl_receive(unsigned *offset)
{
	uint32_t r = r32(bt_protocol, 0x34), w = r32(bt_protocol, 0x68);
	if (r >= BT_ACL_COUNT || w >= BT_ACL_COUNT || r == w) { return false; }
	__builtin_arm_dsb(0xf);
	const uint8_t *td = (const uint8_t *)(bt_protocol + 0xe440 + r * 1032);
	unsigned bytes = hip_u16(td + 2);
	uint16_t handle = hip_u16(td);
	lck_mtx_lock(&scsc_bt_lock);
	uint32_t unit = bt_unit, session = bt_session;
	if (unit == 0 || td[6] != 0 || bytes > BT_ACL_BYTES || handle >= BT_ACL_HANDLES || *offset > bytes) {
		lck_mtx_unlock(&scsc_bt_lock);
		goto consume;
	}
	bool flow = bt_host_flow;
	if (flow && (bt_host_count == 0 || bt_host_used >= bt_host_count)) {
		lck_mtx_unlock(&scsc_bt_lock);
		return false;
	}
	unsigned chunk = bytes - *offset;
	if (bt_host_mtu != 0 && chunk > bt_host_mtu) { chunk = bt_host_mtu; }
	if (flow) { bt_host_used++; bt_links[handle].rx_credits++; }
	lck_mtx_unlock(&scsc_bt_lock);
	uint8_t h4[BT_ACL_BYTES + 5] = {2};
	unsigned pb = *offset != 0 ? 1 : (td[5] & 3);
	hip_put16(h4 + 1, handle | (pb << 12) | ((td[4] & 3) << 14));
	hip_put16(h4 + 3, chunk);
	memcpy(h4 + 5, td + 8 + *offset, chunk);
	if (ctl_enqueuedata(bt_ctl, unit, h4, chunk + 5, 0) != 0) {
		lck_mtx_lock(&scsc_bt_lock);
		if (flow && session == bt_session) { bt_host_used--; bt_links[handle].rx_credits--; }
		lck_mtx_unlock(&scsc_bt_lock);
		return false;
	}
	*offset += chunk;
	if (*offset < bytes) { return true; }
consume:
	*offset = 0;
	w32(bt_protocol, 0x34, (r + 1) % BT_ACL_COUNT);
	bt_acl_doorbell();
	return true;
}

static errno_t
scsc_bt_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sa, void **info)
{
	*info = NULL;
	lck_mtx_lock(&scsc_bt_lock);
	bt_unit = sa->sc_unit;
	bt_session++;
	bt_used = 0;
	bt_reply_pending = false;
	bt_vs_opcode = 0;
	bt_vs_first = 0;
	bt_host_mtu = bt_host_count = bt_host_used = 0;
	bt_host_flow = false;
	bt_reporting = 0;
	memset(bt_links, 0, sizeof(bt_links));
	lck_mtx_unlock(&scsc_bt_lock);
	say("scsc-bt: iOS H4 client connected, unit %u pid %d\n", sa->sc_unit, proc_pid(current_proc()));
	return 0;
}

static errno_t
scsc_bt_disconnect(__unused kern_ctl_ref ref, uint32_t unit, __unused void *info)
{
	lck_mtx_lock(&scsc_bt_lock);
	if (bt_unit == unit) { bt_unit = 0; bt_used = 0; }
	lck_mtx_unlock(&scsc_bt_lock);
	return 0;
}

static errno_t
scsc_bt_send(__unused kern_ctl_ref ref, uint32_t unit, __unused void *info, mbuf_t m, __unused int flags)
{
	errno_t error = 0;
	size_t length = mbuf_pkthdr_len(m);
	lck_mtx_lock(&scsc_bt_lock);
	if (bt_unit != unit || unit == 0) { error = ENXIO; }
	for (size_t off = 0; error == 0 && off < length; off++) {
		if (mbuf_copydata(m, off, 1, bt_fragment + bt_used) != 0) { error = EINVAL; break; }
		bt_used++;
		unsigned type = bt_fragment[0];
		if (type != 1 && type != 2) { error = EOPNOTSUPP; break; }
		if (type == 2 && bt_used >= 5) {
			unsigned bytes = hip_u16(bt_fragment + 3);
			if (bytes > BT_ACL_BYTES) { error = EMSGSIZE; break; }
			if (bt_used == 5 + bytes) { error = bt_acl_send(bt_fragment, bt_used); bt_used = 0; }
		} else if (type == 1 && bt_used >= 4 && bt_used == 4u + bt_fragment[3]) {
			uint16_t opcode = hip_u16(bt_fragment + 1);
			if (opcode == 0xfce2 || opcode == 0xfd1b) {
				/* These two are synchronous: iOS matches the completion by the subopcode in
				 * the command's first parameter, so remember it for the forged event. The
				 * caller already holds scsc_bt_lock (this is not a recursive mutex). */
				bt_vs_opcode = opcode;
				bt_vs_first = bt_used >= 5 ? bt_fragment[4] : 0;
			}
			if (opcode == 0x200b && bt_used >= 11) {
				/* The controller refuses LE Set Scan Enable (0x200c) with 0x12 for
				 * iOS's 300 ms interval; its own probe scans at 60 ms / 30 ms and the
				 * controller accepts that. Ask for those, from the public address (iOS
				 * never sends LE Set Random Address for its own type 1). */
				/* keep iOS's own interval and window (a 30 ms window every 300 ms leaves the antenna to Wi-Fi
				 * ninety percent of the time; 60/30 took half of it and the firmware faulted 0x2a81) */
				bt_fragment[9] = 0;
				say("scsc-bt: LE scan parameters from the public address, interval %#x window %#x\n", hip_u16(bt_fragment + 5), hip_u16(bt_fragment + 7));
			}
			if (opcode == 0x0c31 || opcode == 0x0c33 || opcode == 0x0c35 || opcode == 0x0c5a || opcode == 0x0c5b) {
				error = bt_host_command(bt_fragment + 1, bt_used - 1);
			} else if (!bt_hci_send(bt_protocol, bt_mbox, bt_fragment + 1, bt_used - 1)) {
				error = ENOBUFS;
			}
			if (bt_tx_said++ < 140) {
				say("scsc-bt: iOS TX unit %u HCI %#x len %u errno %d\n", unit, opcode, bt_used - 1, error);
			}
			if (opcode == 0x200b || opcode == 0x200c) {
				/* The controller answers 0x12 (Invalid HCI Command Parameters) to the LE
				 * scan enable: its parameters are what tell why. */
				say("scsc-bt: LE cmd %#x:", opcode);
				for (unsigned at = 3; at < bt_used; at++) { say(" %02x", bt_fragment[at]); }
				say("\n");
			}
			bt_used = 0;
		}
	}
	if (error != 0 && bt_unit == unit) { bt_used = 0; }
	lck_mtx_unlock(&scsc_bt_lock);
	mbuf_freem(m);
	return error;
}

static unsigned bt_card_said;

static errno_t
scsc_bt_getopt(__unused kern_ctl_ref ref, __unused uint32_t unit, __unused void *info, int opt, void *data, size_t *len)
{
	if ((unsigned)opt != (unsigned)TIOCGETA) { return ENOPROTOOPT; }
	if (data != NULL) {
		if (*len < sizeof(struct termios)) { return EINVAL; }
		struct termios t = { .c_cflag = CS8 | CREAD | CLOCAL | CRTSCTS, .c_ispeed = 4000000, .c_ospeed = 4000000 };
		memcpy(data, &t, sizeof(t));
	}
	*len = sizeof(struct termios);
	return 0;
}

static errno_t
scsc_bt_setopt(__unused kern_ctl_ref ref, __unused uint32_t unit, __unused void *info, int opt,
    __unused void *data, __unused size_t len)
{
	unsigned o = (unsigned)opt;
	return o == (unsigned)TIOCSETA || o == (unsigned)TIOCSETAW || o == (unsigned)TIOCSETAF ? 0 : ENOPROTOOPT;
}

static void
scsc_bt_events(__unused void *arg, __unused wait_result_t result)
{
	unsigned rx_offset = 0;
	for (;;) {
		uint8_t reply[sizeof(bt_reply)];
		lck_mtx_lock(&scsc_bt_lock);
		uint32_t owner = bt_unit, session = bt_session;
		bool pending = bt_reply_pending && owner != 0;
		unsigned reply_len = bt_reply_len;
		if (pending) {
			memcpy(reply, bt_reply, sizeof(reply));
			bt_reply_pending = false;
		}
		lck_mtx_unlock(&scsc_bt_lock);
		if (pending && ctl_enqueuedata(bt_ctl, owner, reply, reply_len, 0) != 0) {
			lck_mtx_lock(&scsc_bt_lock);
			if (bt_session == session && !bt_reply_pending) {
				memcpy(bt_reply, reply, sizeof(reply));
				bt_reply_len = reply_len;
				bt_reply_pending = true;
			}
			lck_mtx_unlock(&scsc_bt_lock);
		}
		for (unsigned budget = 0; budget < BT_EVT_COUNT; budget++) {
			uint8_t h4[259] = {4};
			uint32_t index, unit;
			int n = bt_hci_peek(bt_protocol, h4 + 1, &index);
			if (n == -1) {
				say("scsc-bt: iOS transport invalid event index\n");
				thread_terminate(current_thread());
				return;
			}
			if (n == 0) { break; }
			lck_mtx_lock(&scsc_bt_lock);
			unit = bt_unit;
			lck_mtx_unlock(&scsc_bt_lock);
			if (n > 0 && unit != 0) {
				/* Broadcom vendor commands the SCSC firmware does not know come back as
				 * Command Status (Unknown Command). iOS's BCM VSC event parser
				 * (bcm_vsc.c) rejects a completion with fewer than two bytes after the
				 * status ("Invalid data size %d") and then times the command out, which
				 * aborts the whole stack. Complete such a command here as a Command
				 * Complete with the same status and two zero bytes, so its callback
				 * finishes; the opcode is echoed so the stack matches it. */
				uint16_t vendor = n == 6 && h4[1] == 0x0f ? hip_u16(h4 + 5) :
				    n == 7 && h4[1] == 0x0e ? hip_u16(h4 + 4) : 0;
				if (vendor == 0xfce2 || vendor == 0xfd1b) {
					uint8_t first;
					lck_mtx_lock(&scsc_bt_lock);
					first = bt_vs_opcode == vendor ? bt_vs_first : 0;
					lck_mtx_unlock(&scsc_bt_lock);
					if (n == 6 && h4[1] == 0x0f && h4[3] == 1) {
						uint8_t response[] = {4, 0x0e, 6, 1, h4[5], h4[6], 1, first, 0};
						memcpy(h4, response, sizeof(response));
						n = sizeof(response) - 1;
					} else if (n == 7 && h4[1] == 0x0e && h4[6] != 0) {
						uint8_t response[] = {4, 0x0e, 6, 1, h4[4], h4[5], h4[6], first, 0};
						memcpy(h4, response, sizeof(response));
						n = sizeof(response) - 1;
					}
				}
				if (n == 70 && h4[1] == 0x0e && hip_u16(h4 + 4) == 0x1002 && h4[6] == 0) {
					/* The local H4 layer implements these two configuration commands. */
					h4[7 + 18] |= 0x0c;
				}
				if (n == 6 && h4[1] == 0x0e && hip_u16(h4 + 4) == 0x0c03 && h4[6] == 0) {
					lck_mtx_lock(&scsc_bt_lock);
					bt_reporting = 0;
					bt_host_flow = false;
					bt_host_used = bt_host_mtu = bt_host_count = 0;
					memset(bt_links, 0, sizeof(bt_links));
					lck_mtx_unlock(&scsc_bt_lock);
				}
				if (n >= 16 && h4[1] == 0x3e && h4[3] == 0x02 && h4[4] == 1 && 14u + h4[13] <= (unsigned)n + 1) {
					/* Apple's "proximity pairing" advertisement (company 0x004c, type 0x07) is what
					 * makes SharingViewService put AirPods' setup card on the screen; with no
					 * way to dismiss it here, the card stays over everything. Not delivered. */
					bool card = false;
					for (unsigned at = 14, end = 14u + h4[13]; at + 5 < end; at += 1u + h4[at]) {
						if (h4[at] == 0) { break; }
						if (h4[at] >= 4 && h4[at + 1] == 0xff && h4[at + 2] == 0x4c && h4[at + 3] == 0 && h4[at + 4] == 0x07) {
							card = true;
							break;
						}
					}
					if (card) {
						if (bt_card_said++ < 4) { say("scsc-bt: Apple proximity-pairing advertisement not delivered\n"); }
						bt_hci_pop(bt_protocol, bt_mbox, index);
						continue;
					}
				}
				errno_t err = ctl_enqueuedata(bt_ctl, unit, h4, (size_t)n + 1, 0);
				if (err != 0) {
					if (bt_backpressure_said++ < 8) { say("scsc-bt: iOS H4 enqueue error %d unit %u\n", err, unit); }
					break;
				}
				if (bt_rx_said++ < 140) {
					unsigned opcode = n >= 6 && h4[1] == 0x0e ? hip_u16(h4 + 4) : n == 6 && h4[1] == 0x0f ? hip_u16(h4 + 5) : 0;
					unsigned status = h4[1] == 0x0e && n >= 6 ? h4[6] : h4[1] == 0x0f && n == 6 ? h4[3] : 0;
					say("scsc-bt: iOS RX unit %u event %#x HCI %#x status %#x\n", unit, h4[1], opcode, status);
				}
			}
			bt_hci_pop(bt_protocol, bt_mbox, index);
		}
		bt_acl_reap();
		for (unsigned budget = 0; budget < BT_ACL_COUNT && bt_acl_receive(&rx_offset); budget++) {}
		assert_wait_timeout((event_t)&bt_ctl, THREAD_UNINT, 5, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

static bool
scsc_bt_publish(vm_offset_t protocol, vm_offset_t mbox, const uint8_t address[6])
{
	struct kern_ctl_reg reg = {0};
	thread_t worker;
	if (r32(protocol, 0x50) & 2u) {
		say("scsc-bt: ACL transport requires R4 foreground interrupts\n");
		return false;
	}
	bt_protocol = protocol;
	bt_mbox = mbox;
	strlcpy(reg.ctl_name, "com.apple.uart.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_recvsize = 64 * 1024;
	reg.ctl_connect = scsc_bt_connect;
	reg.ctl_disconnect = scsc_bt_disconnect;
	reg.ctl_send = scsc_bt_send;
	reg.ctl_getopt = scsc_bt_getopt;
	reg.ctl_setopt = scsc_bt_setopt;
	errno_t err = ctl_register(&reg, &bt_ctl);
	if (err != 0) { say("scsc-bt: H4 control registration failed %d\n", err); return false; }
	if (kernel_thread_start(scsc_bt_events, NULL, &worker) != KERN_SUCCESS) {
		(void)ctl_deregister(bt_ctl);
		return false;
	}
	thread_deallocate(worker);
	HakiosBTNodeStart(address, 4000000);
	say("scsc-bt: iOS H4 command/event/ACL transport published\n");
	return true;
}
#endif

static void
scsc_bt_probe(vm_offset_t mem, vm_offset_t mbox, uint32_t panic_off, bool bridge)
{
	uint64_t config_size = 0;
	const uint8_t *config = hakios_image_file("fw", "mx140_bt.hcf", &config_size, NULL);
	uint32_t start = (heap_next + 63) & ~63u;

	if (!wlan_started || config == NULL || config_size == 0 || config_size > 4096 ||
	    start > SHARED_BYTES - BT_PROTOCOL_SIZE - 4096 - 128) {
		say("scsc-bt: skipped, WLAN %u, BT HCF size %llu, heap +%#x\n", wlan_started, config_size, start);
		return;
	}
	if (!guard_enter(7)) {
		return;
	}
	uint32_t bhcs = heap_alloc(mem, 28, 0);
	uint32_t proto = heap_alloc(mem, BT_PROTOCOL_SIZE, 0);
	uint32_t conf = heap_alloc(mem, (uint32_t)config_size, 0);
	uint8_t *b = (uint8_t *)(mem + bhcs), *p = (uint8_t *)(mem + proto);
	vm_offset_t protocol = mem + proto;
	/* BT consumes the complete HCF, unlike the WLAN MIB record area. */
	memcpy((void *)(mem + conf), config, (size_t)config_size);
	hip_put32(b, 2);
	hip_put32(b + 4, proto);
	hip_put32(b + 8, BT_PROTOCOL_SIZE);
	hip_put32(b + 12, conf);
	hip_put32(b + 16, (uint32_t)config_size);
	/* Diagnostic local address. The probe neither advertises nor connects. */
	hip_put32(b + 20, 2);
	hip_put16(b + 26, 0x0250);
	hip_put32(p, BT_PROTOCOL_MAGIC);
	hip_put16(p + 0x04, 2);                /* AP -> M4 foreground */
	hip_put16(p + 0x14, BT_R4_BG);         /* AP -> R4 background */
	hip_put16(p + 0x16, 7);                /* AP -> R4 foreground */
	hip_put16(p + 0x18, 8);                /* to AP, background */
	hip_put16(p + 0x1a, 9);                /* to AP, foreground */
	hip_put16(p + 0xbc, 8);                /* AP -> R4 info */
	hip_put16(p + 0xbe, 10);               /* to AP, info */
	__builtin_arm_dsb(0xf);
	say("scsc-bt: START_REQ BHCS +%#x, BSMHCP +%#x, HCF %llu bytes\n", bhcs, proto, config_size);
	if (!bt_service_request(mem, mbox, 0, bhcs)) {
		say("scsc-bt: START_CFM timed out\n");
		goto out;
	}
	say("scsc-bt: START_CFM, controller flags %#x, features %#x\n", r32(protocol, 0x4c), r32(protocol, 0x50));
	static const uint8_t reset[] = {0x03, 0x0c, 0}, read_address[] = {0x09, 0x10, 0};
	uint8_t address[6] = {0};
	if (bt_hci_command(protocol, mbox, reset, sizeof(reset), NULL) && bridge) {
		bool scan_ok = bt_ble_probe(protocol, mbox);
		if (scan_ok && bt_hci_command(protocol, mbox, reset, sizeof(reset), NULL) &&
		    bt_hci_command(protocol, mbox, read_address, sizeof(read_address), address) &&
		    (address[0] || address[1] || address[2] || address[3] || address[4] || address[5])) {
#ifdef HAKIOS_BCM4361_BT
			if (scsc_bt_publish(protocol, mbox, address)) { goto out; }
#else
			say("scsc-bt: iOS bridge requires the existing BT registry helper (HAKIOS_BCM4361_BT)\n");
#endif
		}
	}
	say("scsc-bt: STOP_CFM %s\n", bt_service_request(mem, mbox, 2, 0) ? "received" : "timed out");
out:
	guard_leave();
	log_dump(mem, panic_off);
}

static void
scsc_start(uint32_t steps)
{
	vm_offset_t pmu = ml_io_map(PMU_PA, 0x2000), mbox = ml_io_map(MAILBOX_WLBT_PA, 0x1000), gicd = ml_io_map(GICD_PA, 0x1000);
	vm_offset_t remap = ml_io_map(WLBT_REMAP_PA, 0x1000), dbus = ml_io_map(DBUS_BAAW_PA, 0x1000);
	vm_offset_t pbus = ml_io_map(PBUS_BAAW_PA, 0x1000), boot = ml_io_map(BOOT_CFG_PA, 0x2000);
	vm_offset_t mem = 0;
	const uint8_t *fw, *ka;
	uint64_t fw_size = 0, ka_size = 0, mem_pa;
	uint32_t entry, runtime, panic_off;
	bool cfg_req = false;

	if (!pmu || !mbox || !gicd || !remap || !dbus || !pbus || !boot) {
		say("scsc: 3: a block could not be mapped\n");
		return;
	}
	fw = hakios_image_file("fw", "mx140.bin", &fw_size, NULL);
	ka = hakios_image_file("fw", "scsc_ka.bin", &ka_size, NULL);
	if (fw == NULL || ka == NULL || fw_size < 256 || memcmp(fw + 8, "smxf", 4) != 0) {
		say("scsc: 3: no fw/mx140.bin (%llu bytes) or fw/scsc_ka.bin (%llu) in the small image (../fw-scsc, "
		    "tools/a50-scsc-ka.py)\n", fw_size, ka_size);
		return;
	}
	entry = *(const uint32_t *)(fw + 40);
	runtime = *(const uint32_t *)(fw + 36);
	panic_off = *(const uint32_t *)(fw + 176);
	say("scsc: 3: mx140.bin %llu bytes: header %u.%u, entry %#x, runtime %#x bytes, R4 panic record at +%#x; %s\n",
	    fw_size, *(const uint16_t *)(fw + 14), *(const uint16_t *)(fw + 12), entry, runtime, panic_off,
	    (const char *)fw + 48);
	/*
	 * Not the kernel's memory (16 MB contiguous below 4 GB is not to be had once the system runs): abox_rmem's second
	 * 16 MB (0xE9400000 + 40 MB, kept out of XNU by the loader; ABOX here takes its ring from the kernel), mapped
	 * write-combined, so that nothing waits in a cache between the AP and the chip
	 */
	mem_pa = SHARED_PA;
	mem = ml_io_map_unmappable(SHARED_PA, SHARED_BYTES, VM_WIMG_WCOMB);
	if (mem == 0) {
		say("scsc: 3: the shared DRAM at %#x could not be mapped\n", SHARED_PA);
		return;
	}
	for (uint32_t i = 0; i < SHARED_BYTES / 8; i++) {
		((volatile uint64_t *)mem)[i] = 0;
	}
	memcpy((void *)mem, fw, (size_t)fw_size);
	__builtin_arm_dsb(0xf);
	say("scsc: 3: the shared DRAM at %#llx (16 MB), the firmware copied to its start\n", mem_pa);

	/* the power and the reset (step 2), the mailbox as platform_mif_map() and mbox_init() leave it */
	w32(pmu, WLBT_CTRL_NS, r32(pmu, WLBT_CTRL_NS) | (1u << 18));
	delay(1000);
	w32(pmu, WLBT_CTRL_NS, r32(pmu, WLBT_CTRL_NS) & ~1u);
	delay(1000);
	if (!guard_enter(3)) {
		return;
	}
	for (int i = 0; i < 8; i++) {
		w32(mbox, ISSR(i), 0);
	}
	w32(mbox, INTMR0, 0xFFFF0000u);
	w32(mbox, INTMR1, 0x0000FFFFu);
	w32(mbox, 0x00C, 0xFFFF0000u);         /* INTCR0 */
	w32(mbox, 0x020, 0x0000FFFFu);         /* INTCR1 */
	w32(mbox, ISSR(0), entry);
	w32(mbox, ISSR(1), mxconf_make(mem, runtime));
	w32(mbox, ISSR(2), 0xBCDEEDCBu);
	w32(mbox, ISSR(3), 0);
	say("scsc: 3: mailbox ISSR0-3 %#x %#x %#x %#x; WLBT_START next\n", r32(mbox, ISSR(0)), r32(mbox, ISSR(1)),
	    r32(mbox, ISSR(2)), r32(mbox, ISSR(3)));

	w32(pmu, WLBT_CTRL_S, r32(pmu, WLBT_CTRL_S) | 1u);
	for (int ms = 0; ms < 1000; ms++) {
		if (r32(gicd, 0x200 + 4 * (CFG_REQ_INTID / 32)) & (1u << (CFG_REQ_INTID % 32))) {
			cfg_req = true;
			say("scsc: 3: CFG_REQ after %d ms (WLBT_CTRL_S %#x, CENTRAL_SEQ_WLBT_STATUS %#x)\n", ms, r32(pmu, WLBT_CTRL_S),
			    r32(pmu, CENTRAL_SEQ_WLBT_STATUS));
			break;
		}
		delay(1000);
	}
	if (!cfg_req) {
		say("scsc: 3: no CFG_REQ in 1 s (WLBT_CTRL_NS %#x WLBT_CTRL_S %#x CENTRAL_SEQ_WLBT_STATUS %#x WLBT_DEBUG %#x)\n",
		    r32(pmu, WLBT_CTRL_NS), r32(pmu, WLBT_CTRL_S), r32(pmu, CENTRAL_SEQ_WLBT_STATUS), r32(pmu, WLBT_DEBUG));
		guard_leave();
		return;
	}
	delay(100);
	say("scsc: 3: SMC CONN_IF (the TZPC to non-secure) answered %#llx\n", scsc_smc(0x82000710u, (1ull << 32) | 0, 0, 0));
	w32(remap, 0x0, 0x80000000u >> 12);
	uint32_t id = r32(remap, 0x10);
	w32(remap, 0x10, (id & ~EMA_MASK) | EMA_VALUE);
	say("scsc: 3: WLBT_REMAP %#x, CHIP_VERSION_ID %#x -> %#x\n", r32(remap, 0), id, r32(remap, 0x10));
	w32(dbus, 0x0, 0x80000000u >> 12);
	w32(dbus, 0x4, 0x81000000u >> 12);     /* WLBT_DBUS_BAAW_0_END: 16 MB */
	w32(dbus, 0x8, (uint32_t)(mem_pa >> 12));
	w32(dbus, 0xC, BAAW_ACCESS);
	static const uint32_t pbus_windows[6][3] = {
		{ 0xA0000000u, 0xA0010000u, 0x11950000u }, { 0xA0010000u, 0xA0060000u, 0x119A0000u },
		{ 0xA0060000u, 0xA0100000u, 0x11D00000u }, { 0xA0100000u, 0xA0110000u, 0x11C80000u },
		{ 0xA0110000u, 0xA0120000u, 0x11C20000u }, { 0xA0120000u, 0xA0160000u, 0x11200000u },
	};
	for (int i = 0; i < 6; i++) {
		w32(pbus, 0x10 * i + 0x0, pbus_windows[i][0] >> 12);
		w32(pbus, 0x10 * i + 0x4, pbus_windows[i][1] >> 12);
		w32(pbus, 0x10 * i + 0x8, pbus_windows[i][2] >> 12);
		w32(pbus, 0x10 * i + 0xC, BAAW_ACCESS);
	}
	w32(boot, 0x0, 1);
	for (uint64_t i = 0; i < ka_size / 4; i++) {
		w32(boot, 0x1000 + 4 * (uint32_t)i, ((const uint32_t *)ka)[i]);
	}
	w32(boot, 0x0, 0);
	w32(boot, 0x4, 1);                      /* BOOT_CFG_ACK */
	delay(100);
	w32(pmu, WLBT_CTRL_NS, r32(pmu, WLBT_CTRL_NS) & ~(1u << 18));
	guard_leave();
	say("scsc: 3: configured (DBUS -> %#llx, %llu words of keep-alive patch), CFG_ACK given, the power given back\n",
	    mem_pa, ka_size / 4);
	for (int i = 0; i < 16; i++) {
		delay(500 * 1000);
		(void)mgmt_take(mem);
		if (i == 0 || i == 3 || i == 15) {
			log_dump(mem, panic_off);
		}
		if (i == 1 && hip_make(mem, 5, 7)) {
			/* the service manager (channel 1): struct sm_msg_packet { service_id, msg (SM_MSG_START_REQ 0),
			 * scsc_mifram_ref optional_data } packed: the WLAN service (1) with its HIP4 control block */
			uint8_t sm[6] = { 1, 0, (uint8_t)hip_ref, (uint8_t)(hip_ref >> 8), (uint8_t)(hip_ref >> 16), (uint8_t)(hip_ref >> 24) };

			say("scsc: 6: SM_MSG_START_REQ for the WLAN service (HIP4 at +%#x): %s\n", hip_ref,
			    mgmt_send(mem, mbox, 1, sm, sizeof(sm)) ? "sent" : "the stream is full");
		}
		volatile uint32_t *panic = (volatile uint32_t *)(mem + (panic_off & 0xFFFFFF));
		say("scsc: 3: +%d ms: CFG_REQ pending %u, mailbox INTSR0 %#x INTSR1 %#x ISSR0-7 %#x %#x %#x %#x %#x %#x %#x %#x; "
		    "panic record %#x %#x %#x %#x\n", (i + 1) * 500,
		    (r32(gicd, 0x200 + 4 * (CFG_REQ_INTID / 32)) >> (CFG_REQ_INTID % 32)) & 1, r32(mbox, INTSR0), r32(mbox, INTSR1),
		    r32(mbox, ISSR(0)), r32(mbox, ISSR(1)), r32(mbox, ISSR(2)), r32(mbox, ISSR(3)), r32(mbox, ISSR(4)),
		    r32(mbox, ISSR(5)), r32(mbox, ISSR(6)), r32(mbox, ISSR(7)), panic[0], panic[1], panic[2], panic[3]);
	}
	if (steps == 7 || steps == 8) {
		scsc_bt_probe(mem, mbox, panic_off, steps == 8);
	}
	if (steps >= 6) {
		hip_station(mem, mbox, panic_off);		/* does not return: it keeps the queues */
	} else if (steps >= 4) {
		hip_probe(mem, mbox, steps >= 5);
		log_dump(mem, panic_off);
	}
}

static void
scsc_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg;

	say("scsc: the SCSC S620 (Wi-Fi, Bluetooth), %u step(s)\n", steps);
	scsc_look();
	if (steps == 2) {
		delay(20 * 1000 * 1000);        /* late: the boot well on its way, its log out, before anything risky */
		scsc_wlbt_power();
	} else if (steps >= 3) {
		delay(20 * 1000 * 1000);
		scsc_start(steps);
	}
	thread_terminate(current_thread());
}

__startup_func
static void
scsc_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("scsc", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (kernel_thread_start(scsc_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, scsc_init);
