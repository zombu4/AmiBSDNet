/*
 * bsdsocket.library: network monitoring hooks (AddNetMonitorHookTagList(),
 * RemoveNetMonitorHook()).
 *
 * "the doc" is downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc,
 * "the SDK header" .../netinclude/libraries/bsdsocket.h.
 *
 * The hook types that watch calls of the library itself are supported:
 * MHT_Connect, MHT_Send and MHT_Bind.  They are "invoked before dropping
 * into the kernel" call, in the context of the caller, so that the hook may
 * use bsdsocket.library (the doc).  MHT_ICMP, MHT_UDP, MHT_TCP_Connect and
 * MHT_Packet are invoked "from withing the TCP/IP stack itself": that
 * needs a hook in the NetBSD packet path, which this library does not have
 * (the kernel components are listed in tools/build.py); the doc's
 * [EINVAL] "The monitor type is not supported" applies to them.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "sblib.h"

extern struct UtilityBase *UtilityBase;

/* the SDK header: types of monitoring hooks */
#define	MHT_ICMP	0
#define	MHT_UDP		1
#define	MHT_TCP_Connect	2
#define	MHT_Connect	3
#define	MHT_Send	4
#define	MHT_Packet	5
#define	MHT_Bind	6

/* the SDK header: the messages the hooks get */
struct ConnectMonitorMsg {
	LONG cmm_Size;
	STRPTR cmm_Caller;
	LONG cmm_Socket;
	struct sockaddr *cmm_Name;
	LONG cmm_NameLen;
};

struct BindMonitorMsg {
	LONG bmm_Size;
	STRPTR bmm_Caller;
	LONG bmm_Socket;
	struct sockaddr *bmm_Name;
	LONG bmm_NameLen;
};

struct SendMonitorMessage {
	LONG smm_Size;
	STRPTR smm_Caller;
	LONG smm_Socket;
	APTR smm_Buffer;
	LONG smm_Len;
	LONG smm_Flags;
	struct sockaddr *smm_To;
	LONG smm_ToLen;
	struct msghdr *smm_Msg;
};

/* a call of a hook under way (on the caller's stack, under monlock) */
struct moncall {
	struct moncall *next;
	struct Task *task;
};

struct monhook {
	struct monhook *next;
	ULONG serial;		/* order of installation */
	LONG type;
	struct Hook *hook;
	struct SocketBase *owner;
	struct moncall *calls;	/* the calls of the hook under way */
	int gone;		/* removed: the last call frees it */
};

static struct monhook *hooks;
static ULONG hookserial;
static struct SignalSemaphore monlock;

void
mon_init(void)
{

	InitSemaphore(&monlock);
}

LONG
sb_AddNetMonitorHookTagList(struct SocketBase *sb, LONG type,
    struct Hook *hook, struct TagItem *tags)
{
	struct monhook *h, **hp;

	if (hook == NULL) {
		sb_set_errno(sb, EFAULT);
		return -1;
	}
	if (type != MHT_Connect && type != MHT_Send && type != MHT_Bind) {
		sb_set_errno(sb, EINVAL);
		return -1;
	}
	if ((h = AllocVec(sizeof(*h), MEMF_PUBLIC | MEMF_CLEAR)) == NULL) {
		sb_set_errno(sb, ENOMEM);
		return -1;
	}
	h->type = type;
	h->hook = hook;
	h->owner = sb;
	ObtainSemaphore(&monlock);
	h->serial = ++hookserial;
	for (hp = &hooks; *hp; hp = &(*hp)->next)
		;
	*hp = h;
	ReleaseSemaphore(&monlock);
	return 0;
}

/* unlink what 'match' selects, wait for hook calls under way (one that
   removes its own hook from within does not wait for itself), free */
