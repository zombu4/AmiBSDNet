/*
 * AmiBSDNetStatus: the Wi-Fi window.  Lists the networks in range (scan
 * through the wireless SANA-II driver), takes a passphrase, stores the
 * network in Wireless.prefs and restarts WirelessManager, then has the
 * stack fetch a new address.  The passphrase field shows '*'
 * (secret_hook_init(), settingswin.c).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <amibsdnet/control.h>
#include <amibsdnet/wm.h>
#include <amibsdnet/devopen.h>
#include <amibsdnet/hwcheck.h>

#include "statustool.h"

struct Library *GadToolsBase;

/* the SANA-II definitions this window needs (downloads/sources/NDK3.2/
   SANA+RoadshowTCP-IP/include/devices/sana2.h; the wireless ones from
   downloads/sources/wifipi/include/devices/sana2wireless.h, Neil
   Cafferkey's) */
struct IOSana2Req {
	struct IORequest ios2_Req;
	ULONG	ios2_WireError;
	ULONG	ios2_PacketType;
	UBYTE	ios2_SrcAddr[16];
	UBYTE	ios2_DstAddr[16];
	ULONG	ios2_DataLength;
	APTR	ios2_Data;
	APTR	ios2_StatData;
	APTR	ios2_BufferManagement;
};
#define	S2_GETNETWORKS		0xc011
#define	S2ERR_BAD_STATE		4	/* configured already */
#define	S2ERR_NOT_SUPPORTED	8
#define	S2INFO_SSID		(TAG_USER + 0)
#define	S2INFO_Signal		(TAG_USER + 8)
#define	S2INFO_Capabilities	(TAG_USER + 10)
#define	S2INFO_InfoElements	(TAG_USER + 11)
#define	S2_CopyToBuff		(TAG_USER + 0xb0001)
#define	S2_CopyFromBuff		(TAG_USER + 0xb0002)

#define	MAXNETS	24

/* what a network asks for (security()) */
#define	SEC_OPEN	0	/* nothing */
#define	SEC_PSK		1	/* WPA or WPA2 with a passphrase */
#define	SEC_UNKNOWN	2	/* the driver gave no information elements */
#define	SEC_WEP		3
#define	SEC_SAE		4	/* WPA3 only */
#define	SEC_EAP		5	/* 802.1X (Enterprise) only */
#define	SEC_OTHER	6	/* another key management */

struct net {
	struct Node node;		/* ln_Name -> label */
	char	ssid[34];
	char	label[64];
	LONG	signal;
	int	sec;			/* SEC_* */
	int	wpa1;			/* SEC_PSK through WPA only */
};

static struct net nets[MAXNETS];
static int nnets;
static struct List netlist;

/* buffer hooks: never used for a scan, but drivers want them at open */
static ULONG nocopy(void) { return 0; }
static struct TagItem bufftags[] = {
	{ S2_CopyToBuff, (ULONG)nocopy },
	{ S2_CopyFromBuff, (ULONG)nocopy },
	{ TAG_DONE, 0 }
};

static ULONG
tagdata(struct TagItem *tl, ULONG tag, ULONG def)
{
	while (tl) {
		switch (tl->ti_Tag) {
		case TAG_DONE:
			return def;
		case TAG_MORE:
			tl = (struct TagItem *)tl->ti_Data;
			continue;
		case TAG_SKIP:
			tl += tl->ti_Data + 1;
			continue;
		case TAG_IGNORE:
			break;
		default:
			if (tl->ti_Tag == tag)
				return tl->ti_Data;
		}
		tl++;
	}
	return def;
}

/*
 * The key managements an RSN (WPA2/WPA3, element 48) or WPA (element 221,
 * 00:50:f2 type 1) element offers.  Layout as NetBSD reads them
 * (netbsd-src/sys/net80211/ieee80211_input.c ieee80211_parse_rsn(),
 * ieee80211_parse_wpa()): [OUI and type for WPA,] version (2), group
 * cipher (4), a count (2, little-endian) of 4-byte pairwise ciphers, a
 * count of 4-byte key managements.  The suites (downloads/sources/hostap/
 * wpa_common.h): 00:0f:ac or 00:50:f2 type 1 802.1X, 2 PSK; 00:0f:ac
 * type 8 SAE (WPA3).
 */
#define	AKM_PSK		1
#define	AKM_SAE		2
#define	AKM_EAP		4
#define	AKM_OTHER	8

