/*
 * Other TCP/IP stacks (Roadshow, Miami, AmiTCP, Genesis): only one stack
 * may run, so the installer offers to disable or remove the others.
 *
 * A stack counts as installed if one of its files exists (for Miami,
 * AmiTCP and Genesis: an assign of that name, not a volume), a command
 * line of S:Startup-Sequence, S:User-Startup or a script in S: they run
 * with Execute starts one of its commands, or it has an item in
 * SYS:WBStartup.  A command counts by its name (the last part of its
 * path), and only from C:, S:, the stack's own assign or the Shell's
 * path; the words after it (arguments) do not count.  A bsdsocket.library
 * on disk belongs to some other stack as well (AmiBSDNet's lives in
 * memory only).
 *
 * Disabling comments out those command lines ("; [AmiBSDNet] ...") and
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
#define	PATHMAX		256

struct stack {
	const char *name;
	const char *files[10];		/* any of these existing ("X:": the
					   assign X) */
	const char *commands[14];	/* command lines running these */
	const char *assign;		/* its programs' assign, or NULL */
	const char *wbstartup[3];	/* WBStartup items starting so */
	int found;			/* installed */
	int atboot;			/* still started at boot */
};

static struct stack stacks[] = {
	{ "Roadshow",
	  { "C:AddNetInterface", "C:RoadshowControl", "S:Network-Startup",
	    "DEVS:NetInterfaces", "C:ConfigureNetInterface", "C:NetShutdown",
	    "C:ShowNetStatus", NULL },
	  { "Network-Startup", "AddNetInterface", "NetLogViewer",
	    "ConfigureNetInterface", "AddNetRoute", "DeleteNetRoute",
	    "NetShutdown", "ShowNetStatus", "GetNetStatus",
	    "RemoveNetInterface", "SampleNetSpeed", "RoadshowControl", NULL },
	  NULL,
	  { "NetLogViewer", "Roadshow", NULL } },
	{ "Miami",
	  { "Miami:", NULL },
	  { "Miami", NULL },
	  "Miami:",
	  { "Miami", NULL } },
	{ "AmiTCP",
	  { "AmiTCP:", NULL },
	  { "AmiTCP", "startnet", NULL },
	  "AmiTCP:",
	  { "AmiTCP", NULL } },
	{ "Genesis",
	  { "Genesis:", NULL },
	  { "Genesis", NULL },
	  "Genesis:",
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

static int
eq_nocase(const char *a, const char *b)
{

	while (*a && lc((UBYTE)*a) == lc((UBYTE)*b))
		a++, b++;
	return *a == *b;
}

static int
slen_(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
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

/* a + b into d (n bytes) only if all of it fits */
static int
cat_fits(char *d, const char *a, const char *b, int n)
{

	if (slen_(a) + slen_(b) >= n)
		return 0;
	cat(d, a, b, n);
	return 1;
}

/* no "Please insert volume Miami:" requesters while looking: with
   pr_WindowPtr -1 there are none (dos.doc ErrorReport) */
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

/* an assign of this name ("Miami:"), not a volume or device */
static int
assign_exists(const char *name)
{
	char n[32];
	struct DosList *dl;
	int i, r;

	for (i = 0; name[i] && name[i] != ':' && i < (int)sizeof(n) - 1; i++)
		n[i] = name[i];
	n[i] = '\0';
	dl = LockDosList(LDF_ASSIGNS | LDF_READ);
	/* "name - Name of device entry (without ':')" (dos.doc
	   FindDosEntry) */
	r = FindDosEntry(dl, (CONST_STRPTR)n, LDF_ASSIGNS) != NULL;
	UnLockDosList(LDF_ASSIGNS | LDF_READ);
	return r;
}

static int
file_exists(const char *path)
{
	int l = slen_(path);

	return l > 0 && path[l - 1] == ':' ? assign_exists(path) :
	    exists(path);
}

/* the whole file (NUL-terminated); NULL if it is not there or could not
   all be read (a part must never be written back) */
static char *
read_file(const char *name, LONG *lenp)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
	char *buf = NULL;
	LONG len = 0, size, n = 0;

	*lenp = 0;
	if (fh == 0)
		return NULL;
	if (Seek(fh, 0, OFFSET_END) < 0 ||
	    (size = Seek(fh, 0, OFFSET_BEGINNING)) < 0 ||
	    (buf = AllocVec(size + 1, MEMF_ANY)) == NULL) {
		Close(fh);
		return NULL;
	}
	while (len < size && (n = Read(fh, buf + len, size - len)) > 0)
		len += n;
	Close(fh);
	if (len != size) {
		FreeVec(buf);
		return NULL;
	}
	buf[len] = '\0';
	*lenp = len;
	return buf;
}

/* both files read in full and the same (a missing one is not) */
static int
same_file(const char *a, const char *b)
{
	char *x, *y;
	LONG xl, yl, i;
	int same = 0;

	if ((x = read_file(a, &xl)) == NULL)
		return 0;
	if ((y = read_file(b, &yl)) != NULL) {
		if (xl == yl) {
			for (i = 0; i < xl && x[i] == y[i]; i++)
				;
			same = i == xl;
		}
		FreeVec(y);
	}
	FreeVec(x);
	return same;
}

/*
 * The directories of S: (an assign may have several: dol_List,
 * dos/dosextens.h); 0 when there is none.  The locks are the caller's.
 */
#define	MAXSDIRS	8

static int
s_dirs(BPTR *out)
{
	struct DosList *dl;
	struct AssignList *al;
	int n = 0;

	dl = LockDosList(LDF_ASSIGNS | LDF_READ);
	if ((dl = FindDosEntry(dl, (CONST_STRPTR)"S", LDF_ASSIGNS)) != NULL &&
	    dl->dol_Type == DLT_DIRECTORY) {
		if (dl->dol_Lock)
			out[n++] = DupLock(dl->dol_Lock);
		for (al = dl->dol_misc.dol_assign.dol_List; al && n < MAXSDIRS;
		    al = al->al_Next)
			out[n++] = DupLock(al->al_Lock);
	}
	UnLockDosList(LDF_ASSIGNS | LDF_READ);
	return n;
}

static void
unlock_all(BPTR *l, int n)
{

	while (n-- > 0)
		if (l[n])
			UnLock(l[n]);
}

/* is the object behind this path in a directory of S: (or S: itself)? */
static int
in_s(const char *path)
{
	BPTR s[MAXSDIRS], l, parent;
	int n, i, r = 0;

	if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) == 0)
		return 0;
	parent = ParentDir(l);
	n = s_dirs(s);
	for (i = 0; i < n; i++)
		if (s[i] && ((parent && SameLock(parent, s[i]) == LOCK_SAME) ||
		    SameLock(l, s[i]) == LOCK_SAME))
			r = 1;
	unlock_all(s, n);
	if (parent)
		UnLock(parent);
	UnLock(l);
	return r;
}

