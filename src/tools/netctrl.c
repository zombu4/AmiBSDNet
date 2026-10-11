/*
 * NetCtrl: control the AmiBSDNet stack from the Shell.
 *
 *   NetCtrl [STATUS|ONLINE|OFFLINE|RECONFIG|WAIT|PROBE|CHECK|
 *           DISABLEOTHERS|REMOVEOTHERS|RESTOREOTHERS|FALLBACK|
 *           CHECKDRIVER|KEEPCONF|FINDROADSHOW|REMOVEROADSHOW|UNINSTALL|
 *           CHECKSUM|REENABLE|PANIC|PANICTHREAD] [TIMEOUT=<seconds>]
 *           [FILE=<file>] [NOAUTODETECT]
 *
 * PANIC and PANICTHREAD make the kernel panic, on the stack's own task or
 * on a kernel thread, for tests of what follows; the stack refuses them
 * unless it runs with DEBUG (src/stack/control.c).
 *
 * STATUS (the default) prints the stack's status; ONLINE and OFFLINE
 * bring the network up or down; RECONFIG has the stack read its
 * configuration file again (amibsdnet/control.h NETCTRL_RECONFIG).
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
 *
 * FALLBACK goes back to the TCP/IP stack AmiBSDNet was switched from and
 * takes AmiBSDNet out of the boot; REENABLE is the way back from that
 * (otherstacks_self_on(), otherstacks.h).
 *
 * FINDROADSHOW and REMOVEROADSHOW list or remove Roadshow
 * (src/tools/roadshow.c); UNINSTALL removes AmiBSDNet (what
 * S:AmiBSDNet-Install.log lists); CHECKSUM FILE=<file> prints the size
 * and CRC-32 of a file (for the installer's log).
 *
 * Ctrl-C stops REMOVEROADSHOW and FINDROADSHOW between two files.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <intuition/intuition.h>
#include <proto/intuition.h>

#include <dos/var.h>

#include <amibsdnet/control.h>
#include <amibsdnet/ctlcall.h>
#include <amibsdnet/probe.h>
#include <amibsdnet/drvcheck.h>

#include "otherstacks.h"
#include "roadshow.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

#include "amibsdnet_version.h"	/* build/gen, from tools/version.py */
static const char verstag[] __attribute__((used)) =
    AMIBSDNET_VERSTAG("NetCtrl");

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
	/* drivers whose hardware is not there (said, and for the installer) */
	if (probe_unusable[0]) {
		PutStr((CONST_STRPTR)"not usable here:\n");
		PutStr((CONST_STRPTR)probe_unusable);
	}
	setvar("AmiBSDNet/Unusable", probe_unusable[0] ? probe_unusable : NULL);
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
	LONG werr = 0;

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
			/* (FPuts(): "0 normally, otherwise -1", dos.doc
			   FPuts) */
			if (FPuts(fh, (CONST_STRPTR)(line + i)) != 0)
				werr = IoErr();
		}
		Close(in);
	}
	if (!Close(fh) && werr == 0)
		werr = IoErr();
	/* (only a part of the lines: none, and the error) */
	if (werr != 0) {
		PrintFault(werr, (CONST_STRPTR)"NetCtrl");
		DeleteFile((CONST_STRPTR)out);
		return RETURN_ERROR;
	}
	return RETURN_OK;
}

/*
 * CHECKSUM FILE=<path>: "<size> <crc32>" of a file (size in decimal, the
 * CRC-32 drvcheck.c computes, as 8 lowercase hex digits), printed and in
 * AmiBSDNet/FileSum, for the installer's "FILE <path> <size> <crc32>"
 * lines in S:AmiBSDNet-Install.log.
 */
