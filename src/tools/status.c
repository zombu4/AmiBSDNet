/*
 * AmiBSDNetStatus: Workbench status icon and Commodity for AmiBSDNet.
 *
 * Shows an AppIcon on the Workbench desktop that reflects the network
 * state ("Net: <IP address>" when online, else "Net: Offline", "Net: No
 * cable", ...).
 * Double-clicking it opens a status window with Go Online/Offline,
 * Reconnect, Wi-Fi... and Settings... (drivers, DHCP or fixed address,
 * DNS, host name, Wi-Fi network); both are also in the Workbench Tools
 * menu ("AmiBSDNet..." and "AmiBSDNet Settings...").  It registers with Exchange as the "AmiBSDNet" commodity:
 * Show opens the status window, Enable/Disable switch the stack
 * online/offline, Remove quits.  If the stack is not running, the window
 * offers to start it.
 *
 * Tool types / arguments:
 *   STACKCMD=<command> how to start the stack (default "C:AmiBSDNet";
 *                      also read as STACK=, its name before 0.8)
 *   INTERVAL=<seconds> state polling interval (default 2)
 *
 * Put it in WBStartup to have it on every boot.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <libraries/commodities.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/wb.h>
#include <proto/icon.h>
#include <proto/commodities.h>

#include <amibsdnet/control.h>
#include <amibsdnet/ctlcall.h>
#include <amibsdnet/notice.h>
#include <amibsdnet/devopen.h>

#include "statustool.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *WorkbenchBase;
struct Library *IconBase;
struct Library *CxBase;

static const char verstag[] __attribute__((used)) =
    "\0$VER: AmiBSDNetStatus 0.8.2 (10.10.2026)";

#define	ICON_W	32
#define	ICON_H	22
#define	WORDS_PER_ROW	(ICON_W / 16)

static char stackcmd[128] = "C:AmiBSDNet";
static LONG interval = 2;

static struct NetCtrlMsg *ctl;	/* reused for every request */
static UWORD *chipimg[2];	/* [0] offline, [1] online */
static struct Image images[2];
static struct DiskObject dobj[2];
static struct AppIcon *appicon;
static struct AppMenuItem *appmenu, *appmenu2;

#define	MENU_STATUS	0
#define	MENU_SETTINGS	1
static int shown_state = -1;	/* -1 none, 0 offline, 1 online, 2 no stack,
				   3 no link, 4 connecting */
static char label[32];

/* ------------------------------------------------------------------------
 * talking to the stack
 */

/* (a request the stack did not answer in time: ctl is still its) */
static struct MsgPort *ctl_left;

int
stack_cmd(ULONG cmd)
{
	struct MsgPort *left;
	int r;

	if (ctl_left) {
		if (!amibsdnet_ctl_reclaim(ctl_left))
			return -2;	/* still not answered */
		ctl_left = NULL;
	}
	r = amibsdnet_ctl_call(ctl, cmd, 10, &left);
	if (r == -2)
		ctl_left = left;
	return r;
}

struct NetCtrlMsg *
status_msg(void)
{

	return ctl;
}

void *
memcpy(void *d, const void *src, unsigned long n)
{
	char *p = d;
	const char *q = src;

	while (n--)
		*p++ = *q++;
	return d;
}

void *
memset(void *d, int c, unsigned long n)
{
	char *p = d;

	while (n--)
		*p++ = c;
	return d;
}

/* ------------------------------------------------------------------------
 * icon imagery: drawn procedurally into a 4-colour (2 plane) image.
 * Pens (Workbench default palette): 0 grey, 1 black, 2 white, 3 blue.
 */

static UBYTE pix[ICON_H][ICON_W];

static void
fill(int x0, int y0, int x1, int y1, UBYTE c)
{
	int x, y;

	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
			if (x >= 0 && x < ICON_W && y >= 0 && y < ICON_H)
				pix[y][x] = c;
}

