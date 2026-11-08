/*
 * The Exynos 9810's always-on power processor (APM), asked through its ACPM firmware: the S2MPS18 PMIC's registers
 * (over SPEEDY, which the APM owns) and the clocks' DVFS rates. For the Galaxy S9's drivers (the GPU's supply:
 * drivers/gpu/hakios.mali.c; the touch screen's: hakios.s6sy761.c; ABOX's rate); sys/hakios.exynos_acpm.h says how to
 * call it.
 *
 * The boot loader has started the APM with its firmware; nothing is to be done first (Linux's driver maps the two
 * regions, reads the firmware's table and masks the interrupts it polls, nothing more). The AP and the APM share:
 *   the mailbox   0x14100000: INTGR0 0x08 rings the APM (bit 16 + channel), INTCR1 0x20 clears and INTSR1 0x28 shows
 *                 its answer pending for us (bit = channel); INTMR1 0x24 masks that interrupt
 *   the SRAM      0x02039000, 0x30000 bytes, of which the first three pages (below +0x3000) fault when touched: only
 *                 what lies above is mapped, and an offset below it in a table is refused. At +0x7F00 the firmware's
 *                 "initdata" -- +0x08 where the channel table is, +0x0C how many, +0x18 how many are the AP's, +0x60
 *                 two strings, its version and build time -- and every place in it an offset from the SRAM's start.
 *                 Every access 32 bits wide (the S9+ port: anything narrower is a bus error)
 *   a channel     0x48 bytes: +0x00 its id (the low byte; the rest is not ours), +0x08 its owner, +0x0C its type,
 *                 +0x10/+0x14/+0x18 where the index the APM reads at, the index we write at and the queue of our
 *                 requests are (the firmware's "rx": what it receives); +0x28/+0x2C/+0x30 the same for its answers (it
 *                 writes at +0x2C's, we read at +0x28's); +0x34 the queue's length, +0x38 its elements' size (16 or
 *                 more: four words are used), +0x44 whether the AP polls it. The two queues the wrong way round is
 *                 the one mistake the S9+ port warns of: the requests land in the answers' queue, it fills after four,
 *                 and the APM answers nothing more until the next restart
 * A request on a QUEUE (a length of 2 or more): any answer still in our queue first taken out of the way (all of them:
 * one request at a time, under the lock, so none of them is awaited -- left there, they fill the queue, and a full
 * answer queue stops the APM for the rest of the boot); four words written into the request queue at our index, the
 * index moved on, the doorbell rung; bits 21-16 of the first word a number of ours (1 to 63) that its answer carries
 * back, found between the APM's answer index and ours, copied out, and the queue emptied. 600 ms for an answer, three
 * sends (the PMIC is on a slow bus behind the APM; the S9+ port's tool waits 500 ms and has never needed more).
 * A request on a single-slot BUFFER (a length of 0 or 1, the firmware's type 2): the four words written at the request
 * queue's base, the doorbell rung, INTSR1's bit waited for (1 s), the answer read at the answer queue's base and the bit
 * cleared.
 *   the PMIC (channel 2)   word 0 = type << 8 | register (type: the S2MPS18's block, 0 top, 1 PMIC, 2 RTC -- what its
 *                          driver calls the i2c address), word 1 = function (0 read, 1 write, 2 update) | value << 8 |
 *                          mask << 16; the answer's word 1: the value [15:8], the status [31:24] (0 good, else SPEEDY
 *                          failed)
 *   DVFS (channel 0)       word 0 = the domain's index, word 1 = kHz, word 2 = 0 set, 1 get; a get's answer in word 1.
 *                          Channel 0 because that is where Samsung's kernel sends them: its acpm_dvfs client takes its
 *                          channel from the clock node's "acpm-ipc-channel", which the S9's device tree does not have,
 *                          and keeps 0. (The S9+ port's tool asks on 5, the acpm_dvfs node's -- the channel the APM
 *                          announces MIF changes on -- and finds its requests refused.) Not used for the GPU any more.
 * Sources: the phone's device tree (acpm_ipc, the mailbox, the SRAM, s2mps18mfd's acpm-ipc-channel 2) and Linux's
 * drivers/soc/samsung/acpm (acpm_ipc.c, acpm_mfd.c) and cal-if (acpm_dvfs.c, cal-if.c) of the S9's kernel; the mainline
 * Linux port to the S9+ (github.com/SheroAbi/galaxy-s9plus-mainline-linux: src/soc/s9p-acpm.c and extras/debug-tools'
 * s9p-acpm, what was measured on that phone: the faulting pages, 32-bit accesses, the queues' direction, the stale
 * answers, the timeout). All GPL: read for these facts, not one line copied.
 */
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/hakios.exynos_acpm.h>
#include "hakios.exynos9610_fvp.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern int cpu_number(void);
extern void delay(int usec);

/*
 * The SoC's: the Exynos 9810's (the S9) unless boot-arg acpm_soc=9610 (the Galaxy A50: mailbox 0x11900000, SRAM 0x15000
 * from the same 0x02039000, initdata +0x6F00, the DVFS plugin's fvmap at its base + 0x6000; the port's analysis notes, from
 * Samsung's 4.14 kernel for it and exynos9610.dts). Boot-arg acpm9610=1: passive -- the table read, no queue drained,
 * no interrupt cleared, no request sent.
 */
static unsigned long acpm_mbox_pa = 0x14100000UL;
static uint32_t acpm_sram_size = 0x30000, acpm_initdata = 0x7F00, acpm_build = 0x6C, acpm_fvmap = 0x6700;
static uint32_t acpm_passive;
#define MBOX_PA         acpm_mbox_pa
#define MBOX_INTGR0     0x08
#define MBOX_INTCR1     0x20
#define MBOX_INTMR1     0x24
#define MBOX_INTSR1     0x28
#define SRAM_PA         0x02039000UL
#define SRAM_SIZE       acpm_sram_size
#define SRAM_FIRST      0x3000          /* below: pages that fault; not mapped, never read */
#define INITDATA        acpm_initdata

#define ID_CHANNELS     0x08
#define ID_NCHANNELS    0x0C
#define ID_AP_MAX       0x18
#define ID_BUILD        acpm_build            /* seen on the phone, 1 November: three words more before build_info than the BSP's header has (0x007f0000 0x10 0x40), then "92dbdb1" */

#define CH_SIZE         0x48
#define CH_ID           0x00
#define CH_OWNER        0x08
#define CH_TYPE         0x0C
#define CH_IN_REAR      0x10    /* our requests: where the APM reads */
#define CH_IN_FRONT     0x14    /* ... where we write */
#define CH_IN_BASE      0x18
#define CH_OUT_REAR     0x28    /* its answers: where we read */
#define CH_OUT_FRONT    0x2C    /* ... where it writes */
#define CH_OUT_BASE     0x30
#define CH_QLEN         0x34
#define CH_ELEM         0x38
#define CH_AP_POLL      0x44

#define SEQ_SHIFT       16
#define SEQ_MASK        (0x3Fu << SEQ_SHIFT)
#define ROOM_US         20000   /* for a place in the request queue */
#define ANSWER_US       600000  /* for an answer, each send */
#define SENDS           3
#define BUFFER_US       1000000 /* for a buffer channel's answer */
#define POLL_US         50
#define CHANNELS_MOST   16

