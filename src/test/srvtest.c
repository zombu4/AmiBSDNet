/*
 * Server and concurrency test for bsdsocket.library.
 *
 * Listens on 0.0.0.0:2323 (reachable from the emulator host through
 * tools/run_emu.py --forward 2323) and on 127.0.0.1:2324.  A child
 * process with its own library base connects to 2324 while the main
 * process is serving, so two bases are active at once.  Every accepted
 * connection gets its data echoed.  Passes when both the host's and the
 * child's connections were echoed correctly (tools/run_emu.py checks the
 * host's echo).
 *
 *   tools/build_stack.sh
 *   tools/build_amiga.sh src/test/srvtest.c build/srvtest
 *   python -I tools/run_emu.py build/srvtest --stack tests/slirp.conf
 *       --forward 2323
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <amibsdnet/net.h>
#include <bsdsocket_inline.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *SocketBase;	/* the main process's base */

void	*memset(void *, int, unsigned long);
int	memcmp(const void *, const void *, unsigned long);

static struct Task *mainproc;
static volatile int child_ok = -1;
static const char childmsg[] = "child process, second library base";

#define	SO_REUSEADDR	0x0004

static LONG
listener(struct Library *SocketBase, ULONG addr, UWORD port)
{
	struct sockaddr_in sin;
	LONG s, on = 1;

	if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = port;
	sin.sin_addr.s_addr = addr;
	if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0 ||
	    listen(s, 4) < 0) {
		CloseSocket(s);
		return -1;
	}
	return s;
}

/* the child: its own SocketBase, a client of the local listener */
static void
child(void)
{
	struct Library *SocketBase;
	struct sockaddr_in sin;
	char buf[64];
	LONG s, n, got = 0;

	child_ok = 0;
	if ((SocketBase = OpenLibrary("bsdsocket.library", 4)) != NULL) {
		if ((s = socket(AF_INET, SOCK_STREAM, 0)) >= 0) {
			memset(&sin, 0, sizeof(sin));
			sin.sin_len = sizeof(sin);
			sin.sin_family = AF_INET;
			sin.sin_port = 2324;
			sin.sin_addr.s_addr = INADDR_LOOPBACK;
			if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0 &&
			    send(s, (APTR)childmsg, sizeof(childmsg), 0) ==
			    sizeof(childmsg)) {
				while (got < (LONG)sizeof(childmsg) &&
				    (n = recv(s, buf + got, sizeof(buf) - got,
				    0)) > 0)
					got += n;
				child_ok = got == sizeof(childmsg) &&
				    memcmp(buf, childmsg, got) == 0;
			}
			CloseSocket(s);
		}
		CloseLibrary(SocketBase);
	}
	Forbid();
	Signal(mainproc, SIGBREAKF_CTRL_F);
}

/* echo one accepted connection; returns bytes echoed */
static LONG
serve(LONG ls)
{
	struct sockaddr_in peer;
	socklen_t plen = sizeof(peer);
	char buf[256];
	LONG c, n, total = 0;

	if ((c = accept(ls, (struct sockaddr *)&peer, &plen)) < 0)
		return -1;
	PutStr((CONST_STRPTR)"     connection from ");
	PutStr((CONST_STRPTR)Inet_NtoA(peer.sin_addr.s_addr));
	PutStr((CONST_STRPTR)"\n");
	/* echo what arrives within a short time */
	for (;;) {
		ami_fd_set r;
		struct __timeval tv = { 2, 0 };

		AMI_FD_ZERO(&r);
		AMI_FD_SET(c, &r);
		if (WaitSelect(c + 1, &r, NULL, NULL, &tv, NULL) <= 0)
			break;
		if ((n = recv(c, buf, sizeof(buf), 0)) <= 0)
			break;
		send(c, buf, n, 0);
		total += n;
	}
	CloseSocket(c);
	return total;
}

static void
say(const char *s)
{

	PutStr((CONST_STRPTR)s);
}

