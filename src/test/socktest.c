/*
 * bsdsocket.library client test: an ordinary Amiga program using the
 * AmiTCP/Roadshow API through the NDK's inline calls (the real library
 * ABI).  Run after "Run AmiBSDNet"; in WinUAE's SLIRP network the echo
 * server is 10.0.2.2:7777 (tools/run_emu.py --echo 7777).
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <amibsdnet/net.h>
#include <bsdsocket_inline.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *SocketBase;

void	*memset(void *, int, unsigned long);
int	memcmp(const void *, const void *, unsigned long);

static LONG errno_;
static int failures;

static void
say(const char *s)
{

	PutStr((CONST_STRPTR)s);
}

static void
sayn(LONG n)
{
	char b[12];
	int i = 11, neg = n < 0;
	ULONG u = neg ? -n : n;

	b[i] = '\0';
	do {
		b[--i] = '0' + u % 10;
		u /= 10;
	} while (u);
	if (neg)
		b[--i] = '-';
	say(b + i);
}

static void
check(const char *what, int ok)
{

	say(ok ? "ok   " : "FAIL ");
	say(what);
	if (!ok) {
		say(" (errno ");
		sayn(errno_);
		say(")");
		failures++;
	}
	say("\n");
}

static void
test_tcp(void)
{
	static const char msg[] = "hello through bsdsocket.library";
	struct sockaddr_in sin;
	char buf[64];
	LONG s, n, got = 0;
	ami_fd_set rfds;
	struct __timeval tv;
	ULONG sigs;

	s = socket(AF_INET, SOCK_STREAM, 0);
	check("socket()", s >= 0);
	if (s < 0)
		return;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = 7777;
	sin.sin_addr.s_addr = inet_addr((STRPTR)"10.0.2.2");
	check("connect() to 10.0.2.2:7777",
	    connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0);

	/* nothing to read yet: WaitSelect() must time out */
	AMI_FD_ZERO(&rfds);
	AMI_FD_SET(s, &rfds);
	tv.tv_secs = 0;
	tv.tv_micro = 300000;
	check("WaitSelect() timeout on an idle socket",
	    WaitSelect(s + 1, &rfds, NULL, NULL, &tv, NULL) == 0);

	check("send()", send(s, (APTR)msg, sizeof(msg), 0) == sizeof(msg));
	AMI_FD_ZERO(&rfds);
	AMI_FD_SET(s, &rfds);
	tv.tv_secs = 5;
	tv.tv_micro = 0;
	check("WaitSelect() reports data",
	    WaitSelect(s + 1, &rfds, NULL, NULL, &tv, NULL) == 1 &&
	    AMI_FD_ISSET(s, &rfds));
	while (got < (LONG)sizeof(msg)) {
		n = recv(s, buf + got, sizeof(buf) - got, 0);
		if (n <= 0)
			break;
		got += n;
	}
	check("recv() echo matches", got == sizeof(msg) &&
	    memcmp(buf, msg, sizeof(msg)) == 0);

	/* a pending Ctrl-C must interrupt a blocking recv() with EINTR */
	SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
	n = recv(s, buf, sizeof(buf), 0);
	check("Ctrl-C interrupts blocking recv() (EINTR)",
	    n == -1 && errno_ == EINTR);
	SetSignal(0, SIGBREAKF_CTRL_C);

	/* WaitSelect() woken by one of our own signals returns 0 */
	SetSignal(SIGBREAKF_CTRL_E, SIGBREAKF_CTRL_E);
	AMI_FD_ZERO(&rfds);
	AMI_FD_SET(s, &rfds);
	sigs = SIGBREAKF_CTRL_E;
	tv.tv_secs = 10;
	tv.tv_micro = 0;
	n = WaitSelect(s + 1, &rfds, NULL, NULL, &tv, &sigs);
	check("WaitSelect() returns on a user signal",
	    n == 0 && sigs == SIGBREAKF_CTRL_E);

	check("CloseSocket()", CloseSocket(s) == 0);
}

static void
test_resolver(void)
{
	struct hostent *h;
	struct addrinfo *ai = NULL;

	h = gethostbyname((STRPTR)"aminet.net");
	check("gethostbyname(\"aminet.net\") via DNS", h != NULL &&
	    h->h_length == 4 && h->h_addr_list[0] != NULL);
	if (h) {
		say("     aminet.net = ");
		say((const char *)Inet_NtoA(*(ULONG *)h->h_addr_list[0]));
		say("\n");
	}
	h = gethostbyname((STRPTR)"127.0.0.1");
	check("gethostbyname() numeric", h != NULL &&
	    *(ULONG *)h->h_addr_list[0] == INADDR_LOOPBACK);
	check("getaddrinfo(\"localhost\", \"http\")",
	    getaddrinfo((CONST_STRPTR)"localhost", (CONST_STRPTR)"http",
	    NULL, &ai) == 0 && ai &&
	    ((struct sockaddr_in *)ai->ai_addr)->sin_port == 80);
	if (ai)
		freeaddrinfo(ai);
}

static void
test_misc(void)
{
	char name[64];

	check("inet_addr() / Inet_NtoA() round trip",
	    inet_addr((STRPTR)"192.168.10.7") == 0xc0a80a07UL);
	check("gethostname()", gethostname((STRPTR)name, sizeof(name)) == 0);
	say("     hostname: ");
	say(name);
	say("\n");
	check("getdtablesize() > 0", getdtablesize() > 0);
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	BPTR f;
	int i;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	if (DOSBase == NULL)
		return RETURN_FAIL;

	/* the stack may still be starting */
	for (i = 0; i < 300 && SocketBase == NULL; i++) {
		SocketBase = OpenLibrary("bsdsocket.library", 4);
		if (SocketBase == NULL)
			Delay(10);
	}
	check("OpenLibrary(\"bsdsocket.library\", 4)", SocketBase != NULL);
	if (SocketBase) {
		SetErrnoPtr(&errno_, sizeof(errno_));
		say("     ");
		say((const char *)SocketBase->lib_IdString);
		test_misc();
		test_resolver();
		test_tcp();
		CloseLibrary(SocketBase);
	}
	say(failures ? "socktest: FAILED\n" : "socktest: PASS\n");
	if ((f = Open((CONST_STRPTR)"DH0:done", MODE_NEWFILE))) {
		Write(f, failures ? "FAIL\n" : "PASS\n", 5);
		Close(f);
	}
	CloseLibrary((struct Library *)DOSBase);
	return failures ? RETURN_ERROR : RETURN_OK;
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