static int
checksum(const char *file)
{
	char out[24], t[12];
	unsigned long size, crc;
	int n = 0, k = 0, i;

	if (!file[0]) {
		PutStr((CONST_STRPTR)"NetCtrl: CHECKSUM needs FILE=\n");
		return RETURN_ERROR;
	}
	if (drv_file_sum(file, &size, &crc) != 0) {
		PrintFault(IoErr(), (CONST_STRPTR)file);
		setvar("AmiBSDNet/FileSum", NULL);
		return RETURN_ERROR;
	}
	do {
		t[k++] = '0' + size % 10;
		size /= 10;
	} while (size);
	while (k)
		out[n++] = t[--k];
	out[n++] = ' ';
	for (i = 0; i < 8; i++)
		out[n++] = "0123456789abcdef"[(crc >> (28 - 4 * i)) & 15];
	out[n] = '\0';
	PutStr((CONST_STRPTR)out);
	PutStr((CONST_STRPTR)"\n");
	setvar("AmiBSDNet/FileSum", out);
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
#define	CMD_FINDRS	0xffff0009UL
#define	CMD_REMOVERS	0xffff000aUL
#define	CMD_UNINSTALL	0xffff000bUL
#define	CMD_CHECKSUM	0xffff000cUL
#define	CMD_REENABLE	0xffff000dUL

/*
 * FINDROADSHOW lists the files of Roadshow (roadshow.c: the files of its
 * archive where its installation puts them), REMOVEROADSHOW takes them
 * out - its startup lines (otherstacks.c), its commands and drivers
 * (deleted), its settings and scripts (moved to SYS:Storage/AmiBSDNet-
 * Roadshow); nothing else on any volume is touched.  Afterwards it looks
 * again and says what is left and what failed.  Sets
 * AmiBSDNet/RoadshowCount (what is left, or was found) and
 * AmiBSDNet/RoadshowList (the first of them).
 */
static int report_atboot(void);
static int roadshow(int remove);

/*
 * The commands NetCtrl does itself run in a process of their own with a
 * 64 KB stack, whatever stack NetCtrl itself was started with: PROBE
 * opens drivers, and the others go through drawers.
 */
static ULONG bs_cmd;
static char bs_file[256];
static int bs_noauto, bs_rc;
static struct Task *bs_parent;
static struct Process *bs_child;
static APTR bs_window;
static volatile int bs_done;

static int local_cmd(ULONG, const char *, int);
static int check(void);
static int others(int);
static int fallback(void);

static void
bs_entry(void)
{

	/* (requesters where NetCtrl's would go: set here as well, though
	   dos/dostags.h NP_WindowPtr says "window ptr - default is same as
	   parent") */
	((struct Process *)SysBase->ThisTask)->pr_WindowPtr = bs_window;
	bs_rc = local_cmd(bs_cmd, bs_file, bs_noauto);
	Forbid();		/* lasts until this process is gone */
	bs_done = 1;
	Signal(bs_parent, SIGBREAKF_CTRL_F);
}

static int
bigstack(ULONG cmd, const char *file, int noauto)
{
	int i;

	bs_cmd = cmd;
	for (i = 0; file[i] && i < (int)sizeof(bs_file) - 1; i++)
		bs_file[i] = file[i];
	bs_file[i] = '\0';
	bs_noauto = noauto;
	bs_done = 0;
	bs_parent = SysBase->ThisTask;
	bs_window = ((struct Process *)bs_parent)->pr_WindowPtr;
	SetSignal(0, SIGBREAKF_CTRL_F);
	bs_child = CreateNewProcTags(NP_Entry, (ULONG)bs_entry,
	    NP_Name, (ULONG)"NetCtrl", NP_StackSize, 65536,
	    NP_Input, Input(), NP_CloseInput, FALSE,
	    NP_Output, Output(), NP_CloseOutput, FALSE, TAG_DONE);
	if (bs_child == NULL) {
		/* (not here instead: the stack may be too small) */
		PutStr((CONST_STRPTR)"NetCtrl: not enough memory\n");
		return RETURN_FAIL;
	}
	while (!bs_done) {
		ULONG sigs = Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);

		/* Ctrl-C may come here or to the working process (a console
		   signals the task that used the handle last: ACTION_READ and
		   ACTION_WRITE set breaktask in downloads/sources/AROS/rom/
		   filesys/console_handler/con_handler.c).  One that comes
		   here goes on to the working process, only while that runs:
		   it sets bs_done under a Forbid that lasts until it is gone */
		if (sigs & SIGBREAKF_CTRL_C) {
			Forbid();
			if (!bs_done)
				Signal(&bs_child->pr_Task, SIGBREAKF_CTRL_C);
			Permit();
		}
	}
	return bs_rc;
}

