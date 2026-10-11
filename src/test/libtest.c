/*
 * bsdsocket.library behaviour test, for the paths socktest does not reach:
 * socket state carried through accept(), ObtainSocket() and Dup2Socket(),
 * timeouts as getsockopt() reports them, SO_LINGER, SIGURG, events on a
 * routing socket, interface times, the SocketBaseTagList() string and ICMP
 * tags, the log and monitoring hooks, the mbuf range checks, the resolver
 * search list, and a library base shared by two tasks.
 *
 * Values are from the Roadshow SDK headers,
 * downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/netinclude (file:line at
 * each), behaviour from its autodoc doc/bsdsocket.doc and the catalog
 * locale/bsdsocket.cd.
 *
 *   tools/build_stack.sh
 *   tools/build_amiga.sh src/test/libtest.c build/libtest
 *   python -I tools/run_emu.py build/libtest --stack tests/slirp-static.conf
 *       --echo 7777 --dns 53
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <amibsdnet/net.h>
#include <bsdsocket_inline.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *SocketBase;

void	*memset(void *, int, unsigned long);
int	memcmp(const void *, const void *, unsigned long);

/* sys/socket.h:118,132,143,152,189,296,303 */
#define	SOCK_RAW	3
#define	SO_LINGER	0x0080
#define	SO_SNDTIMEO	0x1005
#define	SO_EVENTMASK	0x2001
#define	AF_ROUTE	17
#define	MSG_OOB		0x1
#define	MSG_DONTWAIT	0x80
/* libraries/bsdsocket.h:74-96 */
#define	SBTF_REF	0x8000
#define	SBTF_SET	1
#define	SBTB_CODE	1
#define	SBTS_CODE	0x3fff
#define	SBTM_GETREF(c)	(TAG_USER | SBTF_REF | (((c) & SBTS_CODE) << SBTB_CODE))
#define	SBTM_SETVAL(c)	(TAG_USER | (((c) & SBTS_CODE) << SBTB_CODE) | SBTF_SET)
/* libraries/bsdsocket.h:110,138-140,187,197,210,250 */
#define	SBTC_SIGURGMASK		3
#define	SBTC_IOERRNOSTRPTR	16
#define	SBTC_S2ERRNOSTRPTR	17
#define	SBTC_S2WERRNOSTRPTR	18
#define	SBTC_ICMP_PROCESS_ECHO	48
#define	SBTC_CAN_SHARE_LIBRARY_BASES 51
#define	SBTC_LOG_HOOK		55
#define	SBTC_IDN_DEFAULT_CHARACTER_SET 66
/* libraries/bsdsocket.h:324-325,336,347,358-364,507,538,693 */
#define	IR_Process	0
#define	IR_Ignore	1
#define	UNIQUE_ID	(-1)
#define	FD_READ		0x08
#define	FD_CLOSE	0x40	/* netinclude/libraries/bsdsocket.h:350 */
#define	RTA_BASE	(TAG_USER + 1600)
#define	RTA_Gateway	(RTA_BASE + 2)
#define	RTA_DestinationHost (RTA_BASE + 4)
#define	IFQ_LastStart	(TAG_USER + 1900 + 13)
#define	MHT_Connect	3
/* sys/syslog.h:64 */
#define	LOG_ERR		3
/* errno values: sys/errno.h */
#define	EBADF		9
#define	EINVAL		22

struct linger { LONG l_onoff; LONG l_linger; };

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

static int
streq_(const char *a, const char *b)
{

	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* 1/50 s since 1978 (dos.doc DateStamp: days, minutes, ticks) */
static ULONG
ticks(void)
{
	struct DateStamp ds;

	DateStamp(&ds);
	return ((ULONG)ds.ds_Days * 1440 + ds.ds_Minute) * 3000 + ds.ds_Tick;
}

/* a listening TCP socket on 127.0.0.1, its port in *port */
static LONG
listener(UWORD *port)
{
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	LONG s = socket(AF_INET, SOCK_STREAM, 0);

	if (s < 0)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_LOOPBACK;
	if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) != 0 ||
	    listen(s, 4) != 0 ||
	    getsockname(s, (struct sockaddr *)&sin, &len) != 0) {
		CloseSocket(s);
		return -1;
	}
	*port = sin.sin_port;
	return s;
}

static LONG
connect_to(ULONG addr, UWORD port)
{
	struct sockaddr_in sin;
	LONG s = socket(AF_INET, SOCK_STREAM, 0);

	if (s < 0)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = port;
	sin.sin_addr.s_addr = addr;
	if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
		CloseSocket(s);
		return -1;
	}
	return s;
}

static int
get_timeo(LONG s, LONG name, ULONG *secs, ULONG *micro)
{
	struct __timeval tv;
	socklen_t len = sizeof(tv);

	if (getsockopt(s, SOL_SOCKET, name, &tv, &len) != 0)
		return -1;
	*secs = tv.tv_secs;
	*micro = tv.tv_micro;
	return 0;
}

static int
set_timeo(LONG s, LONG name, ULONG secs, ULONG micro)
{
	struct __timeval tv;

	tv.tv_secs = secs;
	tv.tv_micro = micro;
	return setsockopt(s, SOL_SOCKET, name, &tv, sizeof(tv));
}

/* ------------------------------------------------------------------------
 * socket state
 */

