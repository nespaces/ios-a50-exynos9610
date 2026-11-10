/*
 * The vibration motor of the Galaxy A50 (SM-A505F*): an eccentric-mass motor whose supply (an LDO) is switched by a
 * pin, Samsung's "sec_vib" (its device tree: sec_vib,ldo_type = 1, sec_vib,ldo_en = gpa2-1 active high,
 * sec_vib,max_timeout = 10000 ms). gpa2 is a bank of the ALIVE pinctrl at 0x11850000 (gpa0 +0x20, gpa1 +0x40, gpa2
 * +0x60; CON 4 bits a pin, 1 = output; DAT +4). On is the pin high, off the pin low: nothing else.
 *
 * Darwin plays its feedbacks and vibrations on the HAL plug-in's "Actuator" device (userland/src/audio_plugin.c),
 * which writes them to /dev/hakios-haptic as samples of one channel, as on the POCO (hakios.haptic.c). Here the motor
 * simply runs while those samples are not silence, and stops ACTIVE_US after they are; never longer than VIB_MAX_MS
 * at once, after which it rests as long before it runs again.
 *
 *   vib=1   the motor for Darwin
 *   vib=2   ... and a 300 ms buzz at start, to tell the pin is the motor's
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_line(const char *line);

#define ALIVE_PA        0x11850000UL
#define GPA2            0x060
#define VIB_PIN         1
#define ACTIVE_US       30000           /* a write that was not silence keeps the motor on this long */
#define SILENT_BELOW    256             /* of 32768 */
#define VIB_MAX_MS      2000

static vm_offset_t alive;
static uint32_t level;
static volatile uint64_t heard_us;
static volatile bool leaving;
static uint32_t runs, capped;
static uint64_t written_frames;

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
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

static void
motor(bool on)
{
	volatile uint32_t *dat = (volatile uint32_t *)(alive + GPA2 + 4);

	*dat = on ? (*dat | (1u << VIB_PIN)) : (*dat & ~(1u << VIB_PIN));
}

static int
vib_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		if (peak > SILENT_BELOW) {
			heard_us = now_us();
		}
	}
	return error;
}

static int
vib_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw vib_cdevsw = {
	.d_open = vib_open, .d_close = vib_open, .d_read = eno_rdwrt, .d_write = vib_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
vib_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t on_since = 0, rest_until = 0;

	for (;;) {
		uint64_t now = now_us();
		bool want = heard_us != 0 && now - heard_us < ACTIVE_US && now >= rest_until && !leaving;

		if (want && on_since == 0) {
			motor(true);
			on_since = now;
			runs++;
		} else if (on_since != 0 && (!want || now - on_since >= VIB_MAX_MS * 1000ULL)) {
			if (want) {
				capped++;
				rest_until = now + (now - on_since);
			}
			motor(false);
			on_since = 0;
		}
		if (leaving) {
			break;
		}
		delay(4000);
	}
	thread_terminate(current_thread());
}

void
hakios_haptic_init(void)
{
	volatile uint32_t *con;
	thread_t thread;
	int major;

	if (!PE_parse_boot_argn("vib", &level, sizeof(level)) || level == 0) {
		return;
	}
	alive = ml_io_map(ALIVE_PA, 0x1000);
	if (alive == 0) {
		say("vib: the ALIVE pins not mapped\n");
		return;
	}
	con = (volatile uint32_t *)(alive + GPA2);
	say("vib: gpa2 as found: CON %#x DAT %#x; pin %u to output, low\n", con[0], con[1], VIB_PIN);
	motor(false);
	con[0] = (con[0] & ~(0xfu << (4 * VIB_PIN))) | (1u << (4 * VIB_PIN));
	if (level >= 2) {
		motor(true);
		delay(300000);
		motor(false);
		say("vib: a 300 ms buzz (gpa2 CON %#x)\n", con[0]);
	}
	major = cdevsw_add(-1, &vib_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		say("vib: no /dev/hakios-haptic\n");
		return;
	}
	if (kernel_thread_start(vib_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	say("vib: the motor for Darwin (/dev/hakios-haptic): on while its Actuator plays, at most %u ms at once\n",
	    VIB_MAX_MS);
}

void
hakios_haptic_quiesce(void)
{
	if (alive != 0) {
		leaving = true;
		motor(false);
		/* loader_return can run from an IPI with interrupts disabled. Never
		 * enter the serial console here; keep the summary in its RAM log. */
		char line[192];
		snprintf(line, sizeof(line), "vib: at the run's end: %u runs (%u cut at %u ms), %llu frames from Darwin\n",
		    runs, capped, VIB_MAX_MS, written_frames);
		loader_log_line(line);
	}
}

void
hakios_haptic_pretend(uint32_t peak)
{
	if (peak > SILENT_BELOW) {
		heard_us = now_us();
	}
}
