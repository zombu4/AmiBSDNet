/*
 * AmiBSDNet bsdsocket.library: internal definitions.
 *
 * Every OpenLibrary() returns a private SocketBase (bsdsocket.doc,
 * OpenLibrary): its own socket table, errno and signal settings.  Each
 * base is served by a server thread inside the stack that owns a NetBSD
 * process (so the base's sockets are that process's file descriptors)
 * and runs on a large stack.  Library calls arrive on the caller's (often
 * tiny) stack, are packed into an Exec message and executed by the server
 * thread.
 *
 * Inside the kernel every socket is non-blocking; blocking semantics,
 * SO_RCVTIMEO/SO_SNDTIMEO and interruption by break signals are
 * implemented here with poll() on the socket plus a per-base "wake"
 * UDP socket on 127.0.0.1 that the stack's manager thread can poke.
 * Asynchronous notification (FIOASYNC, SO_EVENTMASK) is done by a
 * per-base event thread, a second NetBSD LWP in the base's process
 * (events.c).
 *
 * Sources for the Amiga-side definitions: the Roadshow SDK in the NDK
 * (SANA+RoadshowTCP-IP/netinclude/..., doc/bsdsocket.doc).  Sources for
 * the NetBSD-side layouts: netbsd-src/sys/...; their sizes and offsets
 * under the kernel's ABI (-malign-int) were measured by compiling the
 * NetBSD headers with the kernel flags of tools/build.py, and are checked
 * here with _Static_assert.
 */
#ifndef SBLIB_H
#define SBLIB_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <devices/timer.h>

#include "rumpuser_amiga.h"

#define	SB_OFFSETOF(t, m)	__builtin_offsetof(t, m)
#define	SB_CHECK(c)		_Static_assert(c, #c)

/* ------------------------------------------------------------------------
 * Amiga-side (Roadshow ABI) network types (netinclude/sys/socket.h,
 * netinet/in.h, netdb.h)
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

/* netinclude/sys/socket.h; NetBSD's (sys/sys/socket.h) has the same
   layout: 28 bytes, measured */
