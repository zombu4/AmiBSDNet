/*
 * Minimal Amiga network types for AmiBSDNet's own client programs (tools
 * and tests), so they can use the NDK's <inline/bsdsocket.h> without the
 * full Roadshow SDK.  Layouts are the standard 4.4BSD/AmiTCP ones.
 */
#ifndef AMIBSDNET_NET_H
#define AMIBSDNET_NET_H

#include <exec/types.h>
#include <devices/timer.h>
#include <utility/tagitem.h>

/* keep <inline/bsdsocket.h> from pulling in SDK headers we do not have */
#define	NETINET_IN_H
#define	SYS_SOCKET_H
#define	SYS_MBUF_H
#define	NET_ROUTE_H
#define	NETDB_H
#define	LIBRARIES_BSDSOCKET_H

typedef ULONG socklen_t;
typedef ULONG in_addr_t;
typedef UWORD in_port_t;

struct in_addr { in_addr_t s_addr; };
struct sockaddr { UBYTE sa_len; UBYTE sa_family; char sa_data[14]; };
struct sockaddr_in {
	UBYTE sin_len;
	UBYTE sin_family;
	in_port_t sin_port;
	struct in_addr sin_addr;
	char sin_zero[8];
};
struct __timeval { ULONG tv_secs; ULONG tv_micro; };
struct hostent {
	char *h_name;
	char **h_aliases;
	LONG h_addrtype;
	LONG h_length;
	char **h_addr_list;
};
struct addrinfo {
	LONG ai_flags, ai_family, ai_socktype, ai_protocol;
	socklen_t ai_addrlen;
	char *ai_canonname;
	struct sockaddr *ai_addr;
	struct addrinfo *ai_next;
};
struct msghdr;
struct netent;
struct servent;
struct protoent;
struct mbuf;
struct RoadshowDataNode;

#define	AF_INET		2
#define	SOCK_STREAM	1
#define	SOCK_DGRAM	2
#define	SOL_SOCKET	0xffff
#define	SO_RCVTIMEO	0x1006
#define	INADDR_LOOPBACK	0x7f000001UL
#define	FIONBIO		0x8004667eUL
#define	EINTR		4
#define	EWOULDBLOCK	35

/* fd_set helpers for WaitSelect() */
#define	AMI_FD_WORDS	8
typedef struct { ULONG bits[AMI_FD_WORDS]; } ami_fd_set;
#define	AMI_FD_ZERO(s)	do { int _i; for (_i = 0; _i < AMI_FD_WORDS; _i++) \
			    (s)->bits[_i] = 0; } while (0)
#define	AMI_FD_SET(fd, s)	((s)->bits[(fd) >> 5] |= 1UL << ((fd) & 31))
#define	AMI_FD_ISSET(fd, s)	(((s)->bits[(fd) >> 5] >> ((fd) & 31)) & 1)

#endif /* AMIBSDNET_NET_H */