static void
draw(int online)
{
	int i;

	fill(0, 0, ICON_W - 1, ICON_H - 1, 0);
	/* monitor */
	fill(2, 1, 19, 13, 1);
	fill(3, 2, 18, 12, 2);
	fill(4, 3, 17, 11, online ? 3 : 0);
	fill(8, 14, 13, 15, 1);
	fill(5, 16, 16, 17, 1);
	/* network: a cable to a node */
	fill(20, 7, 25, 7, online ? 3 : 1);
	fill(26, 4, 30, 10, 1);
	fill(27, 5, 29, 9, online ? 3 : 0);
	fill(28, 11, 28, 19, online ? 3 : 1);
	fill(22, 19, 31, 19, online ? 3 : 1);
	if (online) {
		/* little activity marks on the screen */
		fill(6, 5, 11, 5, 2);
		fill(6, 7, 14, 7, 2);
		fill(6, 9, 9, 9, 2);
	} else {
		/* a cross over the screen */
		for (i = 0; i < 8; i++) {
			fill(7 + i, 3 + i, 7 + i, 3 + i, 1);
			fill(14 - i, 3 + i, 14 - i, 3 + i, 1);
		}
	}
}

static int
make_image(int online)
{
	UWORD *data;
	int x, y, plane;

	/* image data is blitted by Workbench: it must be in Chip RAM */
	data = AllocVec(2 * ICON_H * WORDS_PER_ROW * 2, MEMF_CHIP | MEMF_CLEAR);
	if (data == NULL)
		return -1;
	draw(online);
	for (plane = 0; plane < 2; plane++)
		for (y = 0; y < ICON_H; y++)
			for (x = 0; x < ICON_W; x++)
				if (pix[y][x] & (1 << plane))
					data[(plane * ICON_H + y) * WORDS_PER_ROW +
					    x / 16] |= 0x8000 >> (x & 15);
	chipimg[online] = data;

	images[online].Width = ICON_W;
	images[online].Height = ICON_H;
	images[online].Depth = 2;
	images[online].ImageData = data;
	images[online].PlanePick = 3;

	dobj[online].do_Magic = WB_DISKMAGIC;
	dobj[online].do_Version = WB_DISKVERSION;
	dobj[online].do_Gadget.Width = ICON_W;
	dobj[online].do_Gadget.Height = ICON_H;
	dobj[online].do_Gadget.Flags = GFLG_GADGIMAGE | GFLG_GADGHCOMP;
	dobj[online].do_Gadget.GadgetRender = &images[online];
	dobj[online].do_Type = WBPROJECT;
	dobj[online].do_CurrentX = NO_ICON_POSITION;
	dobj[online].do_CurrentY = NO_ICON_POSITION;
	return 0;
}

/* ------------------------------------------------------------------------
 * the AppIcon
 */

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
copy(char *d, const char *s, int n)
{

	while (--n > 0 && *s)
		*d++ = *s++;
	*d = '\0';
}

/* s added to d, on a line of its own */
static void
append(char *d, const char *s, int n)
{
	int l = 0;

	while (d[l])
		l++;
	if (l && l < n - 1)
		d[l++] = '\n';
	copy(d + l, s, n - l);
}

static int
strcmp_(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (UBYTE)*a - (UBYTE)*b;
}

/* the start notice (amibsdnet/notice.h): each step is shown first */
static struct notice snotice;
static int starting;

static void
step(const char *what)
{

	if (starting)
		notice_say(&snotice, what);
}

/* the start is over (or the program ends): the notice and marker go */
static void
start_over(void)
{

	if (starting) {
		starting = 0;
		notice_close(&snotice);
		guard_end("Status");
	}
}

