/*
 * Darwin-on-hakios: what stands between the two sides of the station boundary
 * (sys/hakios.sta.h).
 *
 * Whoever wants a network asks from wherever it happens to be: an ioctl of wifid's, the thread
 * that brings the radio up. The station side wants to be called by one thread, one call at a
 * time, and a scan or a join takes seconds. So what is asked for is noted here and done by a
 * thread of this file's; the asker returns at once and hears the outcome as an event. One
 * request of each kind can wait: a second scan asked for while the first has not begun
 * replaces it, as does a second join.
 *
 * What the last scan heard is kept here, so that it can be read without entering the station
 * side, and so is the list of channels.
 *
 * The network the kernel was given in a file is a standing request: joined at the start, and
 * again whenever the radio is turned back on (Settings' switch), until somebody asks for
 * another network -- as Darwin itself rejoins the networks it knows. If that other network cannot
 * be joined (a wrong passphrase), the standing one is joined again.
 */
#include <stdarg.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/hakios.input.h>
#include <sys/hakios.sta.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

extern void thread_set_thread_name(thread_t thread, const char *name);
extern void loader_log_line(const char *line);
extern kern_return_t thread_terminate(thread_t thread);
extern vnode_t rootvnode;

#define HEARD_MOST      128
#define CHANNELS_MOST   96

static LCK_GRP_DECLARE(sta_lock_group, "hakios-sta");
static LCK_MTX_DECLARE(sta_lock, &sta_lock_group);

static const struct hakios_sta_ops *sta_ops;   /* the station side, while there is one */
static bool sta_thread_made;
static bool sta_calls;                          /* the thread is in a call of the station side's */
static unsigned sta_reads;                      /* ... and so many others are in its state() */
static bool sta_on;                             /* the radio's power, as last asked for */

/* what has been asked for and not begun */
static bool asked_power, asked_power_on, asked_scan, asked_join, asked_leave;
static struct hakios_sta_scan asked_scan_what;
static struct hakios_sta_join asked_join_what;
static struct hakios_sta_join standing;        /* the network joined whenever the radio comes on (ssid_length 0: none) */
static struct hakios_sta_join fallback;        /* ... as it was before a join that has not come through yet */
static bool auto_join_pending;                 /* scan first, then standing, once after power/attach */

#define NETWORK_PATH    "/private/var/hakios-wifi"

static void
network_save(const struct hakios_sta_join *what)
{
	vfs_context_t ctx;
	vnode_t vnode = NULLVP;
	char body[160];
	int n, left = 0, error;

	if (what == NULL || what->ssid_length == 0 || rootvnode == NULLVP) {
		return;
	}
	n = 0;
	memcpy(body, what->ssid, what->ssid_length);
	n += what->ssid_length;
	body[n++] = '\n';
	if (what->passphrase_length != 0 && what->passphrase_length <= 63) {
		memcpy(body + n, what->passphrase, what->passphrase_length);
		n += what->passphrase_length;
	} else {
		static const char hex[] = "0123456789abcdef";
		unsigned i;

		for (i = 0; i < 32; i++) {
			body[n++] = hex[what->pmk[i] >> 4];
			body[n++] = hex[what->pmk[i] & 0xf];
		}
	}
	body[n++] = '\n';
	ctx = vfs_context_kernel();
	error = vnode_open(NETWORK_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600, 0, &vnode, ctx);
	if (error != 0) {
		hakios_wifi_tell("wifi: standing network not saved (open %d)\n", error);
		return;
	}
	error = vn_rdwr(UIO_WRITE, vnode, body, n, 0, UIO_SYSSPACE, IO_SYNC, vfs_context_ucred(ctx), &left,
	    vfs_context_proc(ctx));
	(void)vnode_close(vnode, FWASWRITTEN, ctx);
	memset_s(body, sizeof(body), 0, sizeof(body));
	hakios_wifi_tell("wifi: standing network %s (%u-byte name)\n", error == 0 && left == 0 ? "saved" : "not written",
	    what->ssid_length);
}

