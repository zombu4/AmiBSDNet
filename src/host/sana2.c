/*
 * SANA-II backend for the rump kernel's virtif network interface
 * (src/kern/if_virt.c built with VIRTIF_BASE=sana and RUMP_VIF_LINKSTR),
 * giving kernel interfaces sana0, sana1, ...
 *
 * The link string names the SANA-II driver and unit: "uaenet.device:0".
 *
 * One I/O process per interface owns every SANA-II request (Exec reply
 * signals belong to the task that created the port).  It keeps a pool of
 * CMD_READs queued for IPv4, ARP and IPv6, rebuilds the Ethernet header
 * from the SANA-II fields (cooked mode works with every driver, raw mode
 * does not) and feeds frames to the kernel.  Transmission is asynchronous:
 * the kernel copies a frame into a free transmit request and posts it to
 * the I/O process, which issues CMD_WRITE / S2_BROADCAST / S2_MULTICAST.
 *
 * Lifetime: interfaces live in a fixed table, so a struct virtif_user
 * that host code got from sana_find() stays valid memory for as long as
 * the program runs.  A slot belongs to one driver and unit for good and
 * is reused only when the same driver and unit are attached again.
 * After rumpcomp_sana_destroy() the host entry points do nothing (they
 * report a dead interface), and the I/O process releases the driver only
 * once no host call is inside (the users count).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/timer.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

#include <amibsdnet/devopen.h>
#include <amibsdnet/hwcheck.h>

#include "rumpuser_amiga.h"
#include "sana2.h"
#include "sana2_host.h"
#include "entropy.h"

extern struct ExecBase *SysBase;

#define	ETHER_ADDR_LEN	6
#define	ETHER_HDR_LEN	14
#define	FRAME_MAX	1536		/* Ethernet MTU 1500 + header, rounded */

#define	NRX_PER_TYPE	6
#define	NTX		24
#define	MAXVIU		16		/* interfaces (driver + unit) */
#define	EV_RETRIES	5		/* S2_ONEVENT failures in a row */

/* NetBSD errno values (sys/sys/errno.h) */
#define	S_EIO		5
#define	S_ENXIO		6
#define	S_ENOMEM	12
#define	S_ENOSPC	28

static const UWORD rxtypes[] = { 0x0800, 0x0806, 0x86dd };
#define	NRXTYPES	(sizeof(rxtypes) / sizeof(rxtypes[0]))

/* must match struct iovec in the kernel (and struct rumpuser_iovec) */
struct hiovec {
	void	*iov_base;
	size_t	iov_len;
};

struct virtif_sc;
void	rump_virtif_sana_deliverpkt(struct virtif_sc *, struct hiovec *, size_t);

/* every request we hand to the driver is one of these */
struct s2req {
	struct IOSana2Req ios2;
	int	kind;			/* REQ_RX, REQ_TX or REQ_EV */
	int	busy;			/* sent to the driver, not yet back */
	UWORD	rxtype;			/* REQ_RX: the packet type asked for */
	struct s2req *next;		/* free list (transmit), parked reads */
	UBYTE	buf[ETHER_HDR_LEN + FRAME_MAX];
};
#define	REQ_RX	1
#define	REQ_TX	2
#define	REQ_EV	3

/* slot states */
#define	SLOT_FREE	0		/* never handed out */
#define	SLOT_CREATING	1
#define	SLOT_LIVE	2		/* the kernel's interface */
#define	SLOT_DEAD	3		/* destroyed: host calls do nothing */

struct virtif_user {
	int	slotstate;		/* Forbid() protects */
	volatile int users;		/* host calls inside; Forbid() */
	volatile int ioactive;		/* the I/O process has not ended */

	struct virtif_sc *sc;
	char	devname[64];
	ULONG	unit;

	struct Task *iotask;
	void	*iothread;		/* rumpuser_thread_create() cookie */
	struct MsgPort *volatile txport;	/* created by the I/O process */
	struct IOSana2Req *base;	/* the opened request (template) */
	volatile int state;		/* 0 starting, 1 running, <0 failed */
	volatile int stopping;

	UBYTE	mac[ETHER_ADDR_LEN];

	struct s2req *txfree;		/* Forbid() protects */
	struct s2req *rxreqs[NRXTYPES * NRX_PER_TYPE];
	struct s2req *txreqs[NTX];
	struct s2req *rxparked;		/* failed reads waiting to go again */

	volatile ULONG rx_packets, rx_dropped, tx_packets, tx_dropped;

	/* link state from S2_ONEVENT */
	struct s2req *evreq;
	int	evparked;		/* failed: queued again by the timer */
	int	evfails;		/* failed in a row (not aborted) */
	volatile int link;
	int events;			/* 0 none, 1 ONLINE/OFFLINE, 2 + CONNECT */
	int evseen;			/* an event has really come */
	int wireless;
	int paulanet;			/* PaulaNET.device: its quirks */
	int nomcast;			/* S2_MULTICAST refused: CMD_WRITE */
	int nomcastfilter;		/* S2_ADD/DELMULTICASTADDRESS refused */
	int rxerrs;			/* reads failed in a row */
	int rxdown;			/* the link is down because of them */
	int rxbig;			/* an oversized frame was reported */
	volatile int abandoned;		/* nobody waits for the I/O process */
	sana_link_fn linkhook;
	void	*linkctx;

	/* host-side frame tap (DHCP client): sees frames before the kernel */
	sana_tap_fn tap;
	void	*tapctx;
};

static struct virtif_user viutab[MAXVIU];

/* ------------------------------------------------------------------------
 * buffer management hooks: called by the driver with register arguments
 * (A0 = to, A1 = from, D0 = length), possibly from interrupt code.
 */

ULONG s2_copytobuff(UBYTE *to, const UBYTE *from, ULONG n);
ULONG s2_copyfrombuff(UBYTE *to, const UBYTE *from, ULONG n);

ULONG
s2_copytobuff(UBYTE *to, const UBYTE *from, ULONG n)
{

	if (n > FRAME_MAX)
		return FALSE;
	while (n--)
		*to++ = *from++;
	return TRUE;
}

ULONG
s2_copyfrombuff(UBYTE *to, const UBYTE *from, ULONG n)
{

	if (n > FRAME_MAX)
		return FALSE;
	while (n--)
		*to++ = *from++;
	return TRUE;
}

void s2_copytobuff_reg(void);
void s2_copyfrombuff_reg(void);

__asm__(
"	.text\n"
"	.globl	s2_copytobuff_reg\n"
"s2_copytobuff_reg:\n"
"	move.l	%d0,-(%sp)\n"
"	move.l	%a1,-(%sp)\n"
"	move.l	%a0,-(%sp)\n"
"	jsr	s2_copytobuff\n"
"	lea	12(%sp),%sp\n"
"	rts\n"
"	.globl	s2_copyfrombuff_reg\n"
"s2_copyfrombuff_reg:\n"
"	move.l	%d0,-(%sp)\n"
"	move.l	%a1,-(%sp)\n"
"	move.l	%a0,-(%sp)\n"
"	jsr	s2_copyfrombuff\n"
"	lea	12(%sp),%sp\n"
"	rts\n");

