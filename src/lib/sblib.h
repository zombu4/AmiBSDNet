/*
 * AmiBSDNet bsdsocket.library: internal definitions.
 *
 * Every OpenLibrary() returns a private SocketBase (AmiTCP semantics):
 * its own socket table, errno and signal settings.  Each base is served
 * by a server thread inside the stack that owns a NetBSD process (so the
 * base's sockets are that process's file descriptors) and runs on a large
 * stack.  Library calls arrive on the caller's (often tiny) stack, are
 * packed into an Exec message and executed by the server thread.
 *
 * Inside the kernel every socket is non-blocking; blocking semantics,
 * SO_RCVTIMEO/SO_SNDTIMEO and interruption by break signals are
 * implemented here with poll() on the socket plus a per-base "wake"
 * UDP socket on 127.0.0.1 that the stack's waker thread can poke.
 */
#ifndef SBLIB_H
#define SBLIB_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <utility/tagitem.h>
#include <devices/timer.h>

#include "rumpuser_amiga.h"

/* ------------------------------------------------------------------------
 * Amiga-side (AmiTCP/Roadshow ABI) network types.  These are the 4.4BSD
 * layouts; on m68k they are identical to NetBSD's except where noted.
 */

typedef ULONG socklen_t;
typedef ULONG in_addr_t;
typedef UWORD in_port_t;
typedef UBYTE sa_family_t;

struct in_addr {
	in_addr_t s_addr;
};

struct in6_addr {
	UBYTE s6_addr[16];
};

struct sockaddr {
	UBYTE sa_len;
	sa_family_t sa_family;
	char sa_data[14];
};

struct sockaddr_in {
	UBYTE sin_len;
	sa_family_t sin_family;
	in_port_t sin_port;
	struct in_addr sin_addr;
	char sin_zero[8];
};

struct sockaddr_in6 {
	UBYTE sin6_len;
	sa_family_t sin6_family;
	in_port_t sin6_port;
	ULONG sin6_flowinfo;
	struct in6_addr sin6_addr;
	ULONG sin6_scope_id;
};

struct iovec {
	APTR iov_base;
	ULONG iov_len;
};

struct msghdr {			/* same layout as NetBSD's */
	APTR msg_name;
	socklen_t msg_namelen;
	struct iovec *msg_iov;
	LONG msg_iovlen;
	APTR msg_control;
	socklen_t msg_controllen;
	LONG msg_flags;
};

struct linger {
	LONG l_onoff;
	LONG l_linger;
};

typedef struct fd_set {
	ULONG fds_bits[1];	/* variable length; nfds bits */
} fd_set;

struct hostent {
	char *h_name;
	char **h_aliases;
	LONG h_addrtype;
	LONG h_length;
	char **h_addr_list;
};

struct netent {
	char *n_name;
	char **n_aliases;
	LONG n_addrtype;
	ULONG n_net;
};

struct servent {
	char *s_name;
	char **s_aliases;
	LONG s_port;
	char *s_proto;
};

struct protoent {
	char *p_name;
	char **p_aliases;
	LONG p_proto;
};

struct addrinfo {
	LONG ai_flags;
	LONG ai_family;
	LONG ai_socktype;
	LONG ai_protocol;
	socklen_t ai_addrlen;
	char *ai_canonname;
	struct sockaddr *ai_addr;
	struct addrinfo *ai_next;
};

/* WaitSelect()'s timeout (Roadshow name; same layout as timer.device's) */
struct __timeval {
	ULONG tv_secs;
	ULONG tv_micro;
};

/* NetBSD-side types that differ from the Amiga ones */
struct nb_timeval {
	int64_t tv_sec;		/* time_t is 64-bit on NetBSD */
	LONG tv_usec;
};

struct nb_pollfd {
	LONG fd;
	WORD events;
	WORD revents;
};
#define	NB_POLLIN	0x0001
#define	NB_POLLPRI	0x0002
#define	NB_POLLOUT	0x0004
#define	NB_POLLERR	0x0008
#define	NB_POLLHUP	0x0010
#define	NB_POLLNVAL	0x0020

