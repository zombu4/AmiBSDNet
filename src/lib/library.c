/*
 * bsdsocket.library: library housekeeping, per-opener server threads and
 * the call mechanism (see sblib.h for the design).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "sblib.h"

extern struct ExecBase *SysBase;
extern ULONG bsdsocket_vectors[];

#define	LIBNAME		"bsdsocket.library"
#define	LIBVERSION	4
#define	LIBREVISION	1
#define	LIBIDSTRING	"bsdsocket.library 4.1 (AmiBSDNet, NetBSD 11 TCP/IP)\r\n"

static struct SocketBase *master;
static struct MsgPort *mgrport;
static struct Task *mgrtask;
static void *mgrthread;
static LONG mgrsock = -1;

/* messages to the manager thread */
#define	MGR_CREATE	1
#define	MGR_ABORT	2
#define	MGR_QUIT	3

struct mgrmsg {
	struct Message msg;
	int op;
	struct SocketBase *sb;
	LONG result;
};

/* a call travelling to a server thread */
struct sbcall {
	struct Message msg;
	sbfn_t fn;
	void *args;
	LONG result;
	LONG err;
	ULONG seq;
	int quit;
};

/* ------------------------------------------------------------------------
 * small helpers
 */

size_t
sb_strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return p - s;
}

char *
sb_strlcpy(char *d, const char *s, size_t n)
{
	size_t i;

	if (n == 0)
		return d;
	for (i = 0; i + 1 < n && s[i]; i++)
		d[i] = s[i];
	d[i] = '\0';
	return d;
}

int
sb_strcmp(const char *a, const char *b)
{

	while (*a && *a == *b)
		a++, b++;
	return (UBYTE)*a - (UBYTE)*b;
}

int
sb_strcasecmp(const char *a, const char *b)
{
	int ca, cb;

	do {
		ca = (UBYTE)*a++;
		cb = (UBYTE)*b++;
		if (ca >= 'A' && ca <= 'Z')
			ca += 32;
		if (cb >= 'A' && cb <= 'Z')
			cb += 32;
	} while (ca && ca == cb);
	return ca - cb;
}

void
sb_set_errno(struct SocketBase *sb, LONG e)
{

	sb->sb_errno = e;
	if (sb->errnoptr) {
		switch (sb->errnosize) {
		case 1:
			*(UBYTE *)sb->errnoptr = (UBYTE)e;
			break;
		case 2:
			*(UWORD *)sb->errnoptr = (UWORD)e;
			break;
		default:
			*(ULONG *)sb->errnoptr = (ULONG)e;
			break;
		}
	}
}

void
sb_set_herrno(struct SocketBase *sb, LONG e)
{

	sb->sb_herrno = e;
	if (sb->herrnoptr)
		*sb->herrnoptr = e;
}

LONG
sb_rumperr(void)
{

	return amiga_rump_errno();
}

/* ------------------------------------------------------------------------
 * server thread: one per library base, owns a NetBSD process
 */

static LONG
make_wake_socket(struct SocketBase *sb)
{
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	LONG s, on = 1;

	if ((s = rump___sysimpl_socket30(AF_INET, SOCK_DGRAM, 0)) < 0)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_LOOPBACK;
	if (rump___sysimpl_bind(s, &sin, sizeof(sin)) < 0 ||
	    rump___sysimpl_getsockname(s, &sin, &len) < 0 ||
	    rump___sysimpl_ioctl(s, FIONBIO, &on) < 0) {
		rump___sysimpl_close(s);
		return -1;
	}
	sb->wakeport = sin.sin_port;
	return s;
}

void
sb_drain_wake(struct SocketBase *sb)
{
	char buf[16];

	while (rump___sysimpl_recvfrom(sb->wakefd, buf, sizeof(buf), 0, NULL,
	    NULL) > 0)
		;
}

