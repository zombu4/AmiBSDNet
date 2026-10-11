/*
 * bsdsocket.library: kernel settings and statistics through sysctl.
 *
 * GetNetworkStatistics(), ObtainRoadshowData()/ChangeRoadshowData()/
 * ReleaseRoadshowData() and the SocketBaseTagList() options that are
 * kernel settings, as the doc describes them
 * (downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc), with
 * the layouts of the SDK headers (.../netinclude/netinet/ip_var.h and the other *_var.h,
 * sys/mbuf.h, net/route.h, libraries/bsdsocket.h).  The kernel's values
 * come from NetBSD's sysctl tree: the net.inet.* nodes (netbsd-src/sys/
 * netinet/ip_input.c, ip_icmp.c, igmp.c, tcp_usrreq.c, udp_usrreq.c),
 * kern.mbuf.stats (sys/kern/uipc_mbuf.c), net.route (sys/net/rtsock.c).
 * NetBSD's protocol statistics are arrays of 64-bit counters, indexed by
 * the *_STAT_* numbers of netinet/ip_var.h, icmp_var.h, igmp_var.h,
 * tcp_var.h and udp_var.h.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

#include "sblib.h"

int
nb_sysctl(const LONG *mib, ULONG n, void *old, ULONG *oldlen,
    const void *new, ULONG newlen)
{

	return rump___sysimpl___sysctl(mib, n, old, oldlen, new, newlen) < 0 ?
	    -1 : 0;
}

/*
 * The numbers of a sysctl node given by its names, from the top: nodes
 * created with CTL_CREATE have no fixed number.  CTL_QUERY lists a
 * node's children as struct sysctlnode (netbsd-src/sys/kern/kern_sysctl.c
 * sysctl_query(); the query node carries SYSCTL_VERSION).  0, or the
 * error number: ENOENT for a name not there.
 */
int
nb_sysctl_lookup(const char *const *names, LONG *mib, ULONG *n)
{
	struct nb_sysctlnode q, *kids = NULL;
	ULONG depth = 0, len, cap = 0, i, count;
	int e;

	for (; *names; names++) {
		len = cap * sizeof(*kids);
		for (;;) {
			mib[depth] = NB_CTL_QUERY;
			memset(&q, 0, sizeof(q));
			q.sysctl_flags = NB_SYSCTL_VERSION;
			len = cap * sizeof(*kids);
			if (nb_sysctl(mib, depth + 1, kids, &len, &q,
			    sizeof(q)) != 0 && (e = sb_rumperr()) != ENOMEM) {
				if (kids)
					FreeVec(kids);
				return e;
			}
			if (len <= cap * sizeof(*kids))
				break;
			if (kids)
				FreeVec(kids);
			cap = len / sizeof(*kids) + 8;
			if ((kids = AllocVec(cap * sizeof(*kids), MEMF_PUBLIC)) ==
			    NULL)
				return ENOMEM;
		}
		count = len / sizeof(*kids);
		for (i = 0; i < count; i++)
			if (sb_strcmp(kids[i].sysctl_name, *names) == 0)
				break;
		if (i == count) {
			if (kids)
				FreeVec(kids);
			return ENOENT;
		}
		mib[depth++] = kids[i].sysctl_num;
	}
	if (kids)
		FreeVec(kids);
	*n = depth;
	return 0;
}

/* an int-valued node */
static int
int_get(const LONG *mib, ULONG n, LONG *v)
{
	ULONG len = sizeof(*v);

	return nb_sysctl(mib, n, v, &len, NULL, 0);
}

static int
int_set(const LONG *mib, ULONG n, LONG v)
{

	return nb_sysctl(mib, n, NULL, NULL, &v, sizeof(v));
}

/* ------------------------------------------------------------------------
 * SocketBaseTagList() kernel options (codes from the SDK header)
 */

/* net.inet.<proto>.<node> numbers: netbsd-src/sys/netinet/in.h IPCTL_*,
   icmp_var.h ICMPCTL_*, tcp_var.h TCPCTL_*, udp_var.h UDPCTL_* */