static void
update_icon(struct MsgPort *appport)
{
	int state;
	const char *text = "Offline";
	ULONG i;

	step("asking the stack");
	if ((i = stack_cmd(NETCTRL_IFLIST)) != 0) {
		state = 2;
		text = (LONG)i == -2 ? "Not answering" : "No network";
	} else if (ctl->online) {
		state = 1;
	} else {
		state = 0;
		/* say why: no cable / no Wi-Fi, or still getting an address */
		for (i = 0; i < ctl->nifaces; i++) {
			UWORD f = ctl->ifaces[i].flags;

			if (!(f & NETIF_ADMIN))
				continue;
			if (f & NETIF_UNVERIFIED) {
				text = "Bad driver";
				state = 3;
			} else if (f & NETIF_NODRIVER) {
				text = "No driver";
				state = 3;
			} else if (!(f & NETIF_LINK)) {
				text = (f & NETIF_WIRELESS) ? "No Wi-Fi" :
				    "No cable";
				state = 3;
			} else if (state != 3) {
				text = "Connecting";
				state = 4;
			}
		}
	}
	char newlabel[sizeof(label)];

	/* "Net: " so the icon is recognisable as the network status */
	copy(newlabel, "Net: ", sizeof(newlabel));
	if (state == 1)
		fmt_ip(newlabel + 5, ctl->address);
	else
		copy(newlabel + 5, text, sizeof(newlabel) - 5);
	if (state == shown_state && appicon &&
	    !strcmp_(newlabel, label))
		return;
	copy(label, newlabel, sizeof(label));
	if (appicon)
		RemoveAppIcon(appicon);
	step("adding the icon");
	appicon = AddAppIconA(0, 0, (UBYTE *)label, appport, 0,
	    &dobj[state == 1 ? 1 : 0], NULL);
	/* if Workbench was not ready, try again at the next update */
	shown_state = appicon ? state : -1;
}

/* ------------------------------------------------------------------------
 * the status window (an EasyRequest)
 */

static void
start_stack(void)
{
	BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
	BPTR out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);

	/* (asynchronous: the handles are the caller's only on failure) */
	if (SystemTags((CONST_STRPTR)stackcmd, SYS_Asynch, TRUE,
	    SYS_Input, in, SYS_Output, out, TAG_DONE) != 0) {
		if (in)
			Close(in);
		if (out)
			Close(out);
	}
}

static void
show_settings(struct MsgPort *appport)
{

	if (settings_window() == 2)
		start_stack();
	update_icon(appport);
}

static void
show_status(struct MsgPort *appport)
{
	struct EasyStruct es;
	LONG choice;
	int r;

	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (UBYTE *)"AmiBSDNet";
	es.es_TextFormat = (UBYTE *)"%s";

	if ((r = stack_cmd(NETCTRL_STATUS)) == -2) {
		es.es_GadgetFormat = (UBYTE *)"OK";
		EasyRequest(NULL, &es, NULL, (ULONG)
		    "AmiBSDNet does not answer (it seems to hang).\n\n"
		    "Its log is T:AmiBSDNet.log. Reboot to start it again.");
		return;
	}
	if (r != 0) {
		int other;

		/* a bsdsocket.library without AmiBSDNet: another stack runs */
		Forbid();
		other = FindName(&SysBase->LibList,
		    (CONST_STRPTR)"bsdsocket.library") != NULL;
		Permit();
		es.es_GadgetFormat = (UBYTE *)"Start AmiBSDNet|Settings...|Cancel";
		choice = EasyRequest(NULL, &es, NULL, (ULONG)(other ?
		    "The AmiBSDNet network stack is not running:\n"
		    "another TCP/IP stack is running.\n\n"
		    "Take it out of the boot (\"NetCtrl DISABLEOTHERS\" in a\n"
		    "Shell) and reboot." :
		    "The AmiBSDNet network stack is not running."));
		if (choice == 1)
			start_stack();
		else if (choice == 2) {
			show_settings(appport);
			return;
		}
	} else {
		struct NetCtrlIface wifi;
		int online = ctl->online, haswifi = 0;
		ULONG i;

		/* is there a wireless interface?  (fetch the text afterwards) */
		if (stack_cmd(NETCTRL_IFLIST) == 0)
			for (i = 0; i < ctl->nifaces; i++)
				/* (not PaulaNET: PaulaNET Config sets up its
				   network, not WirelessManager) */
				if ((ctl->ifaces[i].flags & NETIF_WIRELESS) &&
				    !amibsdnet_is_paulanet(ctl->ifaces[i].device)) {
					wifi = ctl->ifaces[i];
					haswifi = 1;
					break;
				}
		/* (no answer: ctl may still be the stack's - not shown) */
		if (stack_cmd(NETCTRL_STATUS) != 0)
			return;
		if (haswifi)
			es.es_GadgetFormat = online ?
			    (UBYTE *)"Go Offline|Reconnect|Wi-Fi...|Settings...|OK" :
			    (UBYTE *)"Go Online|Reconnect|Wi-Fi...|Settings...|OK";
		else
			es.es_GadgetFormat = online ?
			    (UBYTE *)"Go Offline|Reconnect|Settings...|OK" :
			    (UBYTE *)"Go Online|Reconnect|Settings...|OK";
		choice = EasyRequest(NULL, &es, NULL, (ULONG)ctl->text);
		if (choice == 1)
			stack_cmd(online ? NETCTRL_OFFLINE : NETCTRL_ONLINE);
		else if (choice == 2) {
			/* drop the address and fetch a fresh one */
			stack_cmd(NETCTRL_OFFLINE);
			stack_cmd(NETCTRL_ONLINE);
		} else if (choice == 3 && haswifi)
			wifi_window(&wifi);
		else if (choice == (haswifi ? 4 : 3)) {
			show_settings(appport);
			return;
		}
	}
	update_icon(appport);
}

