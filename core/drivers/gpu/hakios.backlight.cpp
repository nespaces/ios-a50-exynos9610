/*
 * The panel's brightness, whichever phone: a panel driver that can set it registers here
 * (hakios_panel_register()), and what iOS asks for reaches it through hakios_panel_brightness(), 0-255. Today the
 * Galaxy S9's S6E3HA8 (drivers/gpu/hakios.s6e3ha8.c, panel=1) registers; no other phone's driver does, and there
 * hakios_panel_brightness() says ENODEV, as the stub it replaces did. Two ways in:
 *
 *   the display service (MiniIOMobileFramebuffer.cpp, patch 1500): a key named "brightness" set on IOMobileFramebuffer,
 *   as before. Every phone, nothing published for it.
 *
 *   AppleARMBacklight (boot-arg backlight=1 or 2; nothing without it): the class XNU itself knows as an ARM phone's
 *   backlight (IOPMrootDomain::idleSleepPreventersCount), published under the display service with the parameters of
 *   a display's brightness as IOGraphics names them (IOGraphicsTypes.h: "IODisplayParameters", whose "brightness" and
 *   "linear-brightness" are {min, max, value}, set by name and then "commit"). Which of these iOS 16's CoreBrightness
 *   looks for and sets is not known here: this service says in the log, process by process, every key it is set
 *   (with its value, a level down into a dictionary) and every property asked of it that it does not have, and who
 *   opens it. backlight=1 only says so; backlight=2 also takes "brightness" and "linear-brightness" (a number, or a
 *   dictionary with "value"), 0 to max, as the panel's brightness. Every key is accepted, so that nothing is retried
 *   or given up on because of a refusal. Without a panel driver (QEMU, the POCO) it says what it would have set.
 *
 * Off by default: the POCO and QEMU run as before unless backlight= is given.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <stdarg.h>

extern "C" void loader_log_line(const char * line);

/* IOLog does not reach the run's log (loader_log_line, what comes back to the host): both */
static void bl_say(const char * format, ...) __printflike(1, 2);
static void
bl_say(const char * format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	IOLog("%s", line);
}
#include <sys/errno.h>
#include <sys/proc.h>

extern "C" {
int hakios_panel_register(const char * name, int (*set)(int level));
int hakios_panel_brightness(int level);
int hakios_panel_register_power(int (*power)(int on));
int hakios_panel_power(int on);
void hakios_backlight_publish(IOService * display);
}

#define BACKLIGHT_MAX   65536                   /* the range "brightness" is published with: 0-65536 */
#define SAID_SETS       96                      /* how many keys set are said in the log */
#define SAID_ASKED      48                      /* ... and properties asked for and missing */

static const char * gPanelName;
static int (*gPanelSet)(int level);
static int gPanelLevel = -1;
static int (*gPanelPower)(int on);
static uint32_t gBacklightStep;                 /* backlight=: 0 none, 1 said only, 2 taken */

/* a panel that can be put to sleep and woken (iOS's selector 12 of IOMobileFramebuffer) */
int
hakios_panel_register_power(int (*power)(int on))
{
	gPanelPower = power;
	return 0;
}

int
hakios_panel_power(int on)
{
	return gPanelPower != NULL ? gPanelPower(on) : ENODEV;
}

/* a panel driver that can set its brightness, 0-255: once, at its start */
int
hakios_panel_register(const char * name, int (*set)(int level))
{
	if (set == NULL || gPanelSet != NULL) {
		return EBUSY;
	}
	gPanelName = name;
	gPanelSet = set;
	bl_say("hakios: panel brightness: %s registered\n", name);
	return 0;
}

