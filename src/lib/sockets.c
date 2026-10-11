/*
 * bsdsocket.library: the socket calls.
 *
 * Each sb_<call>() runs in the caller's task: it packs its arguments and
 * has srv_<call>() executed by the base's server thread (sb_rpc()).  The
 * server functions talk to the NetBSD kernel and implement blocking
 * behaviour on top of the kernel's non-blocking sockets (see sblib.h).
 *
 * "the doc" is downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "sblib.h"

#define	RPC(fn)		sb_rpc(sb, (sbfn_t)(fn), &a, 0, NULL)
#define	RPC_INTR(fn)	sb_rpc(sb, (sbfn_t)(fn), &a, RPC_INTERRUPTIBLE, NULL)

/* blocking unless the user made the socket (or this call) non-blocking */
static int
blocking(struct SocketBase *sb, LONG fd, LONG flags)
{

	return !sb->fds[fd].nonblock && !(flags & MSG_DONTWAIT);
}

static LONG
rumpfail(struct SocketBase *sb)
{

	return sb_fail(sb, sb_rumperr());
}

static const struct sbtime *
rcvto(struct SocketBase *sb, LONG fd)
{

	return sb->fds[fd].has_rcvto ? &sb->fds[fd].rcvto : NULL;
}

static const struct sbtime *
sndto(struct SocketBase *sb, LONG fd)
{

	return sb->fds[fd].has_sndto ? &sb->fds[fd].sndto : NULL;
}

/* register a new kernel socket in the base (kernel side non-blocking) */
static LONG
adopt_fd(struct SocketBase *sb, LONG fd, int domain, LONG type,
    LONG protocol, int nonblock)
{
	LONG on = 1;

	if (fd >= sb->dtablesize || fd >= SB_MAXFD) {
		rump___sysimpl_close(fd);
		return sb_fail(sb, EMFILE);
	}
	rump___sysimpl_ioctl(fd, FIONBIO, &on);
	ev_fd_closed(sb, fd);		/* (a fresh slot: no old events) */
	/* (the event thread reads the table) */
	ObtainSemaphore(&sb->evlock);
	memset(&sb->fds[fd], 0, sizeof(sb->fds[fd]));
	sb->fds[fd].type = (UBYTE)type;
	sb->fds[fd].domain = (UBYTE)domain;
	sb->fds[fd].route = domain == AF_ROUTE;
	sb->fds[fd].protocol = protocol;
	sb->fds[fd].nonblock = (UBYTE)nonblock;
	sb->fds[fd].inuse = 1;
	ReleaseSemaphore(&sb->evlock);
	/* SIGURG for every socket while a mask is set (ev_sync() arms the
	   ones open then) */
	if (sb->sigurgmask)
		ev_note(sb, fd, 0, SIGIO_URG);
	return fd;
}

#define	CHECKFD(fd)	do { if (!sb_fdok(sb, fd)) return sb_fail(sb, EBADF); } while (0)

/* ------------------------------------------------------------------------ */

struct socket_args { LONG domain, type, protocol; };

static LONG
srv_socket(struct SocketBase *sb, struct socket_args *a)
{
	LONG fd, dom = a->domain;

	/* Roadshow's AF_ROUTE is 17 (netinclude/sys/socket.h), NetBSD's 34
	   (17 is its AF_OROUTE, for which there is no protocol); the
	   messages are translated by route.c */
	if (dom == AF_ROUTE)
		dom = NB_PF_ROUTE;
	if ((fd = rump___sysimpl_socket30(dom, a->type, a->protocol)) < 0)
		return rumpfail(sb);
	return adopt_fd(sb, fd, a->domain, a->type, a->protocol, 0);
}

LONG
sb_socket(struct SocketBase *sb, LONG domain, LONG type, LONG protocol)
{
	struct socket_args a = { domain, type, protocol };

	return RPC(srv_socket);
}

struct addr_args { LONG fd; struct sockaddr *name; LONG len; };

static LONG
srv_bind(struct SocketBase *sb, struct addr_args *a)
{

	CHECKFD(a->fd);
	if (rump___sysimpl_bind(a->fd, a->name, a->len) < 0)
		return rumpfail(sb);
	return 0;
}

LONG
sb_bind(struct SocketBase *sb, LONG sock, struct sockaddr *name, LONG namelen)
{
	struct addr_args a = { sock, name, namelen };
	LONG e;

	/* MHT_Bind hooks run "before dropping into the kernel 'bind()'
	   call", on the caller's context (the doc,
	   AddNetMonitorHookTagList) */
	if ((e = mon_bind(sb, sock, name, namelen)) > 0) {
		sb_set_errno(sb, e);
		return -1;
	}
	return RPC(srv_bind);
}

struct listen_args { LONG fd, backlog; };

static LONG
srv_listen(struct SocketBase *sb, struct listen_args *a)
{

	CHECKFD(a->fd);
	if (rump___sysimpl_listen(a->fd, a->backlog) < 0)
		return rumpfail(sb);
	sb->fds[a->fd].listening = 1;
	ev_note(sb, a->fd, FD_ACCEPT, SIGIO_READ);
	return 0;
}

LONG
sb_listen(struct SocketBase *sb, LONG sock, LONG backlog)
{
	struct listen_args a = { sock, backlog };

	return RPC(srv_listen);
}

struct accept_args { LONG fd; struct sockaddr *addr; socklen_t *len; };

