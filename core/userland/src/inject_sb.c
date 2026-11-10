/*
 * libhakios.sb.dylib: code of ours inside SpringBoard (see inject.c for how it gets there).
 *
 *   - The phone is "activated". SpringBoard asks mobileactivationd, which is not running (and
 *     could not talk to Apple anyway); without an answer it treats the device as bricked and
 *     shows the first-boot setup ("hello") instead of the lock screen.
 *   - The setup assistant is never needed, whatever the reason: nobody is going to go through
 *     it here.
 *   - SpringBoard starts a few seconds late where the kernel's boot-args say so (sb_delay=N;
 *     "xl run" does for the phone). /private/var is empty at every boot, so the applications
 *     are registered anew each time, one after another; one that is registered after
 *     SpringBoard has laid out the home screen ends up on a later page, and which one that is
 *     differs from boot to boot. On the phone that happens; under QEMU, where everything takes
 *     five times as long, it does not -- and there the delay is harmful: for a reason not yet
 *     found the window server then never takes our touch screen service for its own.
 *     (A wrapper program in the launchd job would be the obvious way to wait; launchd panics
 *     the system when a system job runs anything that is not Apple's.)
 *   - The picture SpringBoard keeps of an application that leaves the screen (for the application
 *     switcher and the next launch) is saved as JPEG or PNG, not as an ASTC texture: see
 *     snapshot_output_format().
 *   - Where CoreAnimation draws with its software renderer (QEMU; the phone without gpu=1) the
 *     "vibrant" views draw their content as it is: see plain_vibrancy().
 *   - A press of the side button that is long over is not taken for a long press (Siri) because
 *     SpringBoard was busy waking the display: see lock_long_press().
 *   - A long press on the lock screen's wallpaper does not open the poster switcher, which the
 *     window server here shows as a black screen: see poster_switcher_allowed().
 *   - With brightness_bridge=1 the brightness the user sets reaches the panel, which CoreBrightness
 *     alone never does here: see brightness_bridge().
 *   - Darwin notifications as a remote control, for scripts (rwtouch notify NAME):
 *       com.hakios.unlock      what the lock screen does after "swipe up to open"
 *       com.hakios.layers      write down the layers SpringBoard has on the screen: see layers()
 *       com.hakios.switcher    open the switcher of running applications: see switcher()
 *     In any other program given this library (INJECT in build.sh) only the last of them is
 *     answered, with that program's layers: its windows' and those of its CoreAnimation contexts
 *     that have no window (chronod's, where the home screen's widgets are drawn: rc.jobs.widgets).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/lock.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* where what is said goes: the standard error, which is the console's for SpringBoard (an application's is nowhere) */
static int say_fd = 2;

static void
say(const char *format, ...)
{
	char line[1280];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(say_fd, line, (size_t)n);
}

/*
 * Boot-arg touch_lat=1: a line for every turn of SpringBoard's main run loop -- when it woke up (a
 * touch, a timer, the display link ...), when CoreAnimation's commit of what that turn changed
 * began and when it was done (our observers just before and after its own, of order 2000000), in
 * microseconds of mach_absolute_time, as the other "tl:" lines (tools/latency.py, tools/frames.py),
 * and how much of the turn the main thread spent on a CPU (the rest it waited: on a reply, a lock):
 *   tl: a WOKE COMMIT DONE CPU_US
 * (a commit with nothing in it takes some microseconds; one that sends changes, a good deal more)
 */
#define CA_COMMIT_ORDER 2000000
static uint64_t main_woke, commit_began, main_cpu_woke;
static mach_timebase_info_data_t timebase;

static uint64_t
now_us(void)
{
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000;
}

/* the calling thread's CPU time, microseconds */
static uint64_t
thread_cpu_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void
main_loop_turn(__unused CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
	if (activity == kCFRunLoopAfterWaiting) {
		main_woke = now_us();
		main_cpu_woke = thread_cpu_us();
	} else if (activity == kCFRunLoopBeforeWaiting && info == NULL) {
		commit_began = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && main_woke != 0) {
		say("tl: a %llu %llu %llu %llu\n", main_woke, commit_began, now_us(), thread_cpu_us() - main_cpu_woke);
		main_woke = 0;
	}
}

static void
watch_main_loop(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *arg;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (arg = strstr(args, "touch_lat=1")) == NULL ||
	    (arg != args && arg[-1] != ' ')) {
		return;
	}
	mach_timebase_info(&timebase);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting, true, LONG_MIN,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, CA_COMMIT_ORDER - 1,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	{
		CFRunLoopObserverContext after = { .info = (void *)1 };

		CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true,
		    CA_COMMIT_ORDER + 1, main_loop_turn, &after), kCFRunLoopCommonModes);
	}
}

/* ---- methods of SpringBoard's classes, replaced --------------------------------------------------- */

/*
 * SpringBoard's own frame rate caps (boot-arg sb_fps=0 leaves them as Apple set them). Some of SpringBoard's animations
 * ask CoreAnimation for fewer frames than the panel has, through their settings' frame rate range: the crossfade of the
 * home screen's page dots and the search pill below the icons that follows every page's slide is
 * CAFrameRateRangeMake(20, 30, 0) (-[SBFolderScrollAccessoryView _setUpAnimation], both pageControlFadeInSettings and
 * auxiliaryViewFadeInSettings, Darwin userland 16.5, 20F66). CoreAnimation then gives SpringBoard's display link 30
 * a second for as long as that animation runs: 400 to 700 ms after the finger lifts and again from 800 ms, half of
 * all the refreshes missed in the USB console's swipe test (3 November: SpringBoard's commits 33.3 ms apart there,
 * the swaps' own timestamps 33.3 ms apart, nothing late). On an iPhone the same 30 a second is a power saving. Here
 * the ranges are raised, as they are set, so that they reach at least the panel's rate: a range whose most is under
 * SB_FPS_FLOOR gets that as its most and as its preferred rate; one that already allows it is left alone.
 */
#define SB_FPS_FLOOR    60.0f
typedef struct {
	float minimum, maximum, preferred;
} sb_frame_rate_range;
static unsigned sb_fps_raised;

static sb_frame_rate_range
sb_fps_raise(sb_frame_rate_range range)
{
	if (range.maximum > 0.0f && range.maximum < SB_FPS_FLOOR) {
		range.maximum = SB_FPS_FLOOR;
		range.preferred = SB_FPS_FLOOR;
		__atomic_fetch_add(&sb_fps_raised, 1, __ATOMIC_RELAXED);
	}
	return range;
}

/*
 * -[SBFFluidBehaviorSettings setFrameRateRange:highFrameRateReason:] and -[SBFAnimationSettings ...]: the same, both
 * "v32@0:8{CAFrameRateRange=fff}16I28". (Not their setPreferredFrameRateRange:, which takes the settings object that
 * holds the range, "v24@0:8@16", set through key-value coding from -initWithDefaultValues.)
 */
static void (*original_fluid_set_range)(id self, SEL selector, sb_frame_rate_range range, unsigned reason);
static void (*original_animation_set_range)(id self, SEL selector, sb_frame_rate_range range, unsigned reason);

static void
fluid_set_range(id self, SEL selector, sb_frame_rate_range range, unsigned reason)
{
	original_fluid_set_range(self, selector, sb_fps_raise(range), reason);
}

static void
animation_set_range(id self, SEL selector, sb_frame_rate_range range, unsigned reason)
{
	original_animation_set_range(self, selector, sb_fps_raise(range), reason);
}

static IMP replace_method(const char *class_name, const char *selector, uintptr_t expected, IMP replacement);

static void
sb_fps_hooks(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *arg;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (arg = strstr(args, "sb_fps=0")) != NULL &&
	    (arg == args || arg[-1] == ' ')) {
		say("hakios: SpringBoard's own frame rate caps left as they are (sb_fps=0)\n");
		return;
	}
	original_fluid_set_range = (void (*)(id, SEL, sb_frame_rate_range, unsigned))replace_method("SBFFluidBehaviorSettings",
	    "setFrameRateRange:highFrameRateReason:", 0x18e62906cUL, (IMP)fluid_set_range);
	original_animation_set_range = (void (*)(id, SEL, sb_frame_rate_range, unsigned))replace_method("SBFAnimationSettings",
	    "setFrameRateRange:highFrameRateReason:", 0x18e5f6de8UL, (IMP)animation_set_range);
	say("hakios: SpringBoard's frame rate caps raised to %.0f a second: fluid settings %s, animation settings %s "
	    "(sb_fps=0: as Apple set them)\n", SB_FPS_FLOOR,
	    original_fluid_set_range ? "yes" : "NO", original_animation_set_range ? "yes" : "NO");
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/*
 * Replace -[CLASS SELECTOR] (or +[CLASS SELECTOR]: `kind` is that first character), whose code must now
 * be at the unslid address `expected` (Darwin userland 16.5, 20F66: what a replacement does depends on what the
 * original did). Returns the original.
 * (dyld interposing of the C functions underneath does not reach calls made from inside the
 * shared cache: there one library branches straight into the other.)
 */
static IMP
replace_method_of_kind(char kind, const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls == NULL ? NULL : kind == '+' ? class_getClassMethod(cls, sel_registerName(selector)) :
	    class_getInstanceMethod(cls, sel_registerName(selector));
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != expected + slide) {
		say("hakios: %c[%s %s]: found %#lx, expected %#lx; left alone\n", kind, class_name, selector,
		    original ? (uintptr_t)original - slide : 0, expected);
		return NULL;
	}
	method_setImplementation(method, replacement);
	return original;
}

static IMP
replace_method(const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	return replace_method_of_kind('-', class_name, selector, expected, replacement);
}

/*
 * -[SBLockdownManager _setupActivationState] asks MobileActivation and keeps the answer in the
 * int at +8: 2 = activated, 1 = not, 3 = unknown; it stays 0 when there is no answer.
 */
#define LOCKDOWN_STATE(manager)         (*(int *)((char *)(manager) + 8))
#define LOCKDOWN_ACTIVATED              2

static void
setup_activation_state(id self, __unused SEL selector)
{
	LOCKDOWN_STATE(self) = LOCKDOWN_ACTIVATED;
}

/* -[SBSetupManager _setSetupRequiredReason:]: 0 = no need for the setup assistant */
static void (*original_set_setup_required_reason)(id self, SEL selector, long reason);

static void
set_setup_required_reason(id self, SEL selector, long reason)
{
	if (reason != 0) {
		say("hakios: SpringBoard wants the setup assistant (reason %ld); not here\n", reason);
	}
	original_set_setup_required_reason(self, selector, 0);
}

/*
 * +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] chooses the file format of the picture
 * SpringBoard takes of an application that leaves the screen: 0 = PNG, 1 and 2 = ATX (an ASTC texture),
 * 3 = JPEG. A device with a wide colour display that can decode ASTC gets ATX, and Darwin compresses it on
 * the processor: fast with the vector code of Apple's cores, but here it takes one core more than ten
 * seconds per picture (under QEMU; seconds on the phone) -- and all that time the application waits for
 * the answer, until the watchdog for scene updates (10 s, there is no setting to scale it) kills it:
 * no application survived going to the background. JPEG takes a fraction of a second; it has no
 * transparency, so a picture that needs it becomes a PNG (which is what Darwin itself does on a device
 * without ASTC).
 */
#define SNAPSHOT_FORMAT_PNG             0
#define SNAPSHOT_FORMAT_JPEG            3

static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");

	if (snapshot != NULL && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		return SNAPSHOT_FORMAT_JPEG;
	}
	return SNAPSHOT_FORMAT_PNG;
}

/* ---- battery state from redwood's Qualcomm charger stack ----------------------------------------- */

/*
 * Darwin reads battery level through UIDevice.batteryLevel and UIDevice.batteryState; SpringBoard's
 * status bar reads it the same way via SBBatteryController. Where the battery has no power source
 * of its own in the kernel (battery=1, the loader's way through Qualcomm's UEFI charger protocols),
 * we hook the UIDevice accessors to return what XNU publishes as kern.hakios.battery_soc (0..100)
 * and kern.hakios.battery_charging.
 * When the sysctl is missing (battery=1 not in boot-args, or the driver never started) we hand
 * back the value the real implementation computed; the status bar then shows whatever Darwin last
 * knew, which is better than crashing.
 * With adsp=1 the battery is a power source (the service HakiosBattery, an IOPMPowerSource:
 * drivers/power/hakios.battery.cpp), which powerd reads like an iPhone's own, and from it Darwin knows
 * everything by itself; once that service is there, the hooks hand everything to Darwin's own code.
 */
static float  (*original_battery_level)(id self, SEL sel);
static long   (*original_battery_state)(id self, SEL sel);
static BOOL   (*original_battery_view_shows_percentage)(id self, SEL sel);
static void   (*original_battery_view_set_shows_percentage)(id self, SEL sel, BOOL shows);
static BOOL   (*original_battery_view_should_show_bolt)(id self, SEL sel);
static double (*original_battery_view_charge_percent)(id self, SEL sel);
static void   (*original_battery_view_set_charge_percent)(id self, SEL sel, double p);
static double (*original_ring_charging_level)(id self, SEL sel);

static int
sysctl_int(const char *name)
{
	int value = -1;
	size_t size = sizeof(value);

	sysctlbyname(name, &value, &size, NULL, 0);
	return value;
}

