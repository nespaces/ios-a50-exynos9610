/*
 * /dev/hakios-touch: a queue of touch samples (see sys/hakios.input.h).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/hakios.input.h>
#include <sys/hakios.exynos_abox.h>
#include <sys/hakios.touch_queue.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <pexpert/pexpert.h>

#define TOUCH_QUEUE     512             /* records; a power of two */

/*
 * Boot-arg touch_lat=1: for measuring how long a touch takes to reach the panel, a line for every
 * sample posted ("tl: p TYPE FINGER X Y at US", the time in microseconds of mach_absolute_time),
 * to be matched with the window server's lines about reading it (userland/src/inject.c), drawing
 * (eagl.c) and the display driver's about the frame (MiniIOMobileFramebuffer.cpp): see
 * tools/latency.py.
 */
static uint32_t touch_lat = 2;          /* 2: not yet looked up */

extern void loader_log_line(const char *line);

/*
 * A "tl:" line, for the kernel's drivers: into the loader's log as it is written -- the kernel's own
 * log keeps only the last of a run, and a live look at the loader's (XL_FOLLOW) then misses nothing.
 */
void hakios_tl(const char *format, ...) __printflike(1, 2);

void
hakios_tl(const char *format, ...)
{
	char line[192];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

static bool
latency_lines(void)
{
	if (touch_lat == 2) {
		uint32_t on = 0;

		touch_lat = PE_parse_boot_argn("touch_lat", &on, sizeof(on)) && on != 0;
	}
	return touch_lat != 0;
}

static LCK_GRP_DECLARE(touch_lck_grp, "hakios-touch");
static LCK_MTX_DECLARE(touch_lock, &touch_lck_grp);
static struct hakios_touch touch_queue[TOUCH_QUEUE];
static uint32_t touch_head, touch_tail;        /* read from head, add at tail */
/* touch_coalesce=1: a slow reader gets the newest pending MOVE, keeping DOWN/UP ordering. */
static TUNABLE(uint32_t, touch_coalesce, "touch_coalesce", 0);
static uint32_t touch_coalesced;
SYSCTL_DECL(_kern_hakios);
SYSCTL_UINT(_kern_hakios, OID_AUTO, touch_coalesced, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
    &touch_coalesced, 0, "Pending trailing touch MOVE events replaced with the latest position");

/*
 * The way out when nothing else is at hand: five fingers held on the screen for a second and a
 * half end the run -- control goes back to the loader, as when loader_return_after expires.
 * Only a driver's fingers count, not samples a program writes to the device.
 */
#define EXIT_FINGERS    5
#define EXIT_HOLD_MS    1500

extern void loader_return(void);
extern void hakios_var_save_on_return(void);    /* drivers/storage/hakios.ufs.c: the data volume synced into the flash */

static thread_call_t exit_call;
static uint32_t fingers_down;           /* one bit for each finger a driver says is down */

static void
exit_hold_elapsed(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	printf("\n--- %d fingers held on the screen for %d ms: back to the loader ---\n", EXIT_FINGERS, EXIT_HOLD_MS);
	hakios_var_save_on_return();    /* as when the run's time is up: the data volume's last writes first */
	hakios_adreno_stop();
	loader_return();
}

/* With touch_lock held. */
static void
count_fingers(uint32_t type, uint32_t finger)
{
	int before = __builtin_popcount(fingers_down), now;
	uint64_t deadline;

	if (finger >= 32 || exit_call == NULL || (type != HAKIOS_TOUCH_DOWN && type != HAKIOS_TOUCH_UP)) {
		return;
	}
	if (type == HAKIOS_TOUCH_DOWN) {
		fingers_down |= 1u << finger;
	} else {
		fingers_down &= ~(1u << finger);
	}
	now = __builtin_popcount(fingers_down);
	if (now >= EXIT_FINGERS && before < EXIT_FINGERS) {
		clock_interval_to_deadline(EXIT_HOLD_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(exit_call, deadline);
	} else if (now < EXIT_FINGERS && before >= EXIT_FINGERS) {
		thread_call_cancel(exit_call);
	}
}

static void
touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, bool from_driver, uint64_t when)
{
	struct hakios_touch *t;

	lck_mtx_lock(&touch_lock);
	t = hakios_touch_queue_slot(touch_queue, TOUCH_QUEUE, &touch_head, &touch_tail,
	    type, finger, touch_coalesce, &touch_coalesced);
	t->time = when != 0 ? when : mach_absolute_time();
	if (latency_lines()) {
		uint64_t ns;

		absolutetime_to_nanoseconds(t->time, &ns);
		hakios_tl("tl: p %u %u %u %u at %llu\n", type, finger, x, y, ns / 1000);
	}
	t->type = type;
	t->finger = finger;
	t->x = x > 65535 ? 65535 : x;
	t->y = y > 65535 ? 65535 : y;
	if (from_driver) {
		count_fingers(type, finger);
	}
	lck_mtx_unlock(&touch_lock);
	wakeup(&touch_queue);
}

void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	touch_post(type, finger, x, y, true, 0);
}

