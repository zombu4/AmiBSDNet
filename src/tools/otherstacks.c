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
 * copied to <name>.amibsdnet-backup before the first change.  Program
 * drawers of the other stacks are left alone.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
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
	  { "Network-Startup", "AddNetInterface", "RoadshowControl",
	    "NetLogViewer", NULL },
	  { "NetLogViewer", NULL } },
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
static struct stack *
line_stack(const char *l, int n, int only_found)
{
	unsigned i, j;
	int k = 0;

	while (k < n && (l[k] == ' ' || l[k] == '\t'))
		k++;
	if (k == n || l[k] == ';')
		return NULL;
	if (has(l, n, "AmiBSDNet"))
		return NULL;
	for (i = 0; i < NSTACKS; i++) {
		if (only_found && !stacks[i].found)
			continue;
		for (j = 0; stacks[i].words[j]; j++)
			if (has(l + k, n - k, stacks[i].words[j]))
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
						    stacks[i].wbstartup[j]) && mark)
							stacks[i].found =
							    stacks[i].atboot = 1;
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
			if (exists(stacks[i].files[j]))
				stacks[i].found = 1;
	}
	for (i = 0; startup_files[i]; i++) {
		if ((buf = read_file(startup_files[i], &len)) == NULL)
			continue;
		for (p = buf; p < buf + len; p = e + 1) {
			struct stack *s;

			for (e = p; e < buf + len && *e != '\n'; e++)
				;
			if ((s = line_stack(p, e - p, 0)) != NULL)
				s->found = s->atboot = 1;
		}
		FreeVec(buf);
	}
	scan_wbstartup(1);
	disk_bsdsocket = exists("LIBS:bsdsocket.library");
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
	return Rename((CONST_STRPTR)path, (CONST_STRPTR)dest) ? 1 : -1;
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
	BPTR fh;
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
	cat(backup, name, ".amibsdnet-backup", sizeof(backup));
	if (!exists(backup))
		copy_file(name, backup);
	if ((fh = Open((CONST_STRPTR)name, MODE_NEWFILE)) == 0) {
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
			Write(fh, p + MARKLEN, n - MARKLEN);
		else if (mode != 2 && m) {
			if (mode == 0) {
				Write(fh, (APTR)MARK, MARKLEN);
				Write(fh, p, n);
			}
		} else
			Write(fh, p, n);
	}
	Close(fh);
	FreeVec(mark);
	FreeVec(buf);
	return changed;
}

static int
move_wbstartup(int mode)
{
	struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
	char names[16][108], path[160];
	int n = 0, i, rv = 0;
	unsigned s, j;
	BPTR l;

	if (fib == NULL)
		return -1;
	/* collect first: renaming while scanning confuses ExNext() */
	if ((l = Lock((CONST_STRPTR)(mode == 2 ? PARKING "/WBStartup" :
	    "SYS:WBStartup"), ACCESS_READ)) != 0) {
		if (Examine(l, fib))
			while (ExNext(l, fib) && n < 16)
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
	return rv < 0 ? -1 : n;
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
		    (CONST_STRPTR)"LIBS:bsdsocket.library"))
			rv = -1;
	} else if (park("LIBS:bsdsocket.library", "Libs", "bsdsocket.library",
	    mode == 1) < 0)
		rv = -1;
	loud(old);
	return rv;
}
