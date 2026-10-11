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
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <devices/timer.h>

#include <amibsdnet/notice.h>
#include <amibsdnet/control.h>
#include <amibsdnet/logs.h>

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
void	bsdsocket_kernel_gone(void);

#define	DEFAULT_CONFIG	"ENV:AmiBSDNet/AmiBSDNet.conf"
#define	FALLBACK_CONFIG	"ENVARC:AmiBSDNet/AmiBSDNet.conf"
/* on disk, to be there after a reboot; T: if that cannot be written
   (amibsdnet/logs.h) */
#define	DEFAULT_LOG	AMIBSDNET_LOG
#define	VERSTAG		AMIBSDNET_VERSTAG("AmiBSDNet")
#define	CLAIM_NAME	"AmiBSDNet stack"
#define	FAILED_NAME	"AmiBSDNet (failed)"
#define	SLEEPER_NAME	"AmiBSDNet launcher"

/* a Workbench launcher sleeps for ever to keep the program loaded; under
   its own name, so FindTask("AmiBSDNet") finds only the stack process */
static void
sleep_forever(void)
{

	Forbid();
	SysBase->ThisTask->tc_Node.ln_Name = (char *)SLEEPER_NAME;
	Permit();
	for (;;)
		Wait(0x80000000UL);
}

static const char verstag[] __attribute__((used)) = VERSTAG;

/*
 * The launcher's stack from the Shell: the Shell scans the first 4 KB of
 * each segment for "$STACK:<bytes>" and gives the command at least that
 * much (NDK3.2 ReleaseNotes/shell-RelNotes:100-110, 301-302).  System()
 * alone wants about 800 bytes (dos.doc SystemTagList NOTES); EasyRequest()
 * switches to its own stack (downloads/sources/NDK3.2/Autodocs/
 * intuition.doc:1921-1922, EasyRequestArgs NOTE).  The section puts it
 * next to _start(), at the start of the code; _start() takes its address
 * so --gc-sections keeps it (gcc 15.1 here warns "'retain' attribute
 * ignored" for it).
 */
static const char stacktag[] __attribute__((used,
    section(".text.unlikely.0_stack"))) = "$STACK:16384\n";

static struct Task *volatile launcher;	/* NULL once it stopped waiting */

/*
 * Only one AmiBSDNet: the launcher looks for an earlier one and puts this
 * semaphore up in the same Forbid(), so two launchers cannot both start a
 * stack.  It stays up while the stack runs (the program stays loaded) and
 * is taken down when the stack is not started or failed to start.
 */
static struct SignalSemaphore claim;

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
static int logpath_given;		/* LOG= on the command line */

/* T: when the default drawer cannot be written; none for LOG= */
static const char *
logfallback(void)
{

	return logpath_given ? NULL : AMIBSDNET_LOG_T;
}
static int guard_failed;		/* the start marker was not made */
static LONG guard_error;

#define	P	amiga_rump_printf

/* a few lines into the log file when the stack does not run (nobody sees
   the Shell output at boot); no requesters */
static void
log_lines(const char *a, const char *b, const char *c)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR old = me->pr_WindowPtr;
	BPTR lf;

	me->pr_WindowPtr = (APTR)-1;
	if ((lf = amibsdnet_log_create(logpath, logfallback(), NULL)) != 0) {
		FPuts(lf, (CONST_STRPTR)a);
		if (b)
			FPuts(lf, (CONST_STRPTR)b);
		if (c)
			FPuts(lf, (CONST_STRPTR)c);
		Close(lf);
	}
	me->pr_WindowPtr = old;
}

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

/*
 * The start failed: never returns.  inkernel: called by rumpuser_exit()
 * on this task, which is inside a kernel call (kernel_gone_here()), so the
 * host side is left as it is (amiga_rump_hostfini() is for a task outside
 * the kernel).
 */
