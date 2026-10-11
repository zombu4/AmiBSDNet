/*
 * fakewifi.device: a stand-in for wifipi.device in WinUAE, for testing
 * the stack and WirelessManager together without the Pi's Wi-Fi chip.
 *
 * The commands it answers are answered as wifipi.device does in its
 * source (downloads/sources/wifipi/src/unit.c HandleRequest() and the
 * Do_* functions it calls, device.c WiFi_Open()/WiFi_AbortIO(), packet.c
 * scan results): shared opens of unit 0, per-opener read and event
 * lists, S2_ONEVENT with the ONLINE/OFFLINE preset, S2_GETSIGNALQUALITY
 * and S2_GETNETWORKINFO only while connected, S2_GETNETWORKS answered
 * later with a list of tag lists from the caller's pool.  It sees one
 * network, "FakeNet", WPA2-PSK with the passphrase "fakepassword":
 * S2_SETOPTIONS with that SSID connects a second later and
 * S2EVENT_CONNECT is reported, as wifipi reports it on BRCMF_E_LINK
 * (packet.c:677-689); then the access point runs the 4-way handshake
 * with the supplicant (src/test/fakeap.c), and data passes only after
 * it.  Frames go to and come
 * from uaenet.device unit 0 (WinUAE's SLIRP), whose station address it
 * takes as its own, while connected.  What it does not do: encryption
 * (S2_SETKEY is accepted and not used), multicast filtering, S2_READMGMT
 * and S2_WRITEMGMT (not in wifipi's list either, unit.c:303-317).
 *
 * Test only: not part of the package.
 *
 *   tools/build_amiga.sh src/test/fakewifi.c build/fakewifi.device \
 *       src/test/fakeap.c \
 *       -Idownloads/sources/NDK3.2/SANA+RoadshowTCP-IP/include \
 *       -Idownloads/sources/wifipi/include \
 *       -idirafter downloads/sources/NDK3.2/Include_H -Wl,-u,romtag
 */
#include <exec/types.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/resident.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <devices/newstyle.h>
#include <devices/sana2.h>
#include <devices/sana2wireless.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "fakeap.h"

#define	DEVNAME		"fakewifi.device"
#define	IDSTRING	"fakewifi.device 1.0 (11.10.2026) test stand-in"
#define	BACKEND		"uaenet.device"
#define	FAKE_SSID	"FakeNet"
#define	FAKE_PSK	"fakepassword"	/* WPA2-PSK (src/test/fakeap.c) */
#define	FW_MTU		1500
#define	FRAME_MAX	1518
#define	NBACKREADS	8

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct UtilityBase *UtilityBase;

/* first in the file: run by mistake as a program, it returns at once */
__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{

	return -1;
}

struct opener {
	struct MinNode node;
	struct MinList reads;		/* CMD_READ */
	struct MinList orphans;		/* S2_READORPHAN */
	struct MinList events;		/* S2_ONEVENT */
	APTR copyto, copyfrom;
};

static struct Device *fwbase;
static struct MinList openers;
static volatile ULONG flags;
#define	FL_CONFIGURED	1
#define	FL_ONLINE	2
#define	FL_CONNECTED	4
static UBYTE mac[6];
static char joined[33];
/* the BSSID of the last S2_SETOPTIONS, or ff:ff:ff:ff:ff:ff without one,
   as wifipi keeps it (unit.c:648-661) and gives it back in
   S2_GETNETWORKINFO (unit.c:946-956) */
static UBYTE joinbssid[6];

/* the S2_SETOPTIONS seen, for SYS:fakewifi.log (worker_main()) */
#define	NSETOPT	16
struct setopt {
	ULONG tags[8];
	int ntags;
	char ssid[33];
	UBYTE bssid[6];
};
static struct setopt setopts[NSETOPT];
static volatile int nsetopts;
static int nlogged;
static volatile int joinreq;
static struct Sana2DeviceStats stats;
static struct Task *volatile worker;
static struct MsgPort *volatile cmdport;	/* the worker's */
static volatile int worker_state;	/* 0 starting, 1 ready, -1 failed */
static struct Task *starter;

static const UWORD supported[] = {
	CMD_READ, CMD_WRITE, CMD_FLUSH, S2_DEVICEQUERY, S2_GETSTATIONADDRESS,
	S2_CONFIGINTERFACE, S2_ADDMULTICASTADDRESS, S2_DELMULTICASTADDRESS,
	S2_MULTICAST, S2_BROADCAST, S2_GETGLOBALSTATS, S2_ONEVENT,
	S2_READORPHAN, S2_ONLINE, S2_OFFLINE, S2_ADDMULTICASTADDRESSES,
	S2_DELMULTICASTADDRESSES, S2_GETSIGNALQUALITY, S2_GETNETWORKS,
	S2_SETOPTIONS, S2_SETKEY, S2_GETNETWORKINFO, S2_GETCRYPTTYPES,
	NSCMD_DEVICEQUERY, 0
};

