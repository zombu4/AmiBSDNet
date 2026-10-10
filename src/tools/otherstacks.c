/*
 * Other TCP/IP stacks (Roadshow, Miami, AmiTCP, Genesis): only one stack
 * may run, so the installer offers to disable or remove the others.
 *
 * A stack counts as installed if one of its files exists, a line of
 * S:Startup-Sequence or S:User-Startup starts it, or it has an item in
 * SYS:WBStartup.  A bsdsocket.library on disk belongs to some other stack
 * as well (AmiBSDNet's lives in memory only).
 *
 * Disabling comments out those startup lines ("; [AmiBSDNet] ...") and
 * moves the WBStartup items and LIBS:bsdsocket.library to
 * SYS:Storage/AmiBSDNet-Disabled; restoring undoes both.  Removing
 * deletes the lines and those files instead.  The startup files are
 * copied to <name>.amibsdnet-bak before the first change.  Program
 * drawers of the other stacks are left alone.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "otherstacks.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define	MARK		"; [AmiBSDNet] "
#define	MARKLEN		14
#define	PARKING		"SYS:Storage/AmiBSDNet-Disabled"

struct stack {
	const char *name;
	const char *files[6];		/* any of these existing */
	const char *words[6];		/* startup lines naming these */
	const char *wbstartup[3];	/* WBStartup items starting so */
	int found;			/* installed */
	int atboot;			/* still started at boot */
};

static struct stack stacks[] = {
	{ "Roadshow",
	  { "C:AddNetInterface", "C:RoadshowControl", "S:Network-Startup",
	    "DEVS:NetInterfaces", NULL },
	  { "Network-Startup", "AddNetInterface", "Roadshow", "NetLogViewer",
	    NULL },
	  { "NetLogViewer", "Roadshow", NULL } },
	{ "Miami",
	  { "Miami:", NULL },
	  { "Miami", NULL },
	  { "Miami", NULL } },
	{ "AmiTCP",
	  { "AmiTCP:", NULL },
	  { "AmiTCP", "startnet", NULL },
	  { "AmiTCP", NULL } },
	{ "Genesis",
	  { "Genesis:", NULL },
	  { "Genesis", NULL },
	  { "Genesis", NULL } },
};
#define	NSTACKS	(sizeof(stacks) / sizeof(stacks[0]))

static const char *startup_files[] = {
	"S:Startup-Sequence", "S:User-Startup", NULL
};

static int disk_bsdsocket;

/* if set, told about every match (NetCtrl CHECK lists them) */
void (*otherstacks_say)(const char *where, const char *what, int len);

static void
say(const char *where, const char *what, int len)
{

	if (otherstacks_say)
		otherstacks_say(where, what, len);
}

/* ------------------------------------------------------------------------ */

static int
lc(int c)
{

	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* case-insensitive substring search within s[0..n) */
static int
has(const char *s, int n, const char *w)
{
	int i, j;

	for (i = 0; i < n; i++) {
		for (j = 0; w[j] && i + j < n &&
		    lc((UBYTE)s[i + j]) == lc((UBYTE)w[j]); j++)
			;
		if (!w[j])
			return 1;
	}
	return 0;
}

static int
starts_nocase(const char *s, const char *p)
{

	while (*p && lc((UBYTE)*s) == lc((UBYTE)*p))
		s++, p++;
	return !*p;
}

static void
cat(char *d, const char *a, const char *b, int n)
{
	int i = 0;

	while (*a && i < n - 1)
		d[i++] = *a++;
	while (*b && i < n - 1)
		d[i++] = *b++;
	d[i] = '\0';
}

/* no "Please insert volume Miami:" requesters while looking */
static APTR
quiet(void)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR old = me->pr_WindowPtr;

	me->pr_WindowPtr = (APTR)-1;
	return old;
}

static void
loud(APTR old)
{

	((struct Process *)SysBase->ThisTask)->pr_WindowPtr = old;
}

static int
exists(const char *path)
{
	BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);

	if (l)
		UnLock(l);
	return l != 0;
}

static char *
read_file(const char *name, LONG *lenp)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
	char *buf = NULL;
	LONG len = 0, size, n;

	*lenp = 0;
	if (fh == 0)
		return NULL;
	Seek(fh, 0, OFFSET_END);
	size = Seek(fh, 0, OFFSET_BEGINNING);
	if (size >= 0 && (buf = AllocVec(size + 1, MEMF_ANY)) != NULL) {
		while (len < size && (n = Read(fh, buf + len, size - len)) > 0)
			len += n;
		buf[len] = '\0';
	}
	Close(fh);
	*lenp = len;
	return buf;
}

