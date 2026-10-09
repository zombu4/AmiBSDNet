/*
 * SANA-II backend for the rump kernel's virtif network interface
 * (sys/rump/net/lib/libvirtif/if_virt.c built with VIRTIF_BASE=sana and
 * RUMP_VIF_LINKSTR), giving kernel interfaces sana0, sana1, ...
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

#include "rumpuser_amiga.h"
#include "sana2.h"
#include "sana2_host.h"

extern struct ExecBase *SysBase;

#define	ETHER_ADDR_LEN	6
#define	ETHER_HDR_LEN	14
#define	FRAME_MAX	1536		/* Ethernet MTU 1500 + header, rounded */

#define	NRX_PER_TYPE	6
#define	NTX		24

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
	int	kind;			/* REQ_RX or REQ_TX */
	int	busy;			/* sent to the driver, not yet back */
	struct s2req *next;		/* free list (transmit) */
	UBYTE	buf[ETHER_HDR_LEN + FRAME_MAX];
};
#define	REQ_RX	1
#define	REQ_TX	2
#define	REQ_EV	3

struct virtif_user {
	struct virtif_sc *sc;
	char	devname[64];
	ULONG	unit;

	struct Task *iotask;
	void	*iothread;		/* rumpuser_thread_create() cookie */
	struct MsgPort *volatile txport;	/* created by the I/O process */
	volatile int state;		/* 0 starting, 1 running, <0 failed */
	volatile int stopping;

	UBYTE	mac[ETHER_ADDR_LEN];

	struct s2req *txfree;		/* Forbid() protects */
	struct s2req *rxreqs[NRXTYPES * NRX_PER_TYPE];
	struct s2req *txreqs[NTX];

	volatile ULONG rx_packets, rx_dropped, tx_packets, tx_dropped;

	/* link state from S2_ONEVENT */
	struct s2req *evreq;
	volatile int link;
	int events;			/* 0 none, 1 ONLINE/OFFLINE, 2 + CONNECT */
	int wireless;
	sana_link_fn linkhook;
	void	*linkctx;

	/* host-side frame tap (DHCP client): sees frames before the kernel */
	sana_tap_fn tap;
	void	*tapctx;
	struct virtif_user *next;	/* all interfaces */
};

static struct virtif_user *allviu;

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
parse_linkstr(struct virtif_user *viu, const char *s)
{
	size_t i = 0;
	ULONG unit = 0;
	int digits = 0;

	while (*s && *s != ':' && i < sizeof(viu->devname) - 1)
		viu->devname[i++] = *s++;
	viu->devname[i] = '\0';
	if (i == 0)
		return RUMPUSER_EINVAL;
	if (*s == ':') {
		s++;
		while (*s >= '0' && *s <= '9') {
			unit = unit * 10 + (*s++ - '0');
			digits++;
		}
		if (!digits || *s)
			return RUMPUSER_EINVAL;
	}
	viu->unit = unit;
	return 0;
}

static struct s2req *
req_alloc(struct IOSana2Req *base, int kind)
{
	struct s2req *r;

	if ((r = AllocVec(sizeof(*r), MEMF_FAST | MEMF_CLEAR)) == NULL)
		return NULL;
	CopyMem(base, &r->ios2, sizeof(r->ios2));
	r->kind = kind;
	return r;
}

static void
queue_read(struct s2req *r, struct MsgPort *port, UWORD type)
{

	r->ios2.ios2_Req.io_Message.mn_ReplyPort = port;
	r->ios2.ios2_Req.io_Command = CMD_READ;
	r->ios2.ios2_Req.io_Flags = 0;
	r->ios2.ios2_PacketType = type;
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
		if ((fn = viu->linkhook) != NULL)
			fn(viu->linkctx, up);
	}
}

