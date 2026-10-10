/*
 * AmiBSDNet: the network stack program.
 *
 *   AmiBSDNet [CONFIG=<file>] [LOG=<file>] [DEBUG]
 *
 * Detaches from the Shell, starts the NetBSD kernel, configures the
 * interfaces from the configuration file and publishes bsdsocket.library.
 * The stack then stays resident (its code must remain loaded while kernel
 * threads run); "Break <process> C" takes the interfaces offline.
 *
 * Configuration (default ENV:AmiBSDNet/AmiBSDNet.conf), one item per line:
 *
 *   hostname   amiga
 *   interface  sana0 uaenet.device 0 dhcp
 *   interface  sana1 a2065.device 0 address 192.168.1.20/24
 *   gateway    192.168.1.1
 *   nameserver 192.168.1.1
 *   domain     example.org
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <devices/timer.h>

#include <amibsdnet/notice.h>

#include "rumpuser_amiga.h"
#include "stack.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

int	rump_init(void);
struct Library *bsdsocket_create(void);

#define	DEFAULT_CONFIG	"ENV:AmiBSDNet/AmiBSDNet.conf"
#define	FALLBACK_CONFIG	"ENVARC:AmiBSDNet/AmiBSDNet.conf"
#define	DEFAULT_LOG	"T:AmiBSDNet.log"
#define	VERSTAG		"\0$VER: AmiBSDNet 0.8.2 (10.10.2026)"

static const char verstag[] __attribute__((used)) = VERSTAG;

static struct Task *volatile launcher;	/* NULL once it stopped waiting */

/* the launcher waits at most two minutes: after that it may be gone (an
   ended Shell), and must not be signalled */
static void
wake_launcher(void)
{

	Forbid();
	if (launcher)
		Signal(launcher, SIGBREAKF_CTRL_F);
	Permit();
}
static volatile int startup_result = -1;
static char cfgpath[256] = DEFAULT_CONFIG;
static char logpath[256] = DEFAULT_LOG;

#define	P	amiga_rump_printf

/*
 * The start notice (amibsdnet/notice.h): for the first minute every line
 * of the log is also shown on screen, before the step it announces.
 */
static struct notice startnotice;
static char teeline[NOTICE_COLS];
static int teelen;

static void
notice_tee(const char *text, long len)
{
	long i;

	/* (whole lines: with DEBUG the log comes a character at a time) */
	for (i = 0; i < len; i++) {
		if (text[i] == '\n' || teelen == NOTICE_COLS - 1) {
			notice_show(&startnotice, teeline, teelen);
			teelen = 0;
		}
		if (text[i] != '\n')
			teeline[teelen++] = text[i];
	}
}

/* the start is over: the notice goes, and so does the start marker */
static void
start_over(void)
{

	amiga_rump_logtee(NULL);
	notice_close(&startnotice);
	guard_end("Stack");
}

static void
stack_main(void)
{
	BPTR log;
	int rv;
	struct MsgPort *tport;
	struct timerequest *treq = NULL;
	ULONG tmask = 0;

	((struct Process *)SysBase->ThisTask)->pr_WindowPtr = (APTR)-1;
	/* empty the log, then keep it open with a shared lock so it can be
	   read (Type T:AmiBSDNet.log) while the stack runs */
	if ((log = Open((CONST_STRPTR)logpath, MODE_NEWFILE)) != 0) {
		Close(log);
		log = Open((CONST_STRPTR)logpath, MODE_READWRITE);
	}
	amiga_rump_loginit((long)log);
	notice_open(&startnotice, "AmiBSDNet is starting (this window closes "
	    "by itself)", 0);
	amiga_rump_logtee(notice_tee);
	/* (with DEBUG only, as for the rump threads: the report ends in
	   rumpuser_exit, which would leave this process hanging; without it
	   a crash gets the system's own Software Failure requester) */
	if (amiga_rump_debug)
		crash_install();
	if (amiga_rump_hostinit(0) != 0)
		goto fail;
	P("AmiBSDNet 0.8.2 starting\n");

	if ((rv = rump_init()) != 0) {
		P("AmiBSDNet: kernel failed to start (%d)\n", rv);
		goto fail;
	}
	if (bsdsocket_create() == NULL) {
		P("AmiBSDNet: cannot create bsdsocket.library\n");
		goto fail;
	}
	if (control_init() != 0)
		P("AmiBSDNet: no control port\n");
	P("AmiBSDNet: bsdsocket.library ready\n");
	/*
	 * Let the Shell continue now: interface setup (DHCP may take a while
	 * when no server answers) must not hold up the boot.
	 * "NetCtrl WAIT" blocks until the network is up.
	 */
	startup_result = 0;
	wake_launcher();

	/* the end of the start, in a minute */
	if ((tport = CreateMsgPort()) != NULL &&
	    (treq = (struct timerequest *)CreateIORequest(tport,
	    sizeof(*treq))) != NULL &&
	    OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
	    (struct IORequest *)treq, 0) == 0) {
		treq->tr_node.io_Command = TR_ADDREQUEST;
		treq->tr_time.tv_secs = START_SECS;
		treq->tr_time.tv_micro = 0;
		SendIO((struct IORequest *)treq);
		tmask = 1UL << tport->mp_SigBit;
	} else {
		if (treq)
			DeleteIORequest((struct IORequest *)treq);
		if (tport)
			DeleteMsgPort(tport);
		treq = NULL;
	}

	if (stack_configure_from(cfgpath, FALLBACK_CONFIG) != 0)
		P("AmiBSDNet: no configuration found, loopback only\n");
	if (treq == NULL)
		start_over();	/* (no timer: no notice for long) */

	for (;;) {
		ULONG s = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D |
		    SIGBREAKF_CTRL_E | control_sigmask() | tmask);

		if (tmask && (s & tmask) && CheckIO((struct IORequest *)treq)) {
			WaitIO((struct IORequest *)treq);
			CloseDevice((struct IORequest *)treq);
			DeleteIORequest((struct IORequest *)treq);
			DeleteMsgPort(tport);
			treq = NULL;
			tmask = 0;
			P("AmiBSDNet: started\n");
			start_over();
		}

		if (s & SIGBREAKF_CTRL_E)
			stack_link_changed();

		if (s & control_sigmask())
			control_handle();

		if (s & SIGBREAKF_CTRL_C) {
			P("AmiBSDNet: going offline\n");
			stack_offline();
		}
		if (s & SIGBREAKF_CTRL_D) {
			P("AmiBSDNet: reconfiguring\n");
			stack_reconfigure();
		}
	}

