/*
 * AmiBSDNet: trial switch-over from another TCP/IP stack.
 *
 * When the installer takes another stack (Roadshow, ...) out of the boot
 * it sets the variable AmiBSDNet/Trial to 1.  Nobody must be left without
 * a network, whatever goes wrong:
 *
 * - Before anything else is started, AmiBSDNet sets it to 2.  Only a
 *   working connection deletes it.  So if a boot ends without one (the
 *   stack hung or crashed while starting, or simply did not connect) the
 *   next boot finds 2 and goes back at once ("NetCtrl FALLBACK") instead
 *   of starting AmiBSDNet again.
 * - A watchdog process, started before the kernel, goes back after
 *   TRIAL_SECONDS without a connection.
 * - If the stack cannot start at all, it goes back at once.
 *
 * Going back restores the previous stack, takes AmiBSDNet out of the
 * boot, keeps the logs and asks for a reboot (all done by NetCtrl).
 *
 * The variable is set and deleted with GVF_SAVE_VAR, which makes SetVar()
 * write ENVARC: too (NDK3.2 Include_H/dos/var.h:52-55); DeleteVar() does
 * the same for ENVARC: (NDK3.2 ReleaseNotes/shell-RelNotes:228, 893-895:
 * UNSETENV SAVE, and the Shell leaves that work to DeleteVar()).
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

#include "rumpuser_amiga.h"
#include "stack.h"

#define	P		amiga_rump_printf
#define	TRIAL_VAR	"AmiBSDNet/Trial"
#define	TRIAL_SECONDS	180

extern struct ExecBase *SysBase;

static volatile int trial_active, fallback_started, code_running;

/* set (value) or delete (NULL) the variable, here and in ENVARC:, without
   DOS requesters ("pr_WindowPtr is -1": dos.doc ErrorReport) */
static void
trial_var(const char *value)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	int proc = me->pr_Task.tc_Node.ln_Type == NT_PROCESS;
	APTR old = NULL;

	if (proc) {
		old = me->pr_WindowPtr;
		me->pr_WindowPtr = (APTR)-1;
	}
	if (value)
		SetVar((CONST_STRPTR)TRIAL_VAR, (CONST_STRPTR)value, -1,
		    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
	else
		DeleteVar((CONST_STRPTR)TRIAL_VAR,
		    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
	if (proc)
		me->pr_WindowPtr = old;
}

/* 1 if a process started here runs this program's code (the launcher must
   then keep the program loaded) */
int
trial_code_running(void)
{

	return code_running;
}

static int
netctrl_present(void)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	BPTR l;

	me->pr_WindowPtr = (APTR)-1;
	l = Lock((CONST_STRPTR)"C:NetCtrl", ACCESS_READ);
	me->pr_WindowPtr = old;
	if (l)
		UnLock(l);
	return l != 0;
}

/*
 * The requester when there is no way back, in a process of its own so
 * whoever ran into it (the launcher at boot) goes on at once.
 * EasyRequest() switches to a stack of its own (intuition.doc
 * EasyRequestArgs NOTE), so the default stack of CreateNewProc() (4000
 * bytes, dos.doc CreateNewProc NP_StackSize) is enough.
 */
static void
no_netctrl_requester(void)
{
	struct Library *IntuitionBase;

	if ((IntuitionBase = OpenLibrary((CONST_STRPTR)"intuition.library",
	    37)) != NULL) {
		struct EasyStruct es = { sizeof(es), 0,
		    (UBYTE *)"AmiBSDNet", (UBYTE *)"AmiBSDNet could not "
		    "connect, and C:NetCtrl is missing to go back\nto the "
		    "previous TCP/IP stack. Copy C:NetCtrl from the\n"
		    "AmiBSDNet package and run \"NetCtrl FALLBACK\" (the\n"
		    "original startup files are S:*.amibsdnet-bak).\n"
		    "Until then AmiBSDNet starts normally at boot.",
		    (UBYTE *)"OK" };

		EasyRequestArgs(NULL, &es, NULL, NULL);
		CloseLibrary(IntuitionBase);
	}
}