static void
test_state(void)
{
	UWORD port;
	LONG l, c, a, t, id, o, on;
	ULONG s, us, t0;
	char b[4];

	/* getsockopt() gives the timeout the library waits with: whole
	   milliseconds, rounded up (src/lib/library.c sbtime_from_tv()) */
	l = listener(&port);
	check("listener on 127.0.0.1", l >= 0);
	if (l < 0)
		return;
	check("SO_RCVTIMEO 1.234567 s set",
	    set_timeo(l, SO_RCVTIMEO, 1, 234567) == 0);
	check("getsockopt(SO_RCVTIMEO) = 1.235000 s",
	    get_timeo(l, SO_RCVTIMEO, &s, &us) == 0 && s == 1 &&
	    us == 235000);
	set_timeo(l, SO_RCVTIMEO, 0, 300000);

	/* accept(): "the same properties" (the doc, accept), timeouts too */
	c = connect_to(INADDR_LOOPBACK, port);
	a = c >= 0 ? accept(l, NULL, NULL) : -1;
	check("accept() a loopback connection", a >= 0);
	if (a >= 0) {
		check("accepted socket has SO_RCVTIMEO 0.3 s",
		    get_timeo(a, SO_RCVTIMEO, &s, &us) == 0 && s == 0 &&
		    us == 300000);
		t0 = ticks();
		check("recv() on it times out (EWOULDBLOCK)",
		    recv(a, b, sizeof(b), 0) == -1 &&
		    errno_ == EWOULDBLOCK);
		t0 = ticks() - t0;
		check("... after about 0.3 s", t0 >= 10 && t0 <= 50);
		CloseSocket(a);
	}
	if (c >= 0)
		CloseSocket(c);
	CloseSocket(l);

	/* Dup2Socket(): "EBADF ... new_socket is not -1" (the doc,
	   Dup2Socket ERRORS) */
	t = socket(AF_INET, SOCK_STREAM, 0);
	check("Dup2Socket(s, -2) fails with EBADF",
	    Dup2Socket(t, -2) == -1 && errno_ == EBADF);

	/* ReleaseSocket() / ObtainSocket() keep non-blocking mode, the
	   timeouts and the listening state */
	l = listener(&port);
	on = 1;
	check("FIONBIO on a listener", l >= 0 &&
	    IoctlSocket(l, FIONBIO, &on) == 0);
	set_timeo(l, SO_RCVTIMEO, 0, 200000);
	id = l >= 0 ? ReleaseSocket(l, UNIQUE_ID) : -1;
	check("ReleaseSocket(UNIQUE_ID)", id > 65535);
	o = id > 0 ? ObtainSocket(id, AF_INET, SOCK_STREAM, 0) : -1;
	check("ObtainSocket()", o >= 0);
	if (o >= 0) {
		check("obtained socket keeps SO_RCVTIMEO 0.2 s",
		    get_timeo(o, SO_RCVTIMEO, &s, &us) == 0 && s == 0 &&
		    us == 200000);
		t0 = ticks();
		check("obtained socket is still non-blocking (accept: "
		    "EWOULDBLOCK)", accept(o, NULL, NULL) == -1 &&
		    errno_ == EWOULDBLOCK && ticks() - t0 < 10);
		CloseSocket(o);
	}
	if (t >= 0)
		CloseSocket(t);
}

/* ------------------------------------------------------------------------
 * SO_LINGER
 */

/* a connection whose receiver never reads, the sender's queue full */
static int
full_pair(LONG *lp, LONG *cp, LONG *ap)
{
	UWORD port;
	LONG on = 1, off = 0, n, i;
	static char block[1024];

	if ((*lp = listener(&port)) < 0)
		return -1;
	*cp = connect_to(INADDR_LOOPBACK, port);
	*ap = *cp >= 0 ? accept(*lp, NULL, NULL) : -1;
	if (*ap < 0)
		return -1;
	IoctlSocket(*cp, FIONBIO, &on);
	for (i = 0; i < 4096; i++)
		if ((n = send(*cp, block, sizeof(block), 0)) <= 0)
			break;
	IoctlSocket(*cp, FIONBIO, &off);
	return i > 0 && i < 4096 && errno_ == EWOULDBLOCK ? 0 : -1;
}

static void
test_linger(void)
{
	struct linger lg;
	LONG l, c, a;
	ULONG t0, dt;

	check("a connection with a full send queue",
	    full_pair(&l, &c, &a) == 0);
	lg.l_onoff = 1;
	lg.l_linger = 2;
	check("SO_LINGER 2 s", setsockopt(c, SOL_SOCKET, SO_LINGER, &lg,
	    sizeof(lg)) == 0);
	t0 = ticks();
	check("CloseSocket() with unsent data", CloseSocket(c) == 0);
	dt = ticks() - t0;
	say("     waited ");
	sayn(dt * 2);
	say(" / 100 s\n");
	check("... waited the linger interval (1.9 to 4 s)",
	    dt >= 95 && dt <= 200);
	CloseSocket(a);
	CloseSocket(l);

	/* a break signal ends the wait */
	check("another full connection", full_pair(&l, &c, &a) == 0);
	setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
	SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
	t0 = ticks();
	CloseSocket(c);
	dt = ticks() - t0;
	SetSignal(0, SIGBREAKF_CTRL_C);
	check("Ctrl-C ends the linger wait (under 1 s)", dt < 50);
	check("... and the socket is closed", CloseSocket(c) == -1 &&
	    errno_ == EBADF);
	CloseSocket(a);
	CloseSocket(l);
}

/* ------------------------------------------------------------------------
 * SIGURG, routing socket events, interface time
 */

