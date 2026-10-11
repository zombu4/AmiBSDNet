/*
 * bsdsocket.library: asynchronous socket events.
 *
 * SO_EVENTMASK / GetSocketEvents() / SBTC_SIGEVENTMASK, FIOASYNC /
 * SBTC_SIGIOMASK, SBTC_SIGURGMASK and SBTC_SIG_ADDRESS_CHANGE_MASK, as the
 * doc describes them (downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/
 * bsdsocket.doc: GetSocketEvents, IoctlSocket, SetSocketSignals,
 * SocketBaseTagList).
 *
 * A base that uses any of them gets an event thread: a host thread that
 * joins the base's NetBSD process as a second LWP (rump_lwproc_newlwp(),
 * netbsd-src/sys/rump/include/rump/rumpkern_if_pub.h), so the base's
 * descriptors are its descriptors, and waits in the kernel's poll() for
 * the conditions that may be reported.  poll() reports a state, an event
 * is a change: a condition is "armed" until it has been reported and is
 * armed again by the call that consumes it (a receive for FD_READ and
 * FD_OOB, accept() for FD_ACCEPT, a send that had to wait for FD_WRITE).
 * The server thread tells the event thread about such changes with a
 * datagram to its control socket.
 *
 * What a condition means, from the doc (GetSocketEvents):
 *   FD_ACCEPT  a listening socket is readable (a connection is pending);
 *              again after each accept() while more are pending
 *   FD_CONNECT a non-blocking connect() completed: writable and connected
 *   FD_READ    readable with data (a peek returns data)
 *   FD_CLOSE   a stream socket is readable at end of file; with an error
 *              FD_ERROR as well
 *   FD_ERROR   an error is pending on the socket: a peek (MSG_PEEK)
 *              returns it without clearing it, as the doc requires
 *              ("The error code associated with the socket is not
 *              cleared"); GetSocketEvents() sets errno to it
 *   FD_OOB     out-of-band data (POLLPRI)
 *   FD_WRITE   writable again after a non-blocking send had to stop
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

#include "sblib.h"

struct sbevents {
	void *thread;
	LONG ctlfd;			/* the thread's control socket */
	UWORD ctlport;
	LONG routefd;			/* for address changes, or -1 */
	volatile int state;		/* 0 starting, 1 running, -1 failed */
	volatile int quit;
};

#define	EV_RUNNING	1
#define	EV_FAILED	(-1)

static void
ev_lock(struct SocketBase *sb)
{

	ObtainSemaphore(&sb->evlock);
}

static void
ev_unlock(struct SocketBase *sb)
{

	ReleaseSemaphore(&sb->evlock);
}

/* does the fd need watching at all? (evlock held) */
static int
watched(struct SocketBase *sb, struct sbfd *f)
{

	return f->inuse && (f->eventmask || f->async ||
	    (sb->sigurgmask && f->type == SOCK_STREAM));
}

/* queue an event for fd (evlock held); returns 1 if the fd is new to the
   queue, then the event signal is due */
static int
post(struct SocketBase *sb, LONG fd, ULONG events)
{
	struct sbfd *f = &sb->fds[fd];

	events &= f->eventmask;
	if (!events)
		return 0;
	f->pending |= events;
	if (f->queued)
		return 0;
	f->queued = 1;
	sb->evqueue[(sb->evhead + sb->evcount) % SB_MAXFD] = (UBYTE)fd;
	sb->evcount++;
	return 1;
}

/* take fd out of the queue (evlock held) */
static void
unqueue(struct SocketBase *sb, LONG fd)
{
	int i, j, n = sb->evcount;
	UBYTE keep[SB_MAXFD];

	if (!sb->fds[fd].queued)
		return;
	for (i = j = 0; i < n; i++) {
		UBYTE q = sb->evqueue[(sb->evhead + i) % SB_MAXFD];

		if (q != fd)
			keep[j++] = q;
	}
	for (i = 0; i < j; i++)
		sb->evqueue[i] = keep[i];
	sb->evhead = 0;
	sb->evcount = j;
	sb->fds[fd].queued = 0;
}