/* a command line (not a comment, not ours) starting a detected stack? */
/* like has(), but w must start a word: "startnet" is not in "RestartNet" */
static int
has_word(const char *s, int n, const char *w)
{
	int i, j, c;

	for (i = 0; i < n; i++) {
		if (i > 0) {
			c = lc((UBYTE)s[i - 1]);
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
				continue;
		}
		for (j = 0; w[j] && i + j < n &&
		    lc((UBYTE)s[i + j]) == lc((UBYTE)w[j]); j++)
			;
		if (!w[j])
			return 1;
	}
	return 0;
}

static struct stack *
line_stack(const char *l, int n, int only_found)
{
	unsigned i, j;
	int k = 0, e, quote = 0;

	while (k < n && (l[k] == ' ' || l[k] == '\t'))
		k++;
	if (k == n || l[k] == ';')
		return NULL;
	/* only the command, not a comment after it ("EndIf ; Roadshow") */
	for (e = k; e < n; e++) {
		if (l[e] == '"')
			quote = !quote;
		else if (l[e] == ';' && !quote)
			break;
	}
	if (has(l + k, e - k, "AmiBSDNet"))
		return NULL;
	for (i = 0; i < NSTACKS; i++) {
		if (only_found && !stacks[i].found)
			continue;
		for (j = 0; stacks[i].words[j]; j++)
			if (has_word(l + k, e - k, stacks[i].words[j]))
				return &stacks[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------------
 * detection
 */

static void
scan_wbstartup(int mark)
{
	struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
	BPTR l;
	unsigned i, j;

	if (fib == NULL)
		return;
	if ((l = Lock((CONST_STRPTR)"SYS:WBStartup", ACCESS_READ)) != 0) {
		if (Examine(l, fib))
			while (ExNext(l, fib))
				for (i = 0; i < NSTACKS; i++)
					for (j = 0; stacks[i].wbstartup[j]; j++)
						if (starts_nocase((const char *)
						    fib->fib_FileName,
						    stacks[i].wbstartup[j]) && mark) {
							stacks[i].found =
							    stacks[i].atboot = 1;
							say("SYS:WBStartup",
							    (const char *)
							    fib->fib_FileName, -1);
							break;
						}
		UnLock(l);
	}
	FreeDosObject(DOS_FIB, fib);
}

int
otherstacks_check(char *names, int size)
{
	APTR old = quiet();
	unsigned i, j;
	int n = 0;
	LONG len;
	char *buf, *p, *e;

	for (i = 0; i < NSTACKS; i++) {
		stacks[i].found = stacks[i].atboot = 0;
		for (j = 0; stacks[i].files[j]; j++)
			if (exists(stacks[i].files[j])) {
				stacks[i].found = 1;
				say("installed", stacks[i].files[j], -1);
			}
	}
	for (i = 0; startup_files[i]; i++) {
		if ((buf = read_file(startup_files[i], &len)) == NULL)
			continue;
		for (p = buf; p < buf + len; p = e + 1) {
			struct stack *s;

			for (e = p; e < buf + len && *e != '\n'; e++)
				;
			if ((s = line_stack(p, e - p, 0)) != NULL) {
				s->found = s->atboot = 1;
				say(startup_files[i], p, e - p);
			}
		}
		FreeVec(buf);
	}
	scan_wbstartup(1);
	if ((disk_bsdsocket = exists("LIBS:bsdsocket.library")) != 0)
		say("installed", "LIBS:bsdsocket.library", -1);
	loud(old);

	names[0] = '\0';
	for (i = 0; i < NSTACKS; i++)
		if (stacks[i].found) {
			if (n++)
				cat(names, names, ", ", size);
			cat(names, names, stacks[i].name, size);
		}
	if (disk_bsdsocket && n == 0) {
		cat(names, names, "another stack (LIBS:bsdsocket.library)",
		    size);
		n = 1;
	}
	return n;
}

/*
 * After otherstacks_check(): what would still start at boot (a startup
 * line, a WBStartup item, or a bsdsocket.library on disk that programs
 * could load before AmiBSDNet is up); returns how many.
 */
int
otherstacks_atboot(char *names, int size)
{
	unsigned i;
	int n = 0;

	names[0] = '\0';
	for (i = 0; i < NSTACKS; i++)
		if (stacks[i].atboot) {
			if (n++)
				cat(names, names, ", ", size);
			cat(names, names, stacks[i].name, size);
		}
	if (disk_bsdsocket) {
		if (n++)
			cat(names, names, ", ", size);
		cat(names, names, "LIBS:bsdsocket.library", size);
	}
	return n;
}

/* ------------------------------------------------------------------------
 * disabling, removing, restoring
 */

static void
make_parking(const char *sub)
{
	char path[128];
	BPTR l;

	if ((l = CreateDir((CONST_STRPTR)PARKING)) != 0)
		UnLock(l);
	if (sub) {
		cat(path, PARKING "/", sub, sizeof(path));
		if ((l = CreateDir((CONST_STRPTR)path)) != 0)
			UnLock(l);
	}
}

static int copy_file(const char *, const char *);

/* park (rename into PARKING/sub) or delete a file */
static int
park(const char *path, const char *sub, const char *name, int remove)
{
	char dest[160];

	if (!exists(path))
		return 0;
	if (remove)
		return DeleteFile((CONST_STRPTR)path) ? 1 : -1;
	make_parking(sub);
	cat(dest, PARKING "/", sub, sizeof(dest));
	cat(dest, dest, "/", sizeof(dest));
	cat(dest, dest, name, sizeof(dest));
	DeleteFile((CONST_STRPTR)dest);		/* an older parked copy */
	if (Rename((CONST_STRPTR)path, (CONST_STRPTR)dest))
		return 1;
	/* another volume (LIBS: may be assigned anywhere): copy, delete */
	return copy_file(path, dest) && DeleteFile((CONST_STRPTR)path) ? 1 : -1;
}

static int
copy_file(const char *from, const char *to)
{
	LONG len;
	char *buf = read_file(from, &len);
	BPTR fh;
	int ok = 0;

	if (buf == NULL)
		return 0;
	if ((fh = Open((CONST_STRPTR)to, MODE_NEWFILE)) != 0) {
		ok = Write(fh, buf, len) == len;
		Close(fh);
	}
	FreeVec(buf);
	return ok;
}

/*
 * Replacing a startup file safely: the new text goes to <name>.amibsdnet-new
 * with every Write() checked, and only then replaces the original.  A full
 * disk or a write error leaves the original as it was.
 */
struct safewrite {
	BPTR	fh;
	int	ok;
	char	tmp[110];
	const char *name;
};

static int
sw_open(struct safewrite *sw, const char *name)
{

	sw->name = name;
	sw->ok = 1;
	cat(sw->tmp, name, ".amibsdnet-new", sizeof(sw->tmp));
	sw->fh = Open((CONST_STRPTR)sw->tmp, MODE_NEWFILE);
	return sw->fh != 0 ? 0 : -1;
}

static void
sw_write(struct safewrite *sw, const void *p, LONG n)
{

	if (sw->ok && n > 0 && Write(sw->fh, (APTR)p, n) != n)
		sw->ok = 0;
}

/* 0 when the original was replaced */
static int
sw_close(struct safewrite *sw)
{
	char old[110];

	if (!Close(sw->fh))
		sw->ok = 0;
	if (!sw->ok) {
		DeleteFile((CONST_STRPTR)sw->tmp);
		return -1;
	}
	/*
	 * Never a moment without the file: the original is renamed out of
	 * the way first, and put back if the new one cannot take its place.
	 * (A script that is running keeps reading the old text.)
	 */
	cat(old, sw->name, ".amibsdnet-old", sizeof(old));
	DeleteFile((CONST_STRPTR)old);
	if (!Rename((CONST_STRPTR)sw->name, (CONST_STRPTR)old)) {
		DeleteFile((CONST_STRPTR)sw->tmp);
		return -1;
	}
	if (!Rename((CONST_STRPTR)sw->tmp, (CONST_STRPTR)sw->name) &&
	    !copy_file(sw->tmp, sw->name)) {
		DeleteFile((CONST_STRPTR)sw->name);
		Rename((CONST_STRPTR)old, (CONST_STRPTR)sw->name);
		DeleteFile((CONST_STRPTR)sw->tmp);
		return -1;
	}
	DeleteFile((CONST_STRPTR)sw->tmp);
	DeleteFile((CONST_STRPTR)old);	/* fails while a script reads it */
	return 0;
}

/* the first word of a script line: "If", "EndIf", ... */
static int
keyword(const char *p, int n, const char *kw)
{
	int k = 0, l = 0;

	while (k < n && (p[k] == ' ' || p[k] == '\t'))
		k++;
	while (kw[l] && k + l < n && lc((UBYTE)p[k + l]) == lc((UBYTE)kw[l]))
		l++;
	return !kw[l] && (k + l == n || p[k + l] == ' ' || p[k + l] == '\t' ||
	    p[k + l] == '\r');
}

/*
 * Which lines to take out: those starting another stack, and when such a
 * line is an "If", its whole block up to the matching "EndIf" (so no
 * stray EndIf is left behind).  mark[] gets one entry per line.
 */
static int
mark_lines(const char *buf, LONG len, UBYTE *mark, int max)
{
	const char *p, *e;
	int line = 0, n = 0, depth = 0;

	for (p = buf; p < buf + len && line < max; p = e + 1, line++) {
		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		mark[line] = 0;
		if (depth) {
			mark[line] = 1;
			if (keyword(p, e - p, "If"))
				depth++;
			else if (keyword(p, e - p, "EndIf"))
				depth--;
		} else if (line_stack(p, e - p, 1) != NULL) {
			mark[line] = 1;
			if (keyword(p, e - p, "If"))
				depth = 1;
		}
		n += mark[line];
	}
	return n;
}

#define	MAXLINES	2000

/* rewrite one startup file; mode 0 disable, 1 remove, 2 restore */
static int
edit_startup(const char *name, int mode)
{
	LONG len;
	char *buf = read_file(name, &len), *p, *e, backup[96];
	UBYTE *mark;
	struct safewrite sw;
	int changed = 0, line;

	if (buf == NULL)
		return 0;
	if ((mark = AllocVec(MAXLINES, MEMF_ANY | MEMF_CLEAR)) == NULL) {
		FreeVec(buf);
		return -1;
	}
	if (mode == 2) {
		for (p = buf, line = 0; p < buf + len && line < MAXLINES;
		    p = e + 1, line++) {
			for (e = p; e < buf + len && *e != '\n'; e++)
				;
			if ((mark[line] = starts_nocase(p, MARK)) != 0)
				changed++;
		}
	} else
		changed = mark_lines(buf, len, mark, MAXLINES);
	if (!changed) {
		FreeVec(mark);
		FreeVec(buf);
		return 0;
	}
	/* the first backup is the original: never overwritten, and no change
	   without it */
	cat(backup, name, ".amibsdnet-bak", sizeof(backup));
	if ((!exists(backup) && !copy_file(name, backup)) ||
	    sw_open(&sw, name) != 0) {
		FreeVec(mark);
		FreeVec(buf);
		return -1;
	}
	for (p = buf, line = 0; p < buf + len; p = e + 1, line++) {
		int n, m = line < MAXLINES && mark[line];

		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		n = e - p + (e < buf + len);	/* with the line feed */
		if (mode == 2 && m)
			sw_write(&sw, p + MARKLEN, n - MARKLEN);
		else if (mode != 2 && m) {
			if (mode == 0) {
				sw_write(&sw, MARK, MARKLEN);
				sw_write(&sw, p, n);
			}
		} else
			sw_write(&sw, p, n);
	}
	FreeVec(mark);
	FreeVec(buf);
	return sw_close(&sw) == 0 ? changed : -1;
}

#define	MAXITEMS	16

static int
move_wbstartup(int mode)
{
	struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
	char (*names)[108], path[160];
	int n = 0, i, rv = 0;
	unsigned s, j;
	BPTR l;

	if (fib == NULL)
		return -1;
	/* big buffers off the stack: NetCtrl may run on a small one */
	if ((names = AllocVec(MAXITEMS * 108, MEMF_ANY)) == NULL) {
		FreeDosObject(DOS_FIB, fib);
		return -1;
	}
	/* collect first: renaming while scanning confuses ExNext() */
	if ((l = Lock((CONST_STRPTR)(mode == 2 ? PARKING "/WBStartup" :
	    "SYS:WBStartup"), ACCESS_READ)) != 0) {
		if (Examine(l, fib))
			while (ExNext(l, fib) && n < MAXITEMS)
				for (s = 0; s < NSTACKS; s++)
					for (j = 0; stacks[s].wbstartup[j]; j++)
						if ((mode == 2 || stacks[s].found) &&
						    starts_nocase((const char *)
						    fib->fib_FileName,
						    stacks[s].wbstartup[j])) {
							cat(names[n++], (const char *)
							    fib->fib_FileName, "", 108);
							s = NSTACKS - 1;
							break;
						}
		UnLock(l);
	}
	FreeDosObject(DOS_FIB, fib);
	for (i = 0; i < n; i++) {
		if (mode == 2) {
			char dest[160];

			cat(path, PARKING "/WBStartup/", names[i], sizeof(path));
			cat(dest, "SYS:WBStartup/", names[i], sizeof(dest));
			if (!Rename((CONST_STRPTR)path, (CONST_STRPTR)dest))
				rv = -1;
		} else {
			cat(path, "SYS:WBStartup/", names[i], sizeof(path));
			if (park(path, "WBStartup", names[i], mode == 1) < 0)
				rv = -1;
		}
	}
	FreeVec(names);
	return rv < 0 ? -1 : n;
}

/* ------------------------------------------------------------------------
 * going back (a trial switch that did not connect, or NetCtrl FALLBACK)
 */

#define	OWN_MARK	"; [AmiBSDNet-off] "
#define	OWN_MARKLEN	18

/* comment out the lines inside ";BEGIN AmiBSDNet" ... ";END AmiBSDNet" */
static int
own_startup_off(void)
{
	static const char *name = "S:User-Startup";
	LONG len;
	char *buf = read_file(name, &len), *p, *e;
	struct safewrite sw;
	int inside = 0;

	if (buf == NULL)
		return -1;
	if (sw_open(&sw, name) != 0) {
		FreeVec(buf);
		return -1;
	}
	for (p = buf; p < buf + len; p = e + 1) {
		int n;

		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		n = e - p + (e < buf + len);
		if (starts_nocase(p, ";END AmiBSDNet"))
			inside = 0;
		if (inside && *p != ';')
			sw_write(&sw, OWN_MARK, OWN_MARKLEN);
		sw_write(&sw, p, n);
		if (starts_nocase(p, ";BEGIN AmiBSDNet"))
			inside = 1;
	}
	FreeVec(buf);
	return sw_close(&sw);
}

/* copy every file of dir "from" into dir "to" (created if needed) */
static int
copy_dir(const char *from, const char *to)
{
	struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
	char (*names)[64], a[160], b[160];
	int n = 0, i, rv = 0;
	BPTR l;

	if (fib == NULL)
		return -1;
	if ((names = AllocVec(40 * 64, MEMF_ANY)) == NULL) {
		FreeDosObject(DOS_FIB, fib);
		return -1;
	}
	if ((l = Lock((CONST_STRPTR)from, ACCESS_READ)) != 0) {
		if (Examine(l, fib))
			while (ExNext(l, fib) && n < 40)
				if (fib->fib_DirEntryType < 0)
					cat(names[n++], (const char *)
					    fib->fib_FileName, "", 64);
		UnLock(l);
	}
	FreeDosObject(DOS_FIB, fib);
	if ((l = CreateDir((CONST_STRPTR)to)) != 0)
		UnLock(l);
	for (i = 0; i < n; i++) {
		cat(a, from, "/", sizeof(a));
		cat(a, a, names[i], sizeof(a));
		cat(b, to, "/", sizeof(b));
		cat(b, b, names[i], sizeof(b));
		if (!copy_file(a, b))
			rv = -1;
	}
	FreeVec(names);
	return rv;
}

/* does S:User-Startup start AmiBSDNet (an active C:AmiBSDNet line)? */
int
otherstacks_self_atboot(void)
{
	LONG len;
	char *buf = read_file("S:User-Startup", &len), *p, *e;
	int found = 0;

	if (buf == NULL)
		return 0;
	for (p = buf; p < buf + len && !found; p = e + 1) {
		const char *q = p;

		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		while (q < e && (*q == ' ' || *q == '\t'))
			q++;
		if (e - q >= 11 && starts_nocase(q, "C:AmiBSDNet") &&
		    (e - q == 11 || q[11] == ' ' || q[11] == '\t' ||
		    q[11] == '\r'))
			found = 1;
	}
	FreeVec(buf);
	return found;
}

int
otherstacks_fallback(char *msg, int size)
{
	APTR old;
	int rv, tries, restored;
	BPTR l;

	/*
	 * At boot the startup scripts may still be running (and so cannot be
	 * replaced): try again for up to two minutes.
	 */
	restored = 0;
	for (tries = 0; tries < 24; tries++) {
		char names[128];

		rv = otherstacks_apply(OTHERS_RESTORE);
		/*
		 * AmiBSDNet leaves the boot only once the other stack is back
		 * in it (never neither of them).  "Back" is what counts, also
		 * if some other part could not be put back (a WBStartup item
		 * or LIBS:bsdsocket.library: reported, not retried).
		 */
		otherstacks_check(names, sizeof(names));
		if (!restored && (rv == 0 || otherstacks_atboot(names,
		    sizeof(names)) > 0)) {
			restored = 1;
			old = quiet();
			if (own_startup_off() != 0) {
				restored = 0;	/* still in the boot: retry */
				rv = -1;
			}
			loud(old);
		}
		if (restored)
			break;
		Delay(250);
	}
	old = quiet();
	/* the status icon is not wanted without the stack */
	park("SYS:WBStartup/AmiBSDNetStatus", "AmiBSDNet", "AmiBSDNetStatus",
	    0);
	park("SYS:WBStartup/AmiBSDNetStatus.info", "AmiBSDNet",
	    "AmiBSDNetStatus.info", 0);
	/* the Wi-Fi driver and firmware the installer replaced */
	if (exists("DEVS:Networks/wifipi.device.old")) {
		DeleteFile((CONST_STRPTR)"DEVS:Networks/wifipi.device.amibsdnet");
		Rename((CONST_STRPTR)"DEVS:Networks/wifipi.device",
		    (CONST_STRPTR)"DEVS:Networks/wifipi.device.amibsdnet");
		if (!Rename((CONST_STRPTR)"DEVS:Networks/wifipi.device.old",
		    (CONST_STRPTR)"DEVS:Networks/wifipi.device"))
			rv = -1;
	}
	if (exists("DEVS:Firmware.old") &&
	    copy_dir("DEVS:Firmware.old", "DEVS:Firmware") != 0)
		rv = -1;
	/* T: is gone after the reboot: keep the logs */
	if ((l = CreateDir((CONST_STRPTR)"SYS:Storage/AmiBSDNet-Logs")) != 0)
		UnLock(l);
	copy_file("T:AmiBSDNet.log", "SYS:Storage/AmiBSDNet-Logs/AmiBSDNet.log");
	copy_file("T:WirelessManager.log",
	    "SYS:Storage/AmiBSDNet-Logs/WirelessManager.log");
	/* not restored: the trial mark stays, so the next boot tries again
	   (with AmiBSDNet still in the boot) */
	if (restored)
		DeleteVar((CONST_STRPTR)"AmiBSDNet/Trial",
		    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
	loud(old);
	cat(msg, rv == 0 ? "The previous TCP/IP stack and Wi-Fi driver are "
	    "back, and AmiBSDNet\nis no longer started at boot. Reboot to use "
	    "them.\n\nAmiBSDNet's logs were saved in "
	    "SYS:Storage/AmiBSDNet-Logs." : restored ?
	    "Not everything could be put back; see \"NetCtrl CHECK\".\n"
	    "AmiBSDNet's logs were saved in SYS:Storage/AmiBSDNet-Logs." :
	    "The previous TCP/IP stack could not be put back into the\n"
	    "startup files (are they protected?). AmiBSDNet stays in the\n"
	    "boot and tries again at the next boot; see \"NetCtrl CHECK\".\n"
	    "AmiBSDNet's logs were saved in SYS:Storage/AmiBSDNet-Logs.", "",
	    size);
	return rv;
}

int
otherstacks_apply(int mode)
{
	char dummy[128];
	APTR old;
	int i, rv = 0;

	if (mode != 2)
		otherstacks_check(dummy, sizeof(dummy));
	old = quiet();
	for (i = 0; startup_files[i]; i++)
		if (edit_startup(startup_files[i], mode) < 0)
			rv = -1;
	if (move_wbstartup(mode) < 0)
		rv = -1;
	if (mode == 2) {
		if (exists(PARKING "/Libs/bsdsocket.library") &&
		    !exists("LIBS:bsdsocket.library") &&
		    !Rename((CONST_STRPTR)PARKING "/Libs/bsdsocket.library",
		    (CONST_STRPTR)"LIBS:bsdsocket.library") &&
		    !(copy_file(PARKING "/Libs/bsdsocket.library",
		    "LIBS:bsdsocket.library") &&
		    DeleteFile((CONST_STRPTR)PARKING "/Libs/bsdsocket.library")))
			rv = -1;
	} else if (park("LIBS:bsdsocket.library", "Libs", "bsdsocket.library",
	    mode == 1) < 0)
		rv = -1;
	loud(old);
	return rv;
}
