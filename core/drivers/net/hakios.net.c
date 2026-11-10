/*
 * en0: a network interface that is Ethernet as far as the rest of the kernel and Darwin can tell.
 *
 * Darwin expects its interfaces from IONetworkingFamily, which this kernel does not have; the BSD
 * half of the kernel does not need it. The interface is made with the kernel's own interface
 * KPI, the way bsd/net/if_fake.c makes its "feth": family Ethernet, the stock Ethernet framing
 * and demultiplexing, one function that is given a frame to send and one call to hand a frame
 * that has arrived to the stack. What Darwin's user space makes of an interface that has no
 * IOKit object behind it is another matter: see userland/src/net_test.m.
 *
 * What carries the frames is a link driver's business; sys/hakios.net.h is all that the two
 * know of each other. The first link is QEMU's virtio network device (hakios.virtio_net.c).
 * There can be two interfaces: en0 for the Wi-Fi (or virtio), en1 for the modem's packets
 * through the IPA (hakios.cell.c); each has its link, its state and its thread.
 */
#include <stdarg.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>
#include <pexpert/pexpert.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/hakios.net.h>
#include <sys/sockio.h>
#include <sys/systm.h>

extern void HakiosWLANStart(const char *interface, const uint8_t *address);  /* drivers/wifi/hakios.wlan.cpp */
extern void HakiosWLANSetLink(bool up);

static LCK_GRP_DECLARE(net_lck_grp, "hakios-net");

/* One interface: enN on its link. */
struct net_if {
	unsigned        unit;
	const struct hakios_net_link *link;
	bool            up;             /* the link says frames can pass */
	ifnet_t         ifp;
	lck_mtx_t       tx_lock;        /* one frame at a time to the link */
	/* What has gone through: said once a minute while anything moves. */
	struct {
		uint64_t sent, received, sent_bytes, received_bytes, dropped_out, dropped_in;
	} count;
};
static struct net_if net_ifs[HAKIOS_NET_UNITS];

static inline struct net_if *
net_if_of(ifnet_t ifp)
{
	return ifnet_softc(ifp);
}

/* The interface on unit `unit`, once attached; NULL before. */
static struct net_if *
net_if_unit(unsigned unit)
{
	return unit < HAKIOS_NET_UNITS && net_ifs[unit].ifp != NULL ? &net_ifs[unit] : NULL;
}

/*
 * Boot-arg net_trace=N: a line in the log for each of the first N frames, either way.
 * Boot-arg net_names=1: a line for every name asked of a DNS server (IPv4 or IPv6, and which server), for every
 * answer that comes back (its code and first addresses) and for every TCP connection begun from here -- which is how
 * to see what a system that has just been given a network does with it (up to NAMES_MAX lines).
 */
#define NAMES_MAX       3000
static uint32_t net_trace, net_names;