static LONG
srv_accept(struct SocketBase *sb, struct accept_args *a)
{
	LONG fd, e;
	struct sbfd *l;

	CHECKFD(a->fd);
	l = &sb->fds[a->fd];
	for (;;) {
		fd = rump___sysimpl_accept(a->fd, a->addr, a->len);
		/* (the error first: every system call sets it, also the one
		   ev_note() may make, netbsd-src/sys/rump/librump/rumpkern/
		   rump_syscalls.c:81 rsys_seterrno()) */
		e = fd < 0 ? sb_rumperr() : 0;
		/* FD_ACCEPT again while connections are pending (the doc,
		   GetSocketEvents) */
		ev_note(sb, a->fd, FD_ACCEPT, SIGIO_READ);
		if (fd >= 0) {
			/* "with the same properties" (the doc, accept): the
			   timeouts too, which the kernel copies from the
			   listener (uipc_socket2.c:351-352) but this library
			   applies itself */
			if ((fd = adopt_fd(sb, fd, l->domain, l->type,
			    l->protocol, l->nonblock)) >= 0) {
				sb->fds[fd].has_rcvto = l->has_rcvto;
				sb->fds[fd].has_sndto = l->has_sndto;
				sb->fds[fd].rcvto = l->rcvto;
				sb->fds[fd].sndto = l->sndto;
			}
			return fd;
		}
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, 0))
			return sb_fail(sb, e);
		if ((e = sb_wait_fd(sb, a->fd, WAIT_READ, rcvto(sb, a->fd))) != 0)
			return sb_fail(sb, e);
	}
}

LONG
sb_accept(struct SocketBase *sb, LONG sock, struct sockaddr *addr,
    socklen_t *addrlen)
{
	struct accept_args a = { sock, addr, addrlen };

	return RPC_INTR(srv_accept);
}

static LONG
srv_connect(struct SocketBase *sb, struct addr_args *a)
{
	LONG e, err;
	socklen_t len = sizeof(err);

	CHECKFD(a->fd);
	if (rump___sysimpl_connect(a->fd, a->name, a->len) == 0)
		return 0;
	e = sb_rumperr();
	if (e != EINPROGRESS || !blocking(sb, a->fd, 0)) {
		if (e == EINPROGRESS) {
			/* FD_CONNECT when it completes */
			sb->fds[a->fd].connecting = 1;
			ev_note(sb, a->fd, FD_CONNECT, SIGIO_WRITE);
		}
		return sb_fail(sb, e);
	}
	if ((e = sb_wait_fd(sb, a->fd, WAIT_WRITE, NULL)) != 0)
		return sb_fail(sb, e);
	if (rump___sysimpl_getsockopt(a->fd, SOL_SOCKET, SO_ERROR, &err,
	    &len) < 0)
		return rumpfail(sb);
	return err ? sb_fail(sb, err) : 0;
}

LONG
sb_connect(struct SocketBase *sb, LONG sock, struct sockaddr *name,
    LONG namelen)
{
	struct addr_args a = { sock, name, namelen };
	LONG e;

	/* MHT_Connect hooks run "before dropping into the kernel
	   'connect()' call" (the doc, AddNetMonitorHookTagList) */
	if ((e = mon_connect(sb, sock, name, namelen)) > 0) {
		sb_set_errno(sb, e);
		return -1;
	}
	return RPC_INTR(srv_connect);
}

/* ------------------------------------------------------------------------
 * data transfer
 */

struct sendto_args {
	LONG fd; APTR buf; LONG len; LONG flags;
	struct sockaddr *to; LONG tolen;
};

/* a send that would block on a non-blocking socket: FD_WRITE / SIGIO
   when it can take data again (the doc, GetSocketEvents FD_WRITE) */
static void
send_blocked(struct SocketBase *sb, LONG fd)
{

	ev_note(sb, fd, FD_WRITE, SIGIO_WRITE);
}

static LONG
srv_sendto(struct SocketBase *sb, struct sendto_args *a)
{
	LONG n, e, done = 0;
	int stream;

	CHECKFD(a->fd);
	if (sb->fds[a->fd].route)
		return route_send(sb, a->fd, a->buf, a->len, a->flags);
	stream = sb->fds[a->fd].type == SOCK_STREAM;
	for (;;) {
		n = rump___sysimpl_sendto(a->fd, (UBYTE *)a->buf + done,
		    a->len - done, (a->flags & ~MSG_DONTWAIT) | NB_MSG_NOSIGNAL,
		    a->to, a->to ? a->tolen : 0);
		if (n >= 0) {
			done += n;
			/* a blocking stream send delivers everything */
			if (!stream || done >= a->len ||
			    !blocking(sb, a->fd, a->flags))
				return done;
			continue;
		}
		e = sb_rumperr();
		if (e == EWOULDBLOCK && !blocking(sb, a->fd, a->flags))
			send_blocked(sb, a->fd);
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, a->flags))
			return done ? done : sb_fail(sb, e);
		if ((e = sb_wait_fd(sb, a->fd, WAIT_WRITE, sndto(sb, a->fd))) != 0)
			return done ? done : sb_fail(sb, e);
	}
}

LONG
sb_sendto(struct SocketBase *sb, LONG sock, APTR buf, LONG len, LONG flags,
    struct sockaddr *to, LONG tolen)
{
	struct sendto_args a = { sock, buf, len, flags, to, tolen };
	LONG e;

