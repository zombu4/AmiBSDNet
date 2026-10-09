/*
 * NetCtrl: control the AmiBSDNet stack from the Shell.
 *
 *   NetCtrl [STATUS|ONLINE|OFFLINE|RECONFIG|WAIT|PROBE] [TIMEOUT=<seconds>]
 *
 * WAIT returns when the network is up (or fails after TIMEOUT seconds,
 * default 30), for scripts that need the network.
 *
 * PROBE lists the network adapters (SANA-II drivers) and whether each is
 * Ethernet or Wi-Fi, and sets the variables AmiBSDNet/Ethernet and
 * AmiBSDNet/WiFi to the first driver of each kind (the installer reads
 * them); it does not need the stack.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <dos/var.h>

#include <amibsdnet/control.h>
#include <amibsdnet/probe.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

static const char verstag[] __attribute__((used)) =
    "\0$VER: NetCtrl 0.1 (09.10.2026)";

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

	if (value)
		SetVar((CONST_STRPTR)name, (CONST_STRPTR)value, -1,
		    GVF_GLOBAL_ONLY);
	else
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
	if (n == 0)
		PutStr((CONST_STRPTR)"no network adapters found\n");
	setvar("AmiBSDNet/Ethernet", eth);
	setvar("AmiBSDNet/WiFi", wifi);
	return n ? RETURN_OK : RETURN_WARN;
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
	LONG arg[2] = { 0, 0 };
	LONG timeout = 30;
	ULONG cmd = NETCTRL_STATUS;
	int rc = RETURN_OK;

	SysBase = *(struct ExecBase **)4;
	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37)) == NULL)
		return RETURN_FAIL;
	if ((rda = ReadArgs((CONST_STRPTR)"COMMAND,TIMEOUT/K/N", arg, NULL)) ==
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
		else if (streq(c, "PROBE")) cmd = ~0UL;
		else {
			PutStr((CONST_STRPTR)"usage: NetCtrl "
			    "[STATUS|ONLINE|OFFLINE|RECONFIG|WAIT|PROBE] "
			    "[TIMEOUT=<seconds>]\n");
			rc = RETURN_ERROR;
		}
	}
	if (arg[1])
		timeout = *(LONG *)arg[1];
	FreeArgs(rda);
	if (rc != RETURN_OK)
		goto out;
	if (cmd == ~0UL) {
		rc = probe();
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