static void
event_done(struct virtif_user *viu, struct s2req *r, struct MsgPort *port)
{
	ULONG ev = r->ios2.ios2_WireError;
	BYTE err = r->ios2.ios2_Req.io_Error;
	int up = viu->link;

	if (err) {
		/*
		 * Aborted (another opener's CMD_FLUSH aborts everybody's event
		 * requests): ask again.  Only "not supported" means no events.
		 */
		if (err == IOERR_ABORTED) {
			if (!viu->stopping)
				queue_event(viu, port);
			return;
		}
		if (viu->wireless) {
			/* never fall back to ONLINE/OFFLINE for Wi-Fi: it says
			   nothing about the radio; the poll decides */
			viu->events = 0;
			return;
		}
		/* not supported: try plain ONLINE/OFFLINE, then give up */
		if (viu->events == 2) {
			viu->events = 1;
			queue_event(viu, port);
		} else
			viu->events = 0;
		return;
	}
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
reap(struct s2req *r, int abort)
{
	int i;

	if (!r->busy)
		return 0;
	if (abort && !CheckIO((struct IORequest *)&r->ios2))
		AbortIO((struct IORequest *)&r->ios2);
	for (i = 0; i < 100 && !CheckIO((struct IORequest *)&r->ios2); i++)
		amiga_host_sleep_ms(20);
	if (!CheckIO((struct IORequest *)&r->ios2))
		return -1;
	WaitIO((struct IORequest *)&r->ios2);
	r->busy = 0;
	return 0;
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
 * when given more than one iovec ("m_copyback failed").
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
	struct timerequest *treq = NULL;
	struct IOSana2Req *base = NULL;
	struct s2req *r;
	ULONG iomask, txmask, tmask = 0, sigs;
	int opened = 0, nrx = 0, ntx = 0, stuck = 0;
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
	/* drivers live in DEVS:Networks: tries "Networks/<name>" as well */
	if (amibsdnet_open_sana(viu->devname, viu->unit,
	    (struct IORequest *)base, 0) != 0) {
		amiga_rump_printf("sana: cannot open %s unit %lu (error %d; "
		    "not found in DEVS:Networks?)\n", viu->devname, viu->unit,
		    base->ios2_Req.io_Error);
		goto fail;
	}
	opened = 1;

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
	DoIO((struct IORequest *)base);
	if (is_zero_mac(viu->mac)) {
		base->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
		if (DoIO((struct IORequest *)base) == 0)
			CopyMem(base->ios2_SrcAddr, viu->mac, ETHER_ADDR_LEN);
	}
	viu->wireless = probe_wireless(base);
	/* NSCMD_DEVICEQUERY's io_Data/io_Length overlay ios2_SrcAddr, which
	   every request below inherits */
	CopyMem(viu->mac, base->ios2_SrcAddr, ETHER_ADDR_LEN);
	base->ios2_Req.io_Command = S2_ONLINE;
	DoIO((struct IORequest *)base);
	viu->link = 1;
	/*
	 * Multicast groups IPv6 (and mDNS) need: some drivers (wifipi) drop
	 * every multicast frame that was not registered.  Single addresses
	 * only (a range is expanded address by address by some drivers).
	 */
	{
		static const UBYTE groups[4][ETHER_ADDR_LEN] = {
			{ 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 },	/* all nodes */
			{ 0x33, 0x33, 0xff, 0, 0, 0 },		/* solicited */
			{ 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 },	/* all hosts */
			{ 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb },	/* mDNS */
		};
		int g;

		for (g = 0; g < 4; g++) {
			CopyMem((APTR)groups[g], base->ios2_SrcAddr,
			    ETHER_ADDR_LEN);
			if (g == 1)
				CopyMem(viu->mac + 3, base->ios2_SrcAddr + 3, 3);
			base->ios2_Req.io_Command = S2_ADDMULTICASTADDRESS;
			DoIO((struct IORequest *)base);
		}
		CopyMem(viu->mac, base->ios2_SrcAddr, ETHER_ADDR_LEN);
	}
	/*
	 * Wi-Fi: the link is the association with the access point, which
	 * WirelessManager makes later (drivers that cannot tell count as
	 * linked).  Checked again every few seconds below.
	 */
	if (viu->wireless && wireless_associated(base) == 0)
		viu->link = 0;

	for (t = 0; t < NRXTYPES; t++) {
		for (i = 0; i < NRX_PER_TYPE; i++) {
			if ((r = req_alloc(base, REQ_RX)) == NULL)
				goto fail;
			viu->rxreqs[nrx++] = r;
			queue_read(r, ioport, rxtypes[t]);
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

	viu->txport = txport;
	viu->state = 1;
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
		}
	}

	while (!viu->stopping) {
		sigs = Wait(iomask | txmask | tmask | SIGBREAKF_CTRL_C);

		if (treq && (sigs & tmask) && CheckIO((struct IORequest *)treq)) {
			int a;

			WaitIO((struct IORequest *)treq);
			if ((a = wireless_associated(base)) >= 0)
				set_link(viu, a);
			treq->tr_node.io_Command = TR_ADDREQUEST;
			treq->tr_time.tv_secs = 5;
			treq->tr_time.tv_micro = 0;
			SendIO((struct IORequest *)treq);
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
				event_done(viu, r, ioport);
				continue;
			}
			if (r->kind == REQ_RX) {
				if (r->ios2.ios2_Req.io_Error == 0) {
					if (!tapped(viu, r))
						deliver(viu, r);
				} else {
					viu->rx_dropped++;
					/* a driver that rejects reads (offline)
					   must not make this loop spin */
					if (!viu->stopping)
						amiga_host_sleep_ms(40);
				}
				if (!viu->stopping)
					queue_read(r, ioport,
					    (UWORD)r->ios2.ios2_PacketType);
			} else {
				if (r->ios2.ios2_Req.io_Error == 0)
					viu->tx_packets++;
				else
					viu->tx_dropped++;
				Forbid();
				r->next = viu->txfree;
				viu->txfree = r;
				Permit();
			}
		}
		if (sigs & SIGBREAKF_CTRL_C)
			viu->stopping = 1;
	}

	/* shut down: abort and reap what the driver still has.  Only those:
	   WaitIO() on a request already taken off the reply port would
	   Remove() it a second time and corrupt the port's list. */
	viu->txport = NULL;
	if (treq) {
		if (!CheckIO((struct IORequest *)treq))
			AbortIO((struct IORequest *)treq);
		WaitIO((struct IORequest *)treq);
		CloseDevice((struct IORequest *)treq);
		DeleteIORequest((struct IORequest *)treq);
	}
	if (tport)
		DeleteMsgPort(tport);
	for (i = 0; i < (unsigned)nrx; i++)
		if (reap(viu->rxreqs[i], 1))
			stuck++;
	for (i = 0; i < (unsigned)ntx; i++)
		if (reap(viu->txreqs[i], 0))	/* writes finish by themselves */
			stuck++;
	if (viu->evreq && reap(viu->evreq, 1))
		stuck++;
	if (stuck)
		amiga_rump_printf("sana: %s keeps %d request(s); leaving it "
		    "open\n", viu->devname, stuck);
	/* frames the kernel posted after the last pass */
	while (GetMsg(txport) != NULL)
		;
	goto cleanup;

fail:
	viu->state = -1;
cleanup:
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
	return NULL;
}

