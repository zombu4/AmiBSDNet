/*
 * bsdsocket.library: library housekeeping, per-opener server threads and
 * the call mechanism (see sblib.h for the design).
 *
 * "the doc" below is the Roadshow autodoc,
 * downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc, and
 * "the SDK header" is
 * downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/netinclude/libraries/bsdsocket.h
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <utility/hooks.h>
#include <utility/utility.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "sblib.h"

extern struct ExecBase *SysBase;
extern ULONG bsdsocket_vectors[];

/* for CallHookPkt() (SBTC_ERROR_HOOK, SBTC_LOG_HOOK, monitoring hooks)
   and NextTagItem() */
struct UtilityBase *UtilityBase;

#define	LIBNAME		"bsdsocket.library"
#define	LIBVERSION	4
#define	LIBREVISION	1
#define	LIBIDSTRING	"bsdsocket.library 4.1 (AmiBSDNet, NetBSD 11 TCP/IP)\r\n"

static struct SocketBase *master;
static struct MsgPort *mgrport;
static void *mgrthread;
static LONG mgrsock = -1;
static volatile int mgrstate;	/* 0 starting, 1 running, -1 failed */

/* messages to the manager thread */
#define	MGR_CREATE	1
#define	MGR_QUIT	3

struct mgrmsg {
	struct Message msg;
	int op;
	struct sbserver *srv;
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
	int flags;		/* RPC_* */
};

static LONG mgr_request(int op, struct sbserver *);
static volatile int kernel_gone;	/* bsdsocket_kernel_gone() */

/* server states (sbserver.state) */
#define	SRV_STARTING	0
#define	SRV_RUNNING	1
#define	SRV_GONE	2
#define	SRV_FAILED	(-1)

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
sb_strncasecmp(const char *a, const char *b, size_t n)
{
	int ca = 0, cb = 0;

	while (n-- > 0) {
		ca = (UBYTE)*a++;
		cb = (UBYTE)*b++;
		if (ca >= 'A' && ca <= 'Z')
			ca += 32;
		if (cb >= 'A' && cb <= 'Z')
			cb += 32;
		if (ca == 0 || ca != cb)
			return ca - cb;
	}
	return 0;
}

int
sb_strcasecmp(const char *a, const char *b)
{

	return sb_strncasecmp(a, b, (size_t)-1);
}