/*
 * The startup scripts: S:Startup-Sequence, S:User-Startup and the
 * scripts in S: they run with Execute (S:Network-Startup and so on),
 * also from lines an earlier switch disabled.  A script elsewhere (on
 * another volume, in a program's drawer) is not edited.
 */
#define	MAXSCRIPTS	12

static char scripts[MAXSCRIPTS][PATHMAX];
static int nscripts;

static void
add_script(const char *p, int n)
{
	char name[PATHMAX];
	int i, k;

	if (n <= 0 || n >= (int)sizeof(name) || nscripts >= MAXSCRIPTS)
		return;
	for (i = 0; i < n; i++)
		name[i] = p[i];
	name[n] = '\0';
	for (k = 0; k < nscripts; k++)
		if (eq_nocase(scripts[k], name))
			return;		/* listed already */
	if (!exists(name) || !in_s(name))
		return;
	cat(scripts[nscripts++], name, "", sizeof(scripts[0]));
}

/* the start of the next word of l[k..n), after blanks */
static int
skip_blanks(const char *l, int k, int n)
{

	while (k < n && (l[k] == ' ' || l[k] == '\t'))
		k++;
	return k;
}

/* a word (quoted or not) at l[k..n): *ws its start, *wl its length;
   returns where the next one may start */
static int
word(const char *l, int k, int n, int *ws, int *wl)
{
	int q = k < n && l[k] == '"', e;

	if (q)
		k++;
	for (e = k; e < n && (q ? l[e] != '"' : (l[e] != ' ' &&
	    l[e] != '\t' && l[e] != '\r' && l[e] != ';')); e++)
		;
	*ws = k;
	*wl = e - k;
	return e < n && q ? e + 1 : e;
}

static int
word_is(const char *l, int ws, int wl, const char *w)
{
	int i;

	for (i = 0; i < wl && w[i]; i++)
		if (lc((UBYTE)l[ws + i]) != lc((UBYTE)w[i]))
			return 0;
	return i == wl && !w[i];
}

/*
 * The command a script line runs, past "Run" and "Execute" and
 * redirections ("Run >NIL: x", "Execute >NIL: S:x"): its start and
 * length in l; *exec set if it is a script run with Execute.  0 if the
 * line runs nothing (empty, a comment).
 */
