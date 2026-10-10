/*
 * NetCtrl: control the AmiBSDNet stack from the Shell.
 *
 *   NetCtrl [STATUS|ONLINE|OFFLINE|RECONFIG|WAIT|PROBE|CHECK|
 *           DISABLEOTHERS|REMOVEOTHERS|RESTOREOTHERS] [TIMEOUT=<seconds>]
 *
 * WAIT returns when the network is up (or fails after TIMEOUT seconds,
 * default 30), for scripts that need the network.
 *
 * PROBE lists the network adapters (SANA-II drivers) and whether each is
 * Ethernet or Wi-Fi, and sets the variables AmiBSDNet/Ethernet and
 * AmiBSDNet/WiFi to the first driver of each kind (the installer reads
 * them); it does not need the stack.
 *
 * CHECK looks for other TCP/IP stacks and an Emu68 system (variables
 * AmiBSDNet/OtherStacks, AmiBSDNet/OthersAtBoot, AmiBSDNet/OtherRunning,
 * AmiBSDNet/Emu68);
 * DISABLEOTHERS, REMOVEOTHERS and RESTOREOTHERS take other stacks out of
 * the boot (reversibly), remove them from it, or put them back
 * (src/tools/otherstacks.c).
 *
 * CHECKDRIVER [FILE=<driver>] checks a PaulaNET.device file
 * (src/common/drvcheck.c); KEEPCONF FILE=<out> copies the PaulaNET
 * lines of the saved configuration (for the installer).
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <intuition/intuition.h>
#include <proto/intuition.h>

#include <dos/var.h>

#include <amibsdnet/control.h>
#include <amibsdnet/probe.h>
#include <amibsdnet/drvcheck.h>

#include "otherstacks.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

static const char verstag[] __attribute__((used)) =
    "\0$VER: NetCtrl 0.6 (10.10.2026)";

static int
streq(const char *a, const char *b)
{
	int ca, cb;

	do {
		ca = (UBYTE)*a++;
		cb = (UBYTE)*b++;
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
	} while (ca && ca == cb);
	return ca == cb;
}

static void
setvar(const char *name, const char *value)
{

	if (value) {
		/* on a first install ENV:AmiBSDNet does not exist yet */
		BPTR l = CreateDir((CONST_STRPTR)"ENV:AmiBSDNet");

		if (l)
			UnLock(l);
		SetVar((CONST_STRPTR)name, (CONST_STRPTR)value, -1,
		    GVF_GLOBAL_ONLY);
	} else
		DeleteVar((CONST_STRPTR)name, GVF_GLOBAL_ONLY);
}

static int
probe(void)
{
	struct probe_adapter a[PROBE_MAX];
	const char *eth = NULL, *wifi = NULL;
	int n, i;

	n = probe_adapters(a, PROBE_MAX);
	for (i = 0; i < n; i++) {
		PutStr((CONST_STRPTR)(a[i].wireless ? "Wi-Fi     " :
		    "Ethernet  "));
		PutStr((CONST_STRPTR)a[i].device);
		PutStr((CONST_STRPTR)"\n");
		if (a[i].wireless && !wifi)
			wifi = a[i].device;
		if (!a[i].wireless && !eth)
			eth = a[i].device;
	}
	if (probe_paulanet[0]) {
		PutStr((CONST_STRPTR)"PaulaNET  ");
		PutStr((CONST_STRPTR)probe_paulanet);
		PutStr((CONST_STRPTR)" (Wi-Fi through the floppy port; used "
		    "when plugged in)\n");
	}
	if (n == 0 && !probe_paulanet[0])
		PutStr((CONST_STRPTR)"no network adapters found\n");
	setvar("AmiBSDNet/Ethernet", eth);
	setvar("AmiBSDNet/WiFi", wifi);
	setvar("AmiBSDNet/PaulaNET", probe_paulanet[0] ? probe_paulanet : NULL);
	return n || probe_paulanet[0] ? RETURN_OK : RETURN_WARN;
}

/*
 * CHECKDRIVER [FILE=<path>]: checks a PaulaNET.device (default: the one
 * that would be loaded) against the official builds and the accepted
 * CRCs.  Sets AmiBSDNet/DriverCheck (OK, ACCEPTED, UNKNOWN, DAMAGED or
 * MISSING), AmiBSDNet/DriverCRC and AmiBSDNet/DriverVersion.  Returns
 * WARN if the stack would not use the file.
 */
