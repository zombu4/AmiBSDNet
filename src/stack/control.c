/*
 * AmiBSDNet: the public control port (see <amibsdnet/control.h>).
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <proto/exec.h>

#include <amibsdnet/control.h>

#include "rumpuser_amiga.h"
#include "sana2_host.h"
#include "stack.h"

extern struct ExecBase *SysBase;

static struct MsgPort *ctlport;

/* RECONFIG requests waiting for the reconfiguration to end */
static struct List reconfig_waiting;

/*
 * How much of a message the sender gave (mn_Length, set by
 * amibsdnet_ctl_call() to sizeof(struct NetCtrlMsg); older clients send
 * the shorter message without the DNS fields).  A field is written only
 * if the message reaches that far.
 */
#define	MSG_HAS(m, field) \
	((m)->msg.mn_Length >= __builtin_offsetof(struct NetCtrlMsg, field) + \
	    sizeof((m)->field))

/* tiny text builder for the status report */
struct tb {
	char *p;
	ULONG left;
};

static void
tb_s(struct tb *b, const char *s)
{

	while (*s && b->left > 1) {
		*b->p++ = *s++;
		b->left--;
	}
	*b->p = '\0';
}

static void
tb_u(struct tb *b, ULONG v)
{
	char tmp[12];
	int i = 11;

	tmp[i] = '\0';
	do {
		tmp[--i] = '0' + v % 10;
		v /= 10;
	} while (v);
	tb_s(b, tmp + i);
}

static void
tb_ip(struct tb *b, ULONG a)
{

	tb_u(b, a >> 24); tb_s(b, ".");
	tb_u(b, (a >> 16) & 255); tb_s(b, ".");
	tb_u(b, (a >> 8) & 255); tb_s(b, ".");
	tb_u(b, a & 255);
}

static void
tb_hex2(struct tb *b, UBYTE v)
{
	char t[3];

	t[0] = "0123456789abcdef"[v >> 4];
	t[1] = "0123456789abcdef"[v & 15];
	t[2] = '\0';
	tb_s(b, t);
}

static ULONG
prefixlen(ULONG mask)
{
	ULONG n = 0;

	while (mask & 0x80000000UL) {
		n++;
		mask <<= 1;
	}
	return n;
}

int	netdb_get_nameservers(ULONG *, int);
const char *netdb_get_domain(void);

static void
iface_list(struct NetCtrlMsg *m)
{
	int i, j;

	if (MSG_HAS(m, dns))
		m->ndns = netdb_get_nameservers(m->dns, 4);

	m->nifaces = 0;
	for (i = 0; i < nifaces && m->nifaces < NETCTRL_MAXIFACES; i++) {
		struct iface *ifc = &ifaces[i];
		struct NetCtrlIface *o;
		struct virtif_user *v;

		if (ifc->hidden)	/* a plug-in adapter not plugged in */
			continue;
		o = &m->ifaces[m->nifaces++];
		v = sana_find(ifc->device, ifc->unit);

		memset(o, 0, sizeof(*o));
		sb_copy(o->name, ifc->name, sizeof(o->name));
		sb_copy(o->device, ifc->device, sizeof(o->device));
		o->unit = ifc->unit;
		o->address = ifc->addr;
		o->netmask = ifc->mask;
		o->gateway = ifc->gateway;
		if (v) {
			for (j = 0; j < 6; j++)
				o->mac[j] = sana_macaddr(v)[j];
			if (sana_link_events(v))
				o->flags |= NETIF_LINKEVENTS;
		}
		o->flags |= (ifc->up ? NETIF_UP : 0) |
		    (ifc->admin ? NETIF_ADMIN : 0) |
		    (ifc->link ? NETIF_LINK : 0) |
		    (ifc->dhcp ? NETIF_DHCP : 0) |
		    (ifc->wireless ? NETIF_WIRELESS : 0) |
		    (ifc->attached ? 0 : NETIF_NODRIVER) |
		    (ifc->unverified ? NETIF_UNVERIFIED : 0);
	}
}