static void
ipv4_text(struct in_addr a, char *text, size_t room)
{
	const uint8_t *b = (const uint8_t *)&a.s_addr;

	snprintf(text, room, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/*
 * A DNS message's name at `p` (labels, or a pointer to some earlier in the message): where it ends in the message, NULL
 * if it is malformed; its text in `name` if one is given (truncated to fit).
 */
static const uint8_t *
dns_name(const uint8_t *message, const uint8_t *p, const uint8_t *end, char *name, size_t room)
{
	const uint8_t *after = NULL;
	unsigned jumps = 0;
	size_t n = 0;

	while (p >= message && p < end) {
		uint8_t length = *p;

		if (length == 0) {
			if (name != NULL && room != 0) {
				name[n ? n - 1 : 0] = 0;
			}
			return after != NULL ? after : p + 1;
		}
		if ((length & 0xc0) == 0xc0) {
			if (p + 2 > end || ++jumps > 16) {
				return NULL;
			}
			if (after == NULL) {
				after = p + 2;
			}
			p = message + ((length & 0x3f) << 8 | p[1]);
			continue;
		}
		if (length >= 64 || p + 1 + length > end) {
			return NULL;
		}
		if (name != NULL && n + length + 2 < room) {
			memcpy(name + n, p + 1, length);
			n += length;
			name[n++] = '.';
		}
		p += 1 + length;
	}
	return NULL;
}

/*
 * A DNS response: its code, how many answers it has, the first `room` IPv4 addresses among them, and the question's
 * name if `name` is given. False if it is not a response.
 */
static bool
dns_answer(const uint8_t *m, size_t length, unsigned *rcode, unsigned *answers, struct in_addr *a, unsigned room,
    unsigned *a_count, char *name, size_t name_room)
{
	const uint8_t *end = m + length, *p;
	unsigned an;

	*a_count = 0;
	if (name != NULL && name_room != 0) {
		name[0] = 0;
	}
	if (length < 12 || !(m[2] & 0x80)) {
		return false;
	}
	*rcode = m[3] & 15;
	*answers = an = (unsigned)(m[6] << 8 | m[7]);
	if ((m[4] << 8 | m[5]) == 0 || (p = dns_name(m, m + 12, end, name, name_room)) == NULL) {
		return true;
	}
	p += 4;
	for (unsigned i = 0; i < an && p != NULL && p < end; i++) {
		unsigned type, data;

		p = dns_name(m, p, end, NULL, 0);
		if (p == NULL || p + 10 > end) {
			break;
		}
		type = (unsigned)(p[0] << 8 | p[1]);
		data = (unsigned)(p[8] << 8 | p[9]);
		p += 10;
		if (p + data > end) {
			break;
		}
		if (type == 1 && data == 4 && *a_count < room) {
			memcpy(&a[(*a_count)++].s_addr, p, 4);
		}
		p += data;
	}
	return true;
}

static uint32_t net_names_said;         /* (net_names: the lines said so far, both ways) */
static bool net_names_console;          /* ("wifi names": the lines on the USB console too, as they come) */

extern void loader_log_line(const char *line);

/* one of net_names's lines: into the kernel's log, and onto the USB console after "wifi names" */
static void __printflike(1, 2)
names_say(const char *format, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	net_names_said++;
	printf("%s", line);
	if (net_names_console) {
		loader_log_line(line);
	}
}

/* an IPv6 address, shortly: its eight groups in hex */
static void
ipv6_text(const uint8_t *a, char *text, size_t room)
{
	snprintf(text, room, "%x:%x:%x:%x:%x:%x:%x:%x", a[0] << 8 | a[1], a[2] << 8 | a[3], a[4] << 8 | a[5], a[6] << 8 | a[7],
	    a[8] << 8 | a[9], a[10] << 8 | a[11], a[12] << 8 | a[13], a[14] << 8 | a[15]);
}

/* net_names: a question to a DNS server, or the start of a TCP connection, on its way out (IPv4 or IPv6) */
static void
names_out(const uint8_t *ip, const uint8_t *end, bool v6)
{
	const uint8_t *l4 = v6 ? ip + 40 : ip + 4 * (ip[0] & 0xf);
	unsigned protocol = v6 ? ip[6] : ip[9];
	char to[48];

	if (l4 + 14 > end || (protocol != IPPROTO_UDP && protocol != IPPROTO_TCP)) {
		return;
	}
	if (v6) {
		ipv6_text(ip + 24, to, sizeof(to));
	} else {
		snprintf(to, sizeof(to), "%u.%u.%u.%u", ip[16], ip[17], ip[18], ip[19]);
	}
	if (protocol == IPPROTO_UDP && (l4[2] << 8 | l4[3]) == 53 && l4 + 8 + 12 < end) {
		const uint8_t *dns = l4 + 8, *after;
		char name[128];

		after = dns_name(dns, dns + 12, end, name, sizeof(name));
		names_say("net: asks %s for the name %s (type %u)\n", to, after != NULL ? name : "?",
		    after != NULL && after + 2 <= end ? after[0] << 8 | after[1] : 0);
	} else if (protocol == IPPROTO_TCP && (l4[13] & 0x12) == 0x02) {
		names_say("net: connects to %s port %u%s\n", to, l4[2] << 8 | l4[3], v6 ? " (IPv6)" : "");
	}
}

/* net_names: a DNS server's answer, on its way in */
static void
names_in(const uint8_t *dns, const uint8_t *end, const char *from)
{
	struct in_addr a[2];
	unsigned rcode, answers, count;
	char name[128], first[16] = "", second[20] = "";

	if (!dns_answer(dns, (size_t)(end - dns), &rcode, &answers, a, 2, &count, name, sizeof(name))) {
		return;
	}
	if (count > 0) {
		ipv4_text(a[0], first, sizeof(first));
	}
	if (count > 1) {
		second[0] = ' ';
		ipv4_text(a[1], second + 1, sizeof(second) - 1);
	}
	names_say("net: %s answers for %s: code %u, %u answer%s%s%s%s\n", from, name[0] ? name : "?", rcode, answers,
	    answers == 1 ? "" : "s", count ? "; IPv4 " : "", first, second);
}

static void
trace_frame(const char *way, const uint8_t *frame, size_t length, size_t whole)
{
	static uint32_t said;
	const uint8_t *end = frame + length, *ip = frame + ETHER_HDR_LEN, *l4;
	uint16_t type;
	bool ports;

	if (length < ETHER_HDR_LEN + 28 || (said >= net_trace && (!net_names || net_names_said >= NAMES_MAX))) {
		return;
	}
	type = (uint16_t)(frame[12] << 8 | frame[13]);
	l4 = ip + 4 * (ip[0] & 0xf);
	ports = type == ETHERTYPE_IP && (ip[9] == IPPROTO_TCP || ip[9] == IPPROTO_UDP) && l4 + 4 <= end;
	if (net_names && net_names_said < NAMES_MAX && way[0] == 'o' && (type == ETHERTYPE_IP || type == ETHERTYPE_IPV6)) {
		names_out(ip, end, type == ETHERTYPE_IPV6);
	}
	if (said >= net_trace) {
		return;
	}
	said++;
	if (type == ETHERTYPE_IP) {
		printf("net: %s %zu bytes: IPv4 %s %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u%s\n", way, whole, ip[9] == IPPROTO_TCP ? "TCP" :
		    ip[9] == IPPROTO_UDP ? "UDP" : ip[9] == IPPROTO_ICMP ? "ICMP" : "protocol?", ip[12], ip[13], ip[14], ip[15],
		    ports ? l4[0] << 8 | l4[1] : 0, ip[16], ip[17], ip[18], ip[19], ports ? l4[2] << 8 | l4[3] : 0,
		    ip[9] == IPPROTO_TCP && ports && l4 + 14 <= end ? (l4[13] & 2 ? " SYN" : l4[13] & 1 ? " FIN" : l4[13] & 4 ? " RST" : "") : "");
	} else {
		printf("net: %s %zu bytes: %s (type %#x) to %02x:%02x:%02x:%02x:%02x:%02x\n", way, whole, type == ETHERTYPE_ARP ? "ARP" :
		    type == ETHERTYPE_IPV6 ? "IPv6" : "?", type, frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);
	}
}

/* ... of a frame on its way out: the start of it is enough */
static void
trace_packet(mbuf_t packet, size_t length)
{
	uint8_t start[192];
	size_t have = length < sizeof(start) ? length : sizeof(start);

	if ((net_trace || net_names) && mbuf_copydata(packet, 0, have, start) == 0) {
		trace_frame("out", start, have, length);
	}
}

/*
 * What has come in and gone out on en0, for the USB console's "wifi stat": frames by size (do the big ones of a
 * download arrive at all), the biggest each way, those too big to hand up, by kind; and what DHCP gave as the router
 * and the DNS servers (configd's resolver is given those; "wifi dns" asks the first itself).
 */
#define NET_SIZES       4
static const uint32_t net_size_top[NET_SIZES] = { 128, 576, 1200, HAKIOS_NET_FRAME_MAX };
static struct {
	uint64_t in[NET_SIZES], out[NET_SIZES];
	uint32_t in_biggest, out_biggest;
	uint64_t in_too_big;
	uint32_t in_too_big_last;
	uint64_t in_ipv4, in_ipv6, in_arp, in_other, in_tcp, in_udp, in_dns;
	struct in_addr dns[2], router;
	unsigned dns_count;
} net_seen;

static unsigned
net_size_class(size_t length)
{
	unsigned i = 0;

	while (i + 1 < NET_SIZES && length > net_size_top[i]) {
		i++;
	}
	return i;
}

/* DHCP's offer or acknowledgement (UDP 67 to 68): the router and the DNS servers among its options */
static void
net_seen_dhcp(const uint8_t *bootp, const uint8_t *end)
{
	const uint8_t *o = bootp + 240;
	struct in_addr dns[2] = { { 0 }, { 0 } }, router = { 0 };
	unsigned count = 0;

	if (o > end || bootp[236] != 99 || bootp[237] != 130 || bootp[238] != 83 || bootp[239] != 99) {
		return;                 /* (no options: not DHCP's) */
	}
	while (o < end && *o != 255) {
		if (*o == 0) {
			o++;
			continue;
		}
		if (o + 2 > end || o + 2 + o[1] > end) {
			break;
		}
		if (o[0] == 6) {
			for (unsigned i = 0; i + 4 <= o[1] && count < 2; i += 4) {
				memcpy(&dns[count++].s_addr, o + 2 + i, 4);
			}
		} else if (o[0] == 3 && o[1] >= 4) {
			memcpy(&router.s_addr, o + 2, 4);
		}
		o += 2 + o[1];
	}
	if (count != 0 && (count != net_seen.dns_count || memcmp(dns, net_seen.dns, sizeof(dns)) != 0)) {
		char first[16], second[16];

		ipv4_text(dns[0], first, sizeof(first));
		ipv4_text(dns[1], second, sizeof(second));
		printf("net: DHCP gives the DNS server%s %s%s%s\n", count > 1 ? "s" : "", first, count > 1 ? " and " : "",
		    count > 1 ? second : "");
		memcpy(net_seen.dns, dns, sizeof(dns));
		net_seen.dns_count = count;
	}
	if (router.s_addr != 0) {
		net_seen.router = router;
	}
}

/* a frame that has come in on en0, before it is handed up: counted, and looked at */
static void
net_seen_in(const uint8_t *frame, size_t length)
{
	const uint8_t *end = frame + length, *ip = frame + ETHER_HDR_LEN, *l4;
	uint16_t type;

	if (length > HAKIOS_NET_FRAME_MAX) {
		net_seen.in_too_big++;
		net_seen.in_too_big_last = (uint32_t)length;
		if (net_seen.in_too_big <= 8) {
			printf("net: a frame of %zu bytes came in, more than %u: dropped\n", length, HAKIOS_NET_FRAME_MAX);
		}
		return;
	}
	net_seen.in[net_size_class(length)]++;
	if (length > net_seen.in_biggest) {
		net_seen.in_biggest = (uint32_t)length;
	}
	if (length < ETHER_HDR_LEN + 20) {
		net_seen.in_other++;
		return;
	}
	type = (uint16_t)(frame[12] << 8 | frame[13]);
	if (type == ETHERTYPE_ARP) {
		net_seen.in_arp++;
	} else if (type == ETHERTYPE_IPV6 && length >= ETHER_HDR_LEN + 48) {
		net_seen.in_ipv6++;
		l4 = ip + 40;
		if (ip[6] == IPPROTO_TCP) {
			net_seen.in_tcp++;
		} else if (ip[6] == IPPROTO_UDP) {
			net_seen.in_udp++;
			if ((l4[0] << 8 | l4[1]) == 53) {
				char from[48];

				net_seen.in_dns++;
				if (net_names && net_names_said < NAMES_MAX) {
					ipv6_text(ip + 8, from, sizeof(from));
					names_in(l4 + 8, end, from);
				}
			}
		}
	} else if (type == ETHERTYPE_IP) {
		net_seen.in_ipv4++;
		l4 = ip + 4 * (ip[0] & 0xf);
		if (ip[9] == IPPROTO_TCP) {
			net_seen.in_tcp++;
		} else if (ip[9] == IPPROTO_UDP && l4 + 8 <= end) {
			unsigned from_port = (unsigned)(l4[0] << 8 | l4[1]), to_port = (unsigned)(l4[2] << 8 | l4[3]);

			net_seen.in_udp++;
			if (from_port == 67 && to_port == 68) {
				net_seen_dhcp(l4 + 8, end);
			} else if (from_port == 53) {
				char from[16];

				net_seen.in_dns++;
				if (net_names && net_names_said < NAMES_MAX) {
					snprintf(from, sizeof(from), "%u.%u.%u.%u", ip[12], ip[13], ip[14], ip[15]);
					names_in(l4 + 8, end, from);
				}
			}
		}
	} else {
		net_seen.in_other++;
	}
}

/* ---- the interface -------------------------------------------------------------------------------- */

/* The stack has a frame to send (a chain of mbufs that starts with the Ethernet header). */
static errno_t
net_output(ifnet_t ifp, mbuf_t packet)
{
	struct net_if *n = net_if_of(ifp);
	size_t length = mbuf_pkthdr_len(packet);
	bool sent = false;

	if (length <= HAKIOS_NET_FRAME_MAX && n->up) {
		bpf_tap_out(ifp, DLT_EN10MB, packet, NULL, 0);
		trace_packet(packet, length);
		lck_mtx_lock(&n->tx_lock);
		sent = n->link->transmit(packet, length);
		lck_mtx_unlock(&n->tx_lock);
	} else if (n->count.dropped_out < 8) {
		printf("net: en%u drop TX %zu bytes (link %s)\n", n->unit, length, n->up ? "up" : "down");
	}
	if (sent) {
		if (n->unit == 0) {
			net_seen.out[net_size_class(length)]++;
			if (length > net_seen.out_biggest) {
				net_seen.out_biggest = (uint32_t)length;
			}
		}
		n->count.sent++;
		n->count.sent_bytes += length;
		ifnet_stat_increment_out(ifp, 1, (uint32_t)length, 0);
	} else {
		n->count.dropped_out++;
		ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	mbuf_freem(packet);
	return 0;
}

/* A frame has arrived on unit `unit`. */
void
hakios_net_input_unit(unsigned unit, const uint8_t *frame, size_t length)
{
	struct net_if *n = net_if_unit(unit);
	struct ifnet_stat_increment_param stats = { .packets_in = 1, .bytes_in = (uint32_t)length };
	unsigned int chunks = 1;
	mbuf_t packet = NULL;
	char *header;

	if (n == NULL) {
		return;
	}
	if (unit == 0) {
		net_seen_in(frame, length);
	}
	trace_frame("in ", frame, length, length);
	if (length < ETHER_HDR_LEN || length > HAKIOS_NET_FRAME_MAX || mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet) != 0) {
		n->count.dropped_in++;
		return;
	}
	if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
		mbuf_freem(packet);
		n->count.dropped_in++;
		return;
	}
	/* as every Ethernet driver hands a frame up: the header noted and stepped over */
	header = mbuf_data(packet);
	mbuf_pkthdr_setrcvif(packet, n->ifp);
	mbuf_pkthdr_setheader(packet, header);
	mbuf_pkthdr_adjustlen(packet, -ETHER_HDR_LEN);
	mbuf_setdata(packet, header + ETHER_HDR_LEN, length - ETHER_HDR_LEN);
	bpf_tap_in(n->ifp, DLT_EN10MB, packet, header, ETHER_HDR_LEN);
	n->count.received++;
	n->count.received_bytes += length;
	ifnet_input(n->ifp, packet, &stats);
}

void
hakios_net_input(const uint8_t *frame, size_t length)
{
	hakios_net_input_unit(0, frame, length);
}

static unsigned net_ioctls_said;

static errno_t
net_ioctl(ifnet_t ifp, u_long command, void *data)
{
	struct net_if *n = net_if_of(ifp);
	/* (a radio says it is one here too: IO80211.framework lists the interfaces whose media is 802.11) */
	const int media[] = { (n->link->wireless ? IFM_IEEE80211 : IFM_ETHER) | IFM_AUTO };
	struct ifreq *request = data;
	struct ifmediareq *media_request = data;
	user_addr_t list;
	errno_t error = 0;

	if (n->link->wireless && net_ioctls_said < 80) {
		char who[32] = "?";

		/* (while Darwin's Wi-Fi side is being got to talk to this: who asks the interface what) */
		proc_selfname(who, sizeof(who));
		if (strcmp(who, "configd") != 0) {
			net_ioctls_said++;
			printf("net: en0 is asked %#lx by %s\n", command, who);
		}
	}
	switch (command) {
	case SIOCSIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		break;
	case SIOCSIFFLAGS:
		/*
		 * configd's IPConfiguration (net_test dhcp en0, scsc26) sent this with data == NULL:
		 * FAR 0x10 is ifr_flags. A null request is EFAULT; IFF_UP already follows the link.
		 */
		if (request == NULL) {
			break;
		}
		ifnet_set_flags(ifp, request->ifr_flags, IFF_UP);
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		break;
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64:
		/* what configd asks to learn whether the cable is in */
		list = command == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)media_request)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)media_request)->ifmu_ulist);
		media_request->ifm_active = media_request->ifm_current = media[0];
		media_request->ifm_mask = 0;
		media_request->ifm_status = IFM_AVALID | (n->up ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && media_request->ifm_count > 0) {
			error = copyout(media, list, sizeof(media));
		}
		media_request->ifm_count = 1;
		break;
	case SIOCGIFDEVMTU:
		request->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
		request->ifr_devmtu.ifdm_max = ETHERMTU;
		request->ifr_devmtu.ifdm_min = IF_MINMTU;
		break;
	case SIOCSIFMTU:
		error = request->ifr_mtu > ETHERMTU || request->ifr_mtu < IF_MINMTU ? EINVAL : ifnet_set_mtu(ifp, request->ifr_mtu);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;                          /* the link hands us everything anyway */
	default:
		/* what Darwin's Wi-Fi side asks of a radio's interface: drivers/wifi/hakios.apple80211.c */
		error = n->link->wireless ? hakios_apple80211_ioctl(command, data) : EOPNOTSUPP;
		break;
	}
	return error;
}