static void
test_signals(void)
{
	struct TagItem tags[2];
	UWORD port;
	LONG l, c, a, r, i, fd, got;
	ULONG ev, mask = FD_READ;
	struct __timeval tv;
	struct DateStamp ds;
	ULONG now;
	char buf[512];

	/* SBTC_SIGURGMASK set first: the sockets made after it get SIGURG
	   (the doc, SocketBaseTagList SBTC_SIGURGMASK) */
	tags[0].ti_Tag = SBTM_SETVAL(SBTC_SIGURGMASK);
	tags[0].ti_Data = SIGBREAKF_CTRL_D;
	tags[1].ti_Tag = TAG_END;
	check("SBTC_SIGURGMASK = Ctrl-D", SocketBaseTagList(tags) == 0);
	SetSignal(0, SIGBREAKF_CTRL_D);
	l = listener(&port);
	c = l >= 0 ? connect_to(INADDR_LOOPBACK, port) : -1;
	a = c >= 0 ? accept(l, NULL, NULL) : -1;
	check("a loopback connection made after it", a >= 0);
	if (a >= 0) {
		check("send(MSG_OOB)", send(c, (APTR)"!", 1, MSG_OOB) == 1);
		for (i = 0; i < 100 && !(SetSignal(0, 0) & SIGBREAKF_CTRL_D);
		    i++)
			Delay(1);
		check("SIGURG (Ctrl-D) arrives",
		    (SetSignal(0, SIGBREAKF_CTRL_D) & SIGBREAKF_CTRL_D) != 0);
	}
	tags[0].ti_Data = 0;
	SocketBaseTagList(tags);
	if (a >= 0)
		CloseSocket(a);
	if (c >= 0)
		CloseSocket(c);
	if (l >= 0)
		CloseSocket(l);

	/* the peer closed while the mask had no FD_CLOSE: a mask with it
	   set later still reports it (src/lib/events.c, ev_setmask()) */
	l = listener(&port);
	c = l >= 0 ? connect_to(INADDR_LOOPBACK, port) : -1;
	a = c >= 0 ? accept(l, NULL, NULL) : -1;
	check("another loopback connection", a >= 0);
	if (a >= 0) {
		mask = FD_READ;
		check("SO_EVENTMASK FD_READ", setsockopt(a, SOL_SOCKET,
		    SO_EVENTMASK, &mask, sizeof(mask)) == 0);
		CloseSocket(c);
		c = -1;
		Delay(25);
		while (GetSocketEvents(&ev) >= 0)
			;
		mask = FD_CLOSE;
		check("SO_EVENTMASK FD_CLOSE after the close",
		    setsockopt(a, SOL_SOCKET, SO_EVENTMASK, &mask,
		    sizeof(mask)) == 0);
		fd = -1;
		for (i = 0; i < 100 && fd != a; i++) {
			fd = GetSocketEvents(&ev);
			if (fd != a)
				Delay(1);
		}
		check("... FD_CLOSE is reported", fd == a && (ev & FD_CLOSE));
		CloseSocket(a);
	}
	if (c >= 0)
		CloseSocket(c);
	if (l >= 0)
		CloseSocket(l);
	mask = FD_READ;

	/* FD_READ on a routing socket, for every message */
	r = socket(AF_ROUTE, SOCK_RAW, 0);
	check("socket(AF_ROUTE)", r >= 0);
	if (r >= 0) {
		struct TagItem rt[3];

		check("SO_EVENTMASK FD_READ", setsockopt(r, SOL_SOCKET,
		    SO_EVENTMASK, &mask, sizeof(mask)) == 0);
		rt[0].ti_Tag = RTA_DestinationHost;
		rt[0].ti_Data = (ULONG)"192.0.2.50";
		rt[1].ti_Tag = RTA_Gateway;
		rt[1].ti_Data = (ULONG)"10.0.2.2";
		rt[2].ti_Tag = TAG_END;
		for (got = 0; got < 2; got++) {
			if (got == 0)
				check("AddRouteTagList() 192.0.2.50",
				    AddRouteTagList(rt) == 0);
			else
				check("DeleteRouteTagList() 192.0.2.50",
				    DeleteRouteTagList(rt) == 0);
			fd = -1;
			for (i = 0; i < 100 && fd != r; i++) {
				fd = GetSocketEvents(&ev);
				if (fd != r)
					Delay(1);
			}
			check(got == 0 ? "FD_READ for the first route message" :
			    "FD_READ again for the next one",
			    fd == r && (ev & FD_READ));
			/* everything queued, so the next event is new */
			while (recv(r, buf, sizeof(buf), MSG_DONTWAIT) > 0)
				;
		}
		CloseSocket(r);
	}

	/* IFQ_LastStart is Amiga time (since 1978, as DateStamp()) */
	tags[0].ti_Tag = IFQ_LastStart;
	tags[0].ti_Data = (ULONG)&tv;
	tags[1].ti_Tag = TAG_END;
	tv.tv_secs = 0;
	check("QueryInterfaceTagList(sana0, IFQ_LastStart)",
	    QueryInterfaceTagList((STRPTR)"sana0", tags) == 0);
	DateStamp(&ds);
	now = ((ULONG)ds.ds_Days * 1440 + ds.ds_Minute) * 60 +
	    ds.ds_Tick / 50;
	say("     IFQ_LastStart ");
	sayn(tv.tv_secs);
	say(", now ");
	sayn(now);
	say("\n");
	check("... is within the last hour", tv.tv_secs <= now &&
	    now - tv.tv_secs < 3600);

	/* In_LocalAddr() against sana0's address and its own mask
	   (tests/slirp-static.conf: 10.0.2.15/24) */
	check("In_LocalAddr(10.0.2.2) is local",
	    In_LocalAddr(0x0a000202UL) != 0);
	check("In_LocalAddr(192.0.2.7) is not",
	    In_LocalAddr(0xc0000207UL) == 0);
}