/* an empty exec list (exec/lists.h: lh_Head, lh_Tail, lh_TailPred) */
static void
newlist(struct List *l)
{

	l->lh_Head = (struct Node *)&l->lh_Tail;
	l->lh_Tail = NULL;
	l->lh_TailPred = (struct Node *)&l->lh_Head;
}

void	fw_beginio(struct IOSana2Req *);
LONG	fw_abortio(struct IOSana2Req *);
void	fw_open(struct IOSana2Req *, ULONG, ULONG);
BPTR	fw_close(struct IOSana2Req *);
BPTR	fw_expunge(void);
struct Device *fw_init(struct Device *, BPTR);

/* an opener's buffer function: a0 to, a1 from, d0 length; result d0 */
static ULONG
call_copy(APTR fn, APTR to, APTR from, ULONG n)
{
	register APTR a0 __asm("a0") = to;
	register APTR a1 __asm("a1") = from;
	register ULONG d0 __asm("d0") = n;
	register APTR a2 __asm("a2") = fn;

	__asm volatile ("jsr (%%a2)"
	    : "+r" (d0), "+r" (a0), "+r" (a1)
	    : "r" (a2)
	    : "d1", "cc", "memory");
	return d0;
}

static void
reply(struct IOSana2Req *io)
{

	if (!(io->ios2_Req.io_Flags & IOF_QUICK))
		ReplyMsg((struct Message *)io);
}

/* wifipi unit.c ReportEvents(): every listener whose mask matches */
static void
report_events(ULONG set)
{
	struct opener *o;
	struct IOSana2Req *io, *next;

	Forbid();
	for (o = (struct opener *)openers.mlh_Head; o->node.mln_Succ;
	    o = (struct opener *)o->node.mln_Succ)
		for (io = (struct IOSana2Req *)o->events.mlh_Head;
		    (next = (struct IOSana2Req *)
		    io->ios2_Req.io_Message.mn_Node.ln_Succ) != NULL;
		    io = next)
			if (io->ios2_WireError & set) {
				io->ios2_WireError &= set;
				Remove((struct Node *)io);
				ReplyMsg((struct Message *)io);
			}
	Permit();
}

/* ------------------------------------------------------------------------
 * the worker process: the backend, delayed scan and connect
 */

#define	MSG_SCAN	1
#define	MSG_WRITE	2

static UBYTE rxbuf[NBACKREADS][FRAME_MAX];
static UBYTE txbuf[FRAME_MAX];

/* the access point: its address, the handshake, and whether data may
   pass (after message 4, as on a WPA2 network) */
static const UBYTE bssid_ap[6] = { 0x02, 0, 0, 0, 0, 0x01 };
static struct fakeap ap;
static volatile int authorized;
static UBYTE hsframe[FAKEAP_FRAME_MAX];	/* the last message 1 or 3 */
static ULONG hslen;

ULONG	back_copy(UBYTE *, const UBYTE *, ULONG);

ULONG
back_copy(UBYTE *to, const UBYTE *from, ULONG n)
{

	if (n > FRAME_MAX)
		return FALSE;
	while (n--)
		*to++ = *from++;
	return TRUE;
}

void back_copy_reg(void);
__asm__(
"	.text\n"
"	.globl	back_copy_reg\n"
"back_copy_reg:\n"
"	move.l	%d0,-(%sp)\n"
"	move.l	%a1,-(%sp)\n"
"	move.l	%a0,-(%sp)\n"
"	jsr	back_copy\n"
"	lea	12(%sp),%sp\n"
"	rts\n");

static struct TagItem backtags[] = {
	{ S2_CopyToBuff, (ULONG)back_copy_reg },
	{ S2_CopyFromBuff, (ULONG)back_copy_reg },
	{ TAG_END, 0 }
};

/* one read request gets the frame (Forbid() held) */
static void
give(struct opener *o, struct IOSana2Req *io, struct IOSana2Req *in,
    const UBYTE *data)
{

	CopyMem(in->ios2_SrcAddr, io->ios2_SrcAddr, 6);
	CopyMem(in->ios2_DstAddr, io->ios2_DstAddr, 6);
	io->ios2_PacketType = in->ios2_PacketType;
	io->ios2_DataLength = in->ios2_DataLength;
	io->ios2_Req.io_Flags = in->ios2_Req.io_Flags &
	    (SANA2IOF_BCAST | SANA2IOF_MCAST);
	io->ios2_Req.io_Error = 0;
	if (!call_copy(o->copyto, io->ios2_Data, (APTR)data,
	    in->ios2_DataLength)) {
		io->ios2_Req.io_Error = S2ERR_SOFTWARE;
		io->ios2_WireError = S2WERR_BUFF_ERROR;
	}
	ReplyMsg((struct Message *)io);
}

/* a frame from the backend: the first read of its type of every opener;
   if no opener reads that type, one S2_READORPHAN */
