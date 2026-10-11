/*
 * bsdsocket.library: interface ioctls, the interface API and address
 * configuration.
 *
 * Interface ioctls: Roadshow's struct ifreq is 32 bytes, its ioctl
 * numbers carry that size (downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/
 * netinclude/net/if.h, sys/sockio.h, sys/ioccom.h); NetBSD's is 144 bytes
 * (netbsd-src/sys/net/if.h, sys/sys/sockio.h), so the same request has
 * another number and layout.  The requests of netinclude/sys/sockio.h
 * that take a struct ifreq or struct ifconf are translated here; the
 * others have the same number and layout on both sides.
 *
 * The interface API (AddInterfaceTagList() etc.) follows the doc,
 * downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc, and
 * works through the stack's own interface table and functions
 * (src/stack/stack.h: ifaces[], dhcp_configure(), stack_update_route(),
 * stack_update_dns(), the rump_amibsdnet_*() kernel helpers) and the
 * SANA-II layer's (src/host/sana2_host.h).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "sblib.h"
#include "stack.h"
#include "sana2_host.h"

/* ------------------------------------------------------------------------
 * small kernel helpers (server side)
 */

static LONG
inet_sock(void)
{

	return rump___sysimpl_socket30(AF_INET, SOCK_DGRAM, 0);
}

static int
k_ioctl_name(LONG s, ULONG req, const char *name, struct nb_ifreq *ifr)
{

	memset(ifr, 0, sizeof(*ifr));
	sb_strlcpy(ifr->ifr_name, name, IFNAMSIZ);
	return rump___sysimpl_ioctl(s, req, ifr) < 0 ? -1 : 0;
}

/* the kernel's interface list (struct ifreq entries of 144 bytes); 0 or
   the error number */
