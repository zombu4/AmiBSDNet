/*
 * AmiBSDNet stack program: shared declarations.
 */
#ifndef STACK_H
#define STACK_H

#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/semaphores.h>

/* the release version (AMIBSDNET_VERSION, AMIBSDNET_VERSTAG()), written
   by tools/version.py, which tools/build_stack.sh runs: the stack's $VER:
   string, the log and the status report use it (main.c, control.c) */
#include "amibsdnet_version.h"

#define	MAX_IFACES	4

struct iface {
	char name[16];			/* kernel interface, e.g. "sana0" */
	char device[64];		/* SANA-II driver */
	ULONG unit;
	int dhcp;
	ULONG addr, mask;		/* network order (= host order on m68k) */
	ULONG gateway;			/* this interface's router, 0 if none */
	ULONG dns[4];			/* its name servers (from DHCP) */
	int ndns;
	char domain[128];		/* its domain name (from DHCP), or "" */
	int up;				/* has an address and is usable */
	volatile int admin;		/* wanted online (NetCtrl ONLINE/OFFLINE) */
	volatile int link;		/* driver reports a link (default yes) */
	volatile int dhcp_event;	/* wakes the DHCP client */
	int wireless;			/* driver supports SANA-II wireless */
	int attached;			/* the driver could be opened */
	volatile int release;		/* went offline: give up the lease */
	volatile int stop;		/* RemoveInterface(force): the DHCP
					   client ends (src/lib/ifapi.c) */
	volatile int dhcp_running;	/* its DHCP client thread exists
					   (dhcp_configure() to its end) */
	int optional;			/* plug-in adapter: hide if absent */
	int hidden;			/* optional and its driver did not open */
	int addr_set;			/* the fixed address is in the kernel */
	int unverified;			/* driver file failed the check */
	int link_logged;
	/* the running DHCP client and its wake-up signal (set and cleared
	   by it under Forbid; dhcp_wake()) */
	struct Task *volatile dhcp_task;
	volatile ULONG dhcp_sigmask;
};

extern struct iface ifaces[MAX_IFACES];
extern int nifaces;
/* the stack process: link changes and DHCP clients that end signal it
   (SIGBREAKF_CTRL_E) */
extern struct Task *volatile stack_task;

int	stack_configure_from(const char *path, const char *fallback);
void	stack_offline(void);
void	stack_online(void);
void	stack_link_changed(void);
/* 1: in progress (stack_reconfigure_poll() finishes it, then calls
   control_reconfig_done()), 0: applied, -1: no configuration file could
   be read (the one in use stays) */
int	stack_reconfigure_begin(void);
void	stack_reconfigure_poll(void);
void	stack_update_route(void);
void	stack_update_dns(void);
void	stack_update_domain(void);
extern volatile ULONG config_generation;
int	stack_reconfig_pending(void);
/* held by the library's interface calls and by the reconfiguration
   (src/lib/ifapi.c, initialised by if_init() when bsdsocket.library is
   made, before the stack configures; src/stack/main.c) */
extern struct SignalSemaphore iftable_lock;

/* control port (control.c) */
int	control_init(void);
ULONG	control_sigmask(void);
void	control_handle(void);
void	control_shutdown(void);	/* the kernel is gone */
int	rump_amibsdnet_panic(int);	/* src/kern/debugpanic.c */
void	control_reconfig_done(int result);
ULONG	netcfg_primary_address(void);
void	sb_copy(char *, const char *, unsigned long);

/* trial switch-over from another stack (trial.c) */
int	trial_begin(void);
void	trial_failed(void);
int	trial_code_running(void);	/* a process runs our code */
int	stack_connected(void);

/* wireless (wireless.c): starts WirelessManager for the interface */
void	wireless_start(struct iface *);

/* DHCP client (dhcp.c): starts the interface's client thread */
int	dhcp_configure(struct iface *);
void	dhcp_wake(struct iface *);	/* signals its client, if any */
extern volatile int dhcp_clients;

/* resolver configuration (src/lib/netdb.c) */
void	netdb_set_nameservers(const ULONG *, int);
void	netdb_set_domain(const char *);
void	netdb_set_hostname(const char *);

/* kernel-side helpers (src/kern/netcfg.c) */
int	rump_amibsdnet_ifcreate(const char *, const char *);
int	rump_amibsdnet_ifdestroy(const char *);
int	rump_amibsdnet_ifaddr4(const char *, ULONG, ULONG);
int	rump_amibsdnet_ifflags(const char *, int, int);
int	rump_amibsdnet_ifdeladdr4(const char *, ULONG);
int	rump_amibsdnet_route4(int, ULONG, ULONG, ULONG, ULONG);
#define	NB_RTM_ADD	1
#define	NB_RTM_DELETE	2
#define	NB_IFF_UP	0x0001

#endif /* STACK_H */