static void
start_failed(int kernel_started, int inkernel)
{
	BPTR log;

	/* a retry must be able to start a new stack: this process stops
	   being "AmiBSDNet" and the claim goes */
	Forbid();
	SysBase->ThisTask->tc_Node.ln_Name = (char *)FAILED_NAME;
	RemSemaphore(&claim);
	Permit();
	startup_result = 20;
	trial_failed();		/* a trial switch goes back at once */
	/* the log file is closed, so a retry can open it again
	   (amiga_rump_logswitch(): nothing writes to the old handle once
	   it returns) */
	if ((log = (BPTR)amiga_rump_logswitch(0)) != 0)
		Close(log);
	/* the host side goes only if the kernel never started or is gone
	   (rumpuser_exit() set amiga_rump_exited): amiga_rump_hostfini() is
	   for that case only (rumpuser_amiga.h) */
	if (!inkernel && (!kernel_started || amiga_rump_exited))
		amiga_rump_hostfini();
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

/*
 * The kernel has stopped (rumpuser_exit(), a panic): from here on nothing
 * may call into it - not the configuration, not the control commands, not
 * the end of the start.  Called from the main loop (inkernel 0), or by
 * rumpuser_exit() itself when this task was inside the kernel
 * (kernel_gone_here()).  Before the start is over (startup_result still
 * -1, the launcher waiting) it is a failed start.
 */
static void
kernel_gone(int inkernel)
{

	P("AmiBSDNet: the kernel has stopped; the stack no longer answers\n");
	bsdsocket_kernel_gone();
	/* (first: the control port may be public already during the start,
	   control_init() comes before startup_result = 0) */
	control_shutdown();
	if (startup_result < 0)
		start_failed(1, inkernel);
	start_over();
	for (;;)
		Wait(0x80000000UL);
}

/* amiga_rump_exitfn */
static void
kernel_gone_here(void)
{

	kernel_gone(1);
}

static void
stack_main(void)
{
	BPTR log;
	int rv;
	struct MsgPort *tport;
	struct timerequest *treq = NULL;
	ULONG tmask = 0;
	int kernel_started = 0;		/* rump_init() was called */

	((struct Process *)SysBase->ThisTask)->pr_WindowPtr = (APTR)-1;
	/* empty the log, then keep it open with a shared lock so it can be
	   read (Type) while the stack runs */
	{
		const char *used = NULL;

		log = amibsdnet_log_create(logpath, logfallback(), &used);
		if (log && used != logpath)
			sb_copy(logpath, used, sizeof(logpath));
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
	amiga_rump_exitfn = kernel_gone_here;
	if (amiga_rump_hostinit(0) != 0)
		goto fail;
	P("AmiBSDNet " AMIBSDNET_VERSION " starting\n");
	if (guard_failed)
		P("AmiBSDNet: the start marker %s/Starting-Stack could not be "
		    "made (error %ld): a freeze during this start is not "
		    "noticed at the next one\n", GUARD_DIR, (long)guard_error);

	kernel_started = 1;
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
		P("AmiBSDNet: only detected adapters are used\n");
	if (treq == NULL)
		start_over();	/* (no timer: no notice for long) */

	for (;;) {
		ULONG s = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D |
		    SIGBREAKF_CTRL_E | control_sigmask() | tmask);

		/* (rumpuser_exit() sends CTRL_C too: everything below
		   calls the kernel) */
		if (amiga_rump_exited)
			kernel_gone(0);

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

		/* (link changes; also a DHCP client that ended, which a
		   reconfiguration waits for) */
		if (s & SIGBREAKF_CTRL_E) {
			stack_link_changed();
			stack_reconfigure_poll();
		}

		if (s & control_sigmask())
			control_handle();

		if (s & SIGBREAKF_CTRL_C) {
			P("AmiBSDNet: going offline\n");
			stack_offline();
		}
		if (s & SIGBREAKF_CTRL_D) {
			P("AmiBSDNet: reconfiguring\n");
			stack_reconfigure_begin();
		}
	}

fail:
	start_failed(kernel_started, 0);
}

/* end without starting the stack */
static int
leave(struct Message *wbmsg, int keep)
{
	struct CommandLineInterface *cli;

	if (keep) {
		/* a process started here runs this program's code: it must
		   stay loaded (see the end of _start()) */
		if ((cli = Cli()) != NULL)
			cli->cli_Module = 0;
		if (wbmsg)
			sleep_forever();
	}
	CloseLibrary((struct Library *)DOSBase);
	if (wbmsg) {
		Forbid();
		ReplyMsg(wbmsg);
	}
	return RETURN_WARN;	/* never abort S:User-Startup */
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct CommandLineInterface *cli;
	struct RDArgs *rda;
	struct Process *me;
	struct Message *wbmsg = NULL;
	int t, ours, other;
	LONG argv[3] = { 0, 0, 0 };
	BPTR out;

	__asm volatile ("" : : "r"(stacktag));	/* (see stacktag) */
	SysBase = *(struct ExecBase **)4;
	me = (struct Process *)SysBase->ThisTask;
	if (me->pr_CLI == 0) {
		/* started from Workbench */
		WaitPort(&me->pr_MsgPort);
		wbmsg = GetMsg(&me->pr_MsgPort);
	}
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	if (DOSBase == NULL) {
		/* (the startup message goes back as in leave()) */
		if (wbmsg) {
			Forbid();
			ReplyMsg(wbmsg);
		}
		return RETURN_FAIL;
	}
	out = wbmsg ? 0 : Output();

	if (!wbmsg) {
		if ((rda = ReadArgs((CONST_STRPTR)"CONFIG/K,LOG/K,DEBUG/S",
		    argv, NULL)) == NULL) {
			LONG err = IoErr();
			char why[80];

			PrintFault(err, (CONST_STRPTR)"AmiBSDNet");
			if (Fault(err, NULL, (STRPTR)why, sizeof(why)) <= 0)
				why[0] = '\0';
			log_lines("AmiBSDNet: not started: bad arguments: ",
			    why, "\n");
			return leave(NULL, 0);
		}
		if (argv[0])
			sb_copy(cfgpath, (const char *)argv[0], sizeof(cfgpath));
		if (argv[1]) {
			sb_copy(logpath, (const char *)argv[1], sizeof(logpath));
			logpath_given = 1;
		}
		amiga_rump_debug = argv[2] ? 1 : 0;
		FreeArgs(rda);
	}

	/*
	 * Only one stack: not twice, and not next to another one.  Ours is
	 * the claim, the control port, or the process (its port comes only
	 * after the kernel started, a few seconds later; not this one: from
	 * Workbench it has the name too).
	 */
	Forbid();
	{
		struct Task *st = FindTask((CONST_STRPTR)"AmiBSDNet");

		ours = FindSemaphore((STRPTR)CLAIM_NAME) != NULL ||
		    FindPort((CONST_STRPTR)AMIBSDNET_PORTNAME) != NULL ||
		    (st != NULL && st != SysBase->ThisTask);
	}
	other = !ours && FindName(&SysBase->LibList,
	    (CONST_STRPTR)"bsdsocket.library") != NULL;
	if (!ours && !other) {
		claim.ss_Link.ln_Name = (char *)CLAIM_NAME;
		claim.ss_Link.ln_Pri = 0;
		AddSemaphore(&claim);
	}
	Permit();
	if (ours) {
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet is already running\n");
		return leave(wbmsg, 0);
	}
	if (other) {
		struct Library *lib;
		char id[80], v[4];
		APTR oldwin = me->pr_WindowPtr;
		int k = 0;

		/*
		 * Another stack runs at boot again: a failed trial's fallback
		 * (see trial.c) is done, also if FALLBACK could not take
		 * AmiBSDNet out of the boot.  A trial still to come ("1": the
		 * installer ran, the old stack runs until the reboot) stays.
		 */
		me->pr_WindowPtr = (APTR)-1;
		if (GetVar((CONST_STRPTR)"AmiBSDNet/Trial", (STRPTR)v,
		    sizeof(v), GVF_GLOBAL_ONLY) >= 0 && v[0] != '1')
			DeleteVar((CONST_STRPTR)"AmiBSDNet/Trial",
			    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
		me->pr_WindowPtr = oldwin;
		/* at boot nobody sees the Shell output: say it in the log,
		   with the name of the library that is there (copied under
		   Forbid: the library may go meanwhile) */
		Forbid();
		if ((lib = (struct Library *)FindName(&SysBase->LibList,
		    "bsdsocket.library")) != NULL && lib->lib_IdString)
			for (; k < 79 && ((char *)lib->lib_IdString)[k] &&
			    ((char *)lib->lib_IdString)[k] != '\r' &&
			    ((char *)lib->lib_IdString)[k] != '\n'; k++)
				id[k] = ((char *)lib->lib_IdString)[k];
		Permit();
		id[k] = '\0';
		log_lines("AmiBSDNet: not started: another TCP/IP stack's "
		    "bsdsocket.library is already in memory (",
		    id[0] ? id : "no name", ").\nRemove that stack (see "
		    "\"NetCtrl CHECK\") and reboot.\n");
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet: another TCP/IP stack is "
			    "running (bsdsocket.library is in use); not "
			    "started\n");
		return leave(wbmsg, 0);
	}

	/*
	 * The last start never finished - the Amiga froze while AmiBSDNet
	 * was starting (or was switched off): not this time, and say so.
	 * The next boot starts it normally again.
	 */
	if (guard_begin("Stack")) {
		RemSemaphore(&claim);
		guard_skipped("Stack");
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet: not started this time: "
			    "its last start never finished (the Amiga froze?)\n");
		return leave(wbmsg, 0);
	}
	guard_failed = guard_nomarker;
	guard_error = guard_nomarker_error;

	for (void (**c)(void) = __init_array_start; c < __init_array_end; c++)
		(*c)();

	/*
	 * A trial switch from another stack (see trial.c): if the last trial
	 * boot never connected, go back to the other stack instead of
	 * starting again.  From here on a process may run our code, so the
	 * segments must stay loaded on every path that started one.
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
		RemSemaphore(&claim);
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet: the switch to AmiBSDNet "
			    "did not work; going back to the previous TCP/IP "
			    "stack\n");
		return leave(wbmsg, trial_code_running());
	}

	launcher = SysBase->ThisTask;
	if (CreateNewProcTags(NP_Entry, (ULONG)stack_main,
	    NP_Name, (ULONG)"AmiBSDNet", NP_StackSize, 256 * 1024,
	    NP_Priority, 1, TAG_DONE) == NULL) {
		guard_end("Stack");
		RemSemaphore(&claim);
		log_lines("AmiBSDNet: not started: the stack process could not "
		    "be created (not enough memory?)\n", NULL, NULL);
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet: cannot create the stack "
			    "process\n");
		trial_failed();
		return leave(wbmsg, trial_code_running());
	}
	{
		/* the kernel starts in seconds; never hold up the boot for long */
		int ticks;

		/* (also over when the kernel stops on one of its threads while
		   the stack process is inside rump_init(): rumpuser_exit()
		   sets amiga_rump_exited, and nothing wakes that process) */
		for (ticks = 0; ticks < 120 * 50 && !(SetSignal(0, 0) &
		    SIGBREAKF_CTRL_F) && !amiga_rump_exited; ticks += 10)
			Delay(10);
		/* a panic on the stack process itself ends in start_failed(),
		   which sets startup_result and wakes this task right after
		   rumpuser_exit() set amiga_rump_exited: up to 5 s more for
		   that (a stack process stuck in rump_init() never does) */
		for (ticks = 0; amiga_rump_exited && startup_result < 0 &&
		    ticks < 5 * 50 && !(SetSignal(0, 0) & SIGBREAKF_CTRL_F);
		    ticks += 10)
			Delay(10);
		/* from now on nobody signals this task (it may be gone) */
		Forbid();
		launcher = NULL;
		Permit();
		SetSignal(0, SIGBREAKF_CTRL_F);
		/* (the stack process keeps its name and the claim then, so
		   the claim check in _start() says "already running") */
		if (startup_result < 0 && amiga_rump_exited && out) {
			PutStr((CONST_STRPTR)"AmiBSDNet: the kernel stopped "
			    "during the start, see ");
			PutStr((CONST_STRPTR)logpath);
			PutStr((CONST_STRPTR)"; restart the computer to start "
			    "it again\n");
		} else if (startup_result < 0 && out)
			PutStr((CONST_STRPTR)"AmiBSDNet: still starting in the "
			    "background\n");
	}
	if (startup_result > 0 && out) {
		PutStr((CONST_STRPTR)"AmiBSDNet: startup failed, see ");
		PutStr((CONST_STRPTR)logpath);
		PutStr((CONST_STRPTR)"; it can be started again\n");
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
		sleep_forever();
	}
	return startup_result == 0 ? RETURN_OK : RETURN_WARN;
}
