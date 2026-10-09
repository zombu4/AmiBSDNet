/*
 * AmiBSDNet stack program: shared declarations.
 */
#ifndef STACK_H
#define STACK_H

#include <exec/types.h>

#define	MAX_IFACES	4

struct iface {
	char name[16];			/* kernel interface, e.g. "sana0" */
	char device[64];		/* SANA-II driver */
	ULONG unit;
	int dhcp;
	ULONG addr, mask;		/* network order (= host order on m68k) */
	ULONG gateway;
	int up;
};

extern struct iface ifaces[MAX_IFACES];
extern int nifaces;

int	stack_configure(const char *path);
void	stack_offline(void);
ULONG	netcfg_primary_address(void);
void	sb_copy(char *, const char *, unsigned long);

/* DHCP client (dhcp.c): returns 0 when configured */
int	dhcp_configure(struct iface *);

/* resolver configuration (src/lib/netdb.c) */
void	netdb_set_nameservers(const ULONG *, int);
void	netdb_set_domain(const char *);
void	netdb_set_hostname(const char *);

/* kernel-side helpers (src/kern/netcfg.c) */
int	rump_amibsdnet_ifcreate(const char *, const char *);
int	rump_amibsdnet_ifaddr4(const char *, ULONG, ULONG);
int	rump_amibsdnet_ifflags(const char *, int, int);
int	rump_amibsdnet_route4(int, ULONG, ULONG, ULONG);
#define	NB_RTM_ADD	1
#define	NB_RTM_DELETE	2
#define	NB_IFF_UP	0x0001

#endif /* STACK_H */