static vm_offset_t mbox, sram;          /* sram: SRAM_PA + SRAM_FIRST */
static struct channel {
	bool usable, buffer;
	uint32_t in_rear, in_front, in_base, out_rear, out_front, out_base, len, elem;
	uint32_t seq;
} channels[CHANNELS_MOST];
/*
 * One lock for everything here: a request, and the first call's setup, which the touch screen's, ABOX's and Mali's threads
 * may make at the same moment (each calls hakios_acpm_init() from its own thread): the second waits for the first's
 * answer instead of taking an unfinished one (acpm_tried set, acpm_ready not yet) for "no ACPM".
 */
static LCK_GRP_DECLARE(acpm_lock_group, "hakios-acpm");
static LCK_MTX_DECLARE(acpm_lock, &acpm_lock_group);
static bool acpm_ready, acpm_tried;

/* a moment `us` microseconds from now, by the clock: a loop bounded by it ends on time, however long delay() takes */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

/* an offset into the SRAM that may be touched: above the faulting pages, inside, 32-bit aligned */
static inline bool
sram_ok(uint32_t off, uint32_t bytes)
{
	return off >= SRAM_FIRST && off <= SRAM_SIZE && bytes <= SRAM_SIZE - off && (off & 3) == 0;
}

static inline uint32_t
srd(uint32_t off)
{
	return rd(sram, off - SRAM_FIRST);
}

static inline void
swr(uint32_t off, uint32_t v)
{
	wr(sram, off - SRAM_FIRST, v);
}

static bool
printable(uint32_t off, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		uint8_t c = (uint8_t)(srd((off + i) & ~3u) >> (8 * ((off + i) & 3)));

		if (c == 0) {
			return i > 0;
		}
		if (c < 0x20 || c > 0x7e) {
			return false;
		}
	}
	return true;
}

static void
copy_string(uint32_t off, char *out, uint32_t len)
{
	uint32_t i;

	for (i = 0; i + 1 < len; i++) {
		char c = (char)(srd((off + i) & ~3u) >> (8 * ((off + i) & 3)));

		if (c == 0) {
			break;
		}
		out[i] = c;
	}
	out[i] = 0;
}

/* every answer in a queue channel's answer queue taken out (none is awaited), and its interrupt cleared */
static void
drain(struct channel *c, uint32_t id)
{
	uint32_t front = srd(c->out_front);

	if (front < c->len && srd(c->out_rear) != front) {
		swr(c->out_rear, front);
	}
	wr(mbox, MBOX_INTCR1, 1u << id);
}

static bool
acpm_setup(int (*say)(const char *, ...))
{
	uint32_t table, count, ap_max;
	char version[26], built[26];

	mbox = ml_io_map(MBOX_PA, 0x1000);
	sram = ml_io_map(SRAM_PA + SRAM_FIRST, SRAM_SIZE - SRAM_FIRST);
	if (mbox == 0 || sram == 0) {
		say("acpm: cannot map the mailbox (%#lx) or the APM's SRAM (%#lx)\n", MBOX_PA, SRAM_PA + SRAM_FIRST);
		return false;
	}
	if (!printable(INITDATA + ID_BUILD, 25) && printable(INITDATA + 0x64, 25)) {
		say("acpm: the version string at initdata +0x64 (the BSP's header), not +%#x\n", acpm_build);
		acpm_build = 0x64;
	}
	if (!printable(INITDATA + ID_BUILD, 25)) {
		say("acpm: no firmware's table at SRAM +%#x (its version string is not text): not used\n", INITDATA);
		for (uint32_t o = 0; o < 0x80; o += 0x20) {
			say("acpm: SRAM +%#x: %08x %08x %08x %08x %08x %08x %08x %08x\n", INITDATA + o, srd(INITDATA + o),
			    srd(INITDATA + o + 4), srd(INITDATA + o + 8), srd(INITDATA + o + 12), srd(INITDATA + o + 16),
			    srd(INITDATA + o + 20), srd(INITDATA + o + 24), srd(INITDATA + o + 28));
		}
		return false;
	}
	copy_string(INITDATA + ID_BUILD, version, sizeof(version));
	copy_string(INITDATA + ID_BUILD + 25, built, sizeof(built));
	table = srd(INITDATA + ID_CHANNELS);
	count = srd(INITDATA + ID_NCHANNELS);
	ap_max = srd(INITDATA + ID_AP_MAX);
	say("acpm: firmware \"%s\" built \"%s\"; %u channels at SRAM +%#x, %u the AP's; mailbox INTMR1 %#x, INTSR1 %#x\n",
	    version, built, count, table, ap_max, rd(mbox, MBOX_INTMR1), rd(mbox, MBOX_INTSR1));
	if (ap_max > count || count > 64 || !sram_ok(table, count * CH_SIZE)) {
		say("acpm: the table is not where it may be read (above SRAM +%#x, inside): not used\n", SRAM_FIRST);
		return false;
	}
	for (uint32_t i = 0; i < ap_max; i++) {
		uint32_t at = table + i * CH_SIZE, id = srd(at + CH_ID) & 0xff;
		struct channel *c;
		struct channel n = { 0 };

		n.in_rear = srd(at + CH_IN_REAR);
		n.in_front = srd(at + CH_IN_FRONT);
		n.in_base = srd(at + CH_IN_BASE);
		n.out_rear = srd(at + CH_OUT_REAR);
		n.out_front = srd(at + CH_OUT_FRONT);
		n.out_base = srd(at + CH_OUT_BASE);
		n.len = srd(at + CH_QLEN);
		n.elem = srd(at + CH_ELEM);
		n.buffer = n.len <= 1;
		say("acpm: channel %u: owner %u, type %u, %s of %u x %u bytes, %s; requests at +%#x, answers at +%#x\n", id,
		    srd(at + CH_OWNER), srd(at + CH_TYPE), n.buffer ? "a single-slot buffer" : "a queue", n.len, n.elem,
		    srd(at + CH_AP_POLL) ? "polled" : "by interrupt", n.in_base, n.out_base);
		if (id >= CHANNELS_MOST) {
			continue;
		}
		if (n.buffer) {
			n.usable = sram_ok(n.in_base, 16) && sram_ok(n.out_base, 16);
		} else {
			n.usable = n.len <= 64 && n.elem >= 16 && n.elem <= 256 && sram_ok(n.in_rear, 4) &&
			    sram_ok(n.in_front, 4) && sram_ok(n.out_rear, 4) && sram_ok(n.out_front, 4) &&
			    sram_ok(n.in_base, n.len * n.elem) && sram_ok(n.out_base, n.len * n.elem);
		}
		if (!n.usable) {
			say("acpm: channel %u: a place in it is outside what may be read: not used\n", id);
			continue;
		}
		c = &channels[id];
		*c = n;
		if (!c->buffer && !acpm_passive) {
			drain(c, id);   /* answers nobody read (a kernel before this one, or none): out of the way */
		}
	}
	say("acpm: the PMIC's channel %s, DVFS's %s\n", channels[HAKIOS_ACPM_CH_PMIC].usable ? "usable" : "NOT usable",
	    channels[HAKIOS_ACPM_CH_DVFS].usable ? "usable" : "NOT usable");
	return channels[HAKIOS_ACPM_CH_PMIC].usable || channels[HAKIOS_ACPM_CH_DVFS].usable;
}