/* a command NetCtrl does itself (in the big-stack process) */
static int
local_cmd(ULONG cmd, const char *file, int noauto)
{
	int rc = RETURN_FAIL;

	switch (cmd) {
	case CMD_PROBE:
		rc = probe();
		return rc;
	case CMD_CHECK:
		rc = check();
		return rc;
	case CMD_DISABLE:
	case CMD_REMOVE:
	case CMD_RESTORE:
		rc = others(cmd == CMD_DISABLE ? OTHERS_DISABLE :
		    cmd == CMD_REMOVE ? OTHERS_REMOVE : OTHERS_RESTORE);
		return rc;
	case CMD_FALLBACK:
		rc = fallback();
		return rc;
	case CMD_CHECKDRV:
		rc = checkdriver(file[0] ? file : NULL);
		return rc;
	case CMD_UNINSTALL: {
		char msg[640];

		rc = otherstacks_uninstall(msg, sizeof(msg)) == 0 ?
		    RETURN_OK : RETURN_WARN;
		/* for the installer (outside ENV:AmiBSDNet, which is gone) */
		SetVar((CONST_STRPTR)"AmiBSDNet-Uninstalled",
		    (CONST_STRPTR)(rc == RETURN_OK ? "1" : "0"), -1,
		    GVF_GLOBAL_ONLY);
		PutStr((CONST_STRPTR)msg);
		PutStr((CONST_STRPTR)"\n");
		return rc;
	}
	case CMD_FINDRS:
	case CMD_REMOVERS:
		rc = roadshow(cmd == CMD_REMOVERS);
		return rc;
	case CMD_CHECKSUM:
		rc = checksum(file);
		return rc;
	case CMD_REENABLE:
		if (otherstacks_self_on() == 0) {
			PutStr((CONST_STRPTR)"AmiBSDNet's lines in S:User-Startup "
			    "are active again\n");
			rc = RETURN_OK;
		} else {
			PutStr((CONST_STRPTR)"NetCtrl: S:User-Startup could not "
			    "be changed\n");
			rc = RETURN_WARN;
		}
		return rc;
	case CMD_KEEPCONF:
		if (!file[0]) {
			PutStr((CONST_STRPTR)"NetCtrl: KEEPCONF needs FILE=\n");
			rc = RETURN_ERROR;
		} else
			rc = keepconf(file, noauto);
		return rc;
	}
	return rc;
}