static void
deliver(struct IOSana2Req *in, const UBYTE *data)
{
	struct opener *o;
	struct IOSana2Req *io;
	int taken = 0;

	Forbid();
	for (o = (struct opener *)openers.mlh_Head; o->node.mln_Succ;
	    o = (struct opener *)o->node.mln_Succ)
		for (io = (struct IOSana2Req *)o->reads.mlh_Head;
		    io->ios2_Req.io_Message.mn_Node.ln_Succ;
		    io = (struct IOSana2Req *)
		    io->ios2_Req.io_Message.mn_Node.ln_Succ)
			if (io->ios2_PacketType == in->ios2_PacketType) {
				Remove((struct Node *)io);
				give(o, io, in, data);
				taken = 1;
				break;
			}
	for (o = (struct opener *)openers.mlh_Head; !taken &&
	    o->node.mln_Succ; o = (struct opener *)o->node.mln_Succ)
		if ((io = (struct IOSana2Req *)RemHead((struct List *)
		    &o->orphans)) != NULL) {
			give(o, io, in, data);
			taken = 1;
		}
	if (taken)
		stats.PacketsReceived++;
	Permit();
}

/* the scan result: one network, in tag lists from the caller's pool
   (ios2_Data), as wifipi packet.c:451-580 builds them */
static void
scan_done(struct IOSana2Req *io)
{
	APTR pool = io->ios2_Data;
	struct TagItem **list, *t;
	char *ssid;
	UBYTE *bssid, *e;
	UWORD *ie;
	int i, n;

	list = AllocPooled(pool, sizeof(*list));
	t = AllocPooled(pool, sizeof(*t) * 9);
	ssid = AllocPooled(pool, sizeof(FAKE_SSID));
	bssid = AllocPooled(pool, 6);
	/* the information elements, as wifipi gives them (packet.c:
	   545-563: a UWORD length, then the elements): SSID, RSN */
	ie = AllocPooled(pool, 2 + 2 + sizeof(FAKE_SSID) - 1 +
	    sizeof(fakeap_rsn_ie));
	if (list == NULL || t == NULL || ssid == NULL || bssid == NULL ||
	    ie == NULL) {
		io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
		io->ios2_WireError = S2WERR_BUFF_ERROR;
		io->ios2_DataLength = 0;
		io->ios2_StatData = NULL;
		ReplyMsg((struct Message *)io);
		return;
	}
	for (i = 0; i < (int)sizeof(FAKE_SSID); i++)
		ssid[i] = FAKE_SSID[i];
	for (i = 0; i < 6; i++)
		bssid[i] = bssid_ap[i];
	t[0].ti_Tag = S2INFO_SSID;
	t[0].ti_Data = (ULONG)ssid;
	t[1].ti_Tag = S2INFO_BSSID;
	t[1].ti_Data = (ULONG)bssid;
	t[2].ti_Tag = S2INFO_BeaconInterval;
	t[2].ti_Data = 100;
	t[3].ti_Tag = S2INFO_Signal;
	t[3].ti_Data = (ULONG)-45;
	t[4].ti_Tag = S2INFO_Capabilities;
	t[4].ti_Data = 0x0011;		/* ESS and privacy (IEEE 802.11-2016
					   9.4.1.4) */
	t[5].ti_Tag = S2INFO_Channel;
	t[5].ti_Data = 6;
	t[6].ti_Tag = S2INFO_Band;
	t[6].ti_Data = S2BAND_B;
	e = (UBYTE *)(ie + 1);
	n = 0;
	e[n++] = 0;			/* the SSID element */
	e[n++] = sizeof(FAKE_SSID) - 1;
	for (i = 0; i < (int)sizeof(FAKE_SSID) - 1; i++)
		e[n++] = FAKE_SSID[i];
	for (i = 0; i < (int)sizeof(fakeap_rsn_ie); i++)
		e[n++] = fakeap_rsn_ie[i];
	ie[0] = n;
	t[7].ti_Tag = S2INFO_InfoElements;
	t[7].ti_Data = (ULONG)ie;
	t[8].ti_Tag = TAG_DONE;
	t[8].ti_Data = 0;
	list[0] = t;
	io->ios2_StatData = list;
	io->ios2_DataLength = 1;
	io->ios2_Req.io_Error = 0;
	ReplyMsg((struct Message *)io);
}

/* an EAPOL frame from the access point to the openers (EtherType 0x888e,
   IEEE 802.1X-2004 7.5) */
static void
inject(const UBYTE *f, ULONG len)
{
	static struct IOSana2Req in;
	int i;

	for (i = 0; i < 6; i++) {
		in.ios2_SrcAddr[i] = bssid_ap[i];
		in.ios2_DstAddr[i] = mac[i];
	}
	in.ios2_PacketType = 0x888e;
	in.ios2_DataLength = len;
	in.ios2_Req.io_Flags = 0;
	deliver(&in, f);
}