struct msghdr {
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

/* netinclude/netdb.h: ai_addr comes before ai_canonname */
struct addrinfo {
	LONG ai_flags;
	LONG ai_family;
	LONG ai_socktype;
	LONG ai_protocol;
	socklen_t ai_addrlen;
	struct sockaddr *ai_addr;
	char *ai_canonname;
	struct addrinfo *ai_next;
};

/* WaitSelect()'s timeout (Roadshow name; same layout as timer.device's) */
struct __timeval {
	ULONG tv_secs;
	ULONG tv_micro;
};

/* netinclude/net/if.h */
#define	IFNAMSIZ	16
struct ifreq {
	char ifr_name[IFNAMSIZ];
	union {
		struct sockaddr ifru_addr;
		WORD ifru_flags;
		LONG ifru_metric;
		APTR ifru_data;
	} ifr_ifru;
};
struct ifaliasreq {
	char ifra_name[IFNAMSIZ];
	struct sockaddr ifra_addr;
	struct sockaddr ifra_broadaddr;
	struct sockaddr ifra_mask;
};
struct ifconf {
	LONG ifc_len;
	APTR ifc_buf;
};
SB_CHECK(sizeof(struct ifreq) == 32);
SB_CHECK(sizeof(struct ifaliasreq) == 64);

/* netinclude/net/route.h (RTM_VERSION 3) */
struct rs_rt_metrics {
	ULONG rmx_locks, rmx_mtu, rmx_hopcount, rmx_expire, rmx_recvpipe,
	    rmx_sendpipe, rmx_ssthresh, rmx_rtt, rmx_rttvar, rmx_pksent;
};
struct rs_rt_msghdr {
	UWORD rtm_msglen;
	UBYTE rtm_version;
	UBYTE rtm_type;
	UWORD rtm_index;
	LONG rtm_flags;
	LONG rtm_addrs;
	LONG rtm_pid;
	LONG rtm_seq;
	LONG rtm_errno;
	LONG rtm_use;
	ULONG rtm_inits;
	struct rs_rt_metrics rtm_rmx;
};
SB_CHECK(sizeof(struct rs_rt_msghdr) == 74);	/* m68k Amiga ABI: LONGs
						   2-byte aligned */
#define	RS_RTM_VERSION	3
/* netinclude/net/if.h if_data, if_msghdr, ifa_msghdr */
struct rs_if_data {
	UBYTE ifi_type, ifi_addrlen, ifi_hdrlen;
	ULONG ifi_mtu, ifi_metric, ifi_baudrate, ifi_ipackets, ifi_ierrors,
	    ifi_opackets, ifi_oerrors, ifi_collisions, ifi_ibytes, ifi_obytes,
	    ifi_imcasts, ifi_omcasts, ifi_iqdrops, ifi_noproto;
	struct __timeval ifi_lastchange;
};
struct rs_if_msghdr {
	UWORD ifm_msglen;
	UBYTE ifm_version;
	UBYTE ifm_type;
	LONG ifm_addrs;
	LONG ifm_flags;
	UWORD ifm_index;
	struct rs_if_data ifm_data;
};
struct rs_ifa_msghdr {
	UWORD ifam_msglen;
	UBYTE ifam_version;
	UBYTE ifam_type;
	LONG ifam_addrs;
	LONG ifam_flags;
	UWORD ifam_index;
	LONG ifam_metric;
};

/* constants: netinclude/sys/socket.h, netinet/in.h, sys/filio.h,
   sys/sockio.h (values identical in NetBSD unless marked) */
#define	AF_UNSPEC	0
#define	AF_INET		2
#define	AF_ROUTE	17	/* Roadshow; NetBSD's AF_OROUTE */
#define	AF_LINK		18
#define	AF_INET6	24	/* NetBSD value; Roadshow does not define one */
#define	SOCK_STREAM	1
#define	SOCK_DGRAM	2
#define	SOCK_RAW	3
#define	SOCK_RDM	4
#define	SOCK_SEQPACKET	5
#define	IPPROTO_IP	0
#define	IPPROTO_ICMP	1
#define	IPPROTO_TCP	6
#define	IPPROTO_UDP	17
#define	IPPROTO_IPV6	41
#define	SOL_SOCKET	0xffff
#define	SO_DEBUG	0x0001
#define	SO_ACCEPTCONN	0x0002
#define	SO_REUSEADDR	0x0004
#define	SO_KEEPALIVE	0x0008
#define	SO_BROADCAST	0x0020
#define	SO_LINGER	0x0080
#define	SO_SNDBUF	0x1001
#define	SO_RCVBUF	0x1002
#define	SO_SNDTIMEO	0x1005	/* Roadshow: struct timeval (Amiga) */
#define	SO_RCVTIMEO	0x1006
#define	SO_ERROR	0x1007
#define	SO_TYPE		0x1008
#define	SO_EVENTMASK	0x2001	/* Roadshow only */
#define	MSG_OOB		0x1
#define	MSG_PEEK	0x2
#define	MSG_DONTROUTE	0x4
#define	MSG_TRUNC	0x10
#define	MSG_WAITALL	0x40
#define	MSG_DONTWAIT	0x80
#define	INADDR_ANY	0x00000000UL
#define	INADDR_NONE	0xffffffffUL
#define	INADDR_LOOPBACK	0x7f000001UL

#define	FIONBIO		0x8004667eUL
#define	FIOASYNC	0x8004667dUL
#define	FIONREAD	0x4004667fUL
#define	SIOCATMARK	0x40047307UL

/* netinclude/libraries/bsdsocket.h: GetSocketEvents() events */
#define	FD_ACCEPT	0x01
#define	FD_CONNECT	0x02
#define	FD_OOB		0x04
#define	FD_READ		0x08
#define	FD_WRITE	0x10
#define	FD_ERROR	0x20
#define	FD_CLOSE	0x40
#define	FD_ALL		0x7f

/* errno values (netinclude/sys/errno.h; NetBSD's are the same) */
#define	EPERM		1
#define	ENOENT		2
#define	EINTR		4
#define	EIO		5
#define	ENXIO		6
#define	EBADF		9
#define	ENOMEM		12
#define	EACCES		13
#define	EFAULT		14
#define	EBUSY		16
#define	EEXIST		17
#define	ENODEV		19
#define	EINVAL		22
#define	EMFILE		24
#define	ENOTTY		25
#define	ENOSPC		28
#define	EPIPE		32
#define	EDOM		33
#define	ERANGE		34
#define	EAGAIN		35
#define	EWOULDBLOCK	EAGAIN
#define	EINPROGRESS	36
#define	EALREADY	37
#define	ENOTSOCK	38
#define	EMSGSIZE	40
#define	EPROTOTYPE	41
#define	ENOPROTOOPT	42
#define	EPROTONOSUPPORT	43
#define	EOPNOTSUPP	45
#define	EAFNOSUPPORT	47
#define	EADDRNOTAVAIL	49
#define	ENETDOWN	50
#define	ENETUNREACH	51
#define	ENOBUFS		55
#define	EISCONN		56
#define	ENOTCONN	57
#define	ETIMEDOUT	60
#define	ECONNREFUSED	61
#define	ENAMETOOLONG	63
#define	EHOSTUNREACH	65
#define	ENOSYS		78

/* h_errno (netinclude/netdb.h) */
#define	NETDB_INTERNAL	-1
#define	HOST_NOT_FOUND	1
#define	TRY_AGAIN	2
#define	NO_RECOVERY	3
#define	NO_DATA		4

/* SocketBaseTagList() encoding (netinclude/libraries/bsdsocket.h) */
#define	SBTF_REF	0x8000
#define	SBTF_SET	0x0001
#define	SBTM_CODE(tag)	(((ULONG)(tag) >> 1) & 0x3fff)

/* ------------------------------------------------------------------------
 * NetBSD-side layouts (sys/sys/poll.h, sys/sys/time.h, sys/net/if.h,
 * sys/net/route.h, sys/sys/sysctl.h, sys/sys/mbuf.h); 64-bit fields are
 * kept as two ULONGs (big-endian: high word first) because host code
 * cannot use 64-bit arithmetic helpers (no libgcc)
 */

struct nb_u64 {
	ULONG hi, lo;
};

/* struct timeval: 64-bit tv_sec, then long tv_usec (12 bytes) */
struct nb_timeval {
	struct nb_u64 tv_sec;
	LONG tv_usec;
};
SB_CHECK(sizeof(struct nb_timeval) == 12);

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

#define	NB_PF_ROUTE	34	/* sys/sys/socket.h */
#define	NB_SO_SNDTIMEO	0x100b	/* NetBSD 6+: 64-bit struct timeval */
#define	NB_SO_RCVTIMEO	0x100c
#define	NB_MSG_NOSIGNAL	0x0400
#define	NB_F_DUPFD	0
#define	NB_RLIMIT_NOFILE 8

/* struct ifreq: name + 128-byte union (sockaddr_storage) = 144 bytes */
struct nb_ifreq {
	char ifr_name[IFNAMSIZ];
	union {
		struct sockaddr ifru_addr;
		WORD ifru_flags;
		LONG ifru_metric;
		LONG ifru_mtu;
		ULONG ifru_value;
		APTR ifru_data;
		UBYTE ifru_space[128];
	} ifr_ifru;
};
SB_CHECK(sizeof(struct nb_ifreq) == 144);
struct nb_ifconf {
	LONG ifc_len;
	APTR ifc_buf;
};
/* struct if_data (132 bytes; 64-bit fields at 4-byte alignment) */
struct nb_if_data {
	UBYTE ifi_type, ifi_addrlen, ifi_hdrlen, ifi_pad;
	LONG ifi_link_state;
	struct nb_u64 ifi_mtu, ifi_metric, ifi_baudrate, ifi_ipackets,
	    ifi_ierrors, ifi_opackets, ifi_oerrors, ifi_collisions,
	    ifi_ibytes, ifi_obytes, ifi_imcasts, ifi_omcasts, ifi_iqdrops,
	    ifi_noproto;
	struct nb_u64 ifi_lastchange_sec;	/* struct timespec */
	LONG ifi_lastchange_nsec;
};
SB_CHECK(sizeof(struct nb_if_data) == 132);
SB_CHECK(SB_OFFSETOF(struct nb_if_data, ifi_mtu) == 8);
SB_CHECK(SB_OFFSETOF(struct nb_if_data, ifi_ipackets) == 32);
SB_CHECK(SB_OFFSETOF(struct nb_if_data, ifi_lastchange_sec) == 120);
struct nb_ifdatareq {
	char ifdr_name[IFNAMSIZ];
	struct nb_if_data ifdr_data;
};
SB_CHECK(sizeof(struct nb_ifdatareq) == 148);

/* FIONWRITE, bytes still in the send queue: _IOR('f', 121, int)
   (netbsd-src/sys/sys/filio.h:57, ioccom.h:58,66-70) */
#define	NB_FIONWRITE		0x40046679UL

/* ioctl numbers (NetBSD sys/sys/sockio.h with NetBSD's sizes, values
   measured) */
#define	NB_SIOCSIFADDR		0x8090690cUL
#define	NB_SIOCGIFADDR		0xc0906921UL
#define	NB_SIOCSIFDSTADDR	0x8090690eUL
#define	NB_SIOCGIFDSTADDR	0xc0906922UL
#define	NB_SIOCSIFFLAGS		0x80906910UL
#define	NB_SIOCGIFFLAGS		0xc0906911UL
#define	NB_SIOCGIFBRDADDR	0xc0906923UL
#define	NB_SIOCSIFBRDADDR	0x80906913UL
#define	NB_SIOCGIFCONF		0xc0086926UL
#define	NB_SIOCGIFNETMASK	0xc0906925UL
#define	NB_SIOCSIFNETMASK	0x80906916UL
#define	NB_SIOCGIFMETRIC	0xc0906917UL
#define	NB_SIOCSIFMETRIC	0x80906918UL
#define	NB_SIOCDIFADDR		0x80906919UL
#define	NB_SIOCAIFADDR		0x8040691aUL
/* _IOWR('i', 27, struct ifaliasreq), netbsd-src/sys/sys/sockio.h:77 */
#define	NB_SIOCGIFALIAS		0xc040691bUL
#define	NB_SIOCADDMULTI		0x80906931UL
#define	NB_SIOCDELMULTI		0x80906932UL
#define	NB_SIOCGIFMTU		0xc090697eUL
#define	NB_SIOCSIFMTU		0x8090697fUL
#define	NB_SIOCGIFDATA		0xc0946985UL

/* interface flags: NetBSD sys/net/if.h; all but 0x20 mean the same as
   Roadshow's net/if.h (0x20 is IFF_NOTRAILERS there, IFF_UNNUMBERED in
   NetBSD) */
#define	IFF_UP		0x0001
#define	IFF_BROADCAST	0x0002
#define	IFF_DEBUG	0x0004
#define	IFF_LOOPBACK	0x0008
#define	IFF_POINTOPOINT	0x0010
#define	IFF_0X20	0x0020
#define	IFF_RUNNING	0x0040
#define	IFF_MULTICAST	0x8000

/* routing messages (NetBSD sys/net/route.h, RTM_VERSION 4) */
struct nb_rt_metrics {
	struct nb_u64 rmx_locks, rmx_mtu, rmx_hopcount, rmx_recvpipe,
	    rmx_sendpipe, rmx_ssthresh, rmx_rtt, rmx_rttvar, rmx_expire,
	    rmx_pksent;
};
SB_CHECK(sizeof(struct nb_rt_metrics) == 80);
struct nb_rt_msghdr {
	UWORD rtm_msglen;
	UBYTE rtm_version;
	UBYTE rtm_type;
	UWORD rtm_index;
	UWORD rtm_pad0;
	LONG rtm_flags;
	LONG rtm_addrs;
	LONG rtm_pid;
	LONG rtm_seq;
	LONG rtm_errno;
	LONG rtm_use;
	LONG rtm_inits;
	LONG rtm_pad1;
	struct nb_rt_metrics rtm_rmx;
};
SB_CHECK(sizeof(struct nb_rt_msghdr) == 120);
SB_CHECK(SB_OFFSETOF(struct nb_rt_msghdr, rtm_flags) == 8);
SB_CHECK(SB_OFFSETOF(struct nb_rt_msghdr, rtm_rmx) == 40);
struct nb_if_msghdr {
	UWORD ifm_msglen;
	UBYTE ifm_version;
	UBYTE ifm_type;
	LONG ifm_addrs;
	LONG ifm_flags;
	UWORD ifm_index;
	UWORD ifm_pad;
	struct nb_if_data ifm_data;
	LONG ifm_pad2;
};
SB_CHECK(SB_OFFSETOF(struct nb_if_msghdr, ifm_index) == 12);
SB_CHECK(SB_OFFSETOF(struct nb_if_msghdr, ifm_data) == 16);
SB_CHECK(sizeof(struct nb_if_msghdr) == 152);
struct nb_ifa_msghdr {
	UWORD ifam_msglen;
	UBYTE ifam_version;
	UBYTE ifam_type;
	UWORD ifam_index;
	UWORD ifam_pad;
	LONG ifam_flags;
	LONG ifam_addrs;
	LONG ifam_pid;
	LONG ifam_addrflags;
	LONG ifam_metric;
	LONG ifam_pad2;
};
SB_CHECK(SB_OFFSETOF(struct nb_ifa_msghdr, ifam_metric) == 24);
SB_CHECK(sizeof(struct nb_ifa_msghdr) == 32);
#define	NB_RTM_VERSION	4
#define	RTM_ADD		0x1
#define	RTM_DELETE	0x2
#define	RTM_CHANGE	0x3
#define	RTM_GET		0x4
#define	RTM_LOSING	0x5
#define	RTM_REDIRECT	0x6
#define	RTM_MISS	0x7
#define	RTM_LOCK	0x8
#define	RS_RTM_NEWADDR	0xc	/* Roadshow numbers */
#define	RS_RTM_DELADDR	0xd
#define	RS_RTM_IFINFO	0xe
#define	NB_RTM_IFINFO	0x14	/* NetBSD numbers */
#define	NB_RTM_NEWADDR	0x16
#define	NB_RTM_DELADDR	0x17
#define	RTF_UP		0x1
#define	RTF_GATEWAY	0x2
#define	RTF_HOST	0x4
#define	RTF_STATIC	0x800
#define	RTF_LLINFO	0x400	/* Roadshow; NetBSD RTF_LLDATA */
#define	RTA_DST		0x1
#define	RTA_GATEWAY	0x2
#define	RTA_NETMASK	0x4
#define	RTA_IFA		0x20
#define	RS_RTAX_MAX	8	/* netinclude/net/route.h */
#define	NB_RTAX_MAX	9	/* sys/net/route.h */
#define	NB_NET_RT_DUMP	1
#define	NB_NET_RT_FLAGS	2

/* sysctl (sys/sys/sysctl.h): struct sysctlnode is 96 bytes */
struct nb_sysctlnode {
	ULONG sysctl_flags;
	LONG sysctl_num;
	char sysctl_name[32];
	ULONG sysctl_ver;
	ULONG sysctl_rsvd;
	UBYTE sysctl_un[16];
	UBYTE sysctl_rest[32];
};
SB_CHECK(sizeof(struct nb_sysctlnode) == 96);
SB_CHECK(SB_OFFSETOF(struct nb_sysctlnode, sysctl_ver) == 40);
#define	NB_CTL_QUERY	(-2)
#define	NB_SYSCTL_VERSION 0x01000000UL
#define	NB_CTL_KERN	1
#define	NB_CTL_NET	4
#define	NB_KERN_ARND	81
#define	NB_KERN_MBUF	39
#define	NB_MBUF_STATS	6

/* struct kinfo_pcb (640 bytes) */
struct nb_kinfo_pcb {
	struct nb_u64 ki_pcbaddr, ki_ppcbaddr, ki_sockaddr;
	ULONG ki_family, ki_type, ki_protocol, ki_pflags;
	ULONG ki_sostate, ki_prstate;
	LONG ki_tstate;
	ULONG ki_tflags;
	struct nb_u64 ki_rcvq, ki_sndq;
	UBYTE ki_s[264];
	UBYTE ki_d[264];
	struct nb_u64 ki_inode, ki_vnode, ki_conn, ki_refs, ki_nextref;
};
SB_CHECK(sizeof(struct nb_kinfo_pcb) == 640);
SB_CHECK(SB_OFFSETOF(struct nb_kinfo_pcb, ki_tstate) == 48);
SB_CHECK(SB_OFFSETOF(struct nb_kinfo_pcb, ki_s) == 72);
SB_CHECK(SB_OFFSETOF(struct nb_kinfo_pcb, ki_d) == 336);

/* ------------------------------------------------------------------------
 * per-socket bookkeeping kept by the library
 */

/* SBTC_DTABLESIZE: "For Roadshow, the default is 256 sockets" */
#define	SB_MAXFD		256
#define	SB_DEFAULT_DTABLESIZE	256
/* the base's own kernel sockets (wake, event thread) live at and above
   this descriptor, out of the way of the table */
#define	SB_PRIVFD		SB_MAXFD

/* a timeout (SO_RCVTIMEO/SO_SNDTIMEO, WaitSelect()) */
struct sbtime {
	ULONG s;	/* seconds */
	ULONG ms;	/* + milliseconds (< 1000) */
};

/* a network database being read (netdb.c) */
struct sbdb {
	BPTR fh;		/* the file, when mode is SBDB_FILE */
	UBYTE mode;		/* SBDB_* */
	UBYTE stay;		/* setservent(): stay open */
	int pos;		/* position in the built-in table */
};
#define	SBDB_CLOSED	0
#define	SBDB_FILE	1
#define	SBDB_BUILTIN	2

struct sbfd {
	UBYTE inuse;
	UBYTE nonblock;		/* user asked for non-blocking I/O */
	UBYTE type;		/* SOCK_STREAM etc. */
	UBYTE route;		/* a Roadshow AF_ROUTE socket (route.c) */
	UBYTE domain;		/* as given to socket() (Roadshow numbers) */
	UBYTE async;		/* FIOASYNC */
	UBYTE has_rcvto, has_sndto;
	LONG protocol;
	struct sbtime rcvto, sndto;
	/* asynchronous events (events.c), under SocketBase.evlock */
	ULONG eventmask;	/* SO_EVENTMASK */
	ULONG armed;		/* FD_* that may be reported next */
	ULONG pending;		/* FD_* reported, not yet fetched */
	ULONG sigio_armed;	/* conditions that may send SIGIO/SIGURG */
	LONG pend_error;	/* error code reported with FD_ERROR */
	UBYTE listening;	/* listen() was called */
	UBYTE queued;		/* in the event queue */
	UBYTE connecting;	/* non-blocking connect() in progress */
	UBYTE closed;		/* FD_CLOSE was reported */
};

/* ------------------------------------------------------------------------
 * the library base
 */

struct sbcall;
struct sbevents;
struct SocketBase;

/*
 * A server thread of a base: it runs the calls in the base's NetBSD
 * process.  The main one serves the opener; with
 * SBTC_CAN_SHARE_LIBRARY_BASES every other task that calls gets one of
 * its own in the same process (library.c sb_rpc()), so that a call that
 * blocks holds up only the task that made it.
 */
struct sbserver {
	struct sbserver *next;		/* SocketBase.extra */
	struct SocketBase *sb;
	struct Task *client;		/* the task it serves (main: NULL) */
	struct MsgPort *port;		/* its request port */
	struct Task *task;
	volatile int state;		/* SRV_* (library.c) */
	struct sbcall *curcall;		/* the call being executed */
	/* interruption of blocking calls */
	LONG wakefd;
	UWORD wakeport;
	volatile ULONG callseq;		/* the call it is executing */
	volatile ULONG abortseq;	/* == callseq: abort that call */
	struct Message abortmsg;
	volatile int abortpending;
	volatile LONG callers;		/* tasks inside sb_rpc() on it */
};

struct SocketBase {
	struct Library lib;