static ULONG
lib_opencount(void)
{
	struct Library *lib;
	ULONG n = 0;

	Forbid();
	lib = (struct Library *)FindName(&SysBase->LibList,
	    "bsdsocket.library");
	if (lib)
		n = lib->lib_OpenCnt;
	Permit();
	return n;
}


static void
status_report(struct NetCtrlMsg *m)
{
	struct tb b = { m->text, sizeof(m->text) };
	ULONG ns[4], rx, tx, rxd, txd;
	int i, j, n;

	tb_s(&b, "AmiBSDNet " AMIBSDNET_VERSION " - NetBSD 11 TCP/IP\n\n");
	for (i = 0, n = 0; i < nifaces; i++) {
		struct iface *ifc = &ifaces[i];
		struct virtif_user *v;

		if (ifc->hidden)
			continue;
		n++;
		v = sana_find(ifc->device, ifc->unit);
		tb_s(&b, ifc->name);
		tb_s(&b, ": ");
		tb_s(&b, ifc->device);
		tb_s(&b, " unit ");
		tb_u(&b, ifc->unit);
		tb_s(&b, ifc->unverified ? "  DRIVER FAILED THE CHECK" :
		    !ifc->attached ? "  DRIVER NOT USED (see the log)" :
		    ifc->up ? "  UP" : !ifc->admin ? "  OFFLINE" :
		    !ifc->link ? "  NO LINK" : ifc->dhcp ? "  WAITING FOR DHCP" :
		    "  DOWN");
		if (ifc->dhcp)
			tb_s(&b, " (DHCP)");
		if (ifc->wireless)
			tb_s(&b, " (Wi-Fi)");
		tb_s(&b, "\n");
		if (ifc->addr) {
			tb_s(&b, "  inet ");
			tb_ip(&b, ifc->addr);
			tb_s(&b, "/");
			tb_u(&b, prefixlen(ifc->mask));
			if (ifc->gateway) {
				tb_s(&b, "  gateway ");
				tb_ip(&b, ifc->gateway);
			}
			tb_s(&b, "\n");
		}
		if (v) {
			const UBYTE *mac = sana_macaddr(v);

			tb_s(&b, "  ether ");
			for (j = 0; j < 6; j++) {
				if (j)
					tb_s(&b, ":");
				tb_hex2(&b, mac[j]);
			}
			sana_stats(v, &rx, &tx, &rxd, &txd);
			tb_s(&b, "\n  packets in ");
			tb_u(&b, rx);
			tb_s(&b, ", out ");
			tb_u(&b, tx);
			tb_s(&b, ", dropped ");
			tb_u(&b, rxd + txd);
			tb_s(&b, "\n");
		}
	}
	if (n == 0)
		tb_s(&b, "no network interfaces configured\n");
	n = netdb_get_nameservers(ns, 4);
	tb_s(&b, "\nname servers:");
	for (j = 0; j < n; j++) {
		tb_s(&b, " ");
		tb_ip(&b, ns[j]);
	}
	if (n == 0)
		tb_s(&b, " none");
	if (netdb_get_domain()[0]) {
		tb_s(&b, "\ndomain: ");
		tb_s(&b, netdb_get_domain());
	}
	tb_s(&b, "\nprograms using bsdsocket.library: ");
	tb_u(&b, lib_opencount());
	tb_s(&b, "\n");
}

int
control_init(void)
{

	/* an empty list (exec/lists.h: IsListEmpty() is lh_TailPred
	   pointing at the list itself) */
	reconfig_waiting.lh_Head = (struct Node *)&reconfig_waiting.lh_Tail;
	reconfig_waiting.lh_Tail = NULL;
	reconfig_waiting.lh_TailPred = (struct Node *)&reconfig_waiting;
	if ((ctlport = CreateMsgPort()) == NULL)
		return -1;
	ctlport->mp_Node.ln_Name = (char *)AMIBSDNET_PORTNAME;
	ctlport->mp_Node.ln_Pri = 0;
	AddPort(ctlport);
	return 0;
}