static int
akms(const UBYTE *e, int l, int wpa)
{
	int off = wpa ? 4 : 0, n, i, r = 0;
	const UBYTE *s;

	off += 2 + 4;			/* version, group cipher */
	if (off + 2 > l)
		return AKM_OTHER;
	n = e[off] | e[off + 1] << 8;
	off += 2 + 4 * n;		/* pairwise ciphers */
	if (off + 2 > l)
		return AKM_OTHER;
	n = e[off] | e[off + 1] << 8;
	off += 2;
	for (i = 0; i < n && off + 4 * i + 4 <= l; i++) {
		s = e + off + 4 * i;
		if ((wpa && s[0] == 0x00 && s[1] == 0x50 && s[2] == 0xf2) ||
		    (!wpa && s[0] == 0x00 && s[1] == 0x0f && s[2] == 0xac))
			r |= s[3] == 2 ? AKM_PSK : s[3] == 1 ? AKM_EAP :
			    !wpa && s[3] == 8 ? AKM_SAE : AKM_OTHER;
		else
			r |= AKM_OTHER;
	}
	return r ? r : AKM_OTHER;
}

/*
 * What the network asks for, from its information elements (wifipi:
 * a length word, then the elements, downloads/sources/wifipi/src/
 * packet.c S2INFO_InfoElements) and its capability field (packet.c
 * S2INFO_Capabilities: bssi_Capability, where IEEE80211_CAPINFO_PRIVACY
 * 0x0010, netbsd-src/sys/net80211/ieee80211.h, means encrypted).
 */
static int
security(const UBYTE *ies, ULONG caps, int *wpa1)
{
	ULONG len, off;
	int rsn = 0, wpa = 0;

	*wpa1 = 0;
	if (ies == NULL)
		return SEC_UNKNOWN;
	/* (never more than a frame holds, whatever the driver says:
	   IEEE80211_MAX_LEN, 2312, netbsd-src/sys/net80211/ieee80211.h) */
	len = *(const UWORD *)ies;
	if (len > 2312)
		len = 2312;
	ies += 2;
	for (off = 0; off + 2 <= len; off += 2 + ies[off + 1]) {
		UBYTE id = ies[off], l = ies[off + 1];

		if (off + 2 + l > len)
			break;
		if (id == 48)
			rsn |= akms(ies + off + 2, l, 0);
		else if (id == 221 && l >= 4 && ies[off + 2] == 0x00 &&
		    ies[off + 3] == 0x50 && ies[off + 4] == 0xf2 &&
		    ies[off + 5] == 0x01)
			wpa |= akms(ies + off + 2, l, 1);
	}
	if ((rsn | wpa) & AKM_PSK) {
		*wpa1 = !(rsn & AKM_PSK);
		return SEC_PSK;
	}
	if (rsn & AKM_SAE)
		return SEC_SAE;
	if ((rsn | wpa) & AKM_EAP)
		return SEC_EAP;
	if (rsn | wpa)
		return SEC_OTHER;
	if (caps != ~0UL && (caps & 0x0010))
		return SEC_WEP;
	return SEC_OPEN;
}

static const char *
secname(const struct net *n)
{
	static const char *const names[] = { "open", "WPA2", "?", "WEP",
	    "WPA3", "802.1X", "other" };

	if (n->sec == SEC_PSK && n->wpa1)
		return "WPA";
	return n->sec >= 0 && n->sec <= SEC_OTHER ? names[n->sec] : "?";
}

static void
scopy(char *d, const char *s, int n)
{

	while (--n > 0 && *s)
		*d++ = *s++;
	*d = '\0';
}