/* Is the battery a power source that Darwin reads by itself (see above)? Once it is, it stays one. */
static bool
battery_power_source(void)
{
	static bool found;
	io_service_t service;

	if (!found && (service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosBattery"))) != 0) {
		IOObjectRelease(service);
		found = true;
	}
	return found;
}

/* ---- the cellular items of the status bar, from the kernel's modem ------------------------------ */

/*
 * SpringBoard's SBStatusBarStateAggregator fills the raw status bar data from CoreTelephony, which
 * has no baseband here. Its item updaters take the data and a getter and setter for the field
 * (Darwin userland 16.5: -[SBStatusBarStateAggregator _updateSignalStrengthItem:withData:SIMInfo:barsGetter:
 * barsSetter:], and the like for the service string): the signal one is replaced so that, when the
 * kernel's modem is registered (sysctl kern.hakios.cellular_registered, _bars: hakios.mpss.c),
 * the bars are set and the item switched on (-[... _setItem:enabled:]). The aggregator's whole
 * method list is said once in the log, for the posting to be found; and the signal item is asked
 * to update itself every few seconds while the kernel's state changes.
 */
static void (*original_update_signal_item)(id self, SEL sel, long item, void *data, id sim, void *getter, void *setter);
static id cellular_aggregator;
static int cellular_last_bars = -2, cellular_said;

/* The getters and setters are blocks (their type is "@?"): called through the block's own invoke pointer, with the block first. */
struct block_layout {
	void *isa;
	int flags, reserved;
	void (*invoke)(void *, ...);
};
#define BLOCK_CALL(block, type, ...) ((type)((struct block_layout *)(block))->invoke)((block), __VA_ARGS__)

static void
update_signal_item(id self, SEL sel, long item, void *data, id sim, void *getter, void *setter)
{
	int up = sysctl_int("kern.hakios.cellular_up"), registered = sysctl_int("kern.hakios.cellular_registered");
	int bars = sysctl_int("kern.hakios.cellular_bars"), dbm = sysctl_int("kern.hakios.cellular_dbm");
	int shown;

	cellular_aggregator = self;
	if (original_update_signal_item) {
		original_update_signal_item(self, sel, item, data, sim, getter, setter);
	}
	if (cellular_said++ < 8) {
		say("hakios: cellular: signal item %ld: data %p, SIM info %p, getter %p, setter %p; the kernel says up %d registered %d bars %d dbm %d\n",
		    item, data, (void *)sim, getter, setter, up, registered, bars, dbm);
	}
	/*
	 * Force the signal item on. Darwin hides it unless its own cellular stack (CommCenter, which is
	 * not here) says there is service, so the bars never appear on their own. The kernel knows
	 * the modem's state, so once it is up the item is shown here with the kernel's bars; when the
	 * kernel has a reading but zero bars (a brief "no signal" sample, or a reading just after
	 * registration) at least one bar is forced so the icon is visibly present rather than empty.
	 * modem_bars_off=1 (as a sysctl) is not honoured; set kern.hakios.cellular_up=0 to hide it.
	 */
	/*
	 * The kernel's NAS state alternates between two reports -- "registered, signal not read" and
	 * "not registered, -86 dBm" -- so at the moments it says registered the dBm reads 0. The last
	 * real reading (within a minute) stands in for it.
	 */
	{
		static int last_dbm;
		static time_t last_dbm_at;
		time_t now = time(NULL);

		if (dbm < 0) {
			last_dbm = dbm;
			last_dbm_at = now;
		} else if (last_dbm < 0 && now - last_dbm_at < 60) {
			dbm = last_dbm;
		}
	}
	shown = bars;
	if (shown <= 0 && dbm < 0) {
		shown = dbm >= -95 ? 4 : dbm >= -105 ? 3 : dbm >= -115 ? 2 : 1;   /* derive from dBm when bars is 0 */
	}
	if (shown <= 0 && (up > 0 || registered > 0)) {
		shown = 1;                      /* up but no reading yet: still show the icon */
	}
	if (shown > 5) {
		shown = 5;
	}
	if ((up > 0 || registered > 0) && shown > 0 && setter != NULL && data != NULL) {
		BLOCK_CALL(setter, void (*)(void *, void *, int), data, shown);
		((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
		if (shown != cellular_last_bars) {
			say("hakios: cellular: %d bars shown (item %ld; kernel bars %d, %d dBm, registered %d)\n", shown, item, bars, dbm, registered);
		}
	}
	cellular_last_bars = shown;
}

static void (*original_update_service_item)(id self, SEL sel, long item, void *data, id sim, id other, void *g1, void *s1, void *g2, void *s2,
    void *g3, void *s3, void *g4, void *s4);

/*
 * The service item ("No SIM", the network's name): once the kernel's modem is registered, the
 * network's name (sysctl kern.hakios.cellular_operator) goes in through the item's string setter
 * and the item is switched on.
 */
static void
update_service_item(id self, SEL sel, long item, void *data, id sim, id other, void *g1, void *s1, void *g2, void *s2, void *g3, void *s3,
    void *g4, void *s4)
{
	static int said;
	static char shown[64];
	char name[64] = "";
	size_t size = sizeof(name) - 1;
	int registered = sysctl_int("kern.hakios.cellular_registered");

	if (original_update_service_item) {
		original_update_service_item(self, sel, item, data, sim, other, g1, s1, g2, s2, g3, s3, g4, s4);
	}
	sysctlbyname("kern.hakios.cellular_operator", name, &size, NULL, 0);
	if (said++ < 4) {
		say("hakios: cellular: service item %ld: data %p, SIM info %p, other %p; string getter %p setter %p, content type getter %p "
		    "setter %p; the kernel's network \"%s\"\n", item, data, (void *)sim, (void *)other, g1, s1, g3, s3, name);
	}
	if (registered > 0 && name[0] != 0 && s1 != NULL && data != NULL) {
		CFStringRef string = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);

		if (string != NULL) {
			BLOCK_CALL(s1, void (*)(void *, void *, CFStringRef), data, string);
			((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
			if (strcmp(shown, name) != 0) {
				snprintf(shown, sizeof(shown), "%s", name);
				say("hakios: cellular: service \"%s\" shown (item %ld)\n", name, item);
			}
			CFRelease(string);
		}
	}
}

/*
 * The bars are drawn by UIKit's signal view, whose "signal mode" says how: inactive (all bars grey),
 * searching, or active (the bars that count dark). Darwin's cellular stack (CommCenter, not here)
 * puts it in active mode once there is service; without it the bars stay grey whatever their
 * number. So for the cellular signal view (not the Wi-Fi one), while the kernel says the modem is
 * registered, the mode is made active. The modes seen are said in the log once each.
 */
static void (*original_set_signal_mode)(id self, SEL sel, long mode);
static long cellular_active_mode = 2;  /* _UIStatusBarSignalView: 0 inactive, 1 searching, 2 active */

static void
set_signal_mode(id self, SEL sel, long mode)
{
	static unsigned said_modes;
	const char *name = object_getClassName(self);
	bool cellular = name != NULL && strstr(name, "Cellular") != NULL;
	long wanted = mode;

	if (cellular && mode < 8 && !(said_modes & (1u << mode))) {
		said_modes |= 1u << mode;
		say("hakios: cellular: %s asked for signal mode %ld\n", name, mode);
	}
	if (cellular && sysctl_int("kern.hakios.cellular_registered") > 0 && mode != cellular_active_mode) {
		wanted = cellular_active_mode;
	}
	original_set_signal_mode(self, sel, wanted);
}

static void
signal_view_hook(void)
{
	static const char *const classes[] = { "_UIStatusBarCellularSignalView", "_UIStatusBarSignalView" };

	for (unsigned c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
		Class cls = objc_getClass(classes[c]);
		Method method = cls ? class_getInstanceMethod(cls, sel_registerName("setSignalMode:")) : NULL;

		if (method == NULL) {
			say("hakios: cellular: no -[%s setSignalMode:]\n", classes[c]);
			continue;
		}
		original_set_signal_mode = (void (*)(id, SEL, long))method_getImplementation(method);
		method_setImplementation(method, (IMP)set_signal_mode);
		say("hakios: cellular: -[%s setSignalMode:] replaced (types \"%s\"): active while the kernel says registered\n",
		    classes[c], method_getTypeEncoding(method));
		return;         /* the base class's method serves the subclasses */
	}
}

static void
cellular_hook(void)
{
	signal_view_hook();
	{
		const char *service_selector = "_updateServiceItem:withData:SIMInfo:otherSIMInfo:serviceStringGetter:serviceStringSetter:"
		    "serviceCrossfadeStringGetter:serviceCrossfadeStringSetter:serviceContentTypeGetter:serviceContentTypeSetter:"
		    "serviceBadgeStringGetter:serviceBadgeStringSetter:";
		Class cls = objc_getClass("SBStatusBarStateAggregator");
		Method method = cls ? class_getInstanceMethod(cls, sel_registerName(service_selector)) : NULL;

		if (method) {
			say("hakios: cellular: -[SBStatusBarStateAggregator %s] at %#lx, types \"%s\": looked at\n", service_selector,
			    (uintptr_t)method_getImplementation(method) - slide, method_getTypeEncoding(method));
			original_update_service_item = (void (*)(id, SEL, long, void *, id, id, void *, void *, void *, void *, void *, void *, void *,
			    void *))method_getImplementation(method);
			method_setImplementation(method, (IMP)update_service_item);
		}
	}
	const char *selector = "_updateSignalStrengthItem:withData:SIMInfo:barsGetter:barsSetter:";
	Class cls = objc_getClass("SBStatusBarStateAggregator");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (method == NULL) {
		say("hakios: cellular: no -[SBStatusBarStateAggregator %s]\n", selector);
		return;
	}
	say("hakios: cellular: -[SBStatusBarStateAggregator %s] at %#lx, types \"%s\": replaced\n", selector,
	    (uintptr_t)method_getImplementation(method) - slide, method_getTypeEncoding(method));
	original_update_signal_item = (void (*)(id, SEL, long, void *, id, void *, void *))method_getImplementation(method);
	method_setImplementation(method, (IMP)update_signal_item);
	{
		/* the whole class, once: which updaters and posters there are */
		unsigned count = 0;
		Method *list = class_copyMethodList(cls, &count);
		char line[1024] = "";
		size_t at = 0;

		for (unsigned i = 0; list && i < count; i++) {
			const char *name = sel_getName(method_getName(list[i]));
			size_t n = strlen(name);

			if (at + n + 2 > sizeof(line)) {
				say("hakios: cellular: aggregator methods: %s\n", line);
				line[0] = 0;
				at = 0;
			}
			memcpy(line + at, name, n);
			at += n;
			line[at++] = ' ';
			line[at] = 0;
		}
		if (at) {
			say("hakios: cellular: aggregator methods: %s\n", line);
		}
		free(list);
	}
	/* every 3 s: when the kernel's registration or bars change, the item is asked to update itself */
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());

	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), 3 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
	dispatch_source_set_event_handler(timer, ^{
		static int last_registered = -2, last_bars = -2;
		int registered = sysctl_int("kern.hakios.cellular_registered"), bars = sysctl_int("kern.hakios.cellular_bars");

		if ((registered != last_registered || bars != last_bars) && cellular_aggregator != nil) {
			SEL update = sel_registerName("_updateSignalStrengthItem");

			last_registered = registered;
			last_bars = bars;
			if (class_getInstanceMethod(object_getClass(cellular_aggregator), update)) {
				((void (*)(id, SEL))objc_msgSend)(cellular_aggregator, update);
				say("hakios: cellular: the signal item asked to update (registered %d, bars %d)\n", registered, bars);
			}
		}
	});
	dispatch_resume(timer);
}

/* UIDevice.batteryLevel: 0.0..1.0; -1.0 if monitoring not enabled */
static float
battery_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_level ? original_battery_level(self, sel) : -1.0f;
	}
	return (float)soc / 100.0f;
}

/* UIDevice.batteryState: 0=unknown 1=unplugged 2=charging 3=full */
static long
battery_state(id self, SEL sel)
{
	int charging = sysctl_int("kern.hakios.battery_charging");
	int present = sysctl_int("kern.hakios.battery_present");
	int plugged = sysctl_int("kern.hakios.battery_charger_present");
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (charging == -1 || present == -1 || plugged == -1 || soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_state ? original_battery_state(self, sel) : 0;
	}
	if (!present) return 0;
	if (plugged && soc >= 100) return 3;
	return charging ? 2 : 1;
}

/*
 * UIKitCore's status-bar view has its own presentation policy, separate from UIDevice. With the power
 * source it is Darwin's own again: the switch Battery Percentage in Settings > Battery, which the image
 * turns on (SBShowBatteryPercentage in userland/var/mobile/Library/Preferences/com.apple.springboard.plist).
 */
static BOOL
battery_view_shows_percentage(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return YES;
	return original_battery_view_shows_percentage ? original_battery_view_shows_percentage(self, sel) : NO;
}

static void
battery_view_set_shows_percentage(id self, SEL sel, BOOL shows)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	original_battery_view_set_shows_percentage(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? YES : shows);
}

static BOOL
battery_view_should_show_bolt(id self, SEL sel)
{
	int charging = sysctl_int("kern.hakios.battery_charging");
	int present = sysctl_int("kern.hakios.battery_present");
	int plugged = sysctl_int("kern.hakios.battery_charger_present");

	if (charging != -1 && present != -1 && plugged != -1 && !battery_power_source()) return charging && present && plugged;
	return original_battery_view_should_show_bolt ? original_battery_view_should_show_bolt(self, sel) : NO;
}

/*
 * The actual number the status bar draws. _UIBatteryView keeps the fill fraction (0.0..1.0) in
 * chargePercent; it is set through setChargePercent: and read back for layout. Darwin feeds it from
 * BatteryCenter / IOPowerSources, which is empty on this phone, so without us it sits near zero
 * (the "1%" the user saw). We override both sides: setChargePercent: so the stored value is ours
 * no matter who writes it, and the getter so any layout path that reads it agrees.
 */
static double
battery_view_charge_percent(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_battery_view_charge_percent ? original_battery_view_charge_percent(self, sel) : 0.0;
}

static void
battery_view_set_charge_percent(id self, SEL sel, double p)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	original_battery_view_set_charge_percent(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? (double)soc / 100.0 : p);
}

/* The big charging ring on the lock screen (CoverSheet) reads its fill level the same way. */
static double
ring_charging_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.hakios.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_ring_charging_level ? original_ring_charging_level(self, sel) : 0.0;
}

/* Keep SpringBoard's normal battery observers awake while the XNU cache changes. */
static void
battery_post_notifications(void)
{
	Class uidev = objc_getClass("UIDevice");
	Class notification_center = objc_getClass("NSNotificationCenter");
	id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, sel_registerName("currentDevice")) : nil;
	id center = notification_center ? ((id (*)(Class, SEL))objc_msgSend)(notification_center,
	    sel_registerName("defaultCenter")) : nil;

	if (center != nil && !battery_power_source()) {
		SEL post = sel_registerName("postNotificationName:object:");
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryLevelDidChangeNotification"), device);
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryStateDidChangeNotification"), device);
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		battery_post_notifications();
	});
}

/* ---- the lock screen's clock without a GPU ------------------------------------------------------- */

#define HAKIOS_GL "/redwood/lib/libhakios.gl.dylib"

/* Does the window server draw with OpenGL ES? The same question, asked the same way, as opengl_es() in inject.c. */
static bool
gpu_draws(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;

	if (access(HAKIOS_GL, R_OK) != 0) {
		return false;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				return argument[3] != '0';
			}
		}
	}
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
	if (gpu == 0) {
		gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosMali"));     /* (the Galaxy S9's) */
	}
	if (gpu != 0) {
		IOObjectRelease(gpu);
	}
	return gpu != 0;
}

/*
 * The clock of the lock screen is a "vibrant" view (BSUIVibrancyEffectView in BaseBoardUI): its
 * digits are not drawn, they are a mask -- a copy of them through a colour matrix that keeps red
 * as alpha -- through which the filter vibrantColorMatrix lets the wallpaper behind come out
 * lighter; and a gradient over the whole screen, red at the top, tones the wallpaper down the
 * same way. On the GPU that is what is seen (eagl.c draws the two filters). CoreAnimation's
 * software renderer shows neither: the digits come out red, and the top of the wallpaper as it
 * was. Why was not found out; that renderer is in QuartzCore, not here.
 * Such views have a switch, the user default BSUIVibrancyEffectViewDisabled: with it they show
 * their content as it is drawn, the clock in plain white. It is set for this run of SpringBoard
 * alone (nothing is written), and only where the software renderer draws.
 */
static void
plain_vibrancy(void)
{
	if (gpu_draws()) {
		return;
	}
	CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
	say("hakios: SpringBoard: no GPU draws, vibrant views are plain (the lock screen's clock is white)\n");
}

/* ---- the side button's long press ---------------------------------------------------------------- */

/*
 * -[SBLockHardwareButton longPress:] is what the side button's long press recognizer calls: Siri
 * (here, its "turn on Siri" sheet, nobody having turned it on). The recognizer is a timer of
 * SpringBoard's own: started when the main thread sees the press, stopped when it sees the release
 * -- and a release it has not got round to is no release. A press of the side button with the
 * display off wakes the display from inside the handling of that very press, which on an iPhone
 * takes no time. Here it takes as long as the software renderer needs for the first picture of the
 * lock screen: a tenth of a second on the phone, a second under QEMU. That is longer than a long
 * press, so there every wake by the button (and by a tap, which is a press of that button made in
 * inject.c) ended with Siri's sheet over the lock screen, however short the press had been.
 * Where the button's events come from -- inject.c, in backboardd -- it is known whether the button
 * is still held; a long press of a button that is not is dropped here.
 */
#define GESTURE_BEGAN                   1       /* UIGestureRecognizerState */
#define GESTURE_ENDED                   3       /* and cancelled, failed after it */

static void (*original_lock_long_press)(id self, SEL selector, id recognizer);

static bool
lock_button_held(void)
{
	static int token = NOTIFY_TOKEN_INVALID;
	uint64_t state = 0;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return true;
	}
	/* 1 = released; held, or not known: as SpringBoard has it */
	return notify_get_state(token, &state) != NOTIFY_STATUS_OK || state != 1;
}

static void
lock_long_press(id self, SEL selector, id recognizer)
{
	static bool dropping;
	SEL state_selector = sel_registerName("state");
	long state = recognizer != NULL && class_getInstanceMethod(object_getClass(recognizer), state_selector) != NULL ?
	    ((long (*)(id, SEL))objc_msgSend)(recognizer, state_selector) : 0;

	if (state == GESTURE_BEGAN) {
		dropping = !lock_button_held();
		if (dropping) {
			say("hakios: SpringBoard: a long press of the side button, which was released long ago: dropped\n");
		}
	}
	if (dropping) {
		if (state >= GESTURE_ENDED) {
			dropping = false;
		}
		return;
	}
	original_lock_long_press(self, selector, recognizer);
}