static int
line_command(const char *l, int n, int *cs, int *cl, int *exec)
{
	int k = 0, ws, wl, e;

	*exec = 0;
	/* only up to a comment (";" outside quotes) */
	for (e = 0, ws = 0; e < n; e++) {
		if (l[e] == '"')
			ws = !ws;
		else if (l[e] == ';' && !ws)
			break;
	}
	n = e;
	for (;;) {
		k = skip_blanks(l, k, n);
		if (k >= n)
			return 0;
		k = word(l, k, n, &ws, &wl);
		if (wl == 0)
			return 0;
		if (l[ws] == '>' || l[ws] == '<')
			continue;		/* a redirection */
		if (word_is(l, ws, wl, "Run") || word_is(l, ws, wl, "RunBack"))
			continue;
		if (!*exec && word_is(l, ws, wl, "Execute")) {
			*exec = 1;
			continue;
		}
		*cs = ws;
		*cl = wl;
		return 1;
	}
}

static void
build_scripts(void)
{
	char *buf, *p, *e;
	LONG len;
	int i, cs, cl, ex;

	nscripts = 0;
	cat(scripts[nscripts++], "S:Startup-Sequence", "", sizeof(scripts[0]));
	cat(scripts[nscripts++], "S:User-Startup", "", sizeof(scripts[0]));
	for (i = 0; i < nscripts; i++) {
		if ((buf = read_file(scripts[i], &len)) == NULL)
			continue;
		for (p = buf; p < buf + len; p = e + 1) {
			const char *q;

			for (e = p; e < buf + len && *e != '\n'; e++)
				;
			q = p;
			while (q < e && (*q == ' ' || *q == '\t'))
				q++;
			if (e - q >= MARKLEN && starts_nocase(q, MARK))
				q += MARKLEN;
			if (line_command(q, e - q, &cs, &cl, &ex) && ex)
				add_script(q + cs, cl);
		}
		FreeVec(buf);
	}
}

/* does the command c[0..n) name one of the stack's commands, from a place
   its commands are in? */
static int
command_of(const struct stack *s, const char *c, int n)
{
	int f, j, k, ok;

	/* its name: after the last ':' or '/' */
	for (f = n; f > 0 && c[f - 1] != ':' && c[f - 1] != '/'; f--)
		;
	/* where from: nowhere said (the Shell's path), C:, S:, or the
	   stack's assign */
	ok = f == 0 || (f == 2 && (lc((UBYTE)c[0]) == 'c' ||
	    lc((UBYTE)c[0]) == 's') && c[1] == ':');
	if (!ok && s->assign) {
		for (k = 0; s->assign[k] && k < f &&
		    lc((UBYTE)c[k]) == lc((UBYTE)s->assign[k]); k++)
			;
		ok = !s->assign[k];
	}
	if (!ok)
		return 0;
	for (j = 0; s->commands[j]; j++)
		if (word_is(c, f, n - f, s->commands[j]))
			return 1;
	return 0;
}

/* a command line (not a comment, not ours) starting a detected stack? */
static struct stack *
line_stack(const char *l, int n, int only_found)
{
	unsigned i;
	int k = 0, cs, cl, ex;

	while (k < n && (l[k] == ' ' || l[k] == '\t'))
		k++;
	if (k == n || l[k] == ';')
		return NULL;
	if (!line_command(l + k, n - k, &cs, &cl, &ex))
		return NULL;
	if (has(l + k + cs, cl, "AmiBSDNet"))
		return NULL;
	for (i = 0; i < NSTACKS; i++) {
		if ((only_found && !stacks[i].found) || !selected(&stacks[i]))
			continue;
		if (command_of(&stacks[i], l + k + cs, cl))
			return &stacks[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------------
 * detection
 */

static void
scan_wbstartup(void)
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
						    stacks[i].wbstartup[j])) {
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
			if (file_exists(stacks[i].files[j])) {
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
				s->found = s->atboot = 1;
				say(scripts[i], p, e - p);
			}
		}
		FreeVec(buf);
	}
	scan_wbstartup();
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

/* a copy, every write checked; a copy that failed is not left behind */
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
		ok = len == 0 || Write(fh, buf, len) == len;
		if (!Close(fh))
			ok = 0;
		if (!ok)
			DeleteFile((CONST_STRPTR)to);
	}
	FreeVec(buf);
	return ok;
}

/*
 * Park (rename into PARKING/sub) or delete a file.  An older parked copy
 * of the same name is kept (as <name>.prev) until the new one is in
 * place, and put back if it cannot be.
 */
