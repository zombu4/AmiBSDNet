/*
 * A kernel panic on request, for testing what the stack does after one.
 * panic() ends in the rump kernel's cpu_reboot(), which calls
 * rumpuser_exit(RUMPUSER_PANIC) (netbsd-src/sys/kern/subr_prf.c:214-288
 * vpanic() kern_reboot(); netbsd-src/sys/rump/librump/rumpkern/
 * emul.c:410-448).
 *
 * Called from host code; schedules itself.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kthread.h>

#include <rump/rump.h>

int	rump_amibsdnet_panic(int);

static void
panic_thread(void *arg)
{

	panic("panic test on a kernel thread");
}

/*
 * on_thread 0: panic on the calling thread (does not return); 1: on a
 * new kernel thread (returns 0 once it is created, or the error)
 */
int
rump_amibsdnet_panic(int on_thread)
{
	int error;

	rump_schedule();
	if (!on_thread)
		panic("panic test on the calling thread");
	error = kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, panic_thread,
	    NULL, NULL, "panictest");
	rump_unschedule();
	return error;
}