/* ---- remote control ------------------------------------------------------------------------------ */

#define UNLOCK_SOURCE_COVER_SHEET       24      /* what the lock screen itself passes */

static void
unlock(void)
{
	Class cls = objc_getClass("SBLockScreenManager");
	SEL shared = sel_registerName("sharedInstance"), locked = sel_registerName("isUILocked");
	SEL unlock_ui = sel_registerName("unlockUIFromSource:withOptions:");
	id manager;

	if (cls == NULL || class_getClassMethod(cls, shared) == NULL || class_getInstanceMethod(cls, locked) == NULL ||
	    class_getInstanceMethod(cls, unlock_ui) == NULL) {
		say("hakios: unlock: SBLockScreenManager is not what it was\n");
		return;
	}
	manager = ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
	if (manager == NULL) {
		say("hakios: unlock: no lock screen manager yet\n");
	} else if (!((BOOL (*)(id, SEL))objc_msgSend)(manager, locked)) {
		say("hakios: unlock: not locked\n");
	} else {
		BOOL done = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, NULL);

		say("hakios: unlock: asked, answer %d\n", done);
	}
}

/*
 * The poster switcher: on Darwin userland 16 a long press on the lock screen's wallpaper shrinks it into a
 * row of posters to choose from or customize. The row is PosterBoard's, an application scene of
 * that process which the lock screen hosts (CSPosterSwitcherViewController, a CALayerHost of the
 * context of PosterBoard's window over an opaque black layer). PosterBoard does lay it out -- its
 * window has the posters, "Customize" and "+", the hosted context is that window's, the scene is
 * in the foreground -- but the window server draws nothing of that context, with either
 * renderer, so what is left is the black layer under it, until a swipe up takes the switcher
 * away. Why is not known: the display is not in secure mode, SpringBoard counts the user as
 * authenticated, the same nesting of hosted contexts works for the wallpaper itself.
 * Until it is, the switcher is not offered, the way SpringBoard itself withholds it where it has
 * none (an iPad, Guided Access): its policy says the capability is not there, and the long press
 * does nothing.
 */
static BOOL
poster_switcher_allowed(__unused id self, __unused SEL selector, id *explanation)
{
	if (explanation != NULL) {
		*explanation = (id)CFSTR("the poster switcher is not shown here (see inject_sb.c)");
	}
	return NO;
}

/*
 * What SpringBoard has put on the screen, as CoreAnimation has it: every layer of every window, a
 * line each -- class and name, place, opacity, what it shows (an image, a colour, another
 * process's layers), and what is done to it: its filters by type with their inputs, in order,
 * the compositing filter, the mask. That is where a picture that comes out wrong on one renderer
 * and right on another is described: the same description goes to both.
 */
#define SEND(type, object, name)        ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))
#define SEND1(type, object, name, argument_type, argument) \
	((type (*)(id, SEL, argument_type))objc_msgSend)((id)(object), sel_registerName(name), argument)

static const char *layers_of = "";               /* in another program than SpringBoard: " of" and its name */

static const char *
text_of(id object)
{
	id text = object ? SEND(id, object, "description") : NULL;

	return text ? SEND(const char *, text, "UTF8String") : "-";
}

/*
 * The switcher of running applications lays a wash of wallpaper over every card behind the front
 * one, above the dimming: -[SBAppSwitcherPageView _createOrDestroyWallpaperOverlayView] makes it,
 * PaperBoardUI fills it from the image provider of the wallpaper, and the result is drawn at about
 * fifteen per cent (the layer tree: SBWallpaperEffectView at opacity 0.1497 -> PBUIFakeBlurView ->
 * PBUIWallpaperView -> PBUISnapshotReplicaView -> a UIImageView).
 *
 * Here there is no picture of the wallpaper to put in it. The wallpaper itself is drawn live by
 * PosterBoard, in another process, and hosted into SpringBoard's layer tree; SpringBoard has no
 * bitmap of it, and nothing ever bakes one -- after three and a half minutes of an idle phone
 * /var/mobile/Library holds sixty directories and not one of them is PaperBoard, PosterBoard or
 * Wallpaper. So the provider hands over a placeholder: 846 x 1827 pixels of one colour,
 * (179, 77, 179), which CoreAnimation compresses to ASTC and draws over the two cards on the left
 * at 14.9 per cent. That is what made them purple instead of merely dimmed.
 *
 * The dimming itself is right and stays: two draws of a flat black at 11.4 per cent, which is the
 * shadow one expects to see. Only the wash is left out, because a wash of a colour that is not the
 * wallpaper's is worse than no wash at all. When something here can give PaperBoardUI a real
 * picture of the wallpaper, this hook is what should go -- and until then switcher_wallpaper=1
 * among the boot-args puts it back as Darwin asks for it, so that whoever works on the wallpaper can
 * see at once whether their picture arrives, without rebuilding an image to find out.
 */
static void (*original_switcher_wallpaper_overlay)(id self, SEL selector);

static void
switcher_wallpaper_overlay(id self, SEL selector)
{
	static int wanted = -1;         /* boot-arg switcher_wallpaper=1: have it back, placeholder and all */
	static unsigned told;

	if (wanted < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		wanted = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "switcher_wallpaper=1")) != NULL && (at == args || at[-1] == ' ');
	}
	if (told++ == 0) {
		say("hakios: the switcher's wash of wallpaper is %s\n", wanted ?
		    "drawn as Darwin asks (switcher_wallpaper=1)" : "left out: there is no picture of the wallpaper for it");
	}
	if (wanted) {
		original_switcher_wallpaper_overlay(self, selector);
	}
}

/*
 * SIRI'S ORB, which is where SpringBoard dies on this port.
 *
 * -[SpringBoard applicationDidFinishLaunching:] builds the banner manager, which builds
 * SBAssistantController, which builds Siri's presentation controller, whose -prewarmOrbViewShaders
 * makes a SUICOrbView whose -_updateMetalTextureRasterSize: asks Metal for something Metal will not
 * give: _MTLReportFailure, then abort. 48 times in the boot of 21 October, every 8.3 seconds, with
 * backboardd alive throughout for the first time.
 *
 * The message Metal reported is in none of our logs -- SpringBoard's own os_log output never
 * reaches them, where backboardd's did -- so this stands one frame above the fall and says what the
 * process has to work with: whether MTLCreateSystemDefaultDevice() answers in SpringBoard at all,
 * and with which class. That one line separates two very different diseases: our device never
 * reaching this process, and our device reaching it and refusing something.
 *
 * The prewarm is skipped unless siri_orb=1. Enabling Siri (the sheet, then the orb) ran it and
 * aborted in MPS ("Failed to get MPSDevice": this Metal device answers Apple1, MPS wants Apple2).
 * Skip keeps SpringBoard up; the orb is empty until metal_profile and MPS agree.
 */
static void (*original_prewarm_orb_view_shaders)(id self, SEL selector);

static bool
boot_arg_is_set(const char *word)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *at;

	return sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
	    (at = strstr(args, word)) != NULL && (at == args || at[-1] == ' ');
}

static void
prewarm_orb_view_shaders(id self, SEL selector)
{
	static int skip = -1;
	id (*create)(void) = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
	id device = create ? create() : nil;

	if (skip < 0) {
		skip = !boot_arg_is_set("siri_orb=1");
	}
	if (create == NULL) {
		say("hakios: Siri's orb: MTLCreateSystemDefaultDevice is not even a symbol here\n");
	} else {
		say("hakios: Siri's orb prewarms its shaders; MTLCreateSystemDefaultDevice() = %s\n",
		    device ? object_getClassName(device) : "nil");
	}
	if (skip) {
		say("hakios: Siri's orb: the prewarm is skipped (siri_orb=0), so what follows is the next wall\n");
		return;
	}
	original_prewarm_orb_view_shaders(self, selector);
	say("hakios: Siri's orb: the prewarm RETURNED -- it did not abort this time\n");
}

/*
 * SiriUIActivation arrives only when SpringBoard first wants Siri, which is after everything else
 * this library replaces at startup. So this waits for the class rather than for a named image: the
 * notifier runs once per image already loaded and once per image afterwards, and the first time the
 * class is there the replacement goes in.
 */
static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	static bool told;

	if (original_prewarm_orb_view_shaders != NULL || objc_getClass("SiriPresentationViewController") == NULL) {
		return;
	}
	original_prewarm_orb_view_shaders = (void (*)(id, SEL))replace_method("SiriPresentationViewController",
	    "prewarmOrbViewShaders", 0x1f0144c30UL, (IMP)prewarm_orb_view_shaders);
	if (!told++) {
		say("hakios: Siri's orb: the prewarm is %s\n",
		    original_prewarm_orb_view_shaders ? "watched" : "NOT watched (the address moved)");
	}
}

static void
layer_filter(id filter, const char *what, int depth)
{
	static const char *const inputs[] = { "inputColorMatrix", "inputRadius", "inputAmount", "inputColor", "inputAngle",
		                              "inputPremultipliedValues", "inputNormalizeEdges", "inputReversed" };
	char line[1024];
	size_t n;

	if (!class_respondsToSelector(object_getClass(filter), sel_registerName("type"))) {
		say("hakios: layers%s: %*s  %s %s\n", layers_of, 2 * depth, "", what, text_of(filter));     /* a name alone */
		return;
	}
	n = (size_t)snprintf(line, sizeof(line), "%s %s \"%s\"%s", what, text_of(SEND(id, filter, "type")),
	    text_of(SEND(id, filter, "name")), SEND(BOOL, filter, "isEnabled") ? "" : " (disabled)");
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]) && n < sizeof(line); i++) {
		id key = SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, inputs[i]);
		id value = SEND1(id, filter, "valueForKey:", id, key);
		float m[20];

		if (value == NULL) {
			continue;
		}
		if (i == 0 && class_respondsToSelector(object_getClass(value), sel_registerName("CAColorMatrixValue"))) {
			SEND1(void, value, "getValue:", void *, m);
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s [%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g | "
			    "%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g]", inputs[i], m[0], m[1], m[2], m[3], m[4], m[5], m[6],
			    m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19]);
		} else {
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s %s", inputs[i], text_of(value));
		}
	}
	say("hakios: layers%s: %*s  %s\n", layers_of, 2 * depth, "", line);
}

static void
layer_line(id layer, const char *role, int depth)
{
	struct { double x, y, w, h; } frame = SEND(typeof(frame), layer, "frame");
	const char *class_name = object_getClassName(layer);
	id contents = SEND(id, layer, "contents"), filters = SEND(id, layer, "filters"), mask = SEND(id, layer, "mask");
	id compositing = SEND(id, layer, "compositingFilter"), sublayers = SEND(id, layer, "sublayers");
	id background = SEND(id, layer, "backgroundFilters");
	void *colour = SEND(void *, layer, "backgroundColor");
	char more[1024] = "";
	size_t n = 0;

	if (SEND(BOOL, layer, "isHidden")) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HIDDEN");
	}
	if (contents != NULL) {
		id text = (id)CFCopyDescription((CFTypeRef)contents);
		const char *from = text_of(text);
		char flat[300];
		size_t k = 0;

		/*
		 * What a CGImage says about itself runs over several lines -- its colour space, how big
		 * it is, how its pixels are laid out -- and all of that is worth having on the one line,
		 * because the size and the colour space are the first things to look at when a picture
		 * comes out wrong. Every run of spaces and newlines becomes one space.
		 */
		while (from != NULL && *from != 0 && k + 1 < sizeof(flat)) {
			if (*from == ' ' || *from == '\n' || *from == '\t' || *from == '\r') {
				if (k > 0 && flat[k - 1] != ' ') {
					flat[k++] = ' ';
				}
				from++;
				continue;
			}
			flat[k++] = *from++;
		}
		flat[k] = 0;
		n += (size_t)snprintf(more + n, sizeof(more) - n, " contents %s", flat);
		CFRelease((CFTypeRef)text);
	}
	if (contents != NULL && class_respondsToSelector(object_getClass(layer), sel_registerName("contentsMultiplyColor")) &&
	    SEND(void *, layer, "contentsMultiplyColor") != NULL) {
		id text = (id)CFCopyDescription(SEND(void *, layer, "contentsMultiplyColor"));
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " times %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (colour != NULL) {
		id text = (id)CFCopyDescription(colour);
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " colour %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (strcmp(class_name, "CAPortalLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " PORTAL of %p%s%s", (void *)SEND(id, layer, "sourceLayer"),
		    SEND(BOOL, layer, "hidesSourceLayer") ? " (hides it)" : "", SEND(BOOL, layer, "matchesOpacity") ? " (its opacity)" : "");
	} else if (strcmp(class_name, "CABackdropLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " BACKDROP group \"%s\" scale %g%s", text_of(SEND(id, layer, "groupName")),
		    SEND(double, layer, "scale"), SEND(BOOL, layer, "captureOnly") ? " (capture only)" : "");
	} else if (strcmp(class_name, "CAGradientLayer") == 0) {
		struct { double x, y; } from = SEND(typeof(from), layer, "startPoint"), to = SEND(typeof(to), layer, "endPoint");
		id colours = SEND(id, layer, "colors");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " GRADIENT %s from %g,%g to %g,%g at %.40s:", text_of(SEND(id, layer, "type")),
		    from.x, from.y, to.x, to.y, text_of(SEND(id, layer, "locations")));
		for (long i = 0; colours != NULL && i < SEND(long, colours, "count") && n < sizeof(more); i++) {
			id text = (id)CFCopyDescription((CFTypeRef)SEND1(id, colours, "objectAtIndex:", long, i));
			const char *numbers = strstr(text_of(text), "( ");

			n += (size_t)snprintf(more + n, sizeof(more) - n, " %.30s", numbers ? numbers : "?");
			CFRelease((CFTypeRef)text);
		}
	} else if (strcmp(class_name, "CAShapeLayer") == 0) {
		void *stroke = SEND(void *, layer, "strokeColor"), *fill = SEND(void *, layer, "fillColor");
		id text_stroke = stroke ? (id)CFCopyDescription(stroke) : NULL, text_fill = fill ? (id)CFCopyDescription(fill) : NULL;
		const char *ns = text_stroke ? strstr(text_of(text_stroke), "( ") : NULL, *nf = text_fill ? strstr(text_of(text_fill), "( ") : NULL;

		n += (size_t)snprintf(more + n, sizeof(more) - n, " SHAPE line %g stroke %.30s fill %.30s ends %g-%g", SEND(double, layer,
		    "lineWidth"), ns ? ns : "-", nf ? nf : "-", SEND(double, layer, "strokeStart"), SEND(double, layer, "strokeEnd"));
		if (text_stroke != NULL) {
			CFRelease((CFTypeRef)text_stroke);
		}
		if (text_fill != NULL) {
			CFRelease((CFTypeRef)text_fill);
		}
	} else if (strcmp(class_name, "CALayerHost") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HOST of context %#x%s%s", SEND(unsigned, layer, "contextId"),
		    SEND(BOOL, layer, "inheritsSecurity") ? " (inherits security)" : "",
		    SEND(BOOL, layer, "rendersAsynchronously") ? " (asynchronously)" : "");
	}
	{
		/* the view a layer belongs to, which is the only thing that names a layer UIKit left
		   unnamed -- and the switcher's wash of wallpaper is such a layer */
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " of %s", object_getClassName(delegate));
		}
	}
	{
		id keys = SEND(id, layer, "animationKeys");

		if (SEND(BOOL, layer, "shouldRasterize")) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " RASTERIZED");
		}
		for (long i = 0; keys != NULL && i < SEND(long, keys, "count") && n < sizeof(more); i++) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, "%s%s", i == 0 ? " ANIMATIONS " : ",",
			    text_of(SEND1(id, keys, "objectAtIndex:", long, i)));
		}
	}
	say("hakios: layers%s: %*s%s%s %p \"%s\" %g,%g %gx%g opacity %g%s\n", layers_of, 2 * depth, "", role, class_name, (void *)layer,
	    text_of(SEND(id, layer, "name")), frame.x, frame.y, frame.w, frame.h, (double)SEND(float, layer, "opacity"), more);
	for (long i = 0; filters != NULL && i < SEND(long, filters, "count"); i++) {
		layer_filter(SEND1(id, filters, "objectAtIndex:", long, i), "filter", depth);
	}
	for (long i = 0; background != NULL && i < SEND(long, background, "count"); i++) {
		layer_filter(SEND1(id, background, "objectAtIndex:", long, i), "background filter", depth);
	}
	if (compositing != NULL) {
		layer_filter(compositing, "compositing filter", depth);
	}
	if (mask != NULL) {
		layer_line(mask, "MASK ", depth + 1);
	}
	for (long i = 0; sublayers != NULL && i < SEND(long, sublayers, "count"); i++) {
		layer_line(SEND1(id, sublayers, "objectAtIndex:", long, i), "", depth + 1);
	}
}