static int
park(const char *path, const char *sub, const char *name, int remove)
{
	char dest[PATHMAX], prev[PATHMAX];
	int had;

	if (!exists(path))
		return 0;
	if (remove)
		return DeleteFile((CONST_STRPTR)path) ? 1 : -1;
	make_parking(sub);
	if (slen_(PARKING) + 1 + slen_(sub) + 1 + slen_(name) + 5 >= PATHMAX)
		return -1;
	cat(dest, PARKING "/", sub, sizeof(dest));
	cat(dest, dest, "/", sizeof(dest));
	cat(dest, dest, name, sizeof(dest));
	cat(prev, dest, ".prev", sizeof(prev));
	if ((had = exists(dest)) != 0) {
		DeleteFile((CONST_STRPTR)prev);
		if (!Rename((CONST_STRPTR)dest, (CONST_STRPTR)prev))
			return -1;
	}
	if (Rename((CONST_STRPTR)path, (CONST_STRPTR)dest) ||
	    /* another volume (LIBS: may be assigned anywhere): copy,
	       delete */
	    (copy_file(path, dest) && DeleteFile((CONST_STRPTR)path))) {
		if (had)
			DeleteFile((CONST_STRPTR)prev);
		return 1;
	}
	if (exists(path))
		DeleteFile((CONST_STRPTR)dest);	/* (a copy: the file stays) */
	if (had)
		Rename((CONST_STRPTR)prev, (CONST_STRPTR)dest);
	return -1;
}

/*
 * Replacing a startup file safely: the new text goes to <name>.amibsdnet-new
 * with every Write() checked, and only then replaces the original.  A full
 * disk or a write error leaves the original as it was.
 */
struct safewrite {
	BPTR	fh;
	int	ok;
	char	tmp[PATHMAX];
	const char *name;
};