/* constants (identical on AmiTCP and NetBSD unless marked) */
#define	AF_UNSPEC	0
#define	AF_INET		2
#define	AF_INET6	24	/* NetBSD value; Roadshow does not define one */
#define	SOCK_STREAM	1
#define	SOCK_DGRAM	2
#define	SOCK_RAW	3
#define	IPPROTO_IP	0
#define	IPPROTO_ICMP	1
#define	IPPROTO_TCP	6
#define	IPPROTO_UDP	17
#define	SOL_SOCKET	0xffff
#define	SO_DEBUG	0x0001
#define	SO_ACCEPTCONN	0x0002
#define	SO_REUSEADDR	0x0004
#define	SO_KEEPALIVE	0x0008
#define	SO_BROADCAST	0x0020
#define	SO_LINGER	0x0080
#define	SO_SNDBUF	0x1001
#define	SO_RCVBUF	0x1002
#define	SO_SNDTIMEO	0x1005	/* AmiTCP: struct timeval (Amiga) */
#define	SO_RCVTIMEO	0x1006
#define	SO_ERROR	0x1007
#define	SO_TYPE		0x1008
#define	NB_SO_SNDTIMEO	0x100b	/* NetBSD 6+: 64-bit struct timeval */
#define	NB_SO_RCVTIMEO	0x100c
#define	MSG_OOB		0x1
#define	MSG_PEEK	0x2
#define	MSG_DONTWAIT	0x80
#define	INADDR_ANY	0x00000000UL
#define	INADDR_NONE	0xffffffffUL
#define	INADDR_LOOPBACK	0x7f000001UL

#define	FIONBIO		0x8004667eUL
#define	FIOASYNC	0x8004667dUL
#define	FIONREAD	0x4004667fUL
#define	NB_F_GETFL	3
#define	NB_F_SETFL	4
#define	NB_O_NONBLOCK	0x0004

/* errno values (BSD numbering, shared by AmiTCP and NetBSD) */
#define	EPERM		1
#define	ENOENT		2
#define	EINTR		4
#define	EIO		5
#define	EBADF		9
#define	ENOMEM		12
#define	EFAULT		14
#define	EBUSY		16
#define	EINVAL		22
#define	EMFILE		24
#define	EPIPE		32
#define	EAGAIN		35
#define	EWOULDBLOCK	EAGAIN
#define	EINPROGRESS	36
#define	EALREADY	37
#define	ENOTSOCK	38
#define	EAFNOSUPPORT	47
#define	ENETDOWN	50
#define	ENOTCONN	57
#define	ETIMEDOUT	60
#define	ECONNREFUSED	61
#define	EHOSTUNREACH	65
#define	ENOSYS		78

/* h_errno */
#define	HOST_NOT_FOUND	1
#define	TRY_AGAIN	2
#define	NO_RECOVERY	3
#define	NO_DATA		4

/* SocketBaseTagList() encoding */
#define	SBTF_REF	0x8000
#define	SBTF_SET	0x0001
#define	SBTM_CODE(tag)	(((UWORD)(tag) >> 1) & 0x3fff)

/* ------------------------------------------------------------------------
 * per-socket bookkeeping kept by the library
 */

#define	SB_MAXFD	256
#define	SB_DEFAULT_DTABLESIZE	64

struct sbfd {
	UBYTE inuse;
	UBYTE nonblock;		/* user asked for non-blocking I/O */
	UBYTE type;		/* SOCK_STREAM etc. */
	LONG rcvtimeo_ms;	/* 0 = forever */
	LONG sndtimeo_ms;
};

/* ------------------------------------------------------------------------
 * the library base
 */

struct sbcall;

struct SocketBase {
	struct Library lib;

	/* master base only */
	struct SignalSemaphore *masterlock;

	/* per-opener */
	struct SocketBase *master;
	struct Task *owner;
	struct MsgPort *replyport;	/* owner's; NULL until first call */
	struct MsgPort *srvport;	/* the server thread's request port */
	struct Task *srvtask;
	void *srvthread;
	volatile int srvstate;		/* 0 starting, 1 running, -1 failed */
	struct sbcall *curcall;		/* server side: call being executed */

	ULONG breakmask, sigiomask, sigurgmask, sigeventmask;
	LONG sb_errno;
	APTR errnoptr;
	LONG errnosize;
	LONG sb_herrno;
	LONG *herrnoptr;
	LONG dtablesize;
	struct sbfd fds[SB_MAXFD];