/* the panel's brightness, 0-255: 0 if a panel driver took it, ENODEV where none is registered (every other phone) */
int
hakios_panel_brightness(int level)
{
	int error;

	if (gPanelSet == NULL) {
		return ENODEV;
	}
	level = level < 0 ? 0 : level > 255 ? 255 : level;
	error = gPanelSet(level);
	if (error == 0) {
		gPanelLevel = level;
	}
	return error;
}

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	using IOService::copyProperty;
	using IOService::newUserClient;

	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	virtual OSPtr<OSObject> copyProperty(const char * aKey) const APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
	    IOUserClient ** handler) APPLE_KEXT_OVERRIDE;

	void publishParameters(uint32_t value);

private:
	void take(const char * who, const char * key, OSObject * value);
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

/* IODisplayParameters: brightness and linear-brightness {min 0, max 65536, value}, and commit */
void
AppleARMBacklight::publishParameters(uint32_t value)
{
	OSDictionary * parameters = OSDictionary::withCapacity(3);
	const char * const names[] = { "brightness", "linear-brightness" };

	if (parameters == NULL) {
		return;
	}
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		OSDictionary * range = OSDictionary::withCapacity(3);
		OSNumber * min = OSNumber::withNumber(0ULL, 32), * max = OSNumber::withNumber(BACKLIGHT_MAX, 32);
		OSNumber * now = OSNumber::withNumber(value, 32);

		if (range != NULL && min != NULL && max != NULL && now != NULL) {
			range->setObject("min", min);
			range->setObject("max", max);
			range->setObject("value", now);
			parameters->setObject(names[i], range);
		}
		OSSafeReleaseNULL(min);
		OSSafeReleaseNULL(max);
		OSSafeReleaseNULL(now);
		OSSafeReleaseNULL(range);
	}
	OSDictionary * commit = OSDictionary::withCapacity(1);
	OSNumber * zero = OSNumber::withNumber(0ULL, 32);

	if (commit != NULL && zero != NULL) {
		commit->setObject("reg", zero);
		parameters->setObject("commit", commit);
	}
	OSSafeReleaseNULL(zero);
	OSSafeReleaseNULL(commit);
	setProperty("IODisplayParameters", parameters);
	parameters->release();
}