/* The same, for a sample taken at mach_absolute_time() `when` (not later than now). */
void
hakios_touch_post_at(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t when)
{
	touch_post(type, finger, x, y, true, when);
}

static int
touch_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_touch t;
	int error = 0, moved = 0;

	/* a buffer smaller than one record can never be served: say so, instead of blocking for data and then returning 0 (end of file) */
	if (uio_resid(uio) == 0) {
		return 0;
	}
	if (uio_resid(uio) < (user_ssize_t)sizeof(t)) {
		return EINVAL;
	}
	lck_mtx_lock(&touch_lock);
	while (touch_head == touch_tail) {
		error = msleep(&touch_queue, &touch_lock, PZERO | PCATCH, "hakios-touch", NULL);
		if (error) {
			lck_mtx_unlock(&touch_lock);
			return error;
		}
	}
	while (touch_head != touch_tail && uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		t = touch_queue[touch_head++ % TOUCH_QUEUE];
		lck_mtx_unlock(&touch_lock);
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		lck_mtx_lock(&touch_lock);
		if (error) {
			/* the copy-out failed: put the record back at the head so a lost DOWN or UP does not leave a finger stuck (only if the ring still has room for it) */
			if (touch_tail - touch_head < TOUCH_QUEUE) {
				touch_queue[--touch_head % TOUCH_QUEUE] = t;
			}
			break;
		}
		moved++;
	}
	lck_mtx_unlock(&touch_lock);
	return moved ? 0 : error;
}

static int
touch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_touch t;
	int error = 0;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		if (error) {
			break;
		}
		if (t.type < HAKIOS_TOUCH_DOWN || t.type > HAKIOS_BUTTON_UP) {
			return EINVAL;
		}
		touch_post(t.type, t.finger, t.x, t.y, false, 0);
	}
	return error;
}

