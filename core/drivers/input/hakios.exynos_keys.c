/*
 * The keys of the Galaxy S9 (starlte, Exynos 9810) -- power, volume up, volume down, Bixby -- as a source of
 * button records for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg exynos_keys=1: every address
 * below belongs to this phone.
 *
 * All four are pins of the SoC's ALIVE GPIO block (pinctrl@14050000 in Samsung's device tree), each pulled up
 * on the board and pulled to ground by its key, so a pin reads 0 while its key is held:
 *   volume up    gpa0 pin 3     DAT 0x14050024 bit 3
 *   volume down  gpa0 pin 4     DAT 0x14050024 bit 4
 *   Bixby        gpa0 pin 6     DAT 0x14050024 bit 6
 *   power        gpa2 pin 4     DAT 0x14050064 bit 4
 * A bank of that block is CON (+0x00, a nibble per pin: 0 input, 0xF its wakeup interrupt), DAT (+0x04, a bit
 * per pin), PUD (+0x08), DRV (+0x0C), banks 0x20 apart, gpa0 at 0x20 and gpa2 at 0x60. DAT says the pin's level
 * whether the pin is an input or an interrupt (Android's own driver reads it with the pins set to 0xF).
 * Sources: the phone's device tree (gpio_keys, gpa0/gpa2 under pinctrl@14050000, the key-* pin settings),
 * Linux's drivers/pinctrl/samsung/pinctrl-exynos.c (the ALIVE banks of the Exynos 9810) and Project Silicium's
 * starlte UEFI, which reads the same four bits the same way (KeypadDeviceLib.c) and sets nothing up first.
 *
 * THIS DRIVER ONLY READS. The firmware has left the pins as Android wants them; a pin found set to anything but
 * an input or its interrupt is not taken for a key, and said so.
 *
 * What a press is: a thread reads the two DAT registers 100 times a second, and a key has changed when two looks
 * in a row say so (Android debounces for 5 ms). A key that reads as held before it has ever been seen released
 * is not believed until it has been.
 *
 * The mapping: the iPhone X, whose user space runs here, has a side button and two volume keys. Power is the
 * side button (HAKIOS_BUTTON_LOCK); Bixby, where the iPhone has nothing, is the home button (HAKIOS_BUTTON_HOME),
 * which is a way home while there is no touch screen.
 *
 * exynos_keys=2: the Galaxy A50's (SM-A505FN, Exynos 9610) instead -- its gpio_keys (TWRP's live device tree): all
 * three on gpa1 of the ALIVE block at 0x11850000 (bank +0x40: gpa0 +0x20, banks 0x20 apart, pinctrl-exynos-arm64.c's
 * exynos9610_pin_banks0), volume up pin 5, volume down pin 6, power pin 7; no Bixby key (home is the touch screen's
 * swipe up, as on the iPhone X).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* (every line to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
extern void loader_log_line(const char *line);

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

#define ALIVE_PA        0x14050000UL    /* pinctrl@14050000: the ALIVE GPIO block */
#define ALIVE_PA_A50    0x11850000UL    /* pinctrl@11850000 on the Exynos 9610 */
#define BANK_GPA1       0x040
#define ALIVE_SIZE      0x1000
#define BANK_GPA0       0x020
#define BANK_GPA2       0x060
#define BANK_CON        0x00
#define BANK_DAT        0x04

#define POLL_MS         10
#define MINUTE_TICKS    (60 * 1000 / POLL_MS)

static struct key {
	const char *name;
	uint32_t bank, pin;
	uint32_t button;                /* HAKIOS_BUTTON_* */
	bool usable;                    /* the pin is an input or its interrupt */
	bool live;                      /* seen released: what it says is believed */
	bool held;                      /* as last reported */
	bool last;                      /* the previous look */
	uint32_t presses;
} keys[] = {
	{ "power", BANK_GPA2, 4, HAKIOS_BUTTON_LOCK },
	{ "volume-up", BANK_GPA0, 3, HAKIOS_BUTTON_VOLUME_UP },
	{ "volume-down", BANK_GPA0, 4, HAKIOS_BUTTON_VOLUME_DOWN },
	{ "bixby", BANK_GPA0, 6, HAKIOS_BUTTON_HOME },
}, keys_a50[] = {
	{ "power", BANK_GPA1, 7, HAKIOS_BUTTON_LOCK },
	{ "volume-up", BANK_GPA1, 5, HAKIOS_BUTTON_VOLUME_UP },
	{ "volume-down", BANK_GPA1, 6, HAKIOS_BUTTON_VOLUME_DOWN },
};
static struct key *keyset = keys;
static unsigned nkeys = sizeof(keys) / sizeof(keys[0]);
static unsigned long alive_pa = ALIVE_PA;
#define NKEYS nkeys