static void
done(int fail)
{
	BPTR f;

	say(fail ? "srvtest: FAILED\n" : "srvtest: PASS\n");
	if ((f = Open((CONST_STRPTR)"DH0:done", MODE_NEWFILE))) {
		Write(f, fail ? "FAIL\n" : "PASS\n", 5);
		Close(f);
	}
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	LONG ext, loc, maxfd, n;
	int host_done = 0, child_done = 0, waits = 0, fail = 0, started = 0;
	int i;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	if (DOSBase == NULL)
		return RETURN_FAIL;
	if ((SocketBase = OpenLibrary("bsdsocket.library", 4)) == NULL) {
		say("FAIL OpenLibrary(\"bsdsocket.library\", 4)\n");
		done(1);
		CloseLibrary((struct Library *)DOSBase);
		return RETURN_FAIL;
	}
	mainproc = SysBase->ThisTask;

	ext = listener(SocketBase, 0, 2323);
	loc = listener(SocketBase, INADDR_LOOPBACK, 2324);
	say(ext >= 0 && loc >= 0 ? "ok   listening on :2323 and 127.0.0.1:2324\n" :
	    "FAIL listen\n");
	if (ext < 0 || loc < 0)
		fail++;
	else if (CreateNewProcTags(NP_Entry, (ULONG)child, NP_Name,
	    (ULONG)"srvtest child", NP_StackSize, 16384, TAG_DONE) == NULL) {
		say("FAIL child process\n");
		fail++;
	} else
		started = 1;
	maxfd = (ext > loc ? ext : loc) + 1;

	while (!fail && (!host_done || !child_done) && waits < 60) {
		ami_fd_set r;
		struct __timeval tv = { 1, 0 };

		AMI_FD_ZERO(&r);
		if (!host_done)
			AMI_FD_SET(ext, &r);
		if (!child_done)
			AMI_FD_SET(loc, &r);
		n = WaitSelect(maxfd, &r, NULL, NULL, &tv, NULL);
		if (n < 0) {
			say("FAIL WaitSelect\n");
			fail++;
			break;
		}
		if (n == 0) {
			waits++;
			continue;
		}
		if (!host_done && AMI_FD_ISSET(ext, &r)) {
			n = serve(ext);
			say(n > 0 ? "ok   echoed the host's connection\n" :
			    "FAIL host connection\n");
			if (n <= 0)
				fail++;
			host_done = 1;
		}
		if (!child_done && AMI_FD_ISSET(loc, &r)) {
			n = serve(loc);
			say(n == sizeof(childmsg) ?
			    "ok   echoed the child base's connection\n" :
			    "FAIL child connection\n");
			if (n != sizeof(childmsg))
				fail++;
			child_done = 1;
		}
	}
	if (!fail && (!host_done || !child_done)) {
		say("FAIL timed out waiting for connections\n");
		fail++;
	}
	/* closing the listeners aborts a connection still waiting to be
	   accepted (netbsd-src/sys/kern/uipc_socket.c soclose(): soabort();
	   netinet/tcp_subr.c tcp_drop() sends the reset), so a child
	   waiting for its echo gets an error instead of waiting for ever */
	if (ext >= 0)
		CloseSocket(ext);
	if (loc >= 0)
		CloseSocket(loc);
	if (started) {
		/* the child signals when it ends: up to 10 seconds */
		for (i = 0; i < 100 && !(SetSignal(0, 0) & SIGBREAKF_CTRL_F);
		    i++)
			Delay(5);
		if (!(SetSignal(0, SIGBREAKF_CTRL_F) & SIGBREAKF_CTRL_F)) {
			say("FAIL the child process did not end\n");
			done(1);
			/* its code is this program's: never unload it */
			for (;;)
				Wait(0x80000000UL);
		}
		say(child_ok == 1 ? "ok   child base got its echo\n" :
		    "FAIL child base\n");
		if (child_ok != 1)
			fail++;
	}
	CloseLibrary(SocketBase);
	done(fail);
	CloseLibrary((struct Library *)DOSBase);
	return fail ? RETURN_ERROR : RETURN_OK;
}

void *
memset(void *d, int c, unsigned long n)
{
	char *p = d;

	while (n--)
		*p++ = c;
	return d;
}

int
memcmp(const void *a, const void *b, unsigned long n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}