/* decimal digits of v at p; returns the end (not terminated) */
char *
sb_fmt_ulong(char *p, ULONG v)
{
	char tmp[12];
	int i = 0;

	do {
		tmp[i++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (i)
		*p++ = tmp[--i];
	return p;
}

/*
 * SBTC_ERROR_HOOK (the doc, SocketBaseTagList: a hook "which is called
 * whenever the global" errno or h_errno variables change, called on the
 * caller's context); struct ErrorHookMsg, EHMA_Set_errno (1) and
 * EHMA_Set_h_errno (2) from the SDK header.
 */
struct ErrorHookMsg {
	ULONG ehm_Size;
	ULONG ehm_Action;
	LONG ehm_Code;
};

static void
call_error_hook(struct SocketBase *sb, ULONG action, LONG code)
{
	struct ErrorHookMsg m;

	if (sb->errorhook == NULL || UtilityBase == NULL)
		return;
	m.ehm_Size = sizeof(m);
	m.ehm_Action = action;
	m.ehm_Code = code;
	CallHookPkt(sb->errorhook, NULL, &m);
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
	call_error_hook(sb, 1, e);
}

void
sb_set_herrno(struct SocketBase *sb, LONG e)
{

	sb->sb_herrno = e;
	if (sb->herrnoptr)
		*sb->herrnoptr = e;
	call_error_hook(sb, 2, e);
}

LONG
sb_rumperr(void)
{

	return amiga_rump_errno();
}


/*
 * Tag list iteration as utility.library NextTagItem() does it
 * (downloads/sources/NDK3.2/Autodocs/utility.doc: TAG_SKIP skips the
 * entry and ti_Data more, TAG_IGNORE that one entry, TAG_MORE goes on
 * with the array ti_Data points to, TAG_DONE ends the array).  The NDK's
 * inline macro for NextTagItem() does not compile with this compiler
 * (inline/macros.h LP1() turns the struct TagItem ** argument into a
 * pointer to a volatile one).
 */
struct TagItem *
sb_next_tag(struct TagItem **tp)
{
	struct TagItem *t = *tp;

	for (;;) {
		if (t == NULL)
			return NULL;
		switch (t->ti_Tag) {
		case TAG_DONE:
			*tp = NULL;
			return NULL;
		case TAG_IGNORE:
			t++;
			continue;
		case TAG_MORE:
			t = (struct TagItem *)t->ti_Data;
			continue;
		case TAG_SKIP:
			t += t->ti_Data + 1;
			continue;
		}
		*tp = t + 1;
		return t;
	}
}

/* exec lists: an empty list (exec/lists.h, the lh_Head/lh_Tail/
   lh_TailPred layout) */
void
sb_newlist(struct List *l)
{

	l->lh_Head = (struct Node *)&l->lh_Tail;
	l->lh_Tail = NULL;
	l->lh_TailPred = (struct Node *)&l->lh_Head;
}

/*
 * The opener's reply port, made here rather than with CreateMsgPort()
 * ("You *must* use DeleteMsgPort() to delete ports created with
 * CreateMsgPort()", exec.doc CreateMsgPort): the base may be closed by
 * another task sharing it, which can give back the port's memory but not
 * its signal bit ("This call must be performed while running in the same
 * task in which the signal was allocated", exec.doc FreeSignal).  The
 * fields as CreateMsgPort() sets them: NewList, a signal bit, PA_SIGNAL
 * to this task (exec.doc CreateMsgPort; exec/ports.h).
 */
static struct MsgPort *
reply_port_new(void)
{
	struct MsgPort *p;
	BYTE sig;

	if ((sig = AllocSignal(-1)) == -1)
		return NULL;
	if ((p = AllocVec(sizeof(*p), MEMF_PUBLIC | MEMF_CLEAR)) == NULL) {
		FreeSignal(sig);
		return NULL;
	}
	p->mp_Node.ln_Type = NT_MSGPORT;
	p->mp_Flags = PA_SIGNAL;
	p->mp_SigBit = sig;
	p->mp_SigTask = SysBase->ThisTask;
	sb_newlist(&p->mp_MsgList);
	return p;
}

/* its signal bit only by the task that has it (the owner) */
static void
reply_port_free(struct SocketBase *sb)
{

	if (sb->replyport == NULL)
		return;
	if (SysBase->ThisTask == sb->owner)
		FreeSignal(sb->replyport->mp_SigBit);
	FreeVec(sb->replyport);
	sb->replyport = NULL;
}

/* ------------------------------------------------------------------------
 * timeouts
 */

void
sbtime_from_tv(struct sbtime *t, ULONG secs, ULONG micro)
{

	t->s = secs + micro / 1000000;
	micro %= 1000000;
	t->ms = micro / 1000 + (micro % 1000 != 0);
	if (t->ms >= 1000) {
		t->s++;
		t->ms -= 1000;
	}
}

int
sbtime_iszero(const struct sbtime *t)
{

	return t->s == 0 && t->ms == 0;
}

/* the next poll() timeout: all of it, or a large piece of a long one */
LONG
sbtime_chunk(const struct sbtime *t)
{

	if (t->s >= 2000000)		/* 2e9 ms fits into a LONG */
		return 2000000000L;
	return (LONG)(t->s * 1000 + t->ms);
}

void
sbtime_sub(struct sbtime *t, ULONG ms)
{
	ULONG s = ms / 1000;

	ms %= 1000;
	if (t->ms < ms) {
		if (t->s == 0) {
			t->ms = 0;
			return;
		}
		t->s--;
		t->ms += 1000;
	}
	t->ms -= ms;
	if (t->s < s) {
		t->s = 0;
		t->ms = 0;
	} else
		t->s -= s;
}

/* ------------------------------------------------------------------------
 * server thread: one per library base, owns a NetBSD process
 */

/* move a kernel descriptor to SB_PRIVFD or above, out of the table */
static LONG
move_private(LONG fd)
{
	LONG nfd;

	if (fd >= SB_PRIVFD)
		return fd;
	nfd = rump___sysimpl_fcntl(fd, NB_F_DUPFD, SB_PRIVFD);
	rump___sysimpl_close(fd);
	return nfd;
}

static LONG
make_wake_socket(struct sbserver *srv)
{
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	LONG s, on = 1;

	if ((s = rump___sysimpl_socket30(AF_INET, SOCK_DGRAM, 0)) < 0)
		return -1;
	if ((s = move_private(s)) < 0)
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
	srv->wakeport = sin.sin_port;
	return s;
}

/* server side: the server thread of sb that is running (NULL if this is
   none of them) */
struct sbserver *
sb_srv(struct SocketBase *sb)
{
	struct Task *me = SysBase->ThisTask;
	struct sbserver *s;

	if (sb->main.task == me)
		return &sb->main;
	Forbid();
	for (s = sb->extra; s && s->task != me; s = s->next)
		;
	Permit();
	return s;
}

void
sb_drain_wake(struct SocketBase *sb)
{
	struct sbserver *srv = sb_srv(sb);
	char buf[16];

	if (srv == NULL)
		return;
	while (rump___sysimpl_recvfrom(srv->wakefd, buf, sizeof(buf), 0,
	    NULL, NULL) > 0)
		;
}

static void *
server_main(void *arg)
{
	struct sbserver *srv = arg;
	struct SocketBase *sb = srv->sb;
	int ismain = srv == &sb->main;
	struct MsgPort *port;
	struct sbcall *call;
	int fd;

	/* the main thread makes the base's process, the others join it
	   (netbsd-src/sys/rump/librump/rumpkern/lwproc.c:394-418
	   rump_lwproc_newlwp()) */
	if ((ismain ? rump_pub_lwproc_rfork(RUMP_RFCFDG) :
	    rump_pub_lwproc_newlwp(sb->pid)) != 0) {
		srv->state = SRV_FAILED;
		return NULL;
	}
	if ((port = CreateMsgPort()) == NULL) {
		rump_pub_lwproc_releaselwp();
		srv->state = SRV_FAILED;
		return NULL;
	}
	/* the process's descriptor limit starts at 128 (OPEN_MAX): raise it
	   above the table (SB_MAXFD) and the base's own sockets */
	if (ismain) {
		struct { struct nb_u64 cur, max; } rl;

		if (rump___sysimpl_getrlimit(NB_RLIMIT_NOFILE, &rl) == 0 &&
		    rl.cur.hi == 0 && rl.cur.lo < SB_PRIVFD + 64) {
			if (rl.max.hi == 0 && rl.max.lo < SB_PRIVFD + 64)
				rl.cur.lo = rl.max.lo;
			else
				rl.cur.lo = SB_PRIVFD + 64;
			rump___sysimpl_setrlimit(NB_RLIMIT_NOFILE, &rl);
		}
	}
	if ((srv->wakefd = make_wake_socket(srv)) < 0) {
		DeleteMsgPort(port);
		rump_pub_lwproc_releaselwp();
		srv->state = SRV_FAILED;
		return NULL;
	}
	if (ismain)
		sb->pid = rump___sysimpl_getpid();
	srv->task = SysBase->ThisTask;
	srv->port = port;
	/* (port stored before state says it is there) */
	__asm__ volatile ("" : : : "memory");
	srv->state = SRV_RUNNING;

	for (;;) {
		WaitPort(port);
		while ((call = (struct sbcall *)GetMsg(port)) != NULL) {
			if (call->quit)
				goto quit;
			sb_drain_wake(sb);
			call->err = 0;
			/* (callseq first: server_quit() reads both) */
			srv->callseq = call->seq;
			__asm__ volatile ("" : : : "memory");
			srv->curcall = call;
			if (!(call->flags & RPC_INTERRUPTIBLE))
				ObtainSemaphore(&sb->calllock);
			call->result = call->fn(sb, call->args);
			if (!(call->flags & RPC_INTERRUPTIBLE))
				ReleaseSemaphore(&sb->calllock);
			srv->curcall = NULL;
			ReplyMsg(&call->msg);
		}
	}
quit:
	if (ismain) {
		/* the event thread first: it polls this process's
		   descriptors */
		ev_stop(sb);
		/* close everything this base still holds (the other server
		   threads are gone), then leave the process */
		for (fd = 0; fd < SB_MAXFD; fd++)
			if (sb->fds[fd].inuse)
				rump___sysimpl_close(fd);
	}
	rump___sysimpl_close(srv->wakefd);
	rump_pub_lwproc_releaselwp();
	srv->port = NULL;
	DeleteMsgPort(port);
	/* the closer polls for this and then frees it: the last access */
	__asm__ volatile ("" : : : "memory");
	srv->state = SRV_GONE;
	return NULL;
}

/*
 * Server functions fail a call with "return sb_fail(sb, errno);".
 */
LONG
sb_fail(struct SocketBase *sb, LONG err)
{
	struct sbserver *srv = sb_srv(sb);

	if (srv && srv->curcall)
		srv->curcall->err = err;
	return -1;
}

int
sb_interrupted(struct SocketBase *sb)
{
	struct sbserver *srv = sb_srv(sb);

	return srv && srv->curcall && srv->curcall->err == EINTR;
}

int
sb_fdok(struct SocketBase *sb, LONG fd)
{

	return fd >= 0 && fd < SB_MAXFD && sb->fds[fd].inuse;
}

/*
 * Wait (server side) until fd is readable/writable, the timeout (NULL:
 * none) expires (EWOULDBLOCK, as BSD reports SO_RCVTIMEO expiry) or the
 * call is aborted (EINTR).  Returns 0 when the fd is ready.
 */
LONG
sb_wait_fd(struct SocketBase *sb, LONG fd, int what,
    const struct sbtime *timeout)
{
	struct sbserver *srv = sb_srv(sb);
	struct nb_pollfd pfd[2];
	struct sbtime left = { 0, 0 };
	LONG n, chunk;
	ULONG start;

	if (srv == NULL)
		return EINVAL;		/* (not on a server thread) */
	if (timeout)
		left = *timeout;
	for (;;) {
		/* an abort that came before the call started: its wake
		   datagram was drained at the start of the call */
		if (srv->abortseq == srv->callseq)
			return EINTR;
		pfd[0].fd = fd;
		pfd[0].events = (what & WAIT_READ ? NB_POLLIN : 0) |
		    (what & WAIT_WRITE ? NB_POLLOUT : 0) |
		    (what & WAIT_PRI ? NB_POLLPRI : 0);
		pfd[0].revents = 0;
		pfd[1].fd = srv->wakefd;
		pfd[1].events = NB_POLLIN;
		pfd[1].revents = 0;
		chunk = timeout ? sbtime_chunk(&left) : -1;
		start = amiga_host_ms();
		n = rump___sysimpl_poll(pfd, 2, chunk);
		if (n < 0)
			return sb_rumperr();
		if (timeout) {
			sbtime_sub(&left, amiga_host_ms() - start);
			if (n == 0 && sbtime_iszero(&left))
				return EWOULDBLOCK;
		}
		if (pfd[1].revents) {
			sb_drain_wake(sb);
			if (srv->abortseq == srv->callseq)
				return EINTR;
		}
		if (pfd[0].revents)
			return 0;
	}
}

/*
 * The server thread for a call by 'me' on a shared base that is not its
 * opener's: the one it has, or a new one (NULL if none can be had).
 */
/* (under Forbid) is t a task of the system: the running one, or in the
   ready or waiting list (NDK3.2 Include_H/exec/execbase.h:89-90
   TaskReady, TaskWait) */
static int
task_alive(struct Task *t)
{
	struct Node *n;

	if (SysBase->ThisTask == t)
		return 1;
	for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
		if (n == &t->tc_Node)
			return 1;
	for (n = SysBase->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
		if (n == &t->tc_Node)
			return 1;
	return 0;
}

static struct sbserver *
task_server(struct SocketBase *sb, struct Task *me)
{
	struct sbserver *s;

	Forbid();
	for (s = sb->extra; s && s->client != me; s = s->next)
		;
	/* else the idle one of a task that has ended: each server keeps a
	   descriptor of the base's process (its wake socket), so they are
	   not made anew for every task that ever called */
	if (s == NULL)
		for (s = sb->extra; s; s = s->next)
			if (s->callers == 0 && s->curcall == NULL &&
			    !task_alive(s->client)) {
				s->client = me;
				break;
			}
	Permit();
	if (s)
		return s;
	if ((s = AllocVec(sizeof(*s), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return NULL;
	s->sb = sb;
	s->client = me;
	s->wakefd = -1;
	s->state = SRV_STARTING;
	s->abortmsg.mn_Node.ln_Type = NT_MESSAGE;
	s->abortmsg.mn_Node.ln_Name = (char *)s;
	s->abortmsg.mn_Length = 0;	/* marks an abort request */
	if (mgr_request(MGR_CREATE, s) != 0) {
		FreeVec(s);
		return NULL;
	}
	Forbid();
	s->next = sb->extra;
	sb->extra = s;
	Permit();
	return s;
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
	struct sbserver *srv = &sb->main;

	if (sb->main.port == NULL || kernel_gone) {
		sb_set_errno(sb, ENETDOWN);
		return -1;
	}
	/* a shared base: each task has a server thread of its own */
	if (sb->share && me != sb->owner &&
	    (srv = task_server(sb, me)) == NULL) {
		sb_set_errno(sb, ENOMEM);
		return -1;
	}
	/* bases belong to their opener; other tasks (a base shared with
	   SBTC_CAN_SHARE_LIBRARY_BASES) use a port of their own */
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
	call.flags = flags;
	/* unique also when several tasks share the base: an abort names
	   the call it is meant for */
	Forbid();
	call.seq = ++sb->seqgen;
	Permit();

	portmask = 1UL << port->mp_SigBit;
	intrmask = (flags & RPC_INTERRUPTIBLE ? sb->breakmask : 0) |
	    (extrasigs ? *extrasigs : 0);
	got = 0;

	Forbid();
	srv->callers++;		/* (server_quit() waits for it to be 0) */
	Permit();
	PutMsg(srv->port, &call.msg);
	for (;;) {
		waitmask = portmask | (aborted ? 0 : intrmask);
		s = Wait(waitmask);
		got |= s & intrmask;	/* Wait() consumed these: report them */
		if (GetMsg(port) == &call.msg)
			break;
		if (!aborted && (s & intrmask)) {
			aborted = 1;
			srv->abortseq = call.seq;
			Forbid();
			if (!srv->abortpending && mgrport) {
				srv->abortpending = 1;
				PutMsg(mgrport, &srv->abortmsg);
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
	if (call.err)
		sb_set_errno(sb, call.err);
	/*
	 * Wait() took the break signal (and others the call did not
	 * report): give them back.  An interrupted call leaves the break
	 * signal set: the doc, WaitSelect NOTES, "the standard break signal
	 * will be posted", so it is still set on return and can be tested;
	 * a later CheckSignal(SIGBREAKF_CTRL_C) still sees Ctrl-C.
	 */
	if (got)
		SetSignal(got, got);
	/* the last access to sb and srv: CloseLibrary() by another task
	   (server_quit()) frees them once callers is 0 */
	Forbid();
	srv->callers--;
	Permit();
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
	struct sbserver *srv;
	struct sockaddr_in sin;
	struct MsgPort *port;
	char one = 1;

	if (rump_pub_lwproc_rfork(RUMP_RFCFDG) != 0) {
		mgrstate = -1;
		return NULL;
	}
	/* (without its socket no abort could reach a blocked call) */
	if ((port = CreateMsgPort()) == NULL ||
	    (mgrsock = rump___sysimpl_socket30(AF_INET, SOCK_DGRAM, 0)) < 0) {
		if (port)
			DeleteMsgPort(port);
		rump_pub_lwproc_releaselwp();
		mgrstate = -1;
		return NULL;
	}
	mgrport = port;
	mgrstate = 1;

	for (;;) {
		WaitPort(port);
		while ((msg = GetMsg(port)) != NULL) {
			if (msg->mn_Length == 0) {
				/* an abort request: the server thread's
				   embedded message */
				srv = (struct sbserver *)msg->mn_Node.ln_Name;
				memset(&sin, 0, sizeof(sin));
				sin.sin_len = sizeof(sin);
				sin.sin_family = AF_INET;
				sin.sin_port = srv->wakeport;
				sin.sin_addr.s_addr = INADDR_LOOPBACK;
				srv->abortpending = 0;
				rump___sysimpl_sendto(mgrsock, &one, 1, 0, &sin,
				    sizeof(sin));
				continue;
			}
			m = (struct mgrmsg *)msg;
			switch (m->op) {
			case MGR_CREATE:
				srv = m->srv;
				m->result = rumpuser_thread_create(server_main,
				    srv, "bsdsocket server", 0, 0, -1, NULL);
				if (m->result == 0)
					while (srv->state == SRV_STARTING)
						amiga_host_sleep_ms(5);
				if (srv->state != SRV_RUNNING)
					m->result = -1;
				break;
			case MGR_QUIT:
				mgrport = NULL;
				ReplyMsg(msg);
				rump___sysimpl_close(mgrsock);
				rump_pub_lwproc_releaselwp();
				DeleteMsgPort(port);
				return NULL;
			}
			ReplyMsg(msg);
		}
	}
}

static LONG
mgr_request(int op, struct sbserver *srv)
{
	struct MsgPort *port;
	struct mgrmsg m;

	if (mgrport == NULL || (port = CreateMsgPort()) == NULL)
		return -1;
	m.msg.mn_Node.ln_Type = NT_MESSAGE;
	m.msg.mn_ReplyPort = port;
	m.msg.mn_Length = sizeof(m);
	m.op = op;
	m.srv = srv;
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

	/* counted before anything waits: the master base is in use from
	   here on, also while the manager creates the server thread */
	mbase->lib.lib_OpenCnt++;
	mbase->lib.lib_Flags &= ~LIBF_DELEXP;
	/* (after a panic the new server thread's rump_pub_lwproc_rfork(),
	   server_main(), would wait for ever for a rump CPU: no new base) */
	if (!mgrport || kernel_gone)
		goto fail;
	mem = AllocVec(neg + sizeof(struct SocketBase), MEMF_PUBLIC | MEMF_CLEAR);
	if (mem == NULL)
		goto fail;
	/* the copy gets its own jump table and Library header */
	CopyMem((UBYTE *)mbase - neg, mem, neg + sizeof(struct Library));
	/* the table is code (JMP instructions): with copyback caches, or
	   Emu68's JIT, it must not run from stale cache contents */
	CacheClearE(mem, neg, CACRF_ClearI | CACRF_ClearD);
	sb = (struct SocketBase *)(mem + neg);
	sb->lib.lib_OpenCnt = 1;
	sb->master = mbase;
	sb->owner = SysBase->ThisTask;
	/* the doc, SocketBaseTagList: SBTC_BREAKMASK "By default, this is
	   SIGBREAKF_CTRL_C"; SBTC_LOGMASK: by default the mask is 0xFF;
	   SBTC_LOGFACILITY "Defaults to LOG_USER", which is (1<<3) in
	   netbsd-src/sys/sys/syslog.h (the SDK's sys/syslog.h has the
	   priorities only) */
	sb->breakmask = SIGBREAKF_CTRL_C;
	sb->dtablesize = SB_DEFAULT_DTABLESIZE;
	sb->logmask = 0xff;
	sb->logfacility = 1 << 3;
	sb->main.sb = sb;
	sb->main.wakefd = -1;
	sb->main.state = SRV_STARTING;
	InitSemaphore(&sb->evlock);
	InitSemaphore(&sb->calllock);
	sb->main.abortmsg.mn_Node.ln_Type = NT_MESSAGE;
	sb->main.abortmsg.mn_Node.ln_Name = (char *)&sb->main;
	sb->main.abortmsg.mn_Length = 0;	/* marks an abort request */
	sb->replyport = reply_port_new();

	if (sb->replyport == NULL || mgr_request(MGR_CREATE, &sb->main) != 0) {
		reply_port_free(sb);
		FreeVec(mem);
		goto fail;
	}
	return sb;
fail:
	mbase->lib.lib_OpenCnt--;
	return NULL;
}

/* a server thread ends (and an abort for it still queued at the manager
   is waited for) */
static void
server_quit(struct sbserver *srv)
{
	struct sbcall call;

	if (srv->port) {
		/* no reply port (none could be had, and none is needed): the
		   server says it is done in state */
		call.msg.mn_Node.ln_Type = NT_MESSAGE;
		call.msg.mn_ReplyPort = NULL;
		call.msg.mn_Length = sizeof(call);
		call.quit = 1;
		PutMsg(srv->port, &call.msg);
		/* the calls still under way or queued before the quit
		   (another task's, on a shared base) may wait for ever: each
		   is aborted as a break signal would (sb_rpc()) and returns
		   EINTR.  (Under Forbid(): server_main() sets callseq before
		   curcall.) */
		while (srv->state != SRV_GONE) {
			Forbid();
			if (srv->curcall && srv->abortseq != srv->callseq) {
				srv->abortseq = srv->callseq;
				if (!srv->abortpending && mgrport) {
					srv->abortpending = 1;
					PutMsg(mgrport, &srv->abortmsg);
				}
			}
			Permit();
			Delay(1);
		}
	}
	while (srv->abortpending)
		Delay(1);
	/* the tasks whose calls were answered have left sb_rpc() (which
	   counts itself in srv->callers): after this nothing of theirs uses
	   srv or the base */
	while (srv->callers > 0)
		Delay(1);
}

ULONG
sb_lib_close(struct SocketBase *sb)
{
	struct SocketBase *mbase = sb->master;
	struct sbserver *s;

	/* the monitoring hooks this base installed point into its program */
	mon_base_closed(sb);
	log_base_closed(sb);	/* (and its log hook) */
	netdb_base_closed(sb);
	/* the kernel has stopped (bsdsocket_kernel_gone()): the server
	   threads would end with kernel calls that never return, so they
	   and the base's memory are left as they are */
	if (kernel_gone) {
		reply_port_free(sb);
		mbase->lib.lib_OpenCnt--;
		return 0;
	}
	/* the other tasks' server threads first: the main one closes the
	   base's descriptors and ends the process */
	while ((s = sb->extra) != NULL) {
		sb->extra = s->next;
		server_quit(s);
		FreeVec(s);
	}
	server_quit(&sb->main);
	reply_port_free(sb);
	FreeVec((UBYTE *)sb - mbase->lib.lib_NegSize);
	mbase->lib.lib_OpenCnt--;
	return 0;
}

/*
 * The library's code is part of the stack program, which keeps running
 * (src/stack/main.c has no shutdown path) and cannot be unloaded: an
 * expunge is refused and the library stays in the system list.
 */
ULONG
sb_lib_expunge(struct SocketBase *mbase)
{

	return 0;
}

LONG
sb_unimplemented(struct SocketBase *sb)
{

	sb_set_errno(sb, ENOSYS);
	return -1;
}

/* ------------------------------------------------------------------------
 * creation by the stack
 */

static void
stop_manager(void)
{

	if (mgrport)
		mgr_request(MGR_QUIT, NULL);
	if (mgrthread)
		amiga_host_thread_join(mgrthread);	/* not on a rump CPU */
	mgrthread = NULL;
	mgrstate = 0;
}

/*
 * The kernel has stopped (src/stack/main.c kernel_gone()): no call may go
 * to a server thread any more, they would never be answered; sb_rpc()
 * fails them with ENETDOWN.  (A call already under way stays where it
 * is.)
 */
void
bsdsocket_kernel_gone(void)
{

	kernel_gone = 1;
}

struct Library *
bsdsocket_create(void)
{
	struct SocketBase *mb;

	if (master)
		return &master->lib;
	if (UtilityBase == NULL &&
	    (UtilityBase = (struct UtilityBase *)OpenLibrary(
	    (CONST_STRPTR)"utility.library", 37)) == NULL)
		return NULL;
	held_init();
	netdb_init();
	mon_init();
	if_init();
	stats_init();
	if (rumpuser_thread_create(manager_main, NULL, "bsdsocket manager",
	    1, 0, -1, &mgrthread) != 0) {
		mgrthread = NULL;
		return NULL;
	}
	/* (Delay, not the rump timer: that needs nothing that can fail;
	   manager_main() sets mgrstate on every path) */
	while (mgrstate == 0)
		Delay(1);
	if (mgrstate != 1) {
		/* it failed and has returned: collect the thread */
		stop_manager();
		return NULL;
	}

	mb = (struct SocketBase *)MakeLibrary((APTR)bsdsocket_vectors, NULL,
	    NULL, sizeof(struct SocketBase), 0);
	if (mb == NULL) {
		stop_manager();
		return NULL;
	}
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