static bool
network_load(struct hakios_sta_join *what)
{
	vfs_context_t ctx;
	vnode_t vnode = NULLVP;
	char body[160];
	int n, left = 0, i;
	char *nl, *nl2;

	if (what == NULL || rootvnode == NULLVP) {
		return false;
	}
	bzero(what, sizeof(*what));
	ctx = vfs_context_kernel();
	if (vnode_lookup(NETWORK_PATH, 0, &vnode, ctx) != 0) {
		return false;
	}
	n = (int)sizeof(body) - 1;
	if (vn_rdwr(UIO_READ, vnode, body, n, 0, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &left, vfs_context_proc(ctx)) != 0) {
		vnode_put(vnode);
		return false;
	}
	vnode_put(vnode);
	n -= left;
	body[n < 0 ? 0 : n] = 0;
	nl = strchr(body, '\n');
	if (nl == NULL || nl == body || (unsigned)(nl - body) > 32) {
		memset_s(body, sizeof(body), 0, sizeof(body));
		return false;
	}
	what->ssid_length = (uint8_t)(nl - body);
	memcpy(what->ssid, body, what->ssid_length);
	nl2 = strchr(nl + 1, '\n');
	n = (int)((nl2 != NULL ? nl2 : body + strlen(body)) - (nl + 1));
	if (n >= 8 && n <= 63) {
		what->security = HAKIOS_STA_WPA2_PSK;
		what->passphrase_length = (uint8_t)n;
		memcpy(what->passphrase, nl + 1, (unsigned)n);
	} else if (n == 64) {
		what->security = HAKIOS_STA_WPA2_PSK;
		for (i = 0; i < 32; i++) {
			unsigned hi = (unsigned char)nl[1 + 2 * i], lo = (unsigned char)nl[2 + 2 * i];

			hi = hi >= 'a' ? hi - 'a' + 10 : hi >= 'A' ? hi - 'A' + 10 : hi - '0';
			lo = lo >= 'a' ? lo - 'a' + 10 : lo >= 'A' ? lo - 'A' + 10 : lo - '0';
			what->pmk[i] = (uint8_t)((hi << 4) | lo);
		}
	} else if (n == 0) {
		what->security = HAKIOS_STA_OPEN;
	} else {
		bzero(what, sizeof(*what));
		memset_s(body, sizeof(body), 0, sizeof(body));
		return false;
	}
	memset_s(body, sizeof(body), 0, sizeof(body));
	hakios_wifi_tell("wifi: standing network loaded (%u-byte name)\n", what->ssid_length);
	return true;
}

static struct hakios_sta_bss *heard;           /* by the last scan that came to an end */
static unsigned heard_count;
static struct hakios_sta_channel channels[CHANNELS_MOST];
static unsigned channel_count;

static void
heard_one(const struct hakios_sta_bss *bss, __unused void *context)
{
	/* (only the thread writes the list, and it reads the count without the lock) */
	if (heard_count < HEARD_MOST) {
		lck_mtx_lock(&sta_lock);
		heard[heard_count++] = *bss;
		lck_mtx_unlock(&sta_lock);
	}
}

static void
sta_runs(__unused void *parameter, __unused wait_result_t result)
{
	static struct hakios_sta_scan scan;
	static struct hakios_sta_join join;

	for (;;) {
		const struct hakios_sta_ops *ops;
		bool power = false, power_on = false, leave = false, do_join = false, do_scan = false;
		int error = 0;

		lck_mtx_lock(&sta_lock);
		while (sta_ops == NULL || !(asked_power || asked_leave || asked_join || asked_scan)) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_ops, THREAD_UNINT);
		}
		ops = sta_ops;
		/* one thing at a time, in the order that makes sense when several wait */
		if (asked_power) {
			power = true;
			power_on = asked_power_on;
			asked_power = false;
		} else if (asked_leave) {
			leave = true;
			asked_leave = false;
		} else if (asked_join) {
			do_join = true;
			join = asked_join_what;
			bzero(&asked_join_what, sizeof(asked_join_what));
			asked_join = false;
		} else {
			do_scan = true;
			scan = asked_scan_what;
			asked_scan = false;
		}
		sta_calls = true;
		lck_mtx_unlock(&sta_lock);

		if (power) {
			struct hakios_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);
			bool was_joined = false;

			if (state != NULL) {
				ops->state(state);
				was_joined = state->phase == HAKIOS_STA_JOINED;
				kfree_data(state, sizeof(*state));
			}
			error = ops->power(power_on);
			if (!power_on && was_joined) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 0);
			}
			hakios_apple80211_event(HAKIOS_STA_POWER, error == 0 ? power_on : !power_on);
			lck_mtx_lock(&sta_lock);
			if (error == 0 && power_on && standing.ssid_length != 0 && !asked_join && !asked_scan) {
				auto_join_pending = true;
				bzero(&asked_scan_what, sizeof(asked_scan_what));
				asked_scan = true;
			}
			lck_mtx_unlock(&sta_lock);
		} else if (leave) {
			ops->leave();
			hakios_apple80211_event(HAKIOS_STA_LINK, 0);
		} else if (do_join) {
			error = ops->join(&join);
			if (error == 0) {
				lck_mtx_lock(&sta_lock);
				standing = join;
				lck_mtx_unlock(&sta_lock);
				network_save(&join);
			}
			bzero(&join, sizeof(join));
			hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, error);
			if (error == 0) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 1);
			}
			/* a join that did not come through (a wrong passphrase) leaves things as they were: the standing network again */
			lck_mtx_lock(&sta_lock);
			if (error != 0 && fallback.ssid_length != 0 && !asked_join && !asked_leave && !asked_power) {
				standing = asked_join_what = fallback;
				asked_join = true;
			}
			bzero(&fallback, sizeof(fallback));
			lck_mtx_unlock(&sta_lock);
		} else if (do_scan) {
			error = ops->scan(&scan);
			lck_mtx_lock(&sta_lock);
			heard_count = 0;
			lck_mtx_unlock(&sta_lock);
			ops->heard(heard_one, NULL);
			hakios_apple80211_event(HAKIOS_STA_SCAN_DONE, error);
			lck_mtx_lock(&sta_lock);
			if (auto_join_pending) {
				auto_join_pending = false;
				if (standing.ssid_length != 0 && !asked_join && !asked_leave && !asked_power) {
					asked_join_what = standing;
					asked_join = true;
				}
			}
			lck_mtx_unlock(&sta_lock);
		}

		lck_mtx_lock(&sta_lock);
		sta_calls = false;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

