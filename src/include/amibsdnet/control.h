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

#define	NETCTRL_TEXTSIZE	2048

struct NetCtrlMsg {
	struct Message	msg;
	ULONG	cmd;
	LONG	result;			/* 0 = ok */
	ULONG	online;			/* any interface up with an address */
	ULONG	address;		/* primary IPv4 address, 0 if none */
	ULONG	opencount;		/* programs using bsdsocket.library */
	char	text[NETCTRL_TEXTSIZE];
};

#endif /* AMIBSDNET_CONTROL_H */
