/*
 * The Galaxy A50's panel brightness: a Samsung EA8076 AMOLED in MIPI command mode on DSIM0 (0x148E0000, Exynos 9610;
 * its registers as the 9810's: dpu20/cal_9610/regs-dsim.h), sent its brightness through the DSIM's command FIFO while
 * the DECON keeps showing what S-Boot set up. Only with boot-arg ea8076=1.
 *
 * Unlike the S9's panel, this one takes DCS "set display brightness" (0x51) with a 10-bit value, behind the level-2
 * key, as Samsung's driver sends it (dpu20/panels/ea8076_a50_lcd_ctrl.c, low_level_set_brightness):
 *   F0 5A 5A        the key on
 *   51 hi lo        the brightness: bits [9:8], then [7:0] of brightness_table[level] (ea8076_a50_param.h, 0-255 here)
 *   F0 A5 A5        the key off
 * Left as S-Boot set them: HBM (53), ELVSS, ACL. A restart clears what is sent.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
int hakios_panel_register(const char *name, int (*set)(int level));
void hakios_ea8076_init(void);

#define DSIM_PA         0x148E0000UL
#define DSIM_INTSRC     0x50
#define  SFR_PH_EMPTY   (1u << 28)
#define DSIM_PKTHDR     0x58
#define DSIM_PAYLOAD    0x5C
#define DSIM_FIFOCTRL   0x68
#define  FULL_PH        (1u << 11)
#define  EMPTY_PH       (1u << 10)
#define  FULL_PL        (1u << 9)
#define DCS_SHORT       0x05
#define DCS_LONG        0x39

static const uint16_t brightness_table[256] = {
	3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45, 48, 53, 56, 60, 63, 67, 70, 74, 77, 81, 84, 88, 91, 
	95, 98, 102, 105, 109, 112, 116, 120, 123, 127, 130, 134, 137, 141, 144, 148, 151, 155, 158, 162, 165, 169, 
	172, 176, 179, 183, 186, 190, 193, 197, 200, 204, 207, 211, 214, 218, 221, 225, 228, 232, 235, 239, 242, 246, 
	249, 253, 257, 260, 264, 267, 271, 274, 278, 281, 285, 288, 292, 295, 299, 302, 306, 309, 313, 316, 320, 323, 
	327, 330, 334, 337, 341, 344, 348, 351, 355, 358, 362, 365, 369, 372, 376, 379, 383, 387, 390, 394, 397, 401, 
	404, 408, 411, 415, 418, 422, 425, 429, 432, 436, 439, 443, 445, 451, 456, 460, 465, 470, 474, 479, 483, 488, 
	492, 497, 500, 506, 510, 515, 520, 524, 529, 533, 538, 542, 547, 551, 556, 561, 565, 570, 574, 579, 583, 588, 
	592, 597, 601, 606, 611, 615, 620, 624, 629, 633, 638, 642, 647, 652, 656, 661, 665, 670, 674, 679, 683, 688, 
	693, 697, 702, 706, 711, 715, 720, 724, 729, 733, 738, 743, 747, 752, 756, 761, 765, 770, 774, 779, 784, 788, 
	793, 797, 802, 806, 811, 815, 820, 825, 829, 834, 838, 843, 847, 852, 856, 861, 865, 870, 875, 879, 884, 888, 
	893, 897, 902, 906, 911, 916, 920, 925, 929, 934, 938, 943, 947, 952, 956, 961, 966, 970, 975, 979, 984, 988, 
	993, 997, 1002, 1007, 1011, 1016, 1020, 1023
};

static vm_offset_t dsim;
static lck_mtx_t *panel_lock;
static int panel_level = -1;

static int
say(const char *format, ...)
{
	char line[200];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(dsim + off); }
static inline void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(dsim + off) = v; }

/* one packet, its payload (if any) and then its header: true once the DSIM says the header has gone out */
static bool
dsim_packet(uint32_t header, const uint8_t *payload, uint32_t len)
{
	uint64_t deadline = deadline_us(50000);
	bool sent;

	while ((rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) && mach_absolute_time() < deadline) {
	}
	if (rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) {
		return false;
	}
	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	for (uint32_t i = 0; i < len; i += 4) {
		uint32_t word = 0;

		for (uint32_t j = 0; j < 4 && i + j < len; j++) {
			word |= (uint32_t)payload[i + j] << (8 * j);
		}
		wr(DSIM_PAYLOAD, word);
	}
	wr(DSIM_PKTHDR, header);
	for (deadline = deadline_us(50000); !(rd(DSIM_INTSRC) & SFR_PH_EMPTY) && !(rd(DSIM_FIFOCTRL) & EMPTY_PH) &&
	    mach_absolute_time() < deadline;) {
	}
	sent = (rd(DSIM_INTSRC) & SFR_PH_EMPTY) || (rd(DSIM_FIFOCTRL) & EMPTY_PH);
	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	return sent;
}

static bool
dsim_send(const uint8_t *data, uint32_t len)
{
	if (len == 1) {
		return dsim_packet(DCS_SHORT | (uint32_t)data[0] << 8, NULL, 0);
	}
	return dsim_packet(DCS_LONG | (len & 0xff) << 8 | (len >> 8) << 16, data, len);
}

static int
panel_set(int level)
{
	static const uint8_t key_on[] = { 0xF0, 0x5A, 0x5A }, key_off[] = { 0xF0, 0xA5, 0xA5 };
	static uint64_t said_at;
	uint8_t bl[3];
	uint64_t now = mach_absolute_time(), second;
	bool ok;

	if (dsim == 0 || level < 0) {
		return EINVAL;
	}
	level = level > 255 ? 255 : level;
	bl[0] = 0x51;
	bl[1] = (uint8_t)((brightness_table[level] >> 8) & 0x3);
	bl[2] = (uint8_t)(brightness_table[level] & 0xff);
	lck_mtx_lock(panel_lock);
	ok = dsim_send(key_on, sizeof(key_on));
	ok = ok && dsim_send(bl, sizeof(bl));
	ok = dsim_send(key_off, sizeof(key_off)) && ok;
	if (ok) {
		panel_level = level;
	}
	lck_mtx_unlock(panel_lock);
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	if (!ok || said_at == 0 || now - said_at >= second) {
		say("ea8076: brightness %d (51 %02x %02x) %s\n", level, bl[1], bl[2], ok ? "sent" : "NOT SENT");
		said_at = now;
	}
	return ok ? 0 : EIO;
}

void
hakios_ea8076_init(void)
{
	uint32_t enabled = 0, level = 0;

	if (!PE_parse_boot_argn("ea8076", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	panel_lock = lck_mtx_alloc_init(lck_grp_alloc_init("ea8076", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	dsim = ml_io_map(DSIM_PA, 0x1000);
	if (dsim == 0) {
		say("ea8076: cannot map DSIM0 at 0x%lx\n", DSIM_PA);
		return;
	}
	say("ea8076: DSIM0 FIFOCTRL %x INTSRC %x; iOS's brightness goes to the panel (51)\n", rd(DSIM_FIFOCTRL),
	    rd(DSIM_INTSRC));
	(void)hakios_panel_register("ea8076", panel_set);
	if (PE_parse_boot_argn("panel_brightness", &level, sizeof(level))) {
		(void)panel_set((int)level);
	}
}
