/*
 * BeginInterfaceConfig() from an ordinary task, and RemoveInterface() of
 * an interface whose DHCP client runs (src/lib/ifapi.c): sana0 of
 * tests/slirp-static.conf (fixed address) is removed (force: it has an
 * address), added again without one, given an address by DHCP through
 * BeginInterfaceConfig() (WinUAE's SLIRP has a DHCP server), and then
 * removed: refused without force, done with it; then NetCtrl RECONFIG
 * (DH0:NetCtrl, which run_emu.py --stack puts there) brings the
 * configured sana0 back.  RemoveInterface()
 * returns "TRUE for success, 0 for failure" (bsdsocket.doc).
 *
 *   tools/build_amiga.sh src/test/rmiftest.c build/rmiftest
 *   python -I tools/run_emu.py build/rmiftest --stack tests/slirp-static.conf
 *       --timeout 180
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <amibsdnet/net.h>

/* downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/netinclude/libraries/
   bsdsocket.h:883-964 */
struct AddressAllocationMessage {
	struct Message aam_Message;
	LONG aam_Reserved;
	LONG aam_Result;
	LONG aam_Version;
	LONG aam_Protocol;
	char aam_InterfaceName[16];
	LONG aam_Timeout;
	ULONG aam_LeaseTime;
	ULONG aam_RequestedAddress;
	STRPTR aam_ClientIdentifier;
	ULONG aam_Address;
	ULONG aam_ServerAddress;
	ULONG aam_SubnetMask;
	STRPTR aam_NAKMessage;
	LONG aam_NAKMessageSize;
	ULONG *aam_RouterTable;
	LONG aam_RouterTableSize;
	ULONG *aam_DNSTable;
	LONG aam_DNSTableSize;
	ULONG *aam_StaticRouteTable;
	LONG aam_StaticRouteTableSize;
	STRPTR aam_HostName;
	LONG aam_HostNameSize;
	STRPTR aam_DomainName;
	LONG aam_DomainNameSize;
	UBYTE *aam_BOOTPMessage;
	LONG aam_BOOTPMessageSize;
	struct DateStamp *aam_LeaseExpires;
	BOOL aam_Unicast;
};

#include <bsdsocket_inline.h>

/* the same header: AAM_VERSION 2 (967), AAMR_Success 0 (973), AAMP_DHCP 1
   (1008); IFQ_LastStart (TAG_USER + 1900 + 13); errno values sys/errno.h */
#define	AAM_VERSION	2
#define	AAMR_Success	0
#define	AAMP_DHCP	1
#define	IFQ_LastStart	(TAG_USER + 1900 + 13)
#define	EBUSY		16

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *SocketBase;

void	*memset(void *, int, unsigned long);

static LONG errno_;
static int failures;

static void
say(const char *s)
{

	PutStr((CONST_STRPTR)s);
}

static void
sayn(LONG n)
{
	char b[12];
	int i = 11, neg = n < 0;
	ULONG u = neg ? -n : n;

	b[i] = '\0';
	do {
		b[--i] = '0' + u % 10;
		u /= 10;
	} while (u);
	if (neg)
		b[--i] = '-';
	say(b + i);
}

static void
check(const char *what, int ok)
{

	say(ok ? "ok   " : "FAIL ");
	say(what);
	if (!ok) {
		say(" (errno ");
		sayn(errno_);
		say(")");
		failures++;
	}
	say("\n");
}

static int
known(const char *name)
{
	struct __timeval tv;
	struct TagItem t[2];

	t[0].ti_Tag = IFQ_LastStart;
	t[0].ti_Data = (ULONG)&tv;
	t[1].ti_Tag = TAG_END;
	return QueryInterfaceTagList((STRPTR)name, t) == 0;
}

