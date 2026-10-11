/*
 * The start notice: while the system starts, AmiBSDNet's programs show
 * on screen what they are about to do - one small window each at the
 * bottom of the Workbench screen, brought to the front before each step
 * is shown.  If the Amiga freezes, the window still shows the step that
 * froze it.  The windows close by themselves once the start is over.
 *
 * And the start guard: a program that starts at boot leaves a marker
 * (ENVARC:AmiBSDNet/Starting-<name>) until its start is over.  A marker
 * still there at the next start means that start never finished (the
 * Amiga froze, or was switched off during it): this time the program
 * does not start, once - the marker is deleted, so the next boot is a
 * normal one again.
 *
 * The program defines IntuitionBase and GfxBase (they may be NULL: the
 * notice opens the libraries itself).
 */
#ifndef AMIBSDNET_NOTICE_H
#define AMIBSDNET_NOTICE_H

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;

#define	NOTICE_LINES	5
#define	NOTICE_COLS	100
#define	GUARD_DIR	"ENVARC:AmiBSDNet"
#define	START_SECS	60	/* a start is over after this */

struct notice {
	struct Window *win;
	int slot;			/* 0 lowest on the screen */
	int n;				/* lines kept */
	char line[NOTICE_LINES][NOTICE_COLS];
	char title[60];
};

static inline void
notice_open(struct notice *nt, const char *title, int slot)
{
	struct Screen *scr;
	int i, h, w, top;

	nt->win = NULL;
	nt->slot = slot;
	nt->n = 0;
	for (i = 0; title[i] && i < (int)sizeof(nt->title) - 1; i++)
		nt->title[i] = title[i];
	nt->title[i] = '\0';
	if (IntuitionBase == NULL)
		IntuitionBase = (struct IntuitionBase *)OpenLibrary(
		    "intuition.library", 37);
	if (GfxBase == NULL)
		GfxBase = (struct GfxBase *)OpenLibrary("graphics.library",
		    37);
	if (IntuitionBase == NULL || GfxBase == NULL)
		return;
	if ((scr = LockPubScreen(NULL)) == NULL)
		return;
	w = scr->Width < 640 ? scr->Width : 640;
	h = scr->WBorTop + scr->Font->ta_YSize + 1 +
	    NOTICE_LINES * (scr->Font->ta_YSize + 1) + scr->WBorBottom + 4;
	top = scr->Height - h * (slot + 1);
	if (top < 0)
		top = 0;
	nt->win = OpenWindowTags(NULL,
	    WA_PubScreen, (ULONG)scr,
	    WA_Left, 0, WA_Top, top, WA_Width, w, WA_Height, h,
	    WA_Title, (ULONG)nt->title,
	    WA_DragBar, TRUE, WA_DepthGadget, TRUE,
	    WA_Activate, FALSE, WA_SmartRefresh, TRUE,
	    WA_NoCareRefresh, TRUE, WA_IDCMP, 0,
	    TAG_DONE);
	UnlockPubScreen(NULL, scr);
}

/* show a line (text, len characters; a '\n' ends it): in front first */
static inline void
notice_show(struct notice *nt, const char *text, int len)
{
	struct Window *win = nt->win;
	struct RastPort *rp;
	struct TextExtent te;
	int i, k, y, fh, room;

	if (win == NULL)
		return;
	/* (the same step again, as a repeated query: shown once) */
	if (nt->n > 0) {
		const char *last = nt->line[nt->n - 1];

		for (k = 0; k < len && k < NOTICE_COLS - 1 && text[k] != '\n' &&
		    last[k] == (text[k] >= ' ' ? text[k] : ' '); k++)
			;
		if (!last[k] && (k == len || text[k] == '\n'))
			return;
	}
	if (nt->n == NOTICE_LINES) {
		for (i = 1; i < NOTICE_LINES; i++)
			for (k = 0; k < NOTICE_COLS; k++)
				nt->line[i - 1][k] = nt->line[i][k];
		nt->n--;
	}
	for (k = 0; k < len && k < NOTICE_COLS - 1 && text[k] != '\n'; k++)
		nt->line[nt->n][k] = text[k] >= ' ' ? text[k] : ' ';
	nt->line[nt->n][k] = '\0';
	nt->n++;
	/* in front of whatever opened meanwhile (Workbench's windows),
	   only when something covers it.  Intuition arranges the window
	   "the next time Intuition receives an input event, which happens
	   currently at a minimum rate of ten times per second"
	   (downloads/sources/NDK3.2/Autodocs/intuition.doc:7774-7777,
	   WindowToFront): at most 1/10 s, 5 ticks of Delay() (50 per
	   second, dos.doc:1215, TICKS_PER_SECOND in dos/dos.h) */
	if (win->WLayer && win->WLayer->front) {
		WindowToFront(win);
		Delay(5);
	}
	rp = win->RPort;
	fh = rp->TxHeight + 1;
	room = win->Width - win->BorderLeft - win->BorderRight - 8;
	SetAPen(rp, 0);
	RectFill(rp, win->BorderLeft, win->BorderTop,
	    win->Width - win->BorderRight - 1,
	    win->Height - win->BorderBottom - 1);
	SetAPen(rp, 1);
	SetDrMd(rp, JAM1);
	y = win->BorderTop + 2 + rp->TxBaseline;
	for (i = 0; i < nt->n; i++, y += fh) {
		for (k = 0; nt->line[i][k]; k++)
			;
		k = TextFit(rp, (CONST_STRPTR)nt->line[i], k, &te, NULL, 1,
		    room, fh);
		Move(rp, win->BorderLeft + 4, y);
		Text(rp, (CONST_STRPTR)nt->line[i], k);
	}
}