static int
checkdriver(const char *file)
{
	static const char *const what[] = { "OK", "ACCEPTED", "UNKNOWN",
	    "DAMAGED", "MISSING" };
	struct drvprefs prefs;
	char path[256], hex[9];
	const char *ver;
	ULONG crc;
	int r, i;

	drv_read_prefs(&prefs);
	if (file) {
		for (i = 0; file[i] && i < (int)sizeof(path) - 1; i++)
			path[i] = file[i];
		path[i] = '\0';
	} else if (!drv_paulanet_path(path, sizeof(path))) {
		PutStr((CONST_STRPTR)"no " PAULANET_NAME " found\n");
		setvar("AmiBSDNet/DriverCheck", "MISSING");
		setvar("AmiBSDNet/DriverCRC", NULL);
		setvar("AmiBSDNet/DriverVersion", NULL);
		return RETURN_WARN;
	}
	r = drv_check_paulanet(path, &prefs, &ver, &crc);
	drv_fmt_crc(hex, crc);
	PutStr((CONST_STRPTR)path);
	PutStr((CONST_STRPTR)": ");
	switch (r) {
	case DRV_OK:
		PutStr((CONST_STRPTR)"official PaulaNET ");
		PutStr((CONST_STRPTR)ver);
		break;
	case DRV_ACCEPTED:
		PutStr((CONST_STRPTR)"accepted (CRC ");
		PutStr((CONST_STRPTR)hex);
		PutStr((CONST_STRPTR)")");
		break;
	case DRV_UNKNOWN:
		PutStr((CONST_STRPTR)"not a version AmiBSDNet knows (CRC ");
		PutStr((CONST_STRPTR)hex);
		PutStr((CONST_STRPTR)(prefs.verify ? "); not used while "
		    "verifying is on" : "); used, verifying is off"));
		break;
	case DRV_DAMAGED:
		PutStr((CONST_STRPTR)"DAMAGED (cannot be read or loaded)");
		break;
	default:
		PutStr((CONST_STRPTR)"not found");
		break;
	}
	PutStr((CONST_STRPTR)"\n");
	setvar("AmiBSDNet/DriverCheck", what[r]);
	setvar("AmiBSDNet/DriverCRC", r <= DRV_UNKNOWN ? hex : NULL);
	setvar("AmiBSDNet/DriverVersion", ver);
	return DRV_USABLE(&prefs, r) ? RETURN_OK : RETURN_WARN;
}

/*
 * KEEPCONF FILE=<out>: the PaulaNET lines ("paulanet ...", "autodetect
 * ...") of ENVARC:AmiBSDNet/AmiBSDNet.conf, for the installer, which
 * writes a new configuration but keeps what was set in the Settings
 * window.  <out> is always written (empty if there is nothing to keep).
 * NOAUTODETECT leaves out "autodetect" (for a configuration that relies
 * on PaulaNET alone: "autodetect off" would leave it without a network).
 */
static int
keepconf(const char *out, int noauto)
{
	char line[200];
	BPTR in, fh;
	int i, j;

	if ((fh = Open((CONST_STRPTR)out, MODE_NEWFILE)) == 0) {
		PrintFault(IoErr(), (CONST_STRPTR)"NetCtrl");
		return RETURN_ERROR;
	}
	if ((in = Open((CONST_STRPTR)"ENVARC:AmiBSDNet/AmiBSDNet.conf",
	    MODE_OLDFILE)) != 0) {
		while (FGets(in, (STRPTR)line, sizeof(line) - 1)) {
			for (i = 0; line[i] == ' ' || line[i] == '\t'; i++)
				;
			for (j = 0; line[i + j] && line[i + j] != ' ' &&
			    line[i + j] != '\t' && line[i + j] != '\n'; j++)
				;
			if (j == 0)
				continue;
			{
				char c = line[i + j];
				int keep;

				line[i + j] = '\0';
				keep = streq(line + i, "paulanet") ||
				    (!noauto && streq(line + i, "autodetect"));
				line[i + j] = c;
				if (!keep)
					continue;
			}
			for (j = 0; line[j]; j++)
				;
			if (j == 0 || line[j - 1] != '\n') {
				line[j++] = '\n';
				line[j] = '\0';
			}
			FPuts(fh, (CONST_STRPTR)(line + i));
		}
		Close(in);
	}
	Close(fh);
	return RETURN_OK;
}