/* ------------------------------------------------------------------------
 * SocketBaseTagList() strings and ICMP / IDN tags
 */

static const char *
tagstr(ULONG code, LONG n)
{
	struct TagItem t[2];
	ULONG v = n;

	t[0].ti_Tag = SBTM_GETREF(code);
	t[0].ti_Data = (ULONG)&v;
	t[1].ti_Tag = TAG_END;
	if (SocketBaseTagList(t) != 0)
		return NULL;
	return (const char *)v;
}

static void
test_tags(void)
{
	struct TagItem t[2];
	const char *s;
	ULONG v;

	/* locale/bsdsocket.cd:526-655 */
	s = tagstr(SBTC_IOERRNOSTRPTR, 1);
	check("IO error 1 = \"Device/unit failed to open\"",
	    s && streq_(s, "Device/unit failed to open"));
	s = tagstr(SBTC_IOERRNOSTRPTR, 9);
	check("IO error 9 = \"Unknown I/O error 9\"",
	    s && streq_(s, "Unknown I/O error 9"));
	s = tagstr(SBTC_S2ERRNOSTRPTR, 3);
	check("S2ERR 3 = \"Bad argument\"", s && streq_(s, "Bad argument"));
	s = tagstr(SBTC_S2ERRNOSTRPTR, 2);
	check("S2ERR 2 = \"Unknown SANA-II error 2\"",
	    s && streq_(s, "Unknown SANA-II error 2"));
	s = tagstr(SBTC_S2WERRNOSTRPTR, 19);
	check("S2WERR 19 = \"Unit currently not connected\"",
	    s && streq_(s, "Unit currently not connected"));
	s = tagstr(SBTC_S2WERRNOSTRPTR, 14);
	check("S2WERR 14 = \"Unknown SANA-II wire error 14\"",
	    s && streq_(s, "Unknown SANA-II wire error 14"));

	t[1].ti_Tag = TAG_END;
	v = 99;
	t[0].ti_Tag = SBTM_GETREF(SBTC_ICMP_PROCESS_ECHO);
	t[0].ti_Data = (ULONG)&v;
	check("SBTC_ICMP_PROCESS_ECHO is IR_Process",
	    SocketBaseTagList(t) == 0 && v == IR_Process);
	t[0].ti_Tag = SBTM_SETVAL(SBTC_ICMP_PROCESS_ECHO);
	t[0].ti_Data = IR_Ignore;
	check("... IR_Ignore is refused (index 1)", SocketBaseTagList(t) == 1);
	t[0].ti_Data = IR_Process;
	check("... IR_Process is taken", SocketBaseTagList(t) == 0);
	v = 99;
	t[0].ti_Tag = SBTM_GETREF(SBTC_IDN_DEFAULT_CHARACTER_SET);
	t[0].ti_Data = (ULONG)&v;
	check("SBTC_IDN_DEFAULT_CHARACTER_SET is IDNCS_ASCII (0)",
	    SocketBaseTagList(t) == 0 && v == 0);
	t[0].ti_Tag = SBTM_SETVAL(SBTC_IDN_DEFAULT_CHARACTER_SET);
	t[0].ti_Data = 1;
	check("... setting Latin 1 is taken (the doc: then like ASCII)",
	    SocketBaseTagList(t) == 0);
}

/* ------------------------------------------------------------------------
 * hooks: CallHookPkt() passes hook, object, message in A0, A2, A1
 * (utility.doc CallHookPkt); the entry calls hookfn(hook, obj, msg)
 */

ULONG	hook_entry(void);
__asm__(
"	.text\n"
"	.globl	hook_entry\n"
"hook_entry:\n"
"	move.l	%a1,-(%sp)\n"
"	move.l	%a2,-(%sp)\n"
"	move.l	%a0,-(%sp)\n"
"	move.l	12(%a0),%a0\n"
"	jsr	(%a0)\n"
"	lea	12(%sp),%sp\n"
"	rts\n");

static volatile int nlog, nconnect;

/* netinclude/libraries/bsdsocket.h:303-316 */
struct LogHookMessage {
	LONG lhm_Size;
	LONG lhm_Priority;
	struct DateStamp lhm_Date;
	STRPTR lhm_Tag;
	ULONG lhm_ID;
	STRPTR lhm_Message;
};
static char logtext[80];

static ULONG
log_fn(struct Hook *h, APTR obj, APTR msg)
{
	const struct LogHookMessage *m = msg;
	int i;

	nlog++;
	for (i = 0; i < 79 && m->lhm_Message && m->lhm_Message[i]; i++)
		logtext[i] = m->lhm_Message[i];
	logtext[i] = '\0';
	return 0;
}

/* removes itself: "a hook that removes itself from inside" */
static ULONG
connect_fn(struct Hook *h, APTR obj, APTR msg)
{

	nconnect++;
	RemoveNetMonitorHook(h);
	return 0;
}

static struct Hook loghook, conhook;

static int
reopen(void)
{
	int i;

	CloseLibrary(SocketBase);
	SocketBase = NULL;
	for (i = 0; i < 50 && SocketBase == NULL; i++)
		if ((SocketBase = OpenLibrary("bsdsocket.library", 4)) == NULL)
			Delay(5);
	if (SocketBase)
		SetErrnoPtr(&errno_, sizeof(errno_));
	return SocketBase != NULL;
}