static const LONG mib_udp_checksum[] = { NB_CTL_NET, AF_INET, IPPROTO_UDP, 1 };
static const LONG mib_ip_forwarding[] = { NB_CTL_NET, AF_INET, IPPROTO_IP, 1 };
static const LONG mib_ip_redirect[] = { NB_CTL_NET, AF_INET, IPPROTO_IP, 2 };
static const LONG mib_ip_ttl[] = { NB_CTL_NET, AF_INET, IPPROTO_IP, 3 };
static const LONG mib_icmp_maskrepl[] = { NB_CTL_NET, AF_INET, IPPROTO_ICMP,
    1 };

static const LONG *
tag_mib(int code)
{

	switch (code) {
	case 42: return mib_udp_checksum;	/* SBTC_UDP_CHECKSUM */
	case 43: return mib_ip_forwarding;	/* SBTC_IP_FORWARDING */
	case 44: return mib_ip_ttl;		/* SBTC_IP_DEFAULT_TTL */
	case 45: return mib_icmp_maskrepl;	/* SBTC_ICMP_MASK_REPLY */
	case 46: return mib_ip_redirect;	/* SBTC_ICMP_SEND_REDIRECTS */
	}
	return NULL;
}

int
nb_tag_get(int code, ULONG *v)
{
	const LONG *mib = tag_mib(code);
	LONG val;

	if (mib == NULL || int_get(mib, 4, &val) != 0)
		return -1;
	*v = (ULONG)val;
	return 0;
}

int
nb_tag_set(int code, ULONG v)
{
	const LONG *mib = tag_mib(code);

	if (mib == NULL)
		return -1;
	return int_set(mib, 4, (LONG)v);
}

/* ------------------------------------------------------------------------
 * interface byte counters (SBTC_GET_BYTES_RECEIVED/SENT): the RTM_IFINFO
 * messages of sysctl net.route.0.0.NET_RT_IFLIST carry every interface's
 * struct if_data (netbsd-src/sys/net/rtsock.c sysctl_iflist())
 */

#define	NB_NET_RT_IFLIST	6

static void
add64(ULONG *hi, ULONG *lo, const struct nb_u64 *v)
{
	ULONG l = *lo + v->lo;

	*hi += v->hi + (l < *lo);
	*lo = l;
}

int
nb_bytes_total(ULONG *in_hi, ULONG *in_lo, ULONG *out_hi, ULONG *out_lo)
{
	UBYTE *buf, *p;
	ULONG len;
	int e;

	if ((e = route_sysctl_dump(0, NB_NET_RT_IFLIST, 0, &buf, &len)) != 0)
		return e;
	*in_hi = *in_lo = *out_hi = *out_lo = 0;
	for (p = buf; buf && p + 4 <= buf + len;) {
		const struct nb_if_msghdr *m = (const void *)p;

		if (m->ifm_msglen == 0)
			break;
		if (m->ifm_type == NB_RTM_IFINFO &&
		    p + sizeof(*m) <= buf + len) {
			add64(in_hi, in_lo, &m->ifm_data.ifi_ibytes);
			add64(out_hi, out_lo, &m->ifm_data.ifi_obytes);
		}
		p += m->ifm_msglen;
	}
	if (buf)
		FreeVec(buf);
	return 0;
}

/* ------------------------------------------------------------------------
 * GetNetworkStatistics()
 */

/* the types and version (the SDK header) */
#define	NETWORKSTATUS_VERSION	1
#define	NETSTATUS_icmp		0
#define	NETSTATUS_igmp		1
#define	NETSTATUS_ip		2
#define	NETSTATUS_mb		3
#define	NETSTATUS_mrt		4
#define	NETSTATUS_rt		5
#define	NETSTATUS_tcp		6
#define	NETSTATUS_udp		7
#define	NETSTATUS_tcp_sockets	9
#define	NETSTATUS_udp_sockets	10

/* netinclude/netinet/ip_icmp.h */
#define	RS_ICMP_MAXTYPE		18