bool
hakios_acpm_init(int (*say)(const char *, ...))
{
	bool ready;

	lck_mtx_lock(&acpm_lock);
	if (!acpm_tried) {
		acpm_tried = true;
		acpm_ready = acpm_setup(say);
	}
	ready = acpm_ready;
	lck_mtx_unlock(&acpm_lock);
	return ready;
}

/* the answer with our number, if it has come: copied out, and the answer queue emptied */
static bool
take_answer(struct channel *c, uint32_t id, uint32_t seq, uint32_t cmd[4])
{
	uint32_t rear = srd(c->out_rear), front = srd(c->out_front);
	bool found = false;

	if (rear >= c->len || front >= c->len || (rear == front && !(rd(mbox, MBOX_INTSR1) & (1u << id)))) {
		return false;
	}
	for (uint32_t i = rear; i != front; i = (i + 1) % c->len) {
		uint32_t slot = c->out_base + i * c->elem;

		if (((srd(slot) & SEQ_MASK) >> SEQ_SHIFT) == seq) {
			for (uint32_t w = 0; w < 4; w++) {
				cmd[w] = srd(slot + 4 * w);
			}
			found = true;
		}
	}
	drain(c, id);
	return found;
}

static int
request_queue(struct channel *c, uint32_t id, uint32_t cmd[4])
{
	uint32_t sent[4], front, next;
	uint64_t deadline;

	for (uint32_t w = 0; w < 4; w++) {
		sent[w] = cmd[w];
	}
	for (int send = 0; send < SENDS; send++) {
		drain(c, id);                   /* never into a queue with stale answers in it */
		front = srd(c->in_front);
		if (front >= c->len) {
			return EIO;
		}
		next = (front + 1) % c->len;
		for (deadline = deadline_us(ROOM_US); srd(c->in_rear) == next && mach_absolute_time() < deadline;) {
			delay(POLL_US);         /* the queue is full: the APM has not read our last requests yet */
		}
		if (srd(c->in_rear) == next) {
			continue;
		}
		c->seq = c->seq % 63 + 1;
		sent[0] = (sent[0] & ~SEQ_MASK) | c->seq << SEQ_SHIFT;
		for (uint32_t w = 0; w < 4; w++) {
			swr(c->in_base + front * c->elem + 4 * w, sent[w]);
		}
		__builtin_arm_dsb(0xf);         /* DSB SY: the words before the index, the index before the doorbell */
		swr(c->in_front, next);
		__builtin_arm_dsb(0xf);
		wr(mbox, MBOX_INTGR0, 1u << (16 + id));
		for (deadline = deadline_us(ANSWER_US); mach_absolute_time() < deadline;) {
			if (take_answer(c, id, c->seq, cmd)) {
				return 0;
			}
			delay(POLL_US);
		}
		if (take_answer(c, id, c->seq, cmd)) {
			return 0;
		}
	}
	return ETIMEDOUT;
}

static int
request_buffer(struct channel *c, uint32_t id, uint32_t cmd[4])
{
	uint64_t deadline;

	c->seq = c->seq % 63 + 1;
	cmd[0] = (cmd[0] & ~SEQ_MASK) | c->seq << SEQ_SHIFT;
	wr(mbox, MBOX_INTCR1, 1u << id);
	for (uint32_t w = 0; w < 4; w++) {
		swr(c->in_base + 4 * w, cmd[w]);
	}
	__builtin_arm_dsb(0xf);
	wr(mbox, MBOX_INTGR0, 1u << (16 + id));
	for (deadline = deadline_us(BUFFER_US); !(rd(mbox, MBOX_INTSR1) & (1u << id));) {
		if (mach_absolute_time() >= deadline) {
			return ETIMEDOUT;
		}
		delay(POLL_US);
	}
	for (uint32_t w = 0; w < 4; w++) {
		cmd[w] = srd(c->out_base + 4 * w);
	}
	wr(mbox, MBOX_INTCR1, 1u << id);
	return 0;
}

int
hakios_acpm_request(uint32_t id, uint32_t cmd[4])
{
	int error;

	if (!acpm_ready || id >= CHANNELS_MOST || !channels[id].usable || acpm_passive) {
		return ENXIO;           /* (acpm9610=1: passive, nothing sent) */
	}
	lck_mtx_lock(&acpm_lock);
	error = channels[id].buffer ? request_buffer(&channels[id], id, cmd) : request_queue(&channels[id], id, cmd);
	lck_mtx_unlock(&acpm_lock);
	return error;
}

int
hakios_acpm_pmic_read(uint32_t type, uint32_t reg, uint8_t *value)
{
	uint32_t cmd[4] = { (type & 0xf) << 8 | (reg & 0xff), 0, 0, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_PMIC, cmd);

	if (error == 0 && (cmd[1] >> 24) != 0) {
		error = EIO;
	}
	if (error == 0) {
		*value = (uint8_t)(cmd[1] >> 8);
	}
	return error;
}

int
hakios_acpm_pmic_write(uint32_t type, uint32_t reg, uint8_t value)
{
	uint32_t cmd[4] = { (type & 0xf) << 8 | (reg & 0xff), 1u | (uint32_t)value << 8, 0, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_PMIC, cmd);

	return error == 0 && (cmd[1] >> 24) != 0 ? EIO : error;
}

int
hakios_acpm_pmic_update(uint32_t type, uint32_t reg, uint8_t value, uint8_t mask)
{
	uint32_t cmd[4] = { (type & 0xf) << 8 | (reg & 0xff), 2u | (uint32_t)value << 8 | (uint32_t)mask << 16, 0, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_PMIC, cmd);

	return error == 0 && (cmd[1] >> 24) != 0 ? EIO : error;
}

int
hakios_acpm_dvfs_get(uint32_t index, uint32_t *khz)
{
	uint32_t cmd[4] = { index, 0, 1, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, cmd);

	if (error == 0) {
		*khz = cmd[1];
	}
	return error;
}

int
hakios_acpm_dvfs_set(uint32_t index, uint32_t khz)
{
	uint32_t cmd[4] = { index, khz, 0, 0 };

	return hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, cmd);
}

/*
 * A DVFS domain's levels as the APM holds them, read only: its rate and voltage per level, the fastest first. Samsung's
 * kernel reads the same "fvmap" once at start (cal-if/fvmap.c fvmap_copy_from_sram, from acpm/acpm.c plugins_init) and
 * only reads it: the voltages in it are the ones the APM itself applies with each rate (ASV already applied by the boot;
 * Linux adds a margin to them only when its command line asks). A DVFS request names a rate, never a voltage.
 *   initdata +0x00   where the APM's plugins are (an offset into the SRAM), +0x04 how many (acpm_framework, framework.h)
 *   a plugin         0x30 bytes (struct plugin, fw_header/common.h, as the 32-bit APM lays it out): +0x04 its base (bit 0
 *                    a flag), +0x20 attached (a byte), +0x24 its size, +0x28 stays attached (a byte), +0x2C its name
 *   the DVFS plugin  the one whose name has "dvfs" in it; its map at base + the acpm node's fvmap_offset (0x6700 in the
 *                    phone's device tree) when the boot left it attached, at base + size when Linux loads it itself
 *   the map          a 24-byte header per domain, in the order of the DVFS indexes (struct fvmap_header, fvmap.h): +0x01
 *                    how many levels, +0x14 (16 bits) where they are from the map's start; a level is two words, the
 *                    rate (kHz) and the voltage (uV)
 * Returns how many levels it read into khz[] and uv[] (at most `most`), 0 when the map is not found or does not look
 * like one (each step said with `say`).
 */