static int
sw_open(struct safewrite *sw, const char *name)
{

	sw->name = name;
	sw->ok = 1;
	sw->fh = 0;
	/* (a name too long for <name>.amibsdnet-new: not changed) */
	if (!cat_fits(sw->tmp, name, ".amibsdnet-new", sizeof(sw->tmp)))
		return -1;
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
	char old[PATHMAX];
	struct FileInfoBlock *fib;
	LONG prot = -1;
	BPTR l;

	if (!Close(sw->fh))
		sw->ok = 0;
	if (!sw->ok || !cat_fits(old, sw->name, ".amibsdnet-old",
	    sizeof(old))) {
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
	 * The text is never lost: the original is renamed out of the way
	 * ("If the file or directory 'newName' exists, Rename() fails",
	 * dos.doc Rename), so for a moment <name> is missing, and it is put
	 * back if the new one cannot take its place.  A <name>.amibsdnet-old
	 * left from before is the file's text if <name> is missing (a
	 * restore that failed, below): it goes back first; next to a
	 * <name> it is an older text (its deletion at the end failed), and
	 * it goes.
	 */
	if ((l = Lock((CONST_STRPTR)sw->name, ACCESS_READ)) != 0)
		UnLock(l);
	else
		Rename((CONST_STRPTR)old, (CONST_STRPTR)sw->name);
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

/* the first backup of a startup file is the original: made before the
   first change, never overwritten; 0 when it is there */
static int
keep_backup(const char *name)
{
	char backup[PATHMAX];

	if (!cat_fits(backup, name, ".amibsdnet-bak", sizeof(backup)))
		return -1;
	return exists(backup) || copy_file(name, backup) ? 0 : -1;
}

/* which lines to take out: those starting another stack's command;
   mark[] gets one entry per line */
static int
mark_lines(const char *buf, LONG len, UBYTE *mark, int max, int only_found)
{
	const char *p, *e;
	int line = 0, n = 0;

	for (p = buf; p < buf + len && line < max; p = e + 1, line++) {
		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		mark[line] = line_stack(p, e - p, only_found) != NULL;
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
	char *buf = read_file(name, &len), *p, *e;
	UBYTE *mark;
	struct safewrite sw;
	int changed = 0, line;

	if (buf == NULL)
		return exists(name) ? -1 : 0;
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
		 * disabled lines are looked at without their mark, the same
		 * way as active ones.
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
	if (keep_backup(name) != 0 || sw_open(&sw, name) != 0) {
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

/* the WBStartup items of the stacks: moved (or deleted, or back) in
   rounds of MAXITEMS, collected first in each (renaming while scanning
   confuses ExNext()) */
static int
move_wbstartup(int mode)
{
	struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
	char (*names)[108], path[160];
	int n, total = 0, i, rv = 0;
	unsigned s, j;
	BPTR l;

	if (fib == NULL)
		return -1;
	/* big buffers off the stack: NetCtrl may run on a small one */
	if ((names = AllocVec(MAXITEMS * 108, MEMF_ANY)) == NULL) {
		FreeDosObject(DOS_FIB, fib);
		return -1;
	}
	do {
		n = 0;
		if ((l = Lock((CONST_STRPTR)(mode == 2 ? PARKING "/WBStartup" :
		    "SYS:WBStartup"), ACCESS_READ)) != 0) {
			if (Examine(l, fib))
				while (n < MAXITEMS && ExNext(l, fib))
					for (s = 0; s < NSTACKS; s++)
						for (j = 0; stacks[s].wbstartup[j]; j++)
							if ((mode == 2 ||
							    stacks[s].found) &&
							    selected(&stacks[s]) &&
							    starts_nocase((const char *)
							    fib->fib_FileName,
							    stacks[s].wbstartup[j])) {
								cat(names[n++],
								    (const char *)
								    fib->fib_FileName,
								    "", 108);
								s = NSTACKS - 1;
								break;
							}
			UnLock(l);
		}
		for (i = 0; i < n; i++) {
			if (mode == 2) {
				char dest[160];

				cat(path, PARKING "/WBStartup/", names[i],
				    sizeof(path));
				cat(dest, "SYS:WBStartup/", names[i], sizeof(dest));
				if (!Rename((CONST_STRPTR)path, (CONST_STRPTR)dest))
					rv = -1;
			} else {
				cat(path, "SYS:WBStartup/", names[i], sizeof(path));
				if (park(path, "WBStartup", names[i], mode == 1) < 0)
					rv = -1;
			}
		}
		total += n;
	/* (another round only while every item of this one went: what is
	   left is then a new one) */
	} while (n == MAXITEMS && rv == 0);
	FreeDosObject(DOS_FIB, fib);
	FreeVec(names);
	return rv < 0 ? -1 : total;
}

/* ------------------------------------------------------------------------
 * going back (a trial switch that did not connect, or NetCtrl FALLBACK)
 */

#define	OWN_MARK	"; [AmiBSDNet-off] "
#define	OWN_MARKLEN	18

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

/* the ";BEGIN AmiBSDNet" ... ";END AmiBSDNet" block of buf: 1 if there is
   one, 0 if not, -1 if it is damaged (BEGIN twice, or no END: the rest
   of the file would count as inside) */
static int
own_block(const char *buf, LONG len)
{
	const char *p, *e;
	int inside = 0, found = 0;

	for (p = buf; p < buf + len; p = e + 1) {
		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		if (block_line(p, e, ";BEGIN AmiBSDNet")) {
			if (inside)
				return -1;
			inside = found = 1;
		} else if (block_line(p, e, ";END AmiBSDNet"))
			inside = 0;
	}
	return inside ? -1 : found;
}

/*
 * The lines inside ";BEGIN AmiBSDNet" ... ";END AmiBSDNet" of
 * S:User-Startup: commented out with OWN_MARK (on 0), or back (on 1).
 * Not changed if that block is damaged; a backup first.
 */
static int
own_startup_set(int on)
{
	static const char *name = "S:User-Startup";
	LONG len;
	char *buf = read_file(name, &len), *p, *e;
	struct safewrite sw;
	int inside = 0, r, changed = 0;

	if (buf == NULL)
		return exists(name) ? -1 : 0;
	if ((r = own_block(buf, len)) <= 0) {
		FreeVec(buf);
		return r;		/* no block: nothing to do */
	}
	/* anything to change? */
	for (p = buf; p < buf + len; p = e + 1) {
		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		if (block_line(p, e, ";END AmiBSDNet"))
			inside = 0;
		if (inside && (on ? starts_nocase(p, OWN_MARK) : *p != ';'))
			changed = 1;
		if (block_line(p, e, ";BEGIN AmiBSDNet"))
			inside = 1;
	}
	if (!changed) {
		FreeVec(buf);
		return 0;
	}
	if (keep_backup(name) != 0 || sw_open(&sw, name) != 0) {
		FreeVec(buf);
		return -1;
	}
	inside = 0;
	for (p = buf; p < buf + len; p = e + 1) {
		int n;

		for (e = p; e < buf + len && *e != '\n'; e++)
			;
		n = e - p + (e < buf + len);
		if (block_line(p, e, ";END AmiBSDNet"))
			inside = 0;
		if (inside && on && starts_nocase(p, OWN_MARK))
			sw_write(&sw, p + OWN_MARKLEN, n - OWN_MARKLEN);
		else {
			if (inside && !on && *p != ';')
				sw_write(&sw, OWN_MARK, OWN_MARKLEN);
			sw_write(&sw, p, n);
		}
		if (block_line(p, e, ";BEGIN AmiBSDNet"))
			inside = 1;
	}
	FreeVec(buf);
	return sw_close(&sw);
}

static int
own_startup_off(void)
{

	return own_startup_set(0);
}

int
otherstacks_self_on(void)
{
	APTR old = quiet();
	int r = own_startup_set(1);

	loud(old);
	return r < 0 ? -1 : 0;
}


/* ------------------------------------------------------------------------
 * uninstalling (NetCtrl UNINSTALL, the installer's "Uninstall")
 */

#define	INSTALL_LOG	"S:AmiBSDNet-Install.log"

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
	int inside = 0, r;

	if (!exists(name))
		return 0;		/* no S:User-Startup */
	if ((buf = read_file(name, &len)) == NULL)
		return -1;
	if ((r = own_block(buf, len)) <= 0) {
		FreeVec(buf);
		return r;
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

/* the type of the entry itself (ST_*, dos/dosextens.h), as its directory
   lists it: a soft link is not followed (a Lock() on it would be); 0 if
   it is not there */
static LONG
entry_type(const char *path)
{
	struct FileInfoBlock *fib;
	char dir[PATHMAX];
	const char *name = (const char *)FilePart((CONST_STRPTR)path);
	LONG type = 0;
	BPTR l;
	int i, n = (const char *)PathPart((CONST_STRPTR)path) - path;

	if (n >= (int)sizeof(dir) || !name[0])
		return 0;
	for (i = 0; i < n; i++)
		dir[i] = path[i];
	dir[n] = '\0';
	if ((fib = AllocDosObject(DOS_FIB, NULL)) == NULL)
		return 0;
	if ((l = Lock((CONST_STRPTR)dir, ACCESS_READ)) != 0) {
		if (Examine(l, fib))
			while (ExNext(l, fib))
				if (eq_nocase((const char *)fib->fib_FileName,
				    name)) {
					type = fib->fib_DirEntryType;
					break;
				}
		UnLock(l);
	}
	FreeDosObject(DOS_FIB, fib);
	return type;
}

/*
 * A file, or a drawer with everything in it: only for AmiBSDNet's own
 * drawers (the log drawer, the parking drawer, its settings).  A hard
 * link is deleted as the link ("If one of the links (or the original
 * entry for the file) is deleted, the data remains until there are no
 * links left", dos.doc MakeLink); a soft link is left alone (what
 * DeleteFile() does with one dos.doc does not say) and counts as not
 * deleted.
 */
static int
delete_tree(const char *path, LONG type)
{
	struct FileInfoBlock *fib;
	char sub[PATHMAX], (*names)[108] = NULL;
	LONG *types = NULL;
	BPTR l;
	int n, i, rv = 0;

	if (type == 0)
		return 0;		/* not there */
	if (type == ST_SOFTLINK)
		return -1;
	if (type != ST_USERDIR) {
		SetProtection((CONST_STRPTR)path, 0);
		return DeleteFile((CONST_STRPTR)path) ? 0 : -1;
	}
	if ((fib = AllocDosObject(DOS_FIB, NULL)) == NULL)
		return -1;
	if ((names = AllocVec(64 * 108, MEMF_ANY)) != NULL &&
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
				if (delete_tree(sub, types[i]) != 0)
					rv = -1;
			}
		} while (n == 64 && rv == 0);
	} else
		rv = -1;
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

static int
delete_all(const char *path)
{

	return delete_tree(path, entry_type(path));
}

/*
 * One entry the installer listed, deleted on its own: a file, a hard
 * link as the link, a drawer only if it is empty (what someone else put
 * there stays, and so does the drawer); a soft link is left alone (see
 * delete_tree()).  0: gone (or was not there), 1: a drawer that is not
 * empty (kept), -1: could not be deleted.
 */
static int
delete_one(const char *path)
{
	LONG err;

	if (entry_type(path) == ST_SOFTLINK)
		return -1;
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
 * not a volume or assign itself, no parent ("/"), and nothing in a
 * directory of S: (wherever S: is assigned: SYS:S/... too) or a startup
 * script build_scripts() found (the log itself is deleted separately).
 */
static int
log_path_ok(const char *p)
{
	int i, colon = -1, k, script = 0;
	BPTR l, sl;

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
	if (in_s(p))
		return 0;
	if ((l = Lock((CONST_STRPTR)p, ACCESS_READ)) != 0) {
		for (k = 0; k < nscripts; k++)
			if ((sl = Lock((CONST_STRPTR)scripts[k], ACCESS_READ))
			    != 0) {
				if (SameLock(l, sl) == LOCK_SAME)
					script = 1;
				UnLock(sl);
			}
		UnLock(l);
	}
	return !script;
}

/* what is always AmiBSDNet's (C:Ping is not: the name is common, and the
   install log lists AmiBSDNet's with its size and CRC) */
#define	SELF	"C:NetCtrl"	/* the uninstaller itself */

static const char *const own_files[] = {
	"C:AmiBSDNet", "C:SerialShell",		/* (C:NetCtrl: SELF) */
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
	"ThirdParty.txt", "ThirdParty.txt.info", "NetBSD.txt", "NetBSD.txt.info",
	"AmiBSDNetStatus",
	"AmiBSDNetStatus.info", NULL
};

/*
 * A "FILE <path>" line of the log, or "FILE <path> <size> <crc32>": the
 * file as the installer created it (size in decimal, CRC-32 in 8 hex
 * digits, as NetCtrl CHECKSUM prints them).  p..p+k is what follows
 * "FILE "; returns the path's length; *sum is set if size and CRC are
 * there.
 */
static int
log_line(const char *p, int k, int *sum, ULONG *size, ULONG *crc)
{
	int e = k, i, h;
	ULONG v;

	*sum = 0;
	/* the CRC: 8 hex digits after the last blank */
	for (i = e; i > 0 && p[i - 1] != ' '; i--)
		;
	if (e - i != 8 || i < 3)
		return k;
	for (v = 0, h = i; h < e; h++) {
		int c = lc((UBYTE)p[h]);

		if (c >= '0' && c <= '9')
			v = v << 4 | (c - '0');
		else if (c >= 'a' && c <= 'f')
			v = v << 4 | (c - 'a' + 10);
		else
			return k;
	}
	*crc = v;
	/* the size: digits before it */
	e = i - 1;
	for (i = e; i > 0 && p[i - 1] >= '0' && p[i - 1] <= '9'; i--)
		;
	if (i == e || i < 2 || p[i - 1] != ' ' || e - i > 10)
		return k;
	for (v = 0, h = i; h < e; h++)
		v = v * 10 + (p[h] - '0');
	*size = v;
	*sum = 1;
	return i - 1;
}

int
otherstacks_uninstall(char *msg, int size)
{
	char *list = NULL, *p, *e, *s, path[PATHMAX];
	LONG len = 0;
	APTR old;
	int rv = 0, kept = 0, changed = 0, parked = 0, i, k, r, pass, icon,
	    restored, sum, bakkept = 0;
	ULONG fsize, fcrc;
	unsigned long nsize, ncrc;
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
	/* another stack an earlier switch took out of the boot: back (this
	   also finds the startup scripts, for log_path_ok()) */
	restored = otherstacks_apply(OTHERS_RESTORE) == 0;
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
	if (exists(INSTALL_LOG) &&
	    (list = read_file(INSTALL_LOG, &len)) == NULL)
		rv = -1;
	if (list) {
		/* (four rounds, below: files (0), the icons whose file is
		   gone (1), drawers (2) - empty by then of what was listed,
		   icons too - and the icons whose drawer is gone (3); an
		   icon whose file or drawer stays, stays too) */
		for (pass = 0; pass < 4; pass++)
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
			k = log_line(p + 5, k, &sum, &fsize, &fcrc);
			if (k >= (int)sizeof(path)) {
				if (pass == 0)
					rv = -1;	/* too long: not cut short */
				continue;
			}
			for (i = 0; i < k; i++)
				path[i] = p[5 + i];
			path[k] = '\0';
			icon = k > 5 && starts_nocase(path + k - 5, ".info");
			if (!log_path_ok(path) || eq_nocase(path, SELF) ||
			    icon != (pass & 1) ||
			    (!icon && (entry_type(path) > 0) != (pass == 2)))
				continue;
			if (icon) {
				/* its file or drawer (the name without
				   ".info") is still there: the icon stays */
				path[k - 5] = '\0';
				if (exists(path))
					continue;
				path[k - 5] = '.';
			}
			/* changed since it was installed (a hosts file, the
			   Wi-Fi networks): the user's now, it stays */
			if (sum && entry_type(path) < 0 &&
			    (drv_file_sum(path, &nsize, &ncrc) != 0 ||
			    nsize != fsize || ncrc != fcrc)) {
				changed = 1;
				continue;
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
	/* the parking drawer: AmiBSDNet's own parked parts go; another
	   stack's parked files only went back if all of it was restored,
	   and what is still there stays (only empty drawers go) */
	if (delete_all(PARKING "/AmiBSDNet") != 0)
		rv = -1;
	if (restored) {
		if (delete_one(PARKING "/WBStartup") > 0 ||
		    delete_one(PARKING "/Libs") > 0)
			parked = 1;
		if (delete_one(PARKING) > 0)
			parked = 1;
	}
	/* the backups of the startup files it changed (S:*.amibsdnet-*):
	   only when all went well - after a failed restore they are the
	   only copies of the original startup files.  Those REMOVEROADSHOW
	   made (*.amibsdnet-rs) stay: Roadshow is not put back.  A
	   <name>.amibsdnet-bak goes only if <name> is the same again: lines
	   a removal deleted (OTHERS_REMOVE, REMOVEROADSHOW) are not put
	   back, and then it is the only copy of them */
	if (restored && rv == 0 &&
	    (fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
		char (*bak)[108] = AllocVec(32 * 108, MEMF_ANY);
		int nb = 0;

		if (bak && (l = Lock((CONST_STRPTR)"S:", ACCESS_READ)) != 0) {
			if (Examine(l, fib))
				while (nb < 32 && ExNext(l, fib)) {
					const char *fn = (const char *)
					    fib->fib_FileName;
					int fl = slen_(fn);

					if (fib->fib_DirEntryType < 0 &&
					    has(fn, fl, ".amibsdnet-") &&
					    !(fl > 3 && eq_nocase(fn + fl - 3,
					    "-rs")))
						cat(bak[nb++], fn, "", 108);
				}
			UnLock(l);
		}
		for (i = 0; i < nb; i++) {
			int bl = slen_(bak[i]);
			char orig[PATHMAX];

			cat(path, "S:", bak[i], sizeof(path));
			if (bl > 14 && eq_nocase(bak[i] + bl - 14,
			    ".amibsdnet-bak")) {
				cat(orig, path, "", sizeof(orig));
				orig[slen_(orig) - 14] = '\0';
				if (!same_file(path, orig)) {
					bakkept = 1;
					continue;
				}
			}
			delete_one(path);
		}
		if (bak)
			FreeVec(bak);
		FreeDosObject(DOS_FIB, fib);
	}
	/* NetCtrl and the log last, and only if all went well: the message
	   below tells to run "NetCtrl UNINSTALL" again, and that run needs
	   both */
	if (rv == 0 && delete_one(SELF) < 0)
		rv = -1;
	if (rv == 0)
		delete_one(INSTALL_LOG);
	loud(old);
	cat(msg, rv == 0 ? "AmiBSDNet is removed. Reboot: the stack still in "
	    "memory goes then." : "AmiBSDNet is removed, but some files could "
	    "not be deleted (in use, protected, or a soft link?).\nReboot "
	    "and run \"NetCtrl UNINSTALL\" again, or delete them by hand.",
	    "", size);
	if (kept)
		cat(msg, msg, "\nA drawer it had created has other files in it "
		    "now: that drawer and those files were left alone.", size);
	if (changed)
		cat(msg, msg, "\nFiles it had installed that were changed since "
		    "(such as DEVS:Internet/hosts or Wireless.prefs) were left "
		    "alone.", size);
	if (!restored)
		cat(msg, msg, "\nThe stack it had switched from could not be put "
		    "back completely: its parked files are still in "
		    "SYS:Storage/AmiBSDNet-Disabled, and S:*.amibsdnet-bak are "
		    "the original startup files.", size);
	else if (bakkept)
		cat(msg, msg, "\nS:*.amibsdnet-bak (the startup files as they "
		    "were before) were kept: the startup files are not the same "
		    "now.", size);
	if (restored && parked)
		cat(msg, msg, "\nSYS:Storage/AmiBSDNet-Disabled still has "
		    "files in it that were not put back: they were left "
		    "alone.", size);
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
			   own block is not taken out; NetCtrl REENABLE puts
			   the block's lines back) */
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
		 * AmiBSDNet stays (the Wi-Fi driver is untouched): the trial ends
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
		    "No other TCP/IP stack would start at boot (by a startup\n"
		    "line or a WBStartup item): AmiBSDNet stays. Another stack's\n"
		    "LIBS:bsdsocket.library or WBStartup items, if there are\n"
		    "any, are in SYS:Storage/AmiBSDNet-Disabled. AmiBSDNet\n"
		    "starts at every boot (reboot now if it is not running)." :
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
	cat(msg, msg, "\n\"NetCtrl REENABLE\" puts AmiBSDNet back into "
	    "S:User-Startup.", size);
	return rv;
}

int
otherstacks_apply(int mode)
{
	char found[128];
	APTR old;
	int i, rv = 0;

	if (mode != 2)
		otherstacks_check(found, sizeof(found));
	old = quiet();
	startup_rv = 0;
	build_scripts();
	for (i = 0; i < nscripts; i++)
		if (edit_startup(scripts[i], mode) < 0)
			rv = startup_rv = -1;
	if (move_wbstartup(mode) < 0)
		rv = -1;
	if (mode == 2) {
		/* the parked library back; if there is a LIBS:bsdsocket.library
		   again, it is not put back, and that is not a full restore:
		   the parked one stays */
		if (exists(PARKING "/Libs/bsdsocket.library")) {
			if (exists("LIBS:bsdsocket.library"))
				rv = -1;
			else if (!Rename((CONST_STRPTR)PARKING
			    "/Libs/bsdsocket.library",
			    (CONST_STRPTR)"LIBS:bsdsocket.library") &&
			    !(copy_file(PARKING "/Libs/bsdsocket.library",
			    "LIBS:bsdsocket.library") &&
			    DeleteFile((CONST_STRPTR)PARKING
			    "/Libs/bsdsocket.library")))
				rv = -1;
		}
	} else if (otherstacks_only == NULL &&
	    park("LIBS:bsdsocket.library", "Libs", "bsdsocket.library",
	    mode == 1) < 0)
		rv = -1;
	loud(old);
	return rv;
}
