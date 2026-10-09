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
	int up;				/* has an address and is usable */
	volatile int admin;		/* wanted online (NetCtrl ONLINE/OFFLINE) */
	volatile int link;		/* driver reports a link (default yes) */
	volatile int dhcp_event;	/* wakes the DHCP client */
	int wireless;			/* driver supports SANA-II wireless */
	int link_logged;
};

extern struct iface ifaces[MAX_IFACES];
extern int nifaces;

int	stack_configure(const char *path);
void	stack_offline(void);
void	stack_online(void);
void	stack_link_changed(void);
int	stack_reconfigure(void);
extern volatile ULONG config_generation;

/* control port (control.c) */
int	control_init(void);
ULONG	control_sigmask(void);
void	control_handle(void);
ULONG	netcfg_primary_address(void);
void	sb_copy(char *, const char *, unsigned long);

/* wireless (wireless.c): starts WirelessManager for the interface */
void	wireless_start(struct iface *);

/* DHCP client (dhcp.c): starts the interface's client thread */
int	dhcp_configure(struct iface *);

/* resolver configuration (src/lib/netdb.c) */
void	netdb_set_nameservers(const ULONG *, int);
void	netdb_set_domain(const char *);
void	netdb_set_hostname(const char *);

/* kernel-side helpers (src/kern/netcfg.c) */
int	rump_amibsdnet_ifcreate(const char *, const char *);
int	rump_amibsdnet_ifaddr4(const char *, ULONG, ULONG);
int	rump_amibsdnet_ifflags(const char *, int, int);
int	rump_amibsdnet_ifdeladdr4(const char *, ULONG);
int	rump_amibsdnet_route4(int, ULONG, ULONG, ULONG);
#define	NB_RTM_ADD	1
#define	NB_RTM_DELETE	2
#define	NB_IFF_UP	0x0001

#endif /* STACK_H */
