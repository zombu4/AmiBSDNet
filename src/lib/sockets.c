/*
 * bsdsocket.library: the socket calls.
 *
 * Each sb_<call>() runs in the caller's task: it packs its arguments and
 * has srv_<call>() executed by the base's server thread (sb_rpc()).  The
 * server functions talk to the NetBSD kernel and implement blocking
 * behaviour on top of the kernel's non-blocking sockets (see sblib.h).
 */

#include <exec/types.h>
#include <proto/exec.h>

#include "sblib.h"

#define	ARGS(...)	struct { __VA_ARGS__; } a
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

/* register a new kernel socket in the base (kernel side non-blocking) */
static LONG
adopt_fd(struct SocketBase *sb, LONG fd, LONG type, int nonblock)
{
	LONG on = 1;

	if (fd >= sb->dtablesize || fd >= SB_MAXFD) {
		rump___sysimpl_close(fd);
		return sb_fail(sb, EMFILE);
	}
	rump___sysimpl_ioctl(fd, FIONBIO, &on);
	memset(&sb->fds[fd], 0, sizeof(sb->fds[fd]));
	sb->fds[fd].inuse = 1;
	sb->fds[fd].type = (UBYTE)type;
	sb->fds[fd].nonblock = (UBYTE)nonblock;
	return fd;
}

#define	CHECKFD(fd)	do { if (!sb_fdok(sb, fd)) return sb_fail(sb, EBADF); } while (0)

/* ------------------------------------------------------------------------ */

struct socket_args { LONG domain, type, protocol; };

static LONG
srv_socket(struct SocketBase *sb, struct socket_args *a)
{
	LONG fd;

	if ((fd = rump___sysimpl_socket30(a->domain, a->type, a->protocol)) < 0)
		return rumpfail(sb);
	return adopt_fd(sb, fd, a->type, 0);
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

	return RPC(srv_bind);
}

struct listen_args { LONG fd, backlog; };

static LONG
srv_listen(struct SocketBase *sb, struct listen_args *a)
{

	CHECKFD(a->fd);
	if (rump___sysimpl_listen(a->fd, a->backlog) < 0)
		return rumpfail(sb);
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

	CHECKFD(a->fd);
	for (;;) {
		fd = rump___sysimpl_accept(a->fd, a->addr, a->len);
		if (fd >= 0)
			return adopt_fd(sb, fd, sb->fds[a->fd].type,
			    sb->fds[a->fd].nonblock);
		e = sb_rumperr();
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, 0))
			return sb_fail(sb, e);
		if ((e = sb_wait_fd(sb, a->fd, WAIT_READ,
		    sb->fds[a->fd].rcvtimeo_ms)) != 0)
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
	if (e != EINPROGRESS || !blocking(sb, a->fd, 0))
		return sb_fail(sb, e);
	if ((e = sb_wait_fd(sb, a->fd, WAIT_WRITE, 0)) != 0)
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

	return RPC_INTR(srv_connect);
}

/* ------------------------------------------------------------------------
 * data transfer
 */

#define	NB_MSG_NOSIGNAL	0x0400

struct sendto_args {
	LONG fd; APTR buf; LONG len; LONG flags;
	struct sockaddr *to; LONG tolen;
};

static LONG
srv_sendto(struct SocketBase *sb, struct sendto_args *a)
{
	LONG n, e, done = 0;
	int stream;

	CHECKFD(a->fd);
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
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, a->flags))
			return done ? done : sb_fail(sb, e);
		if ((e = sb_wait_fd(sb, a->fd, WAIT_WRITE,
		    sb->fds[a->fd].sndtimeo_ms)) != 0)
			return done ? done : sb_fail(sb, e);
	}
}

LONG
sb_sendto(struct SocketBase *sb, LONG sock, APTR buf, LONG len, LONG flags,
    struct sockaddr *to, LONG tolen)
{
	struct sendto_args a = { sock, buf, len, flags, to, tolen };