/* Every WKWebView under a view: whether WebKit takes it for one in the background, its web process and whether that answers. */
static void
web_views(id view, int depth)
{
	Class web = objc_getClass("WKWebView");
	id subviews;
	bool is_web = false;

	if (view == NULL || depth > 40) {
		return;
	}

	for (Class c = object_getClass(view); web != NULL && c != NULL && !is_web; c = class_getSuperclass(c)) {
		is_web = c == web;
	}
	if (is_web) {
		SEL background = sel_registerName("_isBackground"), pid = sel_registerName("_webProcessIdentifier");
		SEL responsive = sel_registerName("_webProcessIsResponsive");

		say("hakios: layers%s: a WKWebView %p: in the background %s, web process %d, responsive %s, in a window %s, hidden %s\n",
		    layers_of, (void *)view, class_respondsToSelector(object_getClass(view), background) ?
		    (SEND(BOOL, view, "_isBackground") ? "YES" : "no") : "?", class_respondsToSelector(object_getClass(view), pid) ?
		    SEND(int, view, "_webProcessIdentifier") : -1, class_respondsToSelector(object_getClass(view), responsive) ?
		    (SEND(BOOL, view, "_webProcessIsResponsive") ? "yes" : "NO") : "?", SEND(id, view, "window") ? "yes" : "NO",
		    SEND(BOOL, view, "isHidden") ? "YES" : "no");
	}
	subviews = SEND(id, view, "subviews");
	for (long i = 0; subviews != NULL && i < SEND(long, subviews, "count"); i++) {
		web_views(SEND1(id, subviews, "objectAtIndex:", long, i), depth + 1);
	}
}

/*
 * The CoreAnimation contexts of this process that no window above has shown, each with its layer tree.
 * A program that is not a UIKit application has no UIWindow at all, and its layers still reach the
 * screen through a context of its own, hosted by another process (a CALayerHost in SpringBoard's tree,
 * "HOST of context 0x..."). chronod is one: the home screen's widgets are drawn in its context
 * (backboardd's touch statistics name "chronod.N contextID ..." as a destination), so what a blank
 * widget holds -- no layer, an empty one, a picture -- is only seen from inside chronod.
 * +[CAContext allContexts] and -[CAContext layer] are asked only where they answer.
 */
static void
contexts(const unsigned *shown, long count)
{
	Class context_class = objc_getClass("CAContext");
	id all = context_class != NULL && class_getClassMethod(context_class, sel_registerName("allContexts")) != NULL ?
	    SEND(id, context_class, "allContexts") : NULL;
	long total = all != NULL ? SEND(long, all, "count") : -1;

	say("hakios: layers%s: %ld CoreAnimation contexts in all\n", layers_of, total);
	for (long i = 0; i < total; i++) {
		id context = SEND1(id, all, "objectAtIndex:", long, i);
		unsigned id_of = class_respondsToSelector(object_getClass(context), sel_registerName("contextId")) ?
		    SEND(unsigned, context, "contextId") : 0;
		id layer = class_respondsToSelector(object_getClass(context), sel_registerName("layer")) ? SEND(id, context, "layer") : NULL;
		bool seen = false;

		for (long k = 0; k < count && !seen; k++) {
			seen = shown[k] == id_of && id_of != 0;
		}
		if (seen) {
			continue;               /* its window's tree is above */
		}
		say("hakios: layers%s: context %#x (%ssecure, %u commits), no window: %s\n", layers_of, id_of,
		    class_respondsToSelector(object_getClass(context), sel_registerName("isSecure")) && SEND(BOOL, context, "isSecure") ?
		    "" : "not ", class_respondsToSelector(object_getClass(context), sel_registerName("commitId")) ?
		    (unsigned)SEND(unsigned long long, context, "commitId") : 0, layer != NULL ? "its layers" : "NO LAYER");
		if (layer != NULL) {
			layer_line(layer, "", 1);
		}
	}
}

/*
 * Which of the libraries that draw a SwiftUI view are in this process, and whether Metal gives it a
 * device. RenderBox draws with Metal where there is a device; with none (every process here but
 * metal_test, without boot-arg metal=2) it has to take another way or draw nothing, and this says
 * which situation a process is in. Metal is asked only when it is already loaded: this must not be
 * what brings it in.
 */
static void
renderers(void)
{
	static const char *const names[] = { "/RenderBox", "/SwiftUI", "/ChronoKit", "/ChronoCore", "/WidgetKit", "/Metal" };
	char found[200] = "";
	size_t n = 0;
	void *metal;

	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *path = _dyld_get_image_name(i), *base = path != NULL ? strrchr(path, '/') : NULL;

		for (size_t k = 0; base != NULL && k < sizeof(names) / sizeof(names[0]) && n < sizeof(found); k++) {
			if (strcmp(base, names[k]) == 0) {
				n += (size_t)snprintf(found + n, sizeof(found) - n, " %s", base + 1);
			}
		}
	}
	say("hakios: layers%s: drawing libraries loaded:%s\n", layers_of, n ? found : " none of them");
	if ((metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOLOAD | RTLD_LAZY)) != NULL) {
		id (*create)(void) = (id (*)(void))dlsym(metal, "MTLCreateSystemDefaultDevice");
		id device = create != NULL ? create() : NULL;

		say("hakios: layers%s: MTLCreateSystemDefaultDevice() = %s\n", layers_of, device ? object_getClassName(device) : "nil");
	}
}

static void
layers(void)
{
	Class window = objc_getClass("UIWindow");
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	id windows = window && class_getClassMethod(window, all) ? ((id (*)(Class, SEL, BOOL, BOOL))objc_msgSend)(window, all, 1, 1) : NULL;
	id application = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id authentication = application && class_respondsToSelector(object_getClass(application),
	    sel_registerName("authenticationController")) ? SEND(id, application, "authenticationController") : NULL;

	unsigned shown[64];
	long shown_count = 0;

	say("hakios: layers%s: %ld windows\n", layers_of, windows ? SEND(long, windows, "count") : 0L);
	renderers();
	if (authentication != NULL) {
		say("hakios: layers: the user is %sauthenticated, %s passcode, the display %sin secure mode\n",
		    SEND(BOOL, authentication, "isAuthenticated") ? "" : "NOT ", SEND(BOOL, authentication, "hasPasscodeSet") ? "a" : "no",
		    SEND(BOOL, authentication, "inSecureDisplayMode") ? "" : "not ");
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		id w = SEND1(id, windows, "objectAtIndex:", long, i);
		id scene = class_respondsToSelector(object_getClass(w), sel_registerName("windowScene")) ? SEND(id, w, "windowScene") : NULL;
		id context = class_respondsToSelector(object_getClass(w), sel_registerName("_boundContext")) ? SEND(id, w, "_boundContext") : NULL;

		say("hakios: layers%s: window %s level %g, context %#x (%ssecure, %u commits), scene %s (activation state %ld)\n",
		    layers_of, object_getClassName(w), SEND(double, w, "windowLevel"), context ? SEND(unsigned, context, "contextId") : 0,
		    context && SEND(BOOL, context, "isSecure") ? "" : "not ", context ? (unsigned)SEND(unsigned long long, context, "commitId") : 0,
		    scene ? text_of(SEND(id, scene, "_persistenceIdentifier")) : "none", scene ? SEND(long, scene, "activationState") : -1L);
		if (context != NULL && shown_count < (long)(sizeof(shown) / sizeof(shown[0]))) {
			shown[shown_count++] = SEND(unsigned, context, "contextId");
		}
		layer_line(SEND(id, w, "layer"), "", 1);
	}
	contexts(shown, shown_count);
	if (application != NULL) {
		say("hakios: layers%s: the application's state %ld (0 active, 1 inactive, 2 in the background)\n", layers_of,
		    SEND(long, application, "applicationState"));
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		web_views(SEND1(id, windows, "objectAtIndex:", long, i), 0);
	}
	say("hakios: layers%s: end\n", layers_of);
}

/*
 * The switcher of running applications, on the notification com.hakios.switcher (rwtouch notify).
 * A drag up from the bottom edge is the way to it on a phone, but the touches a script puts in do
 * not reach Darwin's system gestures -- nothing comes of such a drag, not even going home -- and two
 * presses of the home button only go home (there is no home button on a phone of this shape, and
 * what is sent as one goes home and no more). SpringBoard opens it itself for anything that is
 * not a gesture (a hardware keyboard, accessibility): SBMainSwitcherControllerCoordinator, which
 * is one for the whole program, takes "activate the main switcher not interactively, from this
 * source". The source is an analytics label; zero is "none", which is what the ones that have no
 * gesture behind them pass.
 * As it stands this is not enough: with three applications running it answers that it took the
 * request (source 0, animated and not animated alike) and the screen does not change -- the
 * application in front stays, and SpringBoard says nothing more. So the switcher still cannot be
 * reached from a script; what is left to try is the transition with a window scene
 * (_activateSwitcherValidatorWithEventLabel:animated:windowScene:), which is what its own
 * gestures use.
 */
static void
switcher(void)
{
	Class coordinator = objc_getClass("SBMainSwitcherControllerCoordinator");
	SEL activate = sel_registerName("activateMainSwitcherNoninteractivelyWithSource:animated:");
	id shared = coordinator != NULL ? SEND(id, coordinator, "sharedInstance") : NULL;
	Method method = shared != NULL ? class_getInstanceMethod(object_getClass(shared), activate) : NULL;

	if (method == NULL) {
		say("hakios: the switcher: %s has no %s\n",
		    coordinator != NULL ? "SBMainSwitcherControllerCoordinator" : "nothing", sel_getName(activate));
		return;
	}
	say("hakios: the switcher: opening it (%s, %s)\n", object_getClassName(shared),
	    method_getTypeEncoding(method) != NULL ? method_getTypeEncoding(method) : "?");
	/*
	 * It answers whether it took the request. The source is an analytics label and not every one
	 * of them is allowed from every state, so they are tried in turn until one is taken.
	 */
	/*
	 * Not animated: the animated one is taken and then nothing comes of it -- the transition it
	 * starts is the gesture's to carry, and there is no gesture here.
	 */
	for (long source = 0; source < 10; source++) {
		BOOL taken = ((BOOL (*)(id, SEL, long, BOOL))objc_msgSend)(shared, activate, source, NO);

		say("hakios: the switcher: from source %ld: %s\n", source, taken ? "taken" : "refused");
		if (taken) {
			return;
		}
	}
}

/*
 * Control Center's glyphs whole from its first frames. Its modules and their parts fade in as it
 * comes down: opacity animations, mostly springs that take up to a second to settle at 1. While a
 * layer's opacity is below 1, the window server's OpenGL ES renderer draws the vibrant glyphs in it
 * from a flattened copy that lacks the strokes composited against what is behind them, and draws them
 * whole only at full opacity: the strokes of the media controls, orientation lock, screen mirroring
 * and Focus came in only when the springs had settled, at the end of the presentation. (On an iPhone
 * they are whole all along.) So in Control Center's window an opacity animation is replaced with an
 * eased-out fade of cc_fade=MS between the same values: the modules still fade in and out, and are at
 * full opacity, glyphs whole, after a few frames. Off by default (Darwin's own animations stay as they
 * are; the real fix belongs in the drawing, not in the animations): a workaround, for checks only.
 */
#define CC_FADE_MS      0
static void (*original_add_animation_cc)(id self, SEL selector, id animation, id key);
static double cc_fade_s = CC_FADE_MS / 1000.0;
static unsigned cc_fades;

static bool
in_control_center(id layer)
{
	for (int depth = 0; layer != NULL && depth < 64; depth++) {
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL && strcmp(object_getClassName(delegate), "SBControlCenterWindow") == 0) {
			return true;
		}
		layer = SEND(id, layer, "superlayer");
	}
	return false;
}

static void
add_animation_cc(id self, SEL selector, id animation, id key)
{
	Class basic = objc_getClass("CABasicAnimation");
	id path = animation != NULL && class_respondsToSelector(object_getClass(animation), sel_registerName("keyPath")) ?
	    SEND(id, animation, "keyPath") : NULL;

	if (path != NULL && strcmp(text_of(path), "opacity") == 0 &&
	    class_respondsToSelector(object_getClass(animation), sel_registerName("fromValue")) && in_control_center(self)) {
		id fade = SEND1(id, basic, "animationWithKeyPath:", id, path);
		id ease = SEND1(id, objc_getClass("CAMediaTimingFunction"), "functionWithName:", id,
		    SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, "easeOut"));

		SEND1(void, fade, "setFromValue:", id, SEND(id, animation, "fromValue"));
		SEND1(void, fade, "setToValue:", id, SEND(id, animation, "toValue"));
		SEND1(void, fade, "setByValue:", id, SEND(id, animation, "byValue"));
		SEND1(void, fade, "setAdditive:", BOOL, SEND(BOOL, animation, "isAdditive"));
		SEND1(void, fade, "setBeginTime:", double, SEND(double, animation, "beginTime"));
		SEND1(void, fade, "setFillMode:", id, SEND(id, animation, "fillMode"));
		SEND1(void, fade, "setRemovedOnCompletion:", BOOL, SEND(BOOL, animation, "isRemovedOnCompletion"));
		SEND1(void, fade, "setDelegate:", id, SEND(id, animation, "delegate"));
		SEND1(void, fade, "setDuration:", double, cc_fade_s);
		SEND1(void, fade, "setTimingFunction:", id, ease);
		if (cc_fades++ < 3) {
			say("hakios: Control Center: an opacity animation of %.3f s (%s) made a fade of %.3f s\n",
			    SEND(double, animation, "duration"), object_getClassName(animation), cc_fade_s);
		}
		animation = fade;
	}
	original_add_animation_cc(self, selector, animation, key);
}

static void
control_center_fades(const char *arguments)
{
	const char *setting = strstr(arguments, "cc_fade=");
	Method add = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("addAnimation:forKey:"));

	if (setting != NULL) {
		cc_fade_s = atof(setting + 8) / 1000.0;
	}
	if (cc_fade_s <= 0 || add == NULL) {
		return;
	}
	original_add_animation_cc = (void (*)(id, SEL, id, id))method_getImplementation(add);
	method_setImplementation(add, (IMP)add_animation_cc);
}

/*
 * Open the tweak loader of the package manager's prefix, when there is one. Nothing here depends on
 * it: without a prefix (or without ElleKit in it) the file is not there and this does nothing.
 */
#define HAKIOS_TWEAK_LOADER    "/redwood/lib/libhakios.tweaks.dylib"

static void
load_tweaks(void)
{
	struct stat st;

	if (stat("/var/jb/usr/lib/TweakInject", &st) != 0) {
		return;                 /* no package manager's prefix here, so no tweaks */
	}
	if (dlopen(HAKIOS_TWEAK_LOADER, RTLD_NOW | RTLD_GLOBAL) == NULL) {
		say("hakios: the tweak loader %s: %s\n", HAKIOS_TWEAK_LOADER, dlerror());
	} else {
		say("hakios: tweaks: %s opened\n", HAKIOS_TWEAK_LOADER);
	}
}