/* local commands (not sent to the stack) */
#define	CMD_PROBE	0xffff0001UL
#define	CMD_CHECK	0xffff0002UL
#define	CMD_DISABLE	0xffff0003UL
#define	CMD_REMOVE	0xffff0004UL
#define	CMD_RESTORE	0xffff0005UL
#define	CMD_FALLBACK	0xffff0006UL
#define	CMD_CHECKDRV	0xffff0007UL
#define	CMD_KEEPCONF	0xffff0008UL

/*
 * FALLBACK: back to the previous TCP/IP stack (run by AmiBSDNet when a
 * trial switch did not connect, or by hand).  Says so in a requester as
 * well, since at boot there is no Shell to read it in.
 */
static int
fallback(void)
{
	struct Library *IntuitionBase;
	struct EasyStruct es;
	char msg[400];
	int rv = otherstacks_fallback(msg, sizeof(msg));

	PutStr((CONST_STRPTR)msg);
	PutStr((CONST_STRPTR)"\n");
	if ((IntuitionBase = OpenLibrary((CONST_STRPTR)"intuition.library",
	    37)) != NULL) {
		es.es_StructSize = sizeof(es);
		es.es_Flags = 0;
		es.es_Title = (UBYTE *)"AmiBSDNet";
		es.es_TextFormat = (UBYTE *)"%s";
		es.es_GadgetFormat = (UBYTE *)"OK";
		EasyRequestArgs(NULL, &es, NULL, (APTR)&(char *){ msg });
		CloseLibrary(IntuitionBase);
	}
	return rv == 0 ? RETURN_OK : RETURN_WARN;
}

/* after otherstacks_check(): AmiBSDNet/OthersAtBoot; returns how many */
static int
report_atboot(void)
{
	char names[128];
	int n = otherstacks_atboot(names, sizeof(names));

	if (n) {
		PutStr((CONST_STRPTR)"still started at boot: ");
		PutStr((CONST_STRPTR)names);
		PutStr((CONST_STRPTR)"\n");
	}
	setvar("AmiBSDNet/OthersAtBoot", n ? names : NULL);
	return n;
}

/* DISABLEOTHERS / REMOVEOTHERS / RESTOREOTHERS, then look again */
static int
others(int mode)
{
	char names[128];
	int rv, n;

	/* never neither: the other stack goes only while AmiBSDNet is in
	   the boot (after a FALLBACK it is not) */
	if (mode != OTHERS_RESTORE && !otherstacks_self_atboot()) {
		PutStr((CONST_STRPTR)"NetCtrl: AmiBSDNet is not started at boot "
		    "(S:User-Startup); not changed.\nRun the AmiBSDNet installer "
		    "to switch.\n");
		return RETURN_WARN;
	}
	/* removing for good during a trial would leave no way back */
	if (mode == OTHERS_REMOVE && GetVar((CONST_STRPTR)"AmiBSDNet/Trial",
	    (STRPTR)names, sizeof(names), GVF_GLOBAL_ONLY) >= 0) {
		PutStr((CONST_STRPTR)"NetCtrl: the switch to AmiBSDNet is still "
		    "a trial (until it has connected\nonce); not removed.\n");
		return RETURN_WARN;
	}
	rv = otherstacks_apply(mode);
	otherstacks_check(names, sizeof(names));
	n = report_atboot();
	if (mode != OTHERS_RESTORE && n) {
		PutStr((CONST_STRPTR)"NetCtrl: not everything could be taken "
		    "out of the boot\n");
		return RETURN_WARN;
	}
	if (rv != 0) {
		PutStr((CONST_STRPTR)"NetCtrl: some files could not be "
		    "changed\n");
		return RETURN_WARN;
	}
	PutStr((CONST_STRPTR)(mode == OTHERS_RESTORE ? "restored\n" :
	    "no other TCP/IP stack is started at boot any more\n"));
	return RETURN_OK;
}