static void
worker_main(void)
{
	struct MsgPort *port, *bport, *tport;
	struct IOSana2Req *breq = NULL, *bw = NULL, *rd[NBACKREADS];
	struct timerequest *tr = NULL;
	struct IOSana2Req *scanq = NULL;
	int bopen = 0, topen = 0, tpending = 0, joining = 0, tries = 0, i;
	static const ULONG types[NBACKREADS] = { 0x0800, 0x0800, 0x0800,
	    0x0806, 0x0806, 0x86dd, 0x888e, 0x888e };

	for (i = 0; i < NBACKREADS; i++)
		rd[i] = NULL;
	port = CreateMsgPort();
	bport = CreateMsgPort();
	tport = CreateMsgPort();
	if (port && bport && tport &&
	    (breq = (struct IOSana2Req *)CreateIORequest(bport,
	    sizeof(*breq))) != NULL &&
	    (tr = (struct timerequest *)CreateIORequest(tport,
	    sizeof(*tr))) != NULL) {
		breq->ios2_BufferManagement = backtags;
		if (OpenDevice((CONST_STRPTR)BACKEND, 0,
		    (struct IORequest *)breq, 0) == 0)
			bopen = 1;
		if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
		    (struct IORequest *)tr, 0) == 0)
			topen = 1;
	}
	if (bopen) {
		breq->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
		if (DoIO((struct IORequest *)breq) == 0)
			CopyMem(breq->ios2_DstAddr, mac, 6);
		CopyMem(mac, breq->ios2_SrcAddr, 6);
		breq->ios2_Req.io_Command = S2_CONFIGINTERFACE;
		DoIO((struct IORequest *)breq);
		breq->ios2_Req.io_Command = S2_ONLINE;
		DoIO((struct IORequest *)breq);
		if ((bw = (struct IOSana2Req *)CreateIORequest(bport,
		    sizeof(*bw))) != NULL)
			CopyMem(breq, bw, sizeof(*bw));
	}
	if (!bopen || !topen || bw == NULL) {
		worker_state = -1;
		Signal(starter, SIGF_SINGLE);
		goto out;
	}
	for (i = 0; i < NBACKREADS; i++)
		if ((rd[i] = (struct IOSana2Req *)CreateIORequest(bport,
		    sizeof(*rd[i]))) != NULL) {
			CopyMem(breq, rd[i], sizeof(*rd[i]));
			rd[i]->ios2_Req.io_Command = CMD_READ;
			rd[i]->ios2_PacketType = types[i];
			rd[i]->ios2_Data = rxbuf[i];
			rd[i]->ios2_Req.io_Flags = 0;
			SendIO((struct IORequest *)rd[i]);
		}
	/* (the PMK: 4096 HMAC-SHA1 rounds, done once) */
	fakeap_init(&ap, FAKE_PSK, FAKE_SSID, bssid_ap, (ULONG)mac[5] << 8 |
	    mac[4]);
	cmdport = port;
	worker_state = 1;
	Signal(starter, SIGF_SINGLE);

	for (;;) {
		struct Message *m;

		Wait((1UL << port->mp_SigBit) | (1UL << bport->mp_SigBit) |
		    (1UL << tport->mp_SigBit) | SIGBREAKF_CTRL_F);
		/* frames from the backend */
		while ((m = GetMsg(bport)) != NULL) {
			struct IOSana2Req *r = (struct IOSana2Req *)m;

			for (i = 0; i < NBACKREADS && rd[i] != r; i++)
				;
			if (i == NBACKREADS)
				continue;
			if (r->ios2_Req.io_Error == 0 &&
			    (flags & (FL_ONLINE | FL_CONNECTED)) ==
			    (FL_ONLINE | FL_CONNECTED) && authorized)
				deliver(r, rxbuf[i]);
			r->ios2_Req.io_Command = CMD_READ;
			r->ios2_PacketType = types[i];
			r->ios2_Data = rxbuf[i];
			r->ios2_Req.io_Flags = 0;
			SendIO((struct IORequest *)r);
		}
		/* requests from BeginIO() */
		while ((m = GetMsg(port)) != NULL) {
			struct IOSana2Req *io = (struct IOSana2Req *)m;
			struct opener *o = io->ios2_BufferManagement;

			if (io->ios2_Req.io_Message.mn_Node.ln_Pri == MSG_SCAN) {
				if (scanq)
					scan_done(io);	/* (a second one) */
				else
					scanq = io;
				continue;
			}
			io->ios2_Req.io_Error = 0;
			if ((flags & FL_CONNECTED) == 0) {
				/* (not associated: nothing goes out) */
				ReplyMsg(m);
				continue;
			}
			/* EAPOL to the access point: the handshake */
			if (io->ios2_PacketType == 0x888e &&
			    io->ios2_DataLength <= FAKEAP_FRAME_MAX &&
			    call_copy(o->copyfrom, txbuf, io->ios2_Data,
			    io->ios2_DataLength)) {
				ULONG n = fakeap_rx(&ap, txbuf,
				    io->ios2_DataLength, hsframe);

				if (n) {
					hslen = n;
					tries = 0;
					inject(hsframe, hslen);
				}
				if (ap.state == FAKEAP_DONE && !authorized) {
					authorized = 1;
					hslen = 0;
				}
				ReplyMsg(m);
				continue;
			}
			/* (until the handshake is done only EAPOL passes) */
			if (!authorized) {
				ReplyMsg(m);
				continue;
			}
			if (io->ios2_DataLength > FW_MTU ||
			    !call_copy(o->copyfrom, txbuf, io->ios2_Data,
			    io->ios2_DataLength)) {
				io->ios2_Req.io_Error = S2ERR_MTU_EXCEEDED;
				io->ios2_WireError = S2WERR_GENERIC_ERROR;
				ReplyMsg(m);
				continue;
			}
			bw->ios2_Req.io_Command = CMD_WRITE;
			bw->ios2_Req.io_Flags = 0;
			bw->ios2_PacketType = io->ios2_PacketType;
			CopyMem(io->ios2_DstAddr, bw->ios2_DstAddr, 6);
			bw->ios2_Data = txbuf;
			bw->ios2_DataLength = io->ios2_DataLength;
			DoIO((struct IORequest *)bw);
			io->ios2_Req.io_Error = bw->ios2_Req.io_Error;
			io->ios2_WireError = bw->ios2_WireError;
			stats.PacketsSent++;
			ReplyMsg(m);
		}
		/* a join (S2_SETOPTIONS, Ctrl-F) */
		if (joinreq) {
			joinreq = 0;
			joining = 1;
		}
		/* the S2_SETOPTIONS seen so far, to SYS:fakewifi.log */
		while (nlogged < nsetopts) {
			struct setopt *so = &setopts[nlogged % NSETOPT];
			BPTR lf = Open((CONST_STRPTR)"SYS:fakewifi.log",
			    MODE_READWRITE);
			LONG arg[10];
			int k;

			nlogged++;
			if (lf == 0)
				continue;
			Seek(lf, 0, OFFSET_END);
			arg[0] = nlogged;
			arg[1] = (LONG)so->ssid;
			for (k = 0; k < 6; k++)
				arg[2 + k] = so->bssid[k];
			VFPrintf(lf, (CONST_STRPTR)"S2_SETOPTIONS %ld: SSID '%s' "
			    "BSSID %02lx:%02lx:%02lx:%02lx:%02lx:%02lx tags", arg);
			for (k = 0; k < so->ntags; k++) {
				arg[0] = so->tags[k] - TAG_USER;
				VFPrintf(lf, (CONST_STRPTR)" U+%ld", arg);
			}
			FPuts(lf, (CONST_STRPTR)"\n");
			Close(lf);
		}
		/* scan two seconds, connect one second later; then the
		   handshake, message 1 or 3 again every second, five times
		   at most (then it disconnects) */
		if ((scanq || joining || ((flags & FL_CONNECTED) &&
		    !authorized)) && !tpending) {
			tr->tr_node.io_Command = TR_ADDREQUEST;
			tr->tr_time.tv_secs = scanq ? 2 : 1;
			tr->tr_time.tv_micro = 0;
			SendIO((struct IORequest *)tr);
			tpending = 1;
		}
		if (tpending && CheckIO((struct IORequest *)tr)) {
			WaitIO((struct IORequest *)tr);
			tpending = 0;
			if (scanq) {
				scan_done(scanq);
				scanq = NULL;
			} else if (joining) {
				joining = 0;
				if (Stricmp((STRPTR)joined,
				    (STRPTR)FAKE_SSID) == 0 &&
				    (flags & FL_ONLINE)) {
					authorized = 0;
					ap.state = FAKEAP_IDLE;
					hslen = 0;
					tries = 0;
					flags |= FL_CONNECTED;
					report_events(S2EVENT_CONNECT);
				}
			} else if ((flags & FL_CONNECTED) && !authorized) {
				if (++tries > 5) {
					flags &= ~FL_CONNECTED;
					report_events(S2EVENT_DISCONNECT);
				} else {
					if (ap.state != FAKEAP_SENT3)
						hslen = fakeap_msg1(&ap, mac,
						    hsframe);
					inject(hsframe, hslen);
				}
			}
		}
	}
