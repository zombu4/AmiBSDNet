/*
 * Debug aid: a tc_TrapCode handler that turns a CPU exception in a rump
 * thread into a report on the rump console instead of a Software Failure
 * requester.  It records the trap number and faulting PC, then redirects
 * the exception's return PC to crash_report(), which runs in the faulting
 * task, prints the PC and every stack word that looks like a return
 * address into our code (offsets are ELF addresses, so they can be looked
 * up in the link map), and then stops the task.
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <proto/exec.h>

#include "rumpuser_amiga.h"

extern struct ExecBase *SysBase;
extern char _start[];		/* ELF address 0: our load base */
extern char _etext[];

volatile ULONG crash_trapnum;
volatile ULONG crash_pc;
volatile ULONG crash_sp;

void crash_report(void);
void crash_trap_handler(void);

/*
 * On entry (supervisor mode): (sp) = trap number (long), then the
 * exception frame: SR.w, PC.l, format/vector.w, ...
 */
__asm__(
"	.text\n"
"	.globl	crash_trap_handler\n"
"crash_trap_handler:\n"
"	move.l	(%sp)+,crash_trapnum\n"
"	move.l	2(%sp),crash_pc\n"
"	move.l	%a0,-(%sp)\n"
"	move.l	%usp,%a0\n"
"	move.l	%a0,crash_sp\n"
"	move.l	(%sp)+,%a0\n"
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
	ULONG base = (ULONG)_start, end = (ULONG)_etext;
	ULONG *sp = (ULONG *)crash_sp;
	int i, n = 0;

	amiga_rump_printf("\n*** CPU %s (trap %lu) in task \"%s\"\n",
	    trapname(crash_trapnum), crash_trapnum,
	    SysBase->ThisTask->tc_Node.ln_Name);
	if (crash_pc >= base && crash_pc < end)
		amiga_rump_printf("*** PC = %08lx (ELF %08lx)\n", crash_pc,
		    crash_pc - base);
	else
		amiga_rump_printf("*** PC = %08lx (outside our code; text is "
		    "%08lx-%08lx)\n", crash_pc, base, end);
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

	SysBase->ThisTask->tc_TrapCode = (APTR)crash_trap_handler;
}
