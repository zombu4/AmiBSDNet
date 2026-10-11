/*
 * First boot test: start the NetBSD rump kernel under AmigaOS and move
 * TCP data over lo0 (127.0.0.1).  Output goes to the Shell's Output();
 * DH0:done is written at the end because rump threads keep running, so
 * the program cannot return to the Shell.
 *
 *   tools/link_test.sh src/test/rumptest.c build/rumptest
 *   python -I tools/run_emu.py build/rumptest
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "rumpuser_amiga.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

/* rump kernel entry points (see rump/rump.h, rump/rump_syscalls.h) */
int rump_init(void);
int rump___sysimpl_socket30(int, int, int);
int rump___sysimpl_bind(int, const void *, unsigned int);
int rump___sysimpl_listen(int, int);
int rump___sysimpl_connect(int, const void *, unsigned int);
int rump___sysimpl_accept(int, void *, unsigned int *);
long rump___sysimpl_sendto(int, const void *, unsigned long, int,
    const void *, unsigned int);
long rump___sysimpl_recvfrom(int, void *, unsigned long, int, void *,
    unsigned int *);
int rump___sysimpl_close(int);

/* NetBSD's struct sockaddr_in (note sin_len); m68k is big-endian */
struct nb_sockaddr_in {
	uint8_t sin_len;
	uint8_t sin_family;
	uint16_t sin_port;
	uint32_t sin_addr;
	uint8_t sin_zero[8];
};
#define	NB_AF_INET	2
#define	NB_SOCK_STREAM	1

extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

int _start(void);
static struct Task *parent;
static volatile int result = 20;

#define	P	amiga_rump_printf

static int
fail(const char *what)
{

	P("FAIL: %s (errno %d)\n", what, amiga_rump_errno());
	return 10;
}

static int
same(const void *a, const void *b, long n)
{
	const unsigned char *x = a, *y = b;

	while (n-- > 0)
		if (*x++ != *y++)
			return 0;
	return 1;
}

static int
tcp_loopback_test(void)
{
	static const char msg[] = "hello from NetBSD TCP on AmigaOS";
	struct nb_sockaddr_in sin;
	char buf[64];
	unsigned int slen;
	int ls, cs, as;
	long n, got;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = NB_AF_INET;
	sin.sin_port = 7777;
	sin.sin_addr = 0x7f000001;	/* 127.0.0.1 */

	if ((ls = rump___sysimpl_socket30(NB_AF_INET, NB_SOCK_STREAM, 0)) < 0)
		return fail("socket (listen)");
	if (rump___sysimpl_bind(ls, &sin, sizeof(sin)) < 0)
		return fail("bind");
	if (rump___sysimpl_listen(ls, 5) < 0)
		return fail("listen");
	P("listening on 127.0.0.1:7777\n");

	if ((cs = rump___sysimpl_socket30(NB_AF_INET, NB_SOCK_STREAM, 0)) < 0)
		return fail("socket (client)");
	if (rump___sysimpl_connect(cs, &sin, sizeof(sin)) < 0)
		return fail("connect");
	P("connected\n");

	slen = sizeof(sin);
	if ((as = rump___sysimpl_accept(ls, &sin, &slen)) < 0)
		return fail("accept");
	P("accepted connection from port %u\n", (unsigned)sin.sin_port);

	if (rump___sysimpl_sendto(cs, msg, sizeof(msg), 0, NULL, 0) !=
	    (long)sizeof(msg))
		return fail("send");
	/* TCP may deliver it in parts */
	for (got = 0; got < (long)sizeof(msg); got += n)
		if ((n = rump___sysimpl_recvfrom(as, buf + got,
		    sizeof(buf) - got, 0, NULL, NULL)) <= 0)
			return fail("recv");
	if (got != (long)sizeof(msg) || !same(buf, msg, sizeof(msg))) {
		P("FAIL: received %ld bytes that differ from what was sent\n",
		    got);
		return 10;
	}
	P("received %ld bytes: \"%s\"\n", got, buf);

	rump___sysimpl_close(as);
	rump___sysimpl_close(cs);
	rump___sysimpl_close(ls);
	return 0;
}

static void
test_proc(void)
{
	int rv;

	crash_install();
	if ((rv = amiga_rump_hostinit(0)) != 0) {
		result = 30;
		goto out;
	}
	amiga_rump_notifytask = parent;
	P("rumptest: starting NetBSD rump kernel\n");
	if ((rv = rump_init()) != 0) {
		P("FAIL: rump_init returned %d\n", rv);
		result = 20;
		goto out;
	}
	P("rump_init: ok\n");
	result = tcp_loopback_test();
	P(result == 0 ? "PASS\n" : "test failed\n");
out:
	Forbid();
	Signal(parent, SIGBREAKF_CTRL_F);
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	BPTR f, log;
	ULONG sigs;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	if (DOSBase == NULL)
		return 20;
	parent = SysBase->ThisTask;

	/* logging first, so crash reports work even from constructors */
	amiga_rump_debug = 1;
	log = Open((CONST_STRPTR)"DH0:rump.log", MODE_NEWFILE);
	amiga_rump_loginit((long)(log ? log : Output()));
	amiga_rump_printf("rumptest: loaded at %p\n", (void *)_start);
	crash_install();

	/* RUMP_USE_CTOR: rump components register from constructors */
	for (void (**c)(void) = __init_array_start; c < __init_array_end; c++)
		(*c)();

	if (CreateNewProcTags(NP_Entry, (ULONG)test_proc,
	    NP_Name, (ULONG)"rump-kernel-test", NP_StackSize, 256 * 1024,
	    NP_Output, (ULONG)Output(), NP_CloseOutput, FALSE,
	    TAG_DONE) == NULL)
		return 20;

	sigs = Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
	if (sigs & SIGBREAKF_CTRL_C)
		P("rump kernel exited (code %d)\n", amiga_rump_exitcode);
	Delay(10);	/* let the log drain */

	if ((f = Open((CONST_STRPTR)"DH0:done", MODE_NEWFILE))) {
		Write(f, result == 0 ? "PASS\n" : "FAIL\n", 5);
		Close(f);
	}
	/* rump kernel threads are still running: never unload this code */
	for (;;)
		Wait(0x80000000UL);
}