static int
k_ifconf(LONG s, struct nb_ifreq **bufp, LONG *np)
{
	struct nb_ifconf ifc;
	struct nb_ifreq *buf;

	ifc.ifc_len = 0;
	ifc.ifc_buf = NULL;
	if (rump___sysimpl_ioctl(s, NB_SIOCGIFCONF, &ifc) < 0)
		return sb_rumperr();
	/* (room for one more, in case an address comes meanwhile) */
	ifc.ifc_len += sizeof(*buf);
	if ((buf = AllocVec(ifc.ifc_len, MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return ENOMEM;
	ifc.ifc_buf = buf;
	if (rump___sysimpl_ioctl(s, NB_SIOCGIFCONF, &ifc) < 0) {
		LONG e = sb_rumperr();

		FreeVec(buf);
		return e;
	}
	*bufp = buf;
	*np = ifc.ifc_len / sizeof(*buf);
	return 0;
}

/* ------------------------------------------------------------------------
 * interface ioctls
 */

/* netinclude/sys/ioccom.h _IOC() */
#define	RS_IOC(dir, num, len)	((dir) | ((ULONG)(len) << 16) | ('i' << 8) | (num))
#define	RS_IN		0x80000000UL
#define	RS_INOUT	0xc0000000UL
#define	RS_IFREQ	sizeof(struct ifreq)
#define	RS_IFCONF	sizeof(struct ifconf)

/* the ifreq requests of netinclude/sys/sockio.h and NetBSD's numbers for
   them; old: the request returns a 4.3BSD "osockaddr" (the family as a
   16-bit word where sa_len and sa_family are), as netbsd-src/sys/compat/
   common/if_43.c compat_ifioctl() does for the same requests */
static const struct ifmap {
	ULONG rs, nb;
	UBYTE set, old, flags;
} ifmap[] = {
	{ RS_IOC(RS_IN, 12, RS_IFREQ), NB_SIOCSIFADDR, 1, 0, 0 },
	{ RS_IOC(RS_INOUT, 13, RS_IFREQ), NB_SIOCGIFADDR, 0, 1, 0 },
	{ RS_IOC(RS_INOUT, 33, RS_IFREQ), NB_SIOCGIFADDR, 0, 0, 0 },
	{ RS_IOC(RS_IN, 14, RS_IFREQ), NB_SIOCSIFDSTADDR, 1, 0, 0 },
	{ RS_IOC(RS_INOUT, 15, RS_IFREQ), NB_SIOCGIFDSTADDR, 0, 1, 0 },
	{ RS_IOC(RS_INOUT, 34, RS_IFREQ), NB_SIOCGIFDSTADDR, 0, 0, 0 },
	{ RS_IOC(RS_IN, 16, RS_IFREQ), NB_SIOCSIFFLAGS, 1, 0, 1 },
	{ RS_IOC(RS_INOUT, 17, RS_IFREQ), NB_SIOCGIFFLAGS, 0, 0, 1 },
	{ RS_IOC(RS_INOUT, 18, RS_IFREQ), NB_SIOCGIFBRDADDR, 0, 1, 0 },
	{ RS_IOC(RS_INOUT, 35, RS_IFREQ), NB_SIOCGIFBRDADDR, 0, 0, 0 },
	{ RS_IOC(RS_IN, 19, RS_IFREQ), NB_SIOCSIFBRDADDR, 1, 0, 0 },
	{ RS_IOC(RS_INOUT, 21, RS_IFREQ), NB_SIOCGIFNETMASK, 0, 1, 0 },
	{ RS_IOC(RS_INOUT, 37, RS_IFREQ), NB_SIOCGIFNETMASK, 0, 0, 0 },
	{ RS_IOC(RS_IN, 22, RS_IFREQ), NB_SIOCSIFNETMASK, 1, 0, 0 },
	{ RS_IOC(RS_INOUT, 23, RS_IFREQ), NB_SIOCGIFMETRIC, 0, 0, 2 },
	{ RS_IOC(RS_IN, 24, RS_IFREQ), NB_SIOCSIFMETRIC, 1, 0, 2 },
	{ RS_IOC(RS_IN, 25, RS_IFREQ), NB_SIOCDIFADDR, 1, 0, 0 },
	{ RS_IOC(RS_IN, 49, RS_IFREQ), NB_SIOCADDMULTI, 1, 0, 0 },
	{ RS_IOC(RS_IN, 50, RS_IFREQ), NB_SIOCDELMULTI, 1, 0, 0 },
};
#define	NIFMAP	(sizeof(ifmap) / sizeof(ifmap[0]))
#define	RS_OSIOCGIFCONF	RS_IOC(RS_INOUT, 20, RS_IFCONF)
#define	RS_SIOCGIFCONF	RS_IOC(RS_INOUT, 36, RS_IFCONF)

/*
 * SIOCGIFCONF: entries of the interface name and an address; an address
 * longer than a struct sockaddr makes its entry longer, as Roadshow's
 * libpcap steps through them (source_code/libpcap-0.8.1/fad-gifc.c, with
 * HAVE_SOCKADDR_SA_LEN in its config.h); OSIOCGIFCONF entries are all
 * one struct ifreq with an osockaddr.  "The ifc_len field should be
 * initially set to the size of the buffer ... On return it will contain
 * the length, in bytes, of the configuration list" (the doc,
 * -networking-).
 */
static LONG
rs_ifconf(struct SocketBase *sb, LONG fd, struct ifconf *ifc, int old)
{
	struct nb_ifreq *k;
	LONG n, i, used = 0, e;
	UBYTE *out = ifc->ifc_buf;

	if (ifc->ifc_len < 0 || (out == NULL && ifc->ifc_len > 0))
		return sb_fail(sb, EINVAL);
	if ((e = k_ifconf(fd, &k, &n)) != 0)
		return sb_fail(sb, e);
	for (i = 0; i < n; i++) {
		const struct sockaddr *sa = &k[i].ifr_ifru.ifru_addr;
		LONG al = sa->sa_len, el;

		el = (old || al <= (LONG)sizeof(struct sockaddr)) ?
		    (LONG)sizeof(struct ifreq) : IFNAMSIZ + al;
		if (used + el > ifc->ifc_len)
			break;
		memset(out + used, 0, el);
		CopyMem(k[i].ifr_name, out + used, IFNAMSIZ);
		if (old) {
			CopyMem((APTR)sa, out + used + IFNAMSIZ,
			    al < (LONG)sizeof(struct sockaddr) ? al :
			    sizeof(struct sockaddr));
			*(UWORD *)(out + used + IFNAMSIZ) = sa->sa_family;
		} else if (al > 0)
			CopyMem((APTR)sa, out + used + IFNAMSIZ, al);
		used += el;
	}
	FreeVec(k);
	ifc->ifc_len = used;
	return 0;
}

LONG
sb_ifioctl(struct SocketBase *sb, LONG fd, ULONG req, APTR argp,
    int *handled)
{
	const struct ifmap *m;
	struct ifreq *rr = argp;
	struct nb_ifreq nr;
	ULONG i;

	*handled = 0;
	if (req == RS_SIOCGIFCONF || req == RS_OSIOCGIFCONF) {
		*handled = 1;
		if (argp == NULL)
			return sb_fail(sb, EFAULT);
		return rs_ifconf(sb, fd, argp, req == RS_OSIOCGIFCONF);
	}
	for (i = 0; i < NIFMAP && ifmap[i].rs != req; i++)
		;
	if (i == NIFMAP)
		return 0;
	m = &ifmap[i];
	*handled = 1;
	if (rr == NULL)
		return sb_fail(sb, EFAULT);
	memset(&nr, 0, sizeof(nr));
	CopyMem(rr->ifr_name, nr.ifr_name, IFNAMSIZ);
	if (m->set) {
		if (m->flags == 1) {
			struct nb_ifreq cur;

			/* 0x20 means different things (IFF_NOTRAILERS in
			   netinclude/net/if.h, IFF_UNNUMBERED in NetBSD):
			   the kernel's own bit is kept */
			if (k_ioctl_name(fd, NB_SIOCGIFFLAGS, rr->ifr_name,
			    &cur) != 0)
				return sb_fail(sb, sb_rumperr());
			nr.ifr_ifru.ifru_flags = (rr->ifr_ifru.ifru_flags &
			    ~IFF_0X20) | (cur.ifr_ifru.ifru_flags & IFF_0X20);
		} else if (m->flags == 2)
			nr.ifr_ifru.ifru_metric = rr->ifr_ifru.ifru_metric;
		else {
			struct sockaddr *sa = &nr.ifr_ifru.ifru_addr;

			CopyMem(&rr->ifr_ifru.ifru_addr, sa, sizeof(*sa));
			/* compat_ifioctl() (if_43.c) for the 32-byte ifreq
			   on a big-endian machine: sa_len 0 is the size of
			   the field */
			if (sa->sa_len == 0)
				sa->sa_len = sizeof(*sa);
		}
	}
	if (rump___sysimpl_ioctl(fd, m->nb, &nr) < 0)
		return sb_fail(sb, sb_rumperr());
	if (!m->set) {
		if (m->flags == 1)
			rr->ifr_ifru.ifru_flags = nr.ifr_ifru.ifru_flags &
			    ~IFF_0X20;
		else if (m->flags == 2)
			rr->ifr_ifru.ifru_metric = nr.ifr_ifru.ifru_metric;
		else {
			CopyMem(&nr.ifr_ifru.ifru_addr, &rr->ifr_ifru.ifru_addr,
			    sizeof(struct sockaddr));
			if (m->old)
				*(UWORD *)&rr->ifr_ifru.ifru_addr =
				    nr.ifr_ifru.ifru_addr.sa_family;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * stack state: SBTC_SYSTEM_STATUS (server side)
 */

/* SBSYSSTAT_ flags (netinclude/libraries/bsdsocket.h) */
#define	SBSYSSTAT_Interfaces		(1L << 0)
#define	SBSYSSTAT_PTP_Interfaces	(1L << 1)
#define	SBSYSSTAT_BCast_Interfaces	(1L << 2)

/*
 * "Network interfaces are available, have IP addresses assigned, are
 * configured and operational. Note that 'network interfaces' does not
 * imply the loopback interface": an IPv4 address on an interface that is
 * up and running and not the loopback.
 */
int
if_status_flags(ULONG *flags)
{
	struct nb_ifreq *k, fr;
	LONG s, n, i, e;

	*flags = 0;
	if ((s = inet_sock()) < 0)
		return sb_rumperr();
	/* (the error is read before close(), which sets it too:
	   rump_syscalls.c rsys_seterrno()) */
	if ((e = k_ifconf(s, &k, &n)) != 0) {
		rump___sysimpl_close(s);
		return e;
	}
	for (i = 0; i < n; i++) {
		WORD f;

		if (k[i].ifr_ifru.ifru_addr.sa_family != AF_INET)
			continue;
		if (k_ioctl_name(s, NB_SIOCGIFFLAGS, k[i].ifr_name, &fr) != 0)
			continue;
		f = fr.ifr_ifru.ifru_flags;
		if ((f & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING) ||
		    (f & IFF_LOOPBACK))
			continue;
		*flags |= SBSYSSTAT_Interfaces;
		if (f & IFF_POINTOPOINT)
			*flags |= SBSYSSTAT_PTP_Interfaces;
		if (f & IFF_BROADCAST)
			*flags |= SBSYSSTAT_BCast_Interfaces;
	}
	FreeVec(k);
	rump___sysimpl_close(s);
	return 0;
}

int
if_get_flags(const char *name, LONG *flags)
{
	struct nb_ifreq r;
	LONG s;
	int rv;

	if ((s = inet_sock()) < 0)
		return -1;
	rv = k_ioctl_name(s, NB_SIOCGIFFLAGS, name, &r);
	rump___sysimpl_close(s);
	if (rv == 0)
		*flags = (UWORD)r.ifr_ifru.ifru_flags;
	return rv;
}

/* ------------------------------------------------------------------------
 * In_LocalAddr() / In_CanForward(), as the kernel's in_localaddr() and
 * in_canforward() (netbsd-src/sys/netinet/in.c)
 */

static ULONG
classmask(ULONG a)
{

	if ((a & 0x80000000UL) == 0)
		return 0xff000000UL;
	if ((a & 0xc0000000UL) == 0x80000000UL)
		return 0xffff0000UL;
	return 0xffffff00UL;
}

struct inaddr_args { ULONG addr; };

static LONG
srv_localaddr(struct SocketBase *sb, struct inaddr_args *a)
{
	static const LONG mib[] = { NB_CTL_NET, AF_INET, IPPROTO_IP, 8 };
	struct nb_ifreq *k;
	struct ifaliasreq ar;
	LONG s, n, i, e, local = 0, subnetsarelocal;
	ULONG len = sizeof(subnetsarelocal);

	/* net.inet.ip.subnetsarelocal (in.c, IPCTL_SUBNETSARELOCAL) */
	if (nb_sysctl(mib, 4, &subnetsarelocal, &len, NULL, 0) != 0)
		return sb_fail(sb, sb_rumperr());
	if ((s = inet_sock()) < 0)
		return sb_fail(sb, sb_rumperr());
	if ((e = k_ifconf(s, &k, &n)) != 0) {
		rump___sysimpl_close(s);
		return sb_fail(sb, e);
	}
	for (i = 0; i < n && !local; i++) {
		const struct sockaddr_in *sin = (const void *)
		    &k[i].ifr_ifru.ifru_addr;
		ULONG ia, subnetmask, netmask;

		if (sin->sin_family != AF_INET)
			continue;
		ia = sin->sin_addr.s_addr;
		/* this address's own mask: SIOCGIFALIAS finds the address
		   given in ifra_addr and returns its ia_sockmask
		   (netbsd-src/sys/netinet/in.c:458-470, 700-701);
		   SIOCGIFNETMASK would give the interface's first address's
		   for every alias (in.c:452 in_get_ia_from_ifp_psref()) */
		memset(&ar, 0, sizeof(ar));
		CopyMem(k[i].ifr_name, ar.ifra_name, IFNAMSIZ);
		CopyMem((APTR)sin, &ar.ifra_addr, sizeof(*sin));
		if (rump___sysimpl_ioctl(s, NB_SIOCGIFALIAS, &ar) < 0)
			continue;
		subnetmask = ((struct sockaddr_in *)&ar.ifra_mask)->
		    sin_addr.s_addr;
		/* in_ifinit(): ia_netmask is the class's mask, narrowed by
		   the subnet mask */
		netmask = classmask(ia) & subnetmask;
		if (subnetsarelocal)
			local = (a->addr & netmask) == (ia & netmask);
		else
			local = (a->addr & subnetmask) == (ia & subnetmask);
	}
	FreeVec(k);
	rump___sysimpl_close(s);
	return local;
}

LONG
sb_In_LocalAddr(struct SocketBase *sb, LONG address)
{
	struct inaddr_args a = { (ULONG)address };
	LONG rv;

	/* "Returns 1 if the address was for a local host and 0 otherwise" */
	rv = sb_rpc(sb, (sbfn_t)srv_localaddr, &a, 0, NULL);
	return rv > 0 ? 1 : 0;
}

LONG
sb_In_CanForward(struct SocketBase *sb, LONG address)
{
	ULONG in = (ULONG)address;

	/* in_canforward(): IN_EXPERIMENTAL / IN_MULTICAST, class A net 0
	   or IN_LOOPBACKNET (127) */
	if ((in & 0xf0000000UL) == 0xf0000000UL ||
	    (in & 0xf0000000UL) == 0xe0000000UL)
		return 0;
	if ((in & 0x80000000UL) == 0) {
		ULONG net = in & 0xff000000UL;

		if (net == 0 || net == (127UL << 24))
			return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------------
 * the stack's interface table
 */

/* the interface calls among themselves, and against the stack's
   reconfiguration, which rewrites the table (src/stack/config.c
   stack_reconfigure_poll()) */
struct SignalSemaphore iftable_lock;

void
if_init(void)
{

	InitSemaphore(&iftable_lock);
}

/* the stack's entry for an interface name (Forbid() held), or NULL */
static struct iface *
stack_iface(const char *name)
{
	int i;

	for (i = 0; i < nifaces; i++)
		if (!ifaces[i].hidden &&
		    sb_strcmp(ifaces[i].name, name) == 0)
			return &ifaces[i];
	return NULL;
}

/* ------------------------------------------------------------------------
 * ObtainInterfaceList() / ReleaseInterfaceList()
 */

struct ifl_args { struct List *list; };

static LONG
srv_iflist(struct SocketBase *sb, struct ifl_args *a)
{
	struct nb_ifreq *k;
	LONG s, n, i, j, e;

	if ((s = inet_sock()) < 0)
		return sb_fail(sb, sb_rumperr());
	if ((e = k_ifconf(s, &k, &n)) != 0) {
		rump___sysimpl_close(s);
		return sb_fail(sb, e);
	}
	rump___sysimpl_close(s);
	/* one node per interface (the list has an entry per address) */
	for (i = 0; i < n; i++) {
		struct Node *nd;

		for (j = 0; j < i; j++)
			if (sb_strncasecmp(k[j].ifr_name, k[i].ifr_name,
			    IFNAMSIZ) == 0)
				break;
		if (j < i)
			continue;
		if ((nd = AllocVec(sizeof(*nd) + IFNAMSIZ + 1,
		    MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
			continue;
		nd->ln_Name = (char *)(nd + 1);
		CopyMem(k[i].ifr_name, nd->ln_Name, IFNAMSIZ);
		AddTail(a->list, nd);
	}
	FreeVec(k);
	return 0;
}

struct List *
sb_ObtainInterfaceList(struct SocketBase *sb)
{
	struct ifl_args a;

	if ((a.list = AllocVec(sizeof(struct List), MEMF_PUBLIC)) == NULL) {
		sb_set_errno(sb, ENOMEM);
		return NULL;
	}
	sb_newlist(a.list);
	if (sb_rpc(sb, (sbfn_t)srv_iflist, &a, 0, NULL) != 0) {
		sb_ReleaseInterfaceList(sb, a.list);
		return NULL;
	}
	return a.list;
}

void
sb_ReleaseInterfaceList(struct SocketBase *sb, struct List *list)
{
	struct Node *n;

	/* "This can be NULL in which case this routine will do nothing" */
	if (list == NULL)
		return;
	while ((n = RemHead(list)) != NULL)
		FreeVec(n);
	FreeVec(list);
}

/* ------------------------------------------------------------------------
 * QueryInterfaceTagList()
 */

/* tags (netinclude/libraries/bsdsocket.h) */
#define	IFQ_BASE		(TAG_USER + 1900)
#define	IFQ_DeviceName		(IFQ_BASE + 1)
#define	IFQ_DeviceUnit		(IFQ_BASE + 2)
#define	IFQ_HardwareAddressSize	(IFQ_BASE + 3)
#define	IFQ_HardwareAddress	(IFQ_BASE + 4)
#define	IFQ_MTU			(IFQ_BASE + 5)
#define	IFQ_BPS			(IFQ_BASE + 6)
#define	IFQ_HardwareType	(IFQ_BASE + 7)
#define	IFQ_PacketsReceived	(IFQ_BASE + 8)
#define	IFQ_PacketsSent		(IFQ_BASE + 9)
#define	IFQ_BadData		(IFQ_BASE + 10)
#define	IFQ_Overruns		(IFQ_BASE + 11)
#define	IFQ_UnknownTypes	(IFQ_BASE + 12)
#define	IFQ_LastStart		(IFQ_BASE + 13)
#define	IFQ_Address		(IFQ_BASE + 14)
#define	IFQ_DestinationAddress	(IFQ_BASE + 15)
#define	IFQ_BroadcastAddress	(IFQ_BASE + 16)
#define	IFQ_NetMask		(IFQ_BASE + 17)
#define	IFQ_Metric		(IFQ_BASE + 18)
#define	IFQ_State		(IFQ_BASE + 19)
#define	IFQ_AddressBindType	(IFQ_BASE + 20)
#define	IFQ_PrimaryDNSAddress	(IFQ_BASE + 22)
#define	IFQ_SecondaryDNSAddress	(IFQ_BASE + 23)
#define	IFQ_GetBytesIn		(IFQ_BASE + 28)
#define	IFQ_GetBytesOut		(IFQ_BASE + 29)
#define	IFQ_GetDebugMode	(IFQ_BASE + 30)
#define	IFQ_OutputDrops		(IFQ_BASE + 35)
#define	IFQ_InputDrops		(IFQ_BASE + 36)
#define	IFQ_OutputErrors	(IFQ_BASE + 37)
#define	IFQ_InputErrors		(IFQ_BASE + 38)
#define	IFQ_OutputMulticasts	(IFQ_BASE + 39)
#define	IFQ_InputMulticasts	(IFQ_BASE + 40)

#define	SM_Down		2
#define	SM_Up		3
#define	IFABT_Unknown	0
#define	IFABT_Static	1
#define	IFABT_Dynamic	2

/* devices/sana2.h (SANA+RoadshowTCP-IP/include) */
#define	S2WireType_Ethernet	1
/* netbsd-src/sys/net/if_types.h */
#define	NB_IFT_ETHER		0x06

struct ifq_args { const char *name; struct TagItem *tags; };

static void
put_sockaddr(APTR dst, const struct sockaddr *sa)
{

	CopyMem((APTR)sa, dst, sizeof(struct sockaddr_in));
}

static LONG
srv_query(struct SocketBase *sb, struct ifq_args *a)
{
	struct TagItem *tstate = a->tags, *t;
	struct nb_ifdatareq dr;
	struct nb_ifreq r;
	struct nb_ifreq *k = NULL;
	LONG s, nk = 0, e = 0, i;
	struct iface snap;
	int have_snap = 0;
	struct virtif_user *v = NULL;
	const struct sockaddr *lla = NULL;

	if ((s = inet_sock()) < 0)
		return sb_fail(sb, sb_rumperr());
	/* the interface must exist in the kernel */
	if (k_ioctl_name(s, NB_SIOCGIFFLAGS, a->name, &r) != 0) {
		e = sb_rumperr();
		rump___sysimpl_close(s);
		return sb_fail(sb, e);
	}
	memset(&dr, 0, sizeof(dr));
	sb_strlcpy(dr.ifdr_name, a->name, IFNAMSIZ);
	if (rump___sysimpl_ioctl(s, NB_SIOCGIFDATA, &dr) < 0) {
		e = sb_rumperr();
		rump___sysimpl_close(s);
		return sb_fail(sb, e);
	}
	Forbid();
	{
		struct iface *ifc = stack_iface(a->name);

		if (ifc) {
			snap = *ifc;
			have_snap = 1;
		}
	}
	Permit();
	if (have_snap)
		v = sana_find(snap.device, snap.unit);
	/* the link-level address: the interface's AF_LINK entry */
	if (k_ifconf(s, &k, &nk) == 0)
		for (i = 0; i < nk; i++)
			if (sb_strncasecmp(k[i].ifr_name, a->name, IFNAMSIZ) ==
			    0 && k[i].ifr_ifru.ifru_addr.sa_family == AF_LINK)
				lla = &k[i].ifr_ifru.ifru_addr;

	while ((t = sb_next_tag(&tstate)) != NULL) {
		APTR p = (APTR)t->ti_Data;
		ULONG req = 0;

		if (p == NULL) {
			e = EFAULT;
			continue;
		}
		switch (t->ti_Tag) {
		case IFQ_DeviceName:
			if (!have_snap) { e = ENODEV; break; }
			Forbid();
			{
				struct iface *ifc = stack_iface(a->name);

				*(STRPTR *)p = ifc ? (STRPTR)ifc->device : NULL;
			}
			Permit();
			break;
		case IFQ_DeviceUnit:
			if (!have_snap) { e = ENODEV; break; }
			*(LONG *)p = snap.unit;
			break;
		case IFQ_HardwareAddressSize:
		case IFQ_HardwareAddress:
		    {
			/* struct sockaddr_dl: sdl_nlen at 5, sdl_alen at 6,
			   the name and address from sdl_data (8) on
			   (netinclude/net/if_dl.h, netbsd-src/sys/net/if_dl.h) */
			const UBYTE *dl = (const UBYTE *)lla;

			if (dl == NULL) { e = ENODEV; break; }
			if (t->ti_Tag == IFQ_HardwareAddressSize)
				*(LONG *)p = dl[6] * 8;
			else
				CopyMem((APTR)(dl + 8 + dl[5]), p,
				    dl[6] > 16 ? 16 : dl[6]);
			break;
		    }
		case IFQ_MTU:
			if (k_ioctl_name(s, NB_SIOCGIFMTU, a->name, &r) != 0)
				e = sb_rumperr();
			else
				*(LONG *)p = r.ifr_ifru.ifru_mtu;
			break;
		case IFQ_BPS:
			*(LONG *)p = nb_u64_clamp(&dr.ifdr_data.ifi_baudrate);
			break;
		case IFQ_HardwareType:
			/* the SANA-II interfaces are Ethernet (virtif) */
			if (dr.ifdr_data.ifi_type == NB_IFT_ETHER && have_snap)
				*(LONG *)p = S2WireType_Ethernet;
			else
				e = ENODEV;
			break;
		case IFQ_PacketsReceived:
			*(ULONG *)p = nb_u64_clamp(&dr.ifdr_data.ifi_ipackets);
			break;
		case IFQ_PacketsSent:
			*(ULONG *)p = nb_u64_clamp(&dr.ifdr_data.ifi_opackets);
			break;
		case IFQ_BadData:
		case IFQ_InputErrors:
			*(ULONG *)p = nb_u64_clamp(&dr.ifdr_data.ifi_ierrors);
			break;
		case IFQ_OutputErrors:
			*(ULONG *)p = nb_u64_clamp(&dr.ifdr_data.ifi_oerrors);
			break;
		case IFQ_UnknownTypes:
			*(ULONG *)p = nb_u64_clamp(&dr.ifdr_data.ifi_noproto);
			break;
		case IFQ_InputMulticasts:
			*(ULONG *)p = nb_u64_clamp(&dr.ifdr_data.ifi_imcasts);
			break;
		case IFQ_OutputMulticasts:
			*(ULONG *)p = nb_u64_clamp(&dr.ifdr_data.ifi_omcasts);
			break;
		case IFQ_InputDrops:
		case IFQ_OutputDrops:
		    {
			/* the SANA-II layer's counts of frames it dropped
			   (src/host/sana2.c rx_dropped, tx_dropped) */
			ULONG rx, tx, rxd, txd;

			if (v == NULL) { e = ENODEV; break; }
			sana_stats(v, &rx, &tx, &rxd, &txd);
			*(ULONG *)p = t->ti_Tag == IFQ_InputDrops ? rxd : txd;
			break;
		    }
		case IFQ_GetBytesIn:
			((ULONG *)p)[0] = dr.ifdr_data.ifi_ibytes.hi;
			((ULONG *)p)[1] = dr.ifdr_data.ifi_ibytes.lo;
			break;
		case IFQ_GetBytesOut:
			((ULONG *)p)[0] = dr.ifdr_data.ifi_obytes.hi;
			((ULONG *)p)[1] = dr.ifdr_data.ifi_obytes.lo;
			break;
		case IFQ_LastStart:
			((struct __timeval *)p)->tv_secs = sb_amiga_secs(
			    nb_u64_clamp(&dr.ifdr_data.ifi_lastchange_sec));
			((struct __timeval *)p)->tv_micro =
			    dr.ifdr_data.ifi_lastchange_nsec / 1000;
			break;
		case IFQ_Address:
			req = NB_SIOCGIFADDR;
			break;
		case IFQ_DestinationAddress:
			req = NB_SIOCGIFDSTADDR;
			break;
		case IFQ_BroadcastAddress:
			req = NB_SIOCGIFBRDADDR;
			break;
		case IFQ_NetMask:
			req = NB_SIOCGIFNETMASK;
			break;
		case IFQ_Metric:
			if (k_ioctl_name(s, NB_SIOCGIFMETRIC, a->name, &r) != 0)
				e = sb_rumperr();
			else
				*(LONG *)p = r.ifr_ifru.ifru_metric;
			break;
		case IFQ_State:
			if (k_ioctl_name(s, NB_SIOCGIFFLAGS, a->name, &r) != 0)
				e = sb_rumperr();
			else
				*(LONG *)p = (r.ifr_ifru.ifru_flags & IFF_UP) ?
				    SM_Up : SM_Down;
			break;
		case IFQ_AddressBindType:
			/* the stack's knowledge: a DHCP lease, a fixed
			   address, or neither yet */
			*(LONG *)p = !have_snap || !snap.addr ? IFABT_Unknown :
			    snap.dhcp ? IFABT_Dynamic : IFABT_Static;
			break;
		case IFQ_PrimaryDNSAddress:
		case IFQ_SecondaryDNSAddress:
		    {
			/* "If the address is not known, then the IP address
			   filled in by this tag will be zero" */
			struct sockaddr_in *sin = p;
			int ix = t->ti_Tag == IFQ_PrimaryDNSAddress ? 0 : 1;

			memset(sin, 0, sizeof(*sin));
			sin->sin_len = sizeof(*sin);
			sin->sin_family = AF_INET;
			if (have_snap && snap.ndns > ix)
				sin->sin_addr.s_addr = snap.dns[ix];
			break;
		    }
		case IFQ_GetDebugMode:
			/* there is no debugging mode for an interface */
			*(LONG *)p = FALSE;
			break;
		default:
			/* the SANA-II request counts, copy statistics,
			   overruns, the hardware MTU and the lease expiry
			   are not available to this library */
			e = EINVAL;
			break;
		}
		if (req) {
			if (k_ioctl_name(s, req, a->name, &r) != 0)
				e = sb_rumperr();
			else
				put_sockaddr(p, &r.ifr_ifru.ifru_addr);
		}
	}
	if (k)
		FreeVec(k);
	rump___sysimpl_close(s);
	return e ? sb_fail(sb, e) : 0;
}

LONG
sb_QueryInterfaceTagList(struct SocketBase *sb, STRPTR name,
    struct TagItem *tags)
{
	struct ifq_args a;

	/* "This name cannot be longer than 15 characters" */
	if (name == NULL || sb_strlen((const char *)name) >= IFNAMSIZ) {
		sb_set_errno(sb, name == NULL ? EFAULT : ENAMETOOLONG);
		return -1;
	}
	a.name = (const char *)name;
	a.tags = tags;
	return sb_rpc(sb, (sbfn_t)srv_query, &a, 0, NULL);
}

/* ------------------------------------------------------------------------
 * AddInterfaceTagList() / RemoveInterface()
 */

#define	IFA_BASE		(TAG_USER + 1700)
/* (downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/netinclude/libraries/
   bsdsocket.h:379-382,418) */
#define	IFA_IPType		(IFA_BASE + 1)
#define	IFA_ARPType		(IFA_BASE + 2)
#define	IFA_LimitMTU		(IFA_BASE + 16)

/*
 * The kernel's SANA-II interfaces are clones named "sana<n>" (tools/
 * build.py: VIRTIF_BASE=sana for netbsd-src/sys/rump/net/lib/libvirtif):
 * an interface has to be named so.
 */
static int
clone_name(const char *name)
{
	const char *p;

	if (sb_strncasecmp(name, "sana", 4) != 0 || name[4] == '\0')
		return 0;
	for (p = name + 4; *p; p++)
		if (*p < '0' || *p > '9')
			return 0;
	return 1;
}

struct addif_args {
	const char *name, *device;
	LONG unit;
	struct TagItem *tags;
};

static LONG
srv_addif(struct SocketBase *sb, struct addif_args *a)
{
	struct TagItem *tstate = a->tags, *t;
	char link[80], *p;
	struct iface *ifc = NULL;
	struct virtif_user *v;
	ULONG mtu = 0;
	LONG e;
	int i;

	while ((t = sb_next_tag(&tstate)) != NULL) {
		if (t->ti_Tag == IFA_LimitMTU)
			mtu = t->ti_Data;
		/* IFA_IPType and IFA_ARPType "Default is 2048" / "2054"
		   (downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/
		   bsdsocket.doc:1190-1194): this stack sends the kernel's
		   frames with their own Ethernet type (src/host/sana2.c:1323)
		   and reads 0x0800 and 0x0806 (sana2.c:61 rxtypes), which are
		   these values, so they are taken as they are */
		else if ((t->ti_Tag == IFA_IPType && t->ti_Data == 0x0800) ||
		    (t->ti_Tag == IFA_ARPType && t->ti_Data == 0x0806))
			continue;
		else
			/* any other value, and the other IFA_ options: not
			   something this stack's SANA-II layer
			   (src/host/sana2.c) can be set to */
			return sb_fail(sb, EINVAL);
	}
	if (!clone_name(a->name))
		return sb_fail(sb, EINVAL);
	Forbid();
	if (stack_iface(a->name))
		e = EEXIST;
	else {
		e = 0;
		/* a free slot: a removed interface's, else a new one */
		for (i = 0; i < nifaces; i++)
			if (ifaces[i].hidden && !ifaces[i].dhcp &&
			    !ifaces[i].attached)
				break;
		if (i < nifaces || nifaces < MAX_IFACES)
			ifc = &ifaces[i];
		else
			e = ENOBUFS;
	}
	Permit();
	if (e)
		return sb_fail(sb, e);
	/* the link string the stack gives the kernel: "device:unit"
	   (src/stack/config.c iface_bringup()) */
	sb_strlcpy(link, a->device, sizeof(link) - 12);
	p = link + sb_strlen(link);
	*p++ = ':';
	*sb_fmt_ulong(p, (ULONG)a->unit) = '\0';
	if (rump_amibsdnet_ifcreate(a->name, link) != 0)
		return sb_fail(sb, sb_rumperr());
	if (mtu) {
		struct nb_ifreq r;
		LONG s = inet_sock();

		memset(&r, 0, sizeof(r));
		sb_strlcpy(r.ifr_name, a->name, IFNAMSIZ);
		r.ifr_ifru.ifru_mtu = mtu;
		e = (s < 0 || rump___sysimpl_ioctl(s, NB_SIOCSIFMTU, &r) < 0) ?
		    sb_rumperr() : 0;
		if (s >= 0)
			rump___sysimpl_close(s);
		if (e) {
			rump_amibsdnet_ifdestroy(a->name);
			return sb_fail(sb, e);
		}
	}
	v = sana_find(a->device, a->unit);
	Forbid();
	memset(ifc, 0, sizeof(*ifc));
	sb_strlcpy(ifc->name, a->name, sizeof(ifc->name));
	sb_strlcpy(ifc->device, a->device, sizeof(ifc->device));
	ifc->unit = a->unit;
	ifc->admin = 1;
	ifc->link = v ? sana_link(v) : 1;
	ifc->link_logged = 1;
	ifc->wireless = v ? sana_is_wireless(v) : 0;
	ifc->attached = 1;
	if (ifc == &ifaces[nifaces])
		nifaces++;
	Permit();
	return 0;
}

LONG
sb_AddInterfaceTagList(struct SocketBase *sb, STRPTR name, STRPTR device,
    LONG unit, struct TagItem *tags)
{
	struct addif_args a;

	if (name == NULL || device == NULL) {
		sb_set_errno(sb, EFAULT);
		return -1;
	}
	if (sb_strlen((const char *)name) >= IFNAMSIZ) {
		sb_set_errno(sb, ENAMETOOLONG);
		return -1;
	}
	a.name = (const char *)name;
	a.device = (const char *)device;
	a.unit = unit;
	a.tags = tags;
	ObtainSemaphore(&iftable_lock);
	unit = sb_rpc(sb, (sbfn_t)srv_addif, &a, 0, NULL);
	ReleaseSemaphore(&iftable_lock);
	return unit;
}

struct rmif_args { const char *name; LONG force; };

static LONG
srv_rmif(struct SocketBase *sb, struct rmif_args *a)
{
	struct iface *ifc;
	LONG flags = 0;
	ULONG addr = 0;
	int dhcp = 0;
	ULONG gen;

again:
	Forbid();
	gen = config_generation;
	if ((ifc = stack_iface(a->name)) != NULL) {
		addr = ifc->addr;
		dhcp = ifc->dhcp;
	}
	Permit();
	if (ifc == NULL)
		return sb_fail(sb, ENXIO);
	/* the stack's DHCP client of this interface works on its table
	   entry (src/stack/dhcp.c dhcp_thread()): the interface is in use;
	   "Use a 'force' parameter of TRUE to make it remove the interface
	   anyway" (bsdsocket.doc RemoveInterface), so then the client is
	   told to stop (client_loop(), woken()) and its end waited for */
	if (dhcp && !a->force)
		return sb_fail(sb, EBUSY);
	if (dhcp) {
		Forbid();
		ifc->stop = 1;
		ifc->dhcp_event = 1;
		Permit();
		dhcp_wake(ifc);
		/* (a new configuration meanwhile, src/stack/config.c
		   apply_config(), overwrites the table and may start a new
		   client in this entry: then all again, with the new one) */
		while (ifc->dhcp_running && config_generation == gen)
			Delay(1);
		if (config_generation != gen)
			goto again;
		Forbid();
		ifc->dhcp = 0;
		addr = ifc->addr;	/* (the client took its lease away) */
		Permit();
	}
	/* "RemoveInterface() will refuse to remove an interface which is
	   still in use" */
	if (!a->force && (addr || (if_get_flags(a->name, &flags) == 0 &&
	    (flags & IFF_UP))))
		return sb_fail(sb, EBUSY);
	if (addr)
		rump_amibsdnet_ifdeladdr4(a->name, addr);
	rump_amibsdnet_ifflags(a->name, 0, NB_IFF_UP);
	if (rump_amibsdnet_ifdestroy(a->name) != 0)
		return sb_fail(sb, sb_rumperr());
	/* out of the stack's table: the entry stays in place (other
	   entries are in use by their DHCP clients), hidden and detached */
	Forbid();
	ifc->up = 0;
	ifc->addr = 0;
	ifc->gateway = 0;
	ifc->ndns = 0;
	ifc->addr_set = 0;
	ifc->attached = 0;
	ifc->hidden = 1;
	Permit();
	stack_update_route();
	stack_update_dns();
	return 0;
}

LONG
sb_RemoveInterface(struct SocketBase *sb, STRPTR name, LONG force)
{
	struct rmif_args a;
	LONG rv;

	if (name == NULL) {
		sb_set_errno(sb, EFAULT);
		return FALSE;
	}
	a.name = (const char *)name;
	a.force = force;
	ObtainSemaphore(&iftable_lock);
	rv = sb_rpc(sb, (sbfn_t)srv_rmif, &a, 0, NULL);
	ReleaseSemaphore(&iftable_lock);
	/* "TRUE for success, 0 for failure" */
	return rv == 0 ? TRUE : FALSE;
}

/* ------------------------------------------------------------------------
 * ConfigureInterfaceTagList()
 */

#define	IFC_BASE		(TAG_USER + 1800)
#define	IFC_Address		(IFC_BASE + 1)
#define	IFC_NetMask		(IFC_BASE + 2)
#define	IFC_DestinationAddress	(IFC_BASE + 3)
#define	IFC_BroadcastAddress	(IFC_BASE + 4)
#define	IFC_Metric		(IFC_BASE + 5)
#define	IFC_AddAliasAddress	(IFC_BASE + 6)
#define	IFC_DeleteAliasAddress	(IFC_BASE + 7)
#define	IFC_State		(IFC_BASE + 8)
#define	IFC_ReleaseAddress	(IFC_BASE + 14)
#define	IFC_Complete		(IFC_BASE + 16)
#define	IFC_LimitMTU		(IFC_BASE + 17)

static void
fill_sin(struct sockaddr *sa, ULONG addr)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)sa;

	memset(sin, 0, sizeof(*sin));
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;
}

/* an address tag: "a host name to be resolved or an IP address in
   dotted-decimal notation"; returns 0 or an errno */
static LONG
tag_addr(struct SocketBase *sb, struct TagItem *t, ULONG *addr)
{

	if (t->ti_Data == 0)
		return EFAULT;
	if (netdb_resolve_addr(sb, (const char *)t->ti_Data, addr) != 0)
		return sb_interrupted(sb) ? EINTR : EADDRNOTAVAIL;
	return 0;
}

struct cfgif_args { const char *name; struct TagItem *tags; };

static LONG
srv_cfgif(struct SocketBase *sb, struct cfgif_args *a)
{
	struct TagItem *tstate = a->tags, *t;
	ULONG addr = 0, mask = 0, dst = 0, brd = 0, cur_addr = 0, cur_mask = 0;
	int have_addr = 0, have_mask = 0, have_dst = 0, have_brd = 0;
	LONG s, e = 0, flags;
	struct nb_ifreq r;
	struct iface *ifc;
	int dhcp = 0;

	if ((s = inet_sock()) < 0)
		return sb_fail(sb, sb_rumperr());
	if (k_ioctl_name(s, NB_SIOCGIFFLAGS, a->name, &r) != 0) {
		e = sb_rumperr();
		rump___sysimpl_close(s);
		return sb_fail(sb, e);
	}
	flags = (UWORD)r.ifr_ifru.ifru_flags;
	if (k_ioctl_name(s, NB_SIOCGIFADDR, a->name, &r) == 0)
		cur_addr = ((struct sockaddr_in *)
		    &r.ifr_ifru.ifru_addr)->sin_addr.s_addr;
	if (k_ioctl_name(s, NB_SIOCGIFNETMASK, a->name, &r) == 0)
		cur_mask = ((struct sockaddr_in *)
		    &r.ifr_ifru.ifru_addr)->sin_addr.s_addr;
	Forbid();
	if ((ifc = stack_iface(a->name)) != NULL)
		dhcp = ifc->dhcp;
	Permit();

	/* the addresses first, then the rest in order */
	while (e == 0 && (t = sb_next_tag(&tstate)) != NULL) {
		switch (t->ti_Tag) {
		case IFC_Address:
			e = tag_addr(sb, t, &addr);
			have_addr = 1;
			break;
		case IFC_NetMask:
			if (t->ti_Data == 0 || !netdb_parse_inet_aton(
			    (const char *)t->ti_Data, &mask))
				e = EINVAL;
			have_mask = 1;
			break;
		case IFC_DestinationAddress:
			/* "requires the IFC_Address tag to be present and
			   only works for point-to-point interfaces" */
			e = tag_addr(sb, t, &dst);
			have_dst = 1;
			break;
		case IFC_BroadcastAddress:
			/* "dotted-decimal notation"; "only works for
			   interfaces which support broadcasts" */
			if (t->ti_Data == 0 || !netdb_parse_inet_aton(
			    (const char *)t->ti_Data, &brd))
				e = EINVAL;
			else if (!(flags & IFF_BROADCAST))
				e = EINVAL;
			have_brd = 1;
			break;
		}
	}
	if (e == 0 && have_dst && (!have_addr || !(flags & IFF_POINTOPOINT)))
		e = EINVAL;
	if (e == 0 && (have_addr || have_mask || have_dst || have_brd)) {
		struct {
			char name[IFNAMSIZ];
			struct sockaddr addr, dstaddr, mask;
		} ra;
		ULONG na = have_addr ? addr : cur_addr;
		ULONG nm = have_mask ? mask : cur_mask;

		if (na == 0)
			e = EADDRNOTAVAIL;
		else {
			/* a new primary address replaces the old one */
			if (have_addr && cur_addr && cur_addr != addr)
				rump_amibsdnet_ifdeladdr4(a->name, cur_addr);
			memset(&ra, 0, sizeof(ra));
			sb_strlcpy(ra.name, a->name, IFNAMSIZ);
			fill_sin(&ra.addr, na);
			if (have_dst)
				fill_sin(&ra.dstaddr, dst);
			else if (have_brd)
				fill_sin(&ra.dstaddr, brd);
			else if (!(flags & IFF_POINTOPOINT) && nm)
				fill_sin(&ra.dstaddr, na | ~nm);
			if (nm)
				fill_sin(&ra.mask, nm);
			if (rump___sysimpl_ioctl(s, NB_SIOCAIFADDR, &ra) < 0)
				e = sb_rumperr();
			else if (ifc && !dhcp) {
				/* the stack's table: a fixed address now */
				Forbid();
				if ((ifc = stack_iface(a->name)) != NULL) {
					ifc->addr = na;
					ifc->mask = nm;
					ifc->addr_set = 1;
					ifc->up = ifc->link;
				}
				Permit();
				stack_update_route();
			}
		}
	}
	/* the other tags, in order */
	tstate = a->tags;
	while (e == 0 && (t = sb_next_tag(&tstate)) != NULL) {
		switch (t->ti_Tag) {
		case IFC_Address:
		case IFC_NetMask:
		case IFC_DestinationAddress:
		case IFC_BroadcastAddress:
			break;
		case IFC_Metric:
			memset(&r, 0, sizeof(r));
			sb_strlcpy(r.ifr_name, a->name, IFNAMSIZ);
			r.ifr_ifru.ifru_metric = t->ti_Data;
			if (rump___sysimpl_ioctl(s, NB_SIOCSIFMETRIC, &r) < 0)
				e = sb_rumperr();
			break;
		case IFC_AddAliasAddress:
		    {
			struct {
				char name[IFNAMSIZ];
				struct sockaddr addr, dstaddr, mask;
			} ra;
			ULONG al;

			if ((e = tag_addr(sb, t, &al)) != 0)
				break;
			/* no netmask given: the kernel picks one
			   (netinet/in.c in_ifinit()) */
			memset(&ra, 0, sizeof(ra));
			sb_strlcpy(ra.name, a->name, IFNAMSIZ);
			fill_sin(&ra.addr, al);
			if (rump___sysimpl_ioctl(s, NB_SIOCAIFADDR, &ra) < 0)
				e = sb_rumperr();
			break;
		    }
		case IFC_DeleteAliasAddress:
		    {
			ULONG al;

			if ((e = tag_addr(sb, t, &al)) != 0)
				break;
			if (rump_amibsdnet_ifdeladdr4(a->name, al) != 0)
				e = sb_rumperr();
			break;
		    }
		case IFC_State:
			if (t->ti_Data == SM_Up) {
				if (rump_amibsdnet_ifflags(a->name, NB_IFF_UP,
				    0) != 0)
					e = sb_rumperr();
			} else if (t->ti_Data == SM_Down) {
				if (rump_amibsdnet_ifflags(a->name, 0,
				    NB_IFF_UP) != 0)
					e = sb_rumperr();
			} else
				/* SM_Online / SM_Offline send S2_ONLINE /
				   S2_OFFLINE to the driver, which the SANA-II
				   layer offers no call for */
				e = EOPNOTSUPP;
			break;
		case IFC_ReleaseAddress:
			/* "If this interface's address was dynamically
			   bound (via DHCP or BOOTP), it will be released and
			   the interface will be taken out of service": the
			   stack's DHCP client does that when told to go
			   offline (src/stack/config.c stack_offline()) */
			if (t->ti_Data && dhcp) {
				Forbid();
				if ((ifc = stack_iface(a->name)) != NULL) {
					ifc->admin = 0;
					ifc->release = 1;
					ifc->dhcp_event = 1;
				}
				Permit();
			}
			break;
		case IFC_Complete:
			/* the configuration is complete: the routes and name
			   servers are set up from it */
			if (t->ti_Data) {
				stack_update_route();
				stack_update_dns();
			}
			break;
		case IFC_LimitMTU:
			memset(&r, 0, sizeof(r));
			sb_strlcpy(r.ifr_name, a->name, IFNAMSIZ);
			r.ifr_ifru.ifru_mtu = t->ti_Data;
			if (rump___sysimpl_ioctl(s, NB_SIOCSIFMTU, &r) < 0)
				e = sb_rumperr();
			break;
		default:
			/* IFC_GetPeerAddress, IFC_GetDNS (SANA-IIR4 driver
			   queries), IFC_AssociatedRoute, IFC_AssociatedDNS,
			   IFC_SetDebugMode: not available here */
			e = EOPNOTSUPP;
			break;
		}
	}
	rump___sysimpl_close(s);
	return e ? sb_fail(sb, e) : 0;
}

LONG
sb_ConfigureInterfaceTagList(struct SocketBase *sb, STRPTR name,
    struct TagItem *tags)
{
	struct cfgif_args a;
	LONG rv;

	if (name == NULL) {
		sb_set_errno(sb, EFAULT);
		return -1;
	}
	if (sb_strlen((const char *)name) >= IFNAMSIZ) {
		sb_set_errno(sb, ENAMETOOLONG);
		return -1;
	}
	a.name = (const char *)name;
	a.tags = tags;
	ObtainSemaphore(&iftable_lock);
	rv = sb_rpc(sb, (sbfn_t)srv_cfgif, &a, RPC_INTERRUPTIBLE, NULL);
	ReleaseSemaphore(&iftable_lock);
	return rv;
}

/* ------------------------------------------------------------------------
 * CreateAddrAllocMessageA() / DeleteAddrAllocMessage() /
 * BeginInterfaceConfig() / AbortInterfaceConfig()
 */

/* netinclude/libraries/bsdsocket.h */
struct AddressAllocationMessage {
	struct Message aam_Message;
	LONG aam_Reserved;
	LONG aam_Result;
	LONG aam_Version;
	LONG aam_Protocol;
	char aam_InterfaceName[16];
	LONG aam_Timeout;
	ULONG aam_LeaseTime;
	ULONG aam_RequestedAddress;
	STRPTR aam_ClientIdentifier;
	ULONG aam_Address;
	ULONG aam_ServerAddress;
	ULONG aam_SubnetMask;
	STRPTR aam_NAKMessage;
	LONG aam_NAKMessageSize;
	ULONG *aam_RouterTable;
	LONG aam_RouterTableSize;
	ULONG *aam_DNSTable;
	LONG aam_DNSTableSize;
	ULONG *aam_StaticRouteTable;
	LONG aam_StaticRouteTableSize;
	STRPTR aam_HostName;
	LONG aam_HostNameSize;
	STRPTR aam_DomainName;
	LONG aam_DomainNameSize;
	UBYTE *aam_BOOTPMessage;
	LONG aam_BOOTPMessageSize;
	struct DateStamp *aam_LeaseExpires;
	BOOL aam_Unicast;
};

#define	AAM_VERSION		2
#define	AAM_VERSION_MINIMUM	1
#define	AAMR_Success		0
#define	AAMR_Aborted		1
#define	AAMR_InterfaceNotKnown	2
#define	AAMR_AddressKnown	4
#define	AAMR_VersionUnknown	5
#define	AAMR_NoMemory		6
#define	AAMR_Timeout		7
#define	AAMR_Busy		11
#define	AAMR_Ignored		(-1)
#define	AAM_TIMEOUT_MIN		10
#define	AAMP_BOOTP		0
#define	AAMP_DHCP		1

#define	CAAMTA_BASE		(TAG_USER + 2000)
#define	CAAMTA_Timeout		(CAAMTA_BASE + 1)
#define	CAAMTA_LeaseTime	(CAAMTA_BASE + 2)
#define	CAAMTA_RequestedAddress	(CAAMTA_BASE + 3)
#define	CAAMTA_ClientIdentifier	(CAAMTA_BASE + 4)
#define	CAAMTA_NAKMessageSize	(CAAMTA_BASE + 5)
#define	CAAMTA_RouterTableSize	(CAAMTA_BASE + 6)
#define	CAAMTA_DNSTableSize	(CAAMTA_BASE + 7)
#define	CAAMTA_StaticRouteTableSize (CAAMTA_BASE + 8)
#define	CAAMTA_HostNameSize	(CAAMTA_BASE + 9)
#define	CAAMTA_DomainNameSize	(CAAMTA_BASE + 10)
#define	CAAMTA_BOOTPMessageSize	(CAAMTA_BASE + 11)
#define	CAAMTA_RecordLeaseExpiration (CAAMTA_BASE + 12)
#define	CAAMTA_ReplyPort	(CAAMTA_BASE + 13)
#define	CAAMTA_RequestUnicast	(CAAMTA_BASE + 14)

#define	CAAME_Success			0
#define	CAAME_Invalid_result_ptr	1
#define	CAAME_Not_enough_memory		2
#define	CAAME_Invalid_version		3
#define	CAAME_Invalid_protocol		4
#define	CAAME_Invalid_interface_name	5
#define	CAAME_Interface_not_found	6
#define	CAAME_Invalid_client_identifier	7
#define	CAAME_Client_identifier_too_short 8
#define	CAAME_Client_identifier_too_long 9

/* what CreateAddrAllocMessageA() allocates: the message, then its
   buffers */
struct aamblock {
	ULONG magic;
	ULONG size;
	struct AddressAllocationMessage msg;
};
#define	AAM_MAGIC	0x41414d42UL

LONG
sb_CreateAddrAllocMessageA(struct SocketBase *sb, LONG version,
    LONG protocol, STRPTR interface_name,
    struct AddressAllocationMessage **result_ptr, struct TagItem *tags)
{
	struct TagItem *tstate = tags, *t;
	ULONG timeout = AAM_TIMEOUT_MIN, lease = 0, reqaddr = 0, size;
	LONG nak = 0, routers = 0, dns = 0, statics = 0, host = 0, dom = 0;
	LONG bootp = 0, cidlen = 0;
	int record = 0, unicast = 0, found;
	const char *cid = NULL;
	struct MsgPort *reply = NULL;
	struct aamblock *b;
	struct AddressAllocationMessage *m;
	UBYTE *p;

	if (result_ptr == NULL)
		return CAAME_Invalid_result_ptr;
	*result_ptr = NULL;
	if (version < AAM_VERSION_MINIMUM || version > AAM_VERSION)
		return CAAME_Invalid_version;
	/* "Configuration protocol type, either AAMP_BOOTP or AAMP_DHCP" */
	if (protocol != AAMP_BOOTP && protocol != AAMP_DHCP)
		return CAAME_Invalid_protocol;
	if (interface_name == NULL || interface_name[0] == '\0' ||
	    sb_strlen((const char *)interface_name) >= 16)
		return CAAME_Invalid_interface_name;
	Forbid();
	found = stack_iface((const char *)interface_name) != NULL;
	Permit();
	if (!found)
		return CAAME_Interface_not_found;
	while ((t = sb_next_tag(&tstate)) != NULL) {
		switch (t->ti_Tag) {
		case CAAMTA_Timeout:
			/* "If it is shorter, it is automatically extended to
			   10 seconds" */
			timeout = t->ti_Data < AAM_TIMEOUT_MIN ?
			    AAM_TIMEOUT_MIN : t->ti_Data;
			break;
		case CAAMTA_LeaseTime: lease = t->ti_Data; break;
		case CAAMTA_RequestedAddress: reqaddr = t->ti_Data; break;
		case CAAMTA_ClientIdentifier:
			cid = (const char *)t->ti_Data;
			if (cid == NULL || cid[0] == '\0')
				return CAAME_Invalid_client_identifier;
			cidlen = sb_strlen(cid);
			/* "at least 2 characters long and it cannot be
			   longer than 255 characters" */
			if (cidlen < 2)
				return CAAME_Client_identifier_too_short;
			if (cidlen > 255)
				return CAAME_Client_identifier_too_long;
			break;
		case CAAMTA_NAKMessageSize: nak = t->ti_Data; break;
		case CAAMTA_RouterTableSize: routers = t->ti_Data; break;
		case CAAMTA_DNSTableSize: dns = t->ti_Data; break;
		case CAAMTA_StaticRouteTableSize: statics = t->ti_Data; break;
		case CAAMTA_HostNameSize: host = t->ti_Data; break;
		case CAAMTA_DomainNameSize: dom = t->ti_Data; break;
		case CAAMTA_BOOTPMessageSize: bootp = t->ti_Data; break;
		case CAAMTA_RecordLeaseExpiration: record = t->ti_Data != 0;
			break;
		case CAAMTA_ReplyPort:
			reply = (struct MsgPort *)t->ti_Data;
			break;
		case CAAMTA_RequestUnicast: unicast = t->ti_Data != 0; break;
		}
	}
	if (nak < 0 || routers < 0 || dns < 0 || statics < 0 || host < 0 ||
	    dom < 0 || bootp < 0)
		return CAAME_Not_enough_memory;
	size = sizeof(*b) + 4 * (routers + dns + statics) +
	    (record ? sizeof(struct DateStamp) : 0) + nak + host + dom +
	    bootp + (cid ? cidlen + 1 : 0);
	if ((b = AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return CAAME_Not_enough_memory;
	b->magic = AAM_MAGIC;
	b->size = size;
	m = &b->msg;
	m->aam_Message.mn_Node.ln_Type = NT_MESSAGE;
	m->aam_Message.mn_Length = sizeof(*m);
	m->aam_Message.mn_ReplyPort = reply;
	m->aam_Version = version;
	m->aam_Protocol = protocol;
	sb_strlcpy(m->aam_InterfaceName, (const char *)interface_name, 16);
	m->aam_Timeout = timeout;
	m->aam_LeaseTime = lease;
	m->aam_RequestedAddress = reqaddr;
	m->aam_Unicast = unicast;
	p = (UBYTE *)(b + 1);
	/* the ULONG tables first (aligned), then the strings */
	if (routers) {
		m->aam_RouterTable = (ULONG *)p;
		m->aam_RouterTableSize = routers;
		p += 4 * routers;
	}
	if (dns) {
		m->aam_DNSTable = (ULONG *)p;
		m->aam_DNSTableSize = dns;
		p += 4 * dns;
	}
	if (statics) {
		m->aam_StaticRouteTable = (ULONG *)p;
		m->aam_StaticRouteTableSize = statics;
		p += 4 * statics;
	}
	if (record) {
		m->aam_LeaseExpires = (struct DateStamp *)p;
		p += sizeof(struct DateStamp);
	}
	if (nak) {
		m->aam_NAKMessage = (STRPTR)p;
		m->aam_NAKMessageSize = nak;
		p += nak;
	}
	if (host) {
		m->aam_HostName = (STRPTR)p;
		m->aam_HostNameSize = host;
		p += host;
	}
	if (dom) {
		m->aam_DomainName = (STRPTR)p;
		m->aam_DomainNameSize = dom;
		p += dom;
	}
	if (bootp) {
		m->aam_BOOTPMessage = p;
		m->aam_BOOTPMessageSize = bootp;
		p += bootp;
	}
	if (cid) {
		/* "The name will be duplicated and stored" */
		m->aam_ClientIdentifier = (STRPTR)p;
		sb_strlcpy((char *)p, cid, cidlen + 1);
	}
	*result_ptr = m;
	return CAAME_Success;
}

void
sb_DeleteAddrAllocMessage(struct SocketBase *sb,
    struct AddressAllocationMessage *m)
{
	struct aamblock *b;

	/* "Passing a NULL pointer in place of a valid message address is
	   harmless"; "can only deallocate address allocation messages
	   created by CreateAddrAllocMessageA()" */
	if (m == NULL)
		return;
	b = (struct aamblock *)((UBYTE *)m - SB_OFFSETOF(struct aamblock, msg));
	if (b->magic != AAM_MAGIC)
		return;
	b->magic = 0;
	FreeVec(b);
}

/* a running BeginInterfaceConfig() */
struct aamreq {
	struct aamreq *next;
	struct AddressAllocationMessage *msg;
	struct iface *ifc;
	volatile int abort;
};

static struct aamreq *aamreqs;	/* under Forbid() */

static void
aam_reply(struct AddressAllocationMessage *m, LONG result)
{

	m->aam_Result = result;
	ReplyMsg(&m->aam_Message);
}

/* fill in what the stack's DHCP client learnt (its table entry) */
static void
aam_results(struct AddressAllocationMessage *m, const struct iface *ifc)
{
	int i;

	m->aam_Address = ifc->addr;
	m->aam_SubnetMask = ifc->mask;
	/* "The IP address of the BOOTP server which responsed to the
	   query; this may be 0": the stack's client keeps it to itself */
	m->aam_ServerAddress = 0;
	/* "The allocation process will eventually reset
	   'aam_RequestedAddress' to zero" */
	m->aam_RequestedAddress = 0;
	if (m->aam_NAKMessage && m->aam_NAKMessageSize > 0)
		m->aam_NAKMessage[0] = '\0';
	for (i = 0; i < m->aam_RouterTableSize && m->aam_RouterTable; i++)
		m->aam_RouterTable[i] = i == 0 ? ifc->gateway : 0;
	for (i = 0; i < m->aam_DNSTableSize && m->aam_DNSTable; i++)
		m->aam_DNSTable[i] = i < ifc->ndns ? ifc->dns[i] : 0;
	for (i = 0; i < m->aam_StaticRouteTableSize &&
	    m->aam_StaticRouteTable; i++)
		m->aam_StaticRouteTable[i] = 0;
	if (m->aam_HostName && m->aam_HostNameSize > 0)
		m->aam_HostName[0] = '\0';
	if (m->aam_DomainName && m->aam_DomainNameSize > 0)
		sb_strlcpy((char *)m->aam_DomainName, netdb_get_domain(),
		    m->aam_DomainNameSize);
}

static void *
aam_waiter(void *arg)
{
	struct aamreq *q = arg, **qp;
	struct AddressAllocationMessage *m = q->msg;
	ULONG waited = 0, limit = (ULONG)m->aam_Timeout * 1000;
	LONG result;
	struct iface snap;

	for (;;) {
		Forbid();
		snap = *q->ifc;
		Permit();
		if (snap.up && snap.addr) {
			result = AAMR_Success;
			break;
		}
		if (q->abort) {
			result = AAMR_Aborted;
			break;
		}
		if (waited >= limit) {
			result = AAMR_Timeout;
			break;
		}
		amiga_host_sleep_ms(100);
		waited += 100;
	}
	Forbid();
	for (qp = &aamreqs; *qp && *qp != q; qp = &(*qp)->next)
		;
	if (*qp)
		*qp = q->next;
	Permit();
	if (result == AAMR_Success)
		aam_results(m, &snap);
	FreeVec(q);
	aam_reply(m, result);
	return NULL;
}

/* dhcp_configure() makes kernel calls (rump_amibsdnet_ifflags(),
   src/stack/dhcp.c), which only a rump thread may: each rump system call
   sets errno through rumpuser_seterrno() (rump_syscalls.c
   rsys_seterrno()), whose self() is the task's tc_UserData
   (src/host/rumpuser_amiga.c self()), set for rump threads only.  So it
   runs in the server thread. */
static LONG
srv_dhcp_start(struct SocketBase *sb, struct iface *ifc)
{

	return dhcp_configure(ifc) != 0 ? -1 : 0;
}

void
sb_BeginInterfaceConfig(struct SocketBase *sb,
    struct AddressAllocationMessage *m)
{
	struct iface *ifc;
	struct aamreq *q;
	LONG result = -2;

	if (m == NULL)
		return;
	if (m->aam_Version < AAM_VERSION_MINIMUM ||
	    m->aam_Version > AAM_VERSION) {
		aam_reply(m, AAMR_VersionUnknown);
		return;
	}
	/* the stack's client speaks DHCP (src/stack/dhcp.c); BOOTP and the
	   automatic (169.254/16) protocols it does not */
	if (m->aam_Protocol != AAMP_DHCP) {
		aam_reply(m, AAMR_Ignored);
		return;
	}
	if (m->aam_Timeout < AAM_TIMEOUT_MIN)
		m->aam_Timeout = AAM_TIMEOUT_MIN;
	if ((q = AllocVec(sizeof(*q), MEMF_PUBLIC | MEMF_CLEAR)) == NULL) {
		aam_reply(m, AAMR_NoMemory);
		return;
	}
	ObtainSemaphore(&iftable_lock);
	Forbid();
	ifc = stack_iface(m->aam_InterfaceName);
	if (ifc == NULL)
		result = AAMR_InterfaceNotKnown;
	else if (ifc->up && ifc->addr)
		result = AAMR_AddressKnown;
	else if (ifc->dhcp)
		result = AAMR_Busy;	/* its DHCP client is at it */
	else if (stack_reconfig_pending())
		/* (a client started now would belong to the coming
		   configuration, config_generation, and the reconfiguration
		   waits for all clients to end: stack_reconfigure_poll()) */
		result = AAMR_Busy;
	else
		ifc->dhcp = 1;
	Permit();
	if (result == -2 &&
	    sb_rpc(sb, (sbfn_t)srv_dhcp_start, ifc, 0, NULL) != 0) {
		Forbid();
		ifc->dhcp = 0;
		Permit();
		result = AAMR_NoMemory;
	}
	ReleaseSemaphore(&iftable_lock);
	if (result != -2) {
		FreeVec(q);
		aam_reply(m, result);
		return;
	}
	q->msg = m;
	q->ifc = ifc;
	Forbid();
	q->next = aamreqs;
	aamreqs = q;
	Permit();
	if (rumpuser_thread_create(aam_waiter, q, "bsdsocket DHCP wait", 0,
	    0, -1, NULL) != 0) {
		struct aamreq **qp;

		Forbid();
		for (qp = &aamreqs; *qp && *qp != q; qp = &(*qp)->next)
			;
		if (*qp)
			*qp = q->next;
		Permit();
		FreeVec(q);
		aam_reply(m, AAMR_NoMemory);
	}
}

void
sb_AbortInterfaceConfig(struct SocketBase *sb,
    struct AddressAllocationMessage *m)
{
	struct aamreq *q;

	/* "There is no guarantee that the message can be intercepted" */
	Forbid();
	for (q = aamreqs; q; q = q->next)
		if (q->msg == m)
			q->abort = 1;
	Permit();
}
