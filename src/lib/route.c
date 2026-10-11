/*
 * bsdsocket.library: routing sockets and the routing API.
 *
 * Roadshow's routing socket: AF_ROUTE is 17
 * (downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/netinclude/sys/socket.h),
 * messages are RTM_VERSION 3 with the layouts of netinclude/net/route.h
 * and netinclude/net/if.h.  The addresses after a header follow each
 * other by their sa_len, sizeof(long) when that is 0, as Roadshow's own
 * ShowNetStatus reads them (source_code/Roadshow/ShowNetStatus.c
 * print_route_table()) and arp.c by sdl_len (source_code/4.4BSD-Lite2/
 * arp.c); the addresses this code produces are padded to a multiple of
 * sizeof(long) with sa_len covering the padding, so either way of
 * stepping finds them.
 *
 * NetBSD 11's: AF_ROUTE 34 with RTM_VERSION 4 (netbsd-src/sys/net/route.h:
 * struct rt_msghdr, struct rt_metrics with 64-bit members in a different
 * order, RT_ROUNDUP() to a multiple of sizeof(uint64_t)) and other numbers
 * for RTM_IFINFO/RTM_NEWADDR/RTM_DELADDR.  Messages written to a Roadshow
 * routing socket are translated to NetBSD's and the kernel's messages
 * back; NetBSD message types Roadshow does not have are not passed on.
 *
 * "the doc" is downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/utility.h>

#include "sblib.h"

#define	RS_ROUNDUP(a)	((a) > 0 ? (1 + (((a) - 1) | 3)) : 4)
#define	NB_ROUNDUP(a)	((a) > 0 ? (1 + (((a) - 1) | 7)) : 8)

#define	RTMSG_MAX	2048	/* one routing message, either form */

/*
 * rtm_flags.  NetBSD keeps the values of netinclude/net/route.h except:
 * 0x100 is RTF_CONNECTED (Roadshow RTF_CLONING), 0x200 is not used
 * (RTF_XRESOLVE), 0x400 is RTF_LLDATA, which marks the ARP entries
 * Roadshow flags RTF_LLINFO; flags from 0x10000 on are NetBSD's own
 * (netbsd-src/sys/net/route.h).
 */
#define	SHARED_FLAGS	0x0000fcffUL	/* the same bits on both sides */

static LONG
flags_to_amiga(LONG f)
{

	return f & SHARED_FLAGS;
}

static LONG
flags_to_netbsd(LONG f)
{

	return f & SHARED_FLAGS;
}

/* NetBSD 64-bit -> Roadshow ULONG (saturating) */
ULONG
nb_u64_clamp(const struct nb_u64 *v)
{

	return v->hi ? 0xffffffffUL : v->lo;
}

static void
u64_set(struct nb_u64 *d, ULONG v)
{

	d->hi = 0;
	d->lo = v;
}

void
route_sockaddr_to_amiga(struct sockaddr *sa)
{

	if (sa->sa_family == NB_PF_ROUTE)
		sa->sa_family = AF_ROUTE;
}

/*
 * Copy the addresses named in *addrs from one form to the other; returns
 * the bytes written to d, or -1 if they do not fit.  The RTA_ bits are
 * the same for 0x01..0x80; NetBSD's 0x100 (RTA_TAG) has no Roadshow
 * counterpart (RTAX_MAX 8 in netinclude/net/route.h) and is dropped.
 */
static LONG
copy_addrs(LONG *addrs, const UBYTE *s, LONG slen, UBYTE *d, LONG dlen,
    int to_netbsd)
{
	LONG bit, out = 0, in = 0, nbits = to_netbsd ? RS_RTAX_MAX :
	    NB_RTAX_MAX, keep = 0;

	for (bit = 0; bit < nbits; bit++) {
		const struct sockaddr *sa;
		LONG l, sl, dl;

		if (!(*addrs & (1L << bit)))
			continue;
		if (in + 2 > slen)
			break;			/* truncated message */
		sa = (const struct sockaddr *)(s + in);
		l = sa->sa_len;
		sl = to_netbsd ? RS_ROUNDUP(l) : NB_ROUNDUP(l);
		if (in + sl > slen)
			break;
		in += sl;
		if (bit >= RS_RTAX_MAX)
			continue;		/* RTA_TAG */
		dl = to_netbsd ? NB_ROUNDUP(l) : RS_ROUNDUP(l);
		if (out + dl > dlen)
			return -1;
		memset(d + out, 0, dl);
		if (l > 0)
			CopyMem((APTR)sa, d + out, l);
		/* Roadshow side: sa_len counts the padding (see above) */
		if (!to_netbsd)
			((struct sockaddr *)(d + out))->sa_len = dl;
		out += dl;
		keep |= 1L << bit;
	}
	*addrs = keep;
	return out;
}

