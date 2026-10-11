/*
 * What follows a kernel panic: "NetCtrl PANIC" (on the stack's own task)
 * or "NetCtrl PANICTHREAD" (on a kernel thread), src/stack/control.c,
 * src/kern/debugpanic.c.  Afterwards the stack answers no more control
 * requests (its port is gone: src/stack/control.c control_shutdown()), a
 * new library call fails with ENETDOWN (src/lib/library.c
 * bsdsocket_kernel_gone()), and CloseLibrary() returns.  Without DEBUG the
 * stack refuses the command and goes on working.
 *
 *   tools/build_amiga.sh src/test/paniktest.c build/paniktest
 *   python -I tools/run_emu.py build/paniktest --stack tests/slirp-static.conf
 *       --stackargs DEBUG --file build/NetCtrl --args TASK
 *   (the same with --args THREAD; and without --stackargs, --args NODEBUG)
 */
#include "emuenv.h"

#include <dos/rdargs.h>

#include <amibsdnet/net.h>
#include <bsdsocket_inline.h>

struct Library *SocketBase;

/* netinclude/sys/errno.h:137 */
#define	ENETDOWN	50

#define	OUT	"DH0:pt-out.txt"
#define	LOG	"DH0:rump.log"

static LONG errno_;

/* a NetCtrl command, its output in OUT; the return code */
static long
netctrl(const char *cmd)
{
	char line[96];
	long rc, len;
	char *buf;
	int i, j;
	static const char pre[] = "DH0:NetCtrl ", post[] = " >" OUT;

	for (i = 0; pre[i]; i++)
		line[i] = pre[i];
	for (j = 0; cmd[j]; j++)
		line[i++] = cmd[j];
	for (j = 0; post[j]; j++)
		line[i++] = post[j];
	line[i] = '\0';
	rc = SystemTagList((CONST_STRPTR)line, NULL);
	say("  NetCtrl ");
	say(cmd);
	say(":\n");
	if ((buf = rfile(OUT, &len)) != NULL) {
		say(buf);
		FreeVec(buf);
	}
	return rc;
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	LONG arg[1] = { 0 };
	struct RDArgs *rda;
	const char *mode = "";
	LONG s, rc;
	struct DateStamp a, b;
	int i, gone = 0;

	if (!start("paniktest"))
		return RETURN_FAIL;
	if ((rda = ReadArgs((CONST_STRPTR)"MODE/A", arg, NULL)) != NULL)
		mode = (const char *)arg[0];
	for (i = 0; i < 300 && SocketBase == NULL; i++)
		if ((SocketBase = OpenLibrary("bsdsocket.library", 4)) == NULL)
			Delay(10);
	check("OpenLibrary(\"bsdsocket.library\", 4)", SocketBase != NULL);
	if (SocketBase == NULL || rda == NULL)
		goto out;
	SetErrnoPtr(&errno_, sizeof(errno_));
	s = socket(AF_INET, SOCK_STREAM, 0);
	check("socket() before", s >= 0);
	if (s >= 0)
		CloseSocket(s);

	if (streq(mode, "NODEBUG")) {
		rc = netctrl("PANIC");
		check("without DEBUG the stack refuses it (NetCtrl: failed, "
		    "rc 10)", rc == 10 && file_has(OUT, "failed"));
		s = socket(AF_INET, SOCK_STREAM, 0);
		check("socket() still works", s >= 0);
		if (s >= 0)
			CloseSocket(s);
		goto close;
	}

	rc = netctrl(streq(mode, "TASK") ? "PANIC" : "PANICTHREAD");
	check("NetCtrl gets its answer (ok, rc 0)", rc == 0 &&
	    file_has(OUT, "ok"));
	/* rumpuser_exit() writes this line and flushes the log first
	   (src/host/rumpuser_amiga.c rumpuser_exit()) */
	for (i = 0; i < 50 && !gone; i++) {
		gone = file_has(LOG, "rumpuser: rump kernel panic");
		if (!gone)
			Delay(10);
	}
	check("the log shows the panic", gone);
	Delay(50);	/* (the stack's main loop sees it) */
	errno_ = 0;
	s = socket(AF_INET, SOCK_STREAM, 0);
	check("a new library call fails with ENETDOWN", s == -1 &&
	    errno_ == ENETDOWN);
	rc = netctrl("STATUS");
	check("the stack's port is gone (NetCtrl: not running, rc 5)",
	    rc == 5 && file_has(OUT, "AmiBSDNet is not running"));
close:
	DateStamp(&a);
	CloseLibrary(SocketBase);
	DateStamp(&b);
	check("CloseLibrary() returns (in under 5 s)",
	    (b.ds_Days - a.ds_Days) * 1440 * 3000 +
	    (b.ds_Minute - a.ds_Minute) * 3000 + b.ds_Tick - a.ds_Tick < 250);
	SocketBase = NULL;
	if (!streq(mode, "NODEBUG")) {
		/* (no new base after a panic: src/lib/library.c
		   sb_lib_open()) */
		SocketBase = OpenLibrary("bsdsocket.library", 4);
		check("OpenLibrary() afterwards fails (returns at all)",
		    SocketBase == NULL);
		if (SocketBase)
			CloseLibrary(SocketBase);
	}
out:
	if (rda)
		FreeArgs(rda);
	return finish("paniktest");
}