/*
 * Boot-arg caenv=NAME=VALUE: CoreAnimation's switches here as in the window server (inject.c), set before QuartzCore
 * reads them. The ones that hold the frame rate back (CA_MINIMUM_FRAME_DURATION and its kind) are read by the
 * process that animates as well as by the one that composes, and SpringBoard animates the home screen.
 */
static void
caenv(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	for (const char *argument = arguments; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
		char setting[128];
		size_t length = strcspn(argument + 6, " ");
		char *equals;

		if ((argument != arguments && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
			continue;
		}
		memcpy(setting, argument + 6, length);
		setting[length] = 0;
		if ((equals = strchr(setting, '=')) != NULL) {
			*equals = 0;
			/* a fraction is written 0p25: the boot-arguments lose what follows a "." of a number (0.008 came as 0) */
			for (char *point = equals + 1; *point != 0; point++) {
				if (*point == 'p' && point > equals + 1 && point[-1] >= '0' && point[-1] <= '9' && point[1] >= '0' && point[1] <= '9') {
					*point = '.';
					break;
				}
			}
			setenv(setting, equals + 1, 1);
			say("hakios: caenv: %s=%s for SpringBoard\n", setting, equals + 1);
		}
	}
}

/*
 * Boot-arg brightness_bridge=1: the brightness the user sets (Control Center's slider, Settings) is
 * taken to the panel. CoreBrightness in backboardd keeps the level but never reaches a display
 * here (no AppleARMBacklight it knows, no ambient light sensor), and never even takes the level
 * the slider sets: BKSDisplayBrightnessGetCurrent() stays at 0.5 whatever the user does. So the
 * level is caught where it is set, in SpringBoard, which runs Control Center:
 *   -[CCUIDisplayModuleViewController _setBacklightLevel:]   Control Center's slider (DisplayModule.bundle,
 *                                                            loaded when Control Center is first opened)
 *   -[SBBrightnessController setBrightnessLevel:] and _setBrightnessLevel:showHUD:   SpringBoard's own
 *   -[BrightnessSystemClient setProperty:forKey:]           CoreBrightness, key DisplayBrightness
 *   BKSDisplayBrightnessSet(float, NSInteger)               BackBoardServices; its calls are rebound only in
 *                                                            the libraries outside the shared cache (inside it
 *                                                            one library branches straight into the other)
 * Each passes the level on and then does what it did. Control Center's slider would then jump
 * back to 0.5 when it is opened again (it asks -[... _backlightLevel], which asks
 * BKSDisplayBrightnessGetCurrent), so once the user has set a level, that is what it is told.
 * The old way stays beside them: the level is asked for four times a second --
 * BKSDisplayBrightnessGetCurrent() of BackBoardServices, or failing that CoreBrightness's own
 * BrightnessSystemClient (property DisplayBrightness) -- and taken when it changes (only then, so
 * a level that stays at 0.5 does not undo the user's).
 * A level goes to the panel driver's sysctl kern.hakios.brightness (0-255; the Galaxy S9's
 * hakios.s6e3ha8.c) on a square curve with a floor, so a slider at zero is dim and never black:
 *     level = 8 + 247 * b * b      (b = 0.0 ... 1.0)
 * brightness_bridge=2 also takes the panel to 0 (its lowest level, still lit) while SpringBoard
 * says the screen is blanked (notification state com.apple.springboard.hasBlankedScreen), and back
 * to the user's level as soon as it is not. Which hooks are in is said once; a hook's first call is
 * said, and a change of level at most once a second, with where it came from ("hakios:
 * brightness: ..."). Whatever is not found is said once and only that part is left out; nothing
 * here can stop SpringBoard.
 */
#define BRIGHTNESS_FLOOR 8

static float (*bks_brightness_get)(void);
static void (*bks_brightness_set)(float level, long unknown);
static id brightness_client;            /* a BrightnessSystemClient, when BackBoardServices has not the function */
static dispatch_queue_t brightness_queue;
static int brightness_blanked_token = NOTIFY_TOKEN_INVALID;
/* on brightness_queue only: */
static int brightness_written = -2, brightness_said = -2, brightness_writes, brightness_calls;
static float brightness_value = -1, brightness_polled = -2;
static const char *brightness_source = "";
static uint64_t brightness_said_at;
/* what the user set through a hook (-1: nothing yet), for Control Center's slider when it asks */
static volatile float brightness_user = -1;

static bool
brightness_read(float *value)
{
	if (bks_brightness_get != NULL) {
		*value = bks_brightness_get();
		return true;
	}
	if (brightness_client != nil) {
		CFTypeRef property = ((CFTypeRef (*)(id, SEL, CFStringRef))objc_msgSend)(brightness_client,
		    sel_registerName("copyPropertyForKey:"), CFSTR("DisplayBrightness"));
		bool found = false;

		if (property != NULL && CFGetTypeID(property) == CFDictionaryGetTypeID()) {
			CFTypeRef inner = CFDictionaryGetValue((CFDictionaryRef)property, CFSTR("Brightness"));

			if (inner != NULL && CFGetTypeID(inner) == CFNumberGetTypeID()) {
				found = CFNumberGetValue((CFNumberRef)inner, kCFNumberFloatType, value);
			}
		} else if (property != NULL && CFGetTypeID(property) == CFNumberGetTypeID()) {
			found = CFNumberGetValue((CFNumberRef)property, kCFNumberFloatType, value);
		}
		if (property != NULL) {
			CFRelease(property);
		}
		return found;
	}
	return false;
}

static float
brightness_clamp(float value)
{
	return !(value >= 0.0f) ? 0.0f : value > 1.0f ? 1.0f : value;      /* (a NaN is 0) */
}

/* the level now (brightness_value, and the blanked screen) to the panel; on brightness_queue */
static void
brightness_push(void)
{
	float value = brightness_value;
	uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
	int level, blanked = 0;
	uint64_t state = 0;

	if (value < 0.0f) {
		return;                                 /* nothing yet: the panel keeps what it has */
	}
	if (brightness_blanked_token != NOTIFY_TOKEN_INVALID &&
	    notify_get_state(brightness_blanked_token, &state) == NOTIFY_STATUS_OK && state != 0) {
		blanked = 1;
	}
	level = blanked ? 0 : BRIGHTNESS_FLOOR + (int)((255 - BRIGHTNESS_FLOOR) * value * value + 0.5f);
	if (!blanked && level < BRIGHTNESS_FLOOR) {
		level = BRIGHTNESS_FLOOR;
	}
	if (level != brightness_written) {
		int error = sysctlbyname("kern.hakios.brightness", NULL, NULL, &level, sizeof(level)) == 0 ? 0 : errno;

		if (error != 0) {
			if (brightness_written != -3) {
				say("hakios: brightness: kern.hakios.brightness=%d refused (%s); is the panel driver on (panel=1)?\n",
				    level, strerror(error));
			}
			brightness_written = -3;
			return;
		}
		brightness_written = level;
		brightness_writes++;
	}
	if (brightness_written != brightness_said && now - brightness_said_at >= 1000000) {
		say("hakios: brightness: slider %.3f from %s%s -> kern.hakios.brightness=%d (%d writes, %d hook calls)\n",
		    value, brightness_source, blanked ? ", screen blanked" : "", brightness_written, brightness_writes,
		    brightness_calls);
		brightness_said = brightness_written;
		brightness_said_at = now;
		brightness_writes = 0;
		brightness_calls = 0;
	}
}

static void
brightness_tick(void)
{
	float value;

	if (brightness_read(&value)) {
		value = brightness_clamp(value);
		if (value != brightness_polled) {       /* (taken only when it changes: see above) */
			brightness_polled = value;
			brightness_value = value;
			brightness_source = bks_brightness_get ? "BKSDisplayBrightnessGetCurrent" : "BrightnessSystemClient";
		}
	}
	brightness_push();
}

/* a level the user set, caught by a hook (any thread): to the panel at once */
static void
brightness_from_hook(float value, const char *source, bool *said)
{
	if (brightness_queue == NULL) {
		return;
	}
	value = brightness_clamp(value);
	brightness_user = value;
	dispatch_async(brightness_queue, ^{
		if (!*said) {
			*said = true;
			say("hakios: brightness: %s called (%.3f): the first time\n", source, value);
		}
		brightness_calls++;
		brightness_value = value;
		brightness_source = source;
		brightness_push();
	});
}

/* -[CCUIDisplayModuleViewController _setBacklightLevel:], _backlightLevel and currentBrightness */
static void (*original_cc_set_level)(id self, SEL selector, float level);
static float (*original_cc_level)(id self, SEL selector);
static float (*original_cc_current)(id self, SEL selector);
static bool said_cc_set;

static void
cc_set_level(id self, SEL selector, float level)
{
	brightness_from_hook(level, "-[CCUIDisplayModuleViewController _setBacklightLevel:]", &said_cc_set);
	original_cc_set_level(self, selector, level);
}

static void brightness_unlock(id controller, const char *where);

/* (asked when the slider is made and whenever it is updated: the moment to make it operative again too) */
static float
cc_level(id self, SEL selector)
{
	float user = brightness_user;

	brightness_unlock(self, "_backlightLevel");
	return user >= 0.0f ? user : original_cc_level(self, selector);
}

static float
cc_current(id self, SEL selector)
{
	float user = brightness_user;

	return user >= 0.0f ? user : original_cc_current(self, selector);
}

/* -[SBBrightnessController setBrightnessLevel:] and -_setBrightnessLevel:showHUD: */
static void (*original_sb_set_level)(id self, SEL selector, float level);
static void (*original_sb_set_level_hud)(id self, SEL selector, float level, BOOL hud);
static bool said_sb_set, said_sb_set_hud;

static void
sb_set_level(id self, SEL selector, float level)
{
	brightness_from_hook(level, "-[SBBrightnessController setBrightnessLevel:]", &said_sb_set);
	original_sb_set_level(self, selector, level);
}

static void
sb_set_level_hud(id self, SEL selector, float level, BOOL hud)
{
	brightness_from_hook(level, "-[SBBrightnessController _setBrightnessLevel:showHUD:]", &said_sb_set_hud);
	original_sb_set_level_hud(self, selector, level, hud);
}

/* -[BrightnessSystemClient setProperty:forKey:], key DisplayBrightness: a number, or {Brightness: number} */
static BOOL (*original_bsc_set_property)(id self, SEL selector, id property, id key);
static bool said_bsc_set;

static BOOL
bsc_set_property(id self, SEL selector, id property, id key)
{
	float value;

	if (key != nil && CFGetTypeID((CFTypeRef)key) == CFStringGetTypeID() &&
	    CFStringCompare((CFStringRef)key, CFSTR("DisplayBrightness"), 0) == kCFCompareEqualTo && property != nil) {
		CFTypeRef number = (CFTypeRef)property;

		if (CFGetTypeID(number) == CFDictionaryGetTypeID()) {
			number = CFDictionaryGetValue((CFDictionaryRef)number, CFSTR("Brightness"));
		}
		if (number != NULL && CFGetTypeID(number) == CFNumberGetTypeID() &&
		    CFNumberGetValue((CFNumberRef)number, kCFNumberFloatType, &value)) {
			brightness_from_hook(value, "-[BrightnessSystemClient setProperty:forKey:] DisplayBrightness", &said_bsc_set);
		}
	}
	return original_bsc_set_property(self, selector, property, key);
}

/* BKSDisplayBrightnessSet(level, unknown), where a library outside the shared cache calls it */
static bool said_bks_set;

static void
bks_set(float level, long unknown)
{
	brightness_from_hook(level, "BKSDisplayBrightnessSet", &said_bks_set);
	bks_brightness_set(level, unknown);
}

/*
 * Replace -[CLASS SELECTOR] whatever its address, when its types are `types` (the return type and those of the
 * arguments after self and _cmd, as the runtime writes them: "vf" for -(void)x:(float)). Returns the original, or
 * NULL (and says so once) when the class or the method is not there or is not of that kind. With `own`, a method the
 * class only inherits (UIView's, UIControl's) is not replaced where it is defined but added to this class alone,
 * the original being the inherited one: so only this class's objects come through the hook.
 */
static IMP
brightness_hook_in(const char *class_name, const char *selector, const char *types, IMP replacement, bool own)
{
	Class cls = objc_getClass(class_name);
	SEL sel = sel_registerName(selector);
	Method method = cls == Nil ? NULL : class_getInstanceMethod(cls, sel);
	Class super_class = cls == Nil ? Nil : class_getSuperclass(cls);
	char kind[32];
	unsigned n;

	if (method == NULL) {
		say("hakios: brightness: -[%s %s] %s; not hooked\n", class_name, selector,
		    cls == Nil ? "(no such class)" : "is not there");
		return NULL;
	}
	method_getReturnType(method, kind, sizeof(kind));
	n = method_getNumberOfArguments(method);
	if (kind[0] != types[0] || n != strlen(types) + 1) {
		say("hakios: brightness: -[%s %s] is not of the kind expected ('%s', %u arguments); not hooked\n", class_name,
		    selector, kind, n);
		return NULL;
	}
	for (unsigned i = 2; i < n; i++) {
		method_getArgumentType(method, i, kind, sizeof(kind));
		if (kind[0] != types[i - 1]) {
			say("hakios: brightness: -[%s %s] takes '%s' where '%c' was expected; not hooked\n", class_name,
			    selector, kind, types[i - 1]);
			return NULL;
		}
	}
	if (own && super_class != Nil && class_getInstanceMethod(super_class, sel) == method) {
		IMP inherited = method_getImplementation(method);

		return class_addMethod(cls, sel, replacement, method_getTypeEncoding(method)) ? inherited : NULL;
	}
	return method_setImplementation(method, replacement);
}

static IMP
brightness_hook(const char *class_name, const char *selector, const char *types, IMP replacement)
{
	return brightness_hook_in(class_name, selector, types, replacement, false);
}

/*
 * iOS 16.5's Control Center slider, and why it did not move. Public class dumps of DisplayModule (iOS 17 and 18;
 * the 16 ones are not public, but 14 has the same view controller without the first three of these) show:
 *   -_updateBrightnessControlAvailability   [sliderView setInoperative:![self _canAdjustBrightness]], from
 *                                           -viewWillAppear: and whenever CoreBrightness says something changed
 *   -_canAdjustBrightness                   one call into BackBoardServices (backboardd: is the brightness
 *                                           controllable?), which here has nothing to control
 *   -_sliderEditingDidBegin:                 BKSDisplayBrightnessTransactionCreate()
 *   -_sliderPresentationValueDidChange:      (16+; 14's -_sliderValueDidChange:) -> -_setBacklightLevel: ONLY while
 *                                           that transaction is not NULL
 * An inoperative CCUIContinuousSliderView is drawn dimmed and does not follow the finger: the slider "doesn't go up
 * or down", and -_setBacklightLevel: is never called. So:
 *   - _canAdjustBrightness answers YES (what it would have said is logged once, with the function it calls);
 *   - after _updateBrightnessControlAvailability, -viewWillAppear: and _backlightLevel the slider is made operative
 *     (and enabled) again, and -[CCUIContinuousSliderView setInoperative:YES] is refused for the brightness slider
 *     (the one the module holds, or the one named "cc-brightness-slider");
 *   - the slider's value is read straight from it in each of the module's slider actions and in the slider's own
 *     -sendAction:to:forEvent:, so a level is taken even when the transaction is NULL and _setBacklightLevel: is
 *     never reached.
 * Each candidate's first call is logged ("hakios: brightness: ... first call ..."), and so are the CoreBrightness
 * properties SpringBoard asks for and registers for, so that one run on the phone shows the path 16.5 takes.
 */
static void *brightness_slider;                         /* the module's CCUIContinuousSliderView (compared only) */
static void (*original_slider_set_inoperative)(id self, SEL selector, BOOL inoperative);
static void (*original_slider_set_enabled)(id self, SEL selector, BOOL enabled);

/* the first call of something, said once; `flag` is that something's own */
#define BRIGHTNESS_FIRST(flag, ...) do { static bool flag; if (!flag) { flag = true; say(__VA_ARGS__); } } while (0)

static bool
brightness_responds(id object, const char *selector)
{
	return object != nil && class_respondsToSelector(object_getClass(object), sel_registerName(selector));
}

/* a float or double getter with no argument */
static bool
brightness_get_float(id object, const char *getter, float *value)
{
	SEL sel = sel_registerName(getter);
	Method method = object == nil ? NULL : class_getInstanceMethod(object_getClass(object), sel);
	char kind[8];

	if (method == NULL || method_getNumberOfArguments(method) != 2) {
		return false;
	}
	method_getReturnType(method, kind, sizeof(kind));
	if (kind[0] == 'f') {
		*value = ((float (*)(id, SEL))objc_msgSend)(object, sel);
	} else if (kind[0] == 'd') {
		*value = (float)((double (*)(id, SEL))objc_msgSend)(object, sel);
	} else {
		return false;
	}
	return true;
}

static bool
brightness_get_bool(id object, const char *getter, const char *other, BOOL *value)
{
	const char *name = brightness_responds(object, getter) ? getter : brightness_responds(object, other) ? other : NULL;

	if (name == NULL) {
		return false;
	}
	*value = SEND(BOOL, object, name);
	return true;
}

static bool
is_brightness_slider(id slider)
{
	id name;

	if (slider == nil) {
		return false;
	}
	if ((void *)slider == brightness_slider) {
		return true;
	}
	name = brightness_responds(slider, "accessibilityIdentifier") ? SEND(id, slider, "accessibilityIdentifier") : nil;
	return name != nil && CFGetTypeID((CFTypeRef)name) == CFStringGetTypeID() &&
	       CFStringCompare((CFStringRef)name, CFSTR("cc-brightness-slider"), 0) == kCFCompareEqualTo;
}

/* the module's slider, noted */
static id
brightness_slider_of(id controller)
{
	id slider = brightness_responds(controller, "sliderView") ? SEND(id, controller, "sliderView") : nil;

	if (slider != nil) {
		brightness_slider = (void *)slider;
	}
	return slider;
}

/* the module's slider made operative and enabled again, if it is not; said the first time each is needed */
static void
brightness_unlock(id controller, const char *where)
{
	id slider = brightness_slider_of(controller);
	BOOL state;

	if (slider == nil) {
		return;
	}
	if (brightness_get_bool(slider, "isInoperative", "inoperative", &state) && state) {
		if (original_slider_set_inoperative != NULL) {
			original_slider_set_inoperative(slider, sel_registerName("setInoperative:"), NO);
		} else {
			SEND1(void, slider, "setInoperative:", BOOL, NO);
		}
		BRIGHTNESS_FIRST(said_unlock, "hakios: brightness: the slider was inoperative (CoreBrightness: no brightness "
		    "control); made operative, after %s\n", where);
	}
	if (brightness_get_bool(slider, "isEnabled", "enabled", &state) && !state) {
		SEND1(void, slider, "setEnabled:", BOOL, YES);
		BRIGHTNESS_FIRST(said_enable, "hakios: brightness: the slider was disabled; enabled, after %s\n", where);
	}
}

/* the slider's value now (presentationValue: what is drawn while it is dragged; else value), to the panel */
static void
brightness_from_slider(id slider, const char *source, bool *said)
{
	float value;

	if (slider != nil && (brightness_get_float(slider, "presentationValue", &value) ||
	    brightness_get_float(slider, "value", &value))) {
		brightness_from_hook(value, source, said);
	}
}

/* Where the code at `imp` goes first: its first branch, through a stub if it is one, named by dladdr. */
static const char *
brightness_callee(IMP imp)
{
	const uint32_t *code = (const uint32_t *)(uintptr_t)imp;
	uintptr_t target = 0, x16 = 0;
	Dl_info info;

	for (int i = 0; i < 12 && code != NULL && target == 0; i++) {
		uint32_t insn = code[i];

		if ((insn & 0x7C000000u) == 0x14000000u) {              /* B / BL imm26 */
			int64_t offset = (int64_t)(int32_t)((insn & 0x03FFFFFFu) << 6) >> 4;   /* imm26 * 4, signed */

			target = (uintptr_t)&code[i] + (uintptr_t)offset;
		} else if ((insn & 0xFFFFFC1Fu) == 0xD61F0000u || (insn & 0xFFFFF800u) == 0xD71F0800u ||
		    (insn & 0xFFFFFBE0u) == 0xD61F0BE0u) {
			return "(an indirect branch first)";             /* BR / BRAA / BRAAZ: not followed */
		}
	}
	/* a stub: ADRP x16; ADD x16, x16, #imm or LDR x16, [x16, #imm]; BR x16 */
	for (int hops = 0; hops < 2 && target != 0; hops++) {
		const uint32_t *stub = (const uint32_t *)target;
		uint32_t adrp = stub[0], next = stub[1];

		if ((adrp & 0x9F00001Fu) != 0x90000010u) {
			break;
		}
		int64_t page = (int64_t)(int32_t)((((adrp >> 29) & 3u) | (((adrp >> 5) & 0x7FFFFu) << 2)) << 11) >> 11;

		x16 = (target & ~(uintptr_t)0xFFF) + (uintptr_t)(page << 12);
		if ((next & 0xFFC003FFu) == 0x91000210u) {                     /* ADD x16, x16, #imm12 */
			target = x16 + ((next >> 10) & 0xFFFu);
		} else if ((next & 0xFFC003FFu) == 0xF9400210u) {              /* LDR x16, [x16, #imm12 * 8] */
			target = *(const uintptr_t *)(x16 + ((next >> 10) & 0xFFFu) * 8) & 0x0000007FFFFFFFFFULL;
		} else {
			break;
		}
	}
	if (target != 0 && dladdr((const void *)target, &info) && info.dli_sname != NULL) {
		return info.dli_sname;
	}
	return target != 0 ? "(a function with no name)" : "(no branch in its first instructions)";
}

/* -[CCUIDisplayModuleViewController _canAdjustBrightness]: YES */
static BOOL (*original_cc_can_adjust)(id self, SEL selector);

static BOOL
cc_can_adjust(id self, SEL selector)
{
	static bool said;

	if (!said) {
		said = true;
		say("hakios: brightness: -[CCUIDisplayModuleViewController _canAdjustBrightness] first call: it would say %s "
		    "(it calls %s); YES is answered\n", original_cc_can_adjust(self, selector) ? "YES" : "NO",
		    brightness_callee((IMP)original_cc_can_adjust));
	}
	return YES;
}

/* -_updateBrightnessControlAvailability, -_updateBrightnessControlInteractionEnabled, -viewWillAppear: */
static void (*original_cc_update_availability)(id self, SEL selector);
static void (*original_cc_update_interaction)(id self, SEL selector);
static void (*original_cc_will_appear)(id self, SEL selector, BOOL animated);

static void
cc_update_availability(id self, SEL selector)
{
	BOOL inoperative = NO;

	brightness_slider_of(self);
	original_cc_update_availability(self, selector);
	(void)brightness_get_bool(brightness_slider_of(self), "isInoperative", "inoperative", &inoperative);
	BRIGHTNESS_FIRST(said, "hakios: brightness: -[CCUIDisplayModuleViewController _updateBrightnessControlAvailability] "
	    "first call (the slider %s after it)\n", inoperative ? "inoperative" : "operative");
	brightness_unlock(self, "_updateBrightnessControlAvailability");
}

static void
cc_update_interaction(id self, SEL selector)
{
	id slider;
	BOOL interactive = NO;

	original_cc_update_interaction(self, selector);
	slider = brightness_slider_of(self);
	interactive = slider != nil && SEND(BOOL, slider, "isUserInteractionEnabled");
	BRIGHTNESS_FIRST(said, "hakios: brightness: -[CCUIDisplayModuleViewController "
	    "_updateBrightnessControlInteractionEnabled] first call: the slider takes touches: %s\n", interactive ? "yes" :
	    "no");
}

static void
cc_will_appear(id self, SEL selector, BOOL animated)
{
	original_cc_will_appear(self, selector, animated);
	BRIGHTNESS_FIRST(said, "hakios: brightness: Control Center's display module appears (first time)\n");
	brightness_unlock(self, "viewWillAppear:");
}

/* the module's slider actions: 14's _sliderValueDidChange:, 16+'s _sliderPresentationValueDidChange:, editing */
static void (*original_cc_value_changed)(id self, SEL selector, id slider);
static void (*original_cc_presentation_changed)(id self, SEL selector, id slider);
static void (*original_cc_editing_began)(id self, SEL selector, id slider);
static void (*original_cc_editing_ended)(id self, SEL selector, id slider);
static bool said_cc_value, said_cc_presentation, said_cc_began, said_cc_ended;

static void
cc_value_changed(id self, SEL selector, id slider)
{
	brightness_from_slider(slider, "-[CCUIDisplayModuleViewController _sliderValueDidChange:]", &said_cc_value);
	original_cc_value_changed(self, selector, slider);
}

static void
cc_presentation_changed(id self, SEL selector, id slider)
{
	brightness_from_slider(slider, "-[CCUIDisplayModuleViewController _sliderPresentationValueDidChange:]",
	    &said_cc_presentation);
	original_cc_presentation_changed(self, selector, slider);
}

static void
cc_editing_began(id self, SEL selector, id slider)
{
	original_cc_editing_began(self, selector, slider);
	brightness_from_slider(slider, "-[CCUIDisplayModuleViewController _sliderEditingDidBegin:]", &said_cc_began);
}

static void
cc_editing_ended(id self, SEL selector, id slider)
{
	brightness_from_slider(slider, "-[CCUIDisplayModuleViewController _sliderEditingDidEnd:]", &said_cc_ended);
	original_cc_editing_ended(self, selector, slider);
}

/* CCUIContinuousSliderView: setInoperative:YES refused for the brightness slider; setEnabled:NO likewise */
static void
slider_set_inoperative(id self, SEL selector, BOOL inoperative)
{
	if (inoperative && is_brightness_slider(self)) {
		BRIGHTNESS_FIRST(said, "hakios: brightness: -[CCUIContinuousSliderView setInoperative:YES] for the "
		    "brightness slider (first call): refused, it stays operative\n");
		inoperative = NO;
	}
	original_slider_set_inoperative(self, selector, inoperative);
}

static void
slider_set_enabled(id self, SEL selector, BOOL enabled)
{
	if (!enabled && is_brightness_slider(self)) {
		BRIGHTNESS_FIRST(said, "hakios: brightness: -[CCUIContinuousSliderView setEnabled:NO] for the brightness "
		    "slider (first call): refused\n");
		enabled = YES;
	}
	original_slider_set_enabled(self, selector, enabled);
}

/* setUserInteractionEnabled: logged only (the module turns touches off while it is hidden, as it should) */
static void (*original_slider_set_interaction)(id self, SEL selector, BOOL enabled);

static void
slider_set_interaction(id self, SEL selector, BOOL enabled)
{
	if (!enabled && is_brightness_slider(self)) {
		BRIGHTNESS_FIRST(said, "hakios: brightness: -[CCUIContinuousSliderView setUserInteractionEnabled:NO] for "
		    "the brightness slider (first call; left as it is)\n");
	}
	original_slider_set_interaction(self, selector, enabled);
}

/* every action the brightness slider sends (UIControl -sendAction:to:forEvent:): the first few named, the value taken */
static void (*original_slider_send_action)(id self, SEL selector, SEL action, id target, id event);
static bool said_slider_action;

static void
slider_send_action(id self, SEL selector, SEL action, id target, id event)
{
	static SEL said[8];
	static unsigned n_said;

	if (is_brightness_slider(self)) {
		unsigned i;

		for (i = 0; i < n_said && said[i] != action; i++) {
		}
		if (i == n_said && n_said < sizeof(said) / sizeof(said[0])) {
			float value = -1;

			said[n_said++] = action;
			(void)brightness_get_float(self, "value", &value);
			say("hakios: brightness: the slider sends -[%s %s] (first time; value %.3f)\n",
			    target ? object_getClassName(target) : "(responder chain)", action ? sel_getName(action) : "?",
			    value);
		}
		brightness_from_slider(self, "-[CCUIContinuousSliderView sendAction:to:forEvent:]", &said_slider_action);
	}
	original_slider_send_action(self, selector, action, target, event);
}

/* -setValue:animated: of the slider, float or double: who sets it back, and to what (the first few, logged) */
static void (*original_slider_set_value_f)(id self, SEL selector, float value, BOOL animated);
static void (*original_slider_set_value_d)(id self, SEL selector, double value, BOOL animated);
static unsigned said_set_value;

static void
slider_set_value_f(id self, SEL selector, float value, BOOL animated)
{
	if (said_set_value < 4 && is_brightness_slider(self)) {
		say("hakios: brightness: the slider set to %.3f (setValue:animated:, %u of the first 4)\n", value,
		    ++said_set_value);
	}
	original_slider_set_value_f(self, selector, value, animated);
}

static void
slider_set_value_d(id self, SEL selector, double value, BOOL animated)
{
	if (said_set_value < 4 && is_brightness_slider(self)) {
		say("hakios: brightness: the slider set to %.3f (setValue:animated:, %u of the first 4)\n", value,
		    ++said_set_value);
	}
	original_slider_set_value_d(self, selector, value, animated);
}

/* CoreBrightness, what SpringBoard asks of it: each property the first time, with its answer, and what it watches */
static id (*original_bsc_copy_property)(id self, SEL selector, id key);
static void (*original_bsc_register)(id self, SEL selector, id block, id properties);

static id
bsc_copy_property(id self, SEL selector, id key)
{
	static CFStringRef said[24];
	static unsigned n_said;
	static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
	id result = original_bsc_copy_property(self, selector, key);
	bool first = false;

	if (key != nil && CFGetTypeID((CFTypeRef)key) == CFStringGetTypeID()) {
		os_unfair_lock_lock(&lock);
		unsigned i;

		for (i = 0; i < n_said && !CFEqual(said[i], (CFStringRef)key); i++) {
		}
		if (i == n_said && n_said < sizeof(said) / sizeof(said[0])) {
			said[n_said++] = CFStringCreateCopy(NULL, (CFStringRef)key);
			first = true;
		}
		os_unfair_lock_unlock(&lock);
	}
	if (first) {
		char text[160];

		strlcpy(text, text_of(result), sizeof(text));
		for (char *c = text; *c != 0; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
		say("hakios: brightness: CoreBrightness asked for %s (first time): %s\n", text_of(key), text);
	}
	return result;
}

static void
bsc_register(id self, SEL selector, id block, id properties)
{
	static unsigned said;

	if (said++ < 4) {
		char text[300];

		strlcpy(text, text_of(properties), sizeof(text));
		for (char *c = text; *c != 0; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
		say("hakios: brightness: CoreBrightness: notifications asked for %s\n", text);
	}
	original_bsc_register(self, selector, block, properties);
}

/* which of the names backboardd's "can the brightness be set" might have BackBoardServices exports (presence only) */
static void
brightness_say_bks_names(void)
{
	static const char *const names[] = {
		"BKSDisplayBrightnessIsBrightnessLevelControlAvailable", "BKSDisplayBrightnessIsBrightnessControlAvailable",
		"BKSDisplayBrightnessIsAvailable", "BKSDisplayBrightnessControlAvailable",
		"BKSDisplayBrightnessGetIsBrightnessControlAvailable", "BKSDisplayBrightnessTransactionCreate",
		"BKSDisplayBrightnessSetAutoBrightnessEnabled", "BKSDisplayBrightnessRestoreSystemBrightness",
	};
	char line[600];
	size_t n = 0;

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && n < sizeof(line); i++) {
		n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s %s", i ? ", " : "", names[i],
		    dlsym(RTLD_DEFAULT, names[i]) ? "yes" : "no");
	}
	say("hakios: brightness: BackBoardServices exports: %s\n", line);
}

static void
brightness_hook_control_center(void)
{
	static bool done;
	static const char *const slider_classes[] = { "CCUIContinuousSliderView", "CCUIModuleSliderView" };
	const char *slider_class = NULL;
	IMP original;

	if (done || objc_getClass("CCUIDisplayModuleViewController") == Nil) {
		return;
	}
	done = true;
	/* the slider's own class first, so the module's hooks below can use the original setters */
	for (size_t i = 0; i < sizeof(slider_classes) / sizeof(slider_classes[0]) && slider_class == NULL; i++) {
		slider_class = objc_getClass(slider_classes[i]) != Nil ? slider_classes[i] : NULL;
	}
	if (slider_class != NULL) {
		Method set_value = class_getInstanceMethod(objc_getClass(slider_class), sel_registerName("setValue:animated:"));
		char kind[8] = "";

		original_slider_set_inoperative = (void (*)(id, SEL, BOOL))brightness_hook(slider_class, "setInoperative:",
		    "vB", (IMP)slider_set_inoperative);
		original_slider_set_enabled = (void (*)(id, SEL, BOOL))brightness_hook_in(slider_class, "setEnabled:", "vB",
		    (IMP)slider_set_enabled, true);
		original_slider_set_interaction = (void (*)(id, SEL, BOOL))brightness_hook_in(slider_class,
		    "setUserInteractionEnabled:", "vB", (IMP)slider_set_interaction, true);
		original_slider_send_action = (void (*)(id, SEL, SEL, id, id))brightness_hook_in(slider_class,
		    "sendAction:to:forEvent:", "v:@@", (IMP)slider_send_action, true);
		if (set_value != NULL) {
			method_getArgumentType(set_value, 2, kind, sizeof(kind));
		}
		if (kind[0] == 'd') {
			original_slider_set_value_d = (void (*)(id, SEL, double, BOOL))brightness_hook_in(slider_class,
			    "setValue:animated:", "vdB", (IMP)slider_set_value_d, true);
		} else {
			original_slider_set_value_f = (void (*)(id, SEL, float, BOOL))brightness_hook_in(slider_class,
			    "setValue:animated:", "vfB", (IMP)slider_set_value_f, true);
		}
	} else {
		say("hakios: brightness: neither CCUIContinuousSliderView nor CCUIModuleSliderView is here: the slider's "
		    "own hooks left out\n");
	}

	if ((original = brightness_hook("CCUIDisplayModuleViewController", "_setBacklightLevel:", "vf", (IMP)cc_set_level))) {
		original_cc_set_level = (void (*)(id, SEL, float))original;
	}
	original_cc_level = (float (*)(id, SEL))brightness_hook("CCUIDisplayModuleViewController", "_backlightLevel", "f",
	    (IMP)cc_level);
	original_cc_current = (float (*)(id, SEL))brightness_hook("CCUIDisplayModuleViewController", "currentBrightness",
	    "f", (IMP)cc_current);
	original_cc_can_adjust = (BOOL (*)(id, SEL))brightness_hook("CCUIDisplayModuleViewController",
	    "_canAdjustBrightness", "B", (IMP)cc_can_adjust);
	original_cc_update_availability = (void (*)(id, SEL))brightness_hook("CCUIDisplayModuleViewController",
	    "_updateBrightnessControlAvailability", "v", (IMP)cc_update_availability);
	original_cc_update_interaction = (void (*)(id, SEL))brightness_hook("CCUIDisplayModuleViewController",
	    "_updateBrightnessControlInteractionEnabled", "v", (IMP)cc_update_interaction);
	original_cc_will_appear = (void (*)(id, SEL, BOOL))brightness_hook_in("CCUIDisplayModuleViewController",
	    "viewWillAppear:", "vB", (IMP)cc_will_appear, true);
	original_cc_value_changed = (void (*)(id, SEL, id))brightness_hook("CCUIDisplayModuleViewController",
	    "_sliderValueDidChange:", "v@", (IMP)cc_value_changed);
	original_cc_presentation_changed = (void (*)(id, SEL, id))brightness_hook("CCUIDisplayModuleViewController",
	    "_sliderPresentationValueDidChange:", "v@", (IMP)cc_presentation_changed);
	original_cc_editing_began = (void (*)(id, SEL, id))brightness_hook("CCUIDisplayModuleViewController",
	    "_sliderEditingDidBegin:", "v@", (IMP)cc_editing_began);
	original_cc_editing_ended = (void (*)(id, SEL, id))brightness_hook("CCUIDisplayModuleViewController",
	    "_sliderEditingDidEnd:", "v@", (IMP)cc_editing_ended);
	say("hakios: brightness: Control Center's slider hooked: _setBacklightLevel: %s, _backlightLevel %s, "
	    "currentBrightness %s, _canAdjustBrightness %s, _updateBrightnessControlAvailability %s, viewWillAppear: %s, "
	    "_sliderValueDidChange: %s, _sliderPresentationValueDidChange: %s, editing began/ended %s/%s; %s: "
	    "setInoperative: %s, setEnabled: %s, sendAction:to:forEvent: %s, setValue:animated: %s\n",
	    original_cc_set_level ? "yes" : "no", original_cc_level ? "yes" : "no", original_cc_current ? "yes" : "no",
	    original_cc_can_adjust ? "yes" : "no", original_cc_update_availability ? "yes" : "no",
	    original_cc_will_appear ? "yes" : "no", original_cc_value_changed ? "yes" : "no",
	    original_cc_presentation_changed ? "yes" : "no", original_cc_editing_began ? "yes" : "no",
	    original_cc_editing_ended ? "yes" : "no", slider_class ? slider_class : "no slider class",
	    original_slider_set_inoperative ? "yes" : "no", original_slider_set_enabled ? "yes" : "no",
	    original_slider_send_action ? "yes" : "no",
	    (original_slider_set_value_f || original_slider_set_value_d) ? "yes" : "no");
}

/* in one image outside the shared cache, its imports of BKSDisplayBrightnessSet pointed at bks_set */
static int
brightness_rebind(const struct mach_header_64 *header, intptr_t image_slide)
{
	const struct load_command *command = (const struct load_command *)(header + 1);
	const struct segment_command_64 *linkedit = NULL;
	const struct symtab_command *symtab = NULL;
	const struct dysymtab_command *dysymtab = NULL;
	int rebound = 0;

	if (header->magic != MH_MAGIC_64) {
		return 0;
	}
	for (uint32_t i = 0; i < header->ncmds; i++, command = (const void *)((const char *)command + command->cmdsize)) {
		if (command->cmd == LC_SEGMENT_64 &&
		    strcmp(((const struct segment_command_64 *)command)->segname, SEG_LINKEDIT) == 0) {
			linkedit = (const void *)command;
		} else if (command->cmd == LC_SYMTAB) {
			symtab = (const void *)command;
		} else if (command->cmd == LC_DYSYMTAB) {
			dysymtab = (const void *)command;
		}
	}
	if (linkedit == NULL || symtab == NULL || dysymtab == NULL || dysymtab->nindirectsyms == 0) {
		return 0;
	}
	uintptr_t base = (uintptr_t)image_slide + linkedit->vmaddr - linkedit->fileoff;
	const struct nlist_64 *symbols = (const void *)(base + symtab->symoff);
	const char *strings = (const char *)(base + symtab->stroff);
	const uint32_t *indirect = (const uint32_t *)(base + dysymtab->indirectsymoff);

	command = (const struct load_command *)(header + 1);
	for (uint32_t i = 0; i < header->ncmds; i++, command = (const void *)((const char *)command + command->cmdsize)) {
		const struct segment_command_64 *segment = (const void *)command;
		const struct section_64 *section = (const void *)(segment + 1);

		if (command->cmd != LC_SEGMENT_64 || (strcmp(segment->segname, SEG_DATA) != 0 &&
		    strcmp(segment->segname, "__DATA_CONST") != 0)) {
			continue;
		}
		for (uint32_t s = 0; s < segment->nsects; s++, section++) {
			uint32_t type = section->flags & SECTION_TYPE;
			void **pointers = (void **)((uintptr_t)image_slide + section->addr);

			if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) {
				continue;
			}
			for (uint64_t k = 0; k < section->size / sizeof(void *); k++) {
				uint32_t index = indirect[section->reserved1 + k];
				vm_address_t page;

				if ((index & (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) != 0 || index >= symtab->nsyms ||
				    strcmp(strings + symbols[index].n_un.n_strx, "_BKSDisplayBrightnessSet") != 0 ||
				    pointers[k] == (void *)bks_set) {
					continue;
				}
				page = (vm_address_t)&pointers[k] & ~(vm_address_t)(vm_page_size - 1);
				if (vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ | VM_PROT_WRITE |
				    VM_PROT_COPY) != KERN_SUCCESS) {
					continue;
				}
				pointers[k] = (void *)bks_set;
				rebound++;
				if (strcmp(segment->segname, "__DATA_CONST") == 0) {
					(void)vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ);
				}
			}
		}
	}
	return rebound;
}