	if ((e = mon_send(sb, sock, buf, len, flags, to, tolen, NULL)) > 0) {
		sb_set_errno(sb, e);
		return -1;
	}
	return RPC_INTR(srv_sendto);
}

LONG
sb_send(struct SocketBase *sb, LONG sock, APTR buf, LONG len, LONG flags)
{
	struct sendto_args a = { sock, buf, len, flags, NULL, 0 };
	LONG e;

	if ((e = mon_send(sb, sock, buf, len, flags, NULL, 0, NULL)) > 0) {
		sb_set_errno(sb, e);
		return -1;
	}
	return RPC_INTR(srv_sendto);
}

struct recvfrom_args {
	LONG fd; APTR buf; LONG len; LONG flags;
	struct sockaddr *from; socklen_t *fromlen;
};

/* any receive: data (and out-of-band data) may be reported again */
static void
received(struct SocketBase *sb, LONG fd)
{

	ev_note(sb, fd, FD_READ | FD_OOB | FD_ERROR | FD_CLOSE,
	    SIGIO_READ | SIGIO_URG);
}

static LONG
srv_recvfrom(struct SocketBase *sb, struct recvfrom_args *a)
{
	LONG n, e, done = 0;
	int waitall;

	CHECKFD(a->fd);
	if (sb->fds[a->fd].route) {
		n = route_recv(sb, a->fd, a->buf, a->len, a->flags);
		received(sb, a->fd);
		if (n >= 0 && a->fromlen)
			*a->fromlen = 0;	/* raw routing socket: no peer */
		return n;
	}
	/* MSG_WAITALL: emulated here (the kernel would sleep in the
	   kernel), for byte streams only; a datagram is one record, and
	   peeking again would return the same bytes */
	waitall = (a->flags & MSG_WAITALL) && !(a->flags & MSG_PEEK) &&
	    sb->fds[a->fd].type == SOCK_STREAM;
	for (;;) {
		n = rump___sysimpl_recvfrom(a->fd, (UBYTE *)a->buf + done,
		    a->len - done, a->flags & ~(MSG_DONTWAIT | MSG_WAITALL),
		    a->from, a->fromlen);
		/* (the error before received(), which may make a system
		   call: srv_accept()) */
		e = n < 0 ? sb_rumperr() : 0;
		received(sb, a->fd);
		if (n > 0) {
			done += n;
			if (!waitall || done >= a->len)
				return done;
			continue;
		}
		if (n == 0)
			return done;		/* EOF */
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, a->flags))
			return done ? done : sb_fail(sb, e);
		if ((e = sb_wait_fd(sb, a->fd, (a->flags & MSG_OOB) ?
		    WAIT_PRI : WAIT_READ, rcvto(sb, a->fd))) != 0)
			return done ? done : sb_fail(sb, e);
	}
}

LONG
sb_recvfrom(struct SocketBase *sb, LONG sock, APTR buf, LONG len,
    LONG flags, struct sockaddr *addr, socklen_t *addrlen)
{
	struct recvfrom_args a = { sock, buf, len, flags, addr, addrlen };

	return RPC_INTR(srv_recvfrom);
}

LONG
sb_recv(struct SocketBase *sb, LONG sock, APTR buf, LONG len, LONG flags)
{
	struct recvfrom_args a = { sock, buf, len, flags, NULL, NULL };

	return RPC_INTR(srv_recvfrom);
}

struct msg_args { LONG fd; struct msghdr *msg; LONG flags; };

/* the iovecs of a message, as a copy that may be advanced */
static struct iovec *
iov_copy(const struct msghdr *m, struct iovec *local, int nlocal,
    ULONG *total)
{
	struct iovec *iov = local;
	LONG i;

	if (m->msg_iovlen > nlocal &&
	    (iov = AllocVec(m->msg_iovlen * sizeof(*iov), MEMF_PUBLIC)) == NULL)
		return NULL;
	*total = 0;
	for (i = 0; i < m->msg_iovlen; i++) {
		iov[i] = m->msg_iov[i];
		*total += iov[i].iov_len;
	}
	return iov;
}

/* drop n transferred bytes from the front of m's iovecs */
static void
iov_advance(struct msghdr *m, ULONG n)
{
	LONG k;

	for (k = 0; k < m->msg_iovlen && n >= m->msg_iov[k].iov_len; k++)
		n -= m->msg_iov[k].iov_len;
	m->msg_iov += k;
	m->msg_iovlen -= k;
	if (m->msg_iovlen > 0) {
		m->msg_iov[0].iov_base = (UBYTE *)m->msg_iov[0].iov_base + n;
		m->msg_iov[0].iov_len -= n;
	}
}

/* a routing socket message is one record: gathered into one buffer */
static LONG
route_sendmsg(struct SocketBase *sb, struct msg_args *a)
{
	ULONG total = 0;
	LONG i, rv;
	UBYTE *buf, *p;

	for (i = 0; i < a->msg->msg_iovlen; i++)
		total += a->msg->msg_iov[i].iov_len;
	if ((buf = AllocVec(total ? total : 1, MEMF_PUBLIC)) == NULL)
		return sb_fail(sb, ENOBUFS);
	for (p = buf, i = 0; i < a->msg->msg_iovlen; i++) {
		CopyMem(a->msg->msg_iov[i].iov_base, p,
		    a->msg->msg_iov[i].iov_len);
		p += a->msg->msg_iov[i].iov_len;
	}
	rv = route_send(sb, a->fd, buf, total, a->flags);
	FreeVec(buf);
	return rv;
}