/* ---- the station side ---------------------------------------------------------------------------------- */

void
hakios_sta_attach(const struct hakios_sta_ops *ops)
{
	thread_t thread;

	if (ops == NULL) {
		lck_mtx_lock(&sta_lock);
		sta_ops = NULL;
		asked_power = asked_scan = asked_join = asked_leave = false;
		while (sta_calls || sta_reads != 0) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_calls, THREAD_UNINT);
		}
		heard_count = channel_count = 0;
		lck_mtx_unlock(&sta_lock);
		return;
	}
	if (heard == NULL) {
		heard = kalloc_data(HEARD_MOST * sizeof(*heard), Z_WAITOK | Z_ZERO);
	}
	if (heard == NULL) {
		hakios_wifi_tell("wifi: no memory for what a scan hears: nobody can ask the radio for a network\n");
		return;
	}
	if (!sta_thread_made) {
		if (kernel_thread_start(sta_runs, NULL, &thread) != KERN_SUCCESS) {
			hakios_wifi_tell("wifi: no thread for the station: nobody can ask the radio for a network\n");
			return;
		}
		thread_set_thread_name(thread, "wifi-station");
		thread_deallocate(thread);
		sta_thread_made = true;
	}
	channel_count = ops->channels(channels, CHANNELS_MOST);
	{
		struct hakios_sta_join saved;

		bzero(&saved, sizeof(saved));
		(void)network_load(&saved);
		lck_mtx_lock(&sta_lock);
		sta_ops = ops;
		sta_on = true;
		if (standing.ssid_length == 0 && saved.ssid_length != 0 && !asked_join) {
			standing = saved;
			auto_join_pending = true;
			bzero(&asked_scan_what, sizeof(asked_scan_what));
			asked_scan = true;
		}
		lck_mtx_unlock(&sta_lock);
		memset_s(&saved, sizeof(saved), 0, sizeof(saved));
	}
	thread_wakeup((event_t)&sta_ops);
	hakios_apple80211_event(HAKIOS_STA_POWER, 1);
}

void
hakios_sta_lost(int error)
{
	hakios_apple80211_event(HAKIOS_STA_LINK, error != 0 ? error : ECONNRESET);
}

/* ---- who wants a network ------------------------------------------------------------------------------- */

bool
hakios_sta_present(void)
{
	return sta_ops != NULL;
}

bool
hakios_sta_powered(void)
{
	return sta_ops != NULL && sta_on;
}

bool
hakios_sta_busy(void)
{
	bool busy;

	lck_mtx_lock(&sta_lock);
	busy = sta_ops != NULL && (sta_calls || asked_power || asked_leave || asked_join || asked_scan);
	lck_mtx_unlock(&sta_lock);
	return busy;
}

