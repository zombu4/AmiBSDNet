/*
 * SerialShell: an AmigaDOS Shell on the serial port.
 *
 *   SerialShell [START|STOP|STATUS|AUTO] [BAUD=<n>] [UNIT=<n>]
 *               [DEVICE=<name>]
 *
 * START runs the handler ("Run >NIL: C:SerialShell START"): it opens the
 * serial device with 8 data bits, no parity, 1 stop bit and neither
 * XON/XOFF nor RTS/CTS handshaking (default 19200 baud, serial.device
 * unit 0), makes the DOS device SERSH: and keeps a Shell on it - a new
 * one after EndShell.  The port's settings are its own: the system's
 * Serial preferences are not changed.
 *
 * AUTO does the same if ENV:AmiBSDNet/SerialShell is set (its value is
 * the baud rate), else nothing: AmiBSDNet's line in S:User-Startup.  The
 * Settings window of the status icon sets the variable.  STOP ends the
 * handler (the Shell gets end-of-file); STATUS says whether it runs.
 *
 * On the terminal: line editing with Backspace/Delete and Ctrl-X (line),
 * Ctrl-C breaks the running command, Ctrl-\ is end-of-file.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <devices/serial.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <amibsdnet/notice.h>

struct ExecBase *SysBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct DosLibrary *DOSBase;

static const char verstag[] __attribute__((used)) =
    "\0$VER: SerialShell 0.8.2 (10.10.2026)";

#define	PORTNAME	"AmiBSDNet.SerialShell"
#define	DEVNAME		"SERSH"
#define	VARNAME		"AmiBSDNet/SerialShell"
#define	LINEMAX		512
#define	RAWMAX		1024

void *
memset(void *d, int c, unsigned long n)
{
	char *p = d;

	while (n--)
		*p++ = c;
	return d;
}

static int
streq(const char *a, const char *b)
{
	int ca, cb;

	do {
		ca = (UBYTE)*a++;
		cb = (UBYTE)*b++;
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
	} while (ca && ca == cb);
	return ca == cb;
}

static void
putnum(ULONG v)
{
	char b[12];
	int i = 11;

	b[i] = '\0';
	do {
		b[--i] = '0' + v % 10;
		v /= 10;
	} while (v);
	PutStr((CONST_STRPTR)b + i);
}

/* ------------------------------------------------------------------------
 * the handler
 */

struct pkt {
	struct pkt *next;
	struct DosPacket *dp;
};

static struct MsgPort *dosport, *serport, *timeport, *ctlport;
static struct IOExtSer *rd, *wr;
static struct timerequest *tr;
static struct DosList *dosentry;
static UBYTE rdbyte;
static int rdpending, trpending;

static volatile int opencount;	/* open SERSH: handles */
static int raw;			/* SetMode(): raw, no echo, no editing */
static volatile int stopping;
/* who gets Ctrl-C: the port of the process reading (or the one
   ACTION_CHANGE_SIGNAL named); forgotten when that process closes its
   handle, so a process that is gone is never signalled */
static struct MsgPort *breakport;
static struct Task *handlertask;

static char line[LINEMAX];	/* cooked: the line being typed */
static int linelen;
static char ready[LINEMAX + 1];	/* cooked: finished lines, for READs */
static int readylen, eofpending, lastcr, inesc;
static UBYTE rawbuf[RAWMAX];	/* raw: bytes for READs */
static int rawlen;

static struct pkt *reads, **readtail = &reads;
static struct DosPacket *waitpkt;	/* a WaitForChar() */

static volatile int spawner_running;