out:
	if (bw)
		DeleteIORequest((struct IORequest *)bw);
	if (bopen)
		CloseDevice((struct IORequest *)breq);
	if (topen)
		CloseDevice((struct IORequest *)tr);
	if (tr)
		DeleteIORequest((struct IORequest *)tr);
	if (breq)
		DeleteIORequest((struct IORequest *)breq);
	if (tport)
		DeleteMsgPort(tport);
	if (bport)
		DeleteMsgPort(bport);
	if (port)
		DeleteMsgPort(port);
	worker = NULL;
}

static void
to_worker(struct IOSana2Req *io, int what)
{

	io->ios2_Req.io_Flags &= ~IOF_QUICK;
	io->ios2_Req.io_Message.mn_Node.ln_Pri = what;
	PutMsg(cmdport, (struct Message *)io);
}

/* ------------------------------------------------------------------------
 * the device functions
 */

void
fw_beginio(struct IOSana2Req *io)
{
	struct opener *o = io->ios2_BufferManagement;
	ULONG preset;

	io->ios2_Req.io_Error = 0;
	io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
	if (io->ios2_Req.io_Message.mn_Length < sizeof(struct IOSana2Req) &&
	    io->ios2_Req.io_Command != NSCMD_DEVICEQUERY) {
		io->ios2_Req.io_Error = IOERR_BADLENGTH;
		reply(io);
		return;
	}
	switch (io->ios2_Req.io_Command) {
	case NSCMD_DEVICEQUERY: {
		struct IOStdReq *s = (struct IOStdReq *)io;
		struct NSDeviceQueryResult *q = s->io_Data;

		q->nsdqr_DevQueryFormat = 0;
		q->nsdqr_SizeAvailable = sizeof(*q);
		q->nsdqr_DeviceType = NSDEVTYPE_SANA2;
		q->nsdqr_DeviceSubType = 0;
		q->nsdqr_SupportedCommands = (UWORD *)supported;
		s->io_Actual = sizeof(*q);
		break;
	}
	case S2_DEVICEQUERY: {
		struct Sana2DeviceQuery *d = io->ios2_StatData;

		d->SizeSupplied = sizeof(*d);
		d->DevQueryFormat = 0;
		d->DeviceLevel = 0;
		d->AddrFieldSize = 48;
		d->MTU = FW_MTU;
		d->BPS = 100000000;
		d->HardwareType = S2WireType_Ethernet;
		break;
	}
	case S2_GETSTATIONADDRESS:
		CopyMem(mac, io->ios2_SrcAddr, 6);
		CopyMem(mac, io->ios2_DstAddr, 6);
		break;
	case S2_CONFIGINTERFACE:
		/* (wifipi unit.c:1422-1446) */
		if (flags & FL_CONFIGURED) {
			io->ios2_Req.io_Error = S2ERR_BAD_STATE;
			io->ios2_WireError = S2WERR_IS_CONFIGURED;
		} else {
			CopyMem(mac, io->ios2_SrcAddr, 6);
			flags |= FL_CONFIGURED;
			if (!(flags & FL_ONLINE)) {
				flags |= FL_ONLINE;
				report_events(S2EVENT_ONLINE);
			}
		}
		break;
	case S2_ONLINE:
		if (!(flags & FL_ONLINE)) {
			flags |= FL_ONLINE;
			report_events(S2EVENT_ONLINE);
		}
		break;
	case S2_OFFLINE:
		if (flags & FL_ONLINE) {
			flags &= ~(FL_ONLINE | FL_CONNECTED);
			report_events(S2EVENT_OFFLINE);
		}
		break;
	case S2_GETGLOBALSTATS:
		CopyMem(&stats, io->ios2_StatData, sizeof(stats));
		break;
	case S2_ONEVENT:
		/* (wifipi unit.c:388-421) */
		preset = (flags & FL_ONLINE) ? S2EVENT_ONLINE : S2EVENT_OFFLINE;
		if (io->ios2_WireError & preset) {
			io->ios2_WireError &= preset;
			break;
		}
		io->ios2_Req.io_Flags &= ~IOF_QUICK;
		Forbid();
		AddTail((struct List *)&o->events, (struct Node *)io);
		Permit();
		return;
	case CMD_READ:
		io->ios2_Req.io_Flags &= ~IOF_QUICK;
		Forbid();
		AddTail((struct List *)&o->reads, (struct Node *)io);
		Permit();
		return;
	case S2_READORPHAN:
		io->ios2_Req.io_Flags &= ~IOF_QUICK;
		Forbid();
		AddTail((struct List *)&o->orphans, (struct Node *)io);
		Permit();
		return;
	case S2_BROADCAST:
		io->ios2_DstAddr[0] = io->ios2_DstAddr[1] = 0xff;
		io->ios2_DstAddr[2] = io->ios2_DstAddr[3] = 0xff;
		io->ios2_DstAddr[4] = io->ios2_DstAddr[5] = 0xff;
		/* FALLTHROUGH */
	case S2_MULTICAST:
	case CMD_WRITE:
		/* (wifipi unit.c:1149-1167: queued while the unit is up) */
		if (!(flags & FL_CONFIGURED)) {
			io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
			io->ios2_WireError = S2WERR_UNIT_OFFLINE;
			break;
		}
		to_worker(io, MSG_WRITE);
		return;
	case CMD_FLUSH:
	case S2_ADDMULTICASTADDRESS:
	case S2_DELMULTICASTADDRESS:
	case S2_ADDMULTICASTADDRESSES:
	case S2_DELMULTICASTADDRESSES:
	case S2_SETKEY:
		break;
	case S2_GETSIGNALQUALITY:
		/* (wifipi unit.c:422-447) */
		if (!(flags & FL_ONLINE)) {
			io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
			io->ios2_WireError = S2WERR_UNIT_OFFLINE;
		} else if (!(flags & FL_CONNECTED)) {
			io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
			io->ios2_WireError = S2WERR_NOT_CONFIGURED;
		} else {
			struct Sana2SignalQuality *q = io->ios2_StatData;

			q->SignalLevel = -45;
			q->NoiseLevel = -90;
		}
		break;
	case S2_GETNETWORKS:
		to_worker(io, MSG_SCAN);
		return;
	case S2_SETOPTIONS: {
		/* (wifipi unit.c:613-914: every S2_SETOPTIONS clears the join
		   parameters, takes SSID and BSSID from the tags - BSSID
		   ff:ff:ff:ff:ff:ff without one - and joins with them) */
		struct TagItem *tl = (struct TagItem *)io->ios2_Data;
		const char *s = (const char *)GetTagData(S2INFO_SSID, 0, tl);
		const UBYTE *b = (const UBYTE *)GetTagData(S2INFO_BSSID, 0, tl);
		struct TagItem *ti, *tstate = tl;
		struct setopt *so;
		int i;

		for (i = 0; s && s[i] && i < 32; i++)
			joined[i] = s[i];
		joined[i] = '\0';
		for (i = 0; i < 6; i++)
			joinbssid[i] = b ? b[i] : 0xff;
		/* (recorded for the log, written by the worker) */
		so = &setopts[nsetopts % NSETOPT];
		so->ntags = 0;
		/* (utility/tagitem.h: TAG_DONE ends, TAG_IGNORE is passed
		   over, TAG_MORE goes on at ti_Data, TAG_SKIP skips ti_Data
		   more) */
		for (ti = tstate; ti && ti->ti_Tag != TAG_DONE &&
		    so->ntags < 8; ti++) {
			if (ti->ti_Tag == TAG_IGNORE)
				continue;
			if (ti->ti_Tag == TAG_MORE) {
				ti = (struct TagItem *)ti->ti_Data - 1;
				continue;
			}
			if (ti->ti_Tag == TAG_SKIP) {
				ti += ti->ti_Data;
				continue;
			}
			so->tags[so->ntags++] = ti->ti_Tag;
		}
		for (i = 0; i < 33; i++)
			so->ssid[i] = joined[i];
		for (i = 0; i < 6; i++)
			so->bssid[i] = joinbssid[i];
		nsetopts++;
		if (flags & FL_CONNECTED) {
			flags &= ~FL_CONNECTED;
			report_events(S2EVENT_DISCONNECT);
		}
		joinreq = 1;
		Signal(worker, SIGBREAKF_CTRL_F);
		break;
	}
	case S2_GETNETWORKINFO: {
		/* (wifipi unit.c:916-1023, without the WPA information) */
		APTR pool = io->ios2_Data;
		struct TagItem *t;
		char *s;
		UBYTE *b;
		int i;

		if (!(flags & FL_ONLINE)) {
			io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
			io->ios2_WireError = S2WERR_UNIT_OFFLINE;
			break;
		}
		if (!(flags & FL_CONNECTED)) {
			io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
			io->ios2_WireError = S2WERR_NOT_CONFIGURED;
			break;
		}
		t = AllocPooled(pool, sizeof(*t) * 3);
		s = AllocPooled(pool, 33);
		b = AllocPooled(pool, 6);
		if (t == NULL || s == NULL || b == NULL) {
			io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
			io->ios2_WireError = S2WERR_BUFF_ERROR;
			break;
		}
		for (i = 0; joined[i]; i++)
			s[i] = joined[i];
		s[i] = '\0';
		for (i = 0; i < 6; i++)
			b[i] = joinbssid[i];
		t[0].ti_Tag = S2INFO_SSID;
		t[0].ti_Data = (ULONG)s;
		t[1].ti_Tag = S2INFO_BSSID;
		t[1].ti_Data = (ULONG)b;
		t[2].ti_Tag = TAG_DONE;
		t[2].ti_Data = 0;
		io->ios2_StatData = t;
		break;
	}
	case S2_GETCRYPTTYPES:
		/* (wifipi unit.c:357-386) */
		if (io->ios2_Data) {
			UBYTE *types = AllocPooled(io->ios2_Data, 3);

			if (types == NULL) {
				io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
				io->ios2_WireError = S2WERR_GENERIC_ERROR;
			} else {
				types[0] = S2ENC_CCMP;
				types[1] = S2ENC_TKIP;
				types[2] = S2ENC_WEP;
				io->ios2_StatData = types;
				io->ios2_DataLength = 3;
			}
		} else {
			io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
			io->ios2_WireError = S2WERR_GENERIC_ERROR;
		}
		break;
	default:
		io->ios2_Req.io_Error = IOERR_NOCMD;
		break;
	}
	reply(io);
}

