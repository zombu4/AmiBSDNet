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
#include <proto/exec.h>
#include <proto/dos.h>

#include "rumpuser_amiga.h"
#include "stack.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

int	rump_init(void);
struct Library *bsdsocket_create(void);

#define	DEFAULT_CONFIG	"ENV:AmiBSDNet/AmiBSDNet.conf"
#define	FALLBACK_CONFIG	"ENVARC:AmiBSDNet/AmiBSDNet.conf"
#define	DEFAULT_LOG	"T:AmiBSDNet.log"
#define	VERSTAG		"\0$VER: AmiBSDNet 0.1 (09.10.2026)"

static const char verstag[] __attribute__((used)) = VERSTAG;

static struct Task *launcher;
static volatile int startup_result = -1;
static char cfgpath[256] = DEFAULT_CONFIG;
static char logpath[256] = DEFAULT_LOG;

#define	P	amiga_rump_printf

static void
stack_main(void)
{
	BPTR log;
	int rv;

	((struct Process *)SysBase->ThisTask)->pr_WindowPtr = (APTR)-1;
	log = Open((CONST_STRPTR)logpath, MODE_NEWFILE);
	amiga_rump_loginit((long)log);
	crash_install();
	if (amiga_rump_hostinit(0) != 0)
		goto fail;
	P("AmiBSDNet 0.1 starting\n");

	if ((rv = rump_init()) != 0) {
		P("AmiBSDNet: kernel failed to start (%d)\n", rv);
		goto fail;
	}
	if (stack_configure(cfgpath) != 0 &&
	    stack_configure(FALLBACK_CONFIG) != 0)
		P("AmiBSDNet: no configuration found, loopback only\n");
	if (bsdsocket_create() == NULL) {
		P("AmiBSDNet: cannot create bsdsocket.library\n");
		goto fail;
	}
	if (control_init() != 0)
		P("AmiBSDNet: no control port\n");
	P("AmiBSDNet: bsdsocket.library ready\n");
	startup_result = 0;
	Signal(launcher, SIGBREAKF_CTRL_F);

	for (;;) {
		ULONG s = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D |
		    control_sigmask());

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
	Signal(launcher, SIGBREAKF_CTRL_F);
	/* kernel threads may exist: never return into unloaded code */
	for (;;)
		Wait(0x80000000UL);
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct CommandLineInterface *cli;
	struct RDArgs *rda;
	LONG argv[3] = { 0, 0, 0 };
	BPTR out;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	if (DOSBase == NULL)
		return RETURN_FAIL;
	out = Output();

	if (FindName(&SysBase->LibList, "bsdsocket.library")) {
		if (out)
			PutStr((CONST_STRPTR)"AmiBSDNet: a TCP/IP stack is "
			    "already running\n");
		CloseLibrary((struct Library *)DOSBase);
		return RETURN_WARN;
	}
	if ((rda = ReadArgs((CONST_STRPTR)"CONFIG/K,LOG/K,DEBUG/S", argv,
	    NULL)) != NULL) {
		if (argv[0])
			sb_copy(cfgpath, (const char *)argv[0], sizeof(cfgpath));
		if (argv[1])
			sb_copy(logpath, (const char *)argv[1], sizeof(logpath));
		amiga_rump_debug = argv[2] ? 1 : 0;
		FreeArgs(rda);
	}

	for (void (**c)(void) = __init_array_start; c < __init_array_end; c++)
		(*c)();

	launcher = SysBase->ThisTask;
	if (CreateNewProcTags(NP_Entry, (ULONG)stack_main,
	    NP_Name, (ULONG)"AmiBSDNet", NP_StackSize, 256 * 1024,
	    NP_Priority, 1, TAG_DONE) == NULL)
		return RETURN_FAIL;
	Wait(SIGBREAKF_CTRL_F);
	if (startup_result != 0) {
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
	return startup_result == 0 ? RETURN_OK : RETURN_FAIL;
}
