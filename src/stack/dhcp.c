/*
 * AmiBSDNet: DHCP client (RFC 2131, options RFC 2132).
 *
 * Before an interface has an address the kernel cannot send or receive
 * DHCP traffic, so the exchange is done at the frame level through the
 * SANA-II backend (like BSD DHCP clients use BPF): broadcasts are built
 * here and sent raw; replies are caught by a receive tap before they reach
 * the kernel.  Once the interface has its address, the messages RFC 2131
 * wants sent by unicast to the server (DHCPREQUEST in RENEWING, section
 * 4.4.5; DHCPRELEASE, section 4.4.4) go through a kernel UDP socket bound
 * to the leased address and port 68, so the kernel routes them and
 * resolves the next hop; their replies are caught by the same tap.
 *
 * Each DHCP interface has a client thread (dhcp_thread()) with the states
 * of RFC 2131 figure 5: INIT (SELECTING, REQUESTING), INIT-REBOOT
 * (REBOOTING), BOUND, RENEWING and REBINDING.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <proto/exec.h>

#include "rumpuser_amiga.h"
#include "sana2_host.h"
#include "stack.h"

extern struct ExecBase *SysBase;

#define	P	amiga_rump_printf

/* message types (RFC 2132 section 9.6) */
#define	DHCPDISCOVER	1
#define	DHCPOFFER	2
#define	DHCPREQUEST	3
#define	DHCPDECLINE	4
#define	DHCPACK		5
#define	DHCPNAK		6
#define	DHCPRELEASE	7

/* option codes (RFC 2132) */
#define	OPT_PAD		0
#define	OPT_SUBNET	1
#define	OPT_ROUTER	3
#define	OPT_DNS		6
#define	OPT_DOMAIN	15
#define	OPT_REQADDR	50
#define	OPT_LEASE	51
#define	OPT_OVERLOAD	52
#define	OPT_MSGTYPE	53
#define	OPT_SERVERID	54
#define	OPT_PARAMS	55
#define	OPT_MESSAGE	56
#define	OPT_T1		58
#define	OPT_T2		59
#define	OPT_CLIENTID	61
#define	OPT_END		255

#define	LEASE_INFINITE	0xffffffffUL	/* RFC 2131 section 3.3 */

/* the fixed BOOTP part (RFC 2131 figure 1): sname at 44 (64 octets), file
   at 108 (128 octets), options from 236 on */
#define	BOOTP_LEN	236
#define	BOOTP_SNAME	44
#define	BOOTP_FILE	108
#define	BOOTP_MIN	300	/* RFC 1542 section 2.1; RFC 951 "vend 64" */
#define	MAGIC		0x63825363UL
#define	FRAME_HDRS	(14 + 20 + 8)
#define	MSG_MAX		(BOOTP_LEN + 128)

#define	PORT_SERVER	67	/* RFC 2131 section 4.1 */
#define	PORT_CLIENT	68

/*
 * Address conflict detection (RFC 5227 section 2.1.1), with the values
 * NetBSD uses for its own (netbsd-src/sys/netinet/if_inarp.h:63-71).
 */
#define	PROBE_WAIT	1
#define	PROBE_NUM	3
#define	PROBE_MIN	1
#define	PROBE_MAX	2
#define	ANNOUNCE_WAIT	2
#define	MAX_CONFLICTS	10
#define	RATE_LIMIT_INTERVAL 60

/* getrandom(2) from the kernel: netbsd-src/sys/sys/random.h:41,
   sys/kern/sys_getrandom.c:76-82 (never blocks with GRND_INSECURE) */
#define	GRND_INSECURE	(1u << 2)

/* the kernel's system call entry points (netbsd-src/sys/rump/librump/
   rumpkern/rump_syscalls.c; ssize_t is int on m68k) */
int	rump___sysimpl_getrandom(void *, size_t, unsigned int);
int	rump___sysimpl_socket30(int, int, int);
int	rump___sysimpl_bind(int, const void *, unsigned int);
int	rump___sysimpl_sendto(int, const void *, size_t, int, const void *,
	    unsigned int);
int	rump___sysimpl_close(int);

/* netbsd-src/sys/sys/socket.h:106,204 and netinet/in.h:246-252 */
#define	NB_AF_INET	2
#define	NB_SOCK_DGRAM	2
struct nb_sockaddr_in {
	UBYTE	sin_len;
	UBYTE	sin_family;
	UWORD	sin_port;
	ULONG	sin_addr;
	UBYTE	sin_zero[8];
};

struct lease {
	ULONG addr, mask, router, server;
	ULONG dns[4];
	int ndns;
	char domain[128];		/* (as config.c's; a longer one is not
					   used, dhcp_parse()) */
	ULONG leasetime, t1, t2;	/* t1, t2: 0 if not given */
};

struct dhcpctx {
	struct iface *ifc;
	struct virtif_user *viu;
	UBYTE mac[6];
	volatile ULONG xid;
	struct Task *task;
	ULONG sigmask;			/* the client's wake-up signal */

	/* filled by the tap (on the I/O process, under Forbid) */
	volatile int have_reply;
	UBYTE reply[1500];		/* up to a full Ethernet frame */
	volatile ULONG replylen;
	volatile ULONG probe;		/* address being checked with ARP */
	volatile int conflict;		/* ... and it is in use */
};

struct dhcpclient {
	struct dhcpctx c;
	ULONG generation;		/* configuration this client belongs to */
	struct MsgPort *tport;
	struct timerequest *treq;
	BYTE wakesig;

	struct lease l;			/* the lease bound, or last bound */
	int bound;			/* l.addr is in the kernel */
	int have_lease;			/* l is a lease not yet given up */
	int infinite;
	ULONG t1, t2, expire;		/* absolute now_s() times */
	ULONG hint;			/* address to suggest in DHCPDISCOVER */
	int started;			/* the start delay is over */
	int quiet;			/* "no server" was said */
	int conflicts;
	int down;			/* the interface was taken down */
	int send_err;			/* a unicast failed (said once) */
};

/* ------------------------------------------------------------------------
 * helpers
 */

static ULONG
get32(const UBYTE *p)
{

	return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | (p[2] << 8) | p[3];
}