static void
test_hooks(void)
{
	struct TagItem t[2];
	ULONG v;
	LONG s;
	int n;

	loghook.h_Entry = hook_entry;
	loghook.h_SubEntry = (ULONG (*)())log_fn;
	t[0].ti_Tag = SBTM_SETVAL(SBTC_LOG_HOOK);
	t[0].ti_Data = (ULONG)&loghook;
	t[1].ti_Tag = TAG_END;
	check("SBTC_LOG_HOOK set", SocketBaseTagList(t) == 0);
	vsyslog(LOG_ERR, (STRPTR)"libtest: through the log hook", NULL);
	check("the log hook is called", nlog == 1);
	/* ("the log message to be displayed", bsdsocket.h:314: no
	   priority, tag or line end in it) */
	check("... with the message text only",
	    streq_(logtext, "libtest: through the log hook"));
	check("CloseLibrary() and OpenLibrary() again", reopen());
	if (SocketBase == NULL)
		return;
	v = 1;
	t[0].ti_Tag = SBTM_GETREF(SBTC_LOG_HOOK);
	t[0].ti_Data = (ULONG)&v;
	check("closing the base removed its log hook",
	    SocketBaseTagList(t) == 0 && v == 0);
	n = nlog;
	vsyslog(LOG_ERR, (STRPTR)"libtest: to the stack's log", NULL);
	check("... and it is not called any more", nlog == n);

	conhook.h_Entry = hook_entry;
	conhook.h_SubEntry = (ULONG (*)())connect_fn;
	check("AddNetMonitorHookTagList(MHT_Connect)",
	    AddNetMonitorHookTagList(MHT_Connect, &conhook, NULL) == 0);
	s = connect_to(0x0a000202UL, 7777);
	check("connect() with a hook that removes itself returns", s >= 0);
	if (s >= 0)
		CloseSocket(s);
	s = connect_to(0x0a000202UL, 7777);
	if (s >= 0)
		CloseSocket(s);
	check("... the hook ran once and is gone", nconnect == 1);
}

/* ------------------------------------------------------------------------
 * a SocketBaseTagList() mask tag that fails leaves the old value: with
 * no free block of 16 KB the base's event thread cannot start (its stack
 * is THREAD_STACK, 32 KB, src/host/rumpuser_amiga.c; src/lib/events.c
 * ev_start()), so SBTC_SIGURGMASK fails on a base whose event thread is
 * not running yet (a fresh one: test_hooks() opened it)
 */

static APTR hog[1024];
static ULONG hogsize[1024];

static void
test_rollback(void)
{
	struct TagItem t[2];
	ULONG size, v;
	LONG r;
	int n = 0, i;

	for (size = 1UL << 20; size >= 16384 && n < 1024; )
		if ((hog[n] = AllocMem(size, MEMF_ANY)) != NULL)
			hogsize[n++] = size;
		else
			size >>= 1;
	t[0].ti_Tag = SBTM_SETVAL(SBTC_SIGURGMASK);
	t[0].ti_Data = SIGBREAKF_CTRL_D;
	t[1].ti_Tag = TAG_END;
	errno_ = 0;
	r = SocketBaseTagList(t);
	for (i = 0; i < n; i++)
		FreeMem(hog[i], hogsize[i]);
	say("     with no 16 KB block free: SocketBaseTagList() = ");
	sayn(r);
	say("\n");
	/* (errno stays 0: the call reached the server thread, whose
	   ev_start() failed - src/lib/misc.c srv_tag() returns ev_sync()'s
	   -1 without an error; sb_rpc() itself failing would set it) */
	check("SBTC_SIGURGMASK fails without memory (index 1, errno 0)",
	    r == 1 && errno_ == 0);
	v = 99;
	t[0].ti_Tag = SBTM_GETREF(SBTC_SIGURGMASK);
	t[0].ti_Data = (ULONG)&v;
	check("... and the mask is still the old one (0)",
	    SocketBaseTagList(t) == 0 && v == 0);
	t[0].ti_Tag = SBTM_SETVAL(SBTC_SIGURGMASK);
	t[0].ti_Data = SIGBREAKF_CTRL_D;
	check("with the memory back it is taken", SocketBaseTagList(t) == 0);
	t[0].ti_Data = 0;
	SocketBaseTagList(t);
}

/* ------------------------------------------------------------------------
 * mbuf range checks
 */

static void
test_mbuf(void)
{
	struct mbuf *m = mbuf_get();
	char b[4];

	check("mbuf_get()", m != NULL);
	if (m == NULL)
		return;
	check("mbuf_copydata() with off + len past 2^31 fails",
	    mbuf_copydata(m, 0x7fffffffL, 1, b) == -1);
	check("mbuf_copyback() with off + len past 2^31 fails",
	    mbuf_copyback(m, 0x7fffffffL, 1, b) == -1);
	mbuf_freem(m);
}

/* ------------------------------------------------------------------------
 * resolver: tools/run_emu.py --dns knows amibsdnet.test and parent.y.test
 */

static void
test_resolver(void)
{
	struct hostent *h;

	SetDefaultDomainName((STRPTR)"x.y.test");
	/* "parent": parent.x.y.test (no such name), then the parent
	   domain, parent.y.test */
	h = gethostbyname((STRPTR)"parent");
	check("\"parent\" with domain x.y.test is parent.y.test",
	    h != NULL && *(ULONG *)h->h_addr_list[0] == 0xc0000208UL);
	h = gethostbyname((STRPTR)"amibsdnet.test");
	check("a name with a dot is asked as it is",
	    h != NULL && *(ULONG *)h->h_addr_list[0] == 0xc0000207UL);
	check("a name nobody knows is not found",
	    gethostbyname((STRPTR)"nosuchname") == NULL);
	check("an absolute name is not searched",
	    gethostbyname((STRPTR)"parent.") == NULL);
	SetDefaultDomainName((STRPTR)"");
}