/* ------------------------------------------------------------------------
 * setup from tool types (Workbench) or arguments (Shell)
 */

static void
read_options(struct WBStartup *wbmsg)
{
	if (wbmsg) {
		struct DiskObject *d;
		BPTR old;
		STRPTR v;

		if (IconBase == NULL || wbmsg->sm_NumArgs < 1)
			return;
		old = CurrentDir(wbmsg->sm_ArgList[0].wa_Lock);
		d = GetDiskObject((STRPTR)wbmsg->sm_ArgList[0].wa_Name);
		CurrentDir(old);
		if (d == NULL)
			return;
		/* (STACK= is how it was called before: only a command, not
		   a number - STACK is also the usual stack size tool type) */
		if ((v = FindToolType((STRPTR *)d->do_ToolTypes,
		    (STRPTR)"STACKCMD")) != NULL ||
		    ((v = FindToolType((STRPTR *)d->do_ToolTypes,
		    (STRPTR)"STACK")) != NULL && (*v < '0' || *v > '9')))
			copy(stackcmd, (const char *)v, sizeof(stackcmd));
		if ((v = FindToolType((STRPTR *)d->do_ToolTypes,
		    (STRPTR)"INTERVAL")) != NULL) {
			LONG n = 0;

			while (*v >= '0' && *v <= '9')
				n = n * 10 + (*v++ - '0');
			if (n > 0)
				interval = n;
		}
		FreeDiskObject(d);
	} else {
		struct RDArgs *rda;
		LONG arg[2] = { 0, 0 };

		if ((rda = ReadArgs((CONST_STRPTR)"STACKCMD=STACK/K,INTERVAL/K/N", arg,
		    NULL)) != NULL) {
			if (arg[0])
				copy(stackcmd, (const char *)arg[0],
				    sizeof(stackcmd));
			if (arg[1] && *(LONG *)arg[1] > 0)
				interval = *(LONG *)arg[1];
			FreeArgs(rda);
		}
	}
}

/* ------------------------------------------------------------------------ */

/* "AmiBSDNet..." in the Workbench Tools menu; like the icon, retried
   until Workbench is up */
static void
add_menu(struct MsgPort *appport)
{
	if (appmenu == NULL)
		appmenu = AddAppMenuItemA(MENU_STATUS, 0,
		    (UBYTE *)"AmiBSDNet...", appport, NULL);
	if (appmenu2 == NULL)
		appmenu2 = AddAppMenuItemA(MENU_SETTINGS, 0,
		    (UBYTE *)"AmiBSDNet Settings...", appport, NULL);
}

