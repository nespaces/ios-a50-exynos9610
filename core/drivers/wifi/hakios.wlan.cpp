/*
 * HakiosWLAN: the registry entries by which Darwin finds the Wi-Fi interface.
 *
 * en0 is made with the BSD interface KPI (drivers/net/hakios.net.c) and has no IOKit object behind
 * it of its own; Darwin looks for two, as IO80211Family would have made them:
 *
 *   - wifid's device is whatever service has the property IO80211InterfaceRole =
 *     "Infrastructure", and IO80211.framework binds to an interface through a service whose
 *     IOInterfaceName is the interface's name. The service has no user client, on purpose:
 *     the framework's attempt to open one fails, it says "Falling back to IOCTL path", and
 *     from then on every request is an ioctl on a socket, which drivers/wifi/hakios.apple80211.c
 *     answers.
 *   - SystemConfiguration (SCNetworkInterfaceCopyAll) lists the network interfaces it finds as
 *     objects of the class IONetworkInterface, and calls one a Wi-Fi interface when its parent
 *     is of the class IO80211Controller; type, unit, name and built-in-ness are properties of
 *     the interface, the hardware address one of the controller. configd makes a network
 *     service (DHCP and the rest) for an interface it has listed, and only an interface with a
 *     service can be the one the system routes through -- which wifid waits for after a join
 *     ("failed to become routable interface") before Settings ticks the network.
 *
 *   - MobileGestalt, which lsd asks for the Wi-Fi address of every bundle it registers, looks the
 *     address up as the property local-mac-address of the device tree's "wlan" node, as a phone
 *     has it; without that it waits, up to 5 s every time, for a service of the class
 *     IONetworkController under that node. With neither, each such question cost 5 s, and a
 *     LaunchServices database rebuilt in full (a first boot on an empty data volume) takes hours,
 *     SpringBoard waiting. (Found by the ios-sapphire port, patches 0157 and 0158, which carries
 *     this file on: github.com/Angelpro09xd/ios-sapphire, APSL 2.0.)
 *
 * So: a controller of the class IO80211Controller -- an IOEthernetController and so an
 * IONetworkController, as in IO80211Family -- under the "wlan" node (the platform when there is
 * none), the radio's address on that node as local-mac-address, and below the controller one
 * object that is both, an IONetworkInterface with wifid's properties. The classes are ours, named
 * as IO80211Family and IONetworkingFamily name theirs; none has anything but properties.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>

#ifndef kIOMessageServicePropertyChange
#define kIOMessageServicePropertyChange  ((unsigned)0xe00001d0) /* iokit_common_msg(0x1d0) */
#endif
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>

extern "C" void HakiosWLANStart(const char * interface, const uint8_t * address);
extern "C" void HakiosWLANSetLink(bool up);

class IONetworkController : public IOService
{
	OSDeclareDefaultStructors(IONetworkController);
};

OSDefineMetaClassAndStructors(IONetworkController, IOService);

class IOEthernetController : public IONetworkController
{
	OSDeclareDefaultStructors(IOEthernetController);
};

OSDefineMetaClassAndStructors(IOEthernetController, IONetworkController);

class IO80211Controller : public IOEthernetController
{
	OSDeclareDefaultStructors(IO80211Controller);
};

OSDefineMetaClassAndStructors(IO80211Controller, IOEthernetController);

class IONetworkInterface : public IOService
{
	OSDeclareDefaultStructors(IONetworkInterface);
};

OSDefineMetaClassAndStructors(IONetworkInterface, IOService);

class IOEthernetInterface : public IONetworkInterface
{
	OSDeclareDefaultStructors(IOEthernetInterface);
};

OSDefineMetaClassAndStructors(IOEthernetInterface, IONetworkInterface);

class IO80211Interface : public IOEthernetInterface
{
	OSDeclareDefaultStructors(IO80211Interface);
};

OSDefineMetaClassAndStructors(IO80211Interface, IOEthernetInterface);

class HakiosWLAN : public IO80211Interface
{
	OSDeclareDefaultStructors(HakiosWLAN);

public:
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
	    IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosWLAN, IO80211Interface);

/* There is none (see the top); that somebody asked shows that the service was found. */
IOReturn
HakiosWLAN::newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties, IOUserClient ** handler)
{
	static unsigned said;

	if (said++ < 8) {
		IOLog("HakiosWLAN: a user client of type %u is asked for: there is none\n", (unsigned) type);
	}
	return kIOReturnUnsupported;
}

/* attach, start, register: the service, or NULL */
static IOService *
publish(IOService * service, IOService * provider, const char * name)
{
	if (service == NULL || !service->attach(provider)) {
		OSSafeReleaseNULL(service);
		return NULL;
	}
	service->setName(name);
	if (!service->start(provider)) {
		service->detach(provider);
		service->release();
		return NULL;
	}
	service->registerService();
	return service;
}