static void *
server_main(void *arg)
{
	struct SocketBase *sb = arg;
	struct MsgPort *port;
	struct sbcall *call;
	int fd;

	if (rump_pub_lwproc_rfork(RUMP_RFCFDG) != 0) {
		sb->srvstate = -1;
		return NULL;
	}
	if ((port = CreateMsgPort()) == NULL) {
		rump_pub_lwproc_releaselwp();
		sb->srvstate = -1;
		return NULL;
	}
	/* the process's descriptor limit starts at 128 (OPEN_MAX): raise it
	   above SB_MAXFD, for SBTC_DTABLESIZE up to SB_MAXFD and for moving
	   the wake socket out of the way (Dup2Socket) */
	{
		struct { unsigned long long cur, max; } rl;

		if (rump___sysimpl_getrlimit(8 /* RLIMIT_NOFILE */, &rl) == 0 &&
		    rl.cur < SB_MAXFD + 8) {
			rl.cur = rl.max < SB_MAXFD + 8 ? rl.max : SB_MAXFD + 8;
			rump___sysimpl_setrlimit(8, &rl);
		}
	}
	if ((sb->wakefd = make_wake_socket(sb)) < 0) {
		DeleteMsgPort(port);
		rump_pub_lwproc_releaselwp();
		sb->srvstate = -1;
		return NULL;
	}
	sb->srvtask = SysBase->ThisTask;
	sb->srvport = port;
	sb->srvstate = 1;

	for (;;) {
		WaitPort(port);
		while ((call = (struct sbcall *)GetMsg(port)) != NULL) {
			if (call->quit)
				goto quit;
			sb_drain_wake(sb);
			call->err = 0;
			sb->curcall = call;
			sb->callseq = call->seq;
			call->result = call->fn(sb, call->args);
			sb->curcall = NULL;
			ReplyMsg(&call->msg);
		}
	}
quit:
	/* close everything this base still holds, then leave the process */
	for (fd = 0; fd < SB_MAXFD; fd++)
		if (sb->fds[fd].inuse)
			rump___sysimpl_close(fd);
	rump___sysimpl_close(sb->wakefd);
	rump_pub_lwproc_releaselwp();
	sb->srvport = NULL;
	DeleteMsgPort(port);
	/* no access to sb after this: the closer frees it */
	ReplyMsg(&call->msg);
	return NULL;
}

/*
 * Server functions fail a call with "return sb_fail(sb, errno);".
 */
LONG
sb_fail(struct SocketBase *sb, LONG err)
{
	struct sbcall *c = sb->curcall;

	if (c)
		c->err = err;
	return -1;
}

int
sb_fdok(struct SocketBase *sb, LONG fd)
{

	return fd >= 0 && fd < SB_MAXFD && sb->fds[fd].inuse;
}

/*
 * Wait (server side) until fd is readable/writable, the timeout expires
 * (EWOULDBLOCK, as BSD reports SO_RCVTIMEO expiry) or the call is aborted
 * (EINTR).  Returns 0 when the fd is ready.
 */
LONG
sb_wait_fd(struct SocketBase *sb, LONG fd, int what, LONG timeout_ms)
{
	struct nb_pollfd pfd[2];
	LONG n;

	for (;;) {
		/* an abort that came before the call started: its wake
		   datagram was drained at the start of the call */
		if (sb->abortseq == sb->callseq)
			return EINTR;
		pfd[0].fd = fd;
		pfd[0].events = (what & WAIT_READ ? NB_POLLIN : 0) |
		    (what & WAIT_WRITE ? NB_POLLOUT : 0);
		pfd[0].revents = 0;
		pfd[1].fd = sb->wakefd;
		pfd[1].events = NB_POLLIN;
		pfd[1].revents = 0;
		n = rump___sysimpl_poll(pfd, 2, timeout_ms > 0 ? timeout_ms : -1);
		if (n < 0)
			return sb_rumperr();
		if (n == 0)
			return EWOULDBLOCK;
		if (pfd[1].revents) {
			sb_drain_wake(sb);
			if (sb->abortseq == sb->callseq)
				return EINTR;
		}
		if (pfd[0].revents)
			return 0;
	}
}

/* ------------------------------------------------------------------------
 * caller side
 */

