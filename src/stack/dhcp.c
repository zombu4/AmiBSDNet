/*
 * AmiBSDNet: DHCP client (RFC 2131).
 *
 * Before an interface has an address the kernel cannot send or receive
 * DHCP traffic, so the exchange is done at the frame level through the
 * SANA-II backend (like BSD DHCP clients use BPF): frames are built here
 * and sent raw; replies are caught by a receive tap before they reach the
 * kernel.  The lease is then applied to the kernel interface.
 *
 * Each DHCP interface has a client thread that obtains, renews and, when
 * the link goes away, releases the lease (see dhcp_thread()).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "rumpuser_amiga.h"
#include "sana2_host.h"
#include "stack.h"

extern struct ExecBase *SysBase;

#define	P	amiga_rump_printf

#define	DHCPDISCOVER	1
#define	DHCPOFFER	2
#define	DHCPREQUEST	3
#define	DHCPACK		5
#define	DHCPNAK		6

#define	OPT_PAD		0
#define	OPT_SUBNET	1
#define	OPT_ROUTER	3
#define	OPT_DNS		6
#define	OPT_HOSTNAME	12
#define	OPT_DOMAIN	15
#define	OPT_REQADDR	50
#define	OPT_LEASE	51
#define	OPT_MSGTYPE	53
#define	OPT_SERVERID	54
#define	OPT_PARAMS	55
#define	OPT_T1		58
#define	OPT_CLIENTID	61
#define	OPT_END		255

#define	BOOTP_LEN	236
#define	FRAME_HDRS	(14 + 20 + 8)

struct lease {
	ULONG addr, mask, router, server;
	ULONG dns[4];
	int ndns;
	char domain[64];
	ULONG leasetime, t1;
};

struct dhcpctx {
	struct iface *ifc;
	struct virtif_user *viu;
	UBYTE mac[6];
	ULONG xid;
	struct Task *task;
	ULONG generation;		/* configuration this client belongs to */

	/* filled by the tap (on the I/O process) */
	volatile int have_reply;
	UBYTE reply[600];
	volatile ULONG replylen;
};

static ULONG
get32(const UBYTE *p)
{

	return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | (p[2] << 8) | p[3];
}