/*
 * CHECK, for the installer: other TCP/IP stacks (AmiBSDNet/OtherStacks,
 * AmiBSDNet/OtherRunning if one is running now) and whether this is an
 * Emu68 (PiStorm) system (AmiBSDNet/Emu68).
 */
/* CHECK lists what it found, so a report shows what was matched */
static void
say_match(const char *where, const char *what, int len)
{
	char line[200];
	int i;

	if (len < 0)
		for (len = 0; what[len]; len++)
			;
	for (i = 0; i < len && i < (int)sizeof(line) - 1 && what[i] != '\r';
	    i++)
		line[i] = what[i];
	line[i] = '\0';
	PutStr((CONST_STRPTR)"  ");
	PutStr((CONST_STRPTR)where);
	PutStr((CONST_STRPTR)": ");
	PutStr((CONST_STRPTR)line);
	PutStr((CONST_STRPTR)"\n");
}

static int
check(void)
{
	char names[128];
	int running;

	otherstacks_say = say_match;
	if (otherstacks_check(names, sizeof(names))) {
		PutStr((CONST_STRPTR)"other TCP/IP stacks: ");
		PutStr((CONST_STRPTR)names);
		PutStr((CONST_STRPTR)"\n");
		setvar("AmiBSDNet/OtherStacks", names);
	} else
		setvar("AmiBSDNet/OtherStacks", NULL);
	report_atboot();
	setvar("AmiBSDNet/SelfAtBoot", otherstacks_self_atboot() ? "1" : NULL);
	/* a bsdsocket.library in memory that is not AmiBSDNet's */
	Forbid();
	running = FindName(&SysBase->LibList, (CONST_STRPTR)"bsdsocket.library")
	    != NULL && FindPort((CONST_STRPTR)AMIBSDNET_PORTNAME) == NULL;
	Permit();
	if (running)
		PutStr((CONST_STRPTR)"another TCP/IP stack is running now\n");
	setvar("AmiBSDNet/OtherRunning", running ? "1" : NULL);
	if (OpenResource((CONST_STRPTR)"devicetree.resource")) {
		PutStr((CONST_STRPTR)"Emu68 (PiStorm) system\n");
		setvar("AmiBSDNet/Emu68", "1");
	} else
		setvar("AmiBSDNet/Emu68", NULL);
	return RETURN_OK;
}