	return RPC_INTR(srv_sendto);
}

LONG
sb_send(struct SocketBase *sb, LONG sock, APTR buf, LONG len, LONG flags)
{
	struct sendto_args a = { sock, buf, len, flags, NULL, 0 };

	return RPC_INTR(srv_sendto);
}

#define	NB_MSG_WAITALL	0x0040

struct recvfrom_args {
	LONG fd; APTR buf; LONG len; LONG flags;
	struct sockaddr *from; socklen_t *fromlen;
};

static LONG
srv_recvfrom(struct SocketBase *sb, struct recvfrom_args *a)
{
	LONG n, e, done = 0;
	int waitall;

	CHECKFD(a->fd);
	/* MSG_WAITALL: emulated here (the kernel would sleep in the
	   kernel), for byte streams only; a datagram is one record, and
	   peeking again would return the same bytes */
	waitall = (a->flags & NB_MSG_WAITALL) && !(a->flags & MSG_PEEK) &&
	    sb->fds[a->fd].type == SOCK_STREAM;
	for (;;) {
		n = rump___sysimpl_recvfrom(a->fd, (UBYTE *)a->buf + done,
		    a->len - done, a->flags & ~(MSG_DONTWAIT | NB_MSG_WAITALL),
		    a->from, a->fromlen);
		if (n > 0) {
			done += n;
			if (!waitall || done >= a->len)
				return done;
			continue;
		}
		if (n == 0)
			return done;		/* EOF */
		e = sb_rumperr();
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, a->flags))
			return done ? done : sb_fail(sb, e);
		if ((e = sb_wait_fd(sb, a->fd, WAIT_READ,
		    sb->fds[a->fd].rcvtimeo_ms)) != 0)
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

#define	SENDMSG_IOV	16

/*
 * The kernel socket is non-blocking, so on a byte stream it may take only
 * part of the data; a blocking sendmsg() sends it all (as srv_sendto()),
 * going on with a copy of the rest of the iovecs.
 */
static LONG
srv_sendmsg(struct SocketBase *sb, struct msg_args *a)
{
	struct iovec iov[SENDMSG_IOV];
	struct msghdr m;
	LONG n, e, done = 0, total = 0, i, k;
	int stream;

	CHECKFD(a->fd);
	m = *a->msg;
	stream = sb->fds[a->fd].type == SOCK_STREAM &&
	    blocking(sb, a->fd, a->flags) && m.msg_iovlen > 0 &&
	    m.msg_iovlen <= SENDMSG_IOV;
	if (stream) {
		for (i = 0; i < m.msg_iovlen; i++) {
			iov[i] = m.msg_iov[i];
			total += iov[i].iov_len;
		}
		m.msg_iov = iov;
	}
	for (;;) {
		n = rump___sysimpl_sendmsg(a->fd, &m,
		    (a->flags & ~MSG_DONTWAIT) | NB_MSG_NOSIGNAL);
		if (n >= 0) {
			done += n;
			if (!stream || done >= total)
				return done;
			/* skip what was sent; ancillary data went with it */
			for (k = 0; k < m.msg_iovlen && n >= (LONG)m.msg_iov[k].iov_len;
			    k++)
				n -= m.msg_iov[k].iov_len;
			m.msg_iov += k;
			m.msg_iovlen -= k;
			if (m.msg_iovlen > 0) {
				m.msg_iov[0].iov_base =
				    (UBYTE *)m.msg_iov[0].iov_base + n;
				m.msg_iov[0].iov_len -= n;
			}
			m.msg_control = NULL;
			m.msg_controllen = 0;
			continue;
		}
		e = sb_rumperr();
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, a->flags))
			return done ? done : sb_fail(sb, e);
		if ((e = sb_wait_fd(sb, a->fd, WAIT_WRITE,
		    sb->fds[a->fd].sndtimeo_ms)) != 0)
			return done ? done : sb_fail(sb, e);
	}
}

