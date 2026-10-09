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
#include <exec/execbase.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

#include "rumpuser_amiga.h"
#include "sana2.h"

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
	struct s2req *next;		/* free list (transmit) */
	UBYTE	buf[ETHER_HDR_LEN + FRAME_MAX];
};
#define	REQ_RX	1
#define	REQ_TX	2

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
};

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
	SendIO((struct IORequest *)&r->ios2);
}

static int
is_zero_mac(const UBYTE *m)
{

	return !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

static void
deliver(struct virtif_user *viu, struct s2req *r)
{
	UBYTE *hdr = r->buf;
	struct hiovec iov;
	int i;

	/*
	 * Deliver as ONE contiguous frame: if_virt.c's VIF_DELIVERPKT
	 * miscounts when given more than one iovec ("m_copyback failed").
	 */
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

	iov.iov_base = hdr;
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
	struct MsgPort *ioport = NULL, *txport = NULL;
	struct IOSana2Req *base = NULL;
	struct s2req *r;
	ULONG iomask, txmask, sigs;
	int opened = 0, nrx = 0, ntx = 0;
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
	if (OpenDevice((CONST_STRPTR)viu->devname, viu->unit,
	    (struct IORequest *)base, 0) != 0) {
		amiga_rump_printf("sana: cannot open %s unit %lu (error %d)\n",
		    viu->devname, viu->unit, base->ios2_Req.io_Error);
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
	base->ios2_Req.io_Command = S2_ONLINE;
	DoIO((struct IORequest *)base);

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

	viu->txport = txport;
	viu->state = 1;
	iomask = 1UL << ioport->mp_SigBit;
	txmask = 1UL << txport->mp_SigBit;

	while (!viu->stopping) {
		sigs = Wait(iomask | txmask | SIGBREAKF_CTRL_C);

		/* frames posted by the kernel */
		while ((r = (struct s2req *)GetMsg(txport)) != NULL) {
			r->ios2.ios2_Req.io_Message.mn_ReplyPort = ioport;
			SendIO((struct IORequest *)&r->ios2);
		}
		/* completed requests */
		while ((r = (struct s2req *)GetMsg(ioport)) != NULL) {
			if (r->kind == REQ_RX) {
				if (r->ios2.ios2_Req.io_Error == 0)
					deliver(viu, r);
				else
					viu->rx_dropped++;
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

	/* shut down: abort and reap everything that is in flight */
	viu->txport = NULL;
	for (i = 0; i < (unsigned)nrx; i++) {
		r = viu->rxreqs[i];
		if (!CheckIO((struct IORequest *)&r->ios2))
			AbortIO((struct IORequest *)&r->ios2);
		WaitIO((struct IORequest *)&r->ios2);
	}
	for (i = 0; i < (unsigned)ntx; i++)
		WaitIO((struct IORequest *)&viu->txreqs[i]->ios2);
	goto cleanup;

fail:
	viu->state = -1;
cleanup:
	for (i = 0; i < (unsigned)nrx; i++)
		FreeVec(viu->rxreqs[i]);
	for (i = 0; i < (unsigned)ntx; i++)
		FreeVec(viu->txreqs[i]);
	viu->txfree = NULL;
	if (opened)
		CloseDevice((struct IORequest *)base);
	if (base)
		DeleteIORequest((struct IORequest *)base);
	if (txport)
		DeleteMsgPort(txport);
	if (ioport)
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
			rumpuser_thread_join(viu->iothread);
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
	rumpuser_thread_join(viu->iothread);
	rumpuser_component_schedule(cookie);
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