static void
test_rmif(void)
{
	struct AddressAllocationMessage m;
	struct MsgPort *port;
	int i;

	check("sana0 is there", known("sana0"));
	check("RemoveInterface(sana0, TRUE) (fixed address): TRUE",
	    RemoveInterface((STRPTR)"sana0", TRUE) != 0);
	check("... gone", !known("sana0"));
	check("AddInterfaceTagList(sana0, uaenet.device, 0)",
	    AddInterfaceTagList((STRPTR)"sana0", (STRPTR)"uaenet.device", 0,
	    NULL) == 0);
	if ((port = CreateMsgPort()) == NULL) {
		check("CreateMsgPort()", 0);
		return;
	}
	memset(&m, 0, sizeof(m));
	m.aam_Message.mn_Node.ln_Type = NT_MESSAGE;
	m.aam_Message.mn_ReplyPort = port;
	m.aam_Message.mn_Length = sizeof(m);
	m.aam_Version = AAM_VERSION;
	m.aam_Protocol = AAMP_DHCP;
	m.aam_InterfaceName[0] = 's';
	m.aam_InterfaceName[1] = 'a';
	m.aam_InterfaceName[2] = 'n';
	m.aam_InterfaceName[3] = 'a';
	m.aam_InterfaceName[4] = '0';
	m.aam_Timeout = 60;
	m.aam_Result = -100;
	BeginInterfaceConfig(&m);
	WaitPort(port);
	GetMsg(port);
	DeleteMsgPort(port);
	say("     aam_Result ");
	sayn(m.aam_Result);
	say(", aam_Address ");
	sayn((LONG)(m.aam_Address >> 24));
	say(".");
	sayn((LONG)((m.aam_Address >> 16) & 255));
	say(".");
	sayn((LONG)((m.aam_Address >> 8) & 255));
	say(".");
	sayn((LONG)(m.aam_Address & 255));
	say("\n");
	check("BeginInterfaceConfig(sana0, DHCP): AAMR_Success, 10.0.2.x",
	    m.aam_Result == AAMR_Success &&
	    (m.aam_Address & 0xffffff00UL) == 0x0a000200UL);
	errno_ = 0;
	check("RemoveInterface(sana0, FALSE), DHCP client running: 0, EBUSY",
	    RemoveInterface((STRPTR)"sana0", FALSE) == 0 && errno_ == EBUSY);
	check("RemoveInterface(sana0, TRUE): the client stopped, TRUE",
	    RemoveInterface((STRPTR)"sana0", TRUE) != 0);
	check("... gone", !known("sana0"));

	/* the stack's reconfiguration (it takes the interface table lock,
	   src/stack/config.c stack_reconfigure_poll()): the configuration
	   file, DH0:AmiBSDNet.conf (tools/run_emu.py --stack), has sana0
	   again */
	check("NetCtrl RECONFIG returns 0", SystemTagList(
	    (CONST_STRPTR)"DH0:NetCtrl RECONFIG", NULL) == 0);
	for (i = 0; i < 250 && !known("sana0"); i++)
		Delay(1);
	check("... sana0 is back", known("sana0"));
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	BPTR f;
	int i;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	if (DOSBase == NULL)
		return RETURN_FAIL;
	for (i = 0; i < 300 && SocketBase == NULL; i++) {
		SocketBase = OpenLibrary("bsdsocket.library", 4);
		if (SocketBase == NULL)
			Delay(10);
	}
	check("OpenLibrary(\"bsdsocket.library\", 4)", SocketBase != NULL);
	if (SocketBase) {
		SetErrnoPtr(&errno_, sizeof(errno_));
		test_rmif();
		CloseLibrary(SocketBase);
	}
	say(failures ? "rmiftest: FAILED\n" : "rmiftest: PASS\n");
	if ((f = Open((CONST_STRPTR)"DH0:done", MODE_NEWFILE))) {
		Write(f, failures ? "FAIL\n" : "PASS\n", 5);
		Close(f);
	}
	CloseLibrary((struct Library *)DOSBase);
	return failures ? RETURN_ERROR : RETURN_OK;
}

void *
memset(void *d, int c, unsigned long n)
{
	char *p = d;

	while (n--)
		*p++ = c;
	return d;
}
