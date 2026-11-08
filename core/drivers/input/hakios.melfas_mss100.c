/*
 * The touch screen of the Galaxy A50 (SM-A505FN, Exynos 9610): a Melfas MSS100 at 0x48 on i2c_4 (0x13870000), as a
 * source of touch records for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg melfas=1: every address below
 * belongs to this phone.
 *
 * The bus is not an HSI2C of a USI block (hakios.exynos_hsi2c.c, the S9's) but the older Samsung I2C controller
 * ("samsung,s3c2440-i2c": IICCON, IICSTAT, IICADD, IICDS, IICLC), driven here by polling its pending bit; its clock
 * is the PERI bus's under hardware auto clock gating (GATE_I2C_4_QCH, a Q-channel: nothing to switch on).
 *   I2C_4              0x13870000   IICCON +0, IICSTAT +4, IICDS +0xC, IICLC +0x10
 *   GPIO TOP           0x139B0000   gpp1 +0x20: pin 1 SCL, pin 0 SDA, function 2 (i2c4_bus)
 *                                   gpg3 +0x120: pin 1 tsp_ldo_en, the chip's 3.0 V supply, a GPIO-switched regulator
 *   GPIO ALIVE         0x11850000   gpa0 +0x20: pin 0 the chip's interrupt, low while it has events
 * (bank offsets: pinctrl-exynos-arm64.c, exynos9610_pin_banks0 and 4; a bank is CON, DAT, PUD, DRV, 4 bits a pin)
 *
 * The chip's protocol (MIP4, Samsung's melfas_mss100.c for the A50): a register is two bytes, its group and its
 * offset, written, then the answer read after a repeated start. 06 00 ready status (A0 ready, 05 none, 77 a log to
 * be skipped by writing 10 14 00); 01 00 the product name (16); 01 20 the firmware versions (8); 01 10 the resolution
 * (X, Y, little-endian); 02 10 the packet info (bit 7 an alert, [6:0] its size), then 02 11 the packet. A touch packet
 * here is the A50's device tree's event_info <8 12>, WITH_PRESSURE_2BYTE, 12 bytes an event: b0 [7:4] type (1 the
 * screen), [3:0] the finger from 1; b1 bit 0 pressed; X = (b2 & 15) << 8 | b3, Y = (b2 >> 4) << 8 | b4; 0-4095 each
 * (max_x_y <4095 4095>).
 * Sources: the A50's device tree (TWRP's live one: i2c@13870000/mms_ts@48), Samsung's 4.14 kernel for it
 * (drivers/input/touchscreen/melfas/mss100, drivers/i2c/busses/i2c-s3c2410.c), read for these facts, not copied.
 *
 * In steps, each said before it is made. Boot-arg melfas_steps=N stops after the first N:
 *   1  map the blocks and read what they are set to          (nothing written)
 *   2  the chip's supply on, the bus's pins to I2C, the interrupt pin an input
 *   3  the bus set up: master, ack, the clock divided (melfas_scale=, default 3: PERI / 512 / 4)
 *   4  ask the chip: ready status, name, firmware, resolution (the first transfers)
 *   5  watch it (the default)
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void delay(int usec);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);

/* (every line to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
extern void loader_log_line(const char *line);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

/* ---- where things are ---- */
#define I2C_PA                  0x13870000UL
#define TOP_PA                  0x139B0000UL
#define ALIVE_PA                0x11850000UL
#define GPP1                    0x020
#define GPG3                    0x120
#define GPA0                    0x020
#define GPIO_CON                0x0
#define GPIO_DAT                0x4
#define GPIO_PUD                0x8
#define PIN_SDA                 0
#define PIN_SCL                 1
#define PIN_LDO                 1
#define PIN_IRQ                 0
#define CHIP                    0x48

/* ---- the bus (i2c-s3c2410.c's registers) ---- */
#define IICCON                  0x00
#define IICSTAT                 0x04
#define IICDS                   0x0c
#define IICLC                   0x10
#define CON_ACKEN               (1u << 7)
#define CON_TXDIV_512           (1u << 6)
#define CON_IRQEN               (1u << 5)
#define CON_IRQPEND             (1u << 4)
#define STAT_MASTER_RX          (2u << 6)
#define STAT_MASTER_TX          (3u << 6)
#define STAT_START              (1u << 5)       /* written: START; read: the bus is busy */
#define STAT_TXRXEN             (1u << 4)
#define STAT_ARBITR             (1u << 3)
#define STAT_LASTBIT            (1u << 0)       /* after an address or a byte sent: 1, no ACK */
#define LC_FILTER_ON            (1u << 2)
#define LC_SDA_DELAY5           (1u << 0)