static LONG
route_recvmsg(struct SocketBase *sb, struct msg_args *a)
{
	ULONG total = 0, left, k;
	LONG i, n;
	UBYTE *buf, *p;

	for (i = 0; i < a->msg->msg_iovlen; i++)
		total += a->msg->msg_iov[i].iov_len;
	if ((buf = AllocVec(total ? total : 1, MEMF_PUBLIC)) == NULL)
		return sb_fail(sb, ENOBUFS);
	n = route_recv(sb, a->fd, buf, total, a->flags);
	if (n >= 0) {
		for (p = buf, left = n, i = 0; i < a->msg->msg_iovlen && left;
		    i++) {
			k = a->msg->msg_iov[i].iov_len < left ?
			    a->msg->msg_iov[i].iov_len : left;
			CopyMem(p, a->msg->msg_iov[i].iov_base, k);
			p += k;
			left -= k;
		}
		a->msg->msg_namelen = 0;
		a->msg->msg_controllen = 0;
		a->msg->msg_flags = 0;
	}
	FreeVec(buf);
	return n;
}

#define	MSG_IOV_LOCAL	16

/*
 * The kernel socket is non-blocking, so on a byte stream it may take only
 * part of the data; a blocking sendmsg() sends it all (as srv_sendto()),
 * going on with a copy of the rest of the iovecs.
 */
static LONG
srv_sendmsg(struct SocketBase *sb, struct msg_args *a)
{
	struct iovec local[MSG_IOV_LOCAL], *iov = NULL;
	struct msghdr m;
	LONG n, e, done = 0;
	ULONG total = 0;
	int stream;

	CHECKFD(a->fd);
	if (a->msg == NULL)
		return sb_fail(sb, EFAULT);
	if (sb->fds[a->fd].route)
		return route_sendmsg(sb, a);
	m = *a->msg;
	stream = sb->fds[a->fd].type == SOCK_STREAM &&
	    blocking(sb, a->fd, a->flags) && m.msg_iovlen > 0;
	if (stream) {
		if ((iov = iov_copy(&m, local, MSG_IOV_LOCAL, &total)) == NULL)
			return sb_fail(sb, ENOBUFS);
		m.msg_iov = iov;
	}
	for (;;) {
		n = rump___sysimpl_sendmsg(a->fd, &m,
		    (a->flags & ~MSG_DONTWAIT) | NB_MSG_NOSIGNAL);
		if (n >= 0) {
			done += n;
			if (!stream || (ULONG)done >= total)
				break;
			/* skip what was sent; ancillary data went with it */
			iov_advance(&m, n);
			m.msg_control = NULL;
			m.msg_controllen = 0;
			continue;
		}
		e = sb_rumperr();
		if (e == EWOULDBLOCK && !blocking(sb, a->fd, a->flags))
			send_blocked(sb, a->fd);
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, a->flags)) {
			if (!done)
				done = sb_fail(sb, e);
			break;
		}
		if ((e = sb_wait_fd(sb, a->fd, WAIT_WRITE,
		    sndto(sb, a->fd))) != 0) {
			if (!done)
				done = sb_fail(sb, e);
			break;
		}
	}
	if (iov && iov != local)
		FreeVec(iov);
	return done;
}

/*
 * MSG_WAITALL on a byte stream: the doc (recv) "requests that the
 * operation block until the full request is satisfied"; done here with
 * the non-blocking kernel socket, filling the rest of the iovecs.  The
 * address and control data come with the first part.
 */
static LONG
srv_recvmsg(struct SocketBase *sb, struct msg_args *a)
{
	struct iovec local[MSG_IOV_LOCAL], *iov = NULL;
	struct msghdr m;
	LONG n, e, done = 0;
	ULONG total = 0;
	int waitall;

	CHECKFD(a->fd);
	if (a->msg == NULL)
		return sb_fail(sb, EFAULT);
	if (sb->fds[a->fd].route) {
		n = route_recvmsg(sb, a);
		received(sb, a->fd);
		return n;
	}
	waitall = (a->flags & MSG_WAITALL) && !(a->flags & MSG_PEEK) &&
	    sb->fds[a->fd].type == SOCK_STREAM && a->msg->msg_iovlen > 0;
	m = *a->msg;
	if (waitall) {
		if ((iov = iov_copy(&m, local, MSG_IOV_LOCAL, &total)) == NULL)
			return sb_fail(sb, ENOBUFS);
		m.msg_iov = iov;
	}
	for (;;) {
		n = rump___sysimpl_recvmsg(a->fd, &m,
		    a->flags & ~(MSG_DONTWAIT | MSG_WAITALL));
		/* (the error before received(): srv_accept()) */
		e = n < 0 ? sb_rumperr() : 0;
		received(sb, a->fd);
		if (n >= 0) {
			if (done == 0) {
				/* the first part's address, control data and
				   flags are the caller's results */
				a->msg->msg_namelen = m.msg_namelen;
				a->msg->msg_controllen = m.msg_controllen;
				a->msg->msg_flags = m.msg_flags;
			} else
				a->msg->msg_flags |= m.msg_flags;
			done += n;
			if (!waitall || n == 0 || (ULONG)done >= total)
				break;
			iov_advance(&m, n);
			m.msg_name = NULL;
			m.msg_namelen = 0;
			m.msg_control = NULL;
			m.msg_controllen = 0;
			continue;
		}
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, a->flags)) {
			if (!done)
				done = sb_fail(sb, e);
			break;
		}
		if ((e = sb_wait_fd(sb, a->fd, (a->flags & MSG_OOB) ?
		    WAIT_PRI : WAIT_READ, rcvto(sb, a->fd))) != 0) {
			if (!done)
				done = sb_fail(sb, e);
			break;
		}
	}
	if (iov && iov != local)
		FreeVec(iov);
	return done;
}