/* Roadshow message -> NetBSD message; returns its length or -errno */
static LONG
msg_to_netbsd(const UBYTE *in, LONG inlen, UBYTE *out, LONG outlen)
{
	const struct rs_rt_msghdr *r = (const struct rs_rt_msghdr *)in;
	struct nb_rt_msghdr *n = (struct nb_rt_msghdr *)out;
	LONG addrs, al;

	if (inlen < (LONG)sizeof(*r) || r->rtm_msglen > inlen ||
	    r->rtm_msglen < sizeof(*r))
		return -EINVAL;
	/* another rtm_version: EPROTONOSUPPORT, as the kernel does for its
	   own (netbsd-src/sys/net/rtsock_shared.c route_output()) */
	if (r->rtm_version != RS_RTM_VERSION)
		return -EPROTONOSUPPORT;
	memset(n, 0, sizeof(*n));
	n->rtm_version = NB_RTM_VERSION;
	n->rtm_type = r->rtm_type;
	n->rtm_index = r->rtm_index;
	n->rtm_flags = flags_to_netbsd(r->rtm_flags);
	n->rtm_pid = r->rtm_pid;
	n->rtm_seq = r->rtm_seq;
	n->rtm_errno = r->rtm_errno;
	n->rtm_use = r->rtm_use;
	/* RTV_ bits: the same values in both headers */
	n->rtm_inits = r->rtm_inits;
	u64_set(&n->rtm_rmx.rmx_locks, r->rtm_rmx.rmx_locks);
	u64_set(&n->rtm_rmx.rmx_mtu, r->rtm_rmx.rmx_mtu);
	u64_set(&n->rtm_rmx.rmx_hopcount, r->rtm_rmx.rmx_hopcount);
	/* (Amiga seconds to the kernel's wall clock, rtsock_shared.c:
	   1132-1133; 0, no expiry, stays 0) */
	if (r->rtm_rmx.rmx_expire) {
		n->rtm_rmx.rmx_expire.hi = r->rtm_rmx.rmx_expire >
		    0xffffffffUL - SB_EPOCH_OFFSET;
		n->rtm_rmx.rmx_expire.lo = r->rtm_rmx.rmx_expire +
		    SB_EPOCH_OFFSET;
	} else
		u64_set(&n->rtm_rmx.rmx_expire, 0);
	u64_set(&n->rtm_rmx.rmx_recvpipe, r->rtm_rmx.rmx_recvpipe);
	u64_set(&n->rtm_rmx.rmx_sendpipe, r->rtm_rmx.rmx_sendpipe);
	u64_set(&n->rtm_rmx.rmx_ssthresh, r->rtm_rmx.rmx_ssthresh);
	u64_set(&n->rtm_rmx.rmx_rtt, r->rtm_rmx.rmx_rtt);
	u64_set(&n->rtm_rmx.rmx_rttvar, r->rtm_rmx.rmx_rttvar);
	u64_set(&n->rtm_rmx.rmx_pksent, r->rtm_rmx.rmx_pksent);
	addrs = r->rtm_addrs & ((1L << RS_RTAX_MAX) - 1);
	al = copy_addrs(&addrs, in + sizeof(*r), r->rtm_msglen - sizeof(*r),
	    out + sizeof(*n), outlen - sizeof(*n), 1);
	if (al < 0)
		return -ENOBUFS;
	n->rtm_addrs = addrs;
	n->rtm_msglen = sizeof(*n) + al;
	return n->rtm_msglen;
}