#define BYTE_TIMEOUT_US         5000

/* ---- the chip ---- */
#define MIP_R0_INFO             0x01
#define MIP_R1_INFO_PRODUCT     0x00
#define MIP_R1_INFO_RESOLUTION  0x10
#define MIP_R1_INFO_VERSION     0x20
#define MIP_R0_EVENT            0x02
#define MIP_R1_EVENT_FORMAT     0x04
#define MIP_R1_EVENT_SIZE       0x06
#define MIP_R1_PACKET_INFO      0x10
#define MIP_R1_PACKET_DATA      0x11
#define MIP_R0_CTRL             0x06
#define MIP_R1_CTRL_READY       0x00
#define MIP_R0_LOG              0x10
#define MIP_R1_LOG_TRIGGER      0x14
#define STATUS_READY            0xa0
#define STATUS_NONE             0x05
#define STATUS_LOG              0x77
#define FORMAT_PRESSURE_2BYTE   8
#define FINGERS                 10
#define PACKET_MOST             200

#define POLL_MS                 4
#define MINUTE_TICKS            (60 * 1000 / POLL_MS)

static vm_offset_t i2c, top, alive;
static uint32_t scale = 3;
static uint32_t res_x = 4096, res_y = 4096;
static uint32_t event_size = 12;
static bool finger_down[FINGERS];
static uint32_t trace_left;
static struct {
	uint64_t transfers, errors, events, downs;
	uint32_t last_error;
} stats;

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

/* a pin's 4-bit field of a bank's CON (or PUD) */
static void
pin_field(vm_offset_t bank, uint32_t reg, uint32_t pin, uint32_t v)
{
	uint32_t shift = pin * 4;

	wr(bank, reg, (rd(bank, reg) & ~(0xfu << shift)) | (v << shift));
}

/* ---- the bus, polled ---- */

static uint32_t
bus_con(void)
{
	return CON_ACKEN | CON_TXDIV_512 | CON_IRQEN | (scale & 0xf);
}

/* the pending bit set: the address or byte is through (1: timed out) */
static int
bus_wait(void)
{
	for (uint32_t us = 0; us < BYTE_TIMEOUT_US; us++) {
		if (rd(i2c, IICCON) & CON_IRQPEND) {
			return 0;
		}
		delay(1);
	}
	return 1;
}

/* STOP, in the mode it is in, and the bus free again */
static void
bus_stop(uint32_t mode)
{
	wr(i2c, IICSTAT, mode | STAT_TXRXEN);
	wr(i2c, IICCON, bus_con());
	for (uint32_t us = 0; us < BYTE_TIMEOUT_US && (rd(i2c, IICSTAT) & STAT_START); us++) {
		delay(1);
	}
}

/*
 * Write wlen bytes, then (rlen != 0) a repeated start and read rlen: what the chip's registers want.
 * 0 or the step that failed: 1 the bus busy, 2 no ACK of the address, 3 of a byte, 4 of the read address,
 * 5 a timeout, 6 the arbitration lost.
 */