/* every image, those there now and each loaded later: Control Center's module, and BKSDisplayBrightnessSet's callers */
static void
brightness_image_added(const struct mach_header *header, intptr_t image_slide)
{
	size_t length = 0;
	uintptr_t cache = (uintptr_t)_dyld_get_shared_cache_range(&length);
	Dl_info info;
	int rebound;

	if (bks_brightness_set != NULL && !(cache != 0 && (uintptr_t)header >= cache && (uintptr_t)header < cache + length) &&
	    (rebound = brightness_rebind((const struct mach_header_64 *)header, image_slide)) > 0) {
		say("hakios: brightness: BKSDisplayBrightnessSet rebound in %s (%d)\n",
		    dladdr(header, &info) && info.dli_fname ? info.dli_fname : "?", rebound);
	}
	if (dladdr(header, &info) && info.dli_fname != NULL && strstr(info.dli_fname, "/DisplayModule.bundle/") != NULL) {
		/*
		 * At once, here (as image_added() does for Siri's orb): Control Center makes the module's view controller
		 * right after the bundle loads, in the same turn of the main queue, so hooks put in from the queue
		 * would miss its first appearance. From the queue as well, in case its classes are not there yet.
		 */
		brightness_hook_control_center();
		dispatch_async(dispatch_get_main_queue(), ^{
			brightness_hook_control_center();
		});
	}
}

