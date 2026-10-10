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
	const char *files[10];		/* any of these existing */
	const char *words[14];		/* startup lines naming these */
	const char *wbstartup[3];	/* WBStartup items starting so */
	int found;			/* installed */
	int atboot;			/* still started at boot */
	int inscript;			/* ... by a startup script line */
};

static struct stack stacks[] = {
	{ "Roadshow",
	  { "C:AddNetInterface", "C:RoadshowControl", "S:Network-Startup",
	    "DEVS:NetInterfaces", "C:ConfigureNetInterface", "C:NetShutdown",
	    "C:ShowNetStatus", NULL },
	  { "Network-Startup", "AddNetInterface", "Roadshow", "NetLogViewer",
	    "ConfigureNetInterface", "AddNetRoute", "DeleteNetRoute",
	    "NetShutdown", "ShowNetStatus", "GetNetStatus",
	    "RemoveNetInterface", "SampleNetSpeed", "NetInterfaces", NULL },
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


static int disk_bsdsocket;

/* if set, otherstacks_apply() handles only the stack of this name */
const char *otherstacks_only;

static int
selected(const struct stack *s)
{
	const char *a = s->name, *b = otherstacks_only;

	if (b == NULL)
		return 1;
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}
static int startup_rv;		/* otherstacks_apply(): the script edits */

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

/*
 * The startup scripts: S:Startup-Sequence, S:User-Startup and the
 * scripts they run with Execute (S:Network-Startup and so on), also from
 * lines an earlier switch disabled.
 */
#define	MAXSCRIPTS	12

static char scripts[MAXSCRIPTS][128];
static int nscripts;

static void
add_script(const char *p, int n)
{
	char name[128];
	int i, k;

	if (n <= 0 || n >= (int)sizeof(name) || nscripts >= MAXSCRIPTS)
		return;
	for (i = 0; i < n; i++)
		name[i] = p[i];
	name[n] = '\0';
	for (k = 0; k < nscripts; k++) {
		const char *a = scripts[k], *b = name;

		while (*a && lc((UBYTE)*a) == lc((UBYTE)*b))
			a++, b++;
		if (*a == *b)
			return;		/* listed already */
	}
	if (!exists(name))
		return;
	cat(scripts[nscripts++], name, "", sizeof(scripts[0]));
}

static void
build_scripts(void)
{
	char *buf, *p, *e, *q;
	LONG len;
	int i, n;

	nscripts = 0;
	cat(scripts[nscripts++], "S:Startup-Sequence", "", sizeof(scripts[0]));
	cat(scripts[nscripts++], "S:User-Startup", "", sizeof(scripts[0]));
	for (i = 0; i < nscripts; i++) {
		if ((buf = read_file(scripts[i], &len)) == NULL)
			continue;
		for (p = buf; p < buf + len; p = e + 1) {
			for (e = p; e < buf + len && *e != '\n'; e++)
				;
			q = p;
			while (q < e && (*q == ' ' || *q == '\t'))
				q++;
			if (e - q >= MARKLEN && starts_nocase(q, MARK))
				q += MARKLEN;
			if (e - q < 8 || !starts_nocase(q, "Execute") ||
			    (q[7] != ' ' && q[7] != '\t'))
				continue;
			q += 8;
			while (q < e && (*q == ' ' || *q == '\t'))
				q++;
			/* redirections ("Execute >NIL: S:x") are not the script */
			while (q < e && (*q == '>' || *q == '<')) {
				while (q < e && *q != ' ' && *q != '\t')
					q++;
				while (q < e && (*q == ' ' || *q == '\t'))
					q++;
			}
			if (q < e && *q == '"') {
				q++;
				for (n = 0; q + n < e && q[n] != '"'; n++)
					;
			} else
				for (n = 0; q + n < e && q[n] != ' ' &&
				    q[n] != '\t' && q[n] != '\r' && q[n] != ';';
				    n++)
					;
			add_script(q, n);
		}
		FreeVec(buf);
	}
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
		if ((only_found && !stacks[i].found) || !selected(&stacks[i]))
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
		stacks[i].found = stacks[i].atboot = stacks[i].inscript = 0;
		for (j = 0; stacks[i].files[j]; j++)
			if (exists(stacks[i].files[j])) {
				stacks[i].found = 1;
				say("installed", stacks[i].files[j], -1);
			}
	}
	build_scripts();
	for (i = 0; i < (unsigned)nscripts; i++) {
		if ((buf = read_file(scripts[i], &len)) == NULL)
			continue;
		for (p = buf; p < buf + len; p = e + 1) {
			struct stack *s;

			for (e = p; e < buf + len && *e != '\n'; e++)
				;
			if ((s = line_stack(p, e - p, 0)) != NULL) {
				s->found = s->atboot = s->inscript = 1;
				say(scripts[i], p, e - p);
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
		if (stacks[i].atboot && selected(&stacks[i])) {
			if (n++)
				cat(names, names, ", ", size);
			cat(names, names, stacks[i].name, size);
		}
	if (disk_bsdsocket && otherstacks_only == NULL) {
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
	struct FileInfoBlock *fib;
	LONG prot = -1;
	BPTR l;

	if (!Close(sw->fh))
		sw->ok = 0;
	if (!sw->ok) {
		DeleteFile((CONST_STRPTR)sw->tmp);
		return -1;
	}
	/* the new file gets the original's protection bits (the s bit of a
	   script, say) */
	if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
		if ((l = Lock((CONST_STRPTR)sw->name, ACCESS_READ)) != 0) {
			if (Examine(l, fib))
				prot = fib->fib_Protection;
			UnLock(l);
		}
		FreeDosObject(DOS_FIB, fib);
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
	if (prot != -1)
		SetProtection((CONST_STRPTR)sw->name, prot);
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
mark_lines(const char *buf, LONG len, UBYTE *mark, int max, int only_found)
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
		} else if (line_stack(p, e - p, only_found) != NULL) {
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
	} else {
		changed = mark_lines(buf, len, mark, MAXLINES, 1);
		/*
		 * Removing: also the lines an earlier switch disabled - with
		 * a stack chosen (otherstacks_only), only that stack's: the
		 * disabled lines are looked at without their mark, If blocks
		 * included, the same way as active ones.
		 */
		if (mode == 1) {
			char *plain = NULL;
			UBYTE *pm = NULL;
			LONG pl = 0;

			if (otherstacks_only &&
			    (plain = AllocVec(len + 1, MEMF_ANY)) != NULL &&
			    (pm = AllocVec(MAXLINES, MEMF_ANY | MEMF_CLEAR)) != NULL) {
				for (p = buf; p < buf + len; p = e + 1) {
					for (e = p; e < buf + len && *e != '\n'; e++)
						;
					if (starts_nocase(p, MARK))
						p += MARKLEN;
					while (p < e)
						plain[pl++] = *p++;
					if (e < buf + len)
						plain[pl++] = '\n';
				}
				/* (also when its files are gone already) */
				mark_lines(plain, pl, pm, MAXLINES, 0);
			} else if (otherstacks_only) {
				/* no memory: nothing deleted, and said so */
				if (pm)
					FreeVec(pm);
				if (plain)
					FreeVec(plain);
				FreeVec(mark);
				FreeVec(buf);
				return -1;
			}
			for (p = buf, line = 0; p < buf + len &&
			    line < MAXLINES; p = e + 1, line++) {
				for (e = p; e < buf + len && *e != '\n'; e++)
					;
				if (!mark[line] && starts_nocase(p, MARK) &&
				    (otherstacks_only == NULL ||
				    (pm && pm[line]))) {
					mark[line] = 1;
					changed++;
				}
			}
			if (pm)
				FreeVec(pm);
			if (plain)
				FreeVec(plain);
		}
	}
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
						    selected(&stacks[s]) &&
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


/* ------------------------------------------------------------------------
 * uninstalling (NetCtrl UNINSTALL, the installer's "Uninstall")
 */

#define	INSTALL_LOG	"S:AmiBSDNet-Install.log"

static int
slen_(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

static int block_line(const char *, const char *, const char *);

/* the ";BEGIN AmiBSDNet" ... ";END AmiBSDNet" block out of S:User-Startup
   (0: done or not there; -1: not changed - unreadable, unwritable, or a
   block without its END line, which would take the rest of the file) */
static int
own_startup_remove(void)
{
	static const char *name = "S:User-Startup";
	LONG len;
	char *buf, *p, *e;
	struct safewrite sw;
	int inside = 0, found = 0;
	BPTR l;

	if ((l = Lock((CONST_STRPTR)name, ACCESS_READ)) == 0)
		return 0;		/* no S:User-Startup */
	UnLock(l);
	if ((buf = read_file(name, &len)) == NULL)
		return -1;
	for (p = buf; p < buf + len; p = e + 1) {
		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		if (block_line(p, e, ";BEGIN AmiBSDNet")) {
			if (inside)
				break;		/* BEGIN BEGIN: damaged */
			inside = found = 1;
		} else if (block_line(p, e, ";END AmiBSDNet"))
			inside = 0;
	}
	if (inside) {
		FreeVec(buf);
		return -1;
	}
	if (!found) {
		FreeVec(buf);
		return 0;
	}
	if (sw_open(&sw, name) != 0) {
		FreeVec(buf);
		return -1;
	}
	for (p = buf; p < buf + len; p = e + 1) {
		int n;

		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		n = e - p + (e < buf + len);
		if (block_line(p, e, ";BEGIN AmiBSDNet"))
			inside = 1;
		if (!inside)
			sw_write(&sw, p, n);
		if (block_line(p, e, ";END AmiBSDNet"))
			inside = 0;
	}
	FreeVec(buf);
	return sw_close(&sw);
}

/* exactly this marker line (not ";BEGIN AmiBSDNet-something") */
static int
block_line(const char *p, const char *e, const char *marker)
{
	int l = 0;

	while (marker[l])
		l++;
	return e - p >= l && starts_nocase(p, marker) &&
	    (e - p == l || p[l] == '\r' || p[l] == ' ' || p[l] == '\t');
}

/* a file, or a drawer with everything in it: only for AmiBSDNet's own
   drawers (the log drawer, the parking drawer, its settings) */
static int
delete_all(const char *path)
{
	struct FileInfoBlock *fib;
	char sub[256], (*names)[108] = NULL;
	LONG *types = NULL;
	BPTR l;
	int n, i, rv = 0, isdir = 0;

	if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) == 0)
		return 0;		/* not there */
	if ((fib = AllocDosObject(DOS_FIB, NULL)) == NULL) {
		UnLock(l);
		return -1;
	}
	if (Examine(l, fib))
		isdir = fib->fib_DirEntryType == ST_USERDIR;
	UnLock(l);
	if (isdir && (names = AllocVec(64 * 108, MEMF_ANY)) != NULL &&
	    (types = AllocVec(64 * sizeof(LONG), MEMF_ANY)) != NULL) {
		do {
			n = 0;
			if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) != 0) {
				if (Examine(l, fib))
					while (n < 64 && ExNext(l, fib)) {
						cat(names[n], (const char *)
						    fib->fib_FileName, "", 108);
						types[n++] = fib->fib_DirEntryType;
					}
				UnLock(l);
			}
			for (i = 0; i < n; i++) {
				if (slen_(path) + 1 + slen_(names[i]) >=
				    (int)sizeof(sub)) {
					rv = -1;
					continue;
				}
				cat(sub, path, "/", sizeof(sub));
				cat(sub, sub, names[i], sizeof(sub));
				if (types[i] == ST_USERDIR) {
					if (delete_all(sub) != 0)
						rv = -1;
				} else {
					/* (files, and links as links) */
					SetProtection((CONST_STRPTR)sub, 0);
					if (!DeleteFile((CONST_STRPTR)sub))
						rv = -1;
				}
			}
		} while (n == 64 && rv == 0);
	}
	if (types)
		FreeVec(types);
	if (names)
		FreeVec(names);
	FreeDosObject(DOS_FIB, fib);
	SetProtection((CONST_STRPTR)path, 0);
	if (!DeleteFile((CONST_STRPTR)path))
		rv = -1;
	return rv;
}

/*
 * One entry the installer listed, deleted on its own: a file, a link as
 * the link, a drawer only if it is empty (what someone else put there
 * stays, and so does the drawer).  0: gone (or was not there), 1: a
 * drawer that is not empty (kept), -1: could not be deleted.
 */
static int
delete_one(const char *path)
{
	LONG err;

	SetProtection((CONST_STRPTR)path, 0);
	if (DeleteFile((CONST_STRPTR)path))
		return 0;
	err = IoErr();
	if (err == ERROR_OBJECT_NOT_FOUND || err == ERROR_DIR_NOT_FOUND)
		return 0;
	if (err == ERROR_DIRECTORY_NOT_EMPTY)
		return 1;
	return -1;
}

/*
 * A path from the log that may be deleted: absolute ("NAME:something"),
 * not a volume or assign itself, no parent ("/"), and nothing in S:
 * (the startup files; the log itself is deleted separately).
 */
static int
log_path_ok(const char *p)
{
	int i, colon = -1;

	for (i = 0; p[i]; i++)
		if (p[i] == ':') {
			if (colon >= 0)
				return 0;
			colon = i;
		}
	if (colon <= 0 || p[colon + 1] == '\0' || p[colon + 1] == '/' ||
	    p[i - 1] == '/')
		return 0;
	for (i = colon; p[i]; i++)
		if (p[i] == '/' && p[i + 1] == '/')
			return 0;
	if (colon == 1 && (p[0] == 'S' || p[0] == 's'))
		return 0;
	return 1;
}

/* what is always AmiBSDNet's */
static const char *const own_files[] = {
	"C:AmiBSDNet", "C:NetCtrl", "C:Ping", "C:SerialShell",
	"SYS:WBStartup/AmiBSDNetStatus", "SYS:WBStartup/AmiBSDNetStatus.info",
	"T:AmiBSDNet.log", "T:AmiBSDNet-RS.txt", "T:WirelessManager.log", NULL
};

/* AmiBSDNet's own drawers: deleted with everything in them */
static const char *const own_dirs[] = {
	"SYS:Storage/AmiBSDNet-Logs", "ENV:AmiBSDNet", "ENVARC:AmiBSDNet", NULL
};

/* what the installer puts in the AmiBSDNet drawer.  Without a log (an
   older version, or an installation that stopped before writing it) the
   drawer is SYS:AmiBSDNet: only these go, and the drawer if it is empty
   then (the unpacked archive itself has them in Docs/ and Status/) */
static const char *const drawer_files[] = {
	"AmiBSDNet.txt", "AmiBSDNet.txt.info", "LICENSE.txt", "LICENSE.txt.info",
	"ThirdParty.txt", "ThirdParty.txt.info", "AmiBSDNetStatus",
	"AmiBSDNetStatus.info", NULL
};

int
otherstacks_uninstall(char *msg, int size)
{
	char *list = NULL, *p, *e, *s, path[200], names[128];
	LONG len = 0;
	APTR old;
	int rv = 0, kept = 0, i, k, r, pass, icon, restored;
	struct MsgPort *port;
	struct FileInfoBlock *fib;
	BPTR l;

	/* out of the boot first: if S:User-Startup cannot be changed, nothing
	   is deleted (its C:AmiBSDNet line would stop the rest of the
	   startup at every boot once C:AmiBSDNet is gone) */
	old = quiet();
	r = own_startup_remove();
	loud(old);
	if (r != 0) {
		cat(msg, "Nothing was removed: S:User-Startup could not be "
		    "changed (write-protected, disk full, or its \";BEGIN "
		    "AmiBSDNet\" block has no \";END AmiBSDNet\" line: remove "
		    "that block by hand, then uninstall again).", "", size);
		return -1;
	}
	/* another stack an earlier switch took out of the boot: back */
	restored = otherstacks_apply(OTHERS_RESTORE) == 0;
	otherstacks_check(names, sizeof(names));
	old = quiet();
	/* the serial Shell stops (it is gone after the reboot anyway) */
	Forbid();
	if ((port = FindPort((CONST_STRPTR)"AmiBSDNet.SerialShell")) != NULL)
		Signal(port->mp_SigTask, SIGBREAKF_CTRL_C);
	Permit();
	/* what the installer installed: its log, S:AmiBSDNet-Install.log
	   ("FILE <path>" lines; the other lines are notes).  From the end:
	   what was put in a drawer is listed after the drawer, and goes
	   first */
	if ((l = Lock((CONST_STRPTR)INSTALL_LOG, ACCESS_READ)) != 0) {
		UnLock(l);
		if ((list = read_file(INSTALL_LOG, &len)) == NULL)
			rv = -1;
	}
	if (list) {
		/* (two rounds: icons last, and only those whose file or drawer
		   is gone - a drawer that stays keeps its icon) */
		for (pass = 0; pass < 2; pass++)
		for (e = list + len; e > list; e = s) {
			/* the line p..e; s: the '\n' before it */
			for (p = e; p > list && p[-1] != '\n'; p--)
				;
			s = p > list ? p - 1 : list;
			if (e - p < 6 || !starts_nocase(p, "FILE "))
				continue;
			for (k = 0; p + 5 + k < e && p[5 + k] != '\n' &&
			    p[5 + k] != '\r'; k++)
				;
			if (k >= (int)sizeof(path)) {
				if (pass == 0)
					rv = -1;	/* too long: not cut short */
				continue;
			}
			for (i = 0; i < k; i++)
				path[i] = p[5 + i];
			path[k] = '\0';
			icon = k > 5 && starts_nocase(path + k - 5, ".info");
			if (!log_path_ok(path) || icon != pass)
				continue;
			if (icon) {
				/* its file or drawer (the name without
				   ".info") is still there: the icon stays */
				path[k - 5] = '\0';
				if ((l = Lock((CONST_STRPTR)path,
				    ACCESS_READ)) != 0) {
					UnLock(l);
					continue;
				}
				path[k - 5] = '.';
			}
			if ((r = delete_one(path)) < 0)
				rv = -1;
			else if (r > 0)
				kept = 1;
		}
		FreeVec(list);
	} else if (rv == 0) {
		for (i = 0; drawer_files[i]; i++) {
			cat(path, "SYS:AmiBSDNet/", drawer_files[i], sizeof(path));
			if (delete_one(path) < 0)
				rv = -1;
		}
		if (delete_one("SYS:AmiBSDNet") == 0)
			delete_one("SYS:AmiBSDNet.info");
	}
	/* (T: and ENV: are in RAM and go with the reboot anyway; the running
	   stack still has its log open: not a failure) */
	for (i = 0; own_files[i]; i++)
		if (delete_one(own_files[i]) < 0 &&
		    !starts_nocase(own_files[i], "T:"))
			rv = -1;
	for (i = 0; own_dirs[i]; i++)
		if (delete_all(own_dirs[i]) != 0 &&
		    !starts_nocase(own_dirs[i], "ENV:"))
			rv = -1;
	/* parked files of another stack: only once they are back */
	if (restored)
		delete_all(PARKING);
	/* the backups of the startup files it changed (S:*.amibsdnet-*):
	   only when all went well - after a failed restore they are the
	   only copies of the original startup files */
	if (restored && rv == 0 &&
	    (fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
		char (*bak)[108] = AllocVec(32 * 108, MEMF_ANY);
		int nb = 0;

		if (bak && (l = Lock((CONST_STRPTR)"S:", ACCESS_READ)) != 0) {
			if (Examine(l, fib))
				while (nb < 32 && ExNext(l, fib))
					if (fib->fib_DirEntryType < 0 &&
					    has((const char *)fib->fib_FileName,
					    slen_((const char *)fib->fib_FileName),
					    ".amibsdnet-"))
						cat(bak[nb++], (const char *)
						    fib->fib_FileName, "", 108);
			UnLock(l);
		}
		for (i = 0; i < nb; i++) {
			cat(path, "S:", bak[i], sizeof(path));
			delete_one(path);
		}
		if (bak)
			FreeVec(bak);
		FreeDosObject(DOS_FIB, fib);
	}
	/* the log last: if something failed, a second run still knows */
	if (rv == 0)
		delete_one(INSTALL_LOG);
	loud(old);
	cat(msg, rv == 0 ? "AmiBSDNet is removed. Reboot: the stack still in "
	    "memory goes then." : "AmiBSDNet is removed, but some files could "
	    "not be deleted (in use or protected?).\nReboot and run "
	    "\"NetCtrl UNINSTALL\" again, or delete them by hand.", "", size);
	if (kept)
		cat(msg, msg, "\nA drawer it had created has other files in it "
		    "now: that drawer and those files were left alone.", size);
	if (!restored)
		cat(msg, msg, "\nThe stack it had switched from could not be put "
		    "back completely: its parked files are still in "
		    "SYS:Storage/AmiBSDNet-Disabled, and S:*.amibsdnet-bak are "
		    "the original startup files.", size);
	return rv;
}

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
	char names[128];
	APTR old;
	int rv, tries, back = 0, inscript = 0, offok = 0;
	unsigned i;
	BPTR l;

	/*
	 * Put the other stack back into the startup scripts.  At boot the
	 * scripts may still be running (and so cannot be replaced): try
	 * again for up to two minutes.  "Back" means: every script edit
	 * worked and the other stack starts at boot again (a script line
	 * or its WBStartup item; LIBS:bsdsocket.library alone does not
	 * start it).  Only then does AmiBSDNet leave the boot: never
	 * neither.
	 */
	for (tries = 0; tries < 24; tries++) {
		rv = otherstacks_apply(OTHERS_RESTORE);
		otherstacks_check(names, sizeof(names));
		/* started by a script line or its WBStartup item (Miami);
		   LIBS:bsdsocket.library alone starts nothing */
		for (inscript = 0, i = 0; i < NSTACKS; i++)
			if (stacks[i].atboot)
				inscript = 1;
		if (startup_rv == 0 && !inscript)
			break;		/* nothing there to go back to */
		if (startup_rv == 0) {
			back = 1;
			old = quiet();
			/* (and really gone: a C:AmiBSDNet line outside its
			   own block is not taken out) */
			offok = own_startup_off() == 0 &&
			    !otherstacks_self_atboot();
			loud(old);
			if (offok)
				break;
		}
		Delay(250);
	}

	old = quiet();
	/* T: is gone after the reboot: keep the logs */
	if ((l = CreateDir((CONST_STRPTR)"SYS:Storage/AmiBSDNet-Logs")) != 0)
		UnLock(l);
	copy_file("T:AmiBSDNet.log", "SYS:Storage/AmiBSDNet-Logs/AmiBSDNet.log");
	copy_file("T:WirelessManager.log",
	    "SYS:Storage/AmiBSDNet-Logs/WirelessManager.log");

	if (!back || !offok) {
		/*
		 * AmiBSDNet stays, with its own Wi-Fi driver: the trial ends
		 * here, so that it starts normally from the next boot on and
		 * there is a network (the other stack could not come back,
		 * or AmiBSDNet could not be taken out of the boot).
		 */
		/* what the restore put back anyway (a WBStartup item, the
		   library) goes again: it would only fight AmiBSDNet */
		if (!back)
			otherstacks_apply(OTHERS_DISABLE);
		DeleteVar((CONST_STRPTR)"AmiBSDNet/Trial",
		    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
		loud(old);
		cat(msg, !back && startup_rv == 0 ?
		    "There is no other TCP/IP stack to go back to, so nothing\n"
		    "was changed: AmiBSDNet stays. It starts at every boot\n"
		    "(reboot now if it is not running)." :
		    !back ?
		    "The previous TCP/IP stack could not be put back into the\n"
		    "startup files (are they write-protected?). AmiBSDNet stays,\n"
		    "so that there is a network: reboot to start it. Fix the\n"
		    "files and run \"NetCtrl FALLBACK\" again to go back." :
		    "The previous TCP/IP stack is back in the startup files, but\n"
		    "AmiBSDNet could not be taken out of S:User-Startup: remove\n"
		    "its line there by hand (it does not start next to another\n"
		    "stack anyway).", "", size);
		cat(msg, msg, "\n\nAmiBSDNet's logs were saved in "
		    "SYS:Storage/AmiBSDNet-Logs.", size);
		return -1;
	}

	/* the other stack is back: AmiBSDNet's parts go */
	park("SYS:WBStartup/AmiBSDNetStatus", "AmiBSDNet", "AmiBSDNetStatus",
	    0);
	park("SYS:WBStartup/AmiBSDNetStatus.info", "AmiBSDNet",
	    "AmiBSDNetStatus.info", 0);
	/* (the Wi-Fi driver and firmware are the system's: never touched) */
	DeleteVar((CONST_STRPTR)"AmiBSDNet/Trial",
	    GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
	loud(old);
	cat(msg, rv == 0 ? "The previous TCP/IP stack is back, and AmiBSDNet "
	    "is no longer\nstarted at boot. Reboot to use it." :
	    "The previous TCP/IP stack is back in the startup files and\n"
	    "AmiBSDNet is no longer started at boot, but not everything\n"
	    "could be put back (see \"NetCtrl CHECK\"). Reboot to use it.",
	    "\n\nAmiBSDNet's logs were saved in SYS:Storage/AmiBSDNet-Logs.",
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
	startup_rv = 0;
	build_scripts();
	for (i = 0; i < nscripts; i++)
		if (edit_startup(scripts[i], mode) < 0)
			rv = startup_rv = -1;
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
	} else if (otherstacks_only == NULL &&
	    park("LIBS:bsdsocket.library", "Libs", "bsdsocket.library",
	    mode == 1) < 0)
		rv = -1;
	loud(old);
	return rv;
}