#define ID_PLUGINS      0x00
#define ID_NPLUGINS     0x04
#define PL_SIZE         0x30
#define PL_BASE         0x04
#define PL_ATTACHED     0x20
#define PL_FWSIZE       0x24
#define PL_STAYS        0x28
#define PL_NAME         0x2C
#define FVMAP_OFFSET    acpm_fvmap          /* the device tree's acpm { fvmap_offset = <26368>; } */
#define FV_HEADER       24
#define FV_NUM_LV       0x01
#define FV_O_RATEVOLT   0x14

static uint32_t
srd8(uint32_t off)
{
	return (srd(off & ~3u) >> (8 * (off & 3))) & 0xff;
}

static uint32_t
srd16(uint32_t off)
{
	return (srd(off & ~3u) >> (8 * (off & 2))) & 0xffff;
}

static bool
names_dvfs(const char *name)
{
	for (const char *p = name; p[0] && p[1] && p[2] && p[3]; p++) {
		if ((p[0] | 0x20) == 'd' && (p[1] | 0x20) == 'v' && (p[2] | 0x20) == 'f' && (p[3] | 0x20) == 's') {
			return true;
		}
	}
	return false;
}

uint32_t
hakios_acpm_dvfs_table(uint32_t index, uint32_t *khz, uint32_t *uv, uint32_t most, int (*say)(const char *, ...))
{
	uint32_t plugins, count, map = 0, header, levels, at;
	char name[24];

	if (!acpm_ready) {
		return 0;
	}
	plugins = srd(INITDATA + ID_PLUGINS);
	count = srd(INITDATA + ID_NPLUGINS);
	if (count == 0 || count > 32 || !sram_ok(plugins, count * PL_SIZE)) {
		say("acpm: fvmap: the plugins' table (+%#x, %u of them) is not where it may be read\n", plugins, count);
		return 0;
	}
	for (uint32_t i = 0; i < count && map == 0; i++) {
		uint32_t p = plugins + i * PL_SIZE, at_name = srd(p + PL_NAME), base = srd(p + PL_BASE) & ~1u;
		uint32_t attached = srd8(p + PL_ATTACHED), stays = srd8(p + PL_STAYS), size = srd(p + PL_FWSIZE);

		if (at_name == 0 || !sram_ok(at_name & ~3u, sizeof(name) + 4) || !printable(at_name, sizeof(name) - 1)) {
			continue;
		}
		copy_string(at_name, name, sizeof(name));
		if (!names_dvfs(name)) {
			continue;
		}
		map = base + (attached ? FVMAP_OFFSET : size);
		say("acpm: fvmap: plugin %u \"%s\": base +%#x, attached %u, stays %u, size %#x: the map at +%#x\n", i, name,
		    base, attached, stays, size, map);
	}
	if (map == 0) {
		say("acpm: fvmap: no plugin named for DVFS among %u\n", count);
		return 0;
	}
	header = map + index * FV_HEADER;
	if (!sram_ok(header & ~3u, FV_HEADER + 4)) {
		say("acpm: fvmap: domain %u's header (+%#x) is not where it may be read\n", index, header);
		return 0;
	}
	levels = srd8(header + FV_NUM_LV);
	at = map + srd16(header + FV_O_RATEVOLT);
	if (levels == 0 || levels > most || !sram_ok(at, levels * 8)) {
		say("acpm: fvmap: domain %u: %u levels at +%#x: not a table (at most %u, inside the SRAM)\n", index, levels, at,
		    most);
		return 0;
	}
	for (uint32_t l = 0; l < levels; l++) {
		khz[l] = srd(at + 8 * l);
		uv[l] = srd(at + 8 * l + 4);
		if (khz[l] < 100000 || khz[l] > 4000000 || uv[l] < 300000 || uv[l] > 1500000 || (l > 0 && khz[l] >= khz[l - 1])) {
			say("acpm: fvmap: domain %u: level %u reads %u kHz %u uV: not a table (the fastest first, kHz and uV)\n",
			    index, l, khz[l], uv[l]);
			return 0;
		}
	}
	return levels;
}

/*
 * The APM asked to stop, as Samsung's kernel does before every power-off and restart (exynos_acpm_reboot() ->
 * acpm_enter_wfi(), drivers/soc/samsung/acpm/acpm.c:250-292): one request, word 0 the protocol's STOP bit (bit 22,
 * ACPM_IPC_PROTOCOL_STOP in include/soc/samsung/acpm_ipc_ctrl.h), on the channel of the acpm node itself (4 in the S9's
 * device tree; acpm_dvfs's is 5, s2mps18's 2). Samsung skips it on the 9810's first revision (EVT0) only. Its answer is
 * waited for as any other's; after it the APM answers nothing more, so nothing may come after it but PS_HOLD or a restart.
 */
#define ACPM_STOP       (1u << 22)

int
hakios_acpm_stop(void)
{
	uint32_t cmd[4] = { ACPM_STOP, 0, 0, 0 };

	return hakios_acpm_request(HAKIOS_ACPM_CH_ACPM, cmd);
}

/*
 * The Galaxy S9's CPU clocks through ACPM's DVFS (Samsung's cal_dfs_set_rate -> exynos_acpm_set_rate: domain index,
 * kHz, FREQ_REQ). acpm_dvfs_exynos9810.h: dvfs_cpucl0 (the A55s) is index 2, dvfs_cpucl1 (the M3s) index 3.
 * boot-args cpufreq_lit=kHz, cpufreq_big=kHz; nothing without them. With cpufreq_gov=1 the governor
 * (drivers/power/hakios.exynos_cpufreq.c) sets both clusters' rates instead, and these two are not used.
 */
extern void loader_log_line(const char *line);
void hakios_exynos_dvfs_start(void);

/* printf, and the loader's log too (the passive probe's lines are what the run is for) */
static int dvfs_say(const char *format, ...) __printflike(1, 2);
static int
dvfs_say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

/*
 * acpm9610=4 (the Galaxy A50): the APM's plugins said; then, if the "fvp" plugin (id 2, the clock DVFS channel's
 * go-between) is not attached, its code (hakios.exynos9610_fvp.h) copied into its slot in the SRAM and attached, as
 * Samsung's kernel does at boot (acpm.c plugins_init: firmware_load's memcpy to SRAM + base, then handle_dynamic_plugin:
 * cmd[0] = 1 << 25 | id << 26 on the acpm node's channel, 4), and the three domains' rates asked again. Only when its
 * descriptor is as expected: id 2, not attached, base 0x9000 (the binary's link address), a slot of at least its size
 * inside the SRAM; the copy read back byte for byte before the attach. No descriptor field is written here; no rate is
 * set. the port's analysis notes.
 */
