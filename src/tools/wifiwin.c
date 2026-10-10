/*
 * AmiBSDNetStatus: the Wi-Fi window.  Lists the networks in range (scan
 * through the wireless SANA-II driver), takes a passphrase, stores the
 * network in Wireless.prefs and restarts WirelessManager, then has the
 * stack fetch a new address.
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
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <amibsdnet/control.h>
#include <amibsdnet/wm.h>
#include <amibsdnet/devopen.h>

#include "statustool.h"

struct Library *GadToolsBase;

/* the SANA-II definitions this window needs */
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
#define	S2INFO_InfoElements	(TAG_USER + 11)
#define	S2_CopyToBuff		(TAG_USER + 0xb0001)
#define	S2_CopyFromBuff		(TAG_USER + 0xb0002)

#define	MAXNETS	24

struct net {
	struct Node node;		/* ln_Name -> label */
	char	ssid[34];
	char	label[64];
	LONG	signal;
	int	protected;		/* 1 WPA/WPA2, 2 not known */
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

/* WPA2 (RSN element) or WPA (vendor element) present? */
static const char *
protection(const UBYTE *ies)
{
	ULONG len, off;
	const char *p = "open";

	if (ies == NULL)
		return "?";
	/* (a length word, then the elements: never more than a frame
	   holds, whatever the driver says) */
	len = *(const UWORD *)ies;
	if (len > 2304)
		len = 2304;
	ies += 2;
	for (off = 0; off + 2 <= len; off += 2 + ies[off + 1]) {
		UBYTE id = ies[off], l = ies[off + 1];

		if (off + 2 + l > len)
			break;
		if (id == 48)
			return "WPA2";
		if (id == 221 && l >= 4 && ies[off + 2] == 0x00 &&
		    ies[off + 3] == 0x50 && ies[off + 4] == 0xf2 &&
		    ies[off + 5] == 0x01)
			p = "WPA";
	}
	return p;
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

	for (s = n->ssid; *s && p < n->label + 36; )
		*p++ = *s++;
	while (p < n->label + 34)
		*p++ = ' ';
	for (s = n->protected == 1 ? "WPA " : n->protected ? "? " : "open ";
	    *s; )
		*p++ = *s++;
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
#define	SCAN_SECONDS	60

#define	S2_GETSTATIONADDRESS	(CMD_NONSTD + 1)
#define	S2_CONFIGINTERFACE	(CMD_NONSTD + 2)

/*
 * The scan runs in its own process, which owns the request, its memory
 * pool and the open device, and frees them only when the driver really
 * answers.  It must never be aborted: wifipi.device's AbortIO() replies an
 * active scan without ending it, and the driver then writes into the
 * freed pool and blocks every later scan (WirelessManager's too).  The
 * window only waits for the job; Close or a timeout stop waiting, and no
 * new scan is started while one is still out.
 */
static struct {
	volatile int state;		/* 0 idle, 1 running, 2 done */
	int result;
	char device[64];
	ULONG unit;
	int n;
	struct net nets[MAXNETS];
	/* what the driver said (shown when there is nothing to choose) */
	ULONG listed;			/* entries it returned */
	ULONG noname;			/* of those without a name */
	LONG err, werr;			/* io_Error, ios2_WireError */
	int opened;			/* it opened at all */
	int nomem;			/* no memory for the results */
	LONG cfgerr;			/* S2_CONFIGINTERFACE failed so */
	volatile int scanning;		/* configured: the scan itself runs */
} job;

static void
scan_proc(void)
{
	struct MsgPort *port;
	struct IOSana2Req *req;
	APTR pool;
	int rv = SCAN_FAILED, a, b;
	ULONG i;

	job.n = 0;
	job.listed = job.noname = 0;
	job.err = job.werr = 0;
	if ((port = CreateMsgPort()) == NULL)
		job.nomem = 1;
	else {
		req = (struct IOSana2Req *)CreateIORequest(port, sizeof(*req));
		if (req == NULL)
			job.nomem = 1;
		else {
			req->ios2_BufferManagement = bufftags;
			if (amibsdnet_open_sana(job.device, job.unit,
			    (struct IORequest *)req, 0) == 0) {
				job.opened = 1;
				/*
				 * The firmware scans only once the interface is
				 * configured (the stack normally did that; a
				 * second configure fails harmlessly).
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
						job.cfgerr = req->ios2_Req.io_Error;
						job.werr = req->ios2_WireError;
					}
				}
				job.scanning = 1;
				if (job.cfgerr)
					;
				else if ((pool = CreatePool(MEMF_ANY | MEMF_CLEAR,
				    8192, 2048)) == NULL)
					job.nomem = 1;
				else {
					req->ios2_Req.io_Command = S2_GETNETWORKS;
					req->ios2_Data = pool;
					req->ios2_StatData = NULL;
					req->ios2_DataLength = 0;
					DoIO((struct IORequest *)req);
					job.err = req->ios2_Req.io_Error;
					job.werr = req->ios2_WireError;
					job.listed = req->ios2_StatData ?
					    req->ios2_DataLength : 0;
					if (job.err == 0) {
						struct TagItem **lists =
						    req->ios2_StatData;

						for (i = 0; lists &&
						    i < req->ios2_DataLength; i++) {
							const char *ssid = (const char *)
							    tagdata(lists[i], S2INFO_SSID,
							    (ULONG)"");
							LONG sig = (LONG)tagdata(lists[i],
							    S2INFO_Signal, 0);
							const char *prot = protection(
							    (const UBYTE *)tagdata(
							    lists[i], S2INFO_InfoElements,
							    0));
							int j;

							if (!ssid[0]) {
								job.noname++;
								continue;	/* hidden */
							}
							/* one entry per name: the
							   strongest */
							for (j = 0; j < job.n; j++)
								if (seq(job.nets[j].ssid,
								    ssid))
									break;
							if (j < job.n &&
							    job.nets[j].signal >= sig)
								continue;
							if (j == job.n) {
								if (job.n == MAXNETS)
									continue;
								job.n++;
							}
							scopy(job.nets[j].ssid, ssid,
							    sizeof(job.nets[j].ssid));
							job.nets[j].signal = sig;
							job.nets[j].protected =
							    seq(prot, "open") ? 0 :
							    seq(prot, "?") ? 2 : 1;
						}
						rv = job.n;
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
	for (a = 0; a < job.n; a++)
		for (b = 0; b + 1 < job.n - a; b++)
			if (job.nets[b].signal < job.nets[b + 1].signal) {
				struct net t;

				CopyMem(&job.nets[b], &t, sizeof(t));
				CopyMem(&job.nets[b + 1], &job.nets[b], sizeof(t));
				CopyMem(&t, &job.nets[b + 1], sizeof(t));
			}
	job.result = rv;
	/* under Forbid(), which lasts until this process is gone: the
	   program may be unloaded as soon as the state says "done" */
	Forbid();
	job.state = 2;
}

/* is a scan process still out?  (the status tool must not quit then: the
   process runs this program's code) */
int
wifi_scan_busy(void)
{

	return job.state == 1;
}

/*
 * Scan for networks; returns their number or SCAN_*.  cancelled() keeps
 * the window answering and says whether the user gave up.
 */
static int
scan(const char *device, ULONG unit, int (*cancelled)(void))
{
	int ticks, i, rv;

	nnets = 0;
	NewList(&netlist);
	if (job.state == 2)
		job.state = 0;		/* old results: scan again */
	if (job.state == 0) {
		scopy(job.device, device, sizeof(job.device));
		job.unit = unit;
		job.opened = job.nomem = job.scanning = 0;
		job.cfgerr = 0;
		job.state = 1;
		/* (the driver's code runs on this stack: plenty) */
		if (CreateNewProcTags(NP_Entry, (ULONG)scan_proc,
		    NP_Name, (ULONG)"AmiBSDNet scan", NP_StackSize, 65536,
		    TAG_DONE) == NULL) {
			job.state = 0;
			job.nomem = 1;
			return SCAN_FAILED;
		}
	}
	/* else a scan from before is still out: wait for that one.  The
	   time counts from when the driver is configured (that alone may
	   take seconds: firmware set-up over SDIO) */
	for (ticks = 0; job.state == 1; ticks += job.scanning ? 5 : 1) {
		if (cancelled())
			return SCAN_CANCELLED;
		if (ticks >= SCAN_SECONDS * 50)
			return SCAN_TIMEOUT;
		Delay(5);
	}
	nnets = job.n;
	for (i = 0; i < nnets; i++) {
		CopyMem(&job.nets[i], &nets[i], sizeof(nets[i]));
		make_label(&nets[i]);
		AddTail(&netlist, &nets[i].node);
	}
	rv = job.result;
	job.state = 0;
	return rv;
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

static void
set_status(const char *text)
{
	struct TagItem t[] = { { GTTX_Text, (ULONG)text }, { TAG_DONE, 0 } };

	GT_SetGadgetAttrsA(g_status, win, NULL, t);
}

static void
set_list(struct List *l)
{
	struct TagItem t[] = { { GTLV_Labels, (ULONG)l }, { TAG_DONE, 0 } };

	GT_SetGadgetAttrsA(g_list, win, NULL, t);
}

/* during a scan: keep the window answering; Close, Esc or the close
   gadget stop the scan (and close the window) */
static int scan_quit;

static int
scan_cancelled(void)
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
			scan_quit = 1;
		else if (cls == IDCMP_REFRESHWINDOW) {
			GT_BeginRefresh(win);
			GT_EndRefresh(win, TRUE);
		}
	}
	return scan_quit;
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
	static char msg[120];
	char *p = msg;
	int n;

	set_status("Scanning... (Close stops it)");
	set_list((struct List *)~0);
	n = scan(ifc->device, ifc->unit, scan_cancelled);
	set_list(&netlist);
	if (n > 0) {
		set_status("Choose a network and Connect");
		return;
	}
	/* nothing to choose: say exactly what the driver did */
	if (n == SCAN_CANCELLED)
		p = put_str(p, "Stopped waiting for the scan");
	else if (n == SCAN_TIMEOUT)
		p = put_str(p, job.scanning ? "No answer from the driver to "
		    "the scan in 60 s" : "The driver is still being set up");
	else if (job.nomem)
		p = put_str(p, "Not enough memory for a scan");
	else if (job.cfgerr) {
		p = put_str(p, "The driver cannot be configured (error ");
		p = put_num(p, job.cfgerr);
		p = put_str(p, "/");
		p = put_num(p, job.werr);
		p = put_str(p, "): hardware missing?");
	}
	else if (!job.opened)
		p = put_str(p, "The driver does not open");
	else if (job.err) {
		p = put_str(p, "The driver cannot scan (error ");
		p = put_num(p, job.err);
		p = put_str(p, "/");
		p = put_num(p, job.werr);
		p = put_str(p, ")");
	} else {
		p = put_str(p, "No networks: the driver listed ");
		p = put_num(p, (LONG)job.listed);
		if (job.noname) {
			p = put_str(p, ", ");
			p = put_num(p, (LONG)job.noname);
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
	struct StringInfo *si = (struct StringInfo *)g_pass->SpecialInfo;
	const char *pass = (const char *)si->Buffer;
	static char msg[80];
	int i, plen = 0;

	if (sel < 0 || sel >= nnets) {
		set_status("Choose a network first");
		return;
	}
	while (pass[plen])
		plen++;
	if (!nets[sel].protected)
		plen = 0;		/* open: no passphrase */
	/* (not known: empty for an open network, or a passphrase) */
	if ((nets[sel].protected == 1 || plen > 0) &&
	    (plen < 8 || plen > 63)) {
		set_status("Enter the passphrase (8 to 63 characters)");
		ActivateGadget(g_pass, win, NULL);
		return;
	}
	if (wm_set_network(nets[sel].ssid, plen > 0 ? pass : NULL)) {
		set_status("Cannot write Wireless.prefs");
		return;
	}
	set_status("Restarting WirelessManager...");
	if (wm_stop() != 0) {
		set_status("WirelessManager does not stop; try again");
		return;
	}
	if (!wm_installed() || wm_start(ifc->device, ifc->unit) != 0) {
		set_status("C:WirelessManager is missing");
		return;
	}
	set_status("Connecting...");
	/* new network: drop the old address and let DHCP start over */
	Delay(150);
	stack_cmd(NETCTRL_OFFLINE);
	stack_cmd(NETCTRL_ONLINE);
	for (i = 0; i < 40; i++) {
		Delay(25);
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
		struct TagItem t[] = { { GTST_MaxChars, 63 }, { TAG_DONE, 0 } };

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
	scan_quit = 0;
	do_scan(ifc);

	while (!quit && !scan_quit) {
		WaitPort(win->UserPort);
		while ((im = GT_GetIMsg(win->UserPort)) != NULL) {
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
				else if (code == 's' || code == 'S')
					do_scan(ifc);
				else if (code == 'c' || code == 'C')
					do_connect(ifc, sel);
				else if (code == 'l' || code == 'L')
					quit = 1;
				break;
			case IDCMP_GADGETUP:
				switch (gad->GadgetID) {
				case GID_LIST:
					sel = code;
					if (sel >= 0 && sel < nnets)
						set_status(nets[sel].protected == 1 ?
						    "Enter the passphrase, then Connect" :
						    nets[sel].protected ? "Passphrase "
						    "(empty if it is open), then Connect" :
						    "Open network: press Connect");
					if (sel >= 0 && sel < nnets &&
					    nets[sel].protected)
						ActivateGadget(g_pass, win, NULL);
					break;
				case GID_PASS:
					do_connect(ifc, sel);
					break;
				case GID_SCAN:
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
