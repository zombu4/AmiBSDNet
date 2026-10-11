/*
 * Debug aid: a tc_TrapCode handler that turns a CPU exception in a rump
 * thread into a report on the rump console instead of a Software Failure
 * requester.  It records the trap number, faulting PC and user stack
 * pointer, then redirects the exception's return PC to crash_report(),
 * which runs in the faulting task, prints the PC and every stack word
 * that looks like a return address into our code (offsets are ELF
 * addresses, so they can be looked up in the link map), and then stops
 * the task.
 *
 * The record is per task: it hangs off the task's tc_TrapData, which
 * Exec does not use (exec.doc AllocTrap, :1157-1166: "Traps are sent to
 * the trap handler pointed at by tc_TrapCode ... 0(SP) = Exception vector
 * number ... 4(SP) = ... exception frame ... tc_TrapData is not used"),
 * so two tasks that fault at the same time do not overwrite each other's.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <stddef.h>

#include "rumpuser_amiga.h"

extern struct ExecBase *SysBase;
extern char _start[];		/* ELF address 0: our load base */
extern char _etext[];

struct crashinfo {
	ULONG	trapnum;		/* offset 0, written by the handler */
	ULONG	pc;			/* offset 4 */
	ULONG	sp;			/* offset 8 */
	APTR	oldcode;		/* what tc_TrapCode was */
	APTR	olddata;		/* what tc_TrapData was */
};

void crash_report(void);
void crash_trap_handler(void);

/* the handler below uses these offsets as constants */
_Static_assert(offsetof(struct ExecBase, ThisTask) == 276,
    "ExecBase.ThisTask offset");
_Static_assert(offsetof(struct Task, tc_TrapData) == 46,
    "Task.tc_TrapData offset");
_Static_assert(offsetof(struct crashinfo, trapnum) == 0 &&
    offsetof(struct crashinfo, pc) == 4 &&
    offsetof(struct crashinfo, sp) == 8, "crashinfo layout");

/*
 * On entry (supervisor mode): (sp) = trap number (long), then the
 * exception frame: SR.w, PC.l, format/vector.w, ...  After the movem
 * below: 8(sp) trap number, 12(sp) SR, 14(sp) PC.
 */
__asm__(
"	.text\n"
"	.globl	crash_trap_handler\n"
"crash_trap_handler:\n"
"	movem.l	%a0-%a1,-(%sp)\n"
"	move.l	4.w,%a0\n"
"	move.l	276(%a0),%a0\n"		/* SysBase->ThisTask */
"	move.l	46(%a0),%a0\n"		/* ->tc_TrapData */
"	move.l	8(%sp),(%a0)\n"
"	move.l	14(%sp),4(%a0)\n"
"	move.l	%usp,%a1\n"
"	move.l	%a1,8(%a0)\n"
"	movem.l	(%sp)+,%a0-%a1\n"
"	addq.l	#4,%sp\n"
"	move.l	#crash_report,2(%sp)\n"
"	rte\n");

static const char *
trapname(ULONG n)
{

	switch (n) {
	case 2: return "bus error";
	case 3: return "address error";
	case 4: return "illegal instruction";
	case 5: return "division by zero";
	case 6: return "CHK";
	case 7: return "TRAPV";
	case 8: return "privilege violation";
	case 10: return "line-A";
	case 11: return "line-F";
	default: return "exception";
	}
}

void
crash_report(void)
{
	struct crashinfo *ci = SysBase->ThisTask->tc_TrapData;
	ULONG base = (ULONG)_start, end = (ULONG)_etext;
	ULONG *sp = (ULONG *)ci->sp;
	int i, n = 0;

	amiga_rump_printf("\n*** CPU %s (trap %lu) in task \"%s\"\n",
	    trapname(ci->trapnum), ci->trapnum,
	    SysBase->ThisTask->tc_Node.ln_Name);
	if (ci->pc >= base && ci->pc < end)
		amiga_rump_printf("*** PC = %08lx (ELF %08lx)\n", ci->pc,
		    ci->pc - base);
	else
		amiga_rump_printf("*** PC = %08lx (outside our code; text is "
		    "%08lx-%08lx)\n", ci->pc, base, end);
	amiga_rump_printf("*** SP = %08lx, possible return addresses (ELF):\n",
	    (ULONG)sp);
	for (i = 0; i < 2048 && n < 24; i++) {
		ULONG v = sp[i];

		if (v > base && v < end && !(v & 1)) {
			amiga_rump_printf("***   sp+%04x: %08lx\n", i * 4, v - base);
			n++;
		}
	}
	amiga_rump_printf("*** end of report\n");
	rumpuser_exit(RUMPUSER_PANIC);
}

void
crash_install(void)
{
	struct Task *me = SysBase->ThisTask;
	struct crashinfo *ci;

	if (me->tc_TrapCode == (APTR)crash_trap_handler)
		return;			/* already */
	/* (MEMF_PUBLIC: written from the exception handler, exec.doc
	   AllocMem) */
	if ((ci = AllocVec(sizeof(*ci), MEMF_PUBLIC | MEMF_CLEAR)) == NULL) {
		amiga_rump_printf("crashtrap: no memory; exceptions in \"%s\" "
		    "are not reported\n", me->tc_Node.ln_Name);
		return;
	}
	ci->oldcode = me->tc_TrapCode;
	ci->olddata = me->tc_TrapData;
	Disable();		/* the handler reads both */
	me->tc_TrapData = ci;
	me->tc_TrapCode = (APTR)crash_trap_handler;
	Enable();
}

void
crash_remove(void)
{
	struct Task *me = SysBase->ThisTask;
	struct crashinfo *ci;

	if (me->tc_TrapCode != (APTR)crash_trap_handler)
		return;
	ci = me->tc_TrapData;
	Disable();
	me->tc_TrapCode = ci->oldcode;
	me->tc_TrapData = ci->olddata;
	Enable();
	FreeVec(ci);
}