static IO80211Controller * g_controller;
static HakiosWLAN * g_wlan;

/*
 * configd only starts DHCP for an IONetworkInterface whose controller reports an active link
 * (hakios.wlan.cpp header: "failed to become routable interface"). BSD IFF_UP alone is not enough:
 * scsc20 had link up, RX, and no TX at all.
 */
extern "C" boolean_t ml_get_interrupts_enabled(void);

void
HakiosWLANSetLink(bool up)
{
	UInt32 status = up ? 0x1u | 0x2u : 0x1u;        /* kIONetworkLinkValid | Active */
	OSNumber * number;
	bool notify;

	/*
	 * scsc21 panicked on the way back to the loader: "Console I/O from interrupt-disabled context"
	 * (serial_console.c:168) from IOLog/messageClients here. setProperty only; notify when IRQs are on.
	 */
	/* setProperty/messageClients also print; skip the whole update if IRQs are off (loader return). */
	if (ml_get_interrupts_enabled() == 0) {
		return;
	}
	notify = true;
	if (g_controller != NULL && (number = OSNumber::withNumber((unsigned long long) status, 32)) != NULL) {
		g_controller->setProperty("IOLinkStatus", number);
		number->release();
		if (notify) {
			g_controller->messageClients(kIOMessageServicePropertyChange);
		}
	}
	if (g_wlan != NULL && (number = OSNumber::withNumber(up ? 1ull : 0ull, 32)) != NULL) {
		g_wlan->setProperty("IOInterfaceState", number);
		number->release();
		if (notify) {
			g_wlan->messageClients(kIOMessageServicePropertyChange);
		}
	}
}

/* Once, when the interface of a radio exists. */
void
HakiosWLANStart(const char * interface, const uint8_t * address)
{
	IOService * platform = IOService::getPlatform();
	OSDictionary * match = IOService::nameMatching("wlan");
	IOService * node = match != NULL ? IOService::copyMatchingService(match) : NULL;   /* the device tree's */
	IO80211Controller * controller = OSTypeAlloc(IO80211Controller);
	HakiosWLAN * wlan = OSTypeAlloc(HakiosWLAN);
	OSData * mac;
	OSNumber * number;

	if (platform == NULL || controller == NULL || wlan == NULL || !controller->init() || !wlan->init()) {
		OSSafeReleaseNULL(controller);
		OSSafeReleaseNULL(wlan);
		OSSafeReleaseNULL(node);
		OSSafeReleaseNULL(match);
		IOLog("HakiosWLAN: cannot create the services\n");
		return;
	}
	if ((mac = OSData::withBytes(address, 6)) != NULL) {
		controller->setProperty("IOMACAddress", mac);
		if (node != NULL) {
			node->setProperty("local-mac-address", mac);     /* where MobileGestalt reads it first */
		}
		mac->release();
	}
	controller->setProperty("IOBuiltin", true);
	OSSafeReleaseNULL(match);
	controller = (IO80211Controller *) publish(controller, node != NULL ? node : platform, "HakiosWLANController");
	OSSafeReleaseNULL(node);
	if (controller == NULL) {
		wlan->release();
		IOLog("HakiosWLAN: the controller did not start\n");
		return;
	}
	g_controller = controller;

	wlan->setProperty("IOInterfaceName", interface);
	wlan->setProperty("BSD Name", interface);
	wlan->setProperty("IOInterfaceNamePrefix", "en");
	if ((number = OSNumber::withNumber(0x47ULL, 32)) != NULL) {     /* IFT_IEEE80211: configd's Wi-Fi, not Ethernet */
		wlan->setProperty("IOInterfaceType", number);
		number->release();
	}
	if ((number = OSNumber::withNumber((unsigned long long) (interface[2] - '0'), 32)) != NULL) {
		wlan->setProperty("IOInterfaceUnit", number);
		number->release();
	}
	if ((number = OSNumber::withNumber(1500ULL, 32)) != NULL) {
		wlan->setProperty("IOMaxTransferUnit", number);
		number->release();
	}
	if ((number = OSNumber::withNumber(6ULL, 32)) != NULL) {
		wlan->setProperty("IOMediaAddressLength", number);
		number->release();
	}
	if ((number = OSNumber::withNumber(14ULL, 32)) != NULL) {
		wlan->setProperty("IOMediaHeaderLength", number);
		number->release();
	}
	wlan->setProperty("IOBuiltin", true);
	wlan->setProperty("IOPrimaryInterface", true);
	wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
	{
		OSDictionary * data = OSDictionary::withCapacity(1);

		if (data != NULL) {
			wlan->setProperty("IONetworkData", data);
			data->release();
		}
	}
	if (publish(wlan, controller, interface) == NULL) {             /* registry name en0, as IONetworkingFamily */
		IOLog("HakiosWLAN: the service did not start\n");
	} else {
		g_wlan = wlan;
		wlan->release();
	}
	controller->release();
}