static uint32_t
bus_xfer(const uint8_t *w, uint32_t wlen, uint8_t *r, uint32_t rlen)
{
	uint32_t error = 0, con = bus_con();

	stats.transfers++;
	for (uint32_t us = 0; us < BYTE_TIMEOUT_US && (rd(i2c, IICSTAT) & STAT_START); us++) {
		delay(1);
	}
	if (rd(i2c, IICSTAT) & STAT_START) {
		error = 1;
		goto out;
	}
	wr(i2c, IICSTAT, STAT_MASTER_TX | STAT_TXRXEN);
	wr(i2c, IICDS, CHIP << 1);
	delay(1);
	wr(i2c, IICCON, con);
	wr(i2c, IICSTAT, STAT_MASTER_TX | STAT_START | STAT_TXRXEN);
	if (bus_wait()) {
		error = 5;
		goto stop_tx;
	}
	if (rd(i2c, IICSTAT) & STAT_ARBITR) {
		error = 6;
		goto stop_tx;
	}
	if (rd(i2c, IICSTAT) & STAT_LASTBIT) {
		error = 2;
		goto stop_tx;
	}
	for (uint32_t i = 0; i < wlen; i++) {
		wr(i2c, IICDS, w[i]);
		delay(1);
		wr(i2c, IICCON, con);
		if (bus_wait()) {
			error = 5;
			goto stop_tx;
		}
		if (rd(i2c, IICSTAT) & STAT_LASTBIT) {
			error = 3;
			goto stop_tx;
		}
	}
	if (rlen == 0) {
		goto stop_tx;
	}
	/* the repeated start: the read address, master receive, then the pending bit cleared */
	wr(i2c, IICDS, CHIP << 1 | 1);
	delay(1);
	wr(i2c, IICSTAT, STAT_MASTER_RX | STAT_START | STAT_TXRXEN);
	wr(i2c, IICCON, con);
	if (bus_wait()) {
		error = 5;
		goto stop_rx;
	}
	if (rd(i2c, IICSTAT) & STAT_LASTBIT) {
		error = 4;
		goto stop_rx;
	}
	for (uint32_t i = 0; i < rlen; i++) {
		/* no ACK after the last byte: the chip lets the bus go */
		wr(i2c, IICCON, i + 1 == rlen ? con & ~CON_ACKEN : con);
		if (bus_wait()) {
			error = 5;
			goto stop_rx;
		}
		r[i] = (uint8_t)rd(i2c, IICDS);
	}
stop_rx:
	bus_stop(STAT_MASTER_RX);
	goto out;
stop_tx:
	bus_stop(STAT_MASTER_TX);
out:
	if (error != 0) {
		stats.errors++;
		stats.last_error = error;
	}
	return error;
}

/* a register's bytes (one retry: the first transfer after a reset may be lost) */
static uint32_t
chip_read(uint8_t group, uint8_t offset, uint8_t *answer, uint32_t len)
{
	uint8_t reg[2] = { group, offset };
	uint32_t error = bus_xfer(reg, 2, answer, len);

	return error == 0 ? 0 : bus_xfer(reg, 2, answer, len);
}

static uint32_t
chip_write(uint8_t group, uint8_t offset, uint8_t value)
{
	uint8_t w[3] = { group, offset, value };

	return bus_xfer(w, 3, NULL, 0);
}

static bool
irq_low(void)
{
	return ((rd(alive, GPA0 + GPIO_DAT) >> PIN_IRQ) & 1) == 0;
}

/* ---- events ---- */

static void
all_up(void)
{
	for (uint32_t f = 0; f < FINGERS; f++) {
		if (finger_down[f]) {
			finger_down[f] = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, f, 0, 0);
		}
	}
}

static void
handle_event(const uint8_t *e)
{
	uint32_t type = e[0] >> 4, id = e[0] & 0xf, pressed = e[1] & 1;
	uint32_t x = (uint32_t)(e[2] & 0xf) << 8 | e[3], y = (uint32_t)(e[2] >> 4) << 8 | e[4];
	uint32_t nx, ny;

	if (type != 1 || id == 0 || id > FINGERS) {
		return;
	}
	id--;
	if (trace_left > 0) {
		trace_left--;
		say("melfas: t %llu ms: finger %u %s at %u,%u\n", (unsigned long long)(mach_absolute_time() / 26000), id,
		    !pressed ? "up" : (finger_down[id] ? "moved" : "down"), x, y);
	}
	nx = x >= res_x - 1 ? 65535 : x * 65535 / (res_x - 1);
	ny = y >= res_y - 1 ? 65535 : y * 65535 / (res_y - 1);
	stats.events++;
	if (!pressed) {
		if (finger_down[id]) {
			finger_down[id] = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, nx, ny);
		}
	} else if (!finger_down[id]) {
		finger_down[id] = true;
		stats.downs++;
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, nx, ny);
	} else {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, nx, ny);
	}
}

/* one packet: its info, then its bytes; an alert is said and dropped */
static void
read_packet(void)
{
	uint8_t info = 0, packet[PACKET_MOST];
	uint32_t size;

	if (chip_read(MIP_R0_EVENT, MIP_R1_PACKET_INFO, &info, 1) != 0) {
		return;
	}
	size = info & 0x7f;
	if (size == 0 || size > PACKET_MOST) {
		return;
	}
	if (chip_read(MIP_R0_EVENT, MIP_R1_PACKET_DATA, packet, size) != 0) {
		return;
	}
	if (info & 0x80) {
		say("melfas: alert %u (%02x %02x), every finger up\n", packet[0], packet[0], size > 1 ? packet[1] : 0);
		all_up();
		return;
	}
	for (uint32_t i = 0; i + event_size <= size; i += event_size) {
		handle_event(packet + i);
	}
}