LONG
sb_rpc(struct SocketBase *sb, sbfn_t fn, void *args, int flags,
    ULONG *extrasigs)
{
	struct Task *me = SysBase->ThisTask;
	struct MsgPort *port, *tmp = NULL;
	struct sbcall call;
	ULONG portmask, waitmask, intrmask, got, s;
	int aborted = 0;

	if (sb->srvport == NULL) {
		sb_set_errno(sb, ENETDOWN);
		return -1;
	}
	/* bases belong to their opener; tolerate other tasks with a temp port */
	if (me == sb->owner && sb->replyport)
		port = sb->replyport;
	else if ((port = tmp = CreateMsgPort()) == NULL) {
		sb_set_errno(sb, ENOMEM);
		return -1;
	}

	call.msg.mn_Node.ln_Type = NT_MESSAGE;
	call.msg.mn_ReplyPort = port;
	call.msg.mn_Length = sizeof(call);
	call.fn = fn;
	call.args = args;
	call.result = -1;
	call.err = 0;
	call.quit = 0;
	call.seq = ++sb->callseq;

	portmask = 1UL << port->mp_SigBit;
	intrmask = (flags & RPC_INTERRUPTIBLE ? sb->breakmask : 0) |
	    (extrasigs ? *extrasigs : 0);
	got = 0;

	PutMsg(sb->srvport, &call.msg);
	for (;;) {
		waitmask = portmask | (aborted ? 0 : intrmask);
		s = Wait(waitmask);
		got |= s & intrmask;	/* Wait() consumed these: report them */
		if (GetMsg(port) == &call.msg)
			break;
		if (!aborted && (s & intrmask)) {
			aborted = 1;
			sb->abortseq = call.seq;
			Forbid();
			if (!sb->abortpending && mgrport) {
				sb->abortpending = 1;
				PutMsg(mgrport, &sb->abortmsg);
			}
			Permit();
		}
	}
	if (tmp)
		DeleteMsgPort(tmp);
	if (extrasigs) {
		*extrasigs = got & *extrasigs;
		got &= ~*extrasigs;	/* reported to the caller */
	}
	if (call.err) {
		sb_set_errno(sb, call.err);
		/* a break signal that interrupted us stays consumed (AmiTCP) */
		if (call.err == EINTR)
			got &= ~sb->breakmask;
	}
	/* Wait() took the others although the call did not report them
	   (it completed anyway): give them back, so that for example a
	   later CheckSignal(SIGBREAKF_CTRL_C) still sees Ctrl-C */
	if (got)
		SetSignal(got, got);
	return call.result;
}

/* ------------------------------------------------------------------------
 * the manager thread: creates server threads and pokes wake sockets
 */

static void *
manager_main(void *arg)
{
	struct mgrmsg *m;
	struct Message *msg;
	struct SocketBase *sb;
	struct sockaddr_in sin;
	struct MsgPort *port;
	char one = 1;

	if (rump_pub_lwproc_rfork(RUMP_RFCFDG) != 0 ||
	    (port = CreateMsgPort()) == NULL)
		return NULL;
	mgrsock = rump___sysimpl_socket30(AF_INET, SOCK_DGRAM, 0);
	mgrtask = SysBase->ThisTask;
	mgrport = port;

	for (;;) {
		WaitPort(port);
		while ((msg = GetMsg(port)) != NULL) {
			if (msg->mn_Length == 0) {
				/* an abort request: the base's embedded message */
				sb = (struct SocketBase *)msg->mn_Node.ln_Name;
				memset(&sin, 0, sizeof(sin));
				sin.sin_len = sizeof(sin);
				sin.sin_family = AF_INET;
				sin.sin_port = sb->wakeport;
				sin.sin_addr.s_addr = INADDR_LOOPBACK;
				sb->abortpending = 0;
				rump___sysimpl_sendto(mgrsock, &one, 1, 0, &sin,
				    sizeof(sin));
				continue;
			}
			m = (struct mgrmsg *)msg;
			switch (m->op) {
			case MGR_CREATE:
				sb = m->sb;
				m->result = rumpuser_thread_create(server_main,
				    sb, "bsdsocket server", 0, 0, -1, NULL);
				if (m->result == 0)
					while (sb->srvstate == 0)
						amiga_host_sleep_ms(5);
				if (sb->srvstate < 0)
					m->result = -1;
				break;
			case MGR_QUIT:
				ReplyMsg(msg);
				rump___sysimpl_close(mgrsock);
				rump_pub_lwproc_releaselwp();
				return NULL;
			}
			ReplyMsg(msg);
		}
	}
}

static LONG
mgr_request(int op, struct SocketBase *sb)
{
	struct MsgPort *port;
	struct mgrmsg m;

	if (mgrport == NULL || (port = CreateMsgPort()) == NULL)
		return -1;
	m.msg.mn_Node.ln_Type = NT_MESSAGE;
	m.msg.mn_ReplyPort = port;
	m.msg.mn_Length = sizeof(m);
	m.op = op;
	m.sb = sb;
	m.result = -1;
	PutMsg(mgrport, &m.msg);
	WaitPort(port);
	GetMsg(port);
	DeleteMsgPort(port);
	return m.result;
}

/* ------------------------------------------------------------------------
 * library vectors
 */