/* ------------------------------------------------------------------------
 * WaitSelect() woken by a user signal and the break signal together;
 * servent and protoent results side by side; inet_pton() with "::"
 */

static struct Task *signaller_to;

static void
signaller_main(void)
{

	Delay(25);
	Signal(signaller_to, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);
}

#define	SVC_FILE	"DEVS:Internet/services"
#define	SVC_SAVED	"DEVS:Internet/services.libtest"

/* downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/netinclude/netdb.h:139-152
   (amibsdnet/net.h declares them only) */
struct servent {
	STRPTR s_name;
	STRPTR *s_aliases;
	LONG s_port;
	STRPTR s_proto;
};

struct protoent {
	STRPTR p_name;
	STRPTR *p_aliases;
	LONG p_proto;
};

/* the library's value, src/lib/sblib.h:234 */
#define	AF_INET6	24

/* a services file with an alias; one that is there already is renamed
   to SVC_SAVED meanwhile and put back afterwards */
static void
svc_round(void)
{
	static const char line[] = "libtestsvc 4242/tcp libtestalias\n";
	struct servent *se;
	struct protoent *pe;
	BPTR f, lock;
	int made = 0, saved = 0;

	if ((lock = Lock((CONST_STRPTR)SVC_FILE, ACCESS_READ))) {
		UnLock(lock);
		DeleteFile((CONST_STRPTR)SVC_SAVED);
		saved = Rename((CONST_STRPTR)SVC_FILE, (CONST_STRPTR)SVC_SAVED);
		check("the services file there set aside", saved);
	} else if ((lock = CreateDir((CONST_STRPTR)"DEVS:Internet")))
		UnLock(lock);
	if ((lock = Lock((CONST_STRPTR)SVC_FILE, ACCESS_READ)))
		UnLock(lock);
	else if ((f = Open((CONST_STRPTR)SVC_FILE, MODE_NEWFILE))) {
		made = Write(f, (APTR)line, sizeof(line) - 1) ==
		    (LONG)sizeof(line) - 1;
		Close(f);
	}
	check("a services file (" SVC_FILE ")", made);
	if (made) {
		se = getservbyname((STRPTR)"libtestsvc", (STRPTR)"tcp");
		check("getservbyname(libtestsvc)", se != NULL &&
		    se->s_aliases[0] != NULL);
		pe = getprotobyname((STRPTR)"tcp");
		check("getprotobyname(tcp), no aliases", pe != NULL &&
		    pe->p_aliases[0] == NULL);
		check("... the servent keeps its alias", se != NULL &&
		    se->s_aliases[0] != NULL &&
		    se->s_aliases[0][0] == 'l' && se->s_aliases[0][7] == 'a');
		DeleteFile((CONST_STRPTR)SVC_FILE);
	}
	if (saved)
		check("... and put back", Rename((CONST_STRPTR)SVC_SAVED,
		    (CONST_STRPTR)SVC_FILE));
}

static void
test_netdb(void)
{
	static const char mine[] = "# a services file of its own\n";
	char back[64];
	ULONG sigs;
	LONG rv, n = -1;
	BPTR f, lock;
	UBYTE a6[16];

	/* the break signal arrives with a user signal: -1 and EINTR, not
	   0 (bsdsocket.doc WaitSelect) */
	signaller_to = SysBase->ThisTask;
	SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);
	check("a signalling process", CreateNewProcTags(NP_Entry,
	    (ULONG)signaller_main, NP_Name, (ULONG)"libtest signaller",
	    NP_StackSize, 4096, TAG_END) != NULL);
	sigs = SIGBREAKF_CTRL_E;
	errno_ = 0;
	rv = WaitSelect(0, NULL, NULL, NULL, NULL, &sigs);
	check("WaitSelect() woken by Ctrl-C and Ctrl-E: -1, EINTR",
	    rv == -1 && errno_ == EINTR);
	check("... Ctrl-C stays set", (SetSignal(0, 0) & SIGBREAKF_CTRL_C) !=
	    0);
	SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);

	/* with no services file there, then with one (written here, and
	   checked to be the same after) */
	if ((lock = Lock((CONST_STRPTR)SVC_FILE, ACCESS_READ)))
		UnLock(lock);
	else
		svc_round();
	if ((lock = CreateDir((CONST_STRPTR)"DEVS:Internet")))
		UnLock(lock);
	if ((f = Open((CONST_STRPTR)SVC_FILE, MODE_OLDFILE)) == 0 &&
	    (f = Open((CONST_STRPTR)SVC_FILE, MODE_NEWFILE))) {
		Write(f, (APTR)mine, sizeof(mine) - 1);
		Close(f);
		svc_round();
		if ((f = Open((CONST_STRPTR)SVC_FILE, MODE_OLDFILE))) {
			n = Read(f, back, sizeof(back));
			Close(f);
		}
		check("... the file there before is back as it was",
		    n == (LONG)sizeof(mine) - 1 &&
		    memcmp(back, mine, n) == 0);
		DeleteFile((CONST_STRPTR)SVC_FILE);
	} else {
		if (f)
			Close(f);
		svc_round();
	}

	check("inet_pton(\"1::2:3:4:5:6:7\") (seven groups and ::)",
	    inet_pton(AF_INET6, (STRPTR)"1::2:3:4:5:6:7", a6) == 1 &&
	    a6[2] == 0 && a6[3] == 0 && a6[5] == 2 && a6[15] == 7);
	check("inet_pton(\"1::2:3:4:5:6:7:8\") refused (eight and ::)",
	    inet_pton(AF_INET6, (STRPTR)"1::2:3:4:5:6:7:8", a6) == 0);
}