ULONG
control_sigmask(void)
{

	return ctlport ? 1UL << ctlport->mp_SigBit : 0;
}

/* the state fields every reply has, then the reply */
static void
reply(struct NetCtrlMsg *m, LONG result)
{
	ULONG addr;
	int i;

	m->result = result;
	addr = netcfg_primary_address();
	m->online = addr != 0x7f000001UL;
	m->address = m->online ? addr : 0;
	m->opencount = lib_opencount();
	m->link = 0;
	for (i = 0; i < nifaces; i++)
		if (ifaces[i].link && ifaces[i].attached)
			m->link = 1;
	ReplyMsg(&m->msg);
}

/* the reconfiguration is over: answer the RECONFIG requests */
void
control_reconfig_done(int result)
{
	struct Node *n;

	while ((n = RemHead(&reconfig_waiting)) != NULL)
		reply((struct NetCtrlMsg *)n, result);
}

/*
 * The kernel is gone (rumpuser_exit()): nothing may call into it any more.
 * The port leaves the public list, and every message already sent is
 * answered with -1 without the state fields that ask the kernel.
 * amibsdnet_ctl_call() finds the port and puts its message under
 * Forbid() (ctlcall.h), so after RemPort() under Forbid() no message can
 * arrive that is not in the queue now.
 */
void
control_shutdown(void)
{
	struct NetCtrlMsg *m;
	struct Node *n;

	if (ctlport == NULL)
		return;
	Forbid();
	RemPort(ctlport);
	Permit();
	while ((n = RemHead(&reconfig_waiting)) != NULL) {
		m = (struct NetCtrlMsg *)n;
		m->result = -1;
		m->online = 0;
		m->address = 0;
		m->link = 0;
		ReplyMsg(&m->msg);
	}
	while ((m = (struct NetCtrlMsg *)GetMsg(ctlport)) != NULL) {
		if (MSG_HAS(m, link)) {
			m->result = -1;
			m->online = 0;
			m->address = 0;
			m->link = 0;
		}
		ReplyMsg(&m->msg);
	}
}

void
control_handle(void)
{
	struct NetCtrlMsg *m;
	LONG result;

	/* (the kernel stopped meanwhile, on one of its threads: the rest is
	   left in the port for control_shutdown(), which answers without
	   the kernel; the main loop calls it next, src/stack/main.c
	   kernel_gone()) */
	while (!amiga_rump_exited &&
	    (m = (struct NetCtrlMsg *)GetMsg(ctlport)) != NULL) {
		/* too short for the fields every reply has: returned as it
		   came */
		if (!MSG_HAS(m, link)) {
			ReplyMsg(&m->msg);
			continue;
		}
		result = 0;
		switch (m->cmd) {
		case NETCTRL_STATE:
			break;
		case NETCTRL_STATUS:
			if (MSG_HAS(m, text))
				status_report(m);
			else
				result = -1;
			break;
		case NETCTRL_ONLINE:
			stack_online();
			break;
		case NETCTRL_OFFLINE:
			stack_offline();
			break;
		case NETCTRL_RECONFIG:
			/* (answered when it is over: the port is served
			   meanwhile) */
			if ((result = stack_reconfigure_begin()) > 0) {
				AddTail(&reconfig_waiting, &m->msg.mn_Node);
				continue;
			}
			break;
		case NETCTRL_IFLIST:
			if (MSG_HAS(m, ifaces))
				iface_list(m);
			else
				result = -1;
			break;
		/* (tests of what follows a kernel panic: DEBUG only) */
		case NETCTRL_PANIC:
			if (!amiga_rump_debug) {
				result = -1;
				break;
			}
			/* answered first: the panic does not return
			   (src/kern/debugpanic.c), and the main loop that
			   would answer later is not reached again */
			reply(m, 0);
			rump_amibsdnet_panic(0);
			continue;
		case NETCTRL_PANICTHREAD:
			result = amiga_rump_debug ?
			    (rump_amibsdnet_panic(1) == 0 ? 0 : -1) : -1;
			break;
		default:
			result = -1;
			break;
		}
		reply(m, result);
	}
}