static void
put32(UBYTE *p, ULONG v)
{

	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static UWORD
ip_checksum(const UBYTE *p, int len)
{
	ULONG sum = 0;

	while (len > 1) {
		sum += (p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (UWORD)~sum;
}

/* runs on the interface's I/O process: grab our BOOTP replies */
static int
dhcp_tap(void *arg, const UBYTE *f, ULONG len)
{
	struct dhcpctx *c = arg;
	const UBYTE *ip = f + 14, *udp, *bootp;
	ULONG ihl, blen;

	if (len < FRAME_HDRS + BOOTP_LEN || f[12] != 0x08 || f[13] != 0x00)
		return 0;
	ihl = (ip[0] & 15) * 4;
	if ((ip[0] >> 4) != 4 || ip[9] != 17 || len < 14 + ihl + 8)
		return 0;
	udp = ip + ihl;
	if (udp[2] != 0 || udp[3] != 68)	/* to the client port */
		return 0;
	bootp = udp + 8;
	blen = len - (bootp - f);
	if (blen < BOOTP_LEN || bootp[0] != 2 || get32(bootp + 4) != c->xid)
		return 0;
	if (!c->have_reply && blen <= sizeof(c->reply)) {
		CopyMem((APTR)bootp, c->reply, blen);
		c->replylen = blen;
		c->have_reply = 1;
		Signal(c->task, SIGBREAKF_CTRL_E);
	}
	return 1;
}

/* build and send a broadcast DHCP message */
static void
dhcp_send(struct dhcpctx *c, int type, const struct lease *want)
{
	UBYTE f[FRAME_HDRS + BOOTP_LEN + 80], *ip, *udp, *b, *o;
	int blen, iplen, i;
	static UWORD ipid;

	memset(f, 0, sizeof(f));
	for (i = 0; i < 6; i++) {
		f[i] = 0xff;
		f[6 + i] = c->mac[i];
	}
	f[12] = 0x08;

	ip = f + 14;
	udp = ip + 20;
	b = udp + 8;

	b[0] = 1;			/* BOOTREQUEST */
	b[1] = 1;			/* Ethernet */
	b[2] = 6;
	put32(b + 4, c->xid);
	b[10] = 0x80;			/* please broadcast the reply */
	for (i = 0; i < 6; i++)
		b[28 + i] = c->mac[i];
	o = b + BOOTP_LEN;
	put32(o, 0x63825363UL);		/* magic cookie */
	o += 4;
	*o++ = OPT_MSGTYPE; *o++ = 1; *o++ = type;
	*o++ = OPT_CLIENTID; *o++ = 7; *o++ = 1;
	for (i = 0; i < 6; i++)
		*o++ = c->mac[i];
	if (type == DHCPREQUEST && want) {
		*o++ = OPT_REQADDR; *o++ = 4; put32(o, want->addr); o += 4;
		if (want->server) {
			*o++ = OPT_SERVERID; *o++ = 4;
			put32(o, want->server);
			o += 4;
		}
	}
	*o++ = OPT_PARAMS; *o++ = 5;
	*o++ = OPT_SUBNET; *o++ = OPT_ROUTER; *o++ = OPT_DNS;
	*o++ = OPT_DOMAIN; *o++ = OPT_LEASE;
	*o++ = OPT_END;
	blen = o - b;
	if (blen < 300)
		blen = 300;		/* minimum BOOTP size some servers want */

	/* UDP 68 -> 67, checksum 0 (allowed for IPv4) */
	udp[1] = 68;
	udp[3] = 67;
	udp[4] = (8 + blen) >> 8;
	udp[5] = 8 + blen;

	iplen = 20 + 8 + blen;
	ip[0] = 0x45;
	ip[2] = iplen >> 8;
	ip[3] = iplen;
	ipid++;
	ip[4] = ipid >> 8;
	ip[5] = ipid;
	ip[8] = 64;			/* TTL */
	ip[9] = 17;			/* UDP */
	put32(ip + 12, 0);
	put32(ip + 16, 0xffffffffUL);
	{
		UWORD ck = ip_checksum(ip, 20);

		ip[10] = ck >> 8;
		ip[11] = ck;
	}
	sana_raw_send(c->viu, f, 14 + iplen);
}

/* parse a reply; returns the DHCP message type (0 if malformed) */
static int
dhcp_parse(const UBYTE *b, ULONG len, struct lease *l)
{
	const UBYTE *o = b + BOOTP_LEN + 4, *end = b + len;
	int type = 0;

	if (len < BOOTP_LEN + 4 || get32(b + BOOTP_LEN) != 0x63825363UL)
		return 0;
	memset(l, 0, sizeof(*l));
	l->addr = get32(b + 16);	/* yiaddr */
	l->mask = 0xffffff00UL;
	l->leasetime = 3600;
	while (o < end && *o != OPT_END) {
		int code = *o++, olen, i;

		if (code == OPT_PAD)
			continue;
		if (o >= end)
			break;
		olen = *o++;
		if (o + olen > end)
			break;
		switch (code) {
		case OPT_MSGTYPE:
			if (olen >= 1)
				type = o[0];
			break;
		case OPT_SUBNET:
			if (olen >= 4)
				l->mask = get32(o);
			break;
		case OPT_ROUTER:
			if (olen >= 4)
				l->router = get32(o);
			break;
		case OPT_DNS:
			for (i = 0; i + 4 <= olen && l->ndns < 4; i += 4)
				l->dns[l->ndns++] = get32(o + i);
			break;
		case OPT_DOMAIN:
			for (i = 0; i < olen && i < (int)sizeof(l->domain) - 1; i++)
				l->domain[i] = o[i];
			l->domain[i] = '\0';
			break;
		case OPT_LEASE:
			if (olen >= 4)
				l->leasetime = get32(o);
			break;
		case OPT_T1:
			if (olen >= 4)
				l->t1 = get32(o);
			break;
		case OPT_SERVERID:
			if (olen >= 4)
				l->server = get32(o);
			break;
		}
		o += olen;
	}
	if (!l->t1)
		l->t1 = l->leasetime / 2;
	return type;
}

/* wait up to ms for a reply of one of the wanted types */
static int
dhcp_wait(struct dhcpctx *c, ULONG ms, int want1, int want2,
    struct lease *l)
{
	ULONG waited = 0;
	int type;

	while (waited < ms && c->generation == config_generation) {
		if (c->have_reply) {
			type = dhcp_parse(c->reply, c->replylen, l);
			c->have_reply = 0;
			if (type == want1 || type == want2)
				return type;
			continue;
		}
		amiga_host_sleep_ms(50);
		waited += 50;
	}
	return 0;
}

static void
fmt_ip(char *buf, ULONG a)
{
	int i, n = 0;

	for (i = 24; i >= 0; i -= 8) {
		ULONG v = (a >> i) & 255;

		if (v >= 100) buf[n++] = '0' + v / 100;
		if (v >= 10) buf[n++] = '0' + (v / 10) % 10;
		buf[n++] = '0' + v % 10;
		if (i)
			buf[n++] = '.';
	}
	buf[n] = '\0';
}

static void
apply_lease(struct dhcpctx *c, const struct lease *l)
{
	struct iface *ifc = c->ifc;
	char a[16], g[16];
	int i;

	if (rump_amibsdnet_ifaddr4(ifc->name, l->addr, l->mask) != 0) {
		P("%s: DHCP address rejected (errno %d)\n", ifc->name,
		    amiga_rump_errno());
		return;
	}
	ifc->addr = l->addr;
	ifc->mask = l->mask;
	ifc->gateway = l->router;
	for (i = 0; i < l->ndns && i < 4; i++)
		ifc->dns[i] = l->dns[i];
	ifc->ndns = i;
	ifc->up = 1;
	stack_update_route();
	stack_update_dns();
	if (l->domain[0])
		netdb_set_domain(l->domain);
	fmt_ip(a, l->addr);
	fmt_ip(g, l->router);
	P("%s: DHCP lease %s, gateway %s, %ld DNS, %lu s\n", ifc->name, a, g,
	    (long)l->ndns, l->leasetime);
}

/* one exchange round (a few seconds); returns 0 on success */
static int
dhcp_acquire(struct dhcpctx *c, struct lease *l, const struct lease *prev)
{
	struct EClockVal ev;
	struct lease offer;
	ULONG timeout = 2000;
	int attempt, type;

	for (attempt = 0; attempt < 3; attempt++, timeout *= 2) {
		ReadEClock(&ev);
		c->xid = ev.ev_lo ^ get32(c->mac + 2) ^ (attempt << 24);
		c->have_reply = 0;
		if (prev && attempt == 0) {
			/* try to keep the address we had */
			offer = *prev;
		} else {
			dhcp_send(c, DHCPDISCOVER, NULL);
			if (dhcp_wait(c, timeout, DHCPOFFER, 0, &offer) == 0)
				continue;
		}
		c->have_reply = 0;
		dhcp_send(c, DHCPREQUEST, &offer);
		type = dhcp_wait(c, timeout, DHCPACK, DHCPNAK, l);
		if (type == DHCPACK) {
			if (!l->server)
				l->server = offer.server;
			return 0;
		}
	}
	return -1;
}

/* remove the leased address and route from the kernel */
static void
drop_lease(struct dhcpctx *c)
{
	struct iface *ifc = c->ifc;

	ifc->up = 0;
	ifc->gateway = 0;
	ifc->ndns = 0;
	stack_update_route();
	stack_update_dns();
	if (ifc->addr)
		rump_amibsdnet_ifdeladdr4(ifc->name, ifc->addr);
	ifc->addr = 0;
}

struct dhcpclient {
	struct dhcpctx c;
	struct lease l;
	ULONG generation;
};

/*
 * Sleep up to ms, waking early for a link/admin change of the interface
 * or a reconfiguration.  Returns 1 if woken by such an event.
 */
static int
dhcp_pause(struct dhcpclient *d, ULONG ms)
{
	struct iface *ifc = d->c.ifc;

	for (;;) {
		if (ifc->dhcp_event) {
			ifc->dhcp_event = 0;
			return 1;
		}
		if (d->generation != config_generation)
			return 1;
		if (ms == 0)
			return 0;
		amiga_host_sleep_ms(ms > 250 ? 250 : ms);
		ms = ms > 250 ? ms - 250 : 0;
	}
}

/*
 * The DHCP client of one interface, for as long as this configuration
 * lasts.  While the link is up and there is no lease it keeps asking
 * (backing off to once a minute), so a cable plugged in or a router
 * switched on later is picked up by itself.  On link loss the lease is
 * dropped; when the link returns the previous address is requested again.
 */
static void *
dhcp_thread(void *arg)
{
	struct dhcpclient *d = arg;
	struct dhcpctx *c = &d->c;
	struct iface *ifc = c->ifc;
	struct lease l;
	ULONG backoff = 4000, waited;
	int bound = 0, have_prev = 0, quiet = 0;

	c->task = SysBase->ThisTask;
	while (d->generation == config_generation) {
		if (!ifc->admin || !ifc->link) {
			if (bound) {
				P("%s: %s, releasing %s\n", ifc->name,
				    ifc->admin ? "link lost" : "offline",
				    "the DHCP address");
				drop_lease(c);
				bound = 0;
			}
			dhcp_pause(d, 5000);
			continue;
		}
		if (!bound) {
			if (!quiet)
				P("%s: DHCP discovering\n", ifc->name);
			sana_set_tap(c->viu, dhcp_tap, c);
			if (dhcp_acquire(c, &l, have_prev ? &d->l : NULL) == 0) {
				sana_set_tap(c->viu, NULL, NULL);
				apply_lease(c, &l);
				d->l = l;
				have_prev = 1;
				bound = 1;
				quiet = 0;
				backoff = 4000;
				continue;
			}
			sana_set_tap(c->viu, NULL, NULL);
			if (!quiet)
				P("%s: no DHCP server answered, retrying\n",
				    ifc->name);
			quiet = 1;
			dhcp_pause(d, backoff);
			if (backoff < 60000)
				backoff *= 2;
			continue;
		}
		/* bound: sleep until T1, then renew */
		waited = 0;
		while (waited < d->l.t1 && !dhcp_pause(d, 1000))
			waited++;
		if (waited < d->l.t1)
			continue;	/* an event: re-evaluate */
		sana_set_tap(c->viu, dhcp_tap, c);
		if (dhcp_acquire(c, &l, &d->l) == 0) {
			if (l.addr != d->l.addr) {
				drop_lease(c);
				apply_lease(c, &l);
			}
			d->l = l;
		} else {
			P("%s: DHCP renewal failed\n", ifc->name);
			/* past the lease time the address is no longer ours */
			if (d->l.leasetime <= d->l.t1 + 60) {
				drop_lease(c);
				bound = 0;
			} else
				d->l.leasetime -= d->l.t1, d->l.t1 = 60;
		}
		sana_set_tap(c->viu, NULL, NULL);
	}
	if (bound)
		drop_lease(c);
	sana_set_tap(c->viu, NULL, NULL);
	FreeVec(d);
	Forbid();
	dhcp_clients--;
	Permit();
	return NULL;
}

/* running clients: a reconfiguration waits for the old ones to end */
volatile int dhcp_clients;

/* start the DHCP client of an interface; returns at once */
int
dhcp_configure(struct iface *ifc)
{
	struct dhcpclient *d;
	int i;

	if ((d = AllocVec(sizeof(*d), MEMF_FAST | MEMF_CLEAR)) == NULL)
		return -1;
	d->c.ifc = ifc;
	d->generation = d->c.generation = config_generation;
	if ((d->c.viu = sana_find(ifc->device, ifc->unit)) == NULL) {
		FreeVec(d);
		return -1;
	}
	for (i = 0; i < 6; i++)
		d->c.mac[i] = sana_macaddr(d->c.viu)[i];
	rump_amibsdnet_ifflags(ifc->name, NB_IFF_UP, 0);
	Forbid();
	dhcp_clients++;
	Permit();
	if (rumpuser_thread_create(dhcp_thread, d, "AmiBSDNet DHCP", 0, 0, -1,
	    NULL) != 0) {
		Forbid();
		dhcp_clients--;
		Permit();
		FreeVec(d);
		return -1;
	}
	return 0;
}