struct SocketBase *
sb_lib_open(struct SocketBase *mbase, ULONG version)
{
	struct SocketBase *sb;
	ULONG neg = mbase->lib.lib_NegSize;
	UBYTE *mem;

	if (!mgrport)
		return NULL;
	mem = AllocVec(neg + sizeof(struct SocketBase), MEMF_FAST | MEMF_CLEAR);
	if (mem == NULL)
		return NULL;
	/* the copy gets its own jump table and Library header */
	CopyMem((UBYTE *)mbase - neg, mem, neg + sizeof(struct Library));
	/* the table is code (JMP instructions): with copyback caches, or
	   Emu68's JIT, it must not run from stale cache contents */
	CacheClearE(mem, neg, CACRF_ClearI | CACRF_ClearD);
	sb = (struct SocketBase *)(mem + neg);
	sb->lib.lib_OpenCnt = 1;
	sb->master = mbase;
	sb->owner = SysBase->ThisTask;
	sb->breakmask = SIGBREAKF_CTRL_C;
	sb->dtablesize = SB_DEFAULT_DTABLESIZE;
	sb->wakefd = -1;
	sb->abortmsg.mn_Node.ln_Type = NT_MESSAGE;
	sb->abortmsg.mn_Node.ln_Name = (char *)sb;
	sb->abortmsg.mn_Length = 0;	/* marks an abort request */
	sb->replyport = CreateMsgPort();

	if (sb->replyport == NULL || mgr_request(MGR_CREATE, sb) != 0) {
		if (sb->replyport)
			DeleteMsgPort(sb->replyport);
		FreeVec(mem);
		return NULL;
	}
	mbase->lib.lib_OpenCnt++;
	mbase->lib.lib_Flags &= ~LIBF_DELEXP;
	return sb;
}

ULONG
sb_lib_close(struct SocketBase *sb)
{
	struct SocketBase *mbase = sb->master;
	struct MsgPort *port;
	struct sbcall call;

	if (sb->srvport && (port = CreateMsgPort()) != NULL) {
		call.msg.mn_Node.ln_Type = NT_MESSAGE;
		call.msg.mn_ReplyPort = port;
		call.msg.mn_Length = sizeof(call);
		call.quit = 1;
		PutMsg(sb->srvport, &call.msg);
		WaitPort(port);
		GetMsg(port);
		DeleteMsgPort(port);
	}
	/* an abort may still be queued at the manager: wait for it */
	while (sb->abortpending)
		Delay(1);
	if (sb->replyport)
		DeleteMsgPort(sb->replyport);
	FreeVec((UBYTE *)sb - mbase->lib.lib_NegSize);
	mbase->lib.lib_OpenCnt--;
	return 0;
}

ULONG
sb_lib_expunge(struct SocketBase *mbase)
{

	/* the stack removes the library itself when it shuts down */
	mbase->lib.lib_Flags |= LIBF_DELEXP;
	return 0;
}

LONG
sb_unimplemented(struct SocketBase *sb)
{

	sb_set_errno(sb, ENOSYS);
	return -1;
}

/* ------------------------------------------------------------------------
 * creation / removal by the stack
 */

struct Library *
bsdsocket_create(void)
{
	struct SocketBase *mb;

	if (master)
		return &master->lib;
	if (rumpuser_thread_create(manager_main, NULL, "bsdsocket manager",
	    1, 0, -1, &mgrthread) != 0)
		return NULL;
	while (mgrport == NULL)
		amiga_host_sleep_ms(5);

	mb = (struct SocketBase *)MakeLibrary((APTR)bsdsocket_vectors, NULL,
	    NULL, sizeof(struct SocketBase), 0);
	if (mb == NULL)
		return NULL;
	mb->lib.lib_Node.ln_Type = NT_LIBRARY;
	mb->lib.lib_Node.ln_Name = (char *)LIBNAME;
	mb->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	mb->lib.lib_Version = LIBVERSION;
	mb->lib.lib_Revision = LIBREVISION;
	mb->lib.lib_IdString = (APTR)LIBIDSTRING;
	mb->master = mb;
	master = mb;
	AddLibrary(&mb->lib);
	return &mb->lib;
}

/* remove the library if nobody has it open; returns 0 on success */
int
bsdsocket_remove(void)
{
	int rv = -1;

	if (master == NULL)
		return 0;
	Forbid();
	if (master->lib.lib_OpenCnt == 0) {
		Remove(&master->lib.lib_Node);
		rv = 0;
	}
	Permit();
	if (rv == 0) {
		FreeMem((UBYTE *)master - master->lib.lib_NegSize,
		    master->lib.lib_NegSize + master->lib.lib_PosSize);
		master = NULL;
		mgr_request(MGR_QUIT, NULL);
		amiga_host_thread_join(mgrthread);	/* not on a rump CPU */
		mgrport = NULL;
	}
	return rv;
}
