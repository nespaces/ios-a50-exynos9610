/* the brightness bridge: included by inject.c (backboardd) and inject_sb.c (SpringBoard); needs say() */
/* ---- the brightness to the panel (backboardd, SpringBoard) ------------------------------------------------------------ */

/*
 * backboardd works out the brightness (the slider, Control Center) and hands it to CoreBrightness as the
 * BrightnessSystem property "DisplayBrightness" ({ Brightness: 0..1, Commit }); CoreBrightness then finds no
 * backlight of its kind on this phone and nothing reaches the kernel. So the property is also taken here, and set
 * on the display service as "brightness" (a fraction: MiniIOMobileFramebuffer.cpp hands it to the panel driver,
 * hakios_panel_brightness()). The latest value is written from a queue of our own, so the caller never waits.
 */
static dispatch_queue_t brightness_queue;
/* the SDK marks these unavailable on iOS; IOKit has them all the same */
extern kern_return_t hakios_set_cf_property(io_registry_entry_t entry, CFStringRef key, CFTypeRef value)
    __asm__("_IORegistryEntrySetCFProperty");
static volatile float brightness_wanted = -1;
static io_service_t brightness_display;

static void
brightness_write(void)
{
	float level = brightness_wanted;
	CFNumberRef number;
	kern_return_t error;
	static unsigned said;

	if (level < 0) {
		return;
	}
	if (brightness_display == IO_OBJECT_NULL) {
		brightness_display = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceNameMatching("disp0"));
	}
	if (brightness_display == IO_OBJECT_NULL) {
		return;
	}
	/* as a whole number 2..255 (IOKit's serializer passes integers; 0 and 1 the display service takes as fractions) */
	int whole = (int)(level * 255 + 0.5f);

	whole = whole < 2 ? 2 : whole > 255 ? 255 : whole;
	number = CFNumberCreate(NULL, kCFNumberIntType, &whole);
	error = hakios_set_cf_property(brightness_display, CFSTR("brightness"), number);
	CFRelease(number);
	if (said < 5 || error != KERN_SUCCESS) {
		said++;
		say("hakios: brightness %.3f (%d of 255) to the display: %#x\n", level, whole, error);
	}
}

static void
brightness_take(CFTypeRef key, CFTypeRef value)
{
	CFNumberRef number = NULL;
	float level;

	if (key == NULL || CFGetTypeID(key) != CFStringGetTypeID() || !CFEqual(key, CFSTR("DisplayBrightness")) || value == NULL) {
		return;
	}
	if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
		number = CFDictionaryGetValue(value, CFSTR("Brightness"));
	} else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		number = value;
	}
	if (number == NULL || CFGetTypeID(number) != CFNumberGetTypeID() || !CFNumberGetValue(number, kCFNumberFloatType, &level)) {
		return;
	}
	brightness_wanted = level;
	dispatch_async(brightness_queue, ^{ brightness_write(); });
}

typedef intptr_t (*set_property_fn)(id, SEL, id, id);
static set_property_fn brightness_original[2];

static intptr_t
brightness_set0(id self, SEL selector, id value, id key)
{
	brightness_take((CFTypeRef)key, (CFTypeRef)value);
	return brightness_original[0](self, selector, value, key);
}

static intptr_t
brightness_set1(id self, SEL selector, id value, id key)
{
	brightness_take((CFTypeRef)key, (CFTypeRef)value);
	return brightness_original[1](self, selector, value, key);
}

static void
brightness_bridge(void)
{
	static const char *const classes[] = { "BrightnessSystemClient", "BrightnessSystem" };
	static const char *const selectors[] = { "setProperty:forKey:", "setProperty:withKey:" };
	static const IMP wrappers[] = { (IMP)brightness_set0, (IMP)brightness_set1 };
	static int tries;
	unsigned hooked = 0;

	for (unsigned c = 0; c < 2; c++) {
		Class cls = objc_getClass(classes[c]);

		if (cls == NULL || brightness_original[c] != NULL) {
			hooked += brightness_original[c] != NULL;
			continue;
		}
		for (unsigned s = 0; s < 2; s++) {
			Method method = class_getInstanceMethod(cls, sel_registerName(selectors[s]));

			if (method != NULL && method_getNumberOfArguments(method) == 4) {
				brightness_original[c] = (set_property_fn)method_setImplementation(method, wrappers[c]);
				say("hakios: brightness: -[%s %s] taken, its DisplayBrightness goes to the panel\n", classes[c], selectors[s]);
				hooked++;
				break;
			}
		}
	}
	if (hooked == 0 && ++tries < 30) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), brightness_queue, ^{ brightness_bridge(); });
	} else if (hooked == 0) {
		say("hakios: brightness: no BrightnessSystem class with setProperty: found\n");
	}
}

static void
brightness_start(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	/* with SpringBoard's brightness_bridge= (inject_sb.c) that one alone writes: two writers on two curves flicker */
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "brightness_bridge=") != NULL) {
		say("hakios: brightness: brightness_bridge= given: SpringBoard's bridge writes the panel, not backboardd's\n");
		return;
	}
	brightness_queue = dispatch_queue_create("hakios.brightness", DISPATCH_QUEUE_SERIAL);
	dispatch_async(brightness_queue, ^{ brightness_bridge(); });
}