LONG
sb_sendmsg(struct SocketBase *sb, LONG sock, struct msghdr *msg, LONG flags)
{
	struct msg_args a = { sock, msg, flags };
	LONG e;

	if ((e = mon_send(sb, sock, NULL, 0, flags, NULL, 0, msg)) > 0) {
		sb_set_errno(sb, e);
		return -1;
	}
	return RPC_INTR(srv_sendmsg);
}

LONG
sb_recvmsg(struct SocketBase *sb, LONG sock, struct msghdr *msg, LONG flags)
{
	struct msg_args a = { sock, msg, flags };

	return RPC_INTR(srv_recvmsg);
}

/* ------------------------------------------------------------------------
 * socket state
 */

static LONG
srv_shutdown(struct SocketBase *sb, struct listen_args *a)
{

	CHECKFD(a->fd);
	if (rump___sysimpl_shutdown(a->fd, a->backlog) < 0)
		return rumpfail(sb);
	return 0;
}

LONG
sb_shutdown(struct SocketBase *sb, LONG sock, LONG how)
{
	struct listen_args a = { sock, how };

	return RPC(srv_shutdown);
}

struct sockopt_args {
	LONG fd, level, name; APTR val; LONG len; socklen_t *lenp;
};

static LONG
srv_setsockopt(struct SocketBase *sb, struct sockopt_args *a)
{

	CHECKFD(a->fd);
	if (a->level == SOL_SOCKET &&
	    (a->name == SO_RCVTIMEO || a->name == SO_SNDTIMEO)) {
		const struct __timeval *tv = a->val;
		struct nb_timeval ntv;
		struct sbfd *f = &sb->fds[a->fd];

		if (tv == NULL || a->len < (LONG)sizeof(struct __timeval))
			return sb_fail(sb, EINVAL);
		/*
		 * The kernel checks the value (sys/kern/uipc_socket.c
		 * sosetopt(): EDOM for tv_usec >= 1000000 or too many
		 * seconds) and keeps it for getsockopt(); the waiting is
		 * done here, the kernel socket being non-blocking.
		 */
		ntv.tv_sec.hi = 0;
		ntv.tv_sec.lo = tv->tv_secs;
		ntv.tv_usec = (LONG)tv->tv_micro;
		if (tv->tv_micro >= 1000000)
			return sb_fail(sb, EDOM);
		if (rump___sysimpl_setsockopt(a->fd, SOL_SOCKET,
		    a->name == SO_RCVTIMEO ? NB_SO_RCVTIMEO : NB_SO_SNDTIMEO,
		    &ntv, sizeof(ntv)) < 0)
			return rumpfail(sb);
		if (a->name == SO_RCVTIMEO) {
			sbtime_from_tv(&f->rcvto, tv->tv_secs, tv->tv_micro);
			f->has_rcvto = !sbtime_iszero(&f->rcvto);
		} else {
			sbtime_from_tv(&f->sndto, tv->tv_secs, tv->tv_micro);
			f->has_sndto = !sbtime_iszero(&f->sndto);
		}
		return 0;
	}
	if (a->level == SOL_SOCKET && a->name == SO_EVENTMASK) {
		/* netinclude/sys/socket.h: private option of this stack's
		   family; the value is a mask of FD_* events */
		if (a->val == NULL || a->len < (LONG)sizeof(ULONG))
			return sb_fail(sb, EINVAL);
		if (ev_setmask(sb, a->fd, *(ULONG *)a->val & FD_ALL) != 0)
			return sb_fail(sb, ENOMEM);
		return 0;
	}
	if (rump___sysimpl_setsockopt(a->fd, a->level, a->name, a->val,
	    a->len) < 0)
		return rumpfail(sb);
	return 0;
}

static LONG
srv_getsockopt(struct SocketBase *sb, struct sockopt_args *a)
{

	CHECKFD(a->fd);
	if (a->level == SOL_SOCKET &&
	    (a->name == SO_RCVTIMEO || a->name == SO_SNDTIMEO)) {
		struct __timeval *tv = a->val;
		struct sbfd *f = &sb->fds[a->fd];
		int rcv = a->name == SO_RCVTIMEO;
		const struct sbtime *t = rcv ? &f->rcvto : &f->sndto;

		if (tv == NULL || a->lenp == NULL ||
		    *a->lenp < sizeof(struct __timeval))
			return sb_fail(sb, EINVAL);
		/* the timeout the waiting here uses (setsockopt above), not
		   the kernel's copy, which it rounds to clock ticks
		   (uipc_socket.c:2012-2019) */
		if (rcv ? f->has_rcvto : f->has_sndto) {
			tv->tv_secs = t->s;
			tv->tv_micro = t->ms * 1000;
		} else
			tv->tv_secs = tv->tv_micro = 0;
		*a->lenp = sizeof(*tv);
		return 0;
	}
	if (a->level == SOL_SOCKET && a->name == SO_EVENTMASK) {
		if (a->val == NULL || a->lenp == NULL ||
		    *a->lenp < sizeof(ULONG))
			return sb_fail(sb, EINVAL);
		*(ULONG *)a->val = sb->fds[a->fd].eventmask;
		*a->lenp = sizeof(ULONG);
		return 0;
	}
	if (rump___sysimpl_getsockopt(a->fd, a->level, a->name, a->val,
	    a->lenp) < 0)
		return rumpfail(sb);
	return 0;
}