fail:
	startup_result = 20;
	trial_failed();		/* a trial switch goes back at once */
	wake_launcher();
	/* (it did not freeze: the next start is a normal one at once; the
	   notice shows why for a minute) */
	guard_end("Stack");
	Delay(START_SECS * 50);
	start_over();
	/* kernel threads may exist: never return into unloaded code */
	for (;;)
		Wait(0x80000000UL);
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct CommandLineInterface *cli;
	struct RDArgs *rda;
	struct Process *me;
	struct Message *wbmsg = NULL;
	int t;
	LONG argv[3] = { 0, 0, 0 };
	BPTR out;

	SysBase = *(struct ExecBase **)4;
	me = (struct Process *)SysBase->ThisTask;
	if (me->pr_CLI == 0) {
		/* started from Workbench */
		WaitPort(&me->pr_MsgPort);
		wbmsg = GetMsg(&me->pr_MsgPort);
	}
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	if (DOSBase == NULL)
		return RETURN_FAIL;
	out = wbmsg ? 0 : Output();

	if (FindName(&SysBase->LibList, "bsdsocket.library")) {
		/*
		 * Another stack runs at boot again: a failed trial's fallback
		 * (see trial.c) is done, also if FALLBACK could not take
		 * AmiBSDNet out of the boot.  A trial still to come ("1": the
		 * installer ran, the old stack runs until the reboot) stays.
		 */
		if (FindPort((CONST_STRPTR)"AmiBSDNet") == NULL) {
			APTR oldwin = me->pr_WindowPtr;
			char v[4];

			me->pr_WindowPtr = (APTR)-1;
			if (GetVar((CONST_STRPTR)"AmiBSDNet/Trial", (STRPTR)v,
			    sizeof(v), GVF_GLOBAL_ONLY) >= 0 && v[0] != '1')
				DeleteVar((CONST_STRPTR)"AmiBSDNet/Trial",
				    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
			me->pr_WindowPtr = oldwin;
		}
		/* at boot nobody sees the Shell output: say it in the log,
		   with the name of the library that is there */
		{
			struct Library *lib;
			char id[80];
			BPTR lf;
			int k = 0;

			/* (copied under Forbid: the library may go meanwhile) */
			Forbid();
			if ((lib = (struct Library *)FindName(&SysBase->LibList,
			    "bsdsocket.library")) != NULL && lib->lib_IdString)
				for (; k < 79 && ((char *)lib->lib_IdString)[k] &&
				    ((char *)lib->lib_IdString)[k] != '\r' &&
				    ((char *)lib->lib_IdString)[k] != '\n'; k++)
					id[k] = ((char *)lib->lib_IdString)[k];
			Permit();
			id[k] = '\0';
			if (FindPort((CONST_STRPTR)"AmiBSDNet") == NULL &&
			    (lf = Open((CONST_STRPTR)"T:AmiBSDNet.log",
			    MODE_NEWFILE)) != 0) {
				FPuts(lf, (CONST_STRPTR)"AmiBSDNet: not started: "
				    "another TCP/IP stack's bsdsocket.library is "
				    "already in memory (");
				FPuts(lf, (CONST_STRPTR)(id[0] ? id : "no name"));
				FPuts(lf, (CONST_STRPTR)").\nRemove that stack "
				    "(see \"NetCtrl CHECK\") and reboot.\n");
				Close(lf);
			}
		}
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet: a TCP/IP stack is "
			    "already running\n");
		CloseLibrary((struct Library *)DOSBase);
		if (wbmsg) {
			Forbid();
			ReplyMsg(wbmsg);
		}
		return RETURN_WARN;
	}
	if (!wbmsg && (rda = ReadArgs((CONST_STRPTR)"CONFIG/K,LOG/K,DEBUG/S", argv,
	    NULL)) != NULL) {
		if (argv[0])
			sb_copy(cfgpath, (const char *)argv[0], sizeof(cfgpath));
		if (argv[1])
			sb_copy(logpath, (const char *)argv[1], sizeof(logpath));
		amiga_rump_debug = argv[2] ? 1 : 0;
		FreeArgs(rda);
	}

	/* only one stack: not twice, and not next to another one */
	{
		int ours, other;

		Forbid();
		/* (the process too: its port comes only after the kernel
		   started, a few seconds later) */
		{
			struct Task *st = FindTask((CONST_STRPTR)"AmiBSDNet");

			/* (not this one: from Workbench it has the name too) */
			ours = FindPort((CONST_STRPTR)"AmiBSDNet") != NULL ||
			    (st != NULL && st != SysBase->ThisTask);
		}
		other = !ours && FindName(&SysBase->LibList,
		    (CONST_STRPTR)"bsdsocket.library") != NULL;
		Permit();
		if (ours || other) {
			if (out)
				PutStr((CONST_STRPTR)(ours ?
				    "AmiBSDNet is already running\n" :
				    "AmiBSDNet: another TCP/IP stack is running "
				    "(bsdsocket.library is in use); not started\n"));
			CloseLibrary((struct Library *)DOSBase);
			if (wbmsg) {
				Forbid();
				ReplyMsg(wbmsg);
			}
			return RETURN_WARN;	/* never abort S:User-Startup */
		}
	}

	/*
	 * The last start never finished - the Amiga froze while AmiBSDNet
	 * was starting (or was switched off): not this time, and say so.
	 * The next boot starts it normally again.
	 */
	if (guard_begin("Stack")) {
		guard_skipped("Stack");
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet: not started this time: "
			    "its last start never finished (the Amiga froze?)\n");
		CloseLibrary((struct Library *)DOSBase);
		if (wbmsg) {
			Forbid();
			ReplyMsg(wbmsg);
		}
		return RETURN_WARN;
	}

	for (void (**c)(void) = __init_array_start; c < __init_array_end; c++)
		(*c)();

	/*
	 * A trial switch from another stack (see trial.c): if the last trial
	 * boot never connected, go back to the other stack instead of
	 * starting again.  From here on a process may run our code, so the
	 * segments must stay loaded on every path below.
	 */
	/* the log's lock, before anything may log (trial.c's watchdog);
	   the log file itself is opened by stack_main() */
	amiga_rump_loginit(0);
	t = trial_begin();
	if (t > 0 && out)
		PutStr((CONST_STRPTR)"AmiBSDNet: going back to the previous "
		    "TCP/IP stack did not finish; starting AmiBSDNet\n");
	if (t < 0) {
		guard_end("Stack");
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet: the switch to AmiBSDNet "
			    "did not work; going back to the previous TCP/IP "
			    "stack\n");
		CloseLibrary((struct Library *)DOSBase);
		if (wbmsg) {
			Forbid();
			ReplyMsg(wbmsg);
		}
		return RETURN_WARN;
	}

	launcher = SysBase->ThisTask;
	if (CreateNewProcTags(NP_Entry, (ULONG)stack_main,
	    NP_Name, (ULONG)"AmiBSDNet", NP_StackSize, 256 * 1024,
	    NP_Priority, 1, TAG_DONE) == NULL) {
		guard_end("Stack");
		trial_failed();
	} else {
		/* the kernel starts in seconds; never hold up the boot for long */
		int ticks;

		for (ticks = 0; ticks < 120 * 50 && !(SetSignal(0, 0) &
		    SIGBREAKF_CTRL_F); ticks += 10)
			Delay(10);
		/* from now on nobody signals this task (it may be gone) */
		Forbid();
		launcher = NULL;
		Permit();
		SetSignal(0, SIGBREAKF_CTRL_F);
		if (startup_result < 0 && out)
			PutStr((CONST_STRPTR)"AmiBSDNet: still starting in the "
			    "background\n");
	}
	if (startup_result > 0) {
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet: startup failed, see "
			    "the log\n");
	}
	/*
	 * The stack process keeps running our code: take the program's
	 * segments away from the Shell so they are never unloaded.
	 */
	if ((cli = Cli()) != NULL)
		cli->cli_Module = 0;
	if (wbmsg) {
		/*
		 * Workbench unloads a program when its startup message is
		 * replied; keep it so the code stays resident, and leave this
		 * launcher process sleeping.
		 */
		for (;;)
			Wait(0x80000000UL);
	}
	return startup_result == 0 ? RETURN_OK : RETURN_WARN;
}