/* "NetCtrl FALLBACK" in the background (it shows a requester itself) */
static void
run_fallback(void)
{
	BPTR in, out;

	Forbid();
	if (fallback_started) {
		Permit();
		return;
	}
	fallback_started = 1;
	Permit();
	in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
	out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
	/* (checked first: run in the background a missing command only
	   gives a Shell error nobody sees; with SYS_Asynch, System() closes
	   both handles itself and returns 0, or -1 if the command could not
	   be run: dos.doc SystemTagList) */
	if (netctrl_present() &&
	    SystemTags((CONST_STRPTR)"C:NetCtrl FALLBACK", SYS_Asynch, TRUE,
	    SYS_Input, in, SYS_Output, out, NP_StackSize, 65536,
	    TAG_DONE) == 0)
		return;
	if (in)
		Close(in);
	if (out)
		Close(out);
	P("AmiBSDNet: cannot run C:NetCtrl FALLBACK; AmiBSDNet stays in the "
	    "boot\n");
	/* no way back: rather AmiBSDNet than no network at every boot (the
	   next one starts it normally) */
	trial_var(NULL);
	if (CreateNewProcTags(NP_Entry, (ULONG)no_netctrl_requester,
	    NP_Name, (ULONG)"AmiBSDNet notice", TAG_DONE) != NULL)
		code_running = 1;
	else
		P("AmiBSDNet: cannot show the requester about C:NetCtrl\n");
}

static void
trial_watch(void)
{
	ULONG waited;

	for (waited = 0; waited < TRIAL_SECONDS * 2; waited++) {
		if (stack_connected()) {
			trial_var(NULL);
			trial_active = 0;
			P("AmiBSDNet: connected; the switch to AmiBSDNet is now "
			    "permanent\n");
			return;
		}
		Delay(25);
	}
	P("AmiBSDNet: no connection within %d seconds; going back to the "
	    "previous TCP/IP stack (NetCtrl FALLBACK)\n", TRIAL_SECONDS);
	run_fallback();
}

/*
 * Called by the launcher before the stack is started.  Returns 0 to start
 * the stack (normally, or as a trial), 1 to start it after a fallback that
 * did not finish, -1 if this is the boot after a trial that never
 * connected: the fallback is running and the stack must not start.
 */
int
trial_begin(void)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR oldwin = me->pr_WindowPtr;
	char v[8];
	LONG n;

	/* no "please insert volume ENV" requester on a system without
	   ENV: (a boot without Workbench): then there is no trial */
	me->pr_WindowPtr = (APTR)-1;
	n = GetVar((CONST_STRPTR)TRIAL_VAR, (STRPTR)v, sizeof(v),
	    GVF_GLOBAL_ONLY);
	me->pr_WindowPtr = oldwin;
	if (n < 0)
		return 0;
	if (v[0] == '2') {
		/* a trial boot ended without a connection: go back.  "3"
		   marks that this was tried (NetCtrl FALLBACK ends the trial
		   itself, whatever happens) */
		trial_var("3");
		run_fallback();
		return -1;
	}
	if (v[0] != '1') {
		/* "3": the fallback was started at the last boot and did not
		   finish (NetCtrl failed): rather AmiBSDNet than no network */
		trial_var(NULL);
		return 1;	/* (the caller says so: no log yet here) */
	}
	trial_var("2");
	trial_active = 1;
	if (CreateNewProcTags(NP_Entry, (ULONG)trial_watch,
	    NP_Name, (ULONG)"AmiBSDNet trial", NP_StackSize, 16384,
	    TAG_DONE) == NULL) {
		P("AmiBSDNet: cannot start the trial watchdog; going back\n");
		run_fallback();		/* cannot watch: do not risk it */
	} else
		code_running = 1;
	return 0;
}

/* the stack could not start: go back now if this is a trial */
void
trial_failed(void)
{

	if (trial_active)
		run_fallback();
}