#define FVP_ID          2
#define FVP_BASE        0x9000
#define ACPM_ATTACH     (1u << 25)
#define ACPM_ID_SHIFT   26

/* a queue channel's four indexes, said: requests (APM's rear = taken, our front = put) and answers */
static void
fvp_queues(const char *when)
{
	static const uint32_t ids[] = { HAKIOS_ACPM_CH_DVFS, HAKIOS_ACPM_CH_ACPM, HAKIOS_ACPM_CH_PMIC };

	for (uint32_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		struct channel *c = &channels[ids[i]];

		if (c->usable && !c->buffer) {
			dvfs_say("acpm: fvp: %s: channel %u: requests taken %u / put %u, answers read %u / written %u (of %u); "
			    "INTSR1 %#x, INTMR1 %#x\n", when, ids[i], srd(c->in_rear), srd(c->in_front), srd(c->out_rear),
			    srd(c->out_front), c->len, rd(mbox, MBOX_INTSR1), rd(mbox, MBOX_INTMR1));
		}
	}
	/* the mailbox, both sides, read only: 0x08 INTGR0 .. 0x18 INTMSR0 (the APM's), 0x1C INTGR1 .. 0x2C INTMSR1 (ours) */
	dvfs_say("acpm: fvp: %s: mailbox +0x00..+0x2c: %#x %#x | GR0 %#x CR0 %#x MR0 %#x SR0 %#x MSR0 %#x | GR1 %#x CR1 %#x "
	    "MR1 %#x SR1 %#x MSR1 %#x\n", when, rd(mbox, 0x00), rd(mbox, 0x04), rd(mbox, 0x08), rd(mbox, 0x0C), rd(mbox, 0x10),
	    rd(mbox, 0x14), rd(mbox, 0x18), rd(mbox, 0x1C), rd(mbox, 0x20), rd(mbox, 0x24), rd(mbox, 0x28), rd(mbox, 0x2C));
}

/*
 * acpm9610=5 (the port's analysis notes, tests 1-3): the APM framework's own state, read from its SRAM (APM addresses are
 * SRAM offsets): its log ring (rear +0x7708, front +0x770C, 252 records of 16 bytes from +0x7710: header
 * id[31:28] level[27] time[26:22] count[15:0], 8 characters, a value), intr_to_skip (initdata +0x5C = +0x6F5C), the
 * interrupt table's pointer (+0x61AC) and the mailbox entry's is_disabled byte, the credits (+0x61B4, 8 words).
 */
#define APM_LOG_REAR    0x7708
#define APM_LOG_FRONT   0x770C
#define APM_LOG_DATA    0x7710
#define APM_LOG_N       252
static void
apm_diag(const char *when, uint32_t last)
{
	uint32_t rear = srd(APM_LOG_REAR), front = srd(APM_LOG_FRONT), vec = srd(0x61AC);

	dvfs_say("acpm: apm: %s: intr_to_skip %#x, intr table %#x (mailbox entry: field %#x handler %#x disabled %u), "
	    "credits %u %u %u %u %u %u %u %u, log rear %u front %u\n", when, srd(0x6F5C), vec,
	    sram_ok(vec + 0x10, 0x10) ? srd(vec + 0x10) : 0, sram_ok(vec + 0x10, 0x10) ? srd(vec + 0x14) : 0,
	    sram_ok(vec + 0x10, 0x10) ? srd8(vec + 0x1C) : 0xff, srd(0x61B4), srd(0x61B8), srd(0x61BC), srd(0x61C0),
	    srd(0x61C4), srd(0x61C8), srd(0x61CC), srd(0x61D0), rear, front);
	fvp_queues(when);
	if (last == 0 || rear >= APM_LOG_N) {
		return;
	}
	/* the last `last` records before rear (the newest), oldest first */
	for (uint32_t k = last; k > 0; k--) {
		uint32_t i = (rear + APM_LOG_N - k) % APM_LOG_N, r = APM_LOG_DATA + 16 * i;
		uint32_t h = srd(r), a = srd(r + 4), b = srd(r + 8);
		char str[9];

		if (h == 0 && a == 0 && b == 0) {
			continue;
		}
		for (int c = 0; c < 4; c++) {
			char x = (char)(a >> (8 * c)), y = (char)(b >> (8 * c));
			str[c] = (x >= 32 && x < 127) ? x : (x == 0 ? ' ' : '.');
			str[4 + c] = (y >= 32 && y < 127) ? y : (y == 0 ? ' ' : '.');
		}
		str[8] = 0;
		dvfs_say("acpm: apm log [%3u] id %u lvl %u t %2u n %5u %s %#x\n", i, h >> 28, h >> 27 & 1, h >> 22 & 31,
		    h & 0xffff, str, srd(r + 12));
	}
}