static LONG
srv_recvmsg(struct SocketBase *sb, struct msg_args *a)
{
	LONG n, e;

	CHECKFD(a->fd);
	for (;;) {
		/* not MSG_WAITALL: the kernel would sleep in the kernel, deaf
		   to non-blocking mode, timeouts and Ctrl-C */
		n = rump___sysimpl_recvmsg(a->fd, a->msg,
		    a->flags & ~(MSG_DONTWAIT | NB_MSG_WAITALL));
		if (n >= 0)
			return n;
		e = sb_rumperr();
		if (e != EWOULDBLOCK || !blocking(sb, a->fd, a->flags))
			return sb_fail(sb, e);
		if ((e = sb_wait_fd(sb, a->fd, WAIT_READ,
		    sb->fds[a->fd].rcvtimeo_ms)) != 0)
			return sb_fail(sb, e);
	}
}

LONG
sb_sendmsg(struct SocketBase *sb, LONG sock, struct msghdr *msg, LONG flags)
{
	struct msg_args a = { sock, msg, flags };

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
tv_to_ms(const struct __timeval *tv)
{

	ULONG secs = tv->tv_secs, ms = (tv->tv_micro + 999) / 1000;

	/* no overflow: a huge timeout is the longest one */
	if (secs >= 0x7fffffffUL / 1000 - 1000)
		return 0x7fffffff;
	return (LONG)(secs * 1000 + ms);
}

static LONG
srv_setsockopt(struct SocketBase *sb, struct sockopt_args *a)
{

	CHECKFD(a->fd);
	if (a->level == SOL_SOCKET &&
	    (a->name == SO_RCVTIMEO || a->name == SO_SNDTIMEO)) {
		/* emulated here: kernel sockets are non-blocking anyway */
		if (a->val == NULL || a->len < (LONG)sizeof(struct __timeval))
			return sb_fail(sb, EINVAL);
		if (a->name == SO_RCVTIMEO)
			sb->fds[a->fd].rcvtimeo_ms = tv_to_ms(a->val);
		else
			sb->fds[a->fd].sndtimeo_ms = tv_to_ms(a->val);
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
		LONG ms = a->name == SO_RCVTIMEO ?
		    sb->fds[a->fd].rcvtimeo_ms : sb->fds[a->fd].sndtimeo_ms;

		if (tv == NULL || a->lenp == NULL ||
		    *a->lenp < sizeof(struct __timeval))
			return sb_fail(sb, EINVAL);
		tv->tv_secs = ms / 1000;
		tv->tv_micro = (ms % 1000) * 1000;
		*a->lenp = sizeof(*tv);
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
	return 0;
}

static LONG
srv_getpeername(struct SocketBase *sb, struct accept_args *a)
{

	CHECKFD(a->fd);
	if (rump___sysimpl_getpeername(a->fd, a->addr, a->len) < 0)
		return rumpfail(sb);
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

LONG	sb_ifioctl(struct SocketBase *, LONG, ULONG, APTR, int *);

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
		return 0;	/* TODO: SIGIO delivery */
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

static LONG
srv_close(struct SocketBase *sb, struct listen_args *a)
{

	CHECKFD(a->fd);
	sb->fds[a->fd].inuse = 0;
	if (rump___sysimpl_close(a->fd) < 0)
		return rumpfail(sb);
	return 0;
}

LONG
sb_CloseSocket(struct SocketBase *sb, LONG sock)
{
	struct listen_args a = { sock, 0 };

	return RPC(srv_close);
}

static LONG
srv_dup2(struct SocketBase *sb, struct listen_args *a)
{
	LONG fd;

	CHECKFD(a->fd);
	if (a->backlog < 0)
		fd = rump___sysimpl_fcntl(a->fd, 0 /* F_DUPFD */, 0);
	else {
		if (a->backlog >= sb->dtablesize)
			return sb_fail(sb, EBADF);
		if (a->backlog == sb->wakefd) {
			/* the base's own wake socket is there (usually fd 0):
			   move it out of the way, above the table */
			LONG nw = rump___sysimpl_fcntl(sb->wakefd,
			    0 /* F_DUPFD */, SB_MAXFD);

			if (nw < 0)
				return sb_fail(sb, EBADF);
			rump___sysimpl_close(sb->wakefd);
			sb->wakefd = nw;
		}
		fd = rump___sysimpl_dup2(a->fd, a->backlog);
	}
	if (fd < 0)
		return rumpfail(sb);
	if (fd >= sb->dtablesize) {
		rump___sysimpl_close(fd);
		return sb_fail(sb, EMFILE);
	}
	sb->fds[fd] = sb->fds[a->fd];
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
	struct __timeval *tv;
};

#define	FDISSET(set, fd)	((set) && ((set)[(fd) >> 5] & (1UL << ((fd) & 31))))
#define	FDSET(set, fd)		((set)[(fd) >> 5] |= (1UL << ((fd) & 31)))

static void
zero_sets(struct select_args *a)
{
	LONG words = (a->nfds + 31) >> 5, i;

	for (i = 0; i < words; i++) {
		if (a->r) a->r[i] = 0;
		if (a->w) a->w[i] = 0;
		if (a->e) a->e[i] = 0;
	}
}

static LONG
srv_select(struct SocketBase *sb, struct select_args *a)
{
	struct nb_pollfd pfd[SB_MAXFD + 1];
	LONG nfds = a->nfds, n, i, timeout, ready = 0, np = 0;
	ULONG start;

	if (nfds < 0 || nfds > SB_MAXFD)
		return sb_fail(sb, EINVAL);
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
	pfd[np].fd = sb->wakefd;
	pfd[np].events = NB_POLLIN;
	pfd[np].revents = 0;

	timeout = a->tv ? tv_to_ms(a->tv) : -1;
	start = amiga_host_ms();
	for (;;) {
		/* aborted before the call started (its wake was drained) */
		if (sb->abortseq == sb->callseq) {
			zero_sets(a);
			return sb_fail(sb, EINTR);
		}
		n = rump___sysimpl_poll(pfd, np + 1, timeout);
		if (n < 0)
			return rumpfail(sb);
		if (pfd[np].revents) {
			sb_drain_wake(sb);
			if (sb->abortseq == sb->callseq) {
				zero_sets(a);
				return sb_fail(sb, EINTR);
			}
			/* stale wake from an earlier call: ignore it */
			pfd[np].revents = 0;
			if (--n == 0) {
				/* (the rest of the timeout, not all of it) */
				if (timeout > 0) {
					ULONG now = amiga_host_ms(),
					    el = now - start;

					timeout = el >= (ULONG)timeout ? 0 :
					    timeout - (LONG)el;
					start = now;
				}
				continue;
			}
		}
		break;
	}
	zero_sets(a);
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
	struct select_args a = { nfds, read_fds, write_fds, except_fds,
	    timeout };
	ULONG want = signals ? *signals : 0, got = want;
	LONG rv;

	rv = sb_rpc(sb, (sbfn_t)srv_select, &a, RPC_INTERRUPTIBLE, &got);
	if (signals)
		*signals = got;
	/* woken by one of the caller's own signals: not an error */
	if (rv < 0 && sb->sb_errno == EINTR && (got & want) &&
	    !(got & sb->breakmask & ~want))
		rv = 0;
	return rv;
}

/* ------------------------------------------------------------------------
 * per-base settings
 */

void
sb_SetSocketSignals(struct SocketBase *sb, ULONG int_mask, ULONG io_mask,
    ULONG urgent_mask)
{

	sb->breakmask = int_mask;
	sb->sigiomask = io_mask;
	sb->sigurgmask = urgent_mask;
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

LONG
sb_GetSocketEvents(struct SocketBase *sb, ULONG *event_ptr)
{

	/* TODO: SBTC_SIGEVENTMASK event delivery */
	return -1;
}