static int
run(void)
{
	struct MsgPort *appport = NULL, *cxport = NULL, *tport = NULL;
	struct timerequest *treq = NULL;
	struct NewBroker nb;
	CxObj *broker = NULL;
	ULONG sigs, appmask, cxmask = 0, tmask;
	struct DateStamp t0, now;
	int quit = 0, rc = RETURN_FAIL;

	if ((ctl = AllocVec(sizeof(*ctl), MEMF_PUBLIC | MEMF_CLEAR)) == NULL ||
	    make_image(0) != 0 || make_image(1) != 0 ||
	    (appport = CreateMsgPort()) == NULL ||
	    (tport = CreateMsgPort()) == NULL ||
	    (treq = (struct timerequest *)CreateIORequest(tport,
	    sizeof(*treq))) == NULL ||
	    OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
	    (struct IORequest *)treq, 0) != 0) {
		if (treq)
			DeleteIORequest((struct IORequest *)treq);
		treq = NULL;
		goto out;
	}

	if (CxBase && (cxport = CreateMsgPort()) != NULL) {
		step("adding the commodity");
		nb.nb_Version = NB_VERSION;
		nb.nb_Name = (STRPTR)"AmiBSDNet";
		nb.nb_Title = (STRPTR)"AmiBSDNet network status";
		nb.nb_Descr = (STRPTR)"Show / Enable = online / Disable = offline";
		nb.nb_Unique = NBU_UNIQUE | NBU_NOTIFY;
		nb.nb_Flags = COF_SHOW_HIDE;
		nb.nb_Pri = 0;
		nb.nb_Port = cxport;
		nb.nb_ReservedChannel = 0;
		if ((broker = CxBroker(&nb, NULL)) == NULL) {
			/* already running: that instance shows its window */
			DeleteMsgPort(cxport);
			cxport = NULL;
			rc = RETURN_OK;
			goto out;
		}
		ActivateCxObj(broker, TRUE);
		cxmask = 1UL << cxport->mp_SigBit;
	}

	/*
	 * Not the first instance any more (the broker is unique): the start
	 * guard.  Its own last start froze the Amiga: not this time.  And
	 * what else did not start this time is said here, on Workbench
	 * (nothing waits for a click during the boot).
	 */
	{
		char v[4], what[160];
		int froze = guard_begin("Status");

		what[0] = '\0';
		if (GetVar((CONST_STRPTR)"AmiBSDNet/Skipped-Stack", (STRPTR)v,
		    sizeof(v), GVF_GLOBAL_ONLY) >= 0)
			copy(what, "the network stack (AmiBSDNet)", sizeof(what));
		if (GetVar((CONST_STRPTR)"AmiBSDNet/Skipped-Serial", (STRPTR)v,
		    sizeof(v), GVF_GLOBAL_ONLY) >= 0)
			append(what, "the serial Shell (SerialShell)",
			    sizeof(what));
		if (froze)
			append(what, "the status icon (AmiBSDNetStatus)",
			    sizeof(what));
		if (what[0]) {
			/* (no marker while the requester waits for a click:
			   switching off then is not a frozen start) */
			if (!froze)
				guard_end("Status");
			guard_tell(what);
			if (!froze)
				guard_begin("Status");
		}
		DeleteVar((CONST_STRPTR)"AmiBSDNet/Skipped-Stack",
		    GVF_GLOBAL_ONLY);
		DeleteVar((CONST_STRPTR)"AmiBSDNet/Skipped-Serial",
		    GVF_GLOBAL_ONLY);
		if (froze) {
			rc = RETURN_OK;
			goto out;
		}
	}
	starting = 1;
	DateStamp(&t0);
	notice_open(&snotice, "AmiBSDNetStatus is starting (this window "
	    "closes by itself)", 2);
	step("starting");

	update_icon(appport);
	step("adding the menu items");
	add_menu(appport);
	step("running");
	appmask = 1UL << appport->mp_SigBit;
	tmask = 1UL << tport->mp_SigBit;
	treq->tr_node.io_Command = TR_ADDREQUEST;
	treq->tr_time.tv_secs = interval;
	treq->tr_time.tv_micro = 0;
	SendIO((struct IORequest *)treq);

	while (!quit) {
		sigs = Wait(appmask | cxmask | tmask | SIGBREAKF_CTRL_C);
		if (sigs & SIGBREAKF_CTRL_C)
			quit = 1;
		if (sigs & tmask) {
			WaitIO((struct IORequest *)treq);
			update_icon(appport);
			add_menu(appport);
			/* (by the clock: the interval may be long) */
			/* (a clock set back counts as over too) */
			DateStamp(&now);
			if (starting) {
				LONG el = now.ds_Minute * 60 + now.ds_Tick / 50 -
				    t0.ds_Minute * 60 - t0.ds_Tick / 50;

				if (now.ds_Days != t0.ds_Days || el < 0 ||
				    el >= START_SECS)
					start_over();
			}
			treq->tr_time.tv_secs = interval;
			treq->tr_time.tv_micro = 0;
			SendIO((struct IORequest *)treq);
		}
		if (sigs & appmask) {
			struct AppMessage *am;
			int open = 0;

			while ((am = (struct AppMessage *)GetMsg(appport))) {
				if (am->am_Type == AMTYPE_APPICON)
					open = 1;
				else if (am->am_Type == AMTYPE_APPMENUITEM)
					open = am->am_ID == MENU_SETTINGS ? 2 : 1;
				ReplyMsg((struct Message *)am);
			}
			/* (someone uses it: the start is over) */
			if (open)
				start_over();
			if (open == 1)
				show_status(appport);
			else if (open == 2)
				show_settings(appport);
		}
		if (cxport && (sigs & cxmask)) {
			CxMsg *cm;

			while ((cm = (CxMsg *)GetMsg(cxport))) {
				ULONG type = CxMsgType(cm), id = CxMsgID(cm);

				ReplyMsg((struct Message *)cm);
				if (type != CXM_COMMAND)
					continue;
				switch (id) {
				case CXCMD_DISABLE:
					stack_cmd(NETCTRL_OFFLINE);
					ActivateCxObj(broker, FALSE);
					break;
				case CXCMD_ENABLE:
					stack_cmd(NETCTRL_ONLINE);
					ActivateCxObj(broker, TRUE);
					break;
				case CXCMD_APPEAR:
				case CXCMD_UNIQUE:
					start_over();
					show_status(appport);
					break;
				case CXCMD_KILL:
					quit = 1;
					break;
				}
				update_icon(appport);
			}
		}
	}
	rc = RETURN_OK;
	AbortIO((struct IORequest *)treq);
	WaitIO((struct IORequest *)treq);