static void
hakios_acpm_fvp9610(uint32_t got[3], int got_e[3], const char *const names[3])
{
	uint32_t plugins = srd(INITDATA + ID_PLUGINS), count = srd(INITDATA + ID_NPLUGINS), fvp = 0, found = 0;

	if (count == 0 || count > 32 || !sram_ok(plugins, count * PL_SIZE)) {
		dvfs_say("acpm: fvp: the plugins' table (+%#x, %u) is not where it may be read\n", plugins, count);
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint32_t p = plugins + i * PL_SIZE, at_name = srd(p + PL_NAME);
		char name[32] = "?";

		if (at_name != 0 && sram_ok(at_name & ~3u, sizeof(name) + 4) && printable(at_name, sizeof(name) - 1)) {
			copy_string(at_name, name, sizeof(name));
		}
		dvfs_say("acpm: plugin %u: id %u, base %#x, ops %#x/%#x, attached %u, size %#x, stays %u, \"%s\"\n", i,
		    srd(p), srd(p + PL_BASE), srd(p + 0x08), srd(p + 0x0C), srd8(p + PL_ATTACHED), srd(p + PL_FWSIZE),
		    srd8(p + PL_STAYS), name);
		if (srd(p) == FVP_ID) {
			fvp = p;
			found++;
		}
	}
	if (found != 1) {
		dvfs_say("acpm: fvp: %u descriptors with id %u: nothing done\n", found, FVP_ID);
		return;
	}
	{
		uint32_t base = srd(fvp + PL_BASE) & ~1u, size = srd(fvp + PL_FWSIZE), attached = srd8(fvp + PL_ATTACHED);
		uint32_t cmd[4] = { ACPM_ATTACH | (FVP_ID << ACPM_ID_SHIFT), 0, 0, 0 };
		uint32_t diff = 0;
		int e;

		if (HAKIOS_FVP_9610_SIZE == 0) {
			/* the repository's placeholder: tools/a50-fvp.py makes the real header from the phone's own stock BOOT */
			dvfs_say("acpm: fvp: no plugin built in (hakios.exynos9610_fvp.h is the placeholder: tools/a50-fvp.py): "
			    "nothing done\n");
			return;
		} else if (attached) {
			dvfs_say("acpm: fvp: attached already: not loaded again\n");
		} else if (base != FVP_BASE || size < HAKIOS_FVP_9610_SIZE || !sram_ok(base, HAKIOS_FVP_9610_SIZE)) {
			dvfs_say("acpm: fvp: base %#x size %#x: not the slot expected (base %#x, at least %#x, inside the readable "
			    "SRAM): nothing done\n", base, size, FVP_BASE, HAKIOS_FVP_9610_SIZE);
			return;
		} else {
			for (uint32_t o = 0; o < HAKIOS_FVP_9610_SIZE; o += 4) {
				swr(base + o, (uint32_t)hakios_fvp_9610[o] | (uint32_t)hakios_fvp_9610[o + 1] << 8 |
				    (uint32_t)hakios_fvp_9610[o + 2] << 16 | (uint32_t)hakios_fvp_9610[o + 3] << 24);
			}
			__builtin_arm_dsb(0xf);     /* DSB SY */
			for (uint32_t o = 0; o < HAKIOS_FVP_9610_SIZE; o += 4) {
				uint32_t w = (uint32_t)hakios_fvp_9610[o] | (uint32_t)hakios_fvp_9610[o + 1] << 8 |
				    (uint32_t)hakios_fvp_9610[o + 2] << 16 | (uint32_t)hakios_fvp_9610[o + 3] << 24;

				diff += srd(base + o) != w;
			}
			dvfs_say("acpm: fvp: %u bytes copied to SRAM +%#x, %u words differ on reading back\n", HAKIOS_FVP_9610_SIZE,
			    base, diff);
			if (diff != 0) {
				return;
			}
			fvp_queues("before the attach");
			e = hakios_acpm_request(HAKIOS_ACPM_CH_ACPM, cmd);
			fvp_queues("after the attach");
			dvfs_say("acpm: fvp: attach (cmd[0] %#x on channel %u): %d, answer %#x %#x; the descriptor now: attached %u, "
			    "ops %#x/%#x\n", ACPM_ATTACH | (FVP_ID << ACPM_ID_SHIFT), HAKIOS_ACPM_CH_ACPM, e, cmd[0], cmd[1],
			    srd8(fvp + PL_ATTACHED), srd(fvp + 0x08), srd(fvp + 0x0C));
			if (e != 0) {
				return;
			}
		}
		for (uint32_t d = 0; d < 3; d++) {
			uint32_t khz = 0;

			e = hakios_acpm_dvfs_get(2 + d, &khz);
			dvfs_say("acpm: fvp: GET %s: %u kHz (%d; before the attach %u kHz, %d)\n", names[d], khz, e, got[d], got_e[d]);
		}
	}
}
/*
 * cpu9610=N (the Galaxy A50): the CPU clusters' rates set by this kernel, as hakios.mali.c sets the GPU's -- ACPM's DVFS
 * plugin, without Linux's set-up, answers a SET and changes nothing. The rail first, through ACPM's PMIC channel (S2MPU09
 * BUCK2 vdd_cpucl1 OUT 0x1B, BUCK3 vdd_cpucl0 OUT 0x1D: 300 mV + 6.25 mV a step), then the PLL in the cluster's CMU
 * (cal-if/exynos9610: CMU_CPUCL1 0x10800000 PLL_CON0 +0x120, CMU_CPUCL0 0x10900000 PLL_CON0 +0x140; S [2:0], P [13:8],
 * M [25:16], MUX_SEL bit 4, MUX_BUSY 7, STABLE 29, ENABLE 31): the cluster on the 26 MHz oscillator (MUX_SEL cleared)
 * while the PLL relocks, then back. The rates and their dividers are Samsung's own rows (cmucal-node.c pll_cpucl*_rate_table,
 * cmucal-vclk*.c vdd_cpucl1: DIV_CLK_CLUSTER1_ACLK +0x1800 = 1 from 1499 MHz up); the voltages the phone's fvmap plus a step.
 *   1  read only: both clusters' PLL, dividers and rails said
 *   2  set: A73s cl1_mhz= (1898; or 1499), A53s cl0_mhz= (1449; or 1049, S-Boot's), each only from a row below
 */
static const struct cpu_rate { uint32_t mhz, m, p, s, aclk_div, uv; } cl1_rates[] = {
	{ 1898, 292, 4, 0, 1, 1000000 },        /* fvmap 1898000 kHz: 987500 uV */
	{ 1499, 346, 3, 1, 1, 843750 },         /* fvmap 1508000: 831250 */
}, cl0_rates[] = {
	{ 1449, 223, 4, 0, 0xff, 950000 },      /* fvmap 1456000: 937500 (the A53s' ACLK divider left as found) */
	{ 1049, 323, 4, 1, 0xff, 806250 },      /* fvmap 1053000: 793750 */
};
#define CPU_MUX_SEL     (1u << 4)
#define CPU_MUX_BUSY    (1u << 7)
#define CPU_PLL_STABLE  (1u << 29)
#define CPU_PLL_ENABLE  (1u << 31)
#define CPU_PLL_PMS     ((0x3ffu << 16) | (0x3fu << 8) | 7u)

static bool
cpu_wait(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t deadline = deadline_us(us);

	while ((rd(base, off) & mask) != want) {
		if (mach_absolute_time() >= deadline) {
			return (rd(base, off) & mask) == want;
		}
	}
	return true;
}