/* serial output; '\n' becomes CR LF for the terminal */
static void
ser_write(const char *s, LONG n, int translate)
{
	char buf[256];
	LONG i = 0, k;

	while (i < n) {
		for (k = 0; i < n && k < (LONG)sizeof(buf) - 2; i++) {
			UBYTE c = s[i];

			if (translate) {
				/* for a PC terminal: LF -> CR LF, the Amiga's
				   one-byte CSI -> ESC [, no font shifts */
				if (c == '\n')
					buf[k++] = '\r';
				else if (c == 0x9b) {
					buf[k++] = 0x1b;
					c = '[';
				} else if (c == 0x0e || c == 0x0f)
					continue;
			}
			buf[k++] = c;
		}
		wr->IOSer.io_Command = CMD_WRITE;
		wr->IOSer.io_Data = buf;
		wr->IOSer.io_Length = k;
		DoIO((struct IORequest *)wr);
	}
}

static void
ser_puts(const char *s)
{
	LONG n = 0;

	while (s[n])
		n++;
	ser_write(s, n, 0);
}

static void
start_read(void)
{

	if (rdpending || stopping)
		return;
	rd->IOSer.io_Command = CMD_READ;
	rd->IOSer.io_Data = &rdbyte;
	rd->IOSer.io_Length = 1;
	SendIO((struct IORequest *)rd);
	rdpending = 1;
}

static int
input_available(void)
{

	return raw ? rawlen > 0 : (readylen > 0 || eofpending);
}

/* answer READs from what is there */
static void
serve_reads(void)
{
	while (reads && (input_available() || stopping)) {
		struct pkt *p = reads;
		struct DosPacket *dp = p->dp;
		UBYTE *dst = (UBYTE *)dp->dp_Arg2;
		LONG want = dp->dp_Arg3, n = 0;

		if (stopping && !input_available()) {
			n = 0;			/* end of file */
		} else if (raw) {
			n = rawlen < want ? rawlen : want;
			CopyMem(rawbuf, dst, n);
			rawlen -= n;
			if (rawlen)
				CopyMem(rawbuf + n, rawbuf, rawlen);
		} else if (readylen > 0) {
			n = readylen < want ? readylen : want;
			CopyMem(ready, dst, n);
			readylen -= n;
			if (readylen)
				CopyMem(ready + n, ready, readylen);
		} else {
			eofpending = 0;		/* Ctrl-\: one end-of-file */
			n = 0;
		}
		reads = p->next;
		if (reads == NULL)
			readtail = &reads;
		FreeVec(p);
		ReplyPkt(dp, n, 0);
	}
	if (waitpkt && input_available()) {
		if (trpending) {
			AbortIO((struct IORequest *)tr);
			WaitIO((struct IORequest *)tr);
			trpending = 0;
		}
		ReplyPkt(waitpkt, DOSTRUE, 0);
		waitpkt = NULL;
	}
}

/* Ctrl-C to the reading process (a port that signals a task) */
static void
send_break(void)
{

	if (breakport && (breakport->mp_Flags & PF_ACTION) == PA_SIGNAL &&
	    breakport->mp_SigTask)
		Signal((struct Task *)breakport->mp_SigTask, SIGBREAKF_CTRL_C);
}

/* a byte from the terminal */
static void
got_byte(UBYTE c)
{

	if (c == 3) {			/* Ctrl-C: break, also in raw mode */
		send_break();
		if (!raw) {
			ser_puts("^C\r\n");
			linelen = 0;
		}
		return;
	}
	if (raw) {
		if (rawlen < RAWMAX)
			rawbuf[rawlen++] = c;
		return;
	}
	if (c == '\n' && lastcr) {	/* CR LF from the terminal */
		lastcr = 0;
		return;
	}
	lastcr = c == '\r';
	/* terminal escape sequences (arrow keys: ESC [ A ...) are not
	   typed text: skipped up to their final letter */
	if (inesc) {
		if (inesc == 1 && (c == '[' || c == 'O'))
			inesc = 2;
		else if (inesc == 1 || (c >= 0x40 && c <= 0x7e))
			inesc = 0;
		return;
	}
	if (c == 27) {
		inesc = 1;
		return;
	}
	switch (c) {
	case '\r':
	case '\n':
		ser_puts("\r\n");
		if (readylen + linelen + 1 <= (int)sizeof(ready)) {
			CopyMem(line, ready + readylen, linelen);
			readylen += linelen;
			ready[readylen++] = '\n';
		}
		linelen = 0;
		break;
	case 8:
	case 127:
		if (linelen > 0) {
			linelen--;
			ser_puts("\b \b");
		}
		break;
	case 24:			/* Ctrl-X: the whole line */
		while (linelen > 0) {
			linelen--;
			ser_puts("\b \b");
		}
		break;
	case 28:			/* Ctrl-\: end of file */
		eofpending = 1;
		break;
	default:
		if (c >= 32 && c != 155 && linelen < LINEMAX - 1) {
			line[linelen++] = c;
			ser_write((char *)&c, 1, 0);
		}
		break;
	}
}

