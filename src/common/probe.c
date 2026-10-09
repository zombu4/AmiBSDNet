/*
 * Network adapter detection, shared by NetCtrl PROBE (used by the
 * installer) and the settings window of the status tool.
 *
 * Every driver in DEVS:Networks is opened (unit 0) and asked with
 * NSCMD_DEVICEQUERY which commands it supports: a driver that can scan
 * for networks (S2_GETNETWORKS, SANA-II revision 6) is Wi-Fi, any other
 * is Ethernet.  A driver the running stack already uses is not opened
 * a second time: the stack's interface list says what it is.  Drivers
 * without a file (built into ROM, or loaded from
 * elsewhere) are looked for in the system's device list by name.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "../host/sana2.h"
#include <amibsdnet/probe.h>
#include <amibsdnet/control.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

/* SANA-II drivers that may be resident instead of in DEVS:Networks */
static const char *const resident_names[] = {
	"uaenet.device",	/* WinUAE */
	"wifipi.device",	/* Emu68 / PiStorm Wi-Fi */
	"genet.device",		/* Emu68 / PiStorm Ethernet (Pi 4) */
	NULL
};

/* buffer hooks: never called, but drivers want them at open */
static ULONG nocopy(void) { return 0; }
static struct TagItem bufftags[] = {
	{ S2_CopyToBuff, (ULONG)nocopy },
	{ S2_CopyFromBuff, (ULONG)nocopy },
	{ TAG_DONE, 0 }
};