static vm_offset_t alive;

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(alive + off);
}

/* held: the pin at 0 */
static bool
key_held(const struct key *k)
{
	return ((rd(k->bank + BANK_DAT) >> k->pin) & 1) == 0;
}


/*
 * Rotation by hand. The A50's accelerometer sits behind the sensor hub (a firmware of Samsung's that nobody loads here
 * yet), so nothing tells iOS which way up the phone is. Both volume keys held for 0.7 s turn the screen to the next
 * orientation -- UIDeviceOrientation 1 portrait, 3 landscape left, 2 upside down, 4 landscape right -- which
 * backboardd reads from kern.hakios.orientation (userland/src/inject.c).
 */
SYSCTL_DECL(_kern_hakios);
static int orientation_now = 1;
SYSCTL_INT(_kern_hakios, OID_AUTO, orientation, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY | CTLFLAG_KERN,
    &orientation_now, 0, "UIDeviceOrientation chosen by holding both volume keys: 1 portrait, 3 landscape left, 2 upside down, 4 landscape right");

static void
keys_chord(void)
{
	static unsigned held_ticks;
	static const int order[4] = { 1, 3, 2, 4 };

	if (nkeys >= 3 && keyset[1].held && keyset[2].held) {
		if (++held_ticks == 700 / POLL_MS) {
			unsigned at = 0;

			for (unsigned i = 0; i < 4; i++) {
				if (order[i] == orientation_now) { at = i; }
			}
			orientation_now = order[(at + 1) % 4];
			say("exynos_keys: both volume keys held: orientation %d\n", orientation_now);
		}
	} else {
		held_ticks = 0;
	}
}

static void
keys_poll(void)
{
	for (unsigned i = 0; i < NKEYS; i++) {
		struct key *k = &keyset[i];
		bool now = key_held(k);

		if (!k->usable) {
			continue;
		}
		if (now != k->last) {
			k->last = now;          /* one look: wait for a second that agrees */
			continue;
		}
		if (!k->live) {
			if (!now) {
				k->live = true;
				say("exynos_keys: %s seen released: watching it\n", k->name);
			}
			continue;
		}
		if (now != k->held) {
			k->held = now;
			if (now) {
				k->presses++;
			}
			hakios_touch_post(now ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, k->button, 0, 0);
			say("exynos_keys: %s %s\n", k->name, now ? "down" : "up");
		}
	}
	keys_chord();
}

static void
keys_thread(__unused void *arg, __unused wait_result_t wr)
{
	alive = ml_io_map(alive_pa, ALIVE_SIZE);
	if (alive == 0) {
		say("exynos_keys: cannot map the ALIVE GPIO block at 0x%lx\n", alive_pa);
		thread_terminate(current_thread());
		return;
	}
	for (unsigned i = 0; i < NKEYS; i++) {
		struct key *k = &keyset[i];
		uint32_t con = (rd(k->bank + BANK_CON) >> (4 * k->pin)) & 0xf;

		k->usable = con == 0x0 || con == 0xf;
		k->last = key_held(k);
		k->live = k->usable && !k->last;
		say("exynos_keys: %s: bank 0x%x pin %u, CON %x (%s), %s\n", k->name, k->bank, k->pin, con,
		    con == 0 ? "input" : con == 0xf ? "interrupt" : "SOMETHING ELSE: not a key here, left alone",
		    k->last ? "reads as held: believed once it has been seen released" : "released");
	}
	say("exynos_keys: watching every %u ms: power is the side button%s\n", POLL_MS,
	    keyset == keys ? ", Bixby the home button" : "");
	for (unsigned tick = 1;; tick++) {
		keys_poll();
		if (tick % MINUTE_TICKS == 0) {
			say("exynos_keys: a minute: presses power %u, volume-up %u, volume-down %u%s\n", keyset[0].presses,
			    keyset[1].presses, keyset[2].presses, keyset == keys ? "" : " (the A50's)");
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
}

void
hakios_exynos_keys_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("exynos_keys", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	if (enabled == 2) {
		keyset = keys_a50;
		nkeys = sizeof(keys_a50) / sizeof(keys_a50[0]);
		alive_pa = ALIVE_PA_A50;
	}
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