/* wifipi device.c:319-338: a request still in a list is taken out */
LONG
fw_abortio(struct IOSana2Req *io)
{

	Forbid();
	if (!(io->ios2_Req.io_Flags & IOF_QUICK) &&
	    io->ios2_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE) {
		Remove((struct Node *)io);
		io->ios2_Req.io_Error = IOERR_ABORTED;
		io->ios2_WireError = S2WERR_GENERIC_ERROR;
		ReplyMsg((struct Message *)io);
	}
	Permit();
	return 0;
}

void
fw_open(struct IOSana2Req *io, ULONG unit, ULONG oflags)
{
	struct opener *o;
	struct TagItem *tags;

	io->ios2_Req.io_Error = IOERR_OPENFAIL;
	if (unit != 0 ||
	    io->ios2_Req.io_Message.mn_Length < sizeof(struct IOStdReq))
		return;
	if (worker == NULL) {
		worker_state = 0;
		starter = SysBase->ThisTask;
		SetSignal(0, SIGF_SINGLE);
		worker = (struct Task *)CreateNewProcTags(NP_Entry,
		    (ULONG)worker_main, NP_Name, (ULONG)"fakewifi.device",
		    NP_StackSize, 8192, TAG_DONE);
		if (worker == NULL)
			return;
		while (worker_state == 0)
			Wait(SIGF_SINGLE);
		if (worker_state < 0)
			return;
	}
	if (io->ios2_Req.io_Message.mn_Length >= sizeof(struct IOSana2Req)) {
		if ((o = AllocVec(sizeof(*o), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
			return;
		newlist((struct List *)&o->reads);
		newlist((struct List *)&o->orphans);
		newlist((struct List *)&o->events);
		tags = io->ios2_BufferManagement;
		o->copyto = (APTR)GetTagData(S2_CopyToBuff, 0, tags);
		o->copyfrom = (APTR)GetTagData(S2_CopyFromBuff, 0, tags);
		if (o->copyto == NULL || o->copyfrom == NULL) {
			FreeVec(o);
			return;
		}
		Forbid();
		AddTail((struct List *)&openers, (struct Node *)o);
		Permit();
		io->ios2_BufferManagement = o;
	} else
		io->ios2_BufferManagement = NULL;
	io->ios2_Req.io_Unit = (struct Unit *)1;
	io->ios2_Req.io_Error = 0;
	fwbase->dd_Library.lib_OpenCnt++;
	fwbase->dd_Library.lib_Flags &= ~LIBF_DELEXP;
	(void)oflags;
}

BPTR
fw_close(struct IOSana2Req *io)
{
	struct opener *o = io->ios2_BufferManagement;

	if (o && io->ios2_Req.io_Message.mn_Length >=
	    sizeof(struct IOSana2Req)) {
		Forbid();
		Remove((struct Node *)o);
		Permit();
		FreeVec(o);
	}
	io->ios2_Req.io_Device = (struct Device *)-1;
	io->ios2_Req.io_Unit = (struct Unit *)-1;
	fwbase->dd_Library.lib_OpenCnt--;
	return 0;
}

/* never unloaded: the worker and its backend stay */
BPTR
fw_expunge(void)
{

	fwbase->dd_Library.lib_Flags |= LIBF_DELEXP;
	return 0;
}

struct Device *
fw_init(struct Device *dev, BPTR seg)
{

	SysBase = *(struct ExecBase **)4;
	fwbase = dev;
	newlist((struct List *)&openers);
	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37)) ==
	    NULL || (UtilityBase = (struct UtilityBase *)OpenLibrary("utility.library", 37)) == NULL)
		return NULL;
	dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
	dev->dd_Library.lib_Node.ln_Name = (char *)DEVNAME;
	dev->dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	dev->dd_Library.lib_Version = 1;
	dev->dd_Library.lib_Revision = 0;
	dev->dd_Library.lib_IdString = (APTR)IDSTRING;
	(void)seg;
	return dev;
}