static const struct cdevsw touch_cdevsw = {
	.d_open = touch_open, .d_close = touch_close, .d_read = touch_read, .d_write = touch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

extern void hakios_esslog_start(void);
extern void hakios_exynos_dvfs_start(void);   /* drivers/core/hakios.exynos_acpm.c */
void
hakios_input_init(void)
{
	hakios_esslog_start();
	hakios_exynos_dvfs_start();
	int major = cdevsw_add(-1, &touch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-touch") == NULL) {
		printf("hakios-touch: cannot create the device\n");
	}
	exit_call = thread_call_allocate(exit_hold_elapsed, NULL);
	hakios_cpufreq_init();
	hakios_virtio_input_init();
	hakios_goodix_init();
	hakios_buttons_init();
	hakios_exynos_keys_init();
	hakios_s6sy761_init();
	hakios_melfas_mss100_init();
	hakios_s6e3ha8_init();
	hakios_s2mpb02_torch_init();
	hakios_exynos_abox_init();
	hakios_battery_init();
	hakios_scfb_battery_init();
	hakios_max77705_init();
	hakios_sm5713_init();
	hakios_exynos_camera_init();
	hakios_ufs_init();
	hakios_net_init();
	hakios_adreno_init();
	hakios_mali_init();
	hakios_exynos_pcie_init();
	hakios_bcm4361_bt_init();
	hakios_ssp_init();
	hakios_wifi_init();
	hakios_adsp_init();
	hakios_mpss_init();
}

/*
 * The USB console's "touch X Y" and "touch X1 Y1 X2 Y2 [MS]" (drivers/core/hakios.dwc3.c): a tap, or a swipe over MS
 * milliseconds (300 if not given), put into the queue as the panel's driver would, so that the phone can be worked
 * from the console with nobody at it (with "shot" to see what it did); and "button lock|home|up|down", a press of
 * that button as its driver would give it (the side button wakes a screen that has gone dark). X and Y are thousandths of the screen's width
 * and height (0-1000) from its top left, whatever the panel. Finger 9, which the panels' drivers do not use.
 */
#include <kern/thread.h>

extern kern_return_t thread_terminate(thread_t thread);

#define CONSOLE_FINGER  9

static struct {
	uint32_t x[2], y[2], ms;
	unsigned points;
	volatile bool busy;
} console_touch;

static void
console_touch_runs(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t steps = console_touch.points == 2 ? console_touch.ms / 16 + 1 : 0;
	uint32_t x0 = console_touch.x[0] * 65535 / 1000, y0 = console_touch.y[0] * 65535 / 1000;
	uint32_t x1 = console_touch.x[1] * 65535 / 1000, y1 = console_touch.y[1] * 65535 / 1000;
	char line[96];
	uint64_t began = mach_absolute_time(), began_ns, ended_ns;

	hakios_touch_post(HAKIOS_TOUCH_DOWN, CONSOLE_FINGER, x0, y0);
	for (uint32_t i = 1; i <= steps; i++) {
		delay_for_interval(console_touch.ms / steps ? console_touch.ms / steps : 1, NSEC_PER_MSEC);
		hakios_touch_post(HAKIOS_TOUCH_MOVE, CONSOLE_FINGER, (uint32_t)((int64_t)x0 + ((int64_t)x1 - x0) * i / steps),
		    (uint32_t)((int64_t)y0 + ((int64_t)y1 - y0) * i / steps));
	}
	if (steps == 0) {
		delay_for_interval(80, NSEC_PER_MSEC);
	}
	hakios_touch_post(HAKIOS_TOUCH_UP, CONSOLE_FINGER, steps ? x1 : x0, steps ? y1 : y0);
	snprintf(line, sizeof(line), "usb: touch: %s done\n", steps ? "the swipe" : "the tap");
	loader_log_line(line);
	absolutetime_to_nanoseconds(began, &began_ns);
	absolutetime_to_nanoseconds(mach_absolute_time(), &ended_ns);
	snprintf(line, sizeof(line), "usb: touch time begin_us=%llu end_us=%llu\n", began_ns / 1000, ended_ns / 1000);
	loader_log_line(line);
	console_touch.busy = false;
	thread_terminate(current_thread());
}

static void
console_button_runs(void *parameter, __unused wait_result_t result)
{
	uint32_t which = (uint32_t)(uintptr_t)parameter;

	hakios_touch_post(HAKIOS_BUTTON_DOWN, which, 0, 0);
	delay_for_interval(120, NSEC_PER_MSEC);
	hakios_touch_post(HAKIOS_BUTTON_UP, which, 0, 0);
	loader_log_line("usb: button: pressed and let go\n");
	console_touch.busy = false;
	thread_terminate(current_thread());
}

void
hakios_input_console_button(const char *arg)
{
	static const struct { const char *name; uint32_t which; } buttons[] = {
		{ "lock", HAKIOS_BUTTON_LOCK }, { "home", HAKIOS_BUTTON_HOME },
		{ "up", HAKIOS_BUTTON_VOLUME_UP }, { "down", HAKIOS_BUTTON_VOLUME_DOWN },
	};
	thread_t thread;

	for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (strcmp(arg, buttons[i].name) != 0) {
			continue;
		}
		if (console_touch.busy) {
			loader_log_line("usb: button: a touch or a press is under way\n");
			return;
		}
		console_touch.busy = true;
		if (kernel_thread_start(console_button_runs, (void *)(uintptr_t)buttons[i].which, &thread) != KERN_SUCCESS) {
			console_touch.busy = false;
			loader_log_line("usb: button: no thread for it\n");
			return;
		}
		thread_set_thread_name(thread, "touch-console");
		thread_deallocate(thread);
		return;
	}
	loader_log_line("usb: button lock|home|up|down: a press of that button\n");
}

void
hakios_input_console_touch(const char *arg)
{
	uint32_t v[5] = { 0 };
	unsigned n = 0;
	bool digits = false;
	thread_t thread;

	for (const char *p = arg;; p++) {
		if (*p >= '0' && *p <= '9' && n < 5) {
			v[n] = v[n] * 10 + (uint32_t)(*p - '0');
			digits = true;
			if (v[n] > 100000) {
				n = 9;
				break;
			}
		} else if (*p == ' ' || *p == 0) {
			if (digits) {
				n++;
				digits = false;
			}
			if (*p == 0) {
				break;
			}
		} else {
			n = 9;
			break;
		}
	}
	if ((n != 2 && n != 4 && n != 5) || v[0] > 1000 || v[1] > 1000 || (n >= 4 && (v[2] > 1000 || v[3] > 1000)) ||
	    (n == 5 && (v[4] < 16 || v[4] > 5000))) {
		loader_log_line("usb: touch X Y (a tap) or touch X1 Y1 X2 Y2 [MS] (a swipe, 300 ms by default): X and Y in "
		    "thousandths of the screen's width and height, from its top left\n");
		return;
	}
	if (console_touch.busy) {
		loader_log_line("usb: touch: one is under way\n");
		return;
	}
	console_touch.busy = true;
	console_touch.x[0] = v[0];
	console_touch.y[0] = v[1];
	console_touch.x[1] = v[2];
	console_touch.y[1] = v[3];
	console_touch.points = n >= 4 ? 2 : 1;
	console_touch.ms = n == 5 ? v[4] : 300;
	if (kernel_thread_start(console_touch_runs, NULL, &thread) != KERN_SUCCESS) {
		console_touch.busy = false;
		loader_log_line("usb: touch: no thread for it\n");
		return;
	}
	thread_set_thread_name(thread, "touch-console");
	thread_deallocate(thread);
}