/* ---- the steps ---- */

static void
touch_thread(void *arg, __unused wait_result_t wr_)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, step = 0, error;
	uint8_t answer[17] = { 0 };

	say("melfas: step 1: map the I2C, the GPIO blocks; read what they are set to\n");
	i2c = ml_io_map(I2C_PA, 0x1000);
	top = ml_io_map(TOP_PA, 0x1000);
	alive = ml_io_map(ALIVE_PA, 0x1000);
	if (!i2c || !top || !alive) {
		say("melfas: a block could not be mapped\n");
		goto out;
	}
	say("melfas: I2C_4 IICCON %x IICSTAT %x IICLC %x; gpp1 CON %x DAT %x; gpg3 CON %x DAT %x (tsp_ldo_en %s); "
	    "gpa0 CON %x DAT %x PUD %x (the interrupt %s)\n", rd(i2c, IICCON), rd(i2c, IICSTAT), rd(i2c, IICLC),
	    rd(top + GPP1, GPIO_CON), rd(top + GPP1, GPIO_DAT), rd(top + GPG3, GPIO_CON), rd(top + GPG3, GPIO_DAT),
	    (rd(top + GPG3, GPIO_DAT) >> PIN_LDO) & 1 ? "high" : "low", rd(alive + GPA0, GPIO_CON),
	    rd(alive + GPA0, GPIO_DAT), rd(alive + GPA0, GPIO_PUD), irq_low() ? "low" : "high");
	if (++step >= steps) {
		goto out;
	}

	say("melfas: step 2: the supply on (gpg3-1 high), the pins to I2C (gpp1-0, gpp1-1), the interrupt an input\n");
	{
		bool was_on = (rd(top + GPG3, GPIO_DAT) >> PIN_LDO) & 1 && ((rd(top + GPG3, GPIO_CON) >> (PIN_LDO * 4)) & 0xf) == 1;

		pin_field(top + GPG3, GPIO_CON, PIN_LDO, 1);
		wr(top + GPG3, GPIO_DAT, rd(top + GPG3, GPIO_DAT) | 1u << PIN_LDO);
		pin_field(top + GPP1, GPIO_CON, PIN_SDA, 2);
		pin_field(top + GPP1, GPIO_CON, PIN_SCL, 2);
		pin_field(top + GPP1, GPIO_PUD, PIN_SDA, 0);
		pin_field(top + GPP1, GPIO_PUD, PIN_SCL, 0);
		pin_field(alive + GPA0, GPIO_CON, PIN_IRQ, 0);
		pin_field(alive + GPA0, GPIO_PUD, PIN_IRQ, 0);
		if (!was_on) {
			/* the chip's own start after its supply: Samsung's driver waits as long */
			delay_for_interval(300, NSEC_PER_MSEC);
		}
		say("melfas: the supply %s; now gpp1 CON %x, gpg3 CON %x DAT %x, gpa0 CON %x (the interrupt %s)\n",
		    was_on ? "was on" : "switched on, 300 ms", rd(top + GPP1, GPIO_CON), rd(top + GPG3, GPIO_CON),
		    rd(top + GPG3, GPIO_DAT), rd(alive + GPA0, GPIO_CON), irq_low() ? "low" : "high");
	}
	if (++step >= steps) {
		goto out;
	}

	say("melfas: step 3: the bus: master, ACK, PERI / 512 / %u, filter on\n", scale + 1);
	wr(i2c, IICSTAT, 0);
	wr(i2c, IICLC, LC_FILTER_ON | LC_SDA_DELAY5);
	wr(i2c, IICCON, bus_con());
	wr(i2c, IICSTAT, STAT_TXRXEN);
	say("melfas: IICCON %x IICSTAT %x IICLC %x\n", rd(i2c, IICCON), rd(i2c, IICSTAT), rd(i2c, IICLC));
	if (++step >= steps) {
		goto out;
	}

	say("melfas: step 4: ask the chip at 0x%x\n", CHIP);
	error = 1;
	for (uint32_t tries = 0; tries < 20 && error != 0; tries++) {
		error = chip_read(MIP_R0_CTRL, MIP_R1_CTRL_READY, answer, 1);
		if (error != 0) {
			delay_for_interval(50, NSEC_PER_MSEC);
		}
	}
	if (error != 0) {
		say("melfas: no answer (step %u failed; IICSTAT %x): the bus is not working, or the chip has no power\n",
		    error, rd(i2c, IICSTAT));
		goto out;
	}
	say("melfas: ready status %02x (%s)\n", answer[0], answer[0] == STATUS_READY ? "ready" : answer[0] == STATUS_NONE ?
	    "no event" : answer[0] == STATUS_LOG ? "a log waiting: skipped" : "UNKNOWN");
	if (answer[0] == STATUS_LOG) {
		(void)chip_write(MIP_R0_LOG, MIP_R1_LOG_TRIGGER, 0);
	}
	memset(answer, 0, sizeof(answer));
	if (chip_read(MIP_R0_INFO, MIP_R1_INFO_PRODUCT, answer, 16) == 0) {
		for (int i = 0; i < 16; i++) {
			if (answer[i] < 0x20 || answer[i] > 0x7e) {
				answer[i] = 0;
			}
		}
		say("melfas: product %s\n", (const char *)answer);
	}
	if (chip_read(MIP_R0_INFO, MIP_R1_INFO_VERSION, answer, 8) == 0) {
		say("melfas: firmware boot %x.%x core %x.%x model %x.%x version %x.%x\n", answer[0], answer[1], answer[2],
		    answer[3], answer[4], answer[5], answer[6], answer[7]);
	}
	if (chip_read(MIP_R0_INFO, MIP_R1_INFO_RESOLUTION, answer, 4) == 0) {
		uint32_t x = (uint32_t)answer[1] << 8 | answer[0], y = (uint32_t)answer[3] << 8 | answer[2];

		say("melfas: resolution %u x %u (the device tree: 4095 x 4095)\n", x, y);
		if (x > 1 && x <= 4096 && y > 1 && y <= 4096) {
			res_x = x, res_y = y;
		}
	}
	if (chip_read(MIP_R0_EVENT, MIP_R1_EVENT_FORMAT, answer, 2) == 0 &&
	    chip_read(MIP_R0_EVENT, MIP_R1_EVENT_SIZE, answer + 2, 1) == 0) {
		uint32_t format = (uint32_t)answer[1] << 8 | answer[0];

		say("melfas: event format %u, %u bytes an event (the device tree: 8, 12)\n", format, answer[2]);
		if (format != FORMAT_PRESSURE_2BYTE) {
			say("melfas: not the format this file reads: its events would be misread; stopping\n");
			goto out;
		}
		if (answer[2] >= 12 && answer[2] <= 32) {
			event_size = answer[2];
		}
	}
	if (++step >= steps) {
		goto out;
	}

	say("melfas: step 5: watching the interrupt pin every %u ms; the coordinates are 0-%u x 0-%u\n", POLL_MS,
	    res_x - 1, res_y - 1);
	for (unsigned tick = 1;; tick++) {
		for (int rounds = 0; rounds < 4 && irq_low(); rounds++) {
			read_packet();
		}
		if (tick % MINUTE_TICKS == 0) {
			say("melfas: a minute: %llu touches, %llu events, %llu transfers, %llu failed (last %u)\n", stats.downs,
			    stats.events, stats.transfers, stats.errors, stats.last_error);
		}
		if (stats.errors >= 1000 && stats.errors * 2 > stats.transfers) {
			say("melfas: more than half of %llu transfers have failed: giving up\n", stats.transfers);
			all_up();
			break;
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
out:
	if (step < 5 && steps < 5) {
		say("melfas: stopping here (melfas_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
hakios_melfas_mss100_init(void)
{
	uint32_t enabled = 0, steps = 5, pri = 92;
	thread_t thread;

	if (!PE_parse_boot_argn("melfas", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	PE_parse_boot_argn("melfas_steps", &steps, sizeof(steps));
	PE_parse_boot_argn("melfas_scale", &scale, sizeof(scale));
	PE_parse_boot_argn("melfas_trace", &trace_left, sizeof(trace_left));
	/* melfas_pri=N: the polling thread's priority; 92 as the S9's touch (hakios.s6sy761.c says why) */
	PE_parse_boot_argn("melfas_pri", &pri, sizeof(pri));
	if (steps == 0 || scale > 15) {
		return;
	}
	if (kernel_thread_start_priority((thread_continue_t)touch_thread, (void *)(uintptr_t)steps, (integer_t)pri,
	    &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
