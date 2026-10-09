/*
 * AmiBSDNet control protocol: the stack listens on the public message
 * port AMIBSDNET_PORTNAME.  Clients (NetCtrl, the Workbench status icon)
 * send a NetCtrlMsg and wait for the reply.
 */
#ifndef AMIBSDNET_CONTROL_H
#define AMIBSDNET_CONTROL_H

#include <exec/types.h>
#include <exec/ports.h>

#define	AMIBSDNET_PORTNAME	"AmiBSDNet"

#define	NETCTRL_STATE		1	/* quick: online + primary address */
#define	NETCTRL_STATUS		2	/* full status report in text[] */
#define	NETCTRL_ONLINE		3
#define	NETCTRL_OFFLINE		4
#define	NETCTRL_RECONFIG	5	/* re-read the configuration file */
#define	NETCTRL_IFLIST		6	/* fill ifaces[] */

#define	NETCTRL_TEXTSIZE	2048
#define	NETCTRL_MAXIFACES	4

/* NetCtrlIface.flags */
#define	NETIF_UP		0x01	/* has an address and is usable */
#define	NETIF_ADMIN		0x02	/* wanted online */
#define	NETIF_LINK		0x04	/* link (cable / Wi-Fi association) */
#define	NETIF_DHCP		0x08
#define	NETIF_WIRELESS		0x10	/* SANA-II wireless driver */
#define	NETIF_LINKEVENTS	0x20	/* driver reports link changes */

struct NetCtrlIface {
	char	name[16];		/* "sana0" */
	char	device[64];		/* SANA-II driver */
	ULONG	unit;
	ULONG	address, netmask, gateway;
	UBYTE	mac[6];
	UWORD	flags;
};

struct NetCtrlMsg {
	struct Message	msg;
	ULONG	cmd;
	LONG	result;			/* 0 = ok */
	ULONG	online;			/* any interface up with an address */
	ULONG	address;		/* primary IPv4 address, 0 if none */
	ULONG	opencount;		/* programs using bsdsocket.library */
	ULONG	link;			/* any interface has a link */
	char	text[NETCTRL_TEXTSIZE];
	ULONG	nifaces;		/* NETCTRL_IFLIST */
	struct NetCtrlIface ifaces[NETCTRL_MAXIFACES];
	ULONG	ndns;			/* NETCTRL_IFLIST: name servers in use */
	ULONG	dns[4];
};

#endif /* AMIBSDNET_CONTROL_H */