static void
if_data_to_amiga(const struct nb_if_data *nd, struct rs_if_data *rd)
{

	rd->ifi_type = nd->ifi_type;
	rd->ifi_addrlen = nd->ifi_addrlen;
	rd->ifi_hdrlen = nd->ifi_hdrlen;
	rd->ifi_mtu = nb_u64_clamp(&nd->ifi_mtu);
	rd->ifi_metric = nb_u64_clamp(&nd->ifi_metric);
	rd->ifi_baudrate = nb_u64_clamp(&nd->ifi_baudrate);
	rd->ifi_ipackets = nb_u64_clamp(&nd->ifi_ipackets);
	rd->ifi_ierrors = nb_u64_clamp(&nd->ifi_ierrors);
	rd->ifi_opackets = nb_u64_clamp(&nd->ifi_opackets);
	rd->ifi_oerrors = nb_u64_clamp(&nd->ifi_oerrors);
	rd->ifi_collisions = nb_u64_clamp(&nd->ifi_collisions);
	rd->ifi_ibytes = nb_u64_clamp(&nd->ifi_ibytes);
	rd->ifi_obytes = nb_u64_clamp(&nd->ifi_obytes);
	rd->ifi_imcasts = nb_u64_clamp(&nd->ifi_imcasts);
	rd->ifi_omcasts = nb_u64_clamp(&nd->ifi_omcasts);
	rd->ifi_iqdrops = nb_u64_clamp(&nd->ifi_iqdrops);
	rd->ifi_noproto = nb_u64_clamp(&nd->ifi_noproto);
	rd->ifi_lastchange.tv_secs =
	    sb_amiga_secs(nb_u64_clamp(&nd->ifi_lastchange_sec));
	rd->ifi_lastchange.tv_micro = nd->ifi_lastchange_nsec / 1000;
}

/*
 * NetBSD message -> Roadshow message; returns its length, 0 for a
 * message type Roadshow does not have, or -1 if it does not fit.
 */
