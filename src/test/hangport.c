/*
 * A stack that hangs: the AmiBSDNet control port, whose messages are
 * never answered (until Ctrl-C).  Checks that NetCtrl and the status
 * icon give up instead of waiting for ever.  Usage: Run hangport
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <amibsdnet/control.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct MsgPort *port;
	struct Message *m, *held[16];
	int n = 0;

	SysBase = *(struct ExecBase **)4;
	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37))
	    == NULL)
		return RETURN_FAIL;
	if ((port = CreateMsgPort()) == NULL)
		return RETURN_FAIL;
	port->mp_Node.ln_Name = (char *)AMIBSDNET_PORTNAME;
	port->mp_Node.ln_Pri = 0;
	AddPort(port);
	PutStr((CONST_STRPTR)"hangport: not answering\n");
	while (!(Wait((1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C) &
	    SIGBREAKF_CTRL_C))
		while (n < 16 && (m = GetMsg(port)) != NULL)
			held[n++] = m;
	/* the late replies */
	RemPort(port);
	while (n < 16 && (m = GetMsg(port)) != NULL)
		held[n++] = m;
	while (n > 0)
		ReplyMsg(held[--n]);
	DeleteMsgPort(port);
	PutStr((CONST_STRPTR)"hangport: replied\n");
	CloseLibrary((struct Library *)DOSBase);
	return RETURN_OK;
}