/* a key that is a brightness (backlight=2): a number, or a dictionary with "value", 0-65536 */
void
AppleARMBacklight::take(const char * who, const char * key, OSObject * value)
{
	OSDictionary * inside = OSDynamicCast(OSDictionary, value);
	OSNumber * number = OSDynamicCast(OSNumber, inside != NULL ? inside->getObject("value") : value);
	uint64_t raw;
	int level, error;

	if (number == NULL || (strcmp(key, "brightness") != 0 && strcmp(key, "linear-brightness") != 0)) {
		return;
	}
	raw = number->unsigned64BitValue();
	raw = raw > BACKLIGHT_MAX ? BACKLIGHT_MAX : raw;
	level = (int) ((raw * 255 + BACKLIGHT_MAX / 2) / BACKLIGHT_MAX);
	if (gBacklightStep < 2) {
		bl_say("AppleARMBacklight: (backlight=1: %s's %s %llu would be level %d; not set)\n", who, key, raw, level);
		return;
	}
	error = hakios_panel_brightness(level);
	if (error == 0) {
		publishParameters((uint32_t) raw);
	}
	bl_say("AppleARMBacklight: %s's %s %llu: level %d %s\n", who, key, raw, level,
	    error == 0 ? "set" : error == ENODEV ? "(no panel driver: not set)" : "NOT SET (the panel driver failed)");
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	static unsigned said;
	char who[32] = "?";

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSObject * value = key ? dict->getObject(key) : NULL;
		OSNumber * number = OSDynamicCast(OSNumber, value);
		OSDictionary * inside = OSDynamicCast(OSDictionary, value);

		if (key == NULL) {
			continue;
		}
		if (said < SAID_SETS) {
			said++;
			if (number != NULL) {
				bl_say("AppleARMBacklight: %s sets %s = %llu\n", who, key->getCStringNoCopy(), number->unsigned64BitValue());
			} else {
				bl_say("AppleARMBacklight: %s sets %s (%s)\n", who, key->getCStringNoCopy(),
				    value ? value->getMetaClass()->getClassName() : "?");
			}
			if (inside != NULL) {
				OSCollectionIterator * more = OSCollectionIterator::withCollection(inside);
				const OSSymbol * k;

				while (more != NULL && (k = OSDynamicCast(OSSymbol, more->getNextObject())) != NULL) {
					OSNumber * n = OSDynamicCast(OSNumber, inside->getObject(k));

					if (n != NULL) {
						bl_say("AppleARMBacklight:   %s.%s = %llu\n", key->getCStringNoCopy(), k->getCStringNoCopy(),
						    n->unsigned64BitValue());
					} else {
						bl_say("AppleARMBacklight:   %s.%s (%s)\n", key->getCStringNoCopy(), k->getCStringNoCopy(),
						    inside->getObject(k) ? inside->getObject(k)->getMetaClass()->getClassName() : "?");
					}
				}
				OSSafeReleaseNULL(more);
			}
		}
		take(who, key->getCStringNoCopy(), value);
		/* (IODisplayParameters set whole: its brightness taken as well) */
		if (inside != NULL && strcmp(key->getCStringNoCopy(), "IODisplayParameters") == 0) {
			take(who, "brightness", inside->getObject("brightness"));
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

/* what is asked of the service by name (IORegistryEntryCreateCFProperty, IOKit's matching): said when it is missing */
OSPtr<OSObject>
AppleARMBacklight::copyProperty(const char * aKey) const
{
	OSPtr<OSObject> object = IOService::copyProperty(aKey);
	static unsigned said;

	if (!object && aKey != NULL && said < SAID_ASKED) {
		char who[32] = "?";

		said++;
		proc_selfname(who, sizeof(who));
		bl_say("AppleARMBacklight: %s asks for %s: not there\n", who, aKey);
	}
	return object;
}

IOReturn
AppleARMBacklight::newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
    IOUserClient ** handler)
{
	static unsigned said;
	char who[32] = "?";

	if (said < 8) {
		said++;
		proc_selfname(who, sizeof(who));
		bl_say("AppleARMBacklight: %s opens it (type %u): no user client, refused\n", who, (unsigned) type);
	}
	return IOService::newUserClient(owningTask, securityID, type, properties, handler);
}

/* boot-arg backlight=1 or 2: the service, under the display service (IOMobileFramebuffer::start) */
void
hakios_backlight_publish(IOService * display)
{
	AppleARMBacklight * backlight;
	uint32_t value;

	if (!PE_parse_boot_argn("backlight", &gBacklightStep, sizeof(gBacklightStep)) || gBacklightStep == 0 ||
	    display == NULL) {
		gBacklightStep = 0;
		return;
	}
	backlight = OSTypeAlloc(AppleARMBacklight);
	if (backlight == NULL || !backlight->init()) {
		OSSafeReleaseNULL(backlight);
		bl_say("AppleARMBacklight: not made\n");
		return;
	}
	/* (until something is set, the panel's level if it was set at start, else the top of the range) */
	value = gPanelLevel >= 0 ? (uint32_t) ((gPanelLevel * BACKLIGHT_MAX + 127) / 255) : BACKLIGHT_MAX;
	backlight->publishParameters(value);
	if (!backlight->attach(display)) {
		backlight->release();
		return;
	}
	backlight->setName("backlight");
	if (!backlight->start(display)) {
		backlight->detach(display);
		backlight->release();
		return;
	}
	backlight->registerService();
	bl_say("AppleARMBacklight: published (backlight=%u: %s); panel driver: %s\n", gBacklightStep,
	    gBacklightStep >= 2 ? "brightness taken" : "keys only said", gPanelName != NULL ? gPanelName : "none (yet)");
	backlight->release();           /* (the registry holds it) */
}