static inline void
notice_say(struct notice *nt, const char *text)
{
	int n = 0;

	while (text[n])
		n++;
	notice_show(nt, text, n);
}

static inline void
notice_close(struct notice *nt)
{

	if (nt->win) {
		CloseWindow(nt->win);
		nt->win = NULL;
	}
}

/* ------------------------------------------------------------------------
 * the start guard
 */

static inline void
guard_path(char *out, const char *name)
{
	static const char pre[] = GUARD_DIR "/Starting-";
	int i, j;

	for (i = 0; pre[i]; i++)
		out[i] = pre[i];
	for (j = 0; name[j] && j < 20; j++)
		out[i++] = name[j];
	out[i] = '\0';
}

/* the file system writes what it still has in memory (the marker must
   be on the disk before anything can freeze) */
static inline void
guard_flush(const char *path)
{
	struct DevProc *dp;

	if ((dp = GetDeviceProc((CONST_STRPTR)path, NULL)) != NULL) {
		if (dp->dvp_Port)
			DoPkt(dp->dvp_Port, ACTION_FLUSH, 0, 0, 0, 0, 0);
		FreeDeviceProc(dp);
	}
}

/* set by guard_begin() when the marker could not be made (then a freeze
   during this start goes unnoticed), with the DOS error */
static int guard_nomarker __attribute__((unused));
static LONG guard_nomarker_error __attribute__((unused));

/*
 * 1: the last start never finished (this one is skipped; the marker is
 * gone, so the next is normal); 0: go ahead (the marker is made, or
 * guard_nomarker says why not).
 *
 * The marker stays until guard_end(): for a program that starts at boot
 * that is the whole start window (the stack: START_SECS).  A reset or
 * power-off inside that window counts as a freeze, and the next start is
 * skipped once.  A second copy of the program started inside the window
 * would take the marker as a freeze too, so programs that must not run
 * twice check for a running copy before calling this.
 */
static inline int
guard_begin(const char *name)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR old = me->pr_WindowPtr;
	char path[64];
	BPTR l;
	int froze = 0;

	me->pr_WindowPtr = (APTR)-1;
	guard_path(path, name);
	guard_nomarker = 0;
	guard_nomarker_error = 0;
	if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) != 0) {
		UnLock(l);
		DeleteFile((CONST_STRPTR)path);
		froze = 1;
	} else {
		BPTR fh;

		if ((l = CreateDir((CONST_STRPTR)GUARD_DIR)) != 0)
			UnLock(l);
		if ((fh = Open((CONST_STRPTR)path, MODE_NEWFILE)) != 0)
			Close(fh);
		else {
			guard_nomarker = 1;
			guard_nomarker_error = IoErr();
		}
	}
	guard_flush(path);
	me->pr_WindowPtr = old;
	return froze;
}

/* the start is over */
static inline void
guard_end(const char *name)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR old = me->pr_WindowPtr;
	char path[64];

	me->pr_WindowPtr = (APTR)-1;
	guard_path(path, name);
	DeleteFile((CONST_STRPTR)path);
	guard_flush(path);
	me->pr_WindowPtr = old;
}

/* say that name was skipped (ENV:AmiBSDNet/Skipped-<name>): the
   status icon tells, from Workbench - nothing waits at boot */
static inline void
guard_skipped(const char *name)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR old = me->pr_WindowPtr;
	char var[48];
	int i, j;
	static const char pre[] = "AmiBSDNet/Skipped-";

	for (i = 0; pre[i]; i++)
		var[i] = pre[i];
	for (j = 0; name[j] && j < 20; j++)
		var[i++] = name[j];
	var[i] = '\0';
	me->pr_WindowPtr = (APTR)-1;
	{
		BPTR l = CreateDir((CONST_STRPTR)"ENV:AmiBSDNet");

		if (l)
			UnLock(l);
	}
	SetVar((CONST_STRPTR)var, (CONST_STRPTR)"1", -1, GVF_GLOBAL_ONLY);
	me->pr_WindowPtr = old;
}

/* the requester after a skipped start (from the status icon) */
static inline void
guard_tell(const char *what)
{
	struct EasyStruct es;

	if (IntuitionBase == NULL)
		IntuitionBase = (struct IntuitionBase *)OpenLibrary(
		    "intuition.library", 37);
	if (IntuitionBase == NULL)
		return;
	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (UBYTE *)"AmiBSDNet";
	es.es_TextFormat = (UBYTE *)"Not started this time:\n%s\n\n"
	    "Its last start never finished (the Amiga froze,\n"
	    "or was switched off while it was starting).\n\n"
	    "The window at the bottom of the screen showed\n"
	    "what it was doing when it froze.\n\n"
	    "It starts normally again at the next boot.";
	es.es_GadgetFormat = (UBYTE *)"OK";
	EasyRequest(NULL, &es, NULL, (ULONG)what);
}

#endif /* AMIBSDNET_NOTICE_H */