LONG
sb_setsockopt(struct SocketBase *sb, LONG sock, LONG level, LONG optname,
    APTR optval, LONG optlen)
{
	struct sockopt_args a = { sock, level, optname, optval, optlen, NULL };

	return RPC(srv_setsockopt);
}

LONG
sb_getsockopt(struct SocketBase *sb, LONG sock, LONG level, LONG optname,
    APTR optval, socklen_t *optlen)
{
	struct sockopt_args a = { sock, level, optname, optval, 0, optlen };

	return RPC(srv_getsockopt);
}

static LONG
srv_getsockname(struct SocketBase *sb, struct accept_args *a)
{

	CHECKFD(a->fd);
	if (rump___sysimpl_getsockname(a->fd, a->addr, a->len) < 0)
		return rumpfail(sb);
	if (sb->fds[a->fd].route && a->addr && a->len && *a->len >= 2)
		route_sockaddr_to_amiga(a->addr);
	return 0;
}

static LONG
srv_getpeername(struct SocketBase *sb, struct accept_args *a)
{

	CHECKFD(a->fd);
	if (rump___sysimpl_getpeername(a->fd, a->addr, a->len) < 0)
		return rumpfail(sb);
	if (sb->fds[a->fd].route && a->addr && a->len && *a->len >= 2)
		route_sockaddr_to_amiga(a->addr);
	return 0;
}

LONG
sb_getsockname(struct SocketBase *sb, LONG sock, struct sockaddr *name,
    socklen_t *namelen)
{
	struct accept_args a = { sock, name, namelen };

	return RPC(srv_getsockname);
}

LONG
sb_getpeername(struct SocketBase *sb, LONG sock, struct sockaddr *name,
    socklen_t *namelen)
{
	struct accept_args a = { sock, name, namelen };

	return RPC(srv_getpeername);
}

struct ioctl_args { LONG fd; ULONG req; APTR argp; };

static LONG
srv_ioctl(struct SocketBase *sb, struct ioctl_args *a)
{
	int handled = 0;
	LONG rv;

	CHECKFD(a->fd);
	switch (a->req) {
	case FIONBIO:
		if (a->argp == NULL)
			return sb_fail(sb, EFAULT);
		sb->fds[a->fd].nonblock = *(LONG *)a->argp != 0;
		return 0;
	case FIOASYNC:
		/* the doc, IoctlSocket: "A value of 1 enables asynchronous
		   I/O on the socket"; SIGIO is the SBTC_SIGIOMASK signal */
		if (a->argp == NULL)
			return sb_fail(sb, EFAULT);
		if (ev_setasync(sb, a->fd, *(LONG *)a->argp != 0) != 0)
			return sb_fail(sb, ENOMEM);
		return 0;
	}
	rv = sb_ifioctl(sb, a->fd, a->req, a->argp, &handled);
	if (handled)
		return rv;
	if (rump___sysimpl_ioctl(a->fd, a->req, a->argp) < 0)
		return rumpfail(sb);
	return 0;
}

LONG
sb_IoctlSocket(struct SocketBase *sb, LONG sock, ULONG req, APTR argp)
{
	struct ioctl_args a = { sock, req, argp };

	return RPC(srv_ioctl);
}

/*
 * SO_LINGER: "the system will block the process on the close attempt
 * until it is able to transmit the data or until ... a timeout period,
 * termed the linger interval" (downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/
 * doc/bsdsocket.doc:5726-5732).  The kernel does not wait: its socket is
 * non-blocking (netbsd-src/sys/kern/uipc_socket.c:765-768).  So it is done
 * here, before the close, for a socket the program did not make
 * non-blocking: until the send queue is empty (FIONWRITE), the linger
 * interval is over, or the call is aborted.  The queue is looked at every
 * 50 ms.
 */
static void
linger_wait(struct SocketBase *sb, LONG fd)
{
	struct linger l;
	socklen_t len = sizeof(l);
	struct sbtime left, step;
	ULONG start, ms;
	int queued;

	if (sb->fds[fd].nonblock || sb->fds[fd].type != SOCK_STREAM ||
	    rump___sysimpl_getsockopt(fd, SOL_SOCKET, SO_LINGER, &l,
	    &len) < 0 || !l.l_onoff || l.l_linger <= 0)
		return;
	left.s = l.l_linger;
	left.ms = 0;
	while (!sbtime_iszero(&left)) {
		queued = 0;
		if (rump___sysimpl_ioctl(fd, NB_FIONWRITE, &queued) < 0 ||
		    queued <= 0)
			return;
		step.s = 0;
		step.ms = left.s > 0 || left.ms > 50 ? 50 : left.ms;
		start = amiga_host_ms();
		/* (no fd: only the timeout or an abort ends it) */
		if (sb_wait_fd(sb, -1, 0, &step) == EINTR)
			return;
		ms = amiga_host_ms() - start;
		sbtime_sub(&left, ms ? ms : 1);
	}
}