	/* per-opener */
	struct SocketBase *master;
	struct Task *owner;
	struct MsgPort *replyport;	/* owner's; NULL until first call */
	struct sbserver main;		/* the opener's server thread */
	struct sbserver *extra;		/* the other tasks' (shared base) */
	/* held by the calls that cannot be interrupted, which are the ones
	   that use the base's own buffers and databases; only the other
	   calls of a shared base run at the same time */
	struct SignalSemaphore calllock;
	LONG pid;			/* the base's NetBSD process */
	int share;			/* SBTC_CAN_SHARE_LIBRARY_BASES */

	ULONG breakmask, sigiomask, sigurgmask, sigeventmask;
	ULONG sigaddrmask;		/* SBTC_SIG_ADDRESS_CHANGE_MASK */
	LONG sb_errno;
	APTR errnoptr;
	LONG errnosize;
	LONG sb_herrno;
	LONG *herrnoptr;
	struct Hook *errorhook;		/* SBTC_ERROR_HOOK */
	LONG dtablesize;
	struct sbfd fds[SB_MAXFD];

	/* syslog (SBTC_LOGSTAT etc.) */
	LONG logstat, logfacility, logmask;
	STRPTR logtag;

	/* numbers the calls (callers), for their interruption */
	volatile ULONG seqgen;