	/* interruption of blocking calls */
	LONG wakefd;
	UWORD wakeport;
	volatile ULONG callseq;		/* bumped per call by the caller */
	volatile ULONG abortseq;	/* == callseq: abort that call */
	struct Message abortmsg;
	volatile int abortpending;

	/* static result buffers (AmiTCP functions return pointers to these) */
	char ntoabuf[20];
	char hostname[256];
	struct hostent hent;
	char *hent_aliases[8];
	char *hent_addrs[9];
	UBYTE hent_addrbuf[8 * 16];
	char hent_name[256];
	struct servent sent;
	char sent_name[32];
	char *sent_aliases[2];
	struct protoent pent;
	char *pent_aliases[2];
	struct netent nent;
	char *nent_aliases[2];
	int dbpos_serv, dbpos_proto, dbpos_net;
};

/* ------------------------------------------------------------------------
 * calls into the server thread
 */

typedef LONG (*sbfn_t)(struct SocketBase *, void *);

#define	RPC_INTERRUPTIBLE	0x01	/* break signals abort the call */

/* run fn(sb, args) on sb's server thread; sets errno from *err */
LONG	sb_rpc(struct SocketBase *, sbfn_t, void *, int flags,
	    ULONG *extrasigs);
/* server-side: report an error for the current call */
LONG	sb_fail(struct SocketBase *, LONG err);
void	sb_set_errno(struct SocketBase *, LONG);
void	sb_set_herrno(struct SocketBase *, LONG);

/* server-side helpers for blocking operations */
#define	WAIT_READ	1
#define	WAIT_WRITE	2
LONG	sb_wait_fd(struct SocketBase *, LONG fd, int what, LONG timeout_ms);
int	sb_fdok(struct SocketBase *, LONG fd);
void	sb_drain_wake(struct SocketBase *);
LONG	sb_rumperr(void);

/* stack-wide state (src/lib/library.c) */
struct Library *bsdsocket_create(void);
int	bsdsocket_remove(void);
ULONG	sb_waker_init(void);

/* resolver (src/lib/netdb.c) */
void	netdb_set_nameservers(const ULONG *addrs, int n);
void	netdb_set_domain(const char *);
int	netdb_get_nameservers(ULONG *addrs, int max);
const char *netdb_get_domain(void);

/* rump system calls (NetBSD ABI) */
LONG rump___sysimpl_socket30(LONG, LONG, LONG);
LONG rump___sysimpl_bind(LONG, const void *, socklen_t);
LONG rump___sysimpl_listen(LONG, LONG);
LONG rump___sysimpl_accept(LONG, void *, socklen_t *);
LONG rump___sysimpl_connect(LONG, const void *, socklen_t);
LONG rump___sysimpl_sendto(LONG, const void *, ULONG, LONG, const void *,
    socklen_t);
LONG rump___sysimpl_recvfrom(LONG, void *, ULONG, LONG, void *, socklen_t *);
LONG rump___sysimpl_sendmsg(LONG, const struct msghdr *, LONG);
LONG rump___sysimpl_recvmsg(LONG, struct msghdr *, LONG);
LONG rump___sysimpl_shutdown(LONG, LONG);
LONG rump___sysimpl_setsockopt(LONG, LONG, LONG, const void *, socklen_t);
LONG rump___sysimpl_getsockopt(LONG, LONG, LONG, void *, socklen_t *);
LONG rump___sysimpl_getsockname(LONG, void *, socklen_t *);
LONG rump___sysimpl_getpeername(LONG, void *, socklen_t *);
LONG rump___sysimpl_ioctl(LONG, ULONG, void *);
LONG rump___sysimpl_fcntl(LONG, LONG, LONG);
LONG rump___sysimpl_close(LONG);
LONG rump___sysimpl_dup2(LONG, LONG);
LONG rump___sysimpl_poll(struct nb_pollfd *, ULONG, LONG);
int rump_pub_lwproc_rfork(int);
void rump_pub_lwproc_releaselwp(void);
#define	RUMP_RFCFDG	0x02

/* string helpers (no C library) */
size_t	sb_strlen(const char *);
char	*sb_strlcpy(char *, const char *, size_t);
int	sb_strcasecmp(const char *, const char *);
int	sb_strcmp(const char *, const char *);

#include "bsdsocket_proto.h"

#endif /* SBLIB_H */