static LONG
msg_to_amiga(const UBYTE *in, LONG inlen, UBYTE *out, LONG outlen)
{
	LONG addrs, al, hl, ml;

	if (inlen < 4 || in[2] != NB_RTM_VERSION)
		return 0;
	ml = *(const UWORD *)in;
	if (ml < inlen)
		inlen = ml;
	switch (in[3]) {
	case RTM_ADD: case RTM_DELETE: case RTM_CHANGE: case RTM_GET:
	case RTM_LOSING: case RTM_REDIRECT: case RTM_MISS: case RTM_LOCK:
	case 0x9: case 0xa:	/* RTM_OLDADD, RTM_OLDDEL: same numbers */
	    {
		const struct nb_rt_msghdr *n = (const void *)in;
		struct rs_rt_msghdr *r = (void *)out;

		if (inlen < (LONG)sizeof(*n))
			return 0;
		if (outlen < (LONG)sizeof(*r))
			return -1;
		memset(r, 0, sizeof(*r));
		r->rtm_version = RS_RTM_VERSION;
		r->rtm_type = n->rtm_type;
		r->rtm_index = n->rtm_index;
		r->rtm_flags = flags_to_amiga(n->rtm_flags);
		r->rtm_pid = n->rtm_pid;
		r->rtm_seq = n->rtm_seq;
		r->rtm_errno = n->rtm_errno;
		r->rtm_use = n->rtm_use;
		r->rtm_inits = n->rtm_inits;
		r->rtm_rmx.rmx_locks = nb_u64_clamp(&n->rtm_rmx.rmx_locks);
		r->rtm_rmx.rmx_mtu = nb_u64_clamp(&n->rtm_rmx.rmx_mtu);
		r->rtm_rmx.rmx_hopcount =
		    nb_u64_clamp(&n->rtm_rmx.rmx_hopcount);
		/* (wall-clock time: rtsock_shared.c:1150-1151) */
		r->rtm_rmx.rmx_expire =
		    sb_amiga_secs(nb_u64_clamp(&n->rtm_rmx.rmx_expire));
		r->rtm_rmx.rmx_recvpipe =
		    nb_u64_clamp(&n->rtm_rmx.rmx_recvpipe);
		r->rtm_rmx.rmx_sendpipe =
		    nb_u64_clamp(&n->rtm_rmx.rmx_sendpipe);
		r->rtm_rmx.rmx_ssthresh =
		    nb_u64_clamp(&n->rtm_rmx.rmx_ssthresh);
		r->rtm_rmx.rmx_rtt = nb_u64_clamp(&n->rtm_rmx.rmx_rtt);
		r->rtm_rmx.rmx_rttvar = nb_u64_clamp(&n->rtm_rmx.rmx_rttvar);
		r->rtm_rmx.rmx_pksent = nb_u64_clamp(&n->rtm_rmx.rmx_pksent);
		addrs = n->rtm_addrs;
		hl = sizeof(*r);
		al = copy_addrs(&addrs, in + sizeof(*n), inlen - sizeof(*n),
		    out + hl, outlen - hl, 0);
		if (al < 0)
			return -1;
		r->rtm_addrs = addrs;
		r->rtm_msglen = hl + al;
		return r->rtm_msglen;
	    }
	case NB_RTM_IFINFO:
	    {
		const struct nb_if_msghdr *n = (const void *)in;
		struct rs_if_msghdr *r = (void *)out;

		if (inlen < (LONG)sizeof(*n))
			return 0;
		if (outlen < (LONG)sizeof(*r))
			return -1;
		memset(r, 0, sizeof(*r));
		r->ifm_version = RS_RTM_VERSION;
		r->ifm_type = RS_RTM_IFINFO;
		r->ifm_flags = n->ifm_flags & ~IFF_0X20;
		r->ifm_index = n->ifm_index;
		if_data_to_amiga(&n->ifm_data, &r->ifm_data);
		addrs = n->ifm_addrs;
		hl = sizeof(*r);
		al = copy_addrs(&addrs, in + sizeof(*n), inlen - sizeof(*n),
		    out + hl, outlen - hl, 0);
		if (al < 0)
			return -1;
		r->ifm_addrs = addrs;
		r->ifm_msglen = hl + al;
		return r->ifm_msglen;
	    }
	case NB_RTM_NEWADDR:
	case NB_RTM_DELADDR:
	    {
		const struct nb_ifa_msghdr *n = (const void *)in;
		struct rs_ifa_msghdr *r = (void *)out;

		if (inlen < (LONG)sizeof(*n))
			return 0;
		if (outlen < (LONG)sizeof(*r))
			return -1;
		memset(r, 0, sizeof(*r));
		r->ifam_version = RS_RTM_VERSION;
		r->ifam_type = in[3] == NB_RTM_NEWADDR ? RS_RTM_NEWADDR :
		    RS_RTM_DELADDR;
		r->ifam_flags = flags_to_amiga(n->ifam_flags);
		r->ifam_index = n->ifam_index;
		r->ifam_metric = n->ifam_metric;
		addrs = n->ifam_addrs;
		hl = sizeof(*r);
		al = copy_addrs(&addrs, in + sizeof(*n), inlen - sizeof(*n),
		    out + hl, outlen - hl, 0);
		if (al < 0)
			return -1;
		r->ifam_addrs = addrs;
		r->ifam_msglen = hl + al;
		return r->ifam_msglen;
	    }
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * the socket calls on a routing socket (server side)
 */

LONG
route_send(struct SocketBase *sb, LONG fd, const UBYTE *buf, LONG len,
    LONG flags)
{
	UBYTE *nb;
	LONG n;

	if (buf == NULL)
		return sb_fail(sb, EFAULT);
	if ((nb = AllocVec(RTMSG_MAX, MEMF_PUBLIC)) == NULL)
		return sb_fail(sb, ENOBUFS);
	n = msg_to_netbsd(buf, len, nb, RTMSG_MAX);
	if (n < 0) {
		FreeVec(nb);
		return sb_fail(sb, -n);
	}
	/* the kernel's verdict is the errno of the send (the doc, -route-:
	   "the values for rtm_errno are" available through errno) */
	if (rump___sysimpl_sendto(fd, nb, n, (flags & ~MSG_DONTWAIT) |
	    NB_MSG_NOSIGNAL, NULL, 0) < 0) {
		n = sb_rumperr();
		FreeVec(nb);
		return sb_fail(sb, n);
	}
	FreeVec(nb);
	return len;
}

LONG
route_recv(struct SocketBase *sb, LONG fd, UBYTE *buf, LONG len,
    LONG flags)
{
	UBYTE *nb, *ab;
	LONG n, m, e, kflags = flags & ~(MSG_DONTWAIT | MSG_WAITALL);

	if (buf == NULL && len > 0)
		return sb_fail(sb, EFAULT);
	if ((nb = AllocVec(2 * RTMSG_MAX, MEMF_PUBLIC)) == NULL)
		return sb_fail(sb, ENOBUFS);
	ab = nb + RTMSG_MAX;
	for (;;) {
		n = rump___sysimpl_recvfrom(fd, nb, RTMSG_MAX, kflags, NULL,
		    NULL);
		if (n >= 0) {
			m = msg_to_amiga(nb, n, ab, RTMSG_MAX);
			if (m == 0) {
				/* not a Roadshow message: gone (also when
				   peeking, else it would block the queue) */
				if (kflags & MSG_PEEK)
					rump___sysimpl_recvfrom(fd, nb,
					    RTMSG_MAX, kflags & ~MSG_PEEK,
					    NULL, NULL);
				continue;
			}
			/* (a Roadshow message is never the longer one) */
			if (m < 0)
				m = RTMSG_MAX;
			/* one record: what does not fit is lost */
			if (m > len)
				m = len;
			if (m > 0)
				CopyMem(ab, buf, m);
			FreeVec(nb);
			return m;
		}
		e = sb_rumperr();
		if (e != EWOULDBLOCK || sb->fds[fd].nonblock ||
		    (flags & MSG_DONTWAIT)) {
			FreeVec(nb);
			return sb_fail(sb, e);
		}
		if ((e = sb_wait_fd(sb, fd, WAIT_READ, sb->fds[fd].has_rcvto ?
		    &sb->fds[fd].rcvto : NULL)) != 0) {
			FreeVec(nb);
			return sb_fail(sb, e);
		}
	}
}

/* ------------------------------------------------------------------------
 * routing table dumps (sysctl net.route.0.<af>.<op>.<arg>,
 * netbsd-src/sys/net/rtsock.c sysctl_rtable())
 */

int
route_sysctl_dump(int af, int op, int arg, UBYTE **bufp, ULONG *lenp)
{
	LONG mib[6];
	ULONG len = 0;
	UBYTE *buf = NULL;
	int tries, e;

	mib[0] = NB_CTL_NET;
	mib[1] = NB_PF_ROUTE;
	mib[2] = 0;
	mib[3] = af;
	mib[4] = op;
	mib[5] = arg;
	for (tries = 0; tries < 5; tries++) {
		len = 0;
		if (nb_sysctl(mib, 6, NULL, &len, NULL, 0) != 0)
			return sb_rumperr();
		if (len == 0) {
			*bufp = NULL;
			*lenp = 0;
			return 0;
		}
		if ((buf = AllocVec(len, MEMF_PUBLIC)) == NULL)
			return ENOMEM;
		if (nb_sysctl(mib, 6, buf, &len, NULL, 0) == 0) {
			*bufp = buf;
			*lenp = len;
			return 0;
		}
		e = sb_rumperr();
		FreeVec(buf);
		if (e != ENOMEM)	/* ENOMEM: it grew meanwhile */
			return e;
	}
	return ENOMEM;
}

/* the IPv4 routes: SBSYSSTAT_DefaultRoute (destination 0 through a
   gateway) and SBSYSSTAT_Routes ("Routes are configured and operational
   ... such routes will by default be assigned to interfaces that have IP
   addresses configured, too", bsdsocket.doc SocketBaseTagList: any route
   that is up, not counting those to the loopback net 127, which lo0
   always has) */
ULONG
route_status(void)
{
	UBYTE *buf, *p;
	ULONG len;
	ULONG found = 0;

	if (route_sysctl_dump(AF_INET, NB_NET_RT_DUMP, 0, &buf, &len) != 0 ||
	    buf == NULL)
		return 0;
	for (p = buf; p + sizeof(struct nb_rt_msghdr) <= buf + len;) {
		const struct nb_rt_msghdr *n = (const void *)p;
		const struct sockaddr_in *dst;

		if (n->rtm_msglen == 0)
			break;
		if (n->rtm_version == NB_RTM_VERSION && n->rtm_type == RTM_GET &&
		    (n->rtm_addrs & RTA_DST) && (n->rtm_flags & RTF_UP)) {
			dst = (const void *)(p + sizeof(*n));
			if (dst->sin_family == AF_INET &&
			    (dst->sin_addr.s_addr >> 24) != 127)
				found |= SBSYSSTAT_Routes;
			if (dst->sin_family == AF_INET &&
			    dst->sin_addr.s_addr == 0 &&
			    (n->rtm_flags & RTF_GATEWAY))
				found |= SBSYSSTAT_DefaultRoute;
		}
		p += n->rtm_msglen;
	}
	FreeVec(buf);
	return found;
}

/* ------------------------------------------------------------------------
 * GetRouteInfo() / FreeRouteInfo()
 */

struct gri_args { LONG af, flags; struct rs_rt_msghdr *result; };

static LONG
srv_getrouteinfo(struct SocketBase *sb, struct gri_args *a)
{
	UBYTE *buf, *p, *out, *o;
	ULONG len, size;
	LONG e, m;

	/* RTF_LLINFO: the ARP entries, which NetBSD keeps apart from the
	   routes (sysctl_rtable(): NET_RT_FLAGS with RTF_LLDATA, for one
	   address family); otherwise all routes (NET_RT_DUMP, which adds
	   the ARP entries too) */
	if (a->flags & RTF_LLINFO)
		e = route_sysctl_dump(AF_INET, NB_NET_RT_FLAGS, RTF_LLINFO,
		    &buf, &len);
	else
		e = route_sysctl_dump(a->af, NB_NET_RT_DUMP, 0, &buf, &len);
	if (e)
		return sb_fail(sb, e);
	/* a Roadshow message is never longer than NetBSD's; then the end
	   marker, an entry with rtm_msglen 0 (the doc, GetRouteInfo:
	   "The table is terminated by" it) */
	size = len + sizeof(struct rs_rt_msghdr);
	if ((out = AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR)) == NULL) {
		if (buf)
			FreeVec(buf);
		return sb_fail(sb, ENOMEM);
	}
	o = out;
	for (p = buf; buf && p + 4 <= buf + len;) {
		UWORD ml = *(const UWORD *)p;

		if (ml == 0 || p + ml > buf + len)
			break;
		m = msg_to_amiga(p, ml, o, out + size -
		    sizeof(struct rs_rt_msghdr) - o);
		p += ml;
		if (m <= 0)
			continue;
		/* "Flags which have to be set in each routing table entry"
		   to be returned */
		if ((((struct rs_rt_msghdr *)o)->rtm_flags & a->flags) !=
		    a->flags)
			continue;
		o += m;
	}
	/* the end marker: a message that was converted but then left out
	   by the flags is still at o; its header becomes the marker */
	memset(o, 0, sizeof(struct rs_rt_msghdr));
	if (buf)
		FreeVec(buf);
	a->result = (struct rs_rt_msghdr *)out;
	return 0;
}

struct rt_msghdr *
sb_GetRouteInfo(struct SocketBase *sb, LONG address_family, LONG flags)
{
	struct gri_args a = { address_family, flags, NULL };

	/* the address family "can be AF_UNSPEC or AF_INET" */
	if (address_family != AF_UNSPEC && address_family != AF_INET) {
		sb_set_errno(sb, EAFNOSUPPORT);
		return NULL;
	}
	if (sb_rpc(sb, (sbfn_t)srv_getrouteinfo, &a, 0, NULL) != 0)
		return NULL;
	return (struct rt_msghdr *)a.result;
}

void
sb_FreeRouteInfo(struct SocketBase *sb, struct rt_msghdr *table)
{

	/* "This parameter can be NULL in which case" nothing is done */
	if (table)
		FreeVec(table);
}

/* ------------------------------------------------------------------------
 * AddRouteTagList() / DeleteRouteTagList()
 */

/* tags (netinclude/libraries/bsdsocket.h) */
#define	RTA_BASE		(TAG_USER + 1600)
#define	RTA_Destination		(RTA_BASE + 1)
#define	RTA_Gateway		(RTA_BASE + 2)
#define	RTA_DefaultGateway	(RTA_BASE + 3)
#define	RTA_DestinationHost	(RTA_BASE + 4)
#define	RTA_DestinationNet	(RTA_BASE + 5)

/* netinclude/netinet/in.h */
#define	IN_CLASSA_NET	0xff000000UL
#define	IN_CLASSA_HOST	0x00ffffffUL
#define	IN_CLASSA_MAX	128
#define	IN_CLASSB_NET	0xffff0000UL
#define	IN_CLASSB_HOST	0x0000ffffUL
#define	IN_CLASSB_MAX	65536
#define	IN_CLASSC_NET	0xffffff00UL
#define	IN_CLASSC_HOST	0x000000ffUL

static ULONG
classmask(ULONG a)
{

	/* the class's netmask, as the kernel picks a default one
	   (netbsd-src/sys/netinet/in.c in_ifinit()); network 0 is all of
	   them, mask 0, as route(8) has it ("if (net == 0) mask = addr =
	   0;", downloads/sources/netbsd-route/route.c:687-697
	   inet_makenetandmask()) */
	if (a == 0)
		return 0;
	if ((a & 0x80000000UL) == 0)
		return IN_CLASSA_NET;
	if ((a & 0xc0000000UL) == 0x80000000UL)
		return IN_CLASSB_NET;
	return IN_CLASSC_NET;
}

/* a network number (host order, inet_network() or getnetbyname()) to an
   address: the number is the network part of its class
   (IN_CLASSA_NSHIFT etc. in netinclude/netinet/in.h) */
static ULONG
net_to_addr(ULONG net)
{

	if (net < IN_CLASSA_MAX)
		return net << 24;
	if (net < IN_CLASSB_MAX)
		return net << 16;
	if (net < 16777216UL)
		return net << 8;
	return net;
}

static ULONG
lnaof(ULONG i)
{

	if ((i & 0x80000000UL) == 0) return i & IN_CLASSA_HOST;
	if ((i & 0xc0000000UL) == 0x80000000UL) return i & IN_CLASSB_HOST;
	return i & IN_CLASSC_HOST;
}

/*
 * A destination.  The doc (AddRouteTagList): a destination is "a host
 * name to be resolved or an IP address in dotted-decimal notation", and
 * it is a network "if the destination has a local address part of
 * INADDR_ANY or if the destination is the symbolic name of a network";
 * RTA_DestinationHost forces a host, RTA_DestinationNet a network.
 * force: 1 host, -1 network, 0 either.  Returns 0, or an errno.
 */
static LONG
get_destination(struct SocketBase *sb, const char *s, int force,
    ULONG *addr, ULONG *mask, int *ishost)
{
	ULONG v;

	if (force < 0) {
		/* a network: a network number, a network name, else the
		   network of a host */
		if ((v = netdb_inet_network(s)) != INADDR_NONE ||
		    (netdb_lookup_net(sb, s, &v) && v != 0))
			v = net_to_addr(v);
		else if (netdb_resolve_addr(sb, s, &v) != 0)
			return sb_interrupted(sb) ? EINTR : EADDRNOTAVAIL;
		*ishost = 0;
		*mask = classmask(v);
		*addr = v & *mask;
		return 0;
	}
	if (netdb_parse_inet_aton(s, &v)) {
		*addr = v;
		*ishost = force > 0 || lnaof(v) != 0;
		*mask = *ishost ? 0xffffffffUL : classmask(v);
		return 0;
	}
	if (force == 0 && netdb_lookup_net(sb, s, &v) && v != 0) {
		*addr = net_to_addr(v);
		*ishost = 0;
		*mask = classmask(*addr);
		return 0;
	}
	if (netdb_resolve_addr(sb, s, addr) == 0) {
		*ishost = 1;
		*mask = 0xffffffffUL;
		return 0;
	}
	return sb_interrupted(sb) ? EINTR : EADDRNOTAVAIL;
}

struct rt_args {
	int cmd;
	struct TagItem *tags;
};

static LONG
srv_route(struct SocketBase *sb, struct rt_args *a)
{
	struct TagItem *tstate = a->tags, *t;
	const char *dst = NULL, *gw = NULL, *defgw = NULL;
	int force = 0, ishost = 0, naddr;
	ULONG daddr = 0, dmask = 0, gaddr = 0;
	LONG s, e, n;
	struct {
		struct nb_rt_msghdr rtm;
		struct sockaddr_in addr[3];
	} m;

	while ((t = sb_next_tag(&tstate)) != NULL) {
		switch (t->ti_Tag) {
		case RTA_Destination:
			dst = (const char *)t->ti_Data;
			force = 0;
			break;
		case RTA_DestinationHost:
			dst = (const char *)t->ti_Data;
			force = 1;
			break;
		case RTA_DestinationNet:
			dst = (const char *)t->ti_Data;
			force = -1;
			break;
		case RTA_Gateway:
			gw = (const char *)t->ti_Data;
			break;
		case RTA_DefaultGateway:
			defgw = (const char *)t->ti_Data;
			break;
		}
	}
	/* "The RTA_DefaultGateway tag excludes" the destination and
	   gateway tags; "RTA_Gateway tag requires the use of the
	   RTA_Destination tag" */
	if (defgw) {
		if (dst || gw)
			return sb_fail(sb, EINVAL);
		gw = defgw;
		daddr = dmask = 0;
		ishost = 0;
	} else {
		if (dst == NULL || (a->cmd == RTM_ADD && gw == NULL))
			return sb_fail(sb, EINVAL);
		if ((e = get_destination(sb, dst, force, &daddr, &dmask,
		    &ishost)) != 0)
			return sb_fail(sb, e);
	}
	if (gw && netdb_resolve_addr(sb, gw, &gaddr) != 0)
		return sb_fail(sb, sb_interrupted(sb) ? EINTR : EADDRNOTAVAIL);

	memset(&m, 0, sizeof(m));
	m.rtm.rtm_version = NB_RTM_VERSION;
	m.rtm.rtm_type = a->cmd;
	m.rtm.rtm_seq = 1;
	m.rtm.rtm_flags = RTF_UP | RTF_STATIC | (gw ? RTF_GATEWAY : 0) |
	    (ishost ? RTF_HOST : 0);
	m.rtm.rtm_addrs = RTA_DST;
	naddr = 0;
	m.addr[naddr++].sin_addr.s_addr = daddr;
	if (gw) {
		m.rtm.rtm_addrs |= RTA_GATEWAY;
		m.addr[naddr++].sin_addr.s_addr = gaddr;
	}
	if (!ishost) {
		m.rtm.rtm_addrs |= RTA_NETMASK;
		m.addr[naddr++].sin_addr.s_addr = dmask;
	}
	for (n = 0; n < naddr; n++) {
		m.addr[n].sin_len = sizeof(m.addr[n]);
		m.addr[n].sin_family = AF_INET;
	}
	m.rtm.rtm_msglen = sizeof(m.rtm) + naddr * sizeof(m.addr[0]);

	if ((s = rump___sysimpl_socket30(NB_PF_ROUTE, SOCK_RAW, AF_INET)) < 0)
		return sb_fail(sb, sb_rumperr());
	n = rump___sysimpl_sendto(s, &m, m.rtm.rtm_msglen, 0, NULL, 0);
	e = n < 0 ? sb_rumperr() : 0;
	rump___sysimpl_close(s);
	if (n < 0)
		return sb_fail(sb, e);
	return 0;
}

LONG
sb_AddRouteTagList(struct SocketBase *sb, struct TagItem *tags)
{
	struct rt_args a = { RTM_ADD, tags };

	return sb_rpc(sb, (sbfn_t)srv_route, &a, RPC_INTERRUPTIBLE, NULL);
}

LONG
sb_DeleteRouteTagList(struct SocketBase *sb, struct TagItem *tags)
{
	struct rt_args a = { RTM_DELETE, tags };

	return sb_rpc(sb, (sbfn_t)srv_route, &a, RPC_INTERRUPTIBLE, NULL);
}