/* ------------------------------------------------------------------------
 * GetNetworkStatistics() (the nodes found by name: igmp, rt, the socket
 * lists) and the byte counters (src/lib/stats.c)
 */

/* NETWORKSTATUS_VERSION, NETSTATUS_igmp, _rt, _tcp_sockets (src/lib/
   stats.c:203-212); SBTC_GET_BYTES_RECEIVED (netinclude/libraries/
   bsdsocket.h:241) */
#define	NETWORKSTATUS_VERSION	1
#define	NETSTATUS_igmp		1
#define	NETSTATUS_rt		5
#define	NETSTATUS_tcp_sockets	9
#define	SBTC_GET_BYTES_RECEIVED	64
/* netinclude/libraries/bsdsocket.h; RTA_DefaultGateway src/lib/route.c */
#define	SBTC_SYSTEM_STATUS	56
#define	RTA_DefaultGateway	(RTA_BASE + 3)

static void
test_stats(void)
{
	struct TagItem t[2], rt[2];
	ULONG q[2] = { 0, 0 }, st;
	UBYTE buf[64];

	check("GetNetworkStatistics(igmp) size", GetNetworkStatistics(
	    NETSTATUS_igmp, NETWORKSTATUS_VERSION, NULL, 0) > 0);
	check("GetNetworkStatistics(rt)", GetNetworkStatistics(NETSTATUS_rt,
	    NETWORKSTATUS_VERSION, buf, sizeof(buf)) > 0);
	check("GetNetworkStatistics(tcp_sockets)", GetNetworkStatistics(
	    NETSTATUS_tcp_sockets, NETWORKSTATUS_VERSION, NULL, 0) >= 0);
	errno_ = 0;
	check("version 2 refused, EINVAL", GetNetworkStatistics(NETSTATUS_rt,
	    2, NULL, 0) == -1 && errno_ == EINVAL);
	t[0].ti_Tag = SBTM_GETREF(SBTC_GET_BYTES_RECEIVED);
	t[0].ti_Data = (ULONG)q;
	t[1].ti_Tag = TAG_END;
	check("SBTC_GET_BYTES_RECEIVED, more than 0 (DNS and echo so far)",
	    SocketBaseTagList(t) == 0 && (q[0] || q[1]));

	/* SBSYSSTAT_Routes and _DefaultRoute (bsdsocket.h:292-295), with
	   the default route of tests/slirp-static.conf, then without it
	   (sana0's own net 10.0.2.0/24 still routed) */
	t[0].ti_Tag = SBTM_GETREF(SBTC_SYSTEM_STATUS);
	t[0].ti_Data = (ULONG)&st;
	st = 0;
	check("SBTC_SYSTEM_STATUS: Routes and DefaultRoute",
	    SocketBaseTagList(t) == 0 && (st & 0x30) == 0x30);
	rt[0].ti_Tag = RTA_DefaultGateway;
	rt[0].ti_Data = (ULONG)"10.0.2.2";
	rt[1].ti_Tag = TAG_END;
	check("DeleteRouteTagList(RTA_DefaultGateway 10.0.2.2)",
	    DeleteRouteTagList(rt) == 0);
	st = 0;
	check("... Routes still, DefaultRoute no more",
	    SocketBaseTagList(t) == 0 && (st & 0x30) == 0x10);
	check("AddRouteTagList(RTA_DefaultGateway 10.0.2.2) again",
	    AddRouteTagList(rt) == 0);
}

/* ------------------------------------------------------------------------
 * a base shared by two tasks: a call that blocks in one does not hold up
 * the other (SBTC_CAN_SHARE_LIBRARY_BASES)
 */

static struct Task *parent;
static LONG child_sock;
static volatile LONG child_n = -2;

static void
child_main(void)
{
	char b[16];

	child_n = recv(child_sock, b, sizeof(b), 0);
	Signal(parent, SIGBREAKF_CTRL_F);
}

static void
test_shared(void)
{
	struct TagItem t[2];
	LONG s2, n, i, l, a;
	UWORD port;
	ULONG t0;
	char b[8];

	t[0].ti_Tag = SBTM_SETVAL(SBTC_CAN_SHARE_LIBRARY_BASES);
	t[0].ti_Data = TRUE;
	t[1].ti_Tag = TAG_END;
	check("SBTC_CAN_SHARE_LIBRARY_BASES", SocketBaseTagList(t) == 0);
	child_sock = connect_to(0x0a000202UL, 7777);
	check("a connection to the echo server", child_sock >= 0);
	if (child_sock < 0)
		return;
	parent = SysBase->ThisTask;
	SetSignal(0, SIGBREAKF_CTRL_F);
	check("a second process", CreateNewProcTags(NP_Entry, (ULONG)child_main,
	    NP_Name, (ULONG)"libtest child", NP_StackSize, 16384,
	    TAG_END) != NULL);
	Delay(50);	/* (it is in recv() now, nothing to read) */
	check("... still waiting in recv()", child_n == -2);
	/* (on loopback: the --echo server serves one connection at a time,
	   tools/run_emu.py echo_server()) */
	t0 = ticks();
	l = listener(&port);
	s2 = l >= 0 ? connect_to(INADDR_LOOPBACK, port) : -1;
	a = s2 >= 0 ? accept(l, NULL, NULL) : -1;
	n = a >= 0 ? send(s2, (APTR)"ping", 4, 0) : -1;
	say("     listener ");
	sayn(l);
	say(", connect ");
	sayn(s2);
	say(", accept ");
	sayn(a);
	say(", send ");
	sayn(n);
	n = n == 4 ? recv(a, b, sizeof(b), 0) : -1;
	say(", recv ");
	sayn(n);
	say("\n");
	check("this task's own calls meanwhile", n == 4 &&
	    ticks() - t0 < 250);
	if (a >= 0)
		CloseSocket(a);
	if (s2 >= 0)
		CloseSocket(s2);
	if (l >= 0)
		CloseSocket(l);
	/* now the other task's recv() gets its data */
	send(child_sock, (APTR)"wake", 4, 0);
	for (i = 0; i < 250 && !(SetSignal(0, 0) & SIGBREAKF_CTRL_F); i++)
		Delay(1);
	say("     the other task's recv() returned ");
	sayn(child_n);
	say("\n");
	check("the other task's recv() returns 4", child_n == 4);
	CloseSocket(child_sock);
	t[0].ti_Data = FALSE;
	SocketBaseTagList(t);
}