/* Tell whoever listens for kernel events (configd does) that the link is up, or down. */
static void
net_link_event(ifnet_t ifp, uint32_t code)
{
	_Alignas(struct kern_event_msg) char message[sizeof(struct kern_event_msg) + sizeof(struct net_event_data)] = { 0 };
	struct kern_event_msg *header = (struct kern_event_msg *)message;
	struct net_event_data *about = (struct net_event_data *)(header + 1);

	header->total_size = sizeof(message);
	header->vendor_code = KEV_VENDOR_APPLE;
	header->kev_class = KEV_NETWORK_CLASS;
	header->kev_subclass = KEV_DL_SUBCLASS;
	header->event_code = code;
	about->if_family = ifnet_family(ifp);
	about->if_unit = ifnet_unit(ifp);
	strlcpy(about->if_name, ifnet_name(ifp), IFNAMSIZ);
	ifnet_event(ifp, header);
}

void
hakios_net_link_state_unit(unsigned unit, bool up)
{
	struct net_if *n = net_if_unit(unit);

	if (n == NULL || up == n->up) {
		return;
	}
	n->up = up;
	ifnet_set_flags(n->ifp, up ? (IFF_UP | IFF_RUNNING) : 0, IFF_UP | IFF_RUNNING);
	printf("net: en%u's link (%s) is %s\n", n->unit, n->link->name, up ? "up" : "down");
	net_link_event(n->ifp, up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF);
	if (n->link->wireless) {
		HakiosWLANSetLink(up);
	}
}