static struct TagItem s2_buffertags[] = {
	{ S2_CopyToBuff, (ULONG)s2_copytobuff_reg },
	{ S2_CopyFromBuff, (ULONG)s2_copyfrombuff_reg },
	{ TAG_DONE, 0 }
};

/* ------------------------------------------------------------------------
 * helpers
 */

static int
name_eq(const char *a, const char *b)
{

	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int
parse_linkstr(char *devname, size_t size, ULONG *unitp, const char *s)
{
	/* "<driver>[:<unit>]"; the driver may be a path with a volume name
	   ("PaulaNET:PaulaNET.device:0"), so the unit is after the last ':'
	   and only if it is all digits */
	size_t i, len = 0, end;
	ULONG unit = 0;
	const char *p;

	while (s[len])
		len++;
	end = len;
	for (p = s + len; p > s && p[-1] >= '0' && p[-1] <= '9'; p--)
		;
	if (p < s + len && p > s && p[-1] == ':') {
		end = p - 1 - s;
		for (; *p; p++)
			unit = unit * 10 + (*p - '0');
	}
	if (end == 0 || end >= size)
		return RUMPUSER_EINVAL;
	for (i = 0; i < end; i++)
		devname[i] = s[i];
	devname[i] = '\0';
	*unitp = unit;
	return 0;
}

/* a host call may use the driver's requests and ports until user_exit() */
static int
user_enter(struct virtif_user *viu)
{
	int ok;

	Forbid();
	ok = viu->slotstate == SLOT_LIVE;
	if (ok)
		viu->users++;
	Permit();
	return ok;
}

static void
user_exit(struct virtif_user *viu)
{

	Forbid();
	viu->users--;
	Permit();
}

static struct s2req *
req_alloc(struct IOSana2Req *base, int kind)
{
	struct s2req *r;

	if ((r = AllocVec(sizeof(*r), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return NULL;
	CopyMem(base, &r->ios2, sizeof(r->ios2));
	r->kind = kind;
	return r;
}

static void
queue_read(struct s2req *r, struct MsgPort *port)
{

	r->ios2.ios2_Req.io_Message.mn_ReplyPort = port;
	r->ios2.ios2_Req.io_Command = CMD_READ;
	r->ios2.ios2_Req.io_Flags = 0;
	/* (the driver writes the received frame's type here: the type to
	   ask for again is r->rxtype) */
	r->ios2.ios2_PacketType = r->rxtype;
	/* leave room in front to rebuild the Ethernet header in place */
	r->ios2.ios2_Data = r->buf + ETHER_HDR_LEN;
	r->ios2.ios2_DataLength = FRAME_MAX;
	r->busy = 1;
	SendIO((struct IORequest *)&r->ios2);
}

/* ask for the next link change: the opposite of the current state */
static void
queue_event(struct virtif_user *viu, struct MsgPort *port)
{
	struct s2req *r = viu->evreq;
	ULONG mask;

	if (viu->wireless && viu->events == 2)
		/* association, not the unit's online state (which drivers
		   report at once and which says nothing about the radio) */
		mask = viu->link ? S2EVENT_DISCONNECT : S2EVENT_CONNECT;
	else if (viu->link)
		mask = S2EVENT_OFFLINE | (viu->events == 2 ? S2EVENT_DISCONNECT : 0);
	else
		mask = S2EVENT_ONLINE | (viu->events == 2 ? S2EVENT_CONNECT : 0);
	r->ios2.ios2_Req.io_Message.mn_ReplyPort = port;
	r->ios2.ios2_Req.io_Command = S2_ONEVENT;
	r->ios2.ios2_Req.io_Flags = 0;
	r->ios2.ios2_WireError = mask;
	r->busy = 1;
	SendIO((struct IORequest *)&r->ios2);
}

static void
set_link(struct virtif_user *viu, int up)
{
	sana_link_fn fn;

	if (up != viu->link) {
		viu->link = up;
		/* under Forbid(), as tapped() calls the tap: the hook and its
		   context are set and cleared together under Forbid()
		   (sana_set_linkhook(), rumpcomp_sana_destroy()), so they are
		   read together too.  The one hook (src/stack/config.c
		   link_hook()) only stores and signals */
		Forbid();
		if ((fn = viu->linkhook) != NULL)
			fn(viu->linkctx, up);
		Permit();
	}
}

/*
 * An S2_ONEVENT came back.  Only "not supported" (S2ERR_NOT_SUPPORTED,
 * or IOERR_NOCMD from Exec for an unknown command) changes what is asked
 * for; an abort (our AbortIO, or another opener's CMD_FLUSH, which aborts
 * every opener's event requests: genet unit_commands.c:403-462) asks
 * again at once; any other error asks again from the timer, at most
 * EV_RETRIES times in a row, then link events are given up (logged).
 */
static void
event_done(struct virtif_user *viu, struct s2req *r, struct MsgPort *port)
{
	ULONG ev = r->ios2.ios2_WireError;
	BYTE err = r->ios2.ios2_Req.io_Error;
	int up = viu->link;

	if (err == IOERR_ABORTED) {
		if (!viu->stopping)
			queue_event(viu, port);
		return;
	}
	if (err == S2ERR_NOT_SUPPORTED || err == IOERR_NOCMD) {
		viu->evfails = 0;
		if (viu->wireless) {
			/* never fall back to ONLINE/OFFLINE for Wi-Fi: it says
			   nothing about the radio; the poll decides */
			viu->events = 0;
			return;
		}
		/* try plain ONLINE/OFFLINE, then give up */
		if (viu->events == 2) {
			viu->events = 1;
			if (!viu->stopping)
				queue_event(viu, port);
		} else
			viu->events = 0;
		return;
	}
	if (err) {
		if (++viu->evfails > EV_RETRIES) {
			amiga_rump_printf("sana: %s: link events fail (error %d, "
			    "%ld) %d times in a row; not asked for any more\n",
			    viu->devname, err, (long)ev, EV_RETRIES);
			viu->events = 0;
			return;
		}
		viu->evparked = 1;
		return;
	}
	viu->evfails = 0;
	viu->evseen = 1;
	if (ev & (S2EVENT_OFFLINE | S2EVENT_DISCONNECT))
		up = 0;
	if (ev & (S2EVENT_ONLINE | S2EVENT_CONNECT))
		up = 1;
	set_link(viu, up);
	if (!viu->stopping)
		queue_event(viu, port);
}

/*
 * Wi-Fi: associated with an access point?  S2_GETSIGNALQUALITY only
 * succeeds while associated.  1 yes, 0 no, -1 the driver cannot tell.
 * Uses its own request and port, so the I/O port's signals are not eaten.
 */
static int
wireless_associated(struct IOSana2Req *base)
{
	struct MsgPort *port = CreateMsgPort();
	struct IOSana2Req *q;
	LONG sq[2];
	int rv = -1;

	if (port == NULL)
		return -1;
	if ((q = (struct IOSana2Req *)CreateIORequest(port, sizeof(*q))) != NULL) {
		CopyMem(base, q, sizeof(*q));
		q->ios2_Req.io_Message.mn_ReplyPort = port;
		q->ios2_Req.io_Command = S2_GETSIGNALQUALITY;
		q->ios2_Req.io_Flags = 0;
		q->ios2_StatData = sq;
		if (DoIO((struct IORequest *)q) == 0)
			rv = 1;
		else if (q->ios2_Req.io_Error != IOERR_NOCMD &&
		    q->ios2_Req.io_Error != S2ERR_NOT_SUPPORTED)
			rv = 0;
		DeleteIORequest((struct IORequest *)q);
	}
	DeleteMsgPort(port);
	return rv;
}

/*
 * Get a request back from the driver at shutdown.  Some drivers cannot
 * abort every command (S2_ONEVENT, for one), so wait at most about two
 * seconds: a request that does not come back stays the driver's, and
 * returns -1 so its memory, the reply port and the open device are left
 * alone.
 */
static int
reap(struct s2req *r, int abort, int *budget)
{

	if (!r->busy)
		return 0;
	/* (abort 0: the driver's AbortIO is not safe - PaulaNET Remove()s
	   a request that may have completed meanwhile - or not needed) */
	if (abort && !CheckIO((struct IORequest *)&r->ios2))
		AbortIO((struct IORequest *)&r->ios2);
	/* (one budget for all of them: not two seconds each) */
	while (*budget > 0 && !CheckIO((struct IORequest *)&r->ios2)) {
		amiga_host_sleep_ms(20);
		*budget -= 20;
	}
	if (!CheckIO((struct IORequest *)&r->ios2))
		return -1;
	WaitIO((struct IORequest *)&r->ios2);
	r->busy = 0;
	return 0;
}

/*
 * Drivers that failed (no hardware, would not open, configure or go
 * online) are not tried again until the next boot: a failed open can
 * leave a driver half set up (genet.device then "opens" a dead unit the
 * next time), and a driver whose hardware is not there must not be
 * poked again by a reconfiguration.
 */
#define	MAXFAILED	8
static char failed[MAXFAILED][32];
static int nfailed;

static int
failed_before(const char *name)
{
	const char *b = hw_basename(name);
	int i, f = 0;

	Forbid();
	for (i = 0; i < nfailed && !f; i++)
		f = hw_eq_nocase(failed[i], b);
	Permit();
	return f;
}

static void
mark_failed(const char *name)
{
	const char *b = hw_basename(name);

	if (failed_before(name))
		return;
	Forbid();
	if (nfailed < MAXFAILED)
		hw_copy(failed[nfailed++], b, sizeof(failed[0]));
	Permit();
}

/* a driver is wireless if NSCMD_DEVICEQUERY lists S2_GETNETWORKS */
static int
probe_wireless(struct IOSana2Req *base)
{
	struct NSDeviceQueryResult nsq;
	struct IOStdReq *io = (struct IOStdReq *)base;
	UWORD *cmd;
	int w = 0;

	memset(&nsq, 0, sizeof(nsq));
	io->io_Command = NSCMD_DEVICEQUERY;
	io->io_Data = &nsq;
	io->io_Length = sizeof(nsq);
	if (DoIO((struct IORequest *)io) == 0 && nsq.SupportedCommands)
		for (cmd = nsq.SupportedCommands; *cmd; cmd++)
			if (*cmd == S2_GETNETWORKS)
				w = 1;
	return w;
}

static int
is_zero_mac(const UBYTE *m)
{

	return !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

static void rebuild_header(struct s2req *);

/* give the host tap a look first; returns 1 if it consumed the frame */
static int
tapped(struct virtif_user *viu, struct s2req *r)
{
	sana_tap_fn fn;
	int taken;

	if (viu->tap == NULL)
		return 0;
	rebuild_header(r);
	/*
	 * Under Forbid(): the DHCP client may clear the tap and free its
	 * context at any time.  Tap functions must not block (the DHCP one
	 * only copies and signals).
	 */
	Forbid();
	fn = viu->tap;
	taken = fn != NULL && fn(viu->tapctx, r->buf,
	    ETHER_HDR_LEN + r->ios2.ios2_DataLength);
	Permit();
	if (!taken)
		return 0;
	viu->rx_packets++;	/* received, even if not by the kernel */
	return 1;
}

static void
rebuild_header(struct s2req *r)
{
	UBYTE *hdr = r->buf;
	int i;

	for (i = 0; i < ETHER_ADDR_LEN; i++) {
		hdr[i] = r->ios2.ios2_DstAddr[i];
		hdr[ETHER_ADDR_LEN + i] = r->ios2.ios2_SrcAddr[i];
	}
	/* some drivers leave DstAddr empty for broadcasts */
	if ((r->ios2.ios2_Req.io_Flags & SANA2IOF_BCAST) || is_zero_mac(hdr))
		for (i = 0; i < ETHER_ADDR_LEN; i++)
			hdr[i] = 0xff;
	hdr[12] = (UBYTE)(r->ios2.ios2_PacketType >> 8);
	hdr[13] = (UBYTE)r->ios2.ios2_PacketType;
}

/*
 * Deliver as ONE contiguous frame: if_virt.c's VIF_DELIVERPKT miscounts
 * when given more than one iovec ("m_copyback failed").  The length was
 * checked against the buffer by the caller.
 */
static void
deliver(struct virtif_user *viu, struct s2req *r)
{
	struct hiovec iov;

	rebuild_header(r);
	iov.iov_base = r->buf;
	iov.iov_len = ETHER_HDR_LEN + r->ios2.ios2_DataLength;

	rumpuser_component_schedule(NULL);
	rump_virtif_sana_deliverpkt(viu->sc, &iov, 1);
	rumpuser_component_unschedule();
	viu->rx_packets++;
}

/* ------------------------------------------------------------------------
 * the I/O process
 */

static void *
sana_iothread(void *arg)
{
	struct virtif_user *viu = arg;
	struct MsgPort *ioport = NULL, *txport = NULL, *tport = NULL;
	struct MsgPort *rport = NULL;
	struct timerequest *treq = NULL, *rtreq = NULL;
	struct IOSana2Req *base = NULL;
	struct s2req *r;
	ULONG iomask, txmask, tmask = 0, rmask, sigs;
	int opened = 0, nrx = 0, ntx = 0, stuck = 0, treqpending = 0;
	int rpending = 0, txlogged = 0;
	int budget;
	unsigned i, t;

	viu->iotask = SysBase->ThisTask;
	rumpuser_component_kthread();

	if ((ioport = CreateMsgPort()) == NULL ||
	    (txport = CreateMsgPort()) == NULL)
		goto fail;
	base = (struct IOSana2Req *)CreateIORequest(ioport, sizeof(*base));
	if (base == NULL)
		goto fail;
	base->ios2_BufferManagement = s2_buffertags;
	viu->paulanet = amibsdnet_is_paulanet(viu->devname);
	if (failed_before(viu->devname)) {
		amiga_rump_printf("sana: %s failed before: not tried again until "
		    "the next boot\n", viu->devname);
		goto fail;
	}
	/* a Pi driver only with its hardware (amibsdnet/hwcheck.h) */
	{
		char why[100];

		if (!hw_check(viu->devname, viu->unit, why, sizeof(why))) {
			amiga_rump_printf("sana: %s not used: %s\n",
			    viu->devname, why);
			mark_failed(viu->devname);
			goto fail;
		}
	}
	/* (each step is logged first: the start notice shows it on screen,
	   so a driver that freezes the Amiga is seen doing so) */
	amiga_rump_printf("sana: opening %s unit %lu\n", viu->devname,
	    viu->unit);
	/* drivers live in DEVS:Networks: a plain name is tried again as
	   "DEVS:Networks/<name>" (amibsdnet/devopen.h) */
	if (amibsdnet_open_sana(viu->devname, viu->unit,
	    (struct IORequest *)base, 0) != 0) {
		amiga_rump_printf("sana: cannot open %s unit %lu (error %d; "
		    "not found in DEVS:Networks?)\n", viu->devname, viu->unit,
		    base->ios2_Req.io_Error);
		mark_failed(viu->devname);
		goto fail;
	}
	opened = 1;
	/* (genet.device's version was checked before the open, in its
	   file: hw_check() in amibsdnet/hwcheck.h) */

	amiga_rump_printf("sana: %s open; configuring it\n", viu->devname);
	/* station address: current one, else the factory default */
	base->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
	if (DoIO((struct IORequest *)base) == 0) {
		CopyMem(base->ios2_SrcAddr, viu->mac, ETHER_ADDR_LEN);
		if (is_zero_mac(viu->mac))
			CopyMem(base->ios2_DstAddr, viu->mac, ETHER_ADDR_LEN);
	}
	/* configure the interface (fails harmlessly if already configured) */
	base->ios2_Req.io_Command = S2_CONFIGINTERFACE;
	CopyMem(viu->mac, base->ios2_SrcAddr, ETHER_ADDR_LEN);
	/*
	 * A driver that cannot configure itself - its hardware is missing or
	 * does not answer (genet.device without a working GENET/PHY says
	 * S2ERR_SOFTWARE) - gets nothing more: S2_ONLINE on such a unit
	 * froze a PiStorm.  Only "configured already" (BAD_STATE) and "not
	 * needed" (NOT_SUPPORTED, no such command) carry on.
	 */
	if (DoIO((struct IORequest *)base) != 0 &&
	    base->ios2_Req.io_Error != S2ERR_BAD_STATE &&
	    base->ios2_Req.io_Error != S2ERR_NOT_SUPPORTED &&
	    base->ios2_Req.io_Error != IOERR_NOCMD) {
		/* (genet: S2ERR_SOFTWARE also when no link comes in 6 s -
		   phy.h:19, phy.c:343, bcmgenet.c:605-609 - and when its
		   interrupt or PHY set-up fails, bcmgenet.c:594) */
		amiga_rump_printf("sana: %s: cannot be configured (error %d, "
		    "%ld)%s; not used until the next boot\n",
		    viu->devname, base->ios2_Req.io_Error,
		    (long)base->ios2_WireError,
		    hw_eq_nocase(hw_basename(viu->devname), "genet.device") &&
		    base->ios2_Req.io_Error == S2ERR_SOFTWARE ?
		    " - no link within 6 seconds (is the cable plugged in?), "
		    "or its interrupt or PHY set-up failed" : "");
		mark_failed(viu->devname);
		goto fail;
	}
	if (is_zero_mac(viu->mac)) {
		base->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
		if (DoIO((struct IORequest *)base) == 0)
			CopyMem(base->ios2_SrcAddr, viu->mac, ETHER_ADDR_LEN);
	}
	/* no address: frames could not be sent or received (DHCP would ask
	   with 00:00:00:00:00:00) */
	if (is_zero_mac(viu->mac)) {
		amiga_rump_printf("sana: %s gives no station address; not "
		    "used\n", viu->devname);
		mark_failed(viu->devname);
		goto fail;
	}
	amiga_rump_printf("sana: %s: asking what it can do\n", viu->devname);
	viu->wireless = probe_wireless(base);
	/* NSCMD_DEVICEQUERY's io_Data and io_Offset lie on ios2_SrcAddr
	   (IOStdReq io_Data at 40, io_Offset at 44; IOSana2Req ios2_SrcAddr
	   at 40-55, devices/sana2.h), which every request below inherits */
	CopyMem(viu->mac, base->ios2_SrcAddr, ETHER_ADDR_LEN);
	base->ios2_Req.io_Command = S2_ONLINE;
	amiga_rump_printf("sana: %s: going online\n", viu->devname);
	/* a unit that cannot go online gets nothing more */
	if (DoIO((struct IORequest *)base) != 0 &&
	    base->ios2_Req.io_Error != S2ERR_NOT_SUPPORTED &&
	    base->ios2_Req.io_Error != IOERR_NOCMD) {
		amiga_rump_printf("sana: %s: cannot go online (error %d, %ld); "
		    "not used until the next boot\n", viu->devname,
		    base->ios2_Req.io_Error, (long)base->ios2_WireError);
		mark_failed(viu->devname);
		goto fail;
	}
	amiga_rump_printf("sana: %s unit %lu, address %x:%x:%x:%x:%x:%x\n",
	    viu->devname, viu->unit, viu->mac[0], viu->mac[1], viu->mac[2],
	    viu->mac[3], viu->mac[4], viu->mac[5]);
	viu->link = 1;
	/* (multicast groups come from the kernel: rumpcomp_sana_mcast()) */
	/*
	 * Wi-Fi: the link is the association with the access point, which
	 * WirelessManager makes later (drivers that cannot tell count as
	 * linked).  Checked again every few seconds below.
	 */
	if (viu->wireless) {
		amiga_rump_printf("sana: %s: associated?\n", viu->devname);
		if (wireless_associated(base) == 0)
			viu->link = 0;
	}

	/* timer for requests that failed and go again later */
	if ((rport = CreateMsgPort()) == NULL)
		goto fail;
	rtreq = (struct timerequest *)CreateIORequest(rport, sizeof(*rtreq));
	if (rtreq == NULL || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
	    (struct IORequest *)rtreq, 0) != 0) {
		if (rtreq)
			DeleteIORequest((struct IORequest *)rtreq);
		rtreq = NULL;
		goto fail;
	}
	rmask = 1UL << rport->mp_SigBit;

	for (t = 0; t < NRXTYPES; t++) {
		for (i = 0; i < NRX_PER_TYPE; i++) {
			if ((r = req_alloc(base, REQ_RX)) == NULL)
				goto fail;
			viu->rxreqs[nrx++] = r;
			r->rxtype = rxtypes[t];
			queue_read(r, ioport);
		}
	}
	for (i = 0; i < NTX; i++) {
		if ((r = req_alloc(base, REQ_TX)) == NULL)
			goto fail;
		r->ios2.ios2_Req.io_Message.mn_ReplyPort = ioport;
		viu->txreqs[ntx++] = r;
		r->next = viu->txfree;
		viu->txfree = r;
	}

	/* link change notification, if the driver can do it */
	if ((viu->evreq = req_alloc(base, REQ_EV)) != NULL) {
		viu->events = 2;
		queue_event(viu, ioport);
	}

	/* (the creator gave up waiting: it is not used) */
	Forbid();
	if (viu->abandoned) {
		Permit();
		goto fail;
	}
	viu->base = base;
	viu->txport = txport;
	viu->state = 1;
	Permit();
	iomask = 1UL << ioport->mp_SigBit;
	txmask = 1UL << txport->mp_SigBit;

	/*
	 * Wi-Fi: besides the CONNECT/DISCONNECT events, look at the
	 * association every few seconds (first after one second, which also
	 * covers an association made while the event request was queued).
	 * Events can be missed or not offered; this cannot.
	 */
	if (viu->wireless && (tport = CreateMsgPort()) != NULL) {
		treq = (struct timerequest *)CreateIORequest(tport,
		    sizeof(*treq));
		if (treq == NULL || OpenDevice((CONST_STRPTR)TIMERNAME,
		    UNIT_VBLANK, (struct IORequest *)treq, 0) != 0) {
			if (treq)
				DeleteIORequest((struct IORequest *)treq);
			treq = NULL;
		} else {
			tmask = 1UL << tport->mp_SigBit;
			treq->tr_node.io_Command = TR_ADDREQUEST;
			treq->tr_time.tv_secs = 1;
			treq->tr_time.tv_micro = 0;
			SendIO((struct IORequest *)treq);
			treqpending = 1;
		}
	}

	while (!viu->stopping) {
		sigs = Wait(iomask | txmask | tmask | rmask | SIGBREAKF_CTRL_C);

		/* failed requests: their wait is over */
		if (rpending && (sigs & rmask) &&
		    CheckIO((struct IORequest *)rtreq)) {
			WaitIO((struct IORequest *)rtreq);
			rpending = 0;
			while ((r = viu->rxparked) != NULL) {
				viu->rxparked = r->next;
				queue_read(r, ioport);
			}
			if (viu->evparked) {
				viu->evparked = 0;
				queue_event(viu, ioport);
			}
		}
		if (treq && treqpending && (sigs & tmask) &&
		    CheckIO((struct IORequest *)treq)) {
			int a;

			WaitIO((struct IORequest *)treq);
			if ((a = wireless_associated(base)) >= 0 &&
			    a != viu->link) {
				set_link(viu, a);
				/* the event request waits for the old edge
				   (CONNECT while associated): ask again for
				   the other one (event_done re-queues an
				   aborted request with the new mask) */
				if (viu->evreq && viu->evreq->busy)
					AbortIO((struct IORequest *)
					    &viu->evreq->ios2);
			}
			/*
			 * Again in 5 s for drivers that do not report
			 * CONNECT/DISCONNECT; every 30 s for one that accepted
			 * the event request but has not sent an event yet;
			 * not at all once events
			 * come.  Each query makes wifipi.device talk to its
			 * firmware from this process, through a memory pool
			 * its own tasks use unprotected - asking all the time
			 * invites a crash there.
			 */
			if (viu->events != 2 || !viu->evseen) {
				treq->tr_node.io_Command = TR_ADDREQUEST;
				treq->tr_time.tv_secs =
				    viu->events != 2 ? 5 : 30;
				treq->tr_time.tv_micro = 0;
				SendIO((struct IORequest *)treq);
				treqpending = 1;
			} else
				treqpending = 0;
		}
		/* frames posted by the kernel */
		while ((r = (struct s2req *)GetMsg(txport)) != NULL) {
			r->ios2.ios2_Req.io_Message.mn_ReplyPort = ioport;
			r->busy = 1;
			SendIO((struct IORequest *)&r->ios2);
		}
		/* completed requests */
		while ((r = (struct s2req *)GetMsg(ioport)) != NULL) {
			r->busy = 0;
			if (r->kind == REQ_EV) {
				/* mixed in, never credited: network events
				   are not counted as entropy (NetBSD sets
				   RND_FLAG_NO_COLLECT for RND_TYPE_NET,
				   netbsd-src/sys/kern/kern_entropy.c:
				   1805-1811) */
				amiga_entropy_event(ENT_SRC_EVENT, 0, NULL, 0);
				event_done(viu, r, ioport);
				continue;
			}
			if (r->kind == REQ_RX) {
				if (r->ios2.ios2_Req.io_Error == 0 &&
				    r->ios2.ios2_DataLength > FRAME_MAX) {
					/* more than the buffer holds: the
					   driver's length cannot be used */
					viu->rx_dropped++;
					if (!viu->rxbig++)
						amiga_rump_printf("sana: %s: "
						    "frame of %lu bytes "
						    "reported for a %d byte "
						    "buffer; dropped\n",
						    viu->devname,
						    (unsigned long)r->ios2.
						    ios2_DataLength, FRAME_MAX);
				} else if (r->ios2.ios2_Req.io_Error == 0) {
					/* frame arrival: mixed in, never
					   credited (an adversary can set
					   packet timing; kern_entropy.c:
					   1805-1811) */
					amiga_entropy_event(ENT_SRC_RX, 0,
					    NULL, 0);
					viu->rxerrs = 0;
					if (viu->rxdown) {
						viu->rxdown = 0;
						set_link(viu, 1);
					}
					/* (stopping: the kernel's interface
					   may be gone) */
					if (!viu->stopping && !tapped(viu, r))
						deliver(viu, r);
				} else {
					viu->rx_dropped++;
					/* every read failing, again and again
					   (twice round): the link is down, said
					   once */
					if (++viu->rxerrs == 2 * NRXTYPES *
					    NRX_PER_TYPE && viu->link) {
						amiga_rump_printf("sana: %s: reads "
						    "fail (error %d, %ld): no "
						    "link\n", viu->devname,
						    r->ios2.ios2_Req.io_Error,
						    (long)r->ios2.ios2_WireError);
						viu->rxdown = 1;
						set_link(viu, 0);
						/* (the event request waits for
						   the old edge; not PaulaNET's:
						   its AbortIO is not safe) */
						if (viu->evreq &&
						    viu->evreq->busy &&
						    !viu->paulanet)
							AbortIO((struct
							    IORequest *)
							    &viu->evreq->ios2);
					}
					/* a driver that rejects reads (offline)
					   must not make this loop spin: the
					   read goes again from the timer,
					   and the loop carries on */
					r->next = viu->rxparked;
					viu->rxparked = r;
					continue;
				}
				if (!viu->stopping)
					queue_read(r, ioport);
			} else {
				amiga_entropy_event(ENT_SRC_TX, 0, NULL, 0);
				if (r->ios2.ios2_Req.io_Error == 0)
					viu->tx_packets++;
				else if (r->ios2.ios2_Req.io_Command ==
				    S2_MULTICAST &&
				    (r->ios2.ios2_Req.io_Error ==
				    S2ERR_NOT_SUPPORTED ||
				    r->ios2.ios2_Req.io_Error == IOERR_NOCMD)) {
					/* (this one is lost; the protocols
					   send again) */
					viu->tx_dropped++;
					if (!viu->nomcast)
						amiga_rump_printf("sana: %s has "
						    "no S2_MULTICAST: multicast "
						    "is sent with CMD_WRITE\n",
						    viu->devname);
					viu->nomcast = 1;
				} else if (viu->tx_dropped++, !txlogged++)
					/* (only the first: says why) */
					amiga_rump_printf("sana: %s: sending "
					    "failed (error %d, %ld)\n",
					    viu->devname,
					    r->ios2.ios2_Req.io_Error,
					    (long)r->ios2.ios2_WireError);
				Forbid();
				r->next = viu->txfree;
				viu->txfree = r;
				Permit();
			}
		}
		if (sigs & SIGBREAKF_CTRL_C)
			viu->stopping = 1;
		/* failed requests: go again after a pause (longer once every
		   read has failed twice round) */
		if (!rpending && !viu->stopping &&
		    (viu->rxparked || viu->evparked)) {
			rtreq->tr_node.io_Command = TR_ADDREQUEST;
			if (viu->rxparked) {
				rtreq->tr_time.tv_secs = 0;
				rtreq->tr_time.tv_micro = viu->rxerrs >
				    2 * NRXTYPES * NRX_PER_TYPE ?
				    250000 : 40000;
			} else {
				rtreq->tr_time.tv_secs = 1;
				rtreq->tr_time.tv_micro = 0;
			}
			SendIO((struct IORequest *)rtreq);
			rpending = 1;
		}
		/* the events stopped working (event_done above): back
		   to asking */
		if (treq && !treqpending && viu->events != 2 && !viu->stopping) {
			treq->tr_node.io_Command = TR_ADDREQUEST;
			treq->tr_time.tv_secs = 5;
			treq->tr_time.tv_micro = 0;
			SendIO((struct IORequest *)treq);
			treqpending = 1;
		}
	}

	/* shut down: abort and reap what the driver still has.  Only those:
	   WaitIO() on a request already taken off the reply port would
	   Remove() it a second time and corrupt the port's list. */
	viu->txport = NULL;
	if (treq) {
		if (treqpending) {
			if (!CheckIO((struct IORequest *)treq))
				AbortIO((struct IORequest *)treq);
			WaitIO((struct IORequest *)treq);
		}
		CloseDevice((struct IORequest *)treq);
		DeleteIORequest((struct IORequest *)treq);
	}
	if (tport)
		DeleteMsgPort(tport);
	goto reapall;

fail:
	/* (also part-way through the setup: reads may be queued already) */
	viu->state = -1;
reapall:
	/* host calls still inside use the device and the transmit port */
	for (;;) {
		int busy;

		Forbid();
		busy = viu->users;
		Permit();
		if (!busy)
			break;
		amiga_host_sleep_ms(20);
	}
	viu->base = NULL;
	if (rtreq) {
		if (rpending) {
			if (!CheckIO((struct IORequest *)rtreq))
				AbortIO((struct IORequest *)rtreq);
			WaitIO((struct IORequest *)rtreq);
		}
		CloseDevice((struct IORequest *)rtreq);
		DeleteIORequest((struct IORequest *)rtreq);
	}
	if (rport)
		DeleteMsgPort(rport);
	viu->rxparked = NULL;		/* (not busy: freed below) */
	if (opened && nrx) {
		struct IOSana2Req *q;

		/* on a separate request: base may be in use by nobody, but
		   its fields are what the reads were copied from */
		if ((q = (struct IOSana2Req *)CreateIORequest(ioport,
		    sizeof(*q))) != NULL) {
			CopyMem(base, q, sizeof(*q));
			q->ios2_Req.io_Message.mn_ReplyPort = ioport;
			/*
			 * (PaulaNET, NetDevice/device.c: DevAbortIO,
			 * :487-500, Remove()s without checking the request
			 * is queued; S2_OFFLINE, :431-433, only clears
			 * db_online; rejectAllPackets, :525-530, reads the
			 * next node from a request it has just replied, so
			 * after the first reply the loop is on our port's
			 * list and ends.  Its reads are left to it, and so
			 * is the device.)
			 */
			if (!viu->paulanet && hw_eq_nocase(
			    hw_basename(viu->devname), "genet.device")) {
				/* genet: AbortIO only marks a read
				   (device_abortio.c:37-60); CMD_FLUSH
				   drains the ring and replies them
				   (unit_commands.c:403-462) - also the
				   requests of every other opener */
				for (i = 0; i < (unsigned)nrx; i++)
					if (viu->rxreqs[i]->busy &&
					    !CheckIO((struct IORequest *)
					    &viu->rxreqs[i]->ios2))
						AbortIO((struct IORequest *)
						    &viu->rxreqs[i]->ios2);
				q->ios2_Req.io_Command = CMD_FLUSH;
				DoIO((struct IORequest *)q);
			}
			DeleteIORequest((struct IORequest *)q);
		}
	}
	budget = viu->paulanet ? 100 : 2000;
	for (i = 0; i < (unsigned)nrx; i++)
		if (reap(viu->rxreqs[i], !viu->paulanet, &budget))
			stuck++;
	for (i = 0; i < (unsigned)ntx; i++)	/* (writes end by themselves) */
		if (reap(viu->txreqs[i], 0, &budget))
			stuck++;
	if (viu->evreq && reap(viu->evreq, !viu->paulanet, &budget))
		stuck++;
	if (stuck) {
		amiga_rump_printf("sana: %s keeps %d request(s); leaving it "
		    "open\n", viu->devname, stuck);
		/* this process ends: a late reply must not signal it */
		Forbid();
		ioport->mp_Flags = PA_IGNORE;
		Permit();
	}
	/* frames the kernel posted after the last pass */
	if (txport)
		while (GetMsg(txport) != NULL)
			;

	for (i = 0; i < (unsigned)nrx; i++)
		if (!viu->rxreqs[i]->busy)
			FreeVec(viu->rxreqs[i]);
	for (i = 0; i < (unsigned)ntx; i++)
		if (!viu->txreqs[i]->busy)
			FreeVec(viu->txreqs[i]);
	if (viu->evreq && !viu->evreq->busy)
		FreeVec(viu->evreq);
	viu->evreq = NULL;
	viu->txfree = NULL;
	/* with requests still at the driver, its reply port must stay */
	if (opened && !stuck)
		CloseDevice((struct IORequest *)base);
	if (base && !stuck)
		DeleteIORequest((struct IORequest *)base);
	if (txport)
		DeleteMsgPort(txport);
	if (ioport && !stuck)
		DeleteMsgPort(ioport);
	rumpuser_component_kthread_release();
	/* the slot may be used again from here on */
	Forbid();
	viu->ioactive = 0;
	Permit();
	return NULL;
}

/* ------------------------------------------------------------------------
 * virtif hypercalls (called from the kernel, scheduled)
 */

int
rumpcomp_sana_create(const char *linkstr, struct virtif_sc *sc,
    uint8_t *enaddr, struct virtif_user **viup)
{
	struct virtif_user *viu = NULL, *v;
	char devname[sizeof(viu->devname)];
	ULONG unit;
	void *cookie;
	int rv, i, reused = 0;

	if ((rv = parse_linkstr(devname, sizeof(devname), &unit, linkstr)) != 0)
		return rv;
	/* the slot of this driver and unit, else a new one */
	Forbid();
	for (i = 0; i < MAXVIU && viu == NULL; i++) {
		v = &viutab[i];
		if (v->slotstate == SLOT_FREE || v->unit != unit ||
		    !name_eq(v->devname, devname))
			continue;
		if (v->slotstate != SLOT_DEAD) {
			Permit();
			return RUMPUSER_EBUSY;	/* attached already */
		}
		if (v->ioactive) {
			/* (its I/O process was left to a hung driver) */
			Permit();
			return RUMPUSER_EBUSY;
		}
		viu = v;
		reused = 1;
	}
	for (i = 0; i < MAXVIU && viu == NULL; i++)
		if (viutab[i].slotstate == SLOT_FREE)
			viu = &viutab[i];
	if (viu)
		viu->slotstate = SLOT_CREATING;
	Permit();
	if (viu == NULL) {
		amiga_rump_printf("sana: %s: no room for another interface "
		    "(%d drivers and units used)\n", devname, MAXVIU);
		return S_ENOSPC;
	}
	/* a fresh start (users is 0: nobody enters a slot not LIVE) */
	memset((char *)viu + sizeof(viu->slotstate), 0,
	    sizeof(*viu) - sizeof(viu->slotstate));
	hw_copy(viu->devname, devname, sizeof(viu->devname));
	viu->unit = unit;
	viu->sc = sc;
	viu->ioactive = 1;

	cookie = rumpuser_component_unschedule();
	rv = rumpuser_thread_create(sana_iothread, viu, "AmiBSDNet sana I/O",
	    1, 0, -1, &viu->iothread);
	if (rv != 0)
		viu->ioactive = 0;
	else {
		int ms, gone = 0;

		/* (a thread whose setup failed ends without running) */
		for (ms = 0; viu->state == 0 &&
		    !amiga_host_thread_done(viu->iothread) && ms < 60000;
		    ms += 20)
			amiga_host_sleep_ms(20);
		/* a driver that hangs in its set-up (a firmware wait without
		   a timeout): left to it; the slot is used again only after
		   the thread has ended */
		Forbid();
		if (viu->state == 0 && !amiga_host_thread_done(viu->iothread)) {
			viu->abandoned = 1;
			viu->slotstate = SLOT_DEAD;
			gone = 1;
		}
		Permit();
		if (gone) {
			amiga_host_thread_detach(viu->iothread);
			rumpuser_component_schedule(cookie);
			amiga_rump_printf("sana: %s does not finish its set-up in "
			    "a minute; not used\n", devname);
			mark_failed(devname);
			return RUMPUSER_ENOENT;
		}
		if (viu->state <= 0) {
			amiga_host_thread_join(viu->iothread);
			rv = RUMPUSER_ENOENT;
		}
	}
	rumpuser_component_schedule(cookie);
	if (rv != 0) {
		/* a new slot was never handed out and is free again; one of
		   this driver and unit used before stays theirs (its pointer
		   may still be held: sana_find()), dead, as it was */
		Forbid();
		viu->slotstate = reused ? SLOT_DEAD : SLOT_FREE;
		Permit();
		return rv;
	}

	CopyMem(viu->mac, enaddr, ETHER_ADDR_LEN);
	*viup = viu;
	Forbid();
	viu->slotstate = SLOT_LIVE;
	Permit();
	return 0;
}

void
rumpcomp_sana_destroy(struct virtif_user *viu)
{
	void *cookie = rumpuser_component_unschedule();
	int ms, gone = 0;

	/* no host call gets in any more; the tap and link hook belong to
	   host code that may be gone next */
	Forbid();
	viu->slotstate = SLOT_DEAD;
	viu->tap = NULL;
	viu->tapctx = NULL;
	viu->linkhook = NULL;
	viu->linkctx = NULL;
	Permit();
	viu->stopping = 1;
	Signal(viu->iotask, SIGBREAKF_CTRL_C);
	/* not rumpuser_thread_join(): we are off the rump CPU here; and not
	   for ever (a driver may hang in a command) */
	for (ms = 0; !amiga_host_thread_done(viu->iothread) && ms < 30000;
	    ms += 20)
		amiga_host_sleep_ms(20);
	Forbid();
	if (!amiga_host_thread_done(viu->iothread)) {
		viu->abandoned = 1;
		gone = 1;
	}
	Permit();
	if (gone) {
		amiga_host_thread_detach(viu->iothread);
		rumpuser_component_schedule(cookie);
		amiga_rump_printf("sana: %s does not let go; left to it\n",
		    viu->devname);
		return;
	}
	amiga_host_thread_join(viu->iothread);
	rumpuser_component_schedule(cookie);
}

/* frame from the kernel (or from sana_raw_send()) */
static void
send_frame(struct virtif_user *viu, struct hiovec *iov, size_t iovlen)
{
	static const UBYTE bcast[ETHER_ADDR_LEN] =
	    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	UBYTE hdr[ETHER_HDR_LEN];
	struct MsgPort *port = viu->txport;
	struct s2req *r;
	size_t i, n, off = 0, plen = 0;
	int j;

	if (port == NULL)
		return;
	Forbid();
	if ((r = viu->txfree) != NULL)
		viu->txfree = r->next;
	Permit();
	if (r == NULL) {
		viu->tx_dropped++;	/* the protocols above will retransmit */
		return;
	}

	/* split the frame: 14 header bytes, payload into the request */
	for (i = 0; i < iovlen; i++) {
		const UBYTE *p = iov[i].iov_base;

		for (n = 0; n < iov[i].iov_len; n++, off++) {
			if (off < ETHER_HDR_LEN)
				hdr[off] = p[n];
			else if (plen < FRAME_MAX)
				r->buf[plen++] = p[n];
		}
	}
	if (off < ETHER_HDR_LEN) {
		Forbid();
		r->next = viu->txfree;
		viu->txfree = r;
		Permit();
		return;
	}

	for (j = 0; j < ETHER_ADDR_LEN; j++) {
		r->ios2.ios2_DstAddr[j] = hdr[j];
		r->ios2.ios2_SrcAddr[j] = hdr[ETHER_ADDR_LEN + j];
	}
	r->ios2.ios2_PacketType = ((ULONG)hdr[12] << 8) | hdr[13];
	r->ios2.ios2_Data = r->buf;
	r->ios2.ios2_DataLength = plen;
	r->ios2.ios2_Req.io_Flags = 0;
	for (j = 0; j < ETHER_ADDR_LEN && hdr[j] == bcast[j]; j++)
		;
	if (j == ETHER_ADDR_LEN)
		r->ios2.ios2_Req.io_Command = S2_BROADCAST;
	else if ((hdr[0] & 1) && !viu->nomcast)
		r->ios2.ios2_Req.io_Command = S2_MULTICAST;
	else
		r->ios2.ios2_Req.io_Command = CMD_WRITE;

	PutMsg(port, &r->ios2.ios2_Req.io_Message);
}

void
rumpcomp_sana_send(struct virtif_user *viu, struct hiovec *iov, size_t iovlen)
{

	if (!user_enter(viu))
		return;
	send_frame(viu, iov, iovlen);
	user_exit(viu);
}

/*
 * The kernel's multicast list changed (src/kern/if_virt.c, SIOCADDMULTI
 * and SIOCDELMULTI after ether_ioctl() said ENETRESET): one Ethernet
 * group address to add to or remove from the driver's filter.  Both
 * address fields carry it: genet reads ios2_SrcAddr only for the
 * single-address commands (unit_commands_mcast.c:60-61, 119-120), and
 * wifipi's delete takes the upper bound from ios2_DstAddr unless the
 * command is S2_ADDMULTICASTADDRESS (unit.c:1311-1324), so for its
 * delete DstAddr must equal SrcAddr.  A driver without the commands
 * (PaulaNET answers S2ERR_NOT_SUPPORTED for every command it does not
 * list, device.c:462-466) has no filter to keep: that is not an error.
 */
int
rumpcomp_sana_mcast(struct virtif_user *viu, int add, const uint8_t *addr)
{
	struct MsgPort *port;
	struct IOSana2Req *q;
	void *cookie;
	BYTE err;
	ULONG werr;
	int rv = 0, logged;

	if (!user_enter(viu))
		return S_ENXIO;
	cookie = rumpuser_component_unschedule();
	if ((port = CreateMsgPort()) == NULL)
		rv = S_ENOMEM;
	else if ((q = (struct IOSana2Req *)CreateIORequest(port,
	    sizeof(*q))) == NULL) {
		DeleteMsgPort(port);
		rv = S_ENOMEM;
	} else {
		CopyMem(viu->base, q, sizeof(*q));
		q->ios2_Req.io_Message.mn_ReplyPort = port;
		q->ios2_Req.io_Command = add ? S2_ADDMULTICASTADDRESS :
		    S2_DELMULTICASTADDRESS;
		q->ios2_Req.io_Flags = 0;
		memset(q->ios2_SrcAddr, 0, sizeof(q->ios2_SrcAddr));
		memset(q->ios2_DstAddr, 0, sizeof(q->ios2_DstAddr));
		CopyMem((APTR)addr, q->ios2_SrcAddr, ETHER_ADDR_LEN);
		CopyMem((APTR)addr, q->ios2_DstAddr, ETHER_ADDR_LEN);
		DoIO((struct IORequest *)q);
		err = q->ios2_Req.io_Error;
		werr = q->ios2_WireError;
		DeleteIORequest((struct IORequest *)q);
		DeleteMsgPort(port);
		if (err == S2ERR_NOT_SUPPORTED || err == IOERR_NOCMD) {
			Forbid();
			logged = viu->nomcastfilter++;
			Permit();
			if (!logged)
				amiga_rump_printf("sana: %s has no multicast "
				    "filter commands; the groups are not "
				    "given to it\n", viu->devname);
		} else if (err) {
			amiga_rump_printf("sana: %s: multicast %s "
			    "%x:%x:%x:%x:%x:%x failed (error %d, %ld)\n",
			    viu->devname, add ? "add" : "delete", addr[0],
			    addr[1], addr[2], addr[3], addr[4], addr[5], err,
			    (long)werr);
			rv = err == S2ERR_NO_RESOURCES ? S_ENOMEM : S_EIO;
		}
	}
	rumpuser_component_schedule(cookie);
	user_exit(viu);
	return rv;
}

/* ------------------------------------------------------------------------
 * host-side access (sana2_host.h).  The pointer stays valid memory (see
 * the top of the file); a destroyed interface reads as down, with the
 * counters and address it had.
 */

struct virtif_user *
sana_find(const char *devname, ULONG unit)
{
	struct virtif_user *v = NULL;
	int i;

	Forbid();
	for (i = 0; i < MAXVIU && v == NULL; i++)
		if (viutab[i].slotstate == SLOT_LIVE &&
		    viutab[i].unit == unit &&
		    name_eq(viutab[i].devname, devname))
			v = &viutab[i];
	Permit();
	return v;
}

const UBYTE *
sana_macaddr(struct virtif_user *viu)
{

	return viu->mac;
}

void
sana_set_tap(struct virtif_user *viu, sana_tap_fn fn, void *ctx)
{

	Forbid();
	if (viu->slotstate == SLOT_LIVE || fn == NULL) {
		viu->tapctx = ctx;
		viu->tap = fn;
	}
	Permit();
}

/* send a complete Ethernet frame bypassing the kernel */
void
sana_raw_send(struct virtif_user *viu, const UBYTE *frame, ULONG len)
{
	struct hiovec iov;

	iov.iov_base = (void *)frame;
	iov.iov_len = len;
	rumpcomp_sana_send(viu, &iov, 1);
}

void
sana_stats(struct virtif_user *viu, ULONG *rx, ULONG *tx, ULONG *rxdrop,
    ULONG *txdrop)
{

	*rx = viu->rx_packets;
	*tx = viu->tx_packets;
	*rxdrop = viu->rx_dropped;
	*txdrop = viu->tx_dropped;
}

int
sana_link(struct virtif_user *viu)
{

	return viu->slotstate == SLOT_LIVE && viu->link;
}

int
sana_link_events(struct virtif_user *viu)
{

	return viu->events != 0;
}

int
sana_is_wireless(struct virtif_user *viu)
{

	return viu->wireless;
}

const char *
sana_devname(struct virtif_user *viu)
{

	return viu->devname;
}

ULONG
sana_unit(struct virtif_user *viu)
{

	return viu->unit;
}

void
sana_set_linkhook(struct virtif_user *viu, sana_link_fn fn, void *ctx)
{

	Forbid();
	if (viu->slotstate == SLOT_LIVE || fn == NULL) {
		viu->linkctx = ctx;
		viu->linkhook = fn;
	}
	Permit();
}