static void
cpu9610_cluster(const char *name, uint64_t cmu_pa, uint32_t con0_off, uint32_t rail, const struct cpu_rate *r, bool set)
{
	vm_offset_t cmu = ml_io_map(cmu_pa, 0x2000);
	uint32_t con0, aclk, cpu, lock;
	uint8_t out = 0;
	int e;

	if (cmu == 0) {
		dvfs_say("acpm: cpu: %s: CMU not mapped\n", name);
		return;
	}
	con0 = rd(cmu, con0_off);
	aclk = rd(cmu, 0x1800);
	cpu = rd(cmu, 0x1810);
	lock = rd(cmu, 0x0000);
	e = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, rail, &out);
	dvfs_say("acpm: cpu: %s: PLL_CON0 %#x (M %u P %u S %u: %u MHz; %s, %s, the cluster on the %s), LOCKTIME %u, "
	    "DIV ACLK %#x CPU %#x; rail %#x (%d) = %u uV\n", name, con0, con0 >> 16 & 0x3ff, con0 >> 8 & 0x3f, con0 & 7,
	    (con0 >> 8 & 0x3f) ? 26 * (con0 >> 16 & 0x3ff) / (con0 >> 8 & 0x3f) >> (con0 & 7) : 0,
	    (con0 & CPU_PLL_ENABLE) ? "enabled" : "OFF", (con0 & CPU_PLL_STABLE) ? "stable" : "NOT stable",
	    (con0 & CPU_MUX_SEL) ? "PLL" : "oscillator", lock, aclk, cpu, out, e, 300000 + 6250 * (uint32_t)out);
	if (!set || r == NULL) {
		return;
	}
	if (!(con0 & CPU_PLL_ENABLE) || !(con0 & CPU_MUX_SEL) || e != 0) {
		dvfs_say("acpm: cpu: %s: not as expected (PLL off, the cluster not on it, or the rail unread): left alone\n", name);
		return;
	}
	{
		uint8_t want = (uint8_t)((r->uv - 300000 + 6249) / 6250), back = 0;
		uint32_t v;
		const char *failed = NULL;

		if (want > out) {
			e = hakios_acpm_pmic_write(HAKIOS_ACPM_PMIC_PMIC, rail, want);
			(void)hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, rail, &back);
			dvfs_say("acpm: cpu: %s: rail %#x -> %#x (%d), reads %#x = %u uV\n", name, out, want, e, back,
			    300000 + 6250 * (uint32_t)back);
			if (e != 0 || back != want) {
				dvfs_say("acpm: cpu: %s: the rail not raised: the rate left alone\n", name);
				return;
			}
			delay(300);
		}
		if (r->aclk_div != 0xff && (aclk & 7) < r->aclk_div) {
			wr(cmu, 0x1800, (aclk & ~7u) | r->aclk_div);       /* the cluster's bus slower first, before its clock rises */
			(void)cpu_wait(cmu, 0x1800, 1u << 16, 0, 1000);
		}
		wr(cmu, con0_off, rd(cmu, con0_off) & ~CPU_MUX_SEL);
		if (!cpu_wait(cmu, con0_off, CPU_MUX_BUSY, 0, 1000)) {
			failed = "the mux did not move to the oscillator";
		} else {
			v = rd(cmu, con0_off) & ~(CPU_PLL_ENABLE | CPU_PLL_PMS);
			wr(cmu, con0_off, v);
			v |= r->m << 16 | r->p << 8 | r->s;
			wr(cmu, con0_off, v);
			wr(cmu, con0_off, v | CPU_PLL_ENABLE);
			if (!cpu_wait(cmu, con0_off, CPU_PLL_STABLE, CPU_PLL_STABLE, 5000)) {
				failed = "the PLL did not lock";
			}
		}
		if (failed != NULL) {
			/* back as found: the old M/P/S, locked, the cluster on it */
			wr(cmu, con0_off, (con0 & ~(CPU_PLL_ENABLE | CPU_MUX_SEL)));
			wr(cmu, con0_off, (con0 & ~CPU_MUX_SEL));
			(void)cpu_wait(cmu, con0_off, CPU_PLL_STABLE, CPU_PLL_STABLE, 5000);
		}
		wr(cmu, con0_off, rd(cmu, con0_off) | CPU_MUX_SEL);
		(void)cpu_wait(cmu, con0_off, CPU_MUX_BUSY, 0, 1000);
		if (failed != NULL) {
			wr(cmu, 0x1800, aclk);
		}
		con0 = rd(cmu, con0_off);
		dvfs_say("acpm: cpu: %s: %s: PLL_CON0 %#x (M %u P %u S %u: %u MHz, %s, on the %s), DIV ACLK %#x\n", name,
		    failed ? failed : "switched", con0, con0 >> 16 & 0x3ff, con0 >> 8 & 0x3f, con0 & 7,
		    (con0 >> 8 & 0x3f) ? 26 * (con0 >> 16 & 0x3ff) / (con0 >> 8 & 0x3f) >> (con0 & 7) : 0,
		    (con0 & CPU_PLL_STABLE) ? "stable" : "NOT stable", (con0 & CPU_MUX_SEL) ? "PLL" : "oscillator",
		    rd(cmu, 0x1800));
	}
}

static void
cpu9610(uint32_t level)
{
	uint32_t cl1 = 1898, cl0 = 1449;
	const struct cpu_rate *r1 = NULL, *r0 = NULL;

	PE_parse_boot_argn("cl1_mhz", &cl1, sizeof(cl1));
	PE_parse_boot_argn("cl0_mhz", &cl0, sizeof(cl0));
	for (unsigned i = 0; i < sizeof(cl1_rates) / sizeof(cl1_rates[0]); i++) {
		r1 = cl1_rates[i].mhz == cl1 ? &cl1_rates[i] : r1;
	}
	for (unsigned i = 0; i < sizeof(cl0_rates) / sizeof(cl0_rates[0]); i++) {
		r0 = cl0_rates[i].mhz == cl0 ? &cl0_rates[i] : r0;
	}
	if (!hakios_acpm_init(dvfs_say)) {
		dvfs_say("acpm: cpu: no ACPM (the rails): nothing done\n");
		return;
	}
	dvfs_say("acpm: cpu: cpu9610=%u on cpu %d: A73s %u MHz%s, A53s %u MHz%s\n", level, cpu_number(), cl1,
	    r1 ? "" : " (no such row: left)", cl0, r0 ? "" : " (no such row: left)");
	cpu9610_cluster("cpucl1 (the A73s)", 0x10800000ULL, 0x120, 0x1B, r1, level >= 2);
	cpu9610_cluster("cpucl0 (the A53s)", 0x10900000ULL, 0x140, 0x1D, r0, level >= 2);
}