static UWORD
get16(const UBYTE *p)
{

	return (UWORD)((p[0] << 8) | p[1]);
}

static void
put32(UBYTE *p, ULONG v)
{

	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* one's complement sum (RFC 768) */
static ULONG
csum_add(ULONG sum, const UBYTE *p, ULONG len)
{

	while (len > 1) {
		sum += (p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += p[0] << 8;
	return sum;
}

static UWORD
csum_fold(ULONG sum)
{

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (UWORD)~sum;
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

/* seconds since boot (monotonic) */
static ULONG
now_s(void)
{
	int64_t sec;
	long nsec;

	if (rumpuser_clock_gettime(RUMPUSER_CLOCK_ABSMONO, &sec, &nsec) != 0)
		return 0;
	return (ULONG)sec;
}

/* from the kernel's cprng; 0 if that failed (said in the log) */
static int
random32(ULONG *v)
{
	static int said;

	if (rump___sysimpl_getrandom(v, sizeof(*v), GRND_INSECURE) !=
	    (int)sizeof(*v)) {
		if (!said)
			P("DHCP: getrandom failed (errno %d)\n",
			    amiga_rump_errno());
		said = 1;
		return 0;
	}
	return 1;
}

/* uniform in lo..hi (inclusive); lo if there is no random number */
static ULONG
random_range(ULONG lo, ULONG hi)
{
	ULONG r;

	if (hi <= lo || !random32(&r))
		return lo;
	return lo + r % (hi - lo + 1);
}

/* a usable unicast host address (not 0/8, 127/8, class D or E:
   netbsd-src/sys/netinet/in.h:180-212, in.c:308-310) */
static int
unicast_ok(ULONG a)
{
	ULONG net = a >> 24;

	return net != 0 && net != 127 && (a & 0xe0000000UL) != 0xe0000000UL;
}

/* the classful mask NetBSD gives an address without one
   (netbsd-src/sys/netinet/in.c:1227-1235) */
static ULONG
classful_mask(ULONG a)
{

	if ((a & 0x80000000UL) == 0)
		return 0xff000000UL;
	if ((a & 0xc0000000UL) == 0x80000000UL)
		return 0xffff0000UL;
	return 0xffffff00UL;
}

/* contiguous ones from the top, at least one */
static int
mask_ok(ULONG m)
{
	ULONG inv = ~m;

	return m != 0 && (inv & (inv + 1)) == 0;
}

/*
 * A domain name from option 15: NVT ASCII, trailing NULs removed (RFC 2132
 * section 2), labels of letters, digits and hyphens that neither start nor
 * end with a hyphen (RFC 1035 section 2.3.1; a leading digit allowed by
 * RFC 1123 section 2.1), each 1..63 long, the name at most 253 characters
 * (255 octets on the wire, RFC 1035 section 2.3.4).  One trailing dot is
 * removed.  s has room for len + 1 characters; 1 if the name is usable
 * (then NUL-terminated).
 */
static int
domain_ok(char *s, int len)
{
	int i, lablen = 0;

	while (len > 0 && s[len - 1] == '\0')
		len--;
	if (len > 0 && s[len - 1] == '.')
		len--;
	if (len == 0 || len > 253)
		return 0;
	for (i = 0; i < len; i++) {
		char ch = s[i];

		if (ch == '.') {
			if (lablen == 0 || s[i - 1] == '-')
				return 0;
			lablen = 0;
			continue;
		}
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		    (ch >= '0' && ch <= '9') || (ch == '-' && lablen > 0)))
			return 0;
		if (++lablen > 63)
			return 0;
	}
	if (s[len - 1] == '-')
		return 0;
	s[len] = '\0';
	return 1;
}

/* ------------------------------------------------------------------------
 * waiting: the client sleeps on its timer and its wake-up signal, which
 * the tap (a reply, an address conflict) and dhcp_wake() send
 */

#define	W_TIMEOUT	0
#define	W_REPLY		1
#define	W_WOKEN		2
#define	W_CONFLICT	3

#define	WANT_REPLY	1
#define	WANT_CONFLICT	2
#define	WANT_EVENT	4	/* idle: wake for a dhcp_event only */

/* the exchange must stop: reconfiguration, offline, link lost */
static int
woken(struct dhcpclient *d)
{
	struct iface *ifc = d->c.ifc;

	return d->generation != config_generation || !ifc->admin ||
	    !ifc->link || ifc->release || ifc->stop;
}

static int
dhcp_sleep(struct dhcpclient *d, ULONG secs, ULONG ms, int want)
{
	struct timerequest *tr = d->treq;
	ULONG tsig = 1UL << d->tport->mp_SigBit;
	int r = W_TIMEOUT, armed = 0;

	secs += ms / 1000;
	ms %= 1000;
	if (secs || ms) {
		tr->tr_node.io_Command = TR_ADDREQUEST;
		tr->tr_time.tv_secs = secs;
		tr->tr_time.tv_micro = ms * 1000;
		SendIO((struct IORequest *)tr);
		armed = 1;
	}
	for (;;) {
		/* (idle waits: link and admin changes come with a
		   dhcp_event, see link_hook(), stack_online(),
		   stack_offline() in config.c) */
		/* (stop also in the idle waits: client_loop() clears
		   dhcp_event after its test of stop, so the event set with
		   stop, src/lib/ifapi.c srv_rmif(), can be lost) */
		if (d->generation != config_generation || ((want &
		    WANT_EVENT) ? (d->c.ifc->dhcp_event || d->c.ifc->stop) :
		    woken(d))) {
			r = W_WOKEN;
			break;
		}
		if ((want & WANT_CONFLICT) && d->c.conflict) {
			r = W_CONFLICT;
			break;
		}
		if ((want & WANT_REPLY) && d->c.have_reply) {
			r = W_REPLY;
			break;
		}
		if (!armed || CheckIO((struct IORequest *)tr))
			break;
		Wait(d->c.sigmask | tsig);
	}
	if (armed) {
		if (!CheckIO((struct IORequest *)tr))
			AbortIO((struct IORequest *)tr);
		WaitIO((struct IORequest *)tr);
	}
	return r;
}

void
dhcp_wake(struct iface *ifc)
{

	Forbid();
	if (ifc->dhcp_task)
		Signal(ifc->dhcp_task, ifc->dhcp_sigmask);
	Permit();
}

/* ------------------------------------------------------------------------
 * the receive tap: runs on the interface's I/O process under Forbid()
 * (src/host/sana2.c tapped()), must not block
 */

static int
arp_frame(struct dhcpctx *c, const UBYTE *f, ULONG len)
{
	const UBYTE *a = f + 14, *sha = a + 8;
	ULONG spa, tpa;
	int i, ours = 1;

	if (c->probe == 0 || len < 14 + 28 || get16(a) != 1 ||
	    get16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4)
		return 0;
	for (i = 0; i < 6; i++)
		if (sha[i] != c->mac[i])
			ours = 0;
	if (ours)		/* our own probe echoed (RFC 5227 2.1.1 NOTE) */
		return 0;
	spa = get32(a + 14);
	tpa = get32(a + 24);
	/* RFC 5227 section 2.1.1: any ARP packet from the address, or
	   another host's probe for it */
	if (spa == c->probe || (spa == 0 && get16(a + 6) == 1 &&
	    tpa == c->probe)) {
		c->conflict = 1;
		Signal(c->task, c->sigmask);
	}
	return 0;		/* the kernel sees ARP as always */
}

static int
dhcp_tap(void *arg, const UBYTE *f, ULONG len)
{
	struct dhcpctx *c = arg;
	const UBYTE *ip = f + 14, *udp, *bootp;
	ULONG ihl, iplen, udplen, blen, sum;
	int i;

	if (len >= 14 && f[12] == 0x08 && f[13] == 0x06)
		return arp_frame(c, f, len);
	if (len < FRAME_HDRS + BOOTP_LEN || f[12] != 0x08 || f[13] != 0x00)
		return 0;
	ihl = (ip[0] & 15) * 4;
	iplen = get16(ip + 2);
	if ((ip[0] >> 4) != 4 || ihl < 20 || ip[9] != 17 ||
	    iplen < ihl + 8 || iplen > len - 14 || len < 14 + ihl + 8)
		return 0;
	/* fragments are left to the kernel (the MF bit, the offset) */
	if ((ip[6] & 0x3f) != 0 || ip[7] != 0)
		return 0;
	udp = ip + ihl;
	if (get16(udp + 2) != PORT_CLIENT)
		return 0;
	if (csum_fold(csum_add(0, ip, ihl)) != 0)
		return 0;
	udplen = get16(udp + 4);
	if (udplen < 8 + BOOTP_LEN + 4 || udplen > iplen - ihl)
		return 0;
	bootp = udp + 8;
	blen = udplen - 8;
	if (bootp[0] != 2 || bootp[1] != 1 || bootp[2] != 6 ||
	    get32(bootp + 4) != c->xid)
		return 0;
	for (i = 0; i < 6; i++)		/* chaddr: our hardware address */
		if (bootp[28 + i] != c->mac[i])
			return 0;
	/* a UDP checksum of 0 means none was computed (RFC 768) */
	if (get16(udp + 6) != 0) {
		sum = csum_add(0, ip + 12, 8);
		sum += 17 + udplen;
		sum = csum_add(sum, udp, udplen);
		if (csum_fold(sum) != 0)
			return 1;	/* ours, but damaged: dropped */
	}
	if (!c->have_reply && blen <= sizeof(c->reply)) {
		CopyMem((APTR)bootp, c->reply, blen);
		c->replylen = blen;
		c->have_reply = 1;
		Signal(c->task, c->sigmask);
	}
	return 1;
}

static void
tap_on(struct dhcpclient *d)
{

	d->c.have_reply = 0;
	sana_set_tap(d->c.viu, dhcp_tap, &d->c);
}

static void
tap_off(struct dhcpclient *d)
{

	sana_set_tap(d->c.viu, NULL, NULL);
	d->c.have_reply = 0;
}

/* ------------------------------------------------------------------------
 * messages
 */

struct dmsg {
	int type;
	ULONG ciaddr;
	ULONG reqaddr;			/* option 50, 0 for none */
	ULONG server;			/* option 54, 0 for none */
	int params;			/* option 55 */
	const char *message;		/* option 56 */
};

/* the BOOTP message into b (MSG_MAX octets); returns its length */
static int
dhcp_build(struct dhcpctx *c, UBYTE *b, const struct dmsg *m)
{
	static const UBYTE params[] = { OPT_SUBNET, OPT_ROUTER, OPT_DNS,
	    OPT_DOMAIN, OPT_LEASE, OPT_T1, OPT_T2 };
	UBYTE *o;
	int i, blen;

	memset(b, 0, MSG_MAX);
	b[0] = 1;			/* BOOTREQUEST */
	b[1] = 1;			/* Ethernet */
	b[2] = 6;
	put32(b + 4, c->xid);
	/* the reply by broadcast while we have no address (RFC 2131 4.1,
	   Table 5 'flags') */
	if (m->ciaddr == 0 && (m->type == DHCPDISCOVER ||
	    m->type == DHCPREQUEST))
		b[10] = 0x80;
	put32(b + 12, m->ciaddr);
	for (i = 0; i < 6; i++)
		b[28 + i] = c->mac[i];
	o = b + BOOTP_LEN;
	put32(o, MAGIC);
	o += 4;
	*o++ = OPT_MSGTYPE; *o++ = 1; *o++ = m->type;
	/* the same client identifier in every message (RFC 2131 3.1 step
	   6; type 1 + hardware address: RFC 2132 section 9.14) */
	*o++ = OPT_CLIENTID; *o++ = 7; *o++ = 1;
	for (i = 0; i < 6; i++)
		*o++ = c->mac[i];
	if (m->reqaddr) {
		*o++ = OPT_REQADDR; *o++ = 4; put32(o, m->reqaddr); o += 4;
	}
	if (m->server) {
		*o++ = OPT_SERVERID; *o++ = 4; put32(o, m->server); o += 4;
	}
	if (m->params) {
		*o++ = OPT_PARAMS; *o++ = sizeof(params);
		for (i = 0; i < (int)sizeof(params); i++)
			*o++ = params[i];
	}
	if (m->message) {
		int n = 0;

		while (m->message[n])
			n++;
		*o++ = OPT_MESSAGE; *o++ = n;
		for (i = 0; i < n; i++)
			*o++ = m->message[i];
	}
	*o++ = OPT_END;
	blen = o - b;
	if (blen < BOOTP_MIN)
		blen = BOOTP_MIN;
	return blen;
}

/* a broadcast through the SANA-II backend (IP source: ciaddr, 0 while
   the client has no address, RFC 2131 section 4.1) */
static void
send_broadcast(struct dhcpctx *c, const struct dmsg *m)
{
	UBYTE f[FRAME_HDRS + MSG_MAX], *ip, *udp;
	int blen, iplen, i;
	static UWORD ipid;
	UWORD ck;

	memset(f, 0, FRAME_HDRS);
	for (i = 0; i < 6; i++) {
		f[i] = 0xff;
		f[6 + i] = c->mac[i];
	}
	f[12] = 0x08;
	ip = f + 14;
	udp = ip + 20;
	blen = dhcp_build(c, udp + 8, m);

	/* UDP 68 -> 67, no checksum (allowed for IPv4: RFC 768) */
	udp[1] = PORT_CLIENT;
	udp[3] = PORT_SERVER;
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
	put32(ip + 12, m->ciaddr);
	put32(ip + 16, 0xffffffffUL);
	ck = csum_fold(csum_add(0, ip, 20));
	ip[10] = ck >> 8;
	ip[11] = ck;
	sana_raw_send(c->viu, f, 14 + iplen);
}

/* a unicast to the server through a kernel UDP socket bound to the
   leased address, port 68 (the kernel's credentials are root's:
   netbsd-src/sys/kern/kern_auth.c:110-115, secmodel_suser.c:598-604);
   0 or the NetBSD errno */
static int
send_unicast(struct dhcpclient *d, const struct dmsg *m, ULONG to)
{
	UBYTE b[MSG_MAX];
	struct nb_sockaddr_in sin;
	int s, blen, err = 0;

	blen = dhcp_build(&d->c, b, m);
	if ((s = rump___sysimpl_socket30(NB_AF_INET, NB_SOCK_DGRAM, 0)) < 0)
		err = amiga_rump_errno();
	else {
		memset(&sin, 0, sizeof(sin));
		sin.sin_len = sizeof(sin);
		sin.sin_family = NB_AF_INET;
		sin.sin_port = PORT_CLIENT;
		sin.sin_addr = m->ciaddr;
		if (rump___sysimpl_bind(s, &sin, sizeof(sin)) != 0)
			err = amiga_rump_errno();
		else {
			sin.sin_port = PORT_SERVER;
			sin.sin_addr = to;
			if (rump___sysimpl_sendto(s, b, blen, 0, &sin,
			    sizeof(sin)) != blen)
				err = amiga_rump_errno();
		}
		rump___sysimpl_close(s);
	}
	if (err && !d->send_err) {
		char a[16];

		fmt_ip(a, to);
		P("%s: DHCP: cannot send to the server %s (errno %d)\n",
		    d->c.ifc->name, a, err);
		d->send_err = 1;
	} else if (!err)
		d->send_err = 0;
	return err;
}

/* a new transaction id from the kernel's cprng (RFC 2131 section 4.1) */
static int
new_xid(struct dhcpclient *d)
{
	ULONG x;

	if (!random32(&x))
		return 0;
	d->c.xid = x;
	return 1;
}

/* ------------------------------------------------------------------------
 * replies
 */

struct pstate {
	struct lease *l;
	int type;
	int overload;
	int has_mask, has_lease;
	int domlen, domovf;
	char dom[256];
};

/* one option field (options: the 'options' field itself); 0 ok, -1
   malformed.  needend: the field must end with an 'end' option (RFC 2131
   section 4.1) */
static int
parse_field(const UBYTE *o, const UBYTE *end, struct pstate *ps,
    int options, int needend)
{
	struct lease *l = ps->l;
	int code, olen, i;

	while (o < end) {
		code = *o++;
		if (code == OPT_PAD)
			continue;
		if (code == OPT_END)
			return 0;
		if (o >= end)
			return -1;
		olen = *o++;
		/* each option entirely in its field (RFC 2131 section 4.1) */
		if (olen > end - o)
			return -1;
		switch (code) {
		case OPT_MSGTYPE:
			if (olen == 1 && ps->type == 0)
				ps->type = o[0];
			break;
		case OPT_SUBNET:
			if (olen == 4 && !ps->has_mask) {
				l->mask = get32(o);
				ps->has_mask = 1;
			}
			break;
		case OPT_ROUTER:	/* multiples of 4 (RFC 2132 3.5) */
			if (olen >= 4 && olen % 4 == 0 && l->router == 0)
				l->router = get32(o);
			break;
		case OPT_DNS:		/* instances are concatenated
					   (RFC 2131 section 4.1) */
			if (olen % 4 == 0)
				for (i = 0; i < olen && l->ndns < 4; i += 4)
					l->dns[l->ndns++] = get32(o + i);
			break;
		case OPT_DOMAIN:
			for (i = 0; i < olen; i++)
				if (ps->domlen < (int)sizeof(ps->dom) - 1)
					ps->dom[ps->domlen++] = o[i];
				else
					ps->domovf = 1;
			break;
		case OPT_LEASE:
			if (olen == 4 && !ps->has_lease) {
				l->leasetime = get32(o);
				ps->has_lease = 1;
			}
			break;
		case OPT_OVERLOAD:	/* RFC 2132 section 9.3 */
			if (options && olen == 1 && o[0] >= 1 && o[0] <= 3)
				ps->overload = o[0];
			break;
		case OPT_SERVERID:
			if (olen == 4 && l->server == 0)
				l->server = get32(o);
			break;
		case OPT_T1:
			if (olen == 4 && l->t1 == 0)
				l->t1 = get32(o);
			break;
		case OPT_T2:
			if (olen == 4 && l->t2 == 0)
				l->t2 = get32(o);
			break;
		}
		o += olen;
	}
	return needend ? -1 : 0;
}

/*
 * Parse and check a reply.  Returns the message type of a usable
 * DHCPOFFER, DHCPACK or DHCPNAK, 0 for anything else (said in the log
 * when it is a reply that cannot be used).
 */
static int
dhcp_parse(struct dhcpclient *d, const UBYTE *b, ULONG len, struct lease *l)
{
	struct iface *ifc = d->c.ifc;
	struct pstate ps;
	char a[16];
	int i, n;

	if (len < BOOTP_LEN + 4 || get32(b + BOOTP_LEN) != MAGIC)
		return 0;
	memset(l, 0, sizeof(*l));
	memset(&ps, 0, sizeof(ps));
	ps.l = l;
	if (parse_field(b + BOOTP_LEN + 4, b + len, &ps, 1, 0) != 0)
		return 0;
	/* the overloaded fields: 'file' first, then 'sname'; the options
	   field must then end with 'end' (RFC 2131 section 4.1) */
	if (ps.overload) {
		struct lease scratch;
		struct pstate chk;

		memset(&chk, 0, sizeof(chk));
		chk.l = &scratch;
		memset(&scratch, 0, sizeof(scratch));
		if (parse_field(b + BOOTP_LEN + 4, b + len, &chk, 1, 1) != 0)
			return 0;
		if ((ps.overload & 1) && parse_field(b + BOOTP_FILE,
		    b + BOOTP_FILE + 128, &ps, 0, 1) != 0)
			return 0;
		if ((ps.overload & 2) && parse_field(b + BOOTP_SNAME,
		    b + BOOTP_SNAME + 64, &ps, 0, 1) != 0)
			return 0;
	}
	if (ps.type == DHCPNAK)
		return DHCPNAK;
	if (ps.type != DHCPOFFER && ps.type != DHCPACK)
		return 0;
	l->addr = get32(b + 16);		/* yiaddr */
	fmt_ip(a, l->addr);
	/* server identifier and lease time: MUST (RFC 2131 Table 3) */
	if (!l->server || !ps.has_lease) {
		P("%s: DHCP reply for %s without %s: ignored\n", ifc->name, a,
		    !l->server ? "a server identifier" : "a lease time");
		return 0;
	}
	if (l->leasetime == 0) {
		P("%s: DHCP reply for %s with a lease time of 0: ignored\n",
		    ifc->name, a);
		return 0;
	}
	if (!ps.has_mask)
		l->mask = classful_mask(l->addr);
	if (!unicast_ok(l->addr) || !mask_ok(l->mask)) {
		P("%s: DHCP reply with an unusable address %s or mask: "
		    "ignored\n", ifc->name, a);
		return 0;
	}
	/* not the network's own or broadcast address (the kernel takes
	   addr | ~mask as the broadcast address: src/kern/netcfg.c:135) */
	if (l->mask <= 0xfffffffcUL && ((l->addr & ~l->mask) == 0 ||
	    (l->addr & ~l->mask) == ~l->mask)) {
		P("%s: DHCP reply with %s, the network's %s address: "
		    "ignored\n", ifc->name, a, (l->addr & ~l->mask) == 0 ?
		    "own" : "broadcast");
		return 0;
	}
	if (l->router && (!unicast_ok(l->router) || l->router == l->addr)) {
		fmt_ip(a, l->router);
		P("%s: DHCP router %s is not usable: ignored\n", ifc->name, a);
		l->router = 0;
	}
	for (i = n = 0; i < l->ndns; i++)
		if (unicast_ok(l->dns[i]))
			l->dns[n++] = l->dns[i];
	l->ndns = n;
	if (ps.domlen > 0) {
		int ok = !ps.domovf && domain_ok(ps.dom, ps.domlen);

		for (n = 0; ok && ps.dom[n]; n++)
			;
		if (ok && n < (int)sizeof(l->domain))
			sb_copy(l->domain, ps.dom, sizeof(l->domain));
		else
			P("%s: DHCP domain name is not usable: ignored\n",
			    ifc->name);
	}
	return ps.type;
}

/*
 * Wait up to ms for a usable reply of the wanted types (DHCPOFFER, or
 * DHCPACK and DHCPNAK); other replies are dropped.  Returns the type, 0
 * at the timeout, -1 if woken.
 */
static int
wait_reply(struct dhcpclient *d, ULONG ms, int want1, int want2,
    struct lease *l)
{
	ULONG start = amiga_host_ms(), gone;
	int type, w;

	for (;;) {
		gone = amiga_host_ms() - start;
		if (gone >= ms)
			return 0;
		w = dhcp_sleep(d, 0, ms - gone, WANT_REPLY);
		if (w == W_WOKEN)
			return -1;
		if (w != W_REPLY)
			return 0;
		type = dhcp_parse(d, d->c.reply, d->c.replylen, l);
		d->c.have_reply = 0;
		if (type != 0 && (type == want1 || type == want2))
			return type;
	}
}

/* RFC 2131 section 4.1: 4 s, then doubled up to 64 s, each randomised by
   -1..+1 s (in milliseconds) */
static ULONG
retransmit_ms(int attempt)
{
	ULONG base = 4;

	while (attempt-- > 0 && base < 64)
		base *= 2;
	return random_range(base * 1000 - 1000, base * 1000 + 1000);
}

/* ------------------------------------------------------------------------
 * the lease in the kernel
 */

/* the router and name servers of a lease (also when renewed) */
static void
update_lease(struct dhcpclient *d, const struct lease *l)
{
	struct iface *ifc = d->c.ifc;
	int i;

	ifc->gateway = l->router;
	for (i = 0; i < l->ndns && i < 4; i++)
		ifc->dns[i] = l->dns[i];
	ifc->ndns = i;
	stack_update_route();
	stack_update_dns();
	/* (a lease without option 15 clears this interface's domain) */
	sb_copy(ifc->domain, l->domain, sizeof(ifc->domain));
	stack_update_domain();
}

/* returns 0 if the kernel took the address */
static int
apply_lease(struct dhcpclient *d, const struct lease *l)
{
	struct iface *ifc = d->c.ifc;
	char a[16], g[16];

	if (rump_amibsdnet_ifaddr4(ifc->name, l->addr, l->mask) != 0) {
		P("%s: DHCP address rejected (errno %d)\n", ifc->name,
		    amiga_rump_errno());
		return -1;
	}
	ifc->addr = l->addr;
	ifc->mask = l->mask;
	ifc->up = 1;
	d->bound = 1;
	update_lease(d, l);
	fmt_ip(a, l->addr);
	fmt_ip(g, l->router);
	P("%s: DHCP lease %s, gateway %s, %ld DNS, %lu s\n", ifc->name, a, g,
	    (long)l->ndns, l->leasetime);
	return 0;
}

/* remove the leased address and route from the kernel */
static void
drop_lease(struct dhcpclient *d)
{
	struct iface *ifc = d->c.ifc;

	ifc->up = 0;
	ifc->gateway = 0;
	ifc->ndns = 0;
	ifc->domain[0] = '\0';
	stack_update_route();
	stack_update_dns();
	stack_update_domain();
	if (ifc->addr)
		rump_amibsdnet_ifdeladdr4(ifc->name, ifc->addr);
	ifc->addr = 0;
	d->bound = 0;
}

/*
 * T1, T2 and the expiry of a lease whose request was sent at "sent"
 * (RFC 2131 sections 4.4.1 and 4.4.5): from options 58 and 59, else 0.5
 * and 0.875 of the lease; T1 < T2 < expiry or the defaults are used.
 */
static void
set_times(struct dhcpclient *d, ULONG sent)
{
	ULONG lease = d->l.leasetime, t1 = d->l.t1, t2 = d->l.t2;
	ULONG d1 = lease / 2, d2 = lease - lease / 8;

	d->infinite = lease == LEASE_INFINITE;
	if (d->infinite)
		return;
	if (t1 == 0)
		t1 = d1;
	if (t2 == 0)
		t2 = d2;
	if (!(t1 < t2 && t2 < lease)) {
		t1 = d1;
		t2 = d2;
	}
	d->t1 = sent + t1;
	d->t2 = sent + t2;
	d->expire = sent + lease;
	if (d->expire < sent)		/* past the clock's range */
		d->t1 = d->t2 = d->expire = 0xffffffffUL;
}

/*
 * The address check of RFC 2131 section 4.4.1, as RFC 5227 section 2.1.1
 * describes it.  1 if the address is in use, 0 if not, -1 if woken.
 */
static int
arp_check(struct dhcpclient *d, ULONG addr)
{
	struct dhcpctx *c = &d->c;
	UBYTE f[14 + 28];
	int i, k, w;

	memset(f, 0, sizeof(f));
	for (i = 0; i < 6; i++) {
		f[i] = 0xff;
		f[6 + i] = c->mac[i];
		f[14 + 8 + i] = c->mac[i];	/* sender hardware address */
	}
	f[12] = 0x08; f[13] = 0x06;
	f[14 + 1] = 1;				/* Ethernet */
	f[14 + 2] = 0x08;			/* IPv4 */
	f[14 + 4] = 6;
	f[14 + 5] = 4;
	f[14 + 7] = 1;				/* request */
	put32(f + 14 + 24, addr);		/* target; sender IP 0 */

	c->conflict = 0;
	c->probe = addr;
	w = dhcp_sleep(d, 0, random_range(0, PROBE_WAIT * 1000),
	    WANT_CONFLICT);
	for (k = 0; k < PROBE_NUM && w == W_TIMEOUT; k++) {
		sana_raw_send(c->viu, f, sizeof(f));
		w = dhcp_sleep(d, 0, k < PROBE_NUM - 1 ?
		    random_range(PROBE_MIN * 1000, PROBE_MAX * 1000) :
		    ANNOUNCE_WAIT * 1000, WANT_CONFLICT);
	}
	c->probe = 0;
	if (w == W_WOKEN)
		return -1;
	return w == W_CONFLICT || c->conflict;
}

/* RFC 2131 section 3.1: at least 10 s before configuring again; after
   MAX_CONFLICTS conflicts one address per RATE_LIMIT_INTERVAL (RFC 5227
   section 2.1.1) */
static void
restart_pause(struct dhcpclient *d)
{

	dhcp_sleep(d, d->conflicts >= MAX_CONFLICTS ? RATE_LIMIT_INTERVAL :
	    10, 0, 0);
}

/* the address is in use: DHCPDECLINE (broadcast, RFC 2131 section 4.4.4;
   fields and options: Table 5) */
static void
decline(struct dhcpclient *d, const struct lease *l)
{
	struct dmsg m;
	char a[16];

	fmt_ip(a, l->addr);
	P("%s: DHCP address %s is in use by another host; declined\n",
	    d->c.ifc->name, a);
	d->conflicts++;
	if (!new_xid(d))
		return;
	memset(&m, 0, sizeof(m));
	m.type = DHCPDECLINE;
	m.reqaddr = l->addr;
	m.server = l->server;
	m.message = "address in use";
	send_broadcast(&d->c, &m);
}

/*
 * A DHCPACK to use (its request was sent at "sent").  A new address is
 * checked first (RFC 2131 sections 3.1 step 5, 3.2 step 3).  Returns 0
 * when bound, 1 to start again in INIT, -1 if woken.
 */
static int
take_ack(struct dhcpclient *d, const struct lease *ack, ULONG sent)
{
	int r;

	if (d->bound && ack->addr == d->l.addr) {
		if (ack->mask != d->l.mask) {
			drop_lease(d);
			if (apply_lease(d, ack) != 0) {
				d->have_lease = 0;
				return 1;
			}
		} else
			update_lease(d, ack);	/* router, DNS */
		d->l = *ack;
		set_times(d, sent);
		return 0;
	}
	tap_on(d);
	r = arp_check(d, ack->addr);
	tap_off(d);
	if (r < 0)
		return -1;
	if (r > 0) {
		if (d->bound)
			drop_lease(d);
		decline(d, ack);
		d->have_lease = 0;
		d->hint = 0;
		restart_pause(d);
		return 1;
	}
	if (d->bound)
		drop_lease(d);
	if (apply_lease(d, ack) != 0) {
		d->have_lease = 0;
		restart_pause(d);
		return 1;
	}
	d->l = *ack;
	d->have_lease = 1;
	d->quiet = 0;
	/* an address that passed the check is in use without a conflict:
	   the count of RFC 5227 section 2.1.1 starts again (this project's
	   choice; the RFC gives no reset rule) */
	d->conflicts = 0;
	set_times(d, sent);
	return 0;
}

/* DHCPRELEASE (unicast to the server, RFC 2131 sections 4.4.4, 4.4.6;
   Table 5), then the address goes */
static void
release_lease(struct dhcpclient *d, const char *why)
{
	struct dmsg m;
	char a[16];

	fmt_ip(a, d->l.addr);
	P("%s: %s, releasing the DHCP address %s\n", d->c.ifc->name, why, a);
	if (d->l.server && new_xid(d)) {
		memset(&m, 0, sizeof(m));
		m.type = DHCPRELEASE;
		m.ciaddr = d->l.addr;
		m.server = d->l.server;
		send_unicast(d, &m, d->l.server);
	}
	d->hint = d->l.addr;
	drop_lease(d);
	d->have_lease = 0;
}

/* ------------------------------------------------------------------------
 * the states
 */

/*
 * INIT: SELECTING then REQUESTING (RFC 2131 sections 3.1, 4.4.1).
 * Returns when bound (0), to start again (1) or when woken (-1).
 */
static int
run_init(struct dhcpclient *d)
{
	struct iface *ifc = d->c.ifc;
	struct lease offer, ack;
	struct dmsg m;
	ULONG sent;
	int attempt, type = 0, r;

	if (!d->started) {
		/* a random 1..10 s first (RFC 2131 section 4.4.1) */
		if (dhcp_sleep(d, 0, random_range(1000, 10000), 0) == W_WOKEN)
			return -1;
		d->started = 1;
	}
	if (!d->quiet)
		P("%s: DHCP discovering\n", ifc->name);
	if (!new_xid(d)) {
		restart_pause(d);
		return 1;
	}
	memset(&m, 0, sizeof(m));
	m.type = DHCPDISCOVER;
	m.reqaddr = d->hint;		/* MAY (RFC 2131 Table 5) */
	m.params = 1;
	tap_on(d);
	for (attempt = 0; ; attempt++) {
		send_broadcast(&d->c, &m);
		type = wait_reply(d, retransmit_ms(attempt), DHCPOFFER, 0,
		    &offer);
		if (type < 0) {
			tap_off(d);
			return -1;
		}
		if (type == DHCPOFFER)
			break;
		if (attempt == 0 && !d->quiet) {
			P("%s: no DHCP server answered yet, still asking\n",
			    ifc->name);
			d->quiet = 1;
		}
	}
	/* REQUESTING: the offer's xid, its address and server (Table 5) */
	memset(&m, 0, sizeof(m));
	m.type = DHCPREQUEST;
	m.reqaddr = offer.addr;
	m.server = offer.server;
	m.params = 1;
	sent = now_s();
	/* 4 + 8 + 16 + 32 s, about 60 s (RFC 2131 section 3.1 step 5) */
	for (attempt = 0; attempt < 4; attempt++) {
		send_broadcast(&d->c, &m);
		type = wait_reply(d, retransmit_ms(attempt), DHCPACK, DHCPNAK,
		    &ack);
		if (type != 0)
			break;
	}
	tap_off(d);
	if (type < 0)
		return -1;
	if (type == DHCPACK) {
		if ((r = take_ack(d, &ack, sent)) == 0)
			d->hint = 0;
		return r;
	}
	P("%s: DHCP server %s; starting again\n", ifc->name,
	    type == DHCPNAK ? "refused the offered address" :
	    "did not confirm the offered address");
	restart_pause(d);
	return 1;
}

/*
 * INIT-REBOOT: the known address again (RFC 2131 sections 3.2, 4.4.2),
 * after the link came back.  Without an answer the lease is used until
 * it ends (section 3.2 step 3).
 */
static int
run_reboot(struct dhcpclient *d)
{
	struct iface *ifc = d->c.ifc;
	struct lease ack;
	struct dmsg m;
	char a[16];
	ULONG sent;
	int attempt, type = 0;

	fmt_ip(a, d->l.addr);
	P("%s: DHCP asking for %s again\n", ifc->name, a);
	if (!new_xid(d)) {
		restart_pause(d);
		return 1;
	}
	memset(&m, 0, sizeof(m));
	m.type = DHCPREQUEST;
	m.reqaddr = d->l.addr;		/* no server identifier, ciaddr 0 */
	m.params = 1;
	sent = now_s();
	tap_on(d);
	for (attempt = 0; attempt < 4; attempt++) {
		send_broadcast(&d->c, &m);
		type = wait_reply(d, retransmit_ms(attempt), DHCPACK, DHCPNAK,
		    &ack);
		if (type != 0)
			break;
	}
	tap_off(d);
	if (type < 0)
		return -1;
	if (type == DHCPACK)
		return take_ack(d, &ack, sent);
	if (type == DHCPNAK) {
		/* not ours any more: INIT (section 3.2 step 3) */
		P("%s: DHCP server refused %s\n", ifc->name, a);
		d->have_lease = 0;
		return 1;
	}
	/* no answer: the rest of the unexpired lease */
	if (rump_amibsdnet_ifaddr4(ifc->name, d->l.addr, d->l.mask) != 0) {
		P("%s: DHCP address rejected (errno %d)\n", ifc->name,
		    amiga_rump_errno());
		d->have_lease = 0;
		restart_pause(d);
		return 1;
	}
	ifc->addr = d->l.addr;
	ifc->mask = d->l.mask;
	ifc->up = 1;
	d->bound = 1;
	update_lease(d, &d->l);
	P("%s: no DHCP server answered; using %s for the rest of its "
	    "lease\n", ifc->name, a);
	return 0;
}

/*
 * One DHCPREQUEST of RENEWING (unicast to the server) or REBINDING
 * (broadcast), ciaddr set, no options 50 and 54 (RFC 2131 section 4.4.5,
 * Table 5), then the wait of section 4.4.5: half the time left until T2
 * (RENEWING) or until the lease ends (REBINDING), at least 60 s, but not
 * past that time.
 */
static void
run_extend(struct dhcpclient *d, int rebinding)
{
	struct lease ack;
	struct dmsg m;
	ULONG now = now_s(), until = rebinding ? d->expire : d->t2, left, sent;
	int type;

	if (!new_xid(d)) {
		dhcp_sleep(d, 60, 0, WANT_EVENT);
		return;
	}
	memset(&m, 0, sizeof(m));
	m.type = DHCPREQUEST;
	m.ciaddr = d->l.addr;
	m.params = 1;
	left = until > now ? until - now : 0;
	if (left / 2 >= 60)
		left /= 2;
	else if (left > 60)
		left = 60;
	tap_on(d);
	sent = now_s();
	if (rebinding)
		send_broadcast(&d->c, &m);
	else
		send_unicast(d, &m, d->l.server);
	/* (in pieces of at most an hour: left * 1000 must fit the 32-bit
	   millisecond count of wait_reply()) */
	do {
		ULONG part = left > 3600 ? 3600 : left;

		type = wait_reply(d, part * 1000, DHCPACK, DHCPNAK, &ack);
		left -= part;
	} while (type == 0 && left > 0);
	tap_off(d);
	if (type == DHCPACK)
		take_ack(d, &ack, sent);
	else if (type == DHCPNAK) {
		P("%s: DHCP server refused the address\n", d->c.ifc->name);
		d->hint = 0;
		drop_lease(d);
		d->have_lease = 0;
	}
}

static void
client_loop(struct dhcpclient *d)
{
	struct iface *ifc = d->c.ifc;
	ULONG now;

	while (d->generation == config_generation && !ifc->stop) {
		ifc->dhcp_event = 0;
		/* offline (also a quick offline + online, "Reconnect"): the
		   lease is given back to the server, a new one fetched */
		if (ifc->release || !ifc->admin) {
			ifc->release = 0;
			if (d->bound)
				release_lease(d, "offline");
			d->have_lease = 0;
		}
		if (!ifc->admin) {
			/* down after the release went out */
			if (!d->down) {
				rump_amibsdnet_ifflags(ifc->name, 0, NB_IFF_UP);
				d->down = 1;
			}
			dhcp_sleep(d, 3600, 0, WANT_EVENT);
			continue;
		}
		d->down = 0;
		if (!ifc->link) {
			/* the lease is kept for INIT-REBOOT when the link
			   returns (RFC 2131 section 4.4.2) */
			if (d->bound) {
				P("%s: link lost, removing the DHCP address\n",
				    ifc->name);
				drop_lease(d);
			}
			dhcp_sleep(d, 3600, 0, WANT_EVENT);
			continue;
		}
		now = now_s();
		if (!d->bound) {
			if (d->have_lease && (d->infinite || now < d->expire))
				run_reboot(d);
			else {
				d->have_lease = 0;
				run_init(d);
			}
			continue;
		}
		if (d->infinite || now < d->t1) {
			ULONG secs = d->infinite ? 3600 : d->t1 - now;

			dhcp_sleep(d, secs > 3600 ? 3600 : secs, 0, WANT_EVENT);
			continue;
		}
		if (now >= d->expire) {
			/* RFC 2131 section 4.4.5: stop using it, INIT */
			P("%s: DHCP lease expired\n", ifc->name);
			d->hint = d->l.addr;
			drop_lease(d);
			d->have_lease = 0;
			continue;
		}
		run_extend(d, now >= d->t2);
	}
}

/*
 * The DHCP client of one interface, for as long as this configuration
 * lasts.  While the link is up and there is no lease it keeps asking (up
 * to every 64 s), so a cable plugged in or a router switched on later is
 * picked up by itself.
 */
static void *
dhcp_thread(void *arg)
{
	struct dhcpclient *d = arg;
	struct dhcpctx *c = &d->c;
	struct iface *ifc = c->ifc;

	c->task = SysBase->ThisTask;
	d->wakesig = AllocSignal(-1);
	if (d->wakesig != -1 && (d->tport = CreateMsgPort()) != NULL &&
	    (d->treq = (struct timerequest *)CreateIORequest(d->tport,
	    sizeof(*d->treq))) != NULL &&
	    OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
	    (struct IORequest *)d->treq, 0) == 0) {
		c->sigmask = 1UL << d->wakesig;
		Forbid();
		ifc->dhcp_sigmask = c->sigmask;
		ifc->dhcp_task = c->task;
		Permit();
		client_loop(d);
		if (d->bound)
			drop_lease(d);
		tap_off(d);
		CloseDevice((struct IORequest *)d->treq);
	} else
		P("%s: DHCP client: no signal or timer\n", ifc->name);
	if (d->treq)
		DeleteIORequest((struct IORequest *)d->treq);
	if (d->tport)
		DeleteMsgPort(d->tport);
	Forbid();
	if (ifc->dhcp_task == c->task)
		ifc->dhcp_task = NULL;
	Permit();
	if (d->wakesig != -1)
		FreeSignal(d->wakesig);
	FreeVec(d);
	Forbid();
	ifc->dhcp_running = 0;
	dhcp_clients--;
	if (stack_task)
		Signal(stack_task, SIGBREAKF_CTRL_E);
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

	/* (MEMF_PUBLIC: the receive tap writes into it from the I/O process,
	   and "ALL MEMORY THAT IS REFERENCED VIA INTERRUPTS AND/OR BY OTHER
	   TASKS MUST BE EITHER PUBLIC OR LOCKED INTO MEMORY",
	   downloads/sources/NDK3.2/Autodocs/exec.doc:927-932; no other
	   type: "For expanded systems, the fast memory pool is searched
	   first", exec.doc:902-904) */
	if ((d = AllocVec(sizeof(*d), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return -1;
	d->c.ifc = ifc;
	d->generation = config_generation;
	d->wakesig = -1;
	if ((d->c.viu = sana_find(ifc->device, ifc->unit)) == NULL) {
		FreeVec(d);
		return -1;
	}
	for (i = 0; i < 6; i++)
		d->c.mac[i] = sana_macaddr(d->c.viu)[i];
	rump_amibsdnet_ifflags(ifc->name, NB_IFF_UP, 0);
	Forbid();
	ifc->stop = 0;		/* (a RemoveInterface that failed set it) */
	ifc->dhcp_running = 1;
	dhcp_clients++;
	Permit();
	if (rumpuser_thread_create(dhcp_thread, d, "AmiBSDNet DHCP", 0, 0, -1,
	    NULL) != 0) {
		Forbid();
		ifc->dhcp_running = 0;
		dhcp_clients--;
		Permit();
		FreeVec(d);
		return -1;
	}
	return 0;
}