static void
remove_hooks(struct Hook *hook, struct SocketBase *owner)
{
	struct monhook *h, **hp, *gone = NULL;
	struct Task *me = FindTask(NULL);

	ObtainSemaphore(&monlock);
	for (hp = &hooks; (h = *hp) != NULL;) {
		if ((hook != NULL && h->hook == hook) ||
		    (owner != NULL && h->owner == owner)) {
			*hp = h->next;
			h->next = gone;
			gone = h;
		} else
			hp = &h->next;
	}
	ReleaseSemaphore(&monlock);
	while ((h = gone) != NULL) {
		gone = h->next;
		/* (calls by this task are the ones it is in: not waited
		   for; if there are any, the last of them frees it, in
		   mon_run()) */
		for (;;) {
			struct moncall *c;
			int others = 0;

			ObtainSemaphore(&monlock);
			for (c = h->calls; c; c = c->next)
				if (c->task != me)
					others = 1;
			if (!others) {
				h->gone = 1;
				c = h->calls;
				ReleaseSemaphore(&monlock);
				if (c == NULL)
					FreeVec(h);
				break;
			}
			ReleaseSemaphore(&monlock);
			Delay(1);
		}
	}
}

void
sb_RemoveNetMonitorHook(struct SocketBase *sb, struct Hook *hook)
{

	if (hook != NULL)
		remove_hooks(hook, NULL);
}

/* the monitoring hooks this base installed point into its program */
void
mon_base_closed(struct SocketBase *sb)
{

	remove_hooks(NULL, sb);
}

/*
 * Call the hooks of a type, in the order they were installed, with the
 * message; the first error > 0 ends the call ("any error value > 0 will
 * cause the call to be aborted and the errno variable to be set to this
 * value", the doc).  The hooks are called without the lock held: they may
 * call into the library.
 */
static LONG
mon_run(LONG type, void *msg)
{
	struct monhook *h;
	struct moncall c, **cp;
	ULONG last = 0;
	LONG e;

	c.task = FindTask(NULL);
	for (;;) {
		ObtainSemaphore(&monlock);
		for (h = hooks; h; h = h->next)
			if (h->type == type && h->serial > last)
				break;
		if (h == NULL) {
			ReleaseSemaphore(&monlock);
			return 0;
		}
		last = h->serial;
		c.next = h->calls;
		h->calls = &c;
		ReleaseSemaphore(&monlock);
		e = (LONG)CallHookPkt(h->hook, NULL, msg);
		/* (h is still there: remove_hooks() waits for this call, or,
		   if this task removed it from inside the hook, sets h->gone
		   and leaves the free to the last call of it, this one if no
		   other is under way) */
		ObtainSemaphore(&monlock);
		for (cp = &h->calls; *cp && *cp != &c; cp = &(*cp)->next)
			;
		if (*cp)
			*cp = c.next;
		if (h->gone && h->calls == NULL) {
			ReleaseSemaphore(&monlock);
			FreeVec(h);
		} else
			ReleaseSemaphore(&monlock);
		if (e > 0)
			return e;
	}
}

LONG
mon_connect(struct SocketBase *sb, LONG fd, struct sockaddr *name, LONG len)
{
	struct ConnectMonitorMsg m;

	if (hooks == NULL)
		return 0;
	m.cmm_Size = sizeof(m);
	m.cmm_Caller = sb->logtag;
	m.cmm_Socket = fd;
	m.cmm_Name = name;
	m.cmm_NameLen = len;
	return mon_run(MHT_Connect, &m);
}

LONG
mon_bind(struct SocketBase *sb, LONG fd, struct sockaddr *name, LONG len)
{
	struct BindMonitorMsg m;

	if (hooks == NULL)
		return 0;
	m.bmm_Size = sizeof(m);
	m.bmm_Caller = sb->logtag;
	m.bmm_Socket = fd;
	m.bmm_Name = name;
	m.bmm_NameLen = len;
	return mon_run(MHT_Bind, &m);
}

LONG
mon_send(struct SocketBase *sb, LONG fd, APTR buf, LONG len, LONG flags,
    struct sockaddr *to, LONG tolen, struct msghdr *msg)
{
	struct SendMonitorMessage m;

	if (hooks == NULL)
		return 0;
	m.smm_Size = sizeof(m);
	m.smm_Caller = sb->logtag;
	m.smm_Socket = fd;
	m.smm_Buffer = buf;
	m.smm_Len = len;
	m.smm_Flags = flags;
	m.smm_To = to;
	m.smm_ToLen = tolen;
	m.smm_Msg = msg;
	return mon_run(MHT_Send, &m);
}