static LONG
srv_close(struct SocketBase *sb, struct listen_args *a)
{

	CHECKFD(a->fd);
	linger_wait(sb, a->fd);
	ev_fd_closed(sb, a->fd);
	ObtainSemaphore(&sb->evlock);
	sb->fds[a->fd].inuse = 0;
	ReleaseSemaphore(&sb->evlock);
	if (rump___sysimpl_close(a->fd) < 0)
		return rumpfail(sb);
	return 0;
}

LONG
sb_CloseSocket(struct SocketBase *sb, LONG sock)
{
	struct listen_args a = { sock, 0 };

	/* (interruptible: the SO_LINGER wait, linger_wait()) */
	return RPC_INTR(srv_close);
}

static LONG
srv_dup2(struct SocketBase *sb, struct listen_args *a)
{
	LONG fd;
	struct sbfd *o;

	CHECKFD(a->fd);
	if (a->backlog == -1)
		fd = rump___sysimpl_fcntl(a->fd, NB_F_DUPFD, 0);
	else {
		/* (EBADF for any other negative new_socket: downloads/sources/
		   NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc:3791-3793) */
		if (a->backlog < 0 || a->backlog >= sb->dtablesize)
			return sb_fail(sb, EBADF);
		/* onto itself: nothing to close or copy, as the kernel's dup2()
		   does it ("else if (from == to) error = 0;" and the result is
		   'to', netbsd-src/sys/kern/sys_descrip.c:141-146 dodup()) */
		if (a->backlog == a->fd)
			return a->fd;
		if (sb_fdok(sb, a->backlog))
			ev_fd_closed(sb, a->backlog);
		fd = rump___sysimpl_dup2(a->fd, a->backlog);
	}
	if (fd < 0)
		return rumpfail(sb);
	if (fd >= sb->dtablesize) {
		rump___sysimpl_close(fd);
		return sb_fail(sb, EMFILE);
	}
	/* the same socket: its properties, not its event state */
	o = &sb->fds[a->fd];
	if (fd != a->fd) {
		ObtainSemaphore(&sb->evlock);
		memset(&sb->fds[fd], 0, sizeof(sb->fds[fd]));
		sb->fds[fd].nonblock = o->nonblock;
		sb->fds[fd].type = o->type;
		sb->fds[fd].route = o->route;
		sb->fds[fd].domain = o->domain;
		sb->fds[fd].protocol = o->protocol;
		sb->fds[fd].listening = o->listening;
		sb->fds[fd].has_rcvto = o->has_rcvto;
		sb->fds[fd].has_sndto = o->has_sndto;
		sb->fds[fd].rcvto = o->rcvto;
		sb->fds[fd].sndto = o->sndto;
		sb->fds[fd].inuse = 1;
		ReleaseSemaphore(&sb->evlock);
		if (sb->sigurgmask)
			ev_note(sb, fd, 0, SIGIO_URG);
	}
	return fd;
}

LONG
sb_Dup2Socket(struct SocketBase *sb, LONG old_socket, LONG new_socket)
{
	struct listen_args a = { old_socket, new_socket };

	if (old_socket < 0) {
		sb_set_errno(sb, EBADF);
		return -1;
	}
	return RPC(srv_dup2);
}

/* ------------------------------------------------------------------------
 * WaitSelect()
 */

struct select_args {
	LONG nfds;
	ULONG *r, *w, *e;
	struct sbtime tv;
	int has_tv;
};

#define	FDISSET(set, fd)	((set) && ((set)[(fd) >> 5] & (1UL << ((fd) & 31))))
#define	FDSET(set, fd)		((set)[(fd) >> 5] |= (1UL << ((fd) & 31)))

static void
zero_sets(LONG nfds, ULONG *r, ULONG *w, ULONG *e)
{
	LONG words = (nfds + 31) >> 5, i;

	for (i = 0; i < words; i++) {
		if (r) r[i] = 0;
		if (w) w[i] = 0;
		if (e) e[i] = 0;
	}
}

static LONG
srv_select(struct SocketBase *sb, struct select_args *a)
{
	struct nb_pollfd pfd[SB_MAXFD + 1];
	LONG nfds = a->nfds, n, i, ready = 0, np = 0;
	struct sbtime left = a->tv;
	ULONG start;
	struct sbserver *srv = sb_srv(sb);

	if (srv == NULL)
		return sb_fail(sb, EINVAL);	/* (not on a server thread) */
	for (i = 0; i < nfds; i++) {
		WORD ev = 0;

		if (FDISSET(a->r, i)) ev |= NB_POLLIN;
		if (FDISSET(a->w, i)) ev |= NB_POLLOUT;
		if (FDISSET(a->e, i)) ev |= NB_POLLPRI;
		if (!ev)
			continue;
		if (!sb->fds[i].inuse)
			return sb_fail(sb, EBADF);
		pfd[np].fd = i;
		pfd[np].events = ev;
		pfd[np].revents = 0;
		np++;
	}
	pfd[np].fd = srv->wakefd;
	pfd[np].events = NB_POLLIN;
	pfd[np].revents = 0;

	for (;;) {
		/* aborted before the call started (its wake was drained) */
		if (srv->abortseq == srv->callseq)
			return sb_fail(sb, EINTR);
		start = amiga_host_ms();
		n = rump___sysimpl_poll(pfd, np + 1,
		    a->has_tv ? sbtime_chunk(&left) : -1);
		if (n < 0)
			return rumpfail(sb);
		if (a->has_tv)
			sbtime_sub(&left, amiga_host_ms() - start);
		if (pfd[np].revents) {
			sb_drain_wake(sb);
			if (srv->abortseq == srv->callseq)
				return sb_fail(sb, EINTR);
			/* stale wake from an earlier call: ignore it */
			pfd[np].revents = 0;
			n--;
		}
		if (n > 0 || (a->has_tv && sbtime_iszero(&left)))
			break;
	}
	/* success: only now are the sets replaced (the doc, WaitSelect
	   RESULT: with an error "the descriptor sets will be unmodified") */
	zero_sets(nfds, a->r, a->w, a->e);
	for (i = 0; i < np; i++) {
		WORD re = pfd[i].revents;
		LONG fd = pfd[i].fd;

		if (!re)
			continue;
		if ((re & (NB_POLLIN | NB_POLLHUP | NB_POLLERR)) &&
		    (pfd[i].events & NB_POLLIN))
			FDSET(a->r, fd), ready++;
		if ((re & (NB_POLLOUT | NB_POLLHUP | NB_POLLERR)) &&
		    (pfd[i].events & NB_POLLOUT))
			FDSET(a->w, fd), ready++;
		if ((re & NB_POLLPRI) && (pfd[i].events & NB_POLLPRI))
			FDSET(a->e, fd), ready++;
	}
	return ready;
}