void
hakios_exynos_dvfs_start(void)
{
	static const struct { const char *arg, *name; uint32_t index; } doms[] = {
		{ "cpufreq_lit", "the A55s (cpucl0)", 2 }, { "cpufreq_big", "the M3s (cpucl1)", 3 },
	};

	{
		uint32_t soc = 0, khz[32], uv[32], n;
		static const char *const names[] = { "cpucl0 (the A53s)", "cpucl1 (the A73s)", "g3d (the Mali)" };

		PE_parse_boot_argn("acpm_soc", &soc, sizeof(soc));
		PE_parse_boot_argn("acpm9610", &acpm_passive, sizeof(acpm_passive));
		if (soc == 9610) {
			uint32_t cpu = 0;

			acpm_mbox_pa = 0x11900000UL;
			acpm_sram_size = 0x15000;
			acpm_initdata = 0x6F00;
			acpm_fvmap = 0x6000;
			if (PE_parse_boot_argn("cpu9610", &cpu, sizeof(cpu)) && cpu != 0 && acpm_passive != 1) {
				cpu9610(cpu);
			}
		}
		if (soc == 9610 && acpm_passive >= 2) {
			/*
			 * acpm9610=2: IPC GET of each domain's rate and the PMIC's rails read (BUCK2 vdd_cpucl1 0x1A/0x1B, BUCK3
			 * vdd_cpucl0 0x1C/0x1D, BUCK5 vdd_g3d 0x20/0x21: S2MPU09, s2mpu09-regulator.h); acpm9610=3: then a rate
			 * set through ACPM for each domain whose GET answered, only to a level of its own fvmap (acpm_cl0=,
			 * acpm_cl1=, acpm_g3d= kHz; defaults 1534000, 2080000, 572000), and GET again. The APM sets the voltage
			 * with the rate. the port's analysis notes, steps B and C.
			 */
			static const struct { uint8_t ctrl, out; const char *name; } rails[] = {
				{ 0x1A, 0x1B, "BUCK2 vdd_cpucl1" }, { 0x1C, 0x1D, "BUCK3 vdd_cpucl0" }, { 0x20, 0x21, "BUCK5 vdd_g3d" },
			};
			uint32_t want[3] = { 1534000, 2080000, 572000 }, got[3] = { 0 };
			int got_e[3];
			uint32_t active = acpm_passive;

			PE_parse_boot_argn("acpm_cl0", &want[0], sizeof(want[0]));
			PE_parse_boot_argn("acpm_cl1", &want[1], sizeof(want[1]));
			PE_parse_boot_argn("acpm_g3d", &want[2], sizeof(want[2]));
			acpm_passive = 0;       /* (requests allowed from here; the queues are drained at setup as usual) */
			if (!hakios_acpm_init(dvfs_say)) {
				dvfs_say("acpm: 9610: no ACPM\n");
				return;
			}
			for (uint32_t r = 0; r < 3; r++) {
				uint8_t ctrl = 0, out = 0;
				int e1 = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, rails[r].ctrl, &ctrl);
				int e2 = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, rails[r].out, &out);

				dvfs_say("acpm: 9610: %s: CTRL %#x (%d), OUT %#x (%d) = %u uV\n", rails[r].name, ctrl, e1, out, e2,
				    300000 + 6250 * (uint32_t)out);
			}
			/*
			 * acpm9610=6: channel 0 is the fvp plugin's (the port's analysis notes: owner 1 << 2); until it is attached its
			 * plugin_ops is 0 and the APM's dispatcher calls through it: a GET first leaves the APM dead (run 41: the
			 * PMIC unanswered after it). So the plugin loaded and attached first, the GETs after (in the fvp step).
			 */
			if (active == 6 || active == 7) {
				apm_diag("at start", 24);
				for (uint32_t d = 0; d < 3; d++) {
					got_e[d] = -1;
				}
				hakios_acpm_fvp9610(got, got_e, names);
				/* every word of a GET's answer: cmd[0]'s low 16 bits are the DVFS plugin's result (the port's analysis notes) */
				for (uint32_t d = 0; d < 3; d++) {
					uint32_t cmd[4] = { 2 + d, 0, 1, 0 };

					got_e[d] = hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, cmd);
					got[d] = cmd[1];
					dvfs_say("acpm: 9610: GET %s raw: %d, answer %#x %#x %#x %#x (result %#x)\n", names[d], got_e[d],
					    cmd[0], cmd[1], cmd[2], cmd[3], cmd[0] & 0xffff);
				}
				apm_diag("after the attach and the GETs", 32);
				if (active == 6) {
					return;
				}
			} else for (uint32_t d = 0; d < 3; d++) {
				got_e[d] = hakios_acpm_dvfs_get(2 + d, &got[d]);
				dvfs_say("acpm: 9610: GET %s: %u kHz (%d)\n", names[d], got[d], got_e[d]);
			}
			if (active == 5) {
				uint8_t v = 0;
				int e;

				apm_diag("after the GETs", 64);
				wr(mbox, 0x0C, 0xFFFFFFFFu);    /* INTCR0 from the AP side */
				__builtin_arm_dsb(0xf);
				dvfs_say("acpm: apm: INTCR0 <- 0xffffffff: SR0 now %#x\n", rd(mbox, 0x14));
				e = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, 0x21, &v);
				dvfs_say("acpm: apm: PMIC read (the pump) %#x (%d): SR0 now %#x\n", v, e, rd(mbox, 0x14));
				apm_diag("after the clear and the pump", 16);
				for (uint32_t d = 0; d < 3; d++) {
					got_e[d] = hakios_acpm_dvfs_get(2 + d, &got[d]);
					dvfs_say("acpm: apm: GET again %s: %u kHz (%d); SR0 %#x\n", names[d], got[d], got_e[d],
					    rd(mbox, 0x14));
				}
				apm_diag("after the second GETs", 16);
			}
			if (active == 4 || active == 5) {
				hakios_acpm_fvp9610(got, got_e, names);
				if (active == 5) {
					apm_diag("at the end", 32);
				}
				return;
			}
			if (active < 3) {
				return;
			}
			for (uint32_t d = 0; d < (active == 7 ? 2u : 3u); d++) {    /* (7: the CPUs only; the Mali is mali_mhz's) */
				uint32_t after = 0, ok = 0;
				int e1, e2;

				n = hakios_acpm_dvfs_table(2 + d, khz, uv, 32, dvfs_say);
				for (uint32_t l = 0; l < n; l++) {
					ok |= khz[l] == want[d];
				}
				if (got_e[d] != 0 || !ok) {
					dvfs_say("acpm: 9610: %s: not set (GET %d, %u kHz %s its fvmap)\n", names[d], got_e[d], want[d],
					    ok ? "in" : "NOT IN");
					continue;
				}
				e1 = hakios_acpm_dvfs_set(2 + d, want[d]);
				e2 = hakios_acpm_dvfs_get(2 + d, &after);
				dvfs_say("acpm: 9610: SET %s: %u -> asked %u kHz (%d) -> GET %u kHz (%d)\n", names[d], got[d], want[d], e1,
				    after, e2);
			}
			for (uint32_t r = 0; r < 3; r++) {
				uint8_t out = 0;
				int e = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, rails[r].out, &out);

				dvfs_say("acpm: 9610: after: %s OUT %#x (%d) = %u uV\n", rails[r].name, out, e, 300000 + 6250 * (uint32_t)out);
			}
			cpu9610(1);         /* the clusters' PLLs as they read now: did the APM change them? */
			return;
		}
		if (soc == 9610 && acpm_passive) {
			char line[200];

			snprintf(line, sizeof(line), "acpm: 9610, passive (acpm9610=%u): mailbox %#lx, SRAM %#lx + %#x, initdata +%#x\n",
			    acpm_passive, acpm_mbox_pa, SRAM_PA, acpm_sram_size, acpm_initdata);
			loader_log_line(line);
			if (hakios_acpm_init(dvfs_say)) {
				for (uint32_t d = 0; d < 3; d++) {
					n = hakios_acpm_dvfs_table(2 + d, khz, uv, 32, dvfs_say);
					for (uint32_t l = 0; l < n; l++) {
						dvfs_say("acpm: fvmap: %s level %u: %u kHz, %u uV\n", names[d], l, khz[l], uv[l]);
					}
				}
			}
			return;         /* passive: no governor, no rate set */
		}
	}
	if (hakios_exynos_cpufreq_gov_start()) {
		return;
	}
	for (uint32_t i = 0; i < sizeof(doms) / sizeof(doms[0]); i++) {
		uint32_t want = 0, before = 0, after = 0;
		int e1, e2, e3;

		if (!PE_parse_boot_argn(doms[i].arg, &want, sizeof(want)) || want == 0) {
			continue;
		}
		if (!hakios_acpm_init(printf)) {
			printf("dvfs: no ACPM: %s left as they are\n", doms[i].name);
			return;
		}
		e1 = hakios_acpm_dvfs_get(doms[i].index, &before);
		e2 = hakios_acpm_dvfs_set(doms[i].index, want);
		e3 = hakios_acpm_dvfs_get(doms[i].index, &after);
		{
			char line[160];

			snprintf(line, sizeof(line), "dvfs: %s: %u kHz (%d) -> asked %u kHz (%d) -> now %u kHz (%d)\n", doms[i].name,
			    before, e1, want, e2, after, e3);
			printf("%s", line);
			loader_log_line(line);
		}
	}
}