/* ------------------------------------------------------------------------
 * the event thread
 */

static LONG
move_private(LONG fd)
{
	LONG nfd;

	if (fd < 0 || fd >= SB_PRIVFD)
		return fd;
	nfd = rump___sysimpl_fcntl(fd, NB_F_DUPFD, SB_PRIVFD);
	rump___sysimpl_close(fd);
	return nfd;
}

static int
ctl_socket(struct sbevents *ev)
{
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	LONG s, on = 1;

	if ((s = move_private(rump___sysimpl_socket30(AF_INET, SOCK_DGRAM,
	    0))) < 0)
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
	ev->ctlfd = s;
	ev->ctlport = sin.sin_port;
	return 0;
}

/* the routing socket for SBTC_SIG_ADDRESS_CHANGE_MASK (event thread) */
static void
route_socket(struct SocketBase *sb, struct sbevents *ev)
{
	LONG on = 1;

	if (sb->sigaddrmask && ev->routefd < 0) {
		ev->routefd = move_private(rump___sysimpl_socket30(NB_PF_ROUTE,
		    SOCK_RAW, AF_UNSPEC));
		if (ev->routefd >= 0)
			rump___sysimpl_ioctl(ev->routefd, FIONBIO, &on);
	} else if (!sb->sigaddrmask && ev->routefd >= 0) {
		rump___sysimpl_close(ev->routefd);
		ev->routefd = -1;
	}
}

/* an address was added or removed: NetBSD's RTM_NEWADDR/RTM_DELADDR
   (netbsd-src/sys/net/route.h) */
static int
address_changed(struct sbevents *ev)
{
	UBYTE buf[512];
	LONG n;
	int changed = 0;

	while ((n = rump___sysimpl_recvfrom(ev->routefd, buf, sizeof(buf), 0,
	    NULL, NULL)) > 0)
		if (n >= 4 && (buf[3] == NB_RTM_NEWADDR ||
		    buf[3] == NB_RTM_DELADDR))
			changed = 1;
	return changed;
}

/* what is to be learnt about a readable socket by peeking (event
   thread): >0 data, 0 end of file, <0 -error, or EWOULDBLOCK negated */
static LONG
peek(LONG fd)
{
	UBYTE c;
	LONG n;

	n = rump___sysimpl_recvfrom(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT, NULL,
	    NULL);
	if (n >= 0)
		return n;
	return -sb_rumperr();
}