static int
lower(int c)
{

	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int
eq_nocase(const char *a, const char *b)
{

	while (*a && lower((UBYTE)*a) == lower((UBYTE)*b))
		a++, b++;
	return *a == *b;
}

static int
contains(const char *s, const char *w)
{
	int i;

	for (; *s; s++) {
		for (i = 0; w[i] && lower((UBYTE)s[i]) == w[i]; i++)
			;
		if (!w[i])
			return 1;
	}
	return 0;
}

static int
ends_with(const char *s, const char *suffix)
{
	int ls = 0, lx = 0;

	while (s[ls])
		ls++;
	while (suffix[lx])
		lx++;
	return ls >= lx && eq_nocase(s + ls - lx, suffix);
}

static void
copy(char *d, const char *s, int n)
{

	while (--n > 0 && *s)
		*d++ = *s++;
	*d = '\0';
}

/* open the driver; 1 = Wi-Fi, 0 = Ethernet, -1 = not a usable driver */
static int
probe_one(const char *name, ULONG unit)
{
	struct MsgPort *port;
	struct IOSana2Req *req;
	struct NSDeviceQueryResult nsq;
	int kind = -1;

	if ((port = CreateMsgPort()) == NULL)
		return -1;
	req = (struct IOSana2Req *)CreateIORequest(port, sizeof(*req));
	if (req == NULL) {
		DeleteMsgPort(port);
		return -1;
	}
	req->ios2_BufferManagement = bufftags;
	if (OpenDevice((CONST_STRPTR)name, unit, (struct IORequest *)req,
	    0) == 0) {
		struct IOStdReq *io = (struct IOStdReq *)req;
		UWORD *cmd;

		kind = 0;
		nsq.DevQueryFormat = 0;
		nsq.SizeAvailable = 0;
		nsq.SupportedCommands = NULL;
		io->io_Command = NSCMD_DEVICEQUERY;
		io->io_Data = &nsq;
		io->io_Length = sizeof(nsq);
		if (DoIO((struct IORequest *)req) == 0 && nsq.SupportedCommands)
			for (cmd = nsq.SupportedCommands; *cmd; cmd++)
				if (*cmd == S2_GETNETWORKS)
					kind = 1;
		/* older Wi-Fi drivers without the query: go by the name */
		if (kind == 0 && (contains(name, "wifi") ||
		    contains(name, "wlan") || contains(name, "prism") ||
		    contains(name, "wireless")))
			kind = 1;
		CloseDevice((struct IORequest *)req);
	}
	DeleteIORequest((struct IORequest *)req);
	DeleteMsgPort(port);
	return kind;
}

/* the drivers the running stack has open (NETCTRL_IFLIST) */
static struct NetCtrlMsg *stackinfo;

static void
ask_stack(void)
{
	struct MsgPort *reply, *port;

	if ((stackinfo = AllocVec(sizeof(*stackinfo), MEMF_ANY | MEMF_CLEAR))
	    == NULL)
		return;
	if ((reply = CreateMsgPort()) == NULL)
		goto fail;
	stackinfo->msg.mn_Node.ln_Type = NT_MESSAGE;
	stackinfo->msg.mn_ReplyPort = reply;
	stackinfo->msg.mn_Length = sizeof(*stackinfo);
	stackinfo->cmd = NETCTRL_IFLIST;
	Forbid();
	if ((port = FindPort((CONST_STRPTR)AMIBSDNET_PORTNAME)) != NULL)
		PutMsg(port, &stackinfo->msg);
	Permit();
	if (port) {
		WaitPort(reply);
		GetMsg(reply);
	}
	DeleteMsgPort(reply);
	if (port && stackinfo->result == 0)
		return;
fail:
	FreeVec(stackinfo);
	stackinfo = NULL;
}

static int
stack_kind(const char *name)
{
	ULONG i;

	if (stackinfo)
		for (i = 0; i < stackinfo->nifaces && i < NETCTRL_MAXIFACES; i++)
			if (eq_nocase(stackinfo->ifaces[i].device, name))
				return (stackinfo->ifaces[i].flags & NETIF_WIRELESS) ?
				    1 : 0;
	return -1;
}

static int
add(struct probe_adapter *a, int n, int max, const char *name)
{
	int i, kind;

	for (i = 0; i < n; i++)
		if (eq_nocase(a[i].device, name))
			return n;
	if (n >= max)
		return n;
	if ((kind = stack_kind(name)) < 0 && (kind = probe_one(name, 0)) < 0)
		return n;
	copy(a[n].device, name, sizeof(a[n].device));
	a[n].unit = 0;
	a[n].wireless = kind;
	return n + 1;
}

int
probe_adapters(struct probe_adapter *a, int max)
{
	struct FileInfoBlock *fib;
	struct probe_adapter t;
	char names[PROBE_MAX + 4][64];
	int n = 0, nnames = 0, i, j;
	BPTR lock;

	/* collect the names first: opening a driver may load others */
	if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
		if ((lock = Lock((CONST_STRPTR)"DEVS:Networks", ACCESS_READ))) {
			if (Examine(lock, fib))
				while (ExNext(lock, fib) && nnames < PROBE_MAX)
					if (fib->fib_DirEntryType < 0 &&
					    ends_with((const char *)fib->fib_FileName,
					    ".device"))
						copy(names[nnames++],
						    (const char *)fib->fib_FileName, 64);
			UnLock(lock);
		}
		FreeDosObject(DOS_FIB, fib);
	}
	Forbid();
	for (i = 0; resident_names[i]; i++)
		if (FindName(&SysBase->DeviceList, (CONST_STRPTR)resident_names[i]))
			copy(names[nnames++], resident_names[i], 64);
	Permit();

	ask_stack();
	for (i = 0; i < nnames; i++)
		n = add(a, n, max, names[i]);
	if (stackinfo) {
		FreeVec(stackinfo);
		stackinfo = NULL;
	}

	/* Ethernet first: it is the preferred route when both are up */
	for (i = 0; i < n; i++)
		for (j = 0; j + 1 < n - i; j++)
			if (a[j].wireless > a[j + 1].wireless) {
				CopyMem(&a[j], &t, sizeof(t));
				CopyMem(&a[j + 1], &a[j], sizeof(t));
				CopyMem(&t, &a[j + 1], sizeof(t));
			}
	return n;
}