/* everything the serial device has, then the next read */
static void
serial_done(void)
{
	UBYTE buf[64];
	LONG n, i;

	WaitIO((struct IORequest *)rd);
	rdpending = 0;
	if (rd->IOSer.io_Error == 0 && rd->IOSer.io_Actual == 1)
		got_byte(rdbyte);
	for (;;) {
		wr->IOSer.io_Command = SDCMD_QUERY;
		if (DoIO((struct IORequest *)wr) != 0 ||
		    (n = wr->IOSer.io_Actual) == 0)
			break;
		if (n > (LONG)sizeof(buf))
			n = sizeof(buf);
		rd->IOSer.io_Command = CMD_READ;
		rd->IOSer.io_Data = buf;
		rd->IOSer.io_Length = n;
		if (DoIO((struct IORequest *)rd) != 0)
			break;
		for (i = 0; i < (LONG)rd->IOSer.io_Actual; i++)
			got_byte(buf[i]);
	}
	start_read();
	serve_reads();
}

static void
packet(struct DosPacket *dp)
{
	struct FileHandle *fh;
	struct pkt *p;

	switch (dp->dp_Type) {
	case ACTION_FINDINPUT:
	case ACTION_FINDOUTPUT:
	case ACTION_FINDUPDATE:
		if (stopping) {
			ReplyPkt(dp, DOSFALSE, ERROR_OBJECT_IN_USE);
			return;
		}
		fh = (struct FileHandle *)BADDR(dp->dp_Arg1);
		fh->fh_Arg1 = 1;
		fh->fh_Port = (struct MsgPort *)DOSTRUE;	/* interactive */
		opencount++;
		ReplyPkt(dp, DOSTRUE, 0);
		return;
	case ACTION_END:
		if (opencount > 0)
			opencount--;
		/* the break target closes: it may be gone soon */
		if (dp->dp_Port == breakport)
			breakport = NULL;
		ReplyPkt(dp, DOSTRUE, 0);
		return;
	case ACTION_READ:
		if (!breakport)
			breakport = dp->dp_Port;
		if ((p = AllocVec(sizeof(*p), MEMF_ANY)) == NULL) {
			ReplyPkt(dp, -1, ERROR_NO_FREE_STORE);
			return;
		}
		p->next = NULL;
		p->dp = dp;
		*readtail = p;
		readtail = &p->next;
		serve_reads();
		return;
	case ACTION_WRITE:
		/* (the Ctrl-C target comes from READs only: a background job
		   that writes must not take it from the Shell) */
		ser_write((const char *)dp->dp_Arg2, dp->dp_Arg3, !raw);
		ReplyPkt(dp, dp->dp_Arg3, 0);
		return;
	case ACTION_SCREEN_MODE:
		raw = dp->dp_Arg1 != 0;
		ReplyPkt(dp, DOSTRUE, 0);
		return;
	case ACTION_CHANGE_SIGNAL: {
		/* Res2 is the old one, so a program can put it back */
		struct MsgPort *old = breakport;

		breakport = (struct MsgPort *)dp->dp_Arg2;
		ReplyPkt(dp, DOSTRUE, (LONG)old);
		return;
	}
	case ACTION_WAIT_CHAR:
		if (input_available() || stopping || waitpkt) {
			ReplyPkt(dp, input_available() ? DOSTRUE : DOSFALSE, 0);
			return;
		}
		waitpkt = dp;
		tr->tr_node.io_Command = TR_ADDREQUEST;
		tr->tr_time.tv_secs = (ULONG)dp->dp_Arg1 / 1000000;
		tr->tr_time.tv_micro = (ULONG)dp->dp_Arg1 % 1000000;
		SendIO((struct IORequest *)tr);
		trpending = 1;
		return;
	case ACTION_DISK_INFO: {
		/* (programs that ask a console for its window get none) */
		struct InfoData *id = (struct InfoData *)BADDR(dp->dp_Arg1);

		if (id)
			memset(id, 0, sizeof(*id));
		ReplyPkt(dp, DOSTRUE, 0);
		return;
	}
	case ACTION_IS_FILESYSTEM:
		ReplyPkt(dp, DOSFALSE, 0);
		return;
	default:
		ReplyPkt(dp, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
		return;
	}
}

/* a process that opens SERSH: and starts the Shell on it (the handler
   itself cannot: the opens are packets to the handler) */
static void
spawner(void)
{
	BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
	BPTR out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
	int i;

	/* as the Workbench Shell icon does: NewShell with SERSH: as its
	   "window" (it opens SERSH: itself, for input and output) */
	if (SystemTags((CONST_STRPTR)"NewShell " DEVNAME ":", SYS_Input, in,
	    SYS_Output, out, SYS_Asynch, TRUE, NP_Name,
	    (ULONG)"SerialShell starter", TAG_DONE) != 0) {
		if (in)
			Close(in);
		if (out)
			Close(out);
	} else
		/* done once the Shell has SERSH: open (so the handler does
		   not start a second one meanwhile) */
		for (i = 0; i < 100 && opencount == 0 && !stopping; i++)
			Delay(5);
	Forbid();		/* lasts until this process is gone */
	spawner_running = 0;
	/* wake the handler: it may be waiting for nothing else (a STOP
	   while the Shell was starting, or a failed start) */
	Signal(handlertask, SIGBREAKF_CTRL_F);
}

static int
spawn_shell(void)
{
	static int before;

	if (before++)
		ser_puts("\r\n");	/* after "Process n ending" */
	spawner_running = 1;
	breakport = NULL;
	raw = 0;
	linelen = readylen = rawlen = eofpending = 0;
	if (CreateNewProcTags(NP_Entry, (ULONG)spawner,
	    NP_Name, (ULONG)"SerialShell starter", NP_StackSize, 8192,
	    TAG_DONE) == NULL) {
		spawner_running = 0;
		return -1;
	}
	return 0;
}

static void
remove_dosentry(void)
{

	if (dosentry) {
		LockDosList(LDF_DEVICES | LDF_WRITE);
		RemDosEntry(dosentry);
		UnLockDosList(LDF_DEVICES | LDF_WRITE);
		FreeDosEntry(dosentry);
		dosentry = NULL;
	}
}

/* the start notice, at boot (AUTO) only: each step is shown first */
static struct notice snotice;
static int atboot;

static void
step(const char *what)
{

	if (atboot)
		notice_say(&snotice, what);
}

static int
handler(const char *device, ULONG unit, ULONG baud)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR oldwin;
	ULONG dosmask, sermask, timemask, sigs;
	int rv = RETURN_FAIL, opened = 0, timeropen = 0, gtropen = 0;
	int gtrpending = 0;
	struct timerequest *gtr = NULL;	/* the end of the start */

	oldwin = me->pr_WindowPtr;
	me->pr_WindowPtr = (APTR)-1;
	handlertask = SysBase->ThisTask;
	if ((dosport = CreateMsgPort()) == NULL ||
	    (serport = CreateMsgPort()) == NULL ||
	    (timeport = CreateMsgPort()) == NULL ||
	    (ctlport = CreateMsgPort()) == NULL)
		goto out;
	if ((rd = (struct IOExtSer *)CreateIORequest(serport,
	    sizeof(*rd))) == NULL ||
	    (wr = (struct IOExtSer *)CreateIORequest(serport,
	    sizeof(*wr))) == NULL ||
	    (tr = (struct timerequest *)CreateIORequest(timeport,
	    sizeof(*tr))) == NULL)
		goto out;
	/* exclusive, no handshaking (flags that count at open time) */
	rd->io_SerFlags = SERF_XDISABLED;
	step("opening the serial port");
	if (OpenDevice((CONST_STRPTR)device, unit, (struct IORequest *)rd,
	    0) != 0) {
		PutStr((CONST_STRPTR)"SerialShell: cannot open ");
		PutStr((CONST_STRPTR)device);
		PutStr((CONST_STRPTR)" (in use?)\n");
		goto out;
	}
	opened = 1;
	rd->io_Baud = baud;
	rd->io_ReadLen = 8;
	rd->io_WriteLen = 8;
	rd->io_StopBits = 1;
	rd->io_RBufLen = 4096;
	rd->io_SerFlags = SERF_XDISABLED;	/* no parity, no 7-wire */
	rd->io_ExtFlags = 0;
	rd->IOSer.io_Command = SDCMD_SETPARAMS;
	step("setting the speed (8N1, no handshaking)");
	if (DoIO((struct IORequest *)rd) != 0) {
		PutStr((CONST_STRPTR)"SerialShell: the serial device refuses "
		    "these settings\n");
		goto out;
	}
	/* the write request: a copy (same unit), its own reply port */
	CopyMem(rd, wr, sizeof(*wr));
	wr->IOSer.io_Message.mn_ReplyPort = serport;
	if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
	    (struct IORequest *)tr, 0) != 0)
		goto out;
	timeropen = 1;

	if (atboot && (gtr = (struct timerequest *)CreateIORequest(timeport,
	    sizeof(*gtr))) != NULL && OpenDevice((CONST_STRPTR)TIMERNAME,
	    UNIT_VBLANK, (struct IORequest *)gtr, 0) == 0) {
		gtropen = 1;
		gtr->tr_node.io_Command = TR_ADDREQUEST;
		gtr->tr_time.tv_secs = START_SECS;
		gtr->tr_time.tv_micro = 0;
		SendIO((struct IORequest *)gtr);
		gtrpending = 1;
	} else if (atboot) {
		/* (no timer for the end of the start: over now) */
		notice_close(&snotice);
		guard_end("Serial");
		atboot = 0;
	}

	/* SERSH: */
	step("adding SERSH:");
	if ((dosentry = MakeDosEntry((CONST_STRPTR)DEVNAME, DLT_DEVICE)) ==
	    NULL)
		goto out;
	dosentry->dol_Task = dosport;
	if (!AddDosEntry(dosentry)) {
		FreeDosEntry(dosentry);
		dosentry = NULL;
		PutStr((CONST_STRPTR)"SerialShell: " DEVNAME ": exists "
		    "already\n");
		goto out;
	}
	/* for STOP and STATUS */
	ctlport->mp_Node.ln_Name = (char *)PORTNAME;
	ctlport->mp_Node.ln_Pri = 0;
	AddPort(ctlport);

	dosmask = 1UL << dosport->mp_SigBit;
	sermask = 1UL << serport->mp_SigBit;
	timemask = 1UL << timeport->mp_SigBit;
	step("reading the serial port");
	start_read();
	step("starting the Shell (NewShell SERSH:)");
	spawn_shell();
	step("running");
	rv = RETURN_OK;

	for (;;) {
		struct Message *m;

		sigs = Wait(dosmask | sermask | timemask | SIGBREAKF_CTRL_C |
		    SIGBREAKF_CTRL_F);
		if ((sigs & SIGBREAKF_CTRL_C) && !stopping) {
			/* STOP: the Shell gets end-of-file and goes */
			stopping = 1;
			/* no new opens from now on (open handles keep
			   working: their packets come to dosport directly) */
			remove_dosentry();
			send_break();
			if (waitpkt) {		/* WaitForChar(): no more input */
				if (trpending) {
					AbortIO((struct IORequest *)tr);
					WaitIO((struct IORequest *)tr);
					trpending = 0;
				}
				ReplyPkt(waitpkt, DOSFALSE, 0);
				waitpkt = NULL;
			}
			if (rdpending) {
				AbortIO((struct IORequest *)rd);
				WaitIO((struct IORequest *)rd);
				rdpending = 0;
			}
			serve_reads();
		}
		while ((m = GetMsg(dosport)) != NULL)
			packet((struct DosPacket *)m->mn_Node.ln_Name);
		if (rdpending && CheckIO((struct IORequest *)rd))
			serial_done();
		if (gtrpending && CheckIO((struct IORequest *)gtr)) {
			/* the start is over */
			WaitIO((struct IORequest *)gtr);
			gtrpending = 0;
			notice_close(&snotice);
			guard_end("Serial");
		}
		if (trpending && CheckIO((struct IORequest *)tr)) {
			WaitIO((struct IORequest *)tr);
			trpending = 0;
			if (waitpkt) {
				ReplyPkt(waitpkt, DOSFALSE, 0);
				waitpkt = NULL;
			}
		}
		if (opencount == 0 && !spawner_running) {
			if (stopping)
				break;
			/* the Shell ended (EndShell): a new one after a
			   second (never a tight loop if it cannot start) */
			Delay(50);
			if (spawn_shell() != 0)
				Delay(250);
		}
	}
	/* an open that looked SERSH: up just before it went: answered,
	   not left in a port that is about to be deleted */
	Delay(10);
	{
		struct Message *m;

		while ((m = GetMsg(dosport)) != NULL)
			ReplyPkt((struct DosPacket *)m->mn_Node.ln_Name, DOSFALSE,
			    ERROR_OBJECT_IN_USE);
	}

