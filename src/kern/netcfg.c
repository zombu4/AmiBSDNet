/*
 * Interface and route configuration, callable from AmiBSDNet host code.
 *
 * Compiled against the NetBSD kernel headers so ifreq/ifaliasreq/ifdrv,
 * ioctl numbers and routing messages are exactly NetBSD's, but it only
 * talks to the kernel through the rump system call entry points
 * (rump___sysimpl_*), which schedule themselves, so the functions here
 * are called from the host side without holding a rump CPU.
 *
 * All addresses are IPv4 in network byte order (m68k is big-endian, so
 * that is also host order).  Functions return 0 or -1; the NetBSD errno
 * value of a failure is available through amiga_rump_errno().
 * The rump_ prefix keeps these names out of the rumpns_ symbol rename.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>

#include <lib/libkern/libkern.h>

int	rump___sysimpl_socket30(int, int, int);
int	rump___sysimpl_ioctl(int, u_long, void *);
int	rump___sysimpl_close(int);
ssize_t	rump___sysimpl_write(int, const void *, size_t);

int	rump_amibsdnet_ifcreate(const char *, const char *);
int	rump_amibsdnet_ifaddr4(const char *, uint32_t, uint32_t);
int	rump_amibsdnet_ifflags(const char *, int, int);
int	rump_amibsdnet_ifdeladdr4(const char *, uint32_t);
int	rump_amibsdnet_route4(int, uint32_t, uint32_t, uint32_t);

static int
inet_socket(void)
{

	return rump___sysimpl_socket30(AF_INET, SOCK_DGRAM, 0);
}

/* create a cloned interface ("sana0") and point it at its link */
int
rump_amibsdnet_ifcreate(const char *ifname, const char *linkstr)
{
	struct ifreq ifr;
	struct ifdrv ifd;
	int s, rv;

	if ((s = inet_socket()) < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	rv = rump___sysimpl_ioctl(s, SIOCIFCREATE, &ifr);
	if (rv == 0 && linkstr != NULL) {
		memset(&ifd, 0, sizeof(ifd));
		strlcpy(ifd.ifd_name, ifname, sizeof(ifd.ifd_name));
		ifd.ifd_cmd = 0;
		ifd.ifd_len = strlen(linkstr) + 1;
		ifd.ifd_data = __UNCONST(linkstr);
		rv = rump___sysimpl_ioctl(s, SIOCSLINKSTR, &ifd);
	}
	rump___sysimpl_close(s);
	return rv;
}

/* add an IPv4 address (this also brings the interface up) */
int
rump_amibsdnet_ifaddr4(const char *ifname, uint32_t addr, uint32_t mask)
{
	struct ifaliasreq ifra;
	struct sockaddr_in *sin;
	int s, rv;

	if ((s = inet_socket()) < 0)
		return -1;
	memset(&ifra, 0, sizeof(ifra));
	strlcpy(ifra.ifra_name, ifname, sizeof(ifra.ifra_name));

	sin = (struct sockaddr_in *)&ifra.ifra_addr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;

	sin = (struct sockaddr_in *)&ifra.ifra_mask;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = mask;

	sin = (struct sockaddr_in *)&ifra.ifra_broadaddr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr | ~mask;

	rv = rump___sysimpl_ioctl(s, SIOCAIFADDR, &ifra);
	rump___sysimpl_close(s);
	return rv;
}

/* remove an IPv4 address from an interface */
int
rump_amibsdnet_ifdeladdr4(const char *ifname, uint32_t addr)
{
	struct ifreq ifr;
	struct sockaddr_in *sin;
	int s, rv;

	if ((s = inet_socket()) < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;
	rv = rump___sysimpl_ioctl(s, SIOCDIFADDR, &ifr);
	rump___sysimpl_close(s);
	return rv;
}

/* set and clear interface flags (IFF_UP etc.) */
int
rump_amibsdnet_ifflags(const char *ifname, int set, int clear)
{
	struct ifreq ifr;
	int s, rv;

	if ((s = inet_socket()) < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	rv = rump___sysimpl_ioctl(s, SIOCGIFFLAGS, &ifr);
	if (rv == 0) {
		ifr.ifr_flags = (ifr.ifr_flags | set) & ~clear;
		rv = rump___sysimpl_ioctl(s, SIOCSIFFLAGS, &ifr);
	}
	rump___sysimpl_close(s);
	return rv;
}

/*
 * Add (RTM_ADD) or delete (RTM_DELETE) an IPv4 route through a gateway.
 * dst 0 / mask 0 is the default route.
 */
int
rump_amibsdnet_route4(int cmd, uint32_t dst, uint32_t mask, uint32_t gw)
{
	struct {
		struct rt_msghdr rtm;
		struct sockaddr_in addr[3];	/* dst, gateway, netmask */
	} m;
	int s, i;
	ssize_t n;

	memset(&m, 0, sizeof(m));
	m.rtm.rtm_msglen = sizeof(m);
	m.rtm.rtm_version = RTM_VERSION;
	m.rtm.rtm_type = cmd;
	m.rtm.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
	m.rtm.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
	m.rtm.rtm_seq = 1;
	for (i = 0; i < 3; i++) {
		m.addr[i].sin_len = sizeof(m.addr[i]);
		m.addr[i].sin_family = AF_INET;
	}
	/* sockaddr_in is 16 bytes, already a multiple of RT_ROUNDUP's unit */
	CTASSERT(sizeof(struct sockaddr_in) % sizeof(uint64_t) == 0);
	m.addr[0].sin_addr.s_addr = dst;
	m.addr[1].sin_addr.s_addr = gw;
	m.addr[2].sin_addr.s_addr = mask;

	if ((s = rump___sysimpl_socket30(PF_ROUTE, SOCK_RAW, 0)) < 0)
		return -1;
	n = rump___sysimpl_write(s, &m, sizeof(m));
	rump___sysimpl_close(s);
	return n == (ssize_t)sizeof(m) ? 0 : -1;
}