out:
	start_over();
	if (appmenu)
		RemoveAppMenuItem(appmenu);
	if (appmenu2)
		RemoveAppMenuItem(appmenu2);
	if (appicon)
		RemoveAppIcon(appicon);
	if (broker)
		DeleteCxObjAll(broker);
	if (cxport) {
		struct Message *m;

		while ((m = GetMsg(cxport)))
			ReplyMsg(m);
		DeleteMsgPort(cxport);
	}
	if (treq) {
		CloseDevice((struct IORequest *)treq);
		DeleteIORequest((struct IORequest *)treq);
	}
	if (tport)
		DeleteMsgPort(tport);
	if (appport) {
		struct Message *m;

		while ((m = GetMsg(appport)))
			ReplyMsg(m);
		DeleteMsgPort(appport);
	}
	if (chipimg[0])
		FreeVec(chipimg[0]);
	if (chipimg[1])
		FreeVec(chipimg[1]);
	/* (not while the stack may still answer into it) */
	if (ctl && (!ctl_left || amibsdnet_ctl_reclaim(ctl_left)))
		FreeVec(ctl);
	/* a Wi-Fi scan process still runs this program's code: unloading
	   now would crash it, so wait until the driver has answered */
	while (wifi_scan_busy())
		Delay(50);
	return rc;
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct Process *me;
	struct WBStartup *wbmsg = NULL;
	int rc = RETURN_FAIL;

	SysBase = *(struct ExecBase **)4;
	me = (struct Process *)SysBase->ThisTask;
	if (me->pr_CLI == 0) {
		WaitPort(&me->pr_MsgPort);
		wbmsg = (struct WBStartup *)GetMsg(&me->pr_MsgPort);
	}
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
	WorkbenchBase = OpenLibrary("workbench.library", 37);
	IconBase = OpenLibrary("icon.library", 37);
	CxBase = OpenLibrary("commodities.library", 37);
	if (DOSBase && IntuitionBase && WorkbenchBase) {
		read_options(wbmsg);
		rc = run();
	}
	if (CxBase) CloseLibrary(CxBase);
	if (GfxBase) CloseLibrary((struct Library *)GfxBase);
	if (IconBase) CloseLibrary(IconBase);
	if (WorkbenchBase) CloseLibrary(WorkbenchBase);
	if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
	if (DOSBase) CloseLibrary((struct Library *)DOSBase);
	if (wbmsg) {
		Forbid();
		ReplyMsg((struct Message *)wbmsg);
	}
	return rc;
}