/* register entry points (a6 the base, a1 the request, d0/d1 arguments) */
void fwv_open(void), fwv_close(void), fwv_expunge(void), fwv_null(void);
void fwv_beginio(void), fwv_abortio(void), fwv_init(void);
__asm__(
"	.text\n"
"fwv_open:\n"
"	move.l	%d1,-(%sp)\n"
"	move.l	%d0,-(%sp)\n"
"	move.l	%a1,-(%sp)\n"
"	jsr	fw_open\n"
"	lea	12(%sp),%sp\n"
"	rts\n"
"fwv_close:\n"
"	move.l	%a1,-(%sp)\n"
"	jsr	fw_close\n"
"	addq.l	#4,%sp\n"
"	rts\n"
"fwv_expunge:\n"
"	jsr	fw_expunge\n"
"	rts\n"
"fwv_null:\n"
"	moveq	#0,%d0\n"
"	rts\n"
"fwv_beginio:\n"
"	move.l	%a1,-(%sp)\n"
"	jsr	fw_beginio\n"
"	addq.l	#4,%sp\n"
"	rts\n"
"fwv_abortio:\n"
"	move.l	%a1,-(%sp)\n"
"	jsr	fw_abortio\n"
"	addq.l	#4,%sp\n"
"	rts\n"
"fwv_init:\n"
"	move.l	%a0,-(%sp)\n"
"	move.l	%d0,-(%sp)\n"
"	jsr	fw_init\n"
"	addq.l	#8,%sp\n"
"	rts\n");

static const APTR vectors[] = {
	(APTR)fwv_open, (APTR)fwv_close, (APTR)fwv_expunge, (APTR)fwv_null,
	(APTR)fwv_beginio, (APTR)fwv_abortio, (APTR)-1
};

static const ULONG inittab[4] = {
	sizeof(struct Device), (ULONG)vectors, 0, (ULONG)fwv_init
};

/* exec/resident.h; RTF_AUTOINIT: rt_Init is the table above */
__attribute__((used)) const struct Resident romtag = {
	RTC_MATCHWORD, (struct Resident *)&romtag,
	(APTR)((const char *)&romtag + sizeof(struct Resident)), RTF_AUTOINIT,
	1, NT_DEVICE, 0, (char *)DEVNAME, (char *)IDSTRING, (APTR)inittab
};