int
hakios_sta_ask_power(bool on)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	sta_on = on;
	asked_power = true;
	asked_power_on = on;
	if (!on) {
		asked_scan = asked_join = asked_leave = false;
	}
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_scan(const struct hakios_sta_scan *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	asked_scan_what = *what;
	asked_scan = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_join_standing(const struct hakios_sta_join *what)
{
	int error = hakios_sta_ask_join(what);

	lck_mtx_lock(&sta_lock);
	if (error == 0) {
		standing = *what;
	}
	lck_mtx_unlock(&sta_lock);
	return error;
}

int
hakios_sta_ask_join(const struct hakios_sta_join *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	if (standing.ssid_length != 0) {
		fallback = standing;
	}
	bzero(&standing, sizeof(standing));     /* (whoever asks for a network now has the say) */
	asked_join_what = *what;
	asked_join = true;
	asked_leave = false;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_leave(void)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	asked_leave = true;
	asked_join = false;
	auto_join_pending = false;
	bzero(&asked_join_what, sizeof(asked_join_what));
	bzero(&fallback, sizeof(fallback));
	bzero(&standing, sizeof(standing));
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

bool
hakios_sta_heard(unsigned index, struct hakios_sta_bss *bss)
{
	bool there;

	lck_mtx_lock(&sta_lock);
	if ((there = index < heard_count)) {
		*bss = heard[index];
	}
	lck_mtx_unlock(&sta_lock);
	return there;
}

void
hakios_sta_state(struct hakios_sta_state *state)
{
	const struct hakios_sta_ops *ops;

	bzero(state, sizeof(*state));
	lck_mtx_lock(&sta_lock);
	if ((ops = sta_ops) != NULL) {
		sta_reads++;
	}
	lck_mtx_unlock(&sta_lock);
	if (ops != NULL) {
		ops->state(state);
		lck_mtx_lock(&sta_lock);
		sta_reads--;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

unsigned
hakios_sta_channels(struct hakios_sta_channel *out, unsigned most)
{
	unsigned count;

	lck_mtx_lock(&sta_lock);
	count = channel_count < most ? channel_count : most;
	memcpy(out, channels, count * sizeof(*out));
	lck_mtx_unlock(&sta_lock);
	return count;
}

bool
hakios_sta_address(uint8_t *address)
{
	const struct hakios_sta_ops *ops = sta_ops;

	if (ops != NULL) {
		memcpy(address, ops->address, 6);
	}
	return ops != NULL;
}

/* ---- the USB console's "wifi scan" ------------------------------------------------------------------------ */

static volatile bool console_scanning;

static void
console_say(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	loader_log_line(line);
}

/* what is asked of the station and what it heard, as anybody asks: power if it is off, a scan of every channel */
static void
console_scan_runs(__unused void *parameter, __unused wait_result_t result)
{
	static struct hakios_sta_scan what;
	static struct hakios_sta_bss bss;
	unsigned waited, i;
	int error;

	if (!hakios_sta_present()) {
		console_say("usb: wifi scan: no station offered (the radio's driver did not get to brcmfmac=7)\n");
		goto out;
	}
	if (!hakios_sta_powered()) {
		console_say("usb: wifi scan: the radio is off: it is turned on first\n");
		if ((error = hakios_sta_ask_power(true)) != 0) {
			console_say("usb: wifi scan: turning it on fails: %d\n", error);
			goto out;
		}
		for (waited = 0; hakios_sta_busy() && waited < 200; waited++) {
			delay_for_interval(100, NSEC_PER_MSEC);
		}
	}
	bzero(&what, sizeof(what));		/* (no name, every channel, asking) */
	if ((error = hakios_sta_ask_scan(&what)) != 0) {
		console_say("usb: wifi scan: the station refuses the scan: %d\n", error);
		goto out;
	}
	console_say("usb: wifi scan: asked; the answer in a few seconds\n");
	for (waited = 0; hakios_sta_busy() && waited < 300; waited++) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	console_say("usb: wifi scan: %s after %u.%u s; heard:\n", hakios_sta_busy() ? "STILL UNDER WAY" : "over", waited / 10,
	    waited % 10);
	for (i = 0; hakios_sta_heard(i, &bss); i++) {
		char name[33];
		unsigned n;

		for (n = 0; n < bss.ssid_length && n < 32; n++) {
			name[n] = bss.ssid[n] >= 0x20 && bss.ssid[n] < 0x7f ? (char)bss.ssid[n] : '?';
		}
		name[n] = 0;
		console_say("usb: wifi scan: %2u  %02x:%02x:%02x:%02x:%02x:%02x  %4u MHz  %4d dBm  \"%s\"\n", i, bss.bssid[0],
		    bss.bssid[1], bss.bssid[2], bss.bssid[3], bss.bssid[4], bss.bssid[5], bss.frequency, bss.rssi,
		    n != 0 ? name : "(hidden)");
	}
	console_say("usb: wifi scan: %u access points\n", i);
out:
	console_scanning = false;
	thread_terminate(current_thread());
}

void
hakios_sta_console_scan(void)
{
	thread_t thread;

	if (console_scanning) {
		console_say("usb: wifi scan: one is under way\n");
		return;
	}
	console_scanning = true;
	if (kernel_thread_start(console_scan_runs, NULL, &thread) != KERN_SUCCESS) {
		console_scanning = false;
		console_say("usb: wifi scan: no thread for it\n");
		return;
	}
	thread_set_thread_name(thread, "sta-console-scan");
	thread_deallocate(thread);
}