/* ------------------------------------------------------------------------
 * virtif hypercalls (called from the kernel, scheduled)
 */

int
rumpcomp_sana_create(const char *linkstr, struct virtif_sc *sc,
    uint8_t *enaddr, struct virtif_user **viup)
{
	struct virtif_user *viu;
	void *cookie;
	int rv;

	if ((viu = AllocVec(sizeof(*viu), MEMF_FAST | MEMF_CLEAR)) == NULL)
		return RUMPUSER_ENOMEM;
	if ((rv = parse_linkstr(viu, linkstr)) != 0) {
		FreeVec(viu);
		return rv;
	}
	viu->sc = sc;

	cookie = rumpuser_component_unschedule();
	rv = rumpuser_thread_create(sana_iothread, viu, "AmiBSDNet sana I/O",
	    1, 0, -1, &viu->iothread);
	if (rv == 0) {
		while (viu->state == 0)
			amiga_host_sleep_ms(20);
		if (viu->state < 0) {
			amiga_host_thread_join(viu->iothread);
			rv = RUMPUSER_ENOENT;
		}
	}
	rumpuser_component_schedule(cookie);
	if (rv != 0) {
		FreeVec(viu);
		return rv;
	}

	CopyMem(viu->mac, enaddr, ETHER_ADDR_LEN);
	*viup = viu;
	Forbid();
	viu->next = allviu;
	allviu = viu;
	Permit();
	return 0;
}

int
rumpcomp_sana_dying(struct virtif_user *viu)
{

	return 0;
}

void
rumpcomp_sana_destroy(struct virtif_user *viu)
{
	void *cookie = rumpuser_component_unschedule();

	viu->stopping = 1;
	Signal(viu->iotask, SIGBREAKF_CTRL_C);
	/* not rumpuser_thread_join(): we are off the rump CPU here */
	amiga_host_thread_join(viu->iothread);
	rumpuser_component_schedule(cookie);
	Forbid();
	{
		struct virtif_user **pp;

		for (pp = &allviu; *pp; pp = &(*pp)->next)
			if (*pp == viu) {
				*pp = viu->next;
				break;
			}
	}
	Permit();
	FreeVec(viu);
}

void
rumpcomp_sana_send(struct virtif_user *viu, struct hiovec *iov, size_t iovlen)
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

	for (j = 0; j < ETHER_ADDR_LEN; j++)
		r->ios2.ios2_DstAddr[j] = hdr[j];
	r->ios2.ios2_PacketType = ((ULONG)hdr[12] << 8) | hdr[13];
	r->ios2.ios2_Data = r->buf;
	r->ios2.ios2_DataLength = plen;
	r->ios2.ios2_Req.io_Flags = 0;
	for (j = 0; j < ETHER_ADDR_LEN && hdr[j] == bcast[j]; j++)
		;
	if (j == ETHER_ADDR_LEN)
		r->ios2.ios2_Req.io_Command = S2_BROADCAST;
	else if (hdr[0] & 1)
		r->ios2.ios2_Req.io_Command = S2_MULTICAST;
	else
		r->ios2.ios2_Req.io_Command = CMD_WRITE;

	PutMsg(port, &r->ios2.ios2_Req.io_Message);
}

/* ------------------------------------------------------------------------
 * host-side access (sana2_host.h)
 */

static int
name_eq(const char *a, const char *b)
{

	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

struct virtif_user *
sana_find(const char *devname, ULONG unit)
{
	struct virtif_user *v;

	Forbid();
	for (v = allviu; v; v = v->next)
		if (v->unit == unit && name_eq(v->devname, devname))
			break;
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
	viu->tapctx = ctx;
	viu->tap = fn;
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

	return viu->link;
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
	viu->linkctx = ctx;
	viu->linkhook = fn;
	Permit();
}