static void
brightness_bridge(const char *arguments)
{
	const char *arg = strstr(arguments, "brightness_bridge=");
	int mode;
	void *library;
	dispatch_source_t timer;
	IMP original;

	while (arg != NULL && arg != arguments && arg[-1] != ' ') {
		arg = strstr(arg + 1, "brightness_bridge=");
	}
	if (arg == NULL || (mode = atoi(arg + 18)) <= 0) {
		return;
	}
	library = dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_LAZY);
	bks_brightness_get = library ? (float (*)(void))dlsym(library, "BKSDisplayBrightnessGetCurrent") : NULL;
	bks_brightness_set = library ? (void (*)(float, long))dlsym(library, "BKSDisplayBrightnessSet") : NULL;
	dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness", RTLD_LAZY);
	if (bks_brightness_get == NULL) {
		Class cls = objc_getClass("BrightnessSystemClient");

		if (cls != Nil && class_getInstanceMethod(cls, sel_registerName("copyPropertyForKey:")) != NULL) {
			brightness_client = ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(cls,
			    sel_registerName("alloc")), sel_registerName("init"));
		}
	}
	if (mode >= 2 && notify_register_check("com.apple.springboard.hasBlankedScreen", &brightness_blanked_token) !=
	    NOTIFY_STATUS_OK) {
		brightness_blanked_token = NOTIFY_TOKEN_INVALID;
	}
	brightness_queue = dispatch_queue_create("com.hakios.brightness", DISPATCH_QUEUE_SERIAL);
	say("hakios: brightness: brightness_bridge=%d: the level set in SpringBoard, and %s asked four times a second, "
	    "to kern.hakios.brightness (now %d), floor %d%s\n", mode, bks_brightness_get ? "BKSDisplayBrightnessGetCurrent" :
	    brightness_client ? "BrightnessSystemClient" : "nothing (neither BKSDisplayBrightnessGetCurrent nor "
	    "BrightnessSystemClient is here)", sysctl_int("kern.hakios.brightness"), BRIGHTNESS_FLOOR,
	    brightness_blanked_token != NOTIFY_TOKEN_INVALID ? ", 0 while the screen is blanked" : "");

	/* the hooks: SpringBoard's own, CoreBrightness's, then Control Center's module whenever it is loaded */
	if ((original = brightness_hook("SBBrightnessController", "setBrightnessLevel:", "vf", (IMP)sb_set_level))) {
		original_sb_set_level = (void (*)(id, SEL, float))original;
	}
	if ((original = brightness_hook("SBBrightnessController", "_setBrightnessLevel:showHUD:", "vfB",
	    (IMP)sb_set_level_hud))) {
		original_sb_set_level_hud = (void (*)(id, SEL, float, BOOL))original;
	}
	if ((original = brightness_hook("BrightnessSystemClient", "setProperty:forKey:", "B@@", (IMP)bsc_set_property))) {
		original_bsc_set_property = (BOOL (*)(id, SEL, id, id))original;
	}
	if (bks_brightness_set == NULL) {
		say("hakios: brightness: BKSDisplayBrightnessSet is not there; not hooked\n");
	}
	brightness_say_bks_names();
	if ((original = brightness_hook("BrightnessSystemClient", "copyPropertyForKey:", "@@", (IMP)bsc_copy_property))) {
		original_bsc_copy_property = (id (*)(id, SEL, id))original;
	}
	if ((original = brightness_hook("BrightnessSystemClient", "registerNotificationBlock:forProperties:", "v@@",
	    (IMP)bsc_register))) {
		original_bsc_register = (void (*)(id, SEL, id, id))original;
	}
	if (objc_getClass("CCUIDisplayModuleViewController") == Nil) {
		say("hakios: brightness: Control Center's display module is not loaded yet; hooked when it is\n");
	}
	_dyld_register_func_for_add_image(brightness_image_added);
	dispatch_async(dispatch_get_main_queue(), ^{
		brightness_hook_control_center();
	});

	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, brightness_queue);
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 250 * NSEC_PER_MSEC,
	    50 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(timer, ^{
		brightness_tick();
	});
	dispatch_resume(timer);
}


/*
 * Boot-arg sb_hang=1: SpringBoard's main thread watched from inside. A thread of ours sends the main queue an empty
 * block every 250 ms; when one is not run within 1.5 s the main thread is stopped for a moment, its stack read by its
 * frame pointers (this process's own memory), and let go -- then, with nothing held, each address is named: the
 * image, its nearest exported symbol, and the nearest Objective-C method of that image. Again every 2 s while the
 * stall lasts (at most 6 times), and how long it lasted when the main queue answers.
 */
#include <pthread.h>

static mach_port_t hang_main_thread;
static volatile uint64_t hang_pong;

static uint64_t
hang_ms(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}

static bool
hang_read(uintptr_t address, void *out, size_t size)
{
	vm_size_t got = 0;

	return vm_read_overwrite(mach_task_self(), (vm_address_t)address, size, (vm_address_t)out, &got) == KERN_SUCCESS &&
	       got == size;
}

static unsigned
hang_stack(uintptr_t *frames, unsigned max)
{
	arm_thread_state64_t state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	unsigned n = 0;

	if (thread_suspend(hang_main_thread) != KERN_SUCCESS) {
		return 0;
	}
	if (thread_get_state(hang_main_thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS) {
		uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state);

		frames[n++] = (uintptr_t)arm_thread_state64_get_pc(state);
		frames[n++] = (uintptr_t)arm_thread_state64_get_lr(state);
		while (n < max && fp != 0 && (fp & 7) == 0) {
			uintptr_t pair[2];

			if (!hang_read(fp, pair, sizeof(pair)) || pair[1] == 0) {
				break;
			}
			frames[n++] = pair[1];
			if (pair[0] <= fp) {
				break;
			}
			fp = pair[0];
		}
	}
	(void)thread_resume(hang_main_thread);
	return n;
}