void
hakios_net_link_state(bool up)
{
	hakios_net_link_state_unit(0, up);
}

/* The interface's own thread: for a link that wants to be asked, and for the minute's line in the log. */
static void
net_thread(void *parameter, __unused wait_result_t result)
{
	struct net_if *n = parameter;
	uint64_t said = 0, total;
	unsigned quiet = 0;

	for (unsigned tick = 1;; tick++) {
		quiet = n->link->poll != NULL && n->link->poll() ? 0 : quiet + 1;
		/* about once a minute, if anything has moved since */
		total = n->count.sent + n->count.received + n->count.dropped_out + n->count.dropped_in;
		if (tick % 8192 == 0 && total != said) {
			said = total;
			printf("net: en%u so far %llu frames sent (%llu bytes), %llu received (%llu bytes); dropped: %llu out, %llu in\n",
			    n->unit, n->count.sent, n->count.sent_bytes, n->count.received, n->count.received_bytes,
			    n->count.dropped_out, n->count.dropped_in);
		}
		delay_for_interval(quiet < 100 ? 2 : 10, NSEC_PER_MSEC);
	}
}

bool
hakios_net_attach(const struct hakios_net_link *link)
{
	unsigned unit = link->unit;
	struct net_if *n = unit < HAKIOS_NET_UNITS ? &net_ifs[unit] : NULL;
	char uniqueid[16];
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		/*
		 * The simple model: every frame straight to net_output(). And no Skywalk "nexus" made for
		 * the interface by itself: sockets are all that is asked of it so far.
		 */
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = uniqueid,
		.uniqueid_len = 11,
		.name = "en",
		.unit = unit,
		.softc = n,
		.family = IFNET_FAMILY_ETHERNET,
		.subfamily = link->wireless ? IFNET_SUBFAMILY_WIFI : IFNET_SUBFAMILY_ANY,
		.type = IFT_ETHER,              /* 0x47 IEEE80211 on scsc34: DHCP did not complete; keep Ethernet framing */
		.output = net_output,
		.demux = ether_demux,
		.add_proto = ether_add_proto,
		.del_proto = ether_del_proto,
		.check_multi = ether_check_multi,
		.framer_extended = ether_frameout_extended,
		.ioctl = net_ioctl,
		.broadcast_addr = etherbroadcastaddr,
		.broadcast_len = ETHER_ADDR_LEN,
	};
	const uint8_t *address = link->address;
	ifnet_t ifp = NULL;
	thread_t thread;
	errno_t error;

	if (n == NULL) {
		printf("net: there is no unit %u for %s\n", unit, link->name);
		return false;
	}
	if (n->link != NULL) {
		printf("net: en%u is on %s already; %s is not attached\n", unit, n->link->name, link->name);
		return false;
	}
	snprintf(uniqueid, sizeof(uniqueid), "hakios-en%u", unit);
	n->unit = unit;
	n->link = link;
	n->up = link->up;
	lck_mtx_init(&n->tx_lock, &net_lck_grp, LCK_ATTR_NULL);
	error = ifnet_allocate_extended(&init, &ifp);
	if (error != 0) {
		printf("net: the interface en%u cannot be made: error %d\n", unit, error);
		n->link = NULL;
		return false;
	}
	ifnet_set_capabilities_enabled(ifp, 0, -1);
	ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
	ifnet_set_baudrate(ifp, link->wireless ? 72ull * 1000 * 1000 : 100ull * 1000 * 1000);
	ifnet_set_mtu(ifp, ETHERMTU);
	ifnet_set_flags(ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
	ifnet_set_hdrlen(ifp, ETHER_HDR_LEN);
	ifnet_set_offload(ifp, 0);
	error = ifnet_attach(ifp, NULL);
	if (error != 0) {
		printf("net: the interface en%u cannot be attached: error %d\n", unit, error);
		ifnet_release(ifp);
		n->link = NULL;
		return false;
	}
	ifnet_set_lladdr(ifp, address, ETHER_ADDR_LEN);
	bpfattach(ifp, DLT_EN10MB, ETHER_HDR_LEN);
	n->ifp = ifp;
	if (n->up) {
		net_link_event(ifp, KEV_DL_LINK_ON);
	}
	if (link->wireless) {
		/* (a radio's address is the phone's own: not for the log; and Darwin's Wi-Fi side finds the interface by a service) */
		printf("net: en%u attached on %s (a radio), link %s\n", unit, link->name, n->up ? "up" : "down");
		HakiosWLANStart(uniqueid + 7, address);
	} else {
		printf("net: en%u attached on %s, address %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", unit, link->name, address[0],
		    address[1], address[2], address[3], address[4], address[5], n->up ? "up" : "down");
	}
	if (kernel_thread_start(net_thread, n, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return true;
}

void
hakios_net_init(void)
{
	PE_parse_boot_argn("net_trace", &net_trace, sizeof(net_trace));
	PE_parse_boot_argn("net_names", &net_names, sizeof(net_names));
	hakios_virtio_net_init();
}

/*
 * ---- the USB console's "wifi ip", "wifi ping", "wifi dns", "wifi tcp" and "wifi stat" (drivers/core/hakios.dwc3.c) ----
 * What en0 was given (configd's DHCP, once the radio has joined and said its link is up), and whether the network
 * answers: four ICMP echo requests from an ICMP datagram socket of the kernel's own (the kind ping uses without root),
 * a second for each answer, of any size up to 8000 bytes (more than a frame: fragments both ways); with no address, the
 * default route's gateway. A DNS question of the kernel's own (UDP, to the server DHCP gave); a TCP connection and an
 * HTTP GET, and how many bytes came back and how fast (the data path's big frames, in); and what en0 has counted:
 * frames by size and kind, and the IP, UDP and TCP layers' own counts (bad checksums among them).
 */
#include <net/route.h>
#include <netinet/ip_icmp.h>
#include <sys/kpi_socket.h>
#include <sys/socket.h>

extern void loader_log_line(const char *line);
extern kern_return_t thread_terminate(thread_t thread);

static void
console_say(const char *format, ...)
{
	char line[400];         /* ("wifi stat"'s lines are long) */
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	loader_log_line(line);
}

/* en0's first IPv4 address and mask; false without */
static bool
en0_ipv4(struct in_addr *address, struct in_addr *mask)
{
	ifnet_t ifp = NULL;
	ifaddr_t *list = NULL;
	bool found = false;

	if (ifnet_find_by_name("en0", &ifp) != 0 || ifp == NULL) {
		return false;
	}
	if (ifnet_get_address_list_family(ifp, &list, AF_INET) == 0 && list != NULL) {
		for (unsigned i = 0; list[i] != NULL && !found; i++) {
			struct sockaddr_in a = { 0 }, m = { 0 };

			if (ifaddr_address(list[i], (struct sockaddr *)&a, sizeof(a)) == 0 && a.sin_family == AF_INET) {
				*address = a.sin_addr;
				mask->s_addr = ifaddr_netmask(list[i], (struct sockaddr *)&m, sizeof(m)) == 0 ? m.sin_addr.s_addr : 0;
				found = true;
			}
		}
		ifnet_free_address_list(list);
	}
	ifnet_release(ifp);
	return found;
}

/* the default route's gateway, if there is one */
static bool
default_gateway(struct in_addr *gateway, char *interface, size_t room)
{
	struct sockaddr_in any = { .sin_len = sizeof(any), .sin_family = AF_INET };
	struct rtentry *rt = rtalloc1((struct sockaddr *)&any, 0, 0);
	bool found = false;

	if (rt == NULL) {
		return false;
	}
	RT_LOCK(rt);
	if ((rt->rt_flags & RTF_GATEWAY) != 0 && rt->rt_gateway != NULL && rt->rt_gateway->sa_family == AF_INET) {
		*gateway = ((struct sockaddr_in *)(void *)rt->rt_gateway)->sin_addr;
		if (rt->rt_ifp != NULL) {
			snprintf(interface, room, "%s%u", ifnet_name(rt->rt_ifp), ifnet_unit(rt->rt_ifp));
		} else {
			snprintf(interface, room, "?");
		}
		found = true;
	}
	RT_UNLOCK(rt);
	rtfree(rt);
	return found;
}

void
hakios_net_console_ip(void)
{
	struct in_addr address, mask, gateway;
	char a[16], m[16], g[16], via[IFNAMSIZ + 4];
	struct net_if *n = net_if_unit(0);

	if (n == NULL) {
		console_say("usb: wifi ip: there is no en0 (the radio's driver did not get to brcmfmac=7)\n");
		return;
	}
	if (!en0_ipv4(&address, &mask)) {
		console_say("usb: wifi ip: en0's link is %s; no IPv4 address yet (configd's DHCP, once the link is up)\n",
		    n->up ? "up" : "down");
		return;
	}
	ipv4_text(address, a, sizeof(a));
	ipv4_text(mask, m, sizeof(m));
	if (default_gateway(&gateway, via, sizeof(via))) {
		ipv4_text(gateway, g, sizeof(g));
	} else {
		snprintf(g, sizeof(g), "none");
		via[0] = 0;
	}
	console_say("usb: wifi ip: en0's link is %s; address %s, mask %s; the default route's gateway %s%s%s\n",
	    n->up ? "up" : "down", a, m, g, via[0] ? " on " : "", via);
}

static uint16_t
icmp_sum(const uint8_t *p, size_t length)
{
	uint32_t sum = 0;

	for (size_t i = 0; i + 1 < length; i += 2) {
		sum += (uint32_t)(p[i] << 8 | p[i + 1]);
	}
	if (length & 1) {
		sum += (uint32_t)p[length - 1] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return htons((uint16_t)~sum);
}

/*
 * The console's jobs that take time -- ping, dns, tcp -- run on a thread of their own, one at a time; what each is to
 * do is here while it runs.
 */
static volatile bool console_busy;
static struct {
	struct in_addr to;              /* (0: look the name up first) */
	uint32_t size;
	uint16_t port;
	char name[96];
	char path[96];
} job;

static bool
console_job_start(thread_continue_t body, const char *what)
{
	thread_t thread;

	if (console_busy) {
		console_say("usb: wifi %s: another of ping, dns and tcp is under way\n", what);
		return false;
	}
	console_busy = true;
	if (kernel_thread_start(body, NULL, &thread) != KERN_SUCCESS) {
		console_busy = false;
		console_say("usb: wifi %s: no thread for it\n", what);
		return false;
	}
	thread_set_thread_name(thread, "net-console");
	thread_deallocate(thread);
	return true;
}

static void
console_job_end(void)
{
	console_busy = false;
	thread_terminate(current_thread());
}

/* the next word of *line (up to a space or the end): its start and length; *line moved past it and the spaces after */
static size_t
next_word(const char **line, const char **start)
{
	const char *p = *line;
	size_t n = 0;

	while (*p == ' ') {
		p++;
	}
	*start = p;
	while (p[n] != 0 && p[n] != ' ') {
		n++;
	}
	p += n;
	while (*p == ' ') {
		p++;
	}
	*line = p;
	return n;
}

static bool
ipv4_parse(const char *s, size_t length, struct in_addr *address)
{
	unsigned part[4] = { 0 }, n = 0;
	bool digit = false;
	uint8_t *b = (uint8_t *)&address->s_addr;

	for (size_t i = 0; i < length; i++) {
		if (s[i] >= '0' && s[i] <= '9') {
			part[n] = part[n] * 10 + (unsigned)(s[i] - '0');
			if (part[n] > 255) {
				return false;
			}
			digit = true;
		} else if (s[i] == '.' && digit && n < 3) {
			n++;
			digit = false;
		} else {
			return false;
		}
	}
	if (n != 3 || !digit) {
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		b[i] = (uint8_t)part[i];
	}
	return true;
}

static bool
number_parse(const char *s, size_t length, uint32_t least, uint32_t most, uint32_t *value)
{
	uint32_t v = 0;

	if (length == 0 || length > 9) {
		return false;
	}
	for (size_t i = 0; i < length; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
		v = v * 10 + (uint32_t)(s[i] - '0');
	}
	if (v < least || v > most) {
		return false;
	}
	*value = v;
	return true;
}

static uint64_t
ms_since(uint64_t start)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return ns / NSEC_PER_MSEC;
}

/* ---- wifi ping [A.B.C.D] [SIZE] -------------------------------------------------------------------------------- */

#define PING_MOST       8000            /* (more than one frame takes: the stack cuts it into fragments, and puts the answer together) */

static void
ping_runs(__unused void *parameter, __unused wait_result_t result)
{
	struct sockaddr_in where = { .sin_len = sizeof(where), .sin_family = AF_INET, .sin_addr = job.to };
	struct timeval wait = { .tv_sec = 1, .tv_usec = 0 };
	size_t size = job.size, room = job.size + 128;
	unsigned answered = 0, seq;
	uint8_t *packet = kalloc_data(size, Z_WAITOK | Z_ZERO), *reply = kalloc_data(room, Z_WAITOK);
	socket_t so = NULL;
	char t[16];
	errno_t error;

	ipv4_text(job.to, t, sizeof(t));
	if (packet == NULL || reply == NULL) {
		console_say("usb: wifi ping %s: no memory for %zu bytes\n", t, size);
		goto out;
	}
	if ((error = sock_socket_internal(PF_INET, SOCK_DGRAM, IPPROTO_ICMP, NULL, NULL, &so)) != 0) {
		console_say("usb: wifi ping %s: no ICMP socket: %d\n", t, error);
		goto out;
	}
	(void)sock_setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
	for (seq = 1; seq <= 4; seq++) {
		struct icmp *icp = (struct icmp *)(void *)packet;
		struct iovec iov = { .iov_base = packet, .iov_len = size };
		struct msghdr msg = { .msg_name = &where, .msg_namelen = sizeof(where), .msg_iov = &iov, .msg_iovlen = 1 };
		uint64_t t0 = mach_absolute_time(), ms;
		size_t done = 0;
		bool got = false;

		bzero(packet, size);
		icp->icmp_type = ICMP_ECHO;
		icp->icmp_id = htons(0x4853);
		icp->icmp_seq = htons((uint16_t)seq);
		for (size_t i = 8; i < size; i++) {
			packet[i] = (uint8_t)i;
		}
		icp->icmp_cksum = icmp_sum(packet, size);
		if ((error = sock_send(so, &msg, 0, &done)) != 0) {
			console_say("usb: wifi ping %s: %u: not sent: %d%s\n", t, seq, error,
			    error == ENETUNREACH || error == EHOSTUNREACH ? " (no route: no address yet?)" : "");
			delay_for_interval(1000, NSEC_PER_MSEC);
			continue;
		}
		do {
			struct iovec in = { .iov_base = reply, .iov_len = room };
			struct msghdr m = { .msg_iov = &in, .msg_iovlen = 1 };
			size_t hl;

			done = 0;
			if (sock_receive(so, &m, 0, &done) != 0) {
				break;  /* the second is up */
			}
			hl = done > 0 && (reply[0] >> 4) == 4 ? (size_t)(reply[0] & 15) * 4 : 0;
			if (done >= hl + 8) {
				const struct icmp *r = (const struct icmp *)(const void *)(reply + hl);

				if (r->icmp_type == ICMP_ECHOREPLY && r->icmp_id == htons(0x4853) && r->icmp_seq == htons((uint16_t)seq)) {
					bool same = done - hl == size && memcmp(reply + hl + 8, packet + 8, size - 8) == 0;
					uint64_t us;

					absolutetime_to_nanoseconds(mach_absolute_time() - t0, &us);
					us /= NSEC_PER_USEC;
					console_say("usb: wifi ping %s: %u: %zu bytes back in %llu.%03llu ms%s\n", t, seq, done - hl,
					    us / 1000, us % 1000, same ? "" : " (NOT WHAT WAS SENT)");
					got = true;
					answered++;
				}
			}
			ms = ms_since(t0);
		} while (!got && ms < 1000);
		if (!got) {
			console_say("usb: wifi ping %s: %u: no answer in 1 s\n", t, seq);
		} else {
			delay_for_interval(200, NSEC_PER_MSEC);
		}
	}
	console_say("usb: wifi ping %s: %u of 4 answered (%zu bytes each)\n", t, answered, size);
	sock_close(so);
out:
	if (packet != NULL) {
		kfree_data(packet, size);
	}
	if (reply != NULL) {
		kfree_data(reply, room);
	}
	console_job_end();
}

void
hakios_net_console_ping(const char *arg)
{
	const char *word;
	size_t length;
	char via[IFNAMSIZ + 4];

	job.to.s_addr = 0;
	job.size = 64;
	while (*arg != 0) {
		length = next_word(&arg, &word);
		if (ipv4_parse(word, length, &job.to)) {
			continue;
		}
		if (!number_parse(word, length, 28, PING_MOST, &job.size)) {
			console_say("usb: wifi ping [A.B.C.D] [SIZE]: an IPv4 address and the ICMP message's size, 28 to %u bytes "
			    "(64 if not given)\n", PING_MOST);
			return;
		}
	}
	if (job.to.s_addr == 0 && !default_gateway(&job.to, via, sizeof(via))) {
		console_say("usb: wifi ping: no default route yet (wifi ip); or give an address: wifi ping A.B.C.D\n");
		return;
	}
	(void)console_job_start(ping_runs, "ping");
}

/* ---- wifi dns NAME [A.B.C.D] ----------------------------------------------------------------------------------- */

/* the DNS server DHCP gave, else the default route's gateway; false without either */
static bool
dns_server(struct in_addr *server)
{
	char via[IFNAMSIZ + 4];

	if (net_seen.dns_count != 0) {
		*server = net_seen.dns[0];
		return true;
	}
	return default_gateway(server, via, sizeof(via));
}

/*
 * One question, for the IPv4 addresses of `name`, to `server` on UDP port 53 (three tries, 2 s each): 0 and the
 * answer's code, its number of answers and up to `room` addresses; or an errno (ETIMEDOUT: no answer).
 */
static errno_t
dns_ask(const char *name, struct in_addr server, struct in_addr *a, unsigned room, unsigned *count, unsigned *rcode,
    unsigned *answers, uint64_t *ms)
{
	struct sockaddr_in where = { .sin_len = sizeof(where), .sin_family = AF_INET, .sin_port = htons(53), .sin_addr = server };
	struct timeval wait = { .tv_sec = 2, .tv_usec = 0 };
	uint16_t id = (uint16_t)(mach_absolute_time() & 0xffff);
	uint8_t question[300], answer[1500];
	const char *p = name;
	socket_t so = NULL;
	size_t n = 12;
	errno_t error;

	bzero(question, 12);
	question[0] = (uint8_t)(id >> 8);
	question[1] = (uint8_t)id;
	question[2] = 0x01;             /* recursion wanted */
	question[5] = 1;                /* one question */
	while (*p != 0) {
		size_t length = 0;

		while (p[length] != 0 && p[length] != '.') {
			length++;
		}
		if (length == 0 || length > 63 || n + 1 + length + 5 > sizeof(question)) {
			return EINVAL;
		}
		question[n++] = (uint8_t)length;
		memcpy(question + n, p, length);
		n += length;
		p += length;
		if (*p == '.') {
			p++;
		}
	}
	if (n == 12) {
		return EINVAL;
	}
	question[n++] = 0;
	question[n++] = 0;
	question[n++] = 1;              /* A */
	question[n++] = 0;
	question[n++] = 1;              /* IN */
	if ((error = sock_socket_internal(PF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, NULL, &so)) != 0) {
		return error;
	}
	(void)sock_setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
	error = ETIMEDOUT;
	for (unsigned attempt = 0; attempt < 3 && error == ETIMEDOUT; attempt++) {
		struct iovec iov = { .iov_base = question, .iov_len = n };
		struct msghdr msg = { .msg_name = &where, .msg_namelen = sizeof(where), .msg_iov = &iov, .msg_iovlen = 1 };
		uint64_t t0 = mach_absolute_time();
		size_t done = 0;
		errno_t e;

		if ((e = sock_send(so, &msg, 0, &done)) != 0) {
			error = e;
			break;
		}
		for (;;) {
			struct iovec in = { .iov_base = answer, .iov_len = sizeof(answer) };
			struct msghdr m = { .msg_iov = &in, .msg_iovlen = 1 };

			done = 0;
			if (sock_receive(so, &m, 0, &done) != 0) {
				break;          /* two seconds: ask again */
			}
			if (done >= 12 && answer[0] == question[0] && answer[1] == question[1] &&
			    dns_answer(answer, done, rcode, answers, a, room, count, NULL, 0)) {
				*ms = ms_since(t0);
				error = 0;
				break;
			}
		}
	}
	sock_close(so);
	return error;
}

static void
dns_runs(__unused void *parameter, __unused wait_result_t result)
{
	struct in_addr a[4];
	unsigned count = 0, rcode = 0, answers = 0;
	uint64_t ms = 0;
	char s[16], list[4 * 17] = "";
	errno_t error;

	ipv4_text(job.to, s, sizeof(s));
	error = dns_ask(job.name, job.to, a, 4, &count, &rcode, &answers, &ms);
	if (error != 0) {
		console_say("usb: wifi dns %s: %s %s: %d\n", job.name, error == ETIMEDOUT ? "no answer in 6 s from" : "not asked of", s,
		    error);
	} else {
		for (unsigned i = 0; i < count; i++) {
			size_t used = strlen(list);

			ipv4_text(a[i], list + used + (used ? 1 : 0), sizeof(list) - used - 1);
			if (used) {
				list[used] = ' ';
			}
		}
		console_say("usb: wifi dns %s: %s answers in %llu ms: code %u%s, %u answer%s%s%s\n", job.name, s, ms, rcode,
		    rcode == 3 ? " (no such name)" : rcode ? " (an error)" : "", answers, answers == 1 ? "" : "s", count ? ": " : "",
		    list);
	}
	console_job_end();
}

void
hakios_net_console_dns(const char *arg)
{
	const char *word;
	size_t length;

	length = next_word(&arg, &word);
	if (length == 0 || length >= sizeof(job.name)) {
		console_say("usb: wifi dns NAME [A.B.C.D]: NAME's IPv4 addresses, asked of the DNS server given (by default the one "
		    "DHCP gave, else the gateway)\n");
		return;
	}
	memcpy(job.name, word, length);
	job.name[length] = 0;
	length = next_word(&arg, &word);
	if (length != 0 ? !ipv4_parse(word, length, &job.to) : !dns_server(&job.to)) {
		console_say("usb: wifi dns %s: %s\n", job.name, length ? "the server is to be an IPv4 address" :
		    "no DNS server known yet, and no default route (wifi ip)");
		return;
	}
	(void)console_job_start(dns_runs, "dns");
}

/* ---- wifi tcp HOST [PORT [PATH]] -------------------------------------------------------------------------------- */

#define TCP_MOST_MS     60000

static void
tcp_runs(__unused void *parameter, __unused wait_result_t result)
{
	struct sockaddr_in where = { .sin_len = sizeof(where), .sin_family = AF_INET, .sin_port = htons(job.port) };
	struct timeval connect_wait = { .tv_sec = 10, .tv_usec = 0 }, wait = { .tv_sec = 5, .tv_usec = 0 };
	struct tcpstat before = tcpstat;
	char t[16], request[300], status[100] = "", *buffer = NULL;
	uint64_t t0, connected_ms, ms, bytes = 0, reads = 0;
	const char *ended = "the server closed it";
	socket_t so = NULL;
	size_t done;
	errno_t error;

	if (job.to.s_addr == 0) {
		struct in_addr server, a[1];
		unsigned count = 0, rcode = 0, answers = 0;

		if (!dns_server(&server)) {
			console_say("usb: wifi tcp %s: no DNS server known to look the name up with\n", job.name);
			goto out;
		}
		error = dns_ask(job.name, server, a, 1, &count, &rcode, &answers, &ms);
		if (error != 0 || count == 0) {
			console_say("usb: wifi tcp %s: the name not found (error %d, code %u, %u answers)\n", job.name, error, rcode, answers);
			goto out;
		}
		job.to = a[0];
		ipv4_text(job.to, t, sizeof(t));
		console_say("usb: wifi tcp %s: is %s (DNS, %llu ms)\n", job.name, t, ms);
	}
	ipv4_text(job.to, t, sizeof(t));
	where.sin_addr = job.to;
	if ((buffer = kalloc_data(4096, Z_WAITOK)) == NULL ||
	    (error = sock_socket_internal(PF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, &so)) != 0) {
		console_say("usb: wifi tcp %s: no memory or no socket\n", t);
		goto out;
	}
	t0 = mach_absolute_time();
	error = sock_connect(so, (struct sockaddr *)&where, MSG_DONTWAIT);
	if (error == EINPROGRESS) {
		error = sock_connectwait(so, &connect_wait);
	}
	if (error == 0) {
		int e = 0, length = sizeof(e);

		(void)sock_getsockopt(so, SOL_SOCKET, SO_ERROR, &e, &length);
		error = e;
	}
	connected_ms = ms_since(t0);
	if (error != 0) {
		console_say("usb: wifi tcp %s port %u: no connection after %llu ms: %d%s\n", t, job.port, connected_ms, error,
		    error == EWOULDBLOCK ? " (no answer to the SYN)" : error == ECONNREFUSED ? " (refused)" : "");
		goto out;
	}
	console_say("usb: wifi tcp %s port %u: connected in %llu ms; GET %s\n", t, job.port, connected_ms, job.path);
	snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: hakios-console\r\nAccept: */*\r\n"
	    "Connection: close\r\n\r\n", job.path, job.name);
	{
		struct iovec iov = { .iov_base = request, .iov_len = strlen(request) };
		struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };

		done = 0;
		if ((error = sock_send(so, &msg, 0, &done)) != 0) {
			console_say("usb: wifi tcp %s: the request not sent: %d\n", t, error);
			goto out;
		}
	}
	(void)sock_setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
	for (;;) {
		struct iovec in = { .iov_base = buffer, .iov_len = 4096 };
		struct msghdr m = { .msg_iov = &in, .msg_iovlen = 1 };

		done = 0;
		error = sock_receive(so, &m, 0, &done);
		if (error != 0) {
			ended = error == EWOULDBLOCK ? "NOTHING CAME FOR 5 s" : "an error";
			break;
		}
		if (done == 0) {
			break;
		}
		if (bytes == 0) {
			size_t i = 0;

			while (i < done && i < sizeof(status) - 1 && buffer[i] != '\r' && buffer[i] != '\n') {
				status[i] = buffer[i] >= 0x20 && buffer[i] < 0x7f ? buffer[i] : '?';
				i++;
			}
			status[i] = 0;
		}
		bytes += done;
		reads++;
		if (ms_since(t0) > TCP_MOST_MS) {
			ended = "stopped after 60 s";
			break;
		}
	}
	ms = ms_since(t0);
	console_say("usb: wifi tcp %s: \"%s\": %llu bytes in %llu ms (%llu KB/s, %llu reads); %s (%d)\n", t, status, bytes, ms,
	    ms ? bytes / ms : 0, reads, ended, error);
	console_say("usb: wifi tcp %s: meanwhile TCP had %u packets in order (%u bytes), %u with a bad checksum, %u duplicate, "
	    "%u out of order; sent %u again, %u retransmit timeouts\n", t, tcpstat.tcps_rcvpack - before.tcps_rcvpack,
	    tcpstat.tcps_rcvbyte - before.tcps_rcvbyte, tcpstat.tcps_rcvbadsum - before.tcps_rcvbadsum,
	    tcpstat.tcps_rcvduppack - before.tcps_rcvduppack, tcpstat.tcps_rcvoopack - before.tcps_rcvoopack,
	    tcpstat.tcps_sndrexmitpack - before.tcps_sndrexmitpack, tcpstat.tcps_rexmttimeo - before.tcps_rexmttimeo);
out:
	if (so != NULL) {
		sock_close(so);
	}
	if (buffer != NULL) {
		kfree_data(buffer, 4096);
	}
	console_job_end();
}

void
hakios_net_console_tcp(const char *arg)
{
	const char *word;
	size_t length;
	uint32_t port = 80;

	length = next_word(&arg, &word);
	if (length == 0 || length >= sizeof(job.name)) {
		goto usage;
	}
	memcpy(job.name, word, length);
	job.name[length] = 0;
	if (!ipv4_parse(word, length, &job.to)) {
		job.to.s_addr = 0;      /* a name: DNS first */
	}
	length = next_word(&arg, &word);
	if (length != 0 && !number_parse(word, length, 1, 65535, &port)) {
		goto usage;
	}
	job.port = (uint16_t)port;
	length = next_word(&arg, &word);
	if (length >= sizeof(job.path) || (length != 0 && word[0] != '/')) {
		goto usage;
	}
	memcpy(job.path, length ? word : "/", length ? length : 1);
	job.path[length ? length : 1] = 0;
	(void)console_job_start(tcp_runs, "tcp");
	return;
usage:
	console_say("usb: wifi tcp HOST [PORT [PATH]]: a TCP connection to HOST (A.B.C.D, or a name for DNS), port 80 by "
	    "default, an HTTP GET of PATH (/), and the bytes that come back\n");
}

/* ---- wifi names [off] ------------------------------------------------------------------------------------------- */

/*
 * What boot-arg net_names=1 says -- every name asked of a DNS server and its answer, every TCP connection begun --
 * on the USB console as well as in the kernel's log, from now on (and the count of NAMES_MAX lines begun again): to
 * watch what Safari or any other program does with the network while one uses it. "off" stops it.
 */
void
hakios_net_console_names(const char *arg)
{
	if (strcmp(arg, "off") == 0) {
		net_names_console = false;
		console_say("usb: wifi names: off (the kernel's log keeps them while boot-arg net_names=1)\n");
		return;
	}
	net_names = 1;
	net_names_said = 0;
	net_names_console = true;
	console_say("usb: wifi names: every DNS question and answer and every TCP connection begun, here as they come "
	    "(wifi names off to stop)\n");
}

/* ---- wifi stat ---------------------------------------------------------------------------------------------------- */

void
hakios_net_console_stat(void)
{
	struct net_if *n = net_if_unit(0);
	struct ifnet_stats_param s = { 0 };
	char first[16], second[16], line[160] = "";

	if (n == NULL) {
		console_say("usb: wifi stat: there is no en0\n");
		return;
	}
	(void)ifnet_stat(n->ifp, &s);
	console_say("usb: wifi stat: en0 link %s; sent %llu frames (%llu bytes), received %llu (%llu bytes); dropped %llu out, %llu in\n",
	    n->up ? "up" : "down", n->count.sent, n->count.sent_bytes, n->count.received, n->count.received_bytes,
	    n->count.dropped_out, n->count.dropped_in);
	console_say("usb: wifi stat: en0 in by size: %llu <= 128, %llu <= 576, %llu <= 1200, %llu <= 1514, %llu bigger (the last %u); "
	    "the biggest %u\n", net_seen.in[0], net_seen.in[1], net_seen.in[2], net_seen.in[3], net_seen.in_too_big,
	    net_seen.in_too_big_last, net_seen.in_biggest);
	console_say("usb: wifi stat: en0 out by size: %llu <= 128, %llu <= 576, %llu <= 1200, %llu <= 1514; the biggest %u\n",
	    net_seen.out[0], net_seen.out[1], net_seen.out[2], net_seen.out[3], net_seen.out_biggest);
	console_say("usb: wifi stat: en0 in by kind: %llu IPv4, %llu IPv6, %llu ARP, %llu other; %llu TCP, %llu UDP, %llu from DNS servers\n",
	    net_seen.in_ipv4, net_seen.in_ipv6, net_seen.in_arp, net_seen.in_other, net_seen.in_tcp, net_seen.in_udp,
	    net_seen.in_dns);
	console_say("usb: wifi stat: the interface's own count: %llu packets in (%llu bytes, %llu errors), %llu out (%llu bytes, "
	    "%llu errors), %llu dropped, %llu of no protocol\n", s.packets_in, s.bytes_in, s.errors_in, s.packets_out,
	    s.bytes_out, s.errors_out, s.dropped, s.no_protocol);
	console_say("usb: wifi stat: IP: %u in, %u bad checksum, %u too short or bad length, %u fragments (%u put together), "
	    "%u delivered\n", ipstat.ips_total, ipstat.ips_badsum, ipstat.ips_tooshort + ipstat.ips_badlen, ipstat.ips_fragments,
	    ipstat.ips_reassembled, ipstat.ips_delivered);
	console_say("usb: wifi stat: UDP: %u in, %u bad checksum, %u to no socket\n", udpstat.udps_ipackets, udpstat.udps_badsum,
	    udpstat.udps_noport);
	console_say("usb: wifi stat: TCP: %u connections tried, %u made, %u dropped; in: %u packets in order (%u bytes), %u bad "
	    "checksum, %u duplicate, %u out of order; out: %u packets, %u sent again, %u retransmit timeouts\n",
	    tcpstat.tcps_connattempt, tcpstat.tcps_connects, tcpstat.tcps_conndrops + tcpstat.tcps_drops, tcpstat.tcps_rcvpack,
	    tcpstat.tcps_rcvbyte, tcpstat.tcps_rcvbadsum, tcpstat.tcps_rcvduppack, tcpstat.tcps_rcvoopack, tcpstat.tcps_sndtotal,
	    tcpstat.tcps_sndrexmitpack, tcpstat.tcps_rexmttimeo);
	ipv4_text(net_seen.dns[0], first, sizeof(first));
	ipv4_text(net_seen.dns[1], second, sizeof(second));
	if (n->link->counts != NULL) {
		n->link->counts(line, sizeof(line));
	}
	console_say("usb: wifi stat: DHCP's DNS server%s: %s%s%s; %s\n", net_seen.dns_count > 1 ? "s" : "",
	    net_seen.dns_count ? first : "none seen", net_seen.dns_count > 1 ? " " : "", net_seen.dns_count > 1 ? second : "",
	    line[0] ? line : "(the link says nothing of itself)");
}