static void *
ev_main(void *arg)
{
	struct SocketBase *sb = arg;
	struct sbevents *ev = sb->ev;
	struct nb_pollfd pfd[SB_MAXFD + 2];
	LONG fds[SB_MAXFD];
	LONG n, i, np, nfd;
	ULONG sigs;
	char buf[16];

	/* a second LWP of the base's process: the same descriptors */
	if (rump_pub_lwproc_newlwp(sb->pid) != 0) {
		ev->state = EV_FAILED;
		return NULL;
	}
	if (ctl_socket(ev) != 0) {
		rump_pub_lwproc_releaselwp();
		ev->state = EV_FAILED;
		return NULL;
	}
	ev->routefd = -1;
	__asm__ volatile ("" : : : "memory");
	ev->state = EV_RUNNING;

	while (!ev->quit) {
		/* what to wait for, from the current state */
		route_socket(sb, ev);
		np = 0;
		pfd[np].fd = ev->ctlfd;
		pfd[np].events = NB_POLLIN;
		pfd[np].revents = 0;
		np++;
		if (ev->routefd >= 0) {
			pfd[np].fd = ev->routefd;
			pfd[np].events = NB_POLLIN;
			pfd[np].revents = 0;
			np++;
		}
		nfd = 0;
		ev_lock(sb);
		for (i = 0; i < SB_MAXFD; i++) {
			struct sbfd *f = &sb->fds[i];
			ULONG want = f->armed & f->eventmask;
			WORD e = 0;

			if (!watched(sb, f))
				continue;
			if (want & (FD_READ | FD_ACCEPT | FD_CLOSE | FD_ERROR))
				e |= NB_POLLIN;
			if (want & (FD_WRITE | FD_CONNECT))
				e |= NB_POLLOUT;
			if (want & FD_OOB)
				e |= NB_POLLPRI;
			if (f->async && (f->sigio_armed & SIGIO_READ))
				e |= NB_POLLIN;
			if (f->async && (f->sigio_armed & SIGIO_WRITE))
				e |= NB_POLLOUT;
			if (sb->sigurgmask && (f->sigio_armed & SIGIO_URG))
				e |= NB_POLLPRI;
			if (!e)
				continue;
			pfd[np].fd = i;
			pfd[np].events = e;
			pfd[np].revents = 0;
			fds[nfd++] = np++;
		}
		ev_unlock(sb);

		n = rump___sysimpl_poll(pfd, np, -1);
		if (n <= 0)
			continue;
		if (pfd[0].revents)
			while (rump___sysimpl_recvfrom(ev->ctlfd, buf,
			    sizeof(buf), 0, NULL, NULL) > 0)
				;
		sigs = 0;
		if (ev->routefd >= 0 && pfd[1].revents && address_changed(ev))
			sigs |= sb->sigaddrmask;

		for (i = 0; i < nfd; i++) {
			struct nb_pollfd *p = &pfd[fds[i]];
			LONG fd = p->fd, pk = 0;
			struct sbfd *f = &sb->fds[fd];
			ULONG got = 0, want;
			int havepeek = 0;

			if (!p->revents || (p->revents & NB_POLLNVAL))
				continue;
			/* (outside the lock: a kernel call) */
			if ((p->revents & (NB_POLLIN | NB_POLLHUP | NB_POLLERR))
			    && !f->listening) {
				pk = peek(fd);
				havepeek = 1;
			}
			ev_lock(sb);
			if (!f->inuse) {
				ev_unlock(sb);
				continue;
			}
			want = f->armed & f->eventmask;
			if (p->revents & (NB_POLLIN | NB_POLLHUP | NB_POLLERR)) {
				if (f->listening)
					got |= FD_ACCEPT;
				else if (havepeek && pk > 0)
					got |= FD_READ;
				else if (havepeek && pk == 0) {
					if (f->type == SOCK_STREAM)
						got |= FD_CLOSE;
					else
						got |= FD_READ;	/* empty datagram */
				} else if (havepeek && pk != -EWOULDBLOCK) {
					got |= FD_ERROR;
					f->pend_error = -pk;
					if (f->type == SOCK_STREAM &&
					    !f->connecting)
						got |= FD_CLOSE;
				}
			}
			if (p->revents & (NB_POLLOUT | NB_POLLERR | NB_POLLHUP)) {
				if (f->connecting) {
					struct sockaddr_in6 peer;
					socklen_t len = sizeof(peer);

					f->connecting = 0;
					/* (a short kernel call under the
					   semaphore: the fd cannot go away) */
					if (rump___sysimpl_getpeername(fd, &peer,
					    &len) == 0)
						got |= FD_CONNECT;
					else if (!(got & FD_ERROR)) {
						LONG e = peek(fd);

						got |= FD_ERROR;
						f->pend_error = e < 0 ? -e :
						    ECONNREFUSED;
					}
				} else
					got |= FD_WRITE;
			}
			if (p->revents & NB_POLLPRI)
				got |= FD_OOB;

			/* events: each once until armed again */
			if (f->closed)
				got &= ~FD_CLOSE;
			/* (once reported: a mask without FD_CLOSE now, set
			   with it later, ev_setmask(), still gets it) */
			if (got & want & FD_CLOSE)
				f->closed = 1;
			if (post(sb, fd, got & want) && sb->sigeventmask)
				sigs |= sb->sigeventmask;
			/*
			 * poll() reports states: a state that was seen is
			 * not waited for again until a call changes it
			 * (ev_note()), whatever was reported for it; a
			 * peek that found nothing (EWOULDBLOCK) saw no
			 * state at all
			 */
			if ((p->revents & (NB_POLLIN | NB_POLLHUP | NB_POLLERR))
			    && !(havepeek && pk == -EWOULDBLOCK))
				f->armed &= ~(FD_READ | FD_ACCEPT | FD_CLOSE |
				    FD_ERROR);
			if (p->revents & (NB_POLLOUT | NB_POLLHUP | NB_POLLERR))
				f->armed &= ~(FD_WRITE | FD_CONNECT);
			if (p->revents & NB_POLLPRI)
				f->armed &= ~FD_OOB;

			/* SIGIO: the socket became readable / writable
			   (FIOASYNC); SIGURG: out-of-band data arrived */
			if (f->async && (f->sigio_armed & SIGIO_READ) &&
			    (p->revents & (NB_POLLIN | NB_POLLHUP | NB_POLLERR))) {
				f->sigio_armed &= ~SIGIO_READ;
				sigs |= sb->sigiomask;
			}
			if (f->async && (f->sigio_armed & SIGIO_WRITE) &&
			    (p->revents & (NB_POLLOUT | NB_POLLHUP | NB_POLLERR))) {
				f->sigio_armed &= ~SIGIO_WRITE;
				sigs |= sb->sigiomask;
			}
			if ((f->sigio_armed & SIGIO_URG) &&
			    (p->revents & NB_POLLPRI)) {
				f->sigio_armed &= ~SIGIO_URG;
				sigs |= sb->sigurgmask;
			}
			ev_unlock(sb);
		}
		/* the doc (OpenLibrary): signals are delivered to the opener */
		if (sigs)
			Signal(sb->owner, sigs);
	}
	if (ev->routefd >= 0)
		rump___sysimpl_close(ev->routefd);
	rump___sysimpl_close(ev->ctlfd);
	rump_pub_lwproc_releaselwp();
	return NULL;
}