struct rs_icmpstat {
	ULONG icps_error, icps_oldshort, icps_oldicmp;
	ULONG icps_outhist[RS_ICMP_MAXTYPE + 1];
	ULONG icps_badcode, icps_tooshort, icps_checksum, icps_badlen,
	    icps_reflect;
	ULONG icps_inhist[RS_ICMP_MAXTYPE + 1];
};
struct rs_mbstat {
	ULONG m_mbufs, m_clusters, m_spare, m_clfree, m_drops, m_wait,
	    m_drain;
};
struct rs_rtstat {
	WORD rts_badredirect, rts_dynamic, rts_newgateway, rts_unreach,
	    rts_wildcard;
};
struct rs_pcd {
	struct in_addr pcd_foreign_address;
	UWORD pcd_foreign_port;
	struct in_addr pcd_local_address;
	UWORD pcd_local_port;
	ULONG pcd_receive_queue_size;
	ULONG pcd_send_queue_size;
	LONG pcd_tcp_state;
};

/* field order of each Roadshow structure, as NetBSD *_STAT_* indices */
/* struct ipstat: IP_STAT_TOTAL..BADVERS, then RAWOUT (24) */
static const UBYTE ip_map[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
/* struct igmpstat: IGMP_STAT_RCV_TOTAL (1) .. IGMP_STAT_SND_REPORTS (9) */
static const UBYTE igmp_map[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
/* struct udpstat: UDP_STAT_IPACKETS..PCBHASHMISS, OPACKETS */
static const UBYTE udp_map[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
/* struct tcpstat (netinclude/netinet/tcp_var.h) in TCP_STAT_* numbers;
   tcps_pcbcachemiss is TCP_STAT_PCBHASHMISS, tcps_persistdrop
   TCP_STAT_PERSISTDROPS */
static const UBYTE tcp_map[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
	18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
	28, 29, 30, 31, 32, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
	46, 47, 48, 49, 50, 51, 52, 53, 15, 55,
};
#define	NB_IP_NSTATS	41
#define	NB_IGMP_NSTATS	10
#define	NB_UDP_NSTATS	9
#define	NB_TCP_NSTATS	76
#define	NB_ICMP_STAT_LAST 16	/* netinet/icmp_var.h */
#define	NB_ICMP_NTYPES	41	/* ICMP_MAXTYPE 40 + 1, netinet/ip_icmp.h */
#define	NB_ICMP_NSTATS	(NB_ICMP_STAT_LAST + 2 * NB_ICMP_NTYPES)

/* a counter array from the kernel, with the counters' count, into *vp;
   0 or the error number */
static int
stat_array(const LONG *mib, ULONG n, ULONG count, struct nb_u64 **vp)
{
	struct nb_u64 *v;
	ULONG len = count * sizeof(*v);
	int e;

	if ((v = AllocVec(len, MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return ENOMEM;
	if (nb_sysctl(mib, n, v, &len, NULL, 0) != 0) {
		e = sb_rumperr();
		FreeVec(v);
		return e;
	}
	*vp = v;
	return 0;
}

static void
map_counters(ULONG *dst, const struct nb_u64 *src, const UBYTE *map,
    int n)
{
	int i;

	for (i = 0; i < n; i++)
		dst[i] = nb_u64_clamp(&src[map[i]]);
}

/* the sockets of one protocol (net.inet.<proto>.pcblist, a CTL_CREATE
   node; its arguments: two unused levels, the element size and the
   number of elements, netinet/tcp_usrreq.c sysctl_inpcblist()) */
static UBYTE *
pcb_list(const char *proto, int tcp, ULONG *size, int *ep)
{
	static const char *const tcpnames[] = { "net", "inet", "tcp",
	    "pcblist", NULL };
	static const char *const udpnames[] = { "net", "inet", "udp",
	    "pcblist", NULL };
	LONG mib[8];
	ULONG n, len, count, i;
	struct nb_kinfo_pcb *kp;
	struct rs_pcd *out;

	if ((*ep = nb_sysctl_lookup(tcp ? tcpnames : udpnames, mib, &n)) != 0)
		return NULL;
	mib[n] = 0;
	mib[n + 1] = 0;
	mib[n + 2] = sizeof(struct nb_kinfo_pcb);
	mib[n + 3] = 0x7fffffff;
	len = 0;
	if (nb_sysctl(mib, n + 4, NULL, &len, NULL, 0) != 0) {
		*ep = sb_rumperr();
		return NULL;
	}
	*ep = ENOMEM;
	if ((kp = AllocVec(len ? len : 1, MEMF_PUBLIC)) == NULL)
		return NULL;
	if (nb_sysctl(mib, n + 4, kp, &len, NULL, 0) != 0) {
		*ep = sb_rumperr();
		FreeVec(kp);
		return NULL;
	}
	count = len / sizeof(*kp);
	if ((out = AllocVec((count ? count : 1) * sizeof(*out),
	    MEMF_PUBLIC | MEMF_CLEAR)) == NULL) {
		FreeVec(kp);
		return NULL;
	}
	for (n = i = 0; i < count; i++) {
		const struct sockaddr_in *ls = (const void *)kp[i].ki_s;
		const struct sockaddr_in *fs = (const void *)kp[i].ki_d;

		if (kp[i].ki_family != AF_INET)
			continue;
		out[n].pcd_foreign_address = fs->sin_addr;
		out[n].pcd_foreign_port = fs->sin_port;
		out[n].pcd_local_address = ls->sin_addr;
		out[n].pcd_local_port = ls->sin_port;
		out[n].pcd_receive_queue_size = nb_u64_clamp(&kp[i].ki_rcvq);
		out[n].pcd_send_queue_size = nb_u64_clamp(&kp[i].ki_sndq);
		/* "'pcd_tcp_state' member will be -1" for UDP; the TCPS_
		   values are the same in netinclude/netinet/tcp_fsm.h and
		   netbsd-src/sys/netinet/tcp_fsm.h */
		out[n].pcd_tcp_state = tcp ? kp[i].ki_tstate : -1;
		n++;
	}
	FreeVec(kp);
	*size = n * sizeof(*out);
	return (UBYTE *)out;
}

struct gns_args { LONG type; APTR dest; LONG size; };

static LONG
srv_netstats(struct SocketBase *sb, struct gns_args *a)
{
	static const LONG mib_ip[] = { NB_CTL_NET, AF_INET, IPPROTO_IP, 24 };
	static const LONG mib_icmp[] = { NB_CTL_NET, AF_INET, IPPROTO_ICMP, 7 };
	static const LONG mib_tcp[] = { NB_CTL_NET, AF_INET, IPPROTO_TCP, 30 };
	static const LONG mib_udp[] = { NB_CTL_NET, AF_INET, IPPROTO_UDP, 5 };
	static const LONG mib_mb[] = { NB_CTL_KERN, NB_KERN_MBUF,
	    NB_MBUF_STATS };
	static const char *const igmpnames[] = { "net", "inet", "igmp",
	    "stats", NULL };
	/* net.rtable.stats: the PF_ROUTE node is named "rtable"
	   (netbsd-src/sys/net/rtsock_shared.c:1736,
	   sysctl_net_route_setup(NULL, PF_ROUTE, "rtable"); its "stats"
	   child, rtsock.c:519-524) */
	static const char *const rtnames[] = { "net", "rtable", "stats",
	    NULL };
	union {
		ULONG ip[24];
		struct rs_icmpstat icmp;
		ULONG igmp[9];
		struct rs_mbstat mb;
		struct rs_rtstat rt;
		ULONG tcp[sizeof(tcp_map)];
		ULONG udp[9];
	} u;
	UBYTE *data = (UBYTE *)&u, *list = NULL;
	ULONG len = 0, n;
	struct nb_u64 *v;
	LONG mib[8], rv;
	int i, e;

	memset(&u, 0, sizeof(u));
	switch (a->type) {
	case NETSTATUS_ip:
		if ((e = stat_array(mib_ip, 4, NB_IP_NSTATS, &v)) != 0)
			return sb_fail(sb, e);
		map_counters(u.ip, v, ip_map, sizeof(ip_map));
		len = sizeof(u.ip);
		FreeVec(v);
		break;
	case NETSTATUS_icmp:
		if ((e = stat_array(mib_icmp, 4, NB_ICMP_NSTATS, &v)) != 0)
			return sb_fail(sb, e);
		/* ICMP_STAT_ERROR..REFLECT are 0..7, the histograms start
		   at ICMP_STAT_OUTHIST / ICMP_STAT_INHIST */
		u.icmp.icps_error = nb_u64_clamp(&v[0]);
		u.icmp.icps_oldshort = nb_u64_clamp(&v[1]);
		u.icmp.icps_oldicmp = nb_u64_clamp(&v[2]);
		u.icmp.icps_badcode = nb_u64_clamp(&v[3]);
		u.icmp.icps_tooshort = nb_u64_clamp(&v[4]);
		u.icmp.icps_checksum = nb_u64_clamp(&v[5]);
		u.icmp.icps_badlen = nb_u64_clamp(&v[6]);
		u.icmp.icps_reflect = nb_u64_clamp(&v[7]);
		for (i = 0; i <= RS_ICMP_MAXTYPE; i++) {
			u.icmp.icps_outhist[i] =
			    nb_u64_clamp(&v[NB_ICMP_STAT_LAST + i]);
			u.icmp.icps_inhist[i] = nb_u64_clamp(
			    &v[NB_ICMP_STAT_LAST + NB_ICMP_NTYPES + i]);
		}
		len = sizeof(u.icmp);
		FreeVec(v);
		break;
	case NETSTATUS_igmp:
		if ((e = nb_sysctl_lookup(igmpnames, mib, &n)) != 0 ||
		    (e = stat_array(mib, n, NB_IGMP_NSTATS, &v)) != 0)
			return sb_fail(sb, e);
		map_counters(u.igmp, v, igmp_map, sizeof(igmp_map));
		len = sizeof(u.igmp);
		FreeVec(v);
		break;
	case NETSTATUS_tcp:
		if ((e = stat_array(mib_tcp, 4, NB_TCP_NSTATS, &v)) != 0)
			return sb_fail(sb, e);
		map_counters(u.tcp, v, tcp_map, sizeof(tcp_map));
		len = sizeof(u.tcp);
		FreeVec(v);
		break;
	case NETSTATUS_udp:
		if ((e = stat_array(mib_udp, 4, NB_UDP_NSTATS, &v)) != 0)
			return sb_fail(sb, e);
		map_counters(u.udp, v, udp_map, sizeof(udp_map));
		len = sizeof(u.udp);
		FreeVec(v);
		break;
	case NETSTATUS_mb:
	    {
		/* struct mbstat (netbsd-src/sys/sys/mbuf.h): seven u_long,
		   the first four kept as spares (formerly m_mbufs,
		   m_clusters, a spare, m_clfree), then the mbuf types */
		ULONG k[7 + 256 / 2];
		ULONG kl = sizeof(k);

		if (nb_sysctl(mib_mb, 3, k, &kl, NULL, 0) != 0)
			return sb_fail(sb, sb_rumperr());
		u.mb.m_mbufs = k[0];
		u.mb.m_clusters = k[1];
		u.mb.m_spare = k[2];
		u.mb.m_clfree = k[3];
		u.mb.m_drops = k[4];
		u.mb.m_wait = k[5];
		u.mb.m_drain = k[6];
		len = sizeof(u.mb);
		break;
	    }
	case NETSTATUS_rt:
	    {
		/* struct rtstat (netbsd-src/sys/net/route.h): five uint64_t */
		struct nb_u64 r[5];
		ULONG rl = sizeof(r);
		WORD *w = &u.rt.rts_badredirect;

		if ((e = nb_sysctl_lookup(rtnames, mib, &n)) != 0)
			return sb_fail(sb, e);
		if (nb_sysctl(mib, n, r, &rl, NULL, 0) != 0)
			return sb_fail(sb, sb_rumperr());
		for (i = 0; i < 5; i++)
			w[i] = r[i].hi || r[i].lo > 0x7fff ? 0x7fff : r[i].lo;
		len = sizeof(u.rt);
		break;
	    }
	case NETSTATUS_tcp_sockets:
	case NETSTATUS_udp_sockets:
		if ((list = pcb_list(NULL, a->type == NETSTATUS_tcp_sockets,
		    &len, &e)) == NULL)
			return sb_fail(sb, e);
		data = list;
		break;
	default:
		/* NETSTATUS_mrt among them: NetBSD's IPv4 multicast routing
		   (netinet/ip_mroute.c) is not in the kernel this stack
		   builds (tools/build.py), so there are no such counters */
		return sb_fail(sb, EINVAL);
	}
	/* "Pass a NULL pointer to find out how much memory would be
	   required"; otherwise copy "size" bytes at most */
	if (a->dest == NULL)
		rv = len;
	else {
		rv = (ULONG)a->size < len ? a->size : (LONG)len;
		if (rv > 0)
			CopyMem(data, a->dest, rv);
	}
	if (list)
		FreeVec(list);
	return rv;
}

LONG
sb_GetNetworkStatistics(struct SocketBase *sb, LONG type, LONG version,
    APTR destination, LONG size)
{
	struct gns_args a = { type, destination, size };

	/* "The version described in this documentation is 1" */
	if (version != NETWORKSTATUS_VERSION || size < 0) {
		sb_set_errno(sb, EINVAL);
		return -1;
	}
	return sb_rpc(sb, (sbfn_t)srv_netstats, &a, 0, NULL);
}

/* ------------------------------------------------------------------------
 * ObtainRoadshowData() / ChangeRoadshowData() / ReleaseRoadshowData()
 */

/* struct RoadshowDataNode, RDNT_Integer, RDNF_ReadOnly, ORD_* (the SDK
   header) */
struct RoadshowDataNode {
	struct MinNode rdn_MinNode;
	STRPTR rdn_Name;
	UWORD rdn_Flags;
	WORD rdn_Type;
	ULONG rdn_Length;
	APTR rdn_Data;
};
#define	RDNT_Integer	0
#define	RDNF_ReadOnly	(1 << 0)
#define	ORD_ReadAccess	0
#define	ORD_WriteAccess	1

/*
 * The options of the doc's list (ChangeRoadshowData OPTIONS) that NetBSD
 * has a setting for: the sysctl nodes of netbsd-src/sys/netinet
 * (ip_input.c, ip_icmp.c, tcp_usrreq.c, udp_usrreq.c).  Options without
 * a counterpart in the kernel are not in the list.
 */
static const struct rdopt {
	const char *name;
	LONG mib[4];
} rdopts[] = {
	{ "icmp.maskrepl",	{ NB_CTL_NET, AF_INET, IPPROTO_ICMP, 1 } },
	{ "ip.defttl",		{ NB_CTL_NET, AF_INET, IPPROTO_IP, 3 } },
	{ "ip.forwarding",	{ NB_CTL_NET, AF_INET, IPPROTO_IP, 1 } },
	{ "ip.sendredirects",	{ NB_CTL_NET, AF_INET, IPPROTO_IP, 2 } },
	/* netinet/in.c: IPCTL_SUBNETSARELOCAL */
	{ "ip.subnetsarelocal",	{ NB_CTL_NET, AF_INET, IPPROTO_IP, 8 } },
	{ "tcp.do_rfc1323",	{ NB_CTL_NET, AF_INET, IPPROTO_TCP, 1 } },
	{ "tcp.do_timestamps",	{ NB_CTL_NET, AF_INET, IPPROTO_TCP, 12 } },
	{ "tcp.do_win_scale",	{ NB_CTL_NET, AF_INET, IPPROTO_TCP, 11 } },
	{ "tcp.mssdflt",	{ NB_CTL_NET, AF_INET, IPPROTO_TCP, 4 } },
	{ "tcp.recvspace",	{ NB_CTL_NET, AF_INET, IPPROTO_TCP, 3 } },
	{ "tcp.sendspace",	{ NB_CTL_NET, AF_INET, IPPROTO_TCP, 2 } },
	{ "udp.cksum",		{ NB_CTL_NET, AF_INET, IPPROTO_UDP, 1 } },
	{ "udp.recvspace",	{ NB_CTL_NET, AF_INET, IPPROTO_UDP, 3 } },
	{ "udp.sendspace",	{ NB_CTL_NET, AF_INET, IPPROTO_UDP, 2 } },
};
#define	NRDOPTS	(sizeof(rdopts) / sizeof(rdopts[0]))

/* what ObtainRoadshowData() hands out, the list first */
struct rdlist {
	struct List list;
	ULONG magic;
	LONG access;
	struct RoadshowDataNode node[NRDOPTS];
	LONG value[NRDOPTS];
	const struct rdopt *opt[NRDOPTS];
};
#define	RDLIST_MAGIC	0x52444c53UL

/* "Only one caller can modify the options at a time" */
static struct SignalSemaphore rdwrite;

void
stats_init(void)
{

	InitSemaphore(&rdwrite);
}

struct ord_args { struct rdlist *l; };

static LONG
srv_obtain_rd(struct SocketBase *sb, struct ord_args *a)
{
	struct rdlist *l = a->l;
	ULONG i, n = 0;

	sb_newlist(&l->list);
	for (i = 0; i < NRDOPTS; i++) {
		struct RoadshowDataNode *d = &l->node[n];

		if (int_get(rdopts[i].mib, 4, &l->value[n]) != 0)
			continue;	/* not in this kernel */
		d->rdn_Name = (STRPTR)rdopts[i].name;
		d->rdn_Flags = 0;
		d->rdn_Type = RDNT_Integer;
		d->rdn_Length = sizeof(LONG);
		d->rdn_Data = &l->value[n];
		l->opt[n] = &rdopts[i];
		AddTail(&l->list, (struct Node *)&d->rdn_MinNode);
		n++;
	}
	return 0;
}

struct List *
sb_ObtainRoadshowData(struct SocketBase *sb, LONG access)
{
	struct ord_args a;

	if (access != ORD_ReadAccess && access != ORD_WriteAccess) {
		sb_set_errno(sb, EINVAL);
		return NULL;
	}
	if ((a.l = AllocVec(sizeof(*a.l), MEMF_PUBLIC | MEMF_CLEAR)) == NULL) {
		sb_set_errno(sb, ENOMEM);
		return NULL;
	}
	a.l->magic = RDLIST_MAGIC;
	a.l->access = access;
	if (access == ORD_WriteAccess)
		ObtainSemaphore(&rdwrite);
	if (sb_rpc(sb, (sbfn_t)srv_obtain_rd, &a, 0, NULL) != 0) {
		if (access == ORD_WriteAccess)
			ReleaseSemaphore(&rdwrite);
		FreeVec(a.l);
		return NULL;
	}
	return &a.l->list;
}

void
sb_ReleaseRoadshowData(struct SocketBase *sb, struct List *list)
{
	struct rdlist *l = (struct rdlist *)list;

	/* "If this is NULL, it will be ignored" */
	if (l == NULL || l->magic != RDLIST_MAGIC)
		return;
	l->magic = 0;
	if (l->access == ORD_WriteAccess)
		ReleaseSemaphore(&rdwrite);
	FreeVec(l);
}

struct crd_args { const struct rdopt *opt; LONG value; };

static LONG
srv_change_rd(struct SocketBase *sb, struct crd_args *a)
{

	if (int_set(a->opt->mib, 4, a->value) != 0)
		return sb_fail(sb, sb_rumperr());
	return 0;
}

BOOL
sb_ChangeRoadshowData(struct SocketBase *sb, struct List *list,
    STRPTR name, ULONG length, APTR data)
{
	struct rdlist *l = (struct rdlist *)list;
	struct RoadshowDataNode *d;
	struct crd_args a;
	int i;

	/* the doc's errors: EINVAL (list), EACCES (no write access),
	   ENOENT (name; "Option names are not case-sensitive"), ENOSPC
	   (size), EPERM (read-only) */
	if (l == NULL || l->magic != RDLIST_MAGIC || data == NULL) {
		sb_set_errno(sb, EINVAL);
		return FALSE;
	}
	if (l->access != ORD_WriteAccess) {
		sb_set_errno(sb, EACCES);
		return FALSE;
	}
	for (d = (struct RoadshowDataNode *)l->list.lh_Head, i = 0;
	    d->rdn_MinNode.mln_Succ;
	    d = (struct RoadshowDataNode *)d->rdn_MinNode.mln_Succ, i++)
		if (name && sb_strcasecmp((const char *)d->rdn_Name,
		    (const char *)name) == 0)
			break;
	if (d->rdn_MinNode.mln_Succ == NULL) {
		sb_set_errno(sb, ENOENT);
		return FALSE;
	}
	if (d->rdn_Flags & RDNF_ReadOnly) {
		sb_set_errno(sb, EPERM);
		return FALSE;
	}
	if (length != d->rdn_Length) {
		sb_set_errno(sb, ENOSPC);
		return FALSE;
	}
	a.opt = l->opt[i];
	a.value = *(LONG *)data;
	if (sb_rpc(sb, (sbfn_t)srv_change_rd, &a, 0, NULL) != 0)
		return FALSE;
	*(LONG *)d->rdn_Data = a.value;
	return TRUE;
}
