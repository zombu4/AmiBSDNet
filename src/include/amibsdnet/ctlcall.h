/*
 * Sending a NetCtrlMsg to the stack and waiting for the reply - but not
 * for ever: a stack that hangs (inside a driver, say) must not take the
 * status icon, the Settings window or NetCtrl with it.
 *
 * After a timeout the message still belongs to the stack, which may reply
 * much later: the reply port is then left as it is, set to PA_IGNORE (a
 * late reply is queued without signalling a task that may be gone; its
 * signal bit is given back to the task), and
 * neither it nor the message may be freed or used again - except through
 * amibsdnet_ctl_reclaim(), once the reply is there.
 */
#ifndef AMIBSDNET_CTLCALL_H
#define AMIBSDNET_CTLCALL_H

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "control.h"

extern struct DosLibrary *DOSBase;

#define	AMIBSDNET_CTL_TIMEOUT	30	/* seconds */

/* 0: replied; -1: no stack (or no memory); -2: no reply in time (*leftp:
   the reply port, the message is still the stack's).  Without
   timer.device the reply is polled with Delay() (5 ticks, 1/10 s:
   dos.doc Delay), so the caller must be a process. */
static inline int
amibsdnet_ctl_call(struct NetCtrlMsg *m, ULONG cmd, ULONG secs,
    struct MsgPort **leftp)
{
	struct MsgPort *reply, *port, *tport;
	struct timerequest *treq = NULL;
	int got = 0;

	*leftp = NULL;
	if ((reply = CreateMsgPort()) == NULL)
		return -1;
	m->msg.mn_Node.ln_Type = NT_MESSAGE;
	m->msg.mn_ReplyPort = reply;
	m->msg.mn_Length = sizeof(*m);
	m->cmd = cmd;
	m->text[0] = '\0';
	/* nothing left from an earlier call if the stack does not fill
	   them (an older stack, a failed command) */
	m->nifaces = 0;
	m->ndns = 0;
	if ((tport = CreateMsgPort()) != NULL &&
	    (treq = (struct timerequest *)CreateIORequest(tport,
	    sizeof(*treq))) != NULL &&
	    OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
	    (struct IORequest *)treq, 0) != 0) {
		DeleteIORequest((struct IORequest *)treq);
		treq = NULL;
	}
	Forbid();
	port = NULL;
	/* (Delay() below needs a process) */
	if ((treq != NULL || ((struct Task *)FindTask(NULL))->tc_Node.ln_Type ==
	    NT_PROCESS) &&
	    (port = FindPort((CONST_STRPTR)AMIBSDNET_PORTNAME)) != NULL)
		PutMsg(port, &m->msg);
	Permit();
	if (port == NULL) {
		if (treq) {
			CloseDevice((struct IORequest *)treq);
			DeleteIORequest((struct IORequest *)treq);
		}
		if (tport)
			DeleteMsgPort(tport);
		DeleteMsgPort(reply);
		return -1;
	}
	if (treq == NULL) {
		ULONG ticks;

		for (ticks = 0; ticks < secs * 50; ticks += 5) {
			if (GetMsg(reply) != NULL) {
				got = 1;
				break;
			}
			Delay(5);
		}
	} else {
		treq->tr_node.io_Command = TR_ADDREQUEST;
		treq->tr_time.tv_secs = secs;
		treq->tr_time.tv_micro = 0;
		SendIO((struct IORequest *)treq);
		for (;;) {
			if (GetMsg(reply) != NULL) {
				got = 1;
				break;
			}
			if (CheckIO((struct IORequest *)treq))
				break;
			Wait((1UL << reply->mp_SigBit) |
			    (1UL << tport->mp_SigBit));
		}
		if (!CheckIO((struct IORequest *)treq))
			AbortIO((struct IORequest *)treq);
		WaitIO((struct IORequest *)treq);
		CloseDevice((struct IORequest *)treq);
		DeleteIORequest((struct IORequest *)treq);
	}
	if (!got) {
		Forbid();
		if (GetMsg(reply) != NULL)
			got = 1;	/* (just in time) */
		else {
			/* (its signal goes back to this task now: a Shell
			   that runs NetCtrl again and again would run out of
			   them) */
			reply->mp_Flags = PA_IGNORE;
			FreeSignal(reply->mp_SigBit);
			reply->mp_SigBit = (UBYTE)-1;
		}
		Permit();
	}
	if (tport)
		DeleteMsgPort(tport);
	if (!got) {
		*leftp = reply;
		return -2;
	}
	DeleteMsgPort(reply);
	return 0;
}

/* the late reply to a call that timed out: 1 if it is there (the port is
   gone then and the message is free again), 0 if not yet */
static inline int
amibsdnet_ctl_reclaim(struct MsgPort *left)
{
	int got;

	Forbid();
	got = GetMsg(left) != NULL;
	Permit();
	if (got) {
		BYTE sig;

		/* (its signal was freed at the timeout: DeleteMsgPort needs
		   one to free again; without one the port is just left) */
		if ((sig = AllocSignal(-1)) != -1) {
			left->mp_SigBit = (UBYTE)sig;
			left->mp_Flags = PA_SIGNAL;
			DeleteMsgPort(left);
		}
	}
	return got;
}

#endif /* AMIBSDNET_CTLCALL_H */