static int
roadshow(int remove)
{
	char *list, names[128], num[12], *p;
	int n, failed = 0, left, i, bad = 0;

	if ((list = AllocVec(4096, MEMF_ANY | MEMF_CLEAR)) == NULL)
		return RETURN_FAIL;
	if (remove) {
		/* never neither: only while AmiBSDNet is started at boot */
		if (!otherstacks_self_atboot()) {
			PutStr((CONST_STRPTR)"NetCtrl: AmiBSDNet is not started at "
			    "boot (S:User-Startup); Roadshow not removed\n");
			FreeVec(list);
			return RETURN_WARN;
		}
		/* the startup lines (also in scripts run with Execute), its
		   WBStartup items, LIBS:bsdsocket.library */
		otherstacks_only = "Roadshow";	/* (not Miami etc.) */
		if (otherstacks_apply(OTHERS_REMOVE) != 0) {
			PutStr((CONST_STRPTR)"NetCtrl: Roadshow's startup lines or "
			    "WBStartup items could not all be removed\n");
			bad = 1;
		}
		otherstacks_only = NULL;
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
			PutStr((CONST_STRPTR)"NetCtrl: stopped (Ctrl-C)\n");
			FreeVec(list);
			return RETURN_WARN;
		}
		roadshow_find(1, list, 4096, &failed);
		PutStr((CONST_STRPTR)"removed:\n");
		PutStr((CONST_STRPTR)list);
		if (failed) {
			PutStr((CONST_STRPTR)"NetCtrl: ");
			{
				char t[12];
				int k = 0;
				ULONG v = failed;

				do {
					t[k++] = '0' + v % 10;
					v /= 10;
				} while (v);
				p = num;
				while (k)
					*p++ = t[--k];
				*p = '\0';
			}
			PutStr((CONST_STRPTR)num);
			PutStr((CONST_STRPTR)" could not be removed (see above)\n");
			bad = 1;
		}
		/* a switch to AmiBSDNet without a way back: no trial */
		DeleteVar((CONST_STRPTR)"AmiBSDNet/Trial",
		    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
	}
	/* (also the check after removing) */
	n = roadshow_find(0, list, 4096, &failed);
	/* (looking fails only when stopped by Ctrl-C or out of memory) */
	if (failed)
		bad = 1;
	/* (only Roadshow counts here: another stack is not left over; a
	   bsdsocket.library on disk is roadshow_find()'s, which leaves it
	   alone while another stack is installed) */
	otherstacks_check(names, sizeof(names));
	otherstacks_only = "Roadshow";
	left = n + report_atboot();
	otherstacks_only = NULL;
	if (n > 0) {
		PutStr((CONST_STRPTR)(remove ? "still there:\n" :
		    "Roadshow:\n"));
		PutStr((CONST_STRPTR)list);
	}
	p = num;
	{
		char t[12];
		int k = 0;
		ULONG v = left;

		do {
			t[k++] = '0' + v % 10;
			v /= 10;
		} while (v);
		while (k)
			*p++ = t[--k];
		*p = '\0';
	}
	setvar("AmiBSDNet/RoadshowCount", num);
	for (i = 0; list[i] && i < 900; i++)
		;
	list[i] = '\0';
	setvar("AmiBSDNet/RoadshowList", list[0] ? list : NULL);
	if (left == 0)
		PutStr((CONST_STRPTR)(remove ? "none of Roadshow's files or startup lines that "
		    "REMOVEROADSHOW handles are left\n" :
		    "nothing of Roadshow found\n"));
	FreeVec(list);
	return left || bad ? RETURN_WARN : RETURN_OK;
}

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
	struct MsgPort *left;
	int r;

	if ((r = amibsdnet_ctl_call(m, cmd, AMIBSDNET_CTL_TIMEOUT,
	    &left)) == -2) {
		/* the stack hangs: m stays its (and is never freed) */
		PutStr((CONST_STRPTR)"NetCtrl: AmiBSDNet does not answer\n");
		return -2;
	}
	return r;
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
	int rc = RETURN_OK, r = 0;

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
		/* (for tests, with a stack started with DEBUG only:
		   amibsdnet/control.h) */
		else if (streq(c, "PANIC")) cmd = NETCTRL_PANIC;
		else if (streq(c, "PANICTHREAD")) cmd = NETCTRL_PANICTHREAD;
		else if (streq(c, "WAIT")) cmd = 0;
		else if (streq(c, "PROBE")) cmd = CMD_PROBE;
		else if (streq(c, "CHECK")) cmd = CMD_CHECK;
		else if (streq(c, "DISABLEOTHERS")) cmd = CMD_DISABLE;
		else if (streq(c, "REMOVEOTHERS")) cmd = CMD_REMOVE;
		else if (streq(c, "RESTOREOTHERS")) cmd = CMD_RESTORE;
		else if (streq(c, "FALLBACK")) cmd = CMD_FALLBACK;
		else if (streq(c, "CHECKDRIVER")) cmd = CMD_CHECKDRV;
		else if (streq(c, "KEEPCONF")) cmd = CMD_KEEPCONF;
		else if (streq(c, "FINDROADSHOW")) cmd = CMD_FINDRS;
		else if (streq(c, "REMOVEROADSHOW")) cmd = CMD_REMOVERS;
		else if (streq(c, "UNINSTALL")) cmd = CMD_UNINSTALL;
		else if (streq(c, "CHECKSUM")) cmd = CMD_CHECKSUM;
		else if (streq(c, "REENABLE")) cmd = CMD_REENABLE;
		else {
			PutStr((CONST_STRPTR)"usage: NetCtrl "
			    "[STATUS|ONLINE|OFFLINE|RECONFIG|WAIT|PROBE|CHECK|\n"
			    "    DISABLEOTHERS|REMOVEOTHERS|RESTOREOTHERS|FALLBACK|\n"
			    "    CHECKDRIVER|KEEPCONF|FINDROADSHOW|REMOVEROADSHOW|\n"
			    "    UNINSTALL|CHECKSUM|REENABLE|PANIC|PANICTHREAD]\n"
			    "    [TIMEOUT=<seconds>]\n"
			    "    [FILE=<file>] [NOAUTODETECT]\n");
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
	if (cmd >= CMD_PROBE && cmd <= CMD_REENABLE) {
		rc = bigstack(cmd, file, noauto);
		goto out;
	}

	if ((m = AllocVec(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR)) == NULL) {
		rc = RETURN_FAIL;
		goto out;
	}
	if (cmd == 0) {
		/* WAIT: poll until online (the stack may still be starting) */
		LONG ticks;
		int up = 0;

		for (ticks = 0; ticks <= timeout * 50; ticks += 25) {
			if ((r = netctrl_send(NETCTRL_STATE, m)) == -2)
				break;
			if (r == 0 && m->online) {
				up = 1;
				break;
			}
			if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
				break;
			Delay(25);
		}
		if (!up) {
			PutStr((CONST_STRPTR)"NetCtrl: network not up\n");
			rc = RETURN_WARN;
		}
	} else if ((r = netctrl_send(cmd, m)) == -1) {
		PutStr((CONST_STRPTR)"NetCtrl: AmiBSDNet is not running\n");
		rc = RETURN_WARN;
	} else if (r != 0) {
		rc = RETURN_WARN;	/* (no answer: said so) */
	} else if (cmd == NETCTRL_STATUS) {
		PutStr((CONST_STRPTR)m->text);
	} else {
		PutStr((CONST_STRPTR)(m->result == 0 ? "ok" : "failed"));
		PutStr((CONST_STRPTR)(m->online ? " (online)\n" : " (offline)\n"));
		if (m->result)
			rc = RETURN_ERROR;
	}
	/* (after no answer the message is the stack's: left alone) */
	if (r != -2)
		FreeVec(m);
out:
	CloseLibrary((struct Library *)DOSBase);
	return rc;
}