/* wake the event thread to look at the state again (server side) */
static void
poke(struct SocketBase *sb)
{
	struct sbevents *ev = sb->ev;
	struct sockaddr_in sin;
	char one = 1;

	if (ev == NULL || ev->state != EV_RUNNING)
		return;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = ev->ctlport;
	sin.sin_addr.s_addr = INADDR_LOOPBACK;
	/* (any socket of the process can send it: the main server's) */
	rump___sysimpl_sendto(sb->main.wakefd, &one, 1, 0, &sin, sizeof(sin));
}

/* start the event thread if it is not running (server side) */
static int
ev_start(struct SocketBase *sb)
{
	struct sbevents *ev;

	if (sb->ev)
		return sb->ev->state == EV_RUNNING ? 0 : -1;
	if ((ev = AllocVec(sizeof(*ev), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return -1;
	ev->routefd = -1;
	sb->ev = ev;
	if (rumpuser_thread_create(ev_main, sb, "bsdsocket events", 1, 0, -1,
	    &ev->thread) != 0) {
		sb->ev = NULL;
		FreeVec(ev);
		return -1;
	}
	while (ev->state == 0)
		amiga_host_sleep_ms(2);
	if (ev->state != EV_RUNNING) {
		amiga_host_thread_join(ev->thread);
		sb->ev = NULL;
		FreeVec(ev);
		return -1;
	}
	return 0;
}

/* the SBTC_*MASK signals changed: start the thread if it has something
   to watch now, and let it look (server side, via ev_sync) */
LONG
ev_sync(struct SocketBase *sb, void *unused)
{

	if ((sb->sigurgmask || sb->sigaddrmask) && ev_start(sb) != 0)
		return -1;
	if (sb->sigurgmask) {
		int i;

		ev_lock(sb);
		for (i = 0; i < SB_MAXFD; i++)
			if (sb->fds[i].inuse)
				sb->fds[i].sigio_armed |= SIGIO_URG;
		ev_unlock(sb);
	}
	poke(sb);
	return 0;
}

/* SO_EVENTMASK (server side) */
int
ev_setmask(struct SocketBase *sb, LONG fd, ULONG mask)
{
	struct sbfd *f = &sb->fds[fd];

	if (mask && ev_start(sb) != 0)
		return -1;
	ev_lock(sb);
	f->eventmask = mask;
	/* everything that can be reported at once is armed; FD_WRITE only
	   after a send had to stop, FD_CONNECT while connecting */
	f->armed = FD_READ | FD_ACCEPT | FD_OOB | FD_ERROR |
	    (f->closed ? 0 : FD_CLOSE) | (f->connecting ? FD_CONNECT : 0);
	if (!mask)
		unqueue(sb, fd);
	f->pending &= mask;
	ev_unlock(sb);
	poke(sb);
	return 0;
}

/* FIOASYNC (server side) */
int
ev_setasync(struct SocketBase *sb, LONG fd, int on)
{
	struct sbfd *f = &sb->fds[fd];

	if (on && ev_start(sb) != 0)
		return -1;
	ev_lock(sb);
	f->async = on != 0;
	f->sigio_armed |= SIGIO_READ | (f->connecting ? SIGIO_WRITE : 0);
	ev_unlock(sb);
	poke(sb);
	return 0;
}

/* a call consumed or caused a condition: report it again (server side) */
void
ev_note(struct SocketBase *sb, LONG fd, ULONG fdbits, ULONG siobits)
{
	struct sbfd *f = &sb->fds[fd];
	ULONG oa, os;

	if (sb->ev == NULL)
		return;
	ev_lock(sb);
	oa = f->armed;
	os = f->sigio_armed;
	f->armed |= fdbits;
	f->sigio_armed |= siobits;
	ev_unlock(sb);
	if (watched(sb, f) && ((f->armed & ~oa & f->eventmask) ||
	    (f->sigio_armed & ~os)))
		poke(sb);
}

/* the fd is closed or reused (server side) */
void
ev_fd_closed(struct SocketBase *sb, LONG fd)
{
	struct sbfd *f = &sb->fds[fd];

	ev_lock(sb);
	unqueue(sb, fd);
	f->eventmask = f->armed = f->pending = f->sigio_armed = 0;
	f->async = f->connecting = f->closed = f->listening = 0;
	ev_unlock(sb);
}

/* the base goes away (server side) */
void
ev_stop(struct SocketBase *sb)
{
	struct sbevents *ev = sb->ev;

	if (ev == NULL)
		return;
	ev->quit = 1;
	poke(sb);
	amiga_host_thread_join(ev->thread);
	sb->ev = NULL;
	FreeVec(ev);
}

/*
 * GetSocketEvents(): "returns the next asynchronous event for sockets,
 * and removes it from the internal queue"; -1 if none is pending.
 * Runs on the caller's side (no kernel call).
 */
LONG
sb_GetSocketEvents(struct SocketBase *sb, ULONG *event_ptr)
{
	LONG fd = -1;
	ULONG events = 0;
	LONG err = 0;

	if (event_ptr == NULL) {
		sb_set_errno(sb, EFAULT);
		return -1;
	}
	ev_lock(sb);
	while (sb->evcount > 0 && fd < 0) {
		LONG q = sb->evqueue[sb->evhead];
		struct sbfd *f = &sb->fds[q];

		sb->evhead = (sb->evhead + 1) % SB_MAXFD;
		sb->evcount--;
		f->queued = 0;
		if (!f->inuse || !f->pending)
			continue;
		fd = q;
		events = f->pending;
		f->pending = 0;
		err = f->pend_error;
	}
	ev_unlock(sb);
	if (fd < 0)
		return -1;
	*event_ptr = events;
	/* "When this event is found, the 'errno' variable will be set to
	   the error code associated with the socket" */
	if (events & FD_ERROR)
		sb_set_errno(sb, err);
	return fd;
}