/* send a command; returns 0 and fills *m, or -1 if the stack is not running */
int
netctrl_send(ULONG cmd, struct NetCtrlMsg *m)
{
	struct MsgPort *reply, *port;

	if ((reply = CreateMsgPort()) == NULL)
		return -1;
	m->msg.mn_Node.ln_Type = NT_MESSAGE;
	m->msg.mn_ReplyPort = reply;
	m->msg.mn_Length = sizeof(*m);
	m->cmd = cmd;
	m->text[0] = '\0';
	Forbid();
	if ((port = FindPort((CONST_STRPTR)AMIBSDNET_PORTNAME)) != NULL)
		PutMsg(port, &m->msg);
	Permit();
	if (port) {
		WaitPort(reply);
		GetMsg(reply);
	}
	DeleteMsgPort(reply);
	return port ? 0 : -1;
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct NetCtrlMsg *m;
	struct RDArgs *rda;
	LONG arg[4] = { 0, 0, 0, 0 };
	char file[256];
	int noauto;
	LONG timeout = 30;
	ULONG cmd = NETCTRL_STATUS;
	int rc = RETURN_OK;

	SysBase = *(struct ExecBase **)4;
	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37)) == NULL)
		return RETURN_FAIL;
	if ((rda = ReadArgs((CONST_STRPTR)"COMMAND,TIMEOUT/K/N,FILE/K,NOAUTODETECT/S", arg, NULL)) ==
	    NULL) {
		PrintFault(IoErr(), (CONST_STRPTR)"NetCtrl");
		rc = RETURN_ERROR;
		goto out;
	}
	if (arg[0]) {
		const char *c = (const char *)arg[0];

		if (streq(c, "STATUS")) cmd = NETCTRL_STATUS;
		else if (streq(c, "ONLINE")) cmd = NETCTRL_ONLINE;
		else if (streq(c, "OFFLINE")) cmd = NETCTRL_OFFLINE;
		else if (streq(c, "RECONFIG")) cmd = NETCTRL_RECONFIG;
		else if (streq(c, "WAIT")) cmd = 0;
		else if (streq(c, "PROBE")) cmd = CMD_PROBE;
		else if (streq(c, "CHECK")) cmd = CMD_CHECK;
		else if (streq(c, "DISABLEOTHERS")) cmd = CMD_DISABLE;
		else if (streq(c, "REMOVEOTHERS")) cmd = CMD_REMOVE;
		else if (streq(c, "RESTOREOTHERS")) cmd = CMD_RESTORE;
		else if (streq(c, "FALLBACK")) cmd = CMD_FALLBACK;
		else if (streq(c, "CHECKDRIVER")) cmd = CMD_CHECKDRV;
		else if (streq(c, "KEEPCONF")) cmd = CMD_KEEPCONF;
		else {
			PutStr((CONST_STRPTR)"usage: NetCtrl "
			    "[STATUS|ONLINE|OFFLINE|RECONFIG|WAIT|PROBE|CHECK|\n"
			    "    DISABLEOTHERS|REMOVEOTHERS|RESTOREOTHERS|FALLBACK|\n"
			    "    CHECKDRIVER] [TIMEOUT=<seconds>] [FILE=<driver>]\n");
			rc = RETURN_ERROR;
		}
	}
	if (arg[1])
		timeout = *(LONG *)arg[1];
	noauto = arg[3] != 0;
	file[0] = '\0';
	if (arg[2]) {
		const char *f = (const char *)arg[2];
		int i;

		for (i = 0; f[i] && i < (int)sizeof(file) - 1; i++)
			file[i] = f[i];
		file[i] = '\0';
	}
	FreeArgs(rda);
	if (rc != RETURN_OK)
		goto out;
	switch (cmd) {
	case CMD_PROBE:
		rc = probe();
		goto out;
	case CMD_CHECK:
		rc = check();
		goto out;
	case CMD_DISABLE:
	case CMD_REMOVE:
	case CMD_RESTORE:
		rc = others(cmd == CMD_DISABLE ? OTHERS_DISABLE :
		    cmd == CMD_REMOVE ? OTHERS_REMOVE : OTHERS_RESTORE);
		goto out;
	case CMD_FALLBACK:
		rc = fallback();
		goto out;
	case CMD_CHECKDRV:
		rc = checkdriver(file[0] ? file : NULL);
		goto out;
	case CMD_KEEPCONF:
		if (!file[0]) {
			PutStr((CONST_STRPTR)"NetCtrl: KEEPCONF needs FILE=\n");
			rc = RETURN_ERROR;
		} else
			rc = keepconf(file, noauto);
		goto out;
	}

	if ((m = AllocVec(sizeof(*m), MEMF_ANY | MEMF_CLEAR)) == NULL) {
		rc = RETURN_FAIL;
		goto out;
	}
	if (cmd == 0) {
		/* WAIT: poll until online (the stack may still be starting) */
		LONG ticks;

		for (ticks = 0; ticks <= timeout * 50; ticks += 25) {
			if (netctrl_send(NETCTRL_STATE, m) == 0 && m->online)
				break;
			if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
				break;
			Delay(25);
		}
		if (!m->online) {
			PutStr((CONST_STRPTR)"NetCtrl: network not up\n");
			rc = RETURN_WARN;
		}
	} else if (netctrl_send(cmd, m) != 0) {
		PutStr((CONST_STRPTR)"NetCtrl: AmiBSDNet is not running\n");
		rc = RETURN_WARN;
	} else if (cmd == NETCTRL_STATUS) {
		PutStr((CONST_STRPTR)m->text);
	} else {
		PutStr((CONST_STRPTR)(m->result == 0 ? "ok" : "failed"));
		PutStr((CONST_STRPTR)(m->online ? " (online)\n" : " (offline)\n"));
		if (m->result)
			rc = RETURN_ERROR;
	}
	FreeVec(m);
out:
	CloseLibrary((struct Library *)DOSBase);
	return rc;
}