LONG
sb_WaitSelect(struct SocketBase *sb, LONG nfds, APTR read_fds,
    APTR write_fds, APTR except_fds, struct __timeval *timeout,
    ULONG *signals)
{
	struct select_args a;
	ULONG want = signals ? *signals : 0, got = want, pending;
	LONG rv;

	if (nfds < 0) {
		sb_set_errno(sb, EINVAL);
		return -1;
	}
	/* the doc, WaitSelect: "The timeout value must be sound. This means
	   that the number of microseconds must be smaller than 1000000 and
	   the number of seconds must not be larger than 100000000" */
	if (timeout && (timeout->tv_micro >= 1000000 ||
	    timeout->tv_secs > 100000000)) {
		sb_set_errno(sb, EINVAL);
		return -1;
	}
	/* "The 'nfds' parameter may be truncated if it covers more sockets
	   than are currently in use" */
	if (nfds > sb->dtablesize)
		nfds = sb->dtablesize;
	/* the break signal is tested also when the call does not wait (the
	   doc, WaitSelect BUGS, V4.289), and stays set (the signal "will
	   be posted"); bits shared with the user mask count as user
	   signals (NOTES) */
	if (SetSignal(0, 0) & sb->breakmask & ~want) {
		sb_set_errno(sb, EINTR);
		return -1;
	}
	a.nfds = nfds;
	a.r = read_fds;
	a.w = write_fds;
	a.e = except_fds;
	a.has_tv = timeout != NULL;
	if (timeout)
		sbtime_from_tv(&a.tv, timeout->tv_secs, timeout->tv_micro);
	rv = sb_rpc(sb, (sbfn_t)srv_select, &a, RPC_INTERRUPTIBLE, &got);
	/* woken by one of the caller's own signals: "Reception of a user
	   signal with no socket ready will cause WaitSelect() to stop and
	   to return 0" (with the empty sets of a 0 result) */
	/* (and no break signal with it: "Reception of the standard break
	   signal (e.g. via Ctrl+C) will cause WaitSelect() to return -1 and
	   set the error code to EINTR", downloads/sources/NDK3.2/
	   SANA+RoadshowTCP-IP/doc/bsdsocket.doc:10330-10331.  The break
	   signals are not in got, which sb_rpc() hands back with the user
	   signals only, but set again in the task, library.c sb_rpc()) */
	if (rv < 0 && sb->sb_errno == EINTR && (got & want) &&
	    !(SetSignal(0, 0) & sb->breakmask & ~want)) {
		zero_sets(nfds, read_fds, write_fds, except_fds);
		rv = 0;
	}
	if (rv >= 0 && signals) {
		/* NOTES: with a result of 0 or more, user signals that are
		   set are cleared in the task's received signals and stay
		   set in *signals; those not received are cleared there */
		pending = SetSignal(0, want) & want;
		*signals = (got | pending) & want;
	} else if (signals)
		*signals = got;
	return rv;
}

/* ------------------------------------------------------------------------
 * per-base settings
 */

static LONG
srv_evsync(struct SocketBase *sb, void *unused)
{

	return ev_sync(sb, unused);
}

void
sb_SetSocketSignals(struct SocketBase *sb, ULONG int_mask, ULONG io_mask,
    ULONG urgent_mask)
{

	sb->breakmask = int_mask;
	sb->sigiomask = io_mask;
	sb->sigurgmask = urgent_mask;
	/* the event thread watches for out-of-band data while there is a
	   SIGURG signal to send */
	sb_rpc(sb, (sbfn_t)srv_evsync, NULL, 0, NULL);
}

LONG
sb_getdtablesize(struct SocketBase *sb)
{

	return sb->dtablesize;
}

LONG
sb_Errno(struct SocketBase *sb)
{

	return sb->sb_errno;
}

void
sb_SetErrnoPtr(struct SocketBase *sb, APTR errno_ptr, LONG size)
{

	if (size == 1 || size == 2 || size == 4) {
		sb->errnoptr = errno_ptr;
		sb->errnosize = size;
	}
}