/*
 * CloseLibrary() while another task sharing the base waits in recv(): the
 * call is aborted (EINTR) and CloseLibrary() returns (src/lib/library.c
 * server_quit())
 */
static void
test_close_shared(void)
{
	struct TagItem t[2];
	ULONG t0;
	int i;

	t[0].ti_Tag = SBTM_SETVAL(SBTC_CAN_SHARE_LIBRARY_BASES);
	t[0].ti_Data = TRUE;
	t[1].ti_Tag = TAG_END;
	check("a shared base again", SocketBaseTagList(t) == 0);
	child_sock = connect_to(0x0a000202UL, 7777);
	check("a connection to the echo server", child_sock >= 0);
	if (child_sock < 0)
		return;
	child_n = -2;
	SetSignal(0, SIGBREAKF_CTRL_F);
	check("a process waiting in recv()", CreateNewProcTags(NP_Entry,
	    (ULONG)child_main, NP_Name, (ULONG)"libtest child 2",
	    NP_StackSize, 16384, TAG_END) != NULL);
	Delay(50);
	check("... it waits", child_n == -2);
	t0 = ticks();
	CloseLibrary(SocketBase);
	SocketBase = NULL;
	t0 = ticks() - t0;
	for (i = 0; i < 250 && !(SetSignal(0, 0) & SIGBREAKF_CTRL_F); i++)
		Delay(1);
	say("     CloseLibrary() took ");
	sayn(t0 * 2);
	say(" / 100 s, the other recv() returned ");
	sayn(child_n);
	say("\n");
	check("CloseLibrary() returns (under 5 s)", t0 < 250);
	check("... and the other task's recv() was aborted (-1)",
	    child_n == -1);
}

/* ------------------------------------------------------------------------
 * a shared base closed by the other task, after this one (the opener)
 * made calls with it (its reply port, src/lib/library.c
 * reply_port_free()): the library works on for a new base
 */

static struct Library *closer_base;

static void
closer_main(void)
{

	CloseLibrary(closer_base);
	Signal(parent, SIGBREAKF_CTRL_F);
}

static void
test_close_by_other(void)
{
	struct TagItem t[2];
	LONG s;
	int i;

	SocketBase = OpenLibrary("bsdsocket.library", 4);
	check("a new base", SocketBase != NULL);
	if (SocketBase == NULL)
		return;
	SetErrnoPtr(&errno_, sizeof(errno_));
	t[0].ti_Tag = SBTM_SETVAL(SBTC_CAN_SHARE_LIBRARY_BASES);
	t[0].ti_Data = TRUE;
	t[1].ti_Tag = TAG_END;
	check("... shared", SocketBaseTagList(t) == 0);
	s = socket(AF_INET, SOCK_STREAM, 0);
	check("... a call with it (socket())", s >= 0);
	closer_base = SocketBase;
	SocketBase = NULL;
	parent = SysBase->ThisTask;
	SetSignal(0, SIGBREAKF_CTRL_F);
	check("a process that closes it", CreateNewProcTags(NP_Entry,
	    (ULONG)closer_main, NP_Name, (ULONG)"libtest closer",
	    NP_StackSize, 16384, TAG_END) != NULL);
	for (i = 0; i < 250 && !(SetSignal(0, 0) & SIGBREAKF_CTRL_F); i++)
		Delay(1);
	check("... its CloseLibrary() returned",
	    (SetSignal(0, SIGBREAKF_CTRL_F) & SIGBREAKF_CTRL_F) != 0);
	SocketBase = OpenLibrary("bsdsocket.library", 4);
	check("another base after that", SocketBase != NULL);
	if (SocketBase == NULL)
		return;
	SetErrnoPtr(&errno_, sizeof(errno_));
	s = socket(AF_INET, SOCK_STREAM, 0);
	check("... socket() with it", s >= 0);
	if (s >= 0)
		CloseSocket(s);
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
	for (i = 0; i < 300 && SocketBase == NULL; i++) {
		SocketBase = OpenLibrary("bsdsocket.library", 4);
		if (SocketBase == NULL)
			Delay(10);
	}
	check("OpenLibrary(\"bsdsocket.library\", 4)", SocketBase != NULL);
	if (SocketBase) {
		SetErrnoPtr(&errno_, sizeof(errno_));
		test_tags();
		test_mbuf();
		test_state();
		test_linger();
		test_signals();
		test_resolver();
		test_netdb();
		test_stats();
		test_shared();
		test_hooks();
		if (SocketBase)
			test_rollback();
		if (SocketBase)
			test_close_shared();
		if (SocketBase == NULL)
			test_close_by_other();
		if (SocketBase)
			CloseLibrary(SocketBase);
	}
	say(failures ? "libtest: FAILED\n" : "libtest: PASS\n");
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
