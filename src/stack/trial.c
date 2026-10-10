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
 * Going back restores the previous stack and Wi-Fi driver, takes
 * AmiBSDNet out of the boot, keeps the logs and asks for a reboot.
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

static volatile int trial_active, fallback_started;

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
	if (SystemTags((CONST_STRPTR)"C:NetCtrl FALLBACK", SYS_Asynch, TRUE,
	    SYS_Input, in, SYS_Output, out, NP_StackSize, 65536,
	    TAG_DONE) != 0) {
		struct Library *IntuitionBase;

		if (in)
			Close(in);
		if (out)
			Close(out);
		/* without NetCtrl at least say what to do */
		if ((IntuitionBase = OpenLibrary((CONST_STRPTR)
		    "intuition.library", 37)) != NULL) {
			struct EasyStruct es = { sizeof(es), 0,
			    (UBYTE *)"AmiBSDNet", (UBYTE *)"AmiBSDNet could not "
			    "connect, and C:NetCtrl is missing to go back\nto the "
			    "previous TCP/IP stack. Copy S:User-Startup."
			    "amibsdnet-bak\nto S:User-Startup and reboot.",
			    (UBYTE *)"OK" };

			EasyRequestArgs(NULL, &es, NULL, NULL);
			CloseLibrary(IntuitionBase);
		}
	}
}

static void
trial_watch(void)
{
	ULONG waited;

	for (waited = 0; waited < TRIAL_SECONDS * 2; waited++) {
		if (stack_connected()) {
			DeleteVar((CONST_STRPTR)TRIAL_VAR,
			    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
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
 * the stack (normally, or as a trial), -1 if this is the boot after a
 * trial that never connected: the fallback is running and the stack must
 * not start.
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
	if (v[0] != '1') {
		/* a trial boot ended without a connection */
		run_fallback();
		return -1;
	}
	SetVar((CONST_STRPTR)TRIAL_VAR, (CONST_STRPTR)"2", -1,
	    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
	trial_active = 1;
	if (CreateNewProcTags(NP_Entry, (ULONG)trial_watch,
	    NP_Name, (ULONG)"AmiBSDNet trial", NP_StackSize, 16384,
	    TAG_DONE) == NULL)
		run_fallback();		/* cannot watch: do not risk it */
	return 0;
}

/* the stack could not start: go back now if this is a trial */
void
trial_failed(void)
{

	if (trial_active)
		run_fallback();
}