out:
	remove_dosentry();
	if (rdpending) {
		AbortIO((struct IORequest *)rd);
		WaitIO((struct IORequest *)rd);
	}
	if (trpending) {
		AbortIO((struct IORequest *)tr);
		WaitIO((struct IORequest *)tr);
	}
	if (gtrpending) {
		AbortIO((struct IORequest *)gtr);
		WaitIO((struct IORequest *)gtr);
	}
	if (gtropen)
		CloseDevice((struct IORequest *)gtr);
	if (gtr)
		DeleteIORequest((struct IORequest *)gtr);
	/* (ended or failed during the start: not a freeze) */
	if (atboot) {
		notice_close(&snotice);
		guard_end("Serial");
	}
	if (timeropen)
		CloseDevice((struct IORequest *)tr);
	if (opened)
		CloseDevice((struct IORequest *)rd);
	if (tr)
		DeleteIORequest((struct IORequest *)tr);
	if (wr)
		DeleteIORequest((struct IORequest *)wr);
	if (rd)
		DeleteIORequest((struct IORequest *)rd);
	if (timeport)
		DeleteMsgPort(timeport);
	if (serport)
		DeleteMsgPort(serport);
	if (dosport)
		DeleteMsgPort(dosport);
	/* last: until now STATUS/START see it running (serial.device is
	   exclusive, SERSH: is taken) */
	if (ctlport) {
		if (ctlport->mp_Node.ln_Name)
			RemPort(ctlport);
		DeleteMsgPort(ctlport);
	}
	me->pr_WindowPtr = oldwin;
	return rv;
}