/* the nearest method of `image`'s classes at or below `address` */
static void
hang_objc_name(const char *image, uintptr_t address, char *out, size_t size)
{
	unsigned count = 0;
	const char **names = objc_copyClassNamesForImage(image, &count);
	uintptr_t best = 0;

	out[0] = 0;
	for (unsigned c = 0; names != NULL && c < count; c++) {
		Class cls = objc_getClass(names[c]);

		for (int meta = 0; cls != NULL && meta < 2; meta++) {
			Class k = meta ? object_getClass((id)cls) : cls;
			unsigned m = 0;
			Method *methods = class_copyMethodList(k, &m);

			for (unsigned i = 0; i < m; i++) {
				uintptr_t imp = (uintptr_t)method_getImplementation(methods[i]);

				if (imp <= address && imp > best && address - imp < 0x10000) {
					best = imp;
					snprintf(out, size, "%c[%s %s]+%#lx", meta ? '+' : '-', names[c], sel_getName(method_getName(methods[i])),
					    address - imp);
				}
			}
			free(methods);
		}
	}
	free(names);
}

static void
hang_say_stack(uintptr_t *frames, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		Dl_info info;
		char objc[256] = "";
		const char *image = "?", *symbol = "?";
		uintptr_t offset = 0;

		if (dladdr((void *)frames[i], &info) != 0) {
			image = info.dli_fname ? (strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname) : "?";
			if (info.dli_sname != NULL) {
				symbol = info.dli_sname;
				offset = frames[i] - (uintptr_t)info.dli_saddr;
			}
			if (info.dli_fname != NULL) {
				hang_objc_name(info.dli_fname, frames[i], objc, sizeof(objc));
			}
		}
		say("hakios: sb_hang:   %2u %#lx %s %s+%#lx %s\n", i, frames[i], image, symbol, offset, objc);
	}
}

static void *
hang_watch_thread(__unused void *arg)
{
	pthread_setname_np("hakios.sb_hang");
	for (;;) {
		uint64_t sent = hang_ms(), stalled_at = 0;
		unsigned samples = 0;

		hang_pong = 0;
		dispatch_async(dispatch_get_main_queue(), ^{
			hang_pong = hang_ms();
		});
		for (;;) {
			usleep(250000);
			if (hang_pong != 0) {
				break;
			}
			uint64_t now = hang_ms();

			if (now - sent >= 1500 && samples < 6 && (stalled_at == 0 || now - stalled_at >= 2000)) {
				uintptr_t frames[48];
				unsigned n = hang_stack(frames, 48);

				stalled_at = now;
				say("hakios: sb_hang: the main thread has not answered for %llu ms (sample %u, %u frames):\n",
				    now - sent, ++samples, n);
				hang_say_stack(frames, n);
			}
		}
		if (samples != 0) {
			say("hakios: sb_hang: the main thread answered after %llu ms\n", hang_pong - sent);
		}
		usleep(250000);
	}
	return NULL;
}

static void
hang_watch_start(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	pthread_t thread;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 || strstr(arguments, "sb_hang=1") == NULL) {
		return;
	}
	hang_main_thread = mach_thread_self();          /* (the constructor runs on the main thread) */
	if (pthread_create(&thread, NULL, hang_watch_thread, NULL) == 0) {
		pthread_detach(thread);
		say("hakios: sb_hang: SpringBoard's main thread watched (stalls over 1.5 s are said with its stack)\n");
	}
}


/*
 * Going home stalled SpringBoard for seconds (sb_hang=1 caught it, 9 November): at the start of the switcher's gesture
 * -[SBMainSwitcherControllerCoordinator switcherContentController:setCacheAsynchronousRenderingSurfaces:] asks the
 * render server in backboardd to cache the apps' asynchronous surfaces (CARenderServerSetCacheAsynchronousSurfaces,
 * from +[CATransaction flush]) and waits for its answer, which our window server gives seconds late. It only keeps
 * surfaces around for the animation: not asked at all here (sb_async_cache=1 asks as before).
 */
static void
no_async_surface_cache(__unused id self, __unused SEL selector, __unused id controller, __unused BOOL cache)
{
}

static void
async_surface_cache_off(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	Class cls = objc_getClass("SBMainSwitcherControllerCoordinator");
	Method method = cls ? class_getInstanceMethod(cls,
	    sel_registerName("switcherContentController:setCacheAsynchronousRenderingSurfaces:")) : NULL;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "sb_async_cache=1") != NULL) {
		return;
	}
	if (method == NULL) {
		say("hakios: -[SBMainSwitcherControllerCoordinator switcherContentController:setCacheAsynchronousRenderingSurfaces:] "
		    "not found; left alone\n");
		return;
	}
	method_setImplementation(method, (IMP)no_async_surface_cache);
	say("hakios: the switcher no longer asks the render server to cache asynchronous surfaces (it stalled going home)\n");
}

__attribute__((constructor))
static void
hakios_springboard_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	IMP activation, snapshot_format, battery_lvl, battery_st, poster_switcher;
	int token;

	if (name == NULL) {
		return;
	}
	if (strcmp(name, "SpringBoard") != 0) {
		static char who[64];

		snprintf(who, sizeof(who), " of %s", name);
		layers_of = who;
		if ((say_fd = open("/dev/console", O_WRONLY | O_NOCTTY)) < 0) {
			say_fd = 2;
		}
		say("hakios: in %s[%d]: its layers are said on the notification com.hakios.layers\n", name, getpid());
		notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
			layers();
		});
		return;
	}
	caenv();
	hang_watch_start();
	async_surface_cache_off();
	/*
	 * The tweaks of the package manager's prefix, if one is installed: ElleKit's loader
	 * (/var/jb/usr/lib/TweakLoader.dylib) reads /var/jb/usr/lib/TweakInject and opens the tweaks
	 * whose Filter names this process. The kernel could insert it as well (etc/inject.conf), but
	 * only one inserted library per program is reliable here, and SpringBoard's is this one; so
	 * this one opens it, the way a jailbreak's own hook does.
	 */
	load_tweaks();
	/*
	 * Everything the Metal driver says, it says with printf, and SpringBoard's standard output
	 * goes nowhere -- so inside this process every refusal our driver prints has been invisible.
	 * That is not a small blindness: the boot of 22 October showed SpringBoard dying in Siri's
	 * orb with MTLCreateSystemDefaultDevice() answering HakiosMetalDevice, which means our
	 * device was there and refused something, and the sentence naming what was written to a
	 * file descriptor that is closed. Pointing stdout at stderr, which IS the console here, is
	 * the whole fix and it costs nothing: the driver keeps its printf, and we get to read it.
	 */
	if (dup2(say_fd, 1) < 0) {
		say("hakios: stdout could not be pointed at the console; the driver's own lines stay lost\n");
	}
	/*
	 * And unbuffered, which is the half of this that is easy to leave out and would waste the
	 * boot it is meant to serve: stdio chose its buffering when stdout was still going nowhere,
	 * so it is fully buffered, and the process we are trying to hear from ABORTS -- the buffer
	 * would go with it, holding exactly the sentence we wanted.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	activation = replace_method("SBLockdownManager", "_setupActivationState", 0x1c4bc8dfcUL, (IMP)setup_activation_state);
	_dyld_register_func_for_add_image(image_added);         /* Siri's orb, whose framework comes late */
	original_set_setup_required_reason = (void (*)(id, SEL, long))replace_method("SBSetupManager",
	    "_setSetupRequiredReason:", 0x1c4cdd3a4UL, (IMP)set_setup_required_reason);
	snapshot_format = replace_method_of_kind('+', "XBApplicationSnapshotManifestImpl", "_outputFormatForSnapshot:",
	    0x1f1c91480UL, (IMP)snapshot_output_format);
	plain_vibrancy();
	original_lock_long_press = (void (*)(id, SEL, id))replace_method("SBLockHardwareButton", "longPress:", 0x1c4f6ab44UL,
	    (IMP)lock_long_press);
	poster_switcher = replace_method("SBMainDisplayPolicyAggregator", "_allowsCapabilityPosterSwitcherWithExplanation:",
	    0x1c507e0fcUL, (IMP)poster_switcher_allowed);
	original_switcher_wallpaper_overlay = (void (*)(id, SEL))replace_method("SBAppSwitcherPageView",
	    "_createOrDestroyWallpaperOverlayView", 0x1c4af7860UL, (IMP)switcher_wallpaper_overlay);
	/*
	 * Battery hooks on UIDevice: these are the methods every app and SpringBoard itself calls, so
	 * hooking them here in the SpringBoard process covers both the status bar and any app that
	 * reads [UIDevice currentDevice].batteryLevel. The addresses are for Darwin userland 16.5 (20F66).
	 */
	original_battery_level = (float (*)(id, SEL))replace_method("UIDevice", "batteryLevel", 0x1897021b0UL, (IMP)battery_level);
	original_battery_state = (long (*)(id, SEL))replace_method("UIDevice", "batteryState", 0x189209794UL, (IMP)battery_state);
	original_battery_view_shows_percentage = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "showsPercentage",
	    0x189a9370cUL, (IMP)battery_view_shows_percentage);
	original_battery_view_set_shows_percentage = (void (*)(id, SEL, BOOL))replace_method("_UIBatteryView", "setShowsPercentage:",
	    0x18926e150UL, (IMP)battery_view_set_shows_percentage);
	original_battery_view_should_show_bolt = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "_shouldShowBolt",
	    0x189203e28UL, (IMP)battery_view_should_show_bolt);
	original_battery_view_charge_percent = (double (*)(id, SEL))replace_method("_UIBatteryView", "chargePercent",
	    0x189a935d8UL, (IMP)battery_view_charge_percent);
	original_battery_view_set_charge_percent = (void (*)(id, SEL, double))replace_method("_UIBatteryView", "setChargePercent:",
	    0x18928d310UL, (IMP)battery_view_set_charge_percent);
	original_ring_charging_level = (double (*)(id, SEL))replace_method("CSBatteryChargingRingView", "batteryChargingLevel",
	    0x1c48c2cf4UL, (IMP)ring_charging_level);
	battery_lvl = (IMP)original_battery_level;
	battery_st  = (IMP)original_battery_state;
	say("hakios: SpringBoard: activation %s, setup assistant %s, application snapshots %s, the side button's long press %s, "
	    "battery level %s, battery state %s, poster switcher %s\n",
	    activation ? "answered here" : "NOT replaced", original_set_setup_required_reason ? "refused" : "NOT replaced",
	    snapshot_format ? "as JPEG" : "NOT changed", original_lock_long_press ? "checked" : "NOT checked",
	    battery_lvl ? "from QG" : "NOT replaced", battery_st ? "from QG" : "NOT replaced",
	    poster_switcher ? "not offered" : "NOT withheld");
	/*
	 * Cellular, a survey first (the modem's bars come from the kernel: sysctl kern.hakios.cellular_*,
	 * drivers/modem/hakios.mpss.c): where UIKit makes the status bar's cellular entry from the raw data,
	 * and with which signature -- said in the log with the unslid address, for the hook to come.
	 */
	{
		static const char *const classes[] = { "_UIStatusBarDataCellularEntry", "_UIStatusBarData", "_UIStatusBarDataEntry" };
		const char *selector = "_cellularEntryWithData:signalStrengthEnabled:serviceEnabled:dataNetworkEnabled:serviceString:"
		    "serviceCrossfadeString:serviceBadgeString:serviceContentType:dataNetworkType:gsmSignalStrengthRaw:"
		    "gsmSignalStrengthBars:callForwardingType:lowDataModeActive:";

		for (unsigned c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
			Class cls = objc_getClass(classes[c]);
			Method plus = cls ? class_getClassMethod(cls, sel_registerName(selector)) : NULL;
			Method minus = cls ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

			if (plus) {
				say("hakios: cellular: +[%s %s] at %#lx, types \"%s\"\n", classes[c], selector,
				    (uintptr_t)method_getImplementation(plus) - slide, method_getTypeEncoding(plus));
			}
			if (minus) {
				say("hakios: cellular: -[%s %s] at %#lx, types \"%s\"\n", classes[c], selector,
				    (uintptr_t)method_getImplementation(minus) - slide, method_getTypeEncoding(minus));
			}
			if (cls && !plus && !minus) {
				say("hakios: cellular: %s has no such method\n", classes[c]);
			}
		}
		say("hakios: cellular: the kernel says up=%d registered=%d bars=%d dbm=%d radio=%d\n", sysctl_int("kern.hakios.cellular_up"),
		    sysctl_int("kern.hakios.cellular_registered"), sysctl_int("kern.hakios.cellular_bars"),
		    sysctl_int("kern.hakios.cellular_dbm"), sysctl_int("kern.hakios.cellular_radio"));
	}
	cellular_hook();
	sb_fps_hooks();
	say("hakios: battery UI: shows%% %s, set-shows%% %s, bolt %s, chargePercent %s, setChargePercent %s, lockscreen ring %s\n",
	    original_battery_view_shows_percentage ? "ok" : "NO",
	    original_battery_view_set_shows_percentage ? "ok" : "NO",
	    original_battery_view_should_show_bolt ? "ok" : "NO",
	    original_battery_view_charge_percent ? "ok" : "NO",
	    original_battery_view_set_charge_percent ? "ok" : "NO",
	    original_ring_charging_level ? "ok" : "NO");
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *delay;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (delay = strstr(args, "sb_delay=")) != NULL &&
		    (delay == args || delay[-1] == ' ')) {
			usleep((useconds_t)(atof(delay + 9) * 1e6));
		}
	}
	/*
	 * UIDevice won't call batteryLevel/batteryState unless monitoring is enabled. Flip it on once
	 * SpringBoard's run loop is running (the dispatch_after fires on the main queue, which means
	 * we are past UIApplication startup).
	 */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		Class uidev = objc_getClass("UIDevice");
		SEL current = sel_registerName("currentDevice");
		SEL monitoring = sel_registerName("setBatteryMonitoringEnabled:");
		id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, current) : nil;

		if (device && class_getInstanceMethod(object_getClass(device), monitoring)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(device, monitoring, YES);
		}
		watch_main_loop();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
				control_center_fades(arguments);
			}
		}
		say("hakios: battery: SOC=%d%% battery=%d charger=%d charging=%d\n",
		    sysctl_int("kern.hakios.battery_soc"), sysctl_int("kern.hakios.battery_present"),
		    sysctl_int("kern.hakios.battery_charger_present"), sysctl_int("kern.hakios.battery_charging"));
		battery_post_notifications();
	});
	notify_register_dispatch("com.hakios.unlock", &token, dispatch_get_main_queue(), ^(__unused int t) {
		unlock();
	});
	notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
		layers();
	});
	notify_register_dispatch("com.hakios.switcher", &token, dispatch_get_main_queue(), ^(__unused int t) {
		switcher();
	});
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
			brightness_bridge(args);
		}
	}
	/*
	 * Belt-and-suspenders unlock. The user can always swipe up to open the home screen -- that
	 * works fine when the touch driver is doing its job. But if touch is still probing, has
	 * failed, or the panel is a FocalTech and the firmware upload is still chewing through
	 * ~114 KB of SPI (that takes a hot second during bring-up), the user ends up staring at a
	 * dark lock screen with nothing to swipe. So we just forcibly dismiss it after 10 seconds
	 * so Darwin always lands on the home screen even in the worst case. Harmless if touch is
	 * already working: the user just doesn't need to swipe themselves.
	 * With autounlock=0 among the kernel's arguments the lock screen stays until it is swiped away
	 * (to show the phone starting the way an iPhone does).
	 */
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *arg;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (arg = strstr(args, "autounlock=0")) != NULL &&
		    (arg == args || arg[-1] == ' ')) {
			say("hakios: the lock screen stays until it is swiped away (autounlock=0)\n");
			return;
		}
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		unlock();
	});
}