	/* asynchronous events (events.c) */
	struct SignalSemaphore evlock;
	struct sbevents *ev;
	UBYTE evqueue[SB_MAXFD];	/* FIFO of fds with pending events */
	int evhead, evcount;

	/* static result buffers (functions return pointers to these) */
	char ntoabuf[20];
	char errbuf[40];		/* SBTC_ERRNOSTRPTR "Unknown error" */
	struct hostent hent;
	char hentbuf[1024];		/* hent's name, aliases and addresses */
	struct servent sent;
	struct protoent pent;
	char *pentaliases[1];		/* pent's (empty) alias list: not
					   dbaliases, which sent uses */
	char dbline[512];		/* last database line, split up */
	char *dbaliases[34];
	/* databases (setservent() etc.) */
	struct sbdb db_serv;
	int dbpos_proto;		/* position in the built-in table */
};

/* ------------------------------------------------------------------------
 * calls into the server thread
 */

typedef LONG (*sbfn_t)(struct SocketBase *, void *);

#define	RPC_INTERRUPTIBLE	0x01	/* break signals abort the call */

/* run fn(sb, args) on sb's server thread; sets errno from *err */
LONG	sb_rpc(struct SocketBase *, sbfn_t, void *, int flags,
	    ULONG *extrasigs);
/* server-side: the base's server thread that is running (NULL: none) */
struct sbserver *sb_srv(struct SocketBase *);
/* server-side: report an error for the current call */
LONG	sb_fail(struct SocketBase *, LONG err);
/* server-side: was the current call interrupted (EINTR reported)? */
int	sb_interrupted(struct SocketBase *);
void	sb_set_errno(struct SocketBase *, LONG);
void	sb_set_herrno(struct SocketBase *, LONG);

/* server-side helpers for blocking operations */
#define	WAIT_READ	1
#define	WAIT_WRITE	2
#define	WAIT_PRI	4
LONG	sb_wait_fd(struct SocketBase *, LONG fd, int what,
	    const struct sbtime *timeout);
int	sb_fdok(struct SocketBase *, LONG fd);
void	sb_drain_wake(struct SocketBase *);
LONG	sb_rumperr(void);
/* timeouts: poll() takes an int of milliseconds, so long timeouts are
   waited for in chunks */
LONG	sbtime_chunk(const struct sbtime *);
void	sbtime_sub(struct sbtime *, ULONG ms);
int	sbtime_iszero(const struct sbtime *);
void	sbtime_from_tv(struct sbtime *, ULONG secs, ULONG micro);

/* stack-wide state (src/lib/library.c) */
struct Library *bsdsocket_create(void);

/* resolver (src/lib/netdb.c) */
void	netdb_set_nameservers(const ULONG *addrs, int n);
void	netdb_set_domain(const char *);
void	netdb_set_hostname(const char *);
int	netdb_get_nameservers(ULONG *addrs, int max);
const char *netdb_get_domain(void);
int	netdb_resolve_addr(struct SocketBase *, const char *, ULONG *);
int	netdb_parse_inet_aton(const char *, ULONG *);
int	netdb_nameserver_count(void);
ULONG	netdb_inet_network(const char *);
int	netdb_lookup_net(struct SocketBase *, const char *, ULONG *);
const char *sb_strerror(struct SocketBase *, LONG);

/* asynchronous events (src/lib/events.c) */
int	ev_setmask(struct SocketBase *, LONG fd, ULONG mask);
int	ev_setasync(struct SocketBase *, LONG fd, int on);
void	ev_note(struct SocketBase *, LONG fd, ULONG fdbits, ULONG siobits);
void	ev_fd_closed(struct SocketBase *, LONG fd);
void	ev_stop(struct SocketBase *);
LONG	ev_sync(struct SocketBase *, void *);
/* conditions in sbfd.sigio_armed */
#define	SIGIO_READ	0x01
#define	SIGIO_WRITE	0x02
#define	SIGIO_URG	0x04

/* routing socket translation (src/lib/route.c) */
LONG	route_send(struct SocketBase *, LONG fd, const UBYTE *, LONG len,
	    LONG flags);
LONG	route_recv(struct SocketBase *, LONG fd, UBYTE *, LONG len,
	    LONG flags);
int	route_sysctl_dump(int af, int op, int arg, UBYTE **bufp,
	    ULONG *lenp);
ULONG	route_status(void);
/* SBSYSSTAT_ flags of route_status() (netinclude/libraries/bsdsocket.h:292-295) */
#define	SBSYSSTAT_Routes	(1L << 4)
#define	SBSYSSTAT_DefaultRoute	(1L << 5)
void	route_sockaddr_to_amiga(struct sockaddr *);

/* interface ioctls and the interface API (src/lib/ifapi.c) */
LONG	sb_ifioctl(struct SocketBase *, LONG, ULONG, APTR, int *);
int	if_status_flags(ULONG *flags);	/* 0, or the errno */
int	if_get_flags(const char *name, LONG *flags);

/* Berkeley packet filter channels this library offers
   (src/lib/bpf.c): netbsd-src/sys/net/bpf.c is not among the sources
   tools/build.py compiles, so there are none */
#define	SB_BPF_CHANNELS	0

/* monitoring hooks (src/lib/monitor.c) */
LONG	mon_connect(struct SocketBase *, LONG, struct sockaddr *, LONG);
LONG	mon_bind(struct SocketBase *, LONG, struct sockaddr *, LONG);
LONG	mon_send(struct SocketBase *, LONG, APTR, LONG, LONG,
	    struct sockaddr *, LONG, struct msghdr *);
void	mon_base_closed(struct SocketBase *);
void	mon_init(void);

/* per-base cleanup at CloseLibrary() */
void	netdb_base_closed(struct SocketBase *);
void	log_base_closed(struct SocketBase *);
void	held_init(void);
void	netdb_init(void);
void	if_init(void);
void	stats_init(void);

/* kernel queries (src/lib/stats.c); nb_sysctl_lookup() and
   nb_bytes_total() return 0 or the error number */
int	nb_sysctl(const LONG *mib, ULONG n, void *old, ULONG *oldlen,
	    const void *new, ULONG newlen);
int	nb_sysctl_lookup(const char *const *names, LONG *mib, ULONG *n);
int	nb_bytes_total(ULONG *in_hi, ULONG *in_lo, ULONG *out_hi,
	    ULONG *out_lo);
int	nb_tag_get(int code, ULONG *v);
int	nb_tag_set(int code, ULONG v);
ULONG	nb_u64_clamp(const struct nb_u64 *);

/*
 * A kernel wall-clock time (Unix seconds: rumpuser_clock_gettime() adds
 * AMIGA_EPOCH_OFFSET to the Amiga system time, rumpuser_amiga.c) as Amiga
 * seconds, which count from 01-Jan-1978 (utility.doc Amiga2Date).  0, "not
 * set" in the kernel's structures, stays 0.
 */
#define	SB_EPOCH_OFFSET	252460800UL
static inline ULONG
sb_amiga_secs(ULONG unixsecs)
{

	return unixsecs > SB_EPOCH_OFFSET ? unixsecs - SB_EPOCH_OFFSET : 0;
}

/* syslog formatting (src/lib/misc.c) */
int	sb_format(char *buf, int size, const char *fmt, const LONG *args,
	    struct SocketBase *sb);

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
LONG rump___sysimpl_getrlimit(LONG, void *);
LONG rump___sysimpl_setrlimit(LONG, const void *);
LONG rump___sysimpl_getpid(void);
LONG rump___sysimpl___sysctl(const LONG *, ULONG, void *, ULONG *,
    const void *, ULONG);
int rump_pub_lwproc_rfork(int);
int rump_pub_lwproc_newlwp(LONG);
void rump_pub_lwproc_releaselwp(void);
#define	RUMP_RFCFDG	0x02

/* string helpers (no C library) */
size_t	sb_strlen(const char *);
char	*sb_strlcpy(char *, const char *, size_t);
int	sb_strcasecmp(const char *, const char *);
int	sb_strncasecmp(const char *, const char *, size_t);
int	sb_strcmp(const char *, const char *);
char	*sb_fmt_ulong(char *, ULONG);
struct TagItem *sb_next_tag(struct TagItem **);
void	sb_newlist(struct List *);

/* types the generated prototypes name */
struct rt_msghdr;
struct AddressAllocationMessage;
struct mbuf;
struct Process;
struct List;

#include "bsdsocket_proto.h"

#endif /* SBLIB_H */