/* ------------------------------------------------------------------------ */

static struct Task *
running(void)
{
	struct MsgPort *p;
	struct Task *t = NULL;

	Forbid();
	if ((p = FindPort((CONST_STRPTR)PORTNAME)) != NULL)
		t = p->mp_SigTask;
	Permit();
	return t;
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct RDArgs *rda;
	LONG arg[4] = { 0, 0, 0, 0 };
	char cmd[16], dev[64], var[16];
	ULONG baud = 19200, unit = 0;
	int rc = RETURN_OK, i;
	struct Task *t;

	SysBase = *(struct ExecBase **)4;
	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37)) ==
	    NULL)
		return RETURN_FAIL;
	if ((rda = ReadArgs((CONST_STRPTR)"COMMAND,BAUD/K/N,UNIT/K/N,DEVICE/K",
	    arg, NULL)) == NULL) {
		PrintFault(IoErr(), (CONST_STRPTR)"SerialShell");
		CloseLibrary((struct Library *)DOSBase);
		return RETURN_ERROR;
	}
	cmd[0] = '\0';
	if (arg[0])
		for (i = 0; ((char *)arg[0])[i] && i < 15; i++)
			cmd[i] = ((char *)arg[0])[i], cmd[i + 1] = '\0';
	if (arg[1])
		baud = *(LONG *)arg[1];
	if (arg[2])
		unit = *(LONG *)arg[2];
	for (i = 0; i < 63; i++) {
		const char *s = arg[3] ? (const char *)arg[3] : "serial.device";

		if (!(dev[i] = s[i]))
			break;
	}
	dev[63] = '\0';
	FreeArgs(rda);

	if (cmd[0] == '\0' || streq(cmd, "STATUS")) {
		PutStr((CONST_STRPTR)(running() ? "SerialShell is running\n" :
		    "SerialShell is not running\n"));
		goto done;
	}
	if (streq(cmd, "STOP")) {
		/* (signalled under Forbid: it may be ending meanwhile) */
		Forbid();
		if ((t = running()) != NULL)
			Signal(t, SIGBREAKF_CTRL_C);
		Permit();
		if (t == NULL) {
			PutStr((CONST_STRPTR)"SerialShell is not running\n");
			goto done;
		}
		for (i = 0; i < 25 && running(); i++)
			Delay(10);	/* up to 5 s */
		if (running()) {
			PutStr((CONST_STRPTR)"SerialShell: the Shell is still "
			    "busy; it stops when its command ends\n");
			rc = RETURN_WARN;
		}
		goto done;
	}
	if (streq(cmd, "AUTO")) {
		LONG n = GetVar((CONST_STRPTR)VARNAME, (STRPTR)var, sizeof(var),
		    GVF_GLOBAL_ONLY);

		if (n <= 0)
			goto done;	/* switched off */
		for (baud = 0, i = 0; var[i] >= '0' && var[i] <= '9'; i++)
			baud = baud * 10 + (var[i] - '0');
		if (baud == 0)
			baud = 19200;
		/* (one running already: its start, its marker) */
		if (running())
			goto done;
		/* its last start froze the Amiga: not this time */
		if (guard_begin("Serial")) {
			guard_skipped("Serial");
			goto done;
		}
		atboot = 1;
		notice_open(&snotice, "SerialShell is starting (this window "
		    "closes by itself)", 1);
	} else if (!streq(cmd, "START")) {
		PutStr((CONST_STRPTR)"usage: SerialShell [START|STOP|STATUS|"
		    "AUTO] [BAUD=<n>] [UNIT=<n>] [DEVICE=<name>]\n");
		rc = RETURN_ERROR;
		goto done;
	}
	if (running()) {
		PutStr((CONST_STRPTR)"SerialShell is running already\n");
		goto done;
	}
	if (baud < 110 || baud > 1000000) {
		PutStr((CONST_STRPTR)"SerialShell: bad baud rate\n");
		rc = RETURN_ERROR;
		goto done;
	}
	PutStr((CONST_STRPTR)"SerialShell: ");
	PutStr((CONST_STRPTR)dev);
	PutStr((CONST_STRPTR)" unit ");
	putnum(unit);
	PutStr((CONST_STRPTR)", ");
	putnum(baud);
	PutStr((CONST_STRPTR)" baud, 8N1, no handshaking\n");
	rc = handler(dev, unit, baud);
done:
	if (atboot) {
		/* (also when it did not get as far as the handler) */
		notice_close(&snotice);
		guard_end("Serial");
	}
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	CloseLibrary((struct Library *)DOSBase);
	return rc;
}