static int
seq(const char *a, const char *b)
{

	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static void
make_label(struct net *n)
{
	char *p = n->label, num[12];
	const char *s;
	LONG v = n->signal;
	int i = 0, neg = v < 0;
	ULONG u = neg ? -v : v;

	/* (an SSID is any 32 bytes: what is not printable shows as '?') */
	for (s = n->ssid; *s && p < n->label + 33; s++)
		*p++ = (UBYTE)*s >= 0x20 && (UBYTE)*s != 0x7f ? *s : '?';
	while (p < n->label + 34)
		*p++ = ' ';
	for (s = secname(n); *s; )
		*p++ = *s++;
	*p++ = ' ';
	do {
		num[i++] = '0' + u % 10;
		u /= 10;
	} while (u);
	if (neg)
		*p++ = '-';
	while (i)
		*p++ = num[--i];
	*p = '\0';
	n->node.ln_Name = n->label;
}

#define	SCAN_FAILED	(-1)
#define	SCAN_CANCELLED	(-2)
#define	SCAN_TIMEOUT	(-3)
#define	SCAN_NOPROC	(-4)
#define	SCAN_SECONDS	60

#define	S2_GETSTATIONADDRESS	(CMD_NONSTD + 1)
#define	S2_CONFIGINTERFACE	(CMD_NONSTD + 2)

/*
 * The scan runs in a process of its own, which owns the request, its
 * memory pool and the open device, and frees them only when the driver
 * really answers.  It is never aborted: wifipi.device's AbortIO() replies
 * a queued or active request (downloads/sources/wifipi/src/device.c
 * WiFi_AbortIO()) while the driver keeps it as its scan request
 * (packet.c unit->wu_ScanRequest) and fills it in later, and no other
 * scan starts while that is set (packet.c: "If no scan request is in
 * progress start another one").  The window only waits for the job; Close
 * or a timeout stop waiting, and no new scan starts while one is out.
 *
 * The process runs its own copy of this program (start_scan(): LoadSeg()
 * of the program file, freed by DOS when the process ends with
 * NP_FreeSeglist, dos.doc CreateNewProc), so it may outlive this one: the
 * job is in memory of its own, and whoever is last frees it, under
 * Forbid(): the scan when the program has gone (wifi_scan_release()),
 * else the program.
 */
#define	SCANJOB_MAGIC	0x414e5343UL

struct scanjob {
	ULONG	magic;
	volatile int state;		/* 1 running, 2 done */
	volatile int abandoned;		/* the program has gone */
	int	result;
	char	device[64];
	ULONG	unit;
	int	n;
	struct net nets[MAXNETS];
	/* what the driver said (shown when there is nothing to choose) */
	ULONG	listed;			/* entries it returned */
	ULONG	noname;			/* of those without a name */
	LONG	err, werr;		/* io_Error, ios2_WireError */
	int	opened;			/* it opened at all */
	int	nomem;			/* no memory for the results */
	LONG	cfgerr;			/* S2_CONFIGINTERFACE failed so */
	char	nohw[100];		/* its hardware is not there: why */
	volatile int scanning;		/* configured: the scan itself runs */
};

static struct scanjob *jp;		/* the one out (or done) */
static struct scanjob last;		/* what do_scan() reports */

static void
scan_proc(struct scanjob *j)
{
	struct MsgPort *port;
	struct IOSana2Req *req;
	APTR pool;
	int rv = SCAN_FAILED, a, b;
	ULONG i;

	j->n = 0;
	j->listed = j->noname = 0;
	j->err = j->werr = 0;
	if ((port = CreateMsgPort()) == NULL)
		j->nomem = 1;
	else {
		req = (struct IOSana2Req *)CreateIORequest(port, sizeof(*req));
		if (req == NULL)
			j->nomem = 1;
		else if (!hw_check(j->device, j->unit, j->nohw, sizeof(j->nohw)))
			DeleteIORequest((struct IORequest *)req);
		else {
			req->ios2_BufferManagement = bufftags;
			if (amibsdnet_open_sana(j->device, j->unit,
			    (struct IORequest *)req, 0) == 0) {
				j->opened = 1;
				/*
				 * The interface is configured first (the stack
				 * normally did that: wifipi answers a second
				 * S2_CONFIGINTERFACE with S2ERR_BAD_STATE,
				 * downloads/sources/wifipi/src/unit.c
				 * Do_S2_CONFIGINTERFACE()).
				 */
				req->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
				if (DoIO((struct IORequest *)req) == 0) {
					for (i = 0; i < 6 && !req->ios2_SrcAddr[i];
					    i++)
						;
					if (i == 6)
						CopyMem(req->ios2_DstAddr,
						    req->ios2_SrcAddr, 6);
					req->ios2_Req.io_Command = S2_CONFIGINTERFACE;
					if (DoIO((struct IORequest *)req) != 0 &&
					    req->ios2_Req.io_Error != S2ERR_BAD_STATE &&
					    req->ios2_Req.io_Error != S2ERR_NOT_SUPPORTED &&
					    req->ios2_Req.io_Error != IOERR_NOCMD) {
						/* (its hardware does not answer:
						   nothing more for it) */
						j->cfgerr = req->ios2_Req.io_Error;
						j->werr = req->ios2_WireError;
					}
				}
				j->scanning = 1;
				if (j->cfgerr)
					;
				else if ((pool = CreatePool(MEMF_PUBLIC | MEMF_CLEAR,
				    8192, 2048)) == NULL)
					j->nomem = 1;
				else {
					req->ios2_Req.io_Command = S2_GETNETWORKS;
					req->ios2_Data = pool;
					req->ios2_StatData = NULL;
					req->ios2_DataLength = 0;
					DoIO((struct IORequest *)req);
					j->err = req->ios2_Req.io_Error;
					j->werr = req->ios2_WireError;
					j->listed = req->ios2_StatData ?
					    req->ios2_DataLength : 0;
					if (j->err == 0) {
						struct TagItem **lists =
						    req->ios2_StatData;

						for (i = 0; lists &&
						    i < req->ios2_DataLength; i++) {
							const char *ssid = (const char *)
							    tagdata(lists[i], S2INFO_SSID,
							    (ULONG)"");
							LONG sig = (LONG)tagdata(lists[i],
							    S2INFO_Signal, 0);
							int wpa1, sec = security(
							    (const UBYTE *)tagdata(
							    lists[i],
							    S2INFO_InfoElements, 0),
							    tagdata(lists[i],
							    S2INFO_Capabilities, ~0UL),
							    &wpa1);
							int k;

							if (!ssid[0]) {
								j->noname++;
								continue;	/* hidden */
							}
							/* one entry per name: the
							   strongest */
							for (k = 0; k < j->n; k++)
								if (seq(j->nets[k].ssid,
								    ssid))
									break;
							if (k < j->n &&
							    j->nets[k].signal >= sig)
								continue;
							if (k == j->n) {
								if (j->n == MAXNETS)
									continue;
								j->n++;
							}
							scopy(j->nets[k].ssid, ssid,
							    sizeof(j->nets[k].ssid));
							j->nets[k].signal = sig;
							j->nets[k].sec = sec;
							j->nets[k].wpa1 = wpa1;
						}
						rv = j->n;
					}
					DeletePool(pool);
				}
				CloseDevice((struct IORequest *)req);
			}
			DeleteIORequest((struct IORequest *)req);
		}
		DeleteMsgPort(port);
	}
	/* strongest first */
	for (a = 0; a < j->n; a++)
		for (b = 0; b + 1 < j->n - a; b++)
			if (j->nets[b].signal < j->nets[b + 1].signal) {
				struct net t;

				CopyMem(&j->nets[b], &t, sizeof(t));
				CopyMem(&j->nets[b + 1], &j->nets[b], sizeof(t));
				CopyMem(&t, &j->nets[b + 1], sizeof(t));
			}
	j->result = rv;
}

/* in the process start_scan() starts: arg is the job's address in 8 hex
   digits (its SCANJOB= argument) */
int
wifi_scan_child(const char *arg)
{
	struct scanjob *j;
	ULONG a = 0;
	int i, v;

	for (i = 0; arg[i]; i++) {
		v = arg[i] >= '0' && arg[i] <= '9' ? arg[i] - '0' :
		    arg[i] >= 'a' && arg[i] <= 'f' ? arg[i] - 'a' + 10 : -1;
		if (v < 0 || i >= 8)
			return 1;
		a = a << 4 | v;
	}
	j = (struct scanjob *)a;
	/* only a job in RAM ("If the address is not in known-space, a zero
	   will be returned.", downloads/sources/NDK3.2/Autodocs/exec.doc
	   TypeOfMem) that says it is one */
	if (i != 8 || TypeOfMem(j) == 0 || TypeOfMem((UBYTE *)j +
	    sizeof(*j) - 1) == 0 || j->magic != SCANJOB_MAGIC)
		return 1;
	scan_proc(j);
	Forbid();
	if (j->abandoned) {
		j->magic = 0;
		FreeVec(j);
	} else
		j->state = 2;
	Permit();
	return 1;
}

void
wifi_scan_release(void)
{

	Forbid();
	if (jp) {
		if (jp->state == 1)
			jp->abandoned = 1;	/* the scan frees it */
		else {
			jp->magic = 0;
			FreeVec(jp);
		}
		jp = NULL;
	}
	Permit();
}

/* "SCANJOB=" and the address in 8 hex digits */
static void
job_arg(char *d, ULONG a)
{
	int i;

	for (i = 0; "SCANJOB="[i]; i++)
		*d++ = "SCANJOB="[i];
	for (i = 28; i >= 0; i -= 4)
		*d++ = "0123456789abcdef"[(a >> i) & 15];
	*d++ = '\n';
	*d = '\0';
}

static int
start_scan(const char *device, ULONG unit)
{
	BPTR dir, old, seg;
	const char *name;
	char arg[24];

	if (!status_program(&dir, &name))
		return SCAN_NOPROC;
	if ((jp = AllocVec(sizeof(*jp), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return SCAN_FAILED;
	jp->magic = SCANJOB_MAGIC;
	scopy(jp->device, device, sizeof(jp->device));
	jp->unit = unit;
	jp->state = 1;
	old = CurrentDir(dir);
	seg = LoadSeg((CONST_STRPTR)name);
	CurrentDir(old);
	job_arg(arg, (ULONG)jp);
	/* (the driver's code runs on this stack: plenty; NP_Input left to
	   its default, an open of NIL:, since NP_Arguments needs one:
	   dos.doc CreateNewProc) */
	if (seg == 0 || CreateNewProcTags(NP_Seglist, seg, NP_FreeSeglist,
	    TRUE, NP_Name, (ULONG)"AmiBSDNet scan", NP_StackSize, 65536,
	    NP_Arguments, (ULONG)arg, TAG_DONE) == NULL) {
		if (seg)
			UnLoadSeg(seg);
		FreeVec(jp);
		jp = NULL;
		return SCAN_NOPROC;
	}
	return 0;
}

/* while it waits (a scan, connecting): the window answers; Close, Esc or
   the close gadget ask to stop (and close the window) */
static int win_quit;

/*
 * Scan for networks; returns their number or SCAN_*.  service() keeps
 * the window answering and says whether the user gave up.
 */
static int
scan(const char *device, ULONG unit, int (*service)(void))
{
	int ticks, i, rv;

	nnets = 0;
	NewList(&netlist);
	memset(&last, 0, sizeof(last));
	if (jp && jp->state == 2) {	/* old results: scan again */
		FreeVec(jp);
		jp = NULL;
	}
	if (jp == NULL && (rv = start_scan(device, unit)) != 0)
		return rv;
	/* else a scan from before is still out: wait for that one.  The
	   time counts from when the driver is configured (that alone may
	   take seconds: firmware set-up over SDIO) */
	for (ticks = 0; jp->state == 1; ticks += jp->scanning ? 5 : 1) {
		if (service() || ticks >= SCAN_SECONDS * 50) {
			last.magic = SCANJOB_MAGIC;
			last.scanning = jp->scanning;
			return win_quit ? SCAN_CANCELLED : SCAN_TIMEOUT;
		}
		Delay(5);
	}
	CopyMem(jp, &last, sizeof(last));
	FreeVec(jp);
	jp = NULL;
	nnets = last.n;
	for (i = 0; i < nnets; i++) {
		CopyMem(&last.nets[i], &nets[i], sizeof(nets[i]));
		make_label(&nets[i]);
		AddTail(&netlist, &nets[i].node);
	}
	return last.result;
}

/*
 * CreateContext() (gadtools.library LVO -114, A0 = &glist): the NDK's
 * inline macro for it does not compile with current GCC.
 */
struct Gadget *
create_context(struct Gadget **glistp)
{
	register struct Gadget *res __asm("d0");
	register ULONG d1 __asm("d1");
	register struct Gadget **a0 __asm("a0") = glistp;
	register ULONG a1 __asm("a1");
	register struct Library *a6 __asm("a6") = GadToolsBase;

	__asm volatile ("jsr -114(%%a6)"
	    : "=r"(res), "=r"(d1), "+r"(a0), "=r"(a1)
	    : "r"(a6)
	    : "fp0", "fp1", "cc", "memory");
	return res;
}

/* ------------------------------------------------------------------------ */

#define	GID_LIST	1
#define	GID_PASS	2
#define	GID_SCAN	3
#define	GID_CONNECT	4
#define	GID_CLOSE	5
#define	GID_STATUS	6

static struct Gadget *g_list, *g_pass, *g_status;
static struct Window *win;
static struct Hook pass_hook;
static char pass_real[65];		/* the passphrase; the field has '*' */

static void
set_status(const char *text)
{
	struct TagItem t[] = { { GTTX_Text, (ULONG)text }, { TAG_DONE, 0 } };

	GT_SetGadgetAttrsA(g_status, win, NULL, t);
}

/* new labels, nothing selected (GTLV_Selected: "Starting with V39, you
   can provide ~0" to deselect, downloads/sources/NDK3.2/Autodocs/
   gadtools.doc, GT_SetGadgetAttrsA LISTVIEW_KIND) */
static void
set_list(struct List *l)
{
	struct TagItem t[] = { { GTLV_Labels, (ULONG)l },
	    { GTLV_Selected, ~0UL }, { TAG_DONE, 0 } };

	GT_SetGadgetAttrsA(g_list, win, NULL, t);
}

static int
service_window(void)
{
	struct IntuiMessage *im;

	while ((im = GT_GetIMsg(win->UserPort)) != NULL) {
		ULONG cls = im->Class;
		UWORD code = im->Code;
		struct Gadget *gad = (struct Gadget *)im->IAddress;

		GT_ReplyIMsg(im);
		if (cls == IDCMP_CLOSEWINDOW ||
		    (cls == IDCMP_VANILLAKEY && (code == 27 || code == 'l' ||
		    code == 'L')) ||
		    (cls == IDCMP_GADGETUP && gad->GadgetID == GID_CLOSE))
			win_quit = 1;
		else if (cls == IDCMP_REFRESHWINDOW) {
			GT_BeginRefresh(win);
			GT_EndRefresh(win, TRUE);
		}
	}
	return win_quit;
}

/* ticks/50 s with the window answering; 1 if it was asked to close */
static int
wait_serviced(int ticks)
{

	for (; ticks > 0; ticks -= 5) {
		service_window();
		Delay(5);
	}
	return service_window();
}

/* "text" plus a number */
static char *
put_num(char *p, LONG v)
{
	char t[12];
	int i = 0;
	ULONG u = v < 0 ? -v : v;

	if (v < 0)
		*p++ = '-';
	do {
		t[i++] = '0' + u % 10;
		u /= 10;
	} while (u);
	while (i)
		*p++ = t[--i];
	*p = '\0';
	return p;
}

static char *
put_str(char *p, const char *s)
{

	while (*s)
		*p++ = *s++;
	*p = '\0';
	return p;
}

static void
do_scan(const struct NetCtrlIface *ifc)
{
	static char msg[160];
	char *p = msg;
	int n;

	set_status("Scanning... (Close stops it)");
	set_list((struct List *)~0);
	n = scan(ifc->device, ifc->unit, service_window);
	set_list(&netlist);
	if (n > 0) {
		set_status("Choose a network and Connect");
		return;
	}
	/* nothing to choose: say exactly what the driver did */
	if (n == SCAN_CANCELLED)
		p = put_str(p, "Stopped waiting for the scan");
	else if (n == SCAN_NOPROC)
		p = put_str(p, "Cannot start the scan (the program file "
		    "AmiBSDNetStatus is not found)");
	else if (n == SCAN_TIMEOUT)
		p = put_str(p, last.scanning ? "No answer from the driver to "
		    "the scan in 60 s" : "The driver is still being set up");
	else if (last.magic == 0 || last.nomem)
		p = put_str(p, "Not enough memory for a scan");
	else if (last.nohw[0]) {
		p = put_str(p, "Not used: ");
		p = put_str(p, last.nohw);
	} else if (last.cfgerr) {
		p = put_str(p, "The driver cannot be configured (error ");
		p = put_num(p, last.cfgerr);
		p = put_str(p, "/");
		p = put_num(p, last.werr);
		p = put_str(p, "): hardware missing?");
	}
	else if (!last.opened)
		p = put_str(p, "The driver does not open");
	else if (last.err) {
		p = put_str(p, "The driver cannot scan (error ");
		p = put_num(p, last.err);
		p = put_str(p, "/");
		p = put_num(p, last.werr);
		p = put_str(p, ")");
	} else {
		p = put_str(p, "No networks: the driver listed ");
		p = put_num(p, (LONG)last.listed);
		if (last.noname) {
			p = put_str(p, ", ");
			p = put_num(p, (LONG)last.noname);
			p = put_str(p, " without a name");
		}
	}
	set_status(msg);
}

static void
fmt_ip(char *b, ULONG a)
{
	int i, n = 0;

	for (i = 24; i >= 0; i -= 8) {
		ULONG v = (a >> i) & 255;

		if (v >= 100) b[n++] = '0' + v / 100;
		if (v >= 10) b[n++] = '0' + (v / 10) % 10;
		b[n++] = '0' + v % 10;
		if (i)
			b[n++] = '.';
	}
	b[n] = '\0';
}

static void
do_connect(const struct NetCtrlIface *ifc, int sel)
{
	const char *pass = pass_real;
	static char msg[80];
	int i, plen = 0, rc;

	if (sel < 0 || sel >= nnets) {
		set_status("Choose a network first");
		return;
	}
	switch (nets[sel].sec) {
	case SEC_WEP:
		set_status("WEP networks are not supported");
		return;
	case SEC_SAE:
		/* (WirelessManager is wpa_supplicant 0.7.3: no SAE among its
		   key managements, downloads/sources/AROS/WirelessManager/
		   src/common/defs.h WPA_KEY_MGMT_*) */
		set_status("WPA3-only networks are not supported");
		return;
	case SEC_EAP:
		set_status("802.1X (Enterprise) networks are not supported");
		return;
	case SEC_OTHER:
		set_status("This network's key management is not supported");
		return;
	}
	while (pass[plen])
		plen++;
	if (nets[sel].sec == SEC_OPEN)
		plen = 0;		/* open: no passphrase */
	/* (not known: empty for an open network, or a passphrase) */
	if ((nets[sel].sec == SEC_PSK || plen > 0) &&
	    !wm_passphrase_ok(pass)) {
		set_status("Passphrase: 8 to 63 characters, or 64 hex digits");
		ActivateGadget(g_pass, win, NULL);
		return;
	}
	/* (security not known and no passphrase typed: what Wireless.prefs
	   has for it stays) */
	if (wm_set_network(nets[sel].ssid, plen > 0 ? pass :
	    nets[sel].sec == SEC_UNKNOWN ? NULL : "", 1)) {
		set_status("Cannot write Wireless.prefs");
		return;
	}
	/* (WirelessManager is stopped and started again whatever is
	   clicked meanwhile: the network must not stay without it) */
	set_status("Restarting WirelessManager...");
	wm_signal_stop();
	for (i = 0; i < 75 && wm_running(); i++)
		wait_serviced(10);
	if (wm_running()) {
		set_status("WirelessManager does not stop; try again");
		return;
	}
	if (!wm_installed()) {
		set_status("C:WirelessManager is missing");
		return;
	}
	/* (-1: it could not be loaded or started; -2: it runs, wm.c) */
	if ((rc = wm_start(ifc->device, ifc->unit)) != 0) {
		set_status(rc == -2 ? "WirelessManager runs already; try again" :
		    "C:WirelessManager could not be started");
		return;
	}
	set_status("Connecting...");
	/* new network: drop the old address and let DHCP start over */
	wait_serviced(150);
	stack_cmd(NETCTRL_OFFLINE);
	stack_cmd(NETCTRL_ONLINE);
	for (i = 0; i < 40 && !win_quit; i++) {
		wait_serviced(25);
		if (stack_cmd(NETCTRL_STATE) == 0 && status_msg()->online) {
			char ip[16];

			fmt_ip(ip, status_msg()->address);
			scopy(msg, "Connected, address ", sizeof(msg));
			scopy(msg + 19, ip, sizeof(msg) - 19);
			set_status(msg);
			return;
		}
	}
	set_status("Saved; no address yet (check the passphrase)");
}

void
wifi_window(const struct NetCtrlIface *ifc)
{
	struct Screen *scr;
	APTR vi;
	struct Gadget *glist = NULL, *g;
	struct NewGadget ng;
	struct IntuiMessage *im;
	int quit = 0, sel = -1;
	UWORD fh, top, w = 380, h;

	if ((GadToolsBase = OpenLibrary("gadtools.library", 37)) == NULL)
		return;
	if ((scr = LockPubScreen(NULL)) == NULL)
		goto out;
	if ((vi = GetVisualInfoA(scr, NULL)) == NULL)
		goto unlock;
	fh = scr->Font->ta_YSize;
	top = scr->WBorTop + fh + 1;
	h = top + 10 * fh + 4 * (fh + 6) + 30;

	NewList(&netlist);
	g = create_context(&glist);
	memset(&ng, 0, sizeof(ng));
	ng.ng_TextAttr = scr->Font;
	ng.ng_VisualInfo = vi;

	ng.ng_LeftEdge = 10;
	ng.ng_TopEdge = top + 6;
	ng.ng_Width = w - 20;
	ng.ng_Height = 10 * fh + 4;
	ng.ng_GadgetID = GID_LIST;
	{
		struct TagItem t[] = { { GTLV_Labels, (ULONG)&netlist },
		    { GTLV_ShowSelected, 0 }, { TAG_DONE, 0 } };

		g = g_list = CreateGadgetA(LISTVIEW_KIND, g, &ng, t);
	}
	ng.ng_TopEdge += ng.ng_Height + 6;
	ng.ng_Height = fh + 6;
	ng.ng_LeftEdge = 110;
	ng.ng_Width = w - 120;
	ng.ng_GadgetText = (UBYTE *)"Passphrase";
	ng.ng_GadgetID = GID_PASS;
	{
		/* (64: a PSK given as 64 hex digits, wm_passphrase_ok();
		   shown as '*', the text in pass_real) */
		struct TagItem t[] = { { GTST_MaxChars, 64 },
		    { GTST_EditHook, (ULONG)&pass_hook }, { TAG_DONE, 0 } };

		secret_hook_init(&pass_hook, pass_real);
		g = g_pass = CreateGadgetA(STRING_KIND, g, &ng, t);
	}
	ng.ng_TopEdge += ng.ng_Height + 6;
	ng.ng_LeftEdge = 10;
	ng.ng_Width = w - 20;
	ng.ng_GadgetText = NULL;
	ng.ng_GadgetID = GID_STATUS;
	{
		struct TagItem t[] = { { GTTX_Text, (ULONG)"" },
		    { GTTX_Border, TRUE }, { TAG_DONE, 0 } };

		g = g_status = CreateGadgetA(TEXT_KIND, g, &ng, t);
	}
	ng.ng_TopEdge += ng.ng_Height + 6;
	ng.ng_Width = (w - 40) / 3;
	ng.ng_GadgetText = (UBYTE *)"_Scan";
	ng.ng_GadgetID = GID_SCAN;
	ng.ng_Flags = PLACETEXT_IN;
	{
		struct TagItem t[] = { { GT_Underscore, '_' }, { TAG_DONE, 0 } };

		g = CreateGadgetA(BUTTON_KIND, g, &ng, t);
		ng.ng_LeftEdge += ng.ng_Width + 10;
		ng.ng_GadgetText = (UBYTE *)"_Connect";
		ng.ng_GadgetID = GID_CONNECT;
		g = CreateGadgetA(BUTTON_KIND, g, &ng, t);
		ng.ng_LeftEdge += ng.ng_Width + 10;
		ng.ng_GadgetText = (UBYTE *)"C_lose";
		ng.ng_GadgetID = GID_CLOSE;
		g = CreateGadgetA(BUTTON_KIND, g, &ng, t);
	}
	h = ng.ng_TopEdge + ng.ng_Height + scr->WBorBottom + 6;
	if (g == NULL)
		goto freegads;

	{
		struct TagItem t[] = {
			{ WA_Title, (ULONG)"AmiBSDNet Wi-Fi" },
			{ WA_Width, w }, { WA_Height, h },
			{ WA_Gadgets, (ULONG)glist },
			{ WA_PubScreen, (ULONG)scr },
			{ WA_DragBar, TRUE }, { WA_DepthGadget, TRUE },
			{ WA_CloseGadget, TRUE }, { WA_Activate, TRUE },
			{ WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			    IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE |
			    IDCMP_VANILLAKEY | IDCMP_INTUITICKS },
			{ TAG_DONE, 0 }
		};

		if ((win = OpenWindowTagList(NULL, t)) == NULL)
			goto freegads;
	}
	GT_RefreshWindow(win, NULL);
	win_quit = 0;
	do_scan(ifc);

	while (!quit && !win_quit) {
		WaitPort(win->UserPort);
		while (!win_quit && (im = GT_GetIMsg(win->UserPort)) != NULL) {
			ULONG cls = im->Class;
			UWORD code = im->Code;
			struct Gadget *gad = (struct Gadget *)im->IAddress;

			GT_ReplyIMsg(im);
			switch (cls) {
			case IDCMP_CLOSEWINDOW:
				quit = 1;
				break;
			case IDCMP_REFRESHWINDOW:
				GT_BeginRefresh(win);
				GT_EndRefresh(win, TRUE);
				break;
			case IDCMP_VANILLAKEY:
				if (code == 27)
					quit = 1;
				else if (code == 's' || code == 'S') {
					sel = -1;	/* new list */
					do_scan(ifc);
				} else if (code == 'c' || code == 'C')
					do_connect(ifc, sel);
				else if (code == 'l' || code == 'L')
					quit = 1;
				break;
			case IDCMP_GADGETUP:
				switch (gad->GadgetID) {
				case GID_LIST:
					sel = code;
					if (sel >= 0 && sel < nnets)
						set_status(nets[sel].sec ==
						    SEC_PSK ? "Enter the "
						    "passphrase, then Connect" :
						    nets[sel].sec == SEC_UNKNOWN ?
						    "Passphrase (empty if it is "
						    "open), then Connect" :
						    nets[sel].sec == SEC_OPEN ?
						    "Open network: press Connect" :
						    "This network's security is "
						    "not supported");
					if (sel >= 0 && sel < nnets &&
					    (nets[sel].sec == SEC_PSK ||
					    nets[sel].sec == SEC_UNKNOWN))
						ActivateGadget(g_pass, win, NULL);
					break;
				case GID_PASS:
					do_connect(ifc, sel);
					break;
				case GID_SCAN:
					sel = -1;	/* new list */
					do_scan(ifc);
					break;
				case GID_CONNECT:
					do_connect(ifc, sel);
					break;
				case GID_CLOSE:
					quit = 1;
					break;
				}
				break;
			}
		}
	}
	CloseWindow(win);
	win = NULL;
freegads:
	FreeGadgets(glist);
	FreeVisualInfo(vi);
unlock:
	UnlockPubScreen(NULL, scr);
out:
	CloseLibrary(GadToolsBase);
}
