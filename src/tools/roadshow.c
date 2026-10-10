/*
 * Removing Roadshow completely (NetCtrl FINDROADSHOW / REMOVEROADSHOW,
 * the installer's "Remove Roadshow" choice).
 *
 * Every mounted volume is searched, every drawer, for what belongs to
 * Roadshow (also its trial version):
 *   - drawers and files whose name contains "Roadshow" (with their icons)
 *   - its commands (AddNetInterface, RoadshowControl, ...) and scripts
 *     (Network-Startup...), the NetInterfaces drawer
 *   - every bsdsocket.library on disk (AmiBSDNet's lives in memory only)
 *   - its files in DEVS:Internet, except the hosts file AmiBSDNet uses
 *   - what an earlier switch parked in SYS:Storage/AmiBSDNet-Disabled
 * Nothing on a volume called Work, or in a drawer called Work, is
 * touched: that is where the Roadshow installer is kept.  Removing
 * deletes for good.  The startup-script lines are removed by
 * otherstacks_apply(OTHERS_REMOVE).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "roadshow.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define	MAXDEPTH	16
#define	MAXFOUND	400
#define	PATHMAX		256
#define	PARKING		"SYS:Storage/AmiBSDNet-Disabled"

/* Roadshow's commands (names only Roadshow uses) */
static const char *const tools[] = {
	"AddNetInterface", "AddNetRoute", "ConfigureNetInterface",
	"DeleteNetRoute", "GetNetStatus", "NetLogViewer", "NetShutdown",
	"RemoveNetInterface", "RoadshowControl", "ShowNetStatus",
	"SampleNetSpeed", NULL
};

/* what AmiBSDNet keeps of DEVS:Internet */
static const char *const keep_internet[] = { "hosts", "hosts.info", NULL };

static char **found;
static int nfound, ovf;

static int
lc(int c)
{

	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int
eq(const char *a, const char *b)
{

	while (*a && lc((UBYTE)*a) == lc((UBYTE)*b))
		a++, b++;
	return *a == *b;
}

static int
contains(const char *s, const char *w)
{
	int i;

	for (; *s; s++) {
		for (i = 0; w[i] && lc((UBYTE)s[i]) == lc((UBYTE)w[i]); i++)
			;
		if (!w[i])
			return 1;
	}
	return 0;
}

static int
starts(const char *s, const char *p)
{

	while (*p && lc((UBYTE)*s) == lc((UBYTE)*p))
		s++, p++;
	return !*p;
}

static int
slen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

static void
scpy(char *d, const char *s, int n)
{

	while (--n > 0 && *s)
		*d++ = *s++;
	*d = '\0';
}

static int
Lock_exists(const char *path)
{
	BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);

	if (l)
		UnLock(l);
	return l != 0;
}

/* name without a trailing ".info" (for icons) */
static void
base_name(const char *name, char *out, int n)
{
	int l = slen(name);

	scpy(out, name, n);
	if (l > 5 && eq(name + l - 5, ".info") && l - 5 < n)
		out[l - 5] = '\0';
}

/* AmiBSDNet's own (its settings mention Roadshow): never touched - but
   the drawer an earlier switch parked Roadshow in is Roadshow's */
static int
ours(const char *name)
{

	return starts(name, "AmiBSDNet") && !eq(name, "AmiBSDNet-Disabled");
}

/* does this entry belong to Roadshow? */
static int
is_roadshow(const char *name, int dir, int indevsinternet)
{
	char b[108];
	int i;

	base_name(name, b, sizeof(b));
	if (contains(b, "Roadshow"))
		return 1;
	if (eq(b, "NetInterfaces"))		/* (also its icon) */
		return 1;
	if (!dir && eq(name, "bsdsocket.library"))
		return 1;
	if (starts(b, "Network-Startup"))
		return 1;
	for (i = 0; tools[i]; i++)
		if (eq(b, tools[i]))
			return 1;
	/* its settings in DEVS:Internet itself (found by lock, not by
	   name: other drawers called Internet are not Roadshow's), not the
	   hosts file */
	if (!dir && indevsinternet) {
		for (i = 0; keep_internet[i]; i++)
			if (eq(name, keep_internet[i]))
				return 0;
		return 1;
	}
	return 0;
}

static void
add_found(const char *path, int dir)
{
	int l = slen(path);

	if (nfound >= MAXFOUND) {
		ovf = 1;
		return;
	}
	if ((found[nfound] = AllocVec(l + 2, MEMF_ANY)) == NULL) {
		ovf = 1;
		return;
	}
	found[nfound][0] = dir ? 'D' : 'F';
	scpy(found[nfound] + 1, path, l + 1);
	nfound++;
}

/* the drawers of the other TCP/IP stacks: their own bsdsocket.library
   is not Roadshow's */
static int
other_stack_drawer(const char *n)
{

	return starts(n, "Miami") || starts(n, "AmiTCP") || starts(n, "Genesis");
}

static BPTR devs_internet;	/* DEVS:Internet: Roadshow's settings there */

/* does a drawer hold a drawer called Work (somewhere inside)? */
static int
has_work(const char *path, int depth)
{
	struct FileInfoBlock *fib;
	char sub[PATHMAX];
	BPTR l;
	int found_work = 0;

	if (depth > MAXDEPTH || (fib = AllocDosObject(DOS_FIB, NULL)) == NULL)
		return 0;
	if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) != 0) {
		if (Examine(l, fib) && fib->fib_DirEntryType > 0)
			while (!found_work && ExNext(l, fib)) {
				if (fib->fib_DirEntryType != ST_USERDIR)
					continue;
				if (eq((const char *)fib->fib_FileName, "Work")) {
					found_work = 1;
					break;
				}
				if (slen(path) + slen((const char *)fib->fib_FileName)
				    + 2 >= PATHMAX)
					continue;
				scpy(sub, path, sizeof(sub));
				scpy(sub + slen(sub), "/", sizeof(sub) - slen(sub));
				scpy(sub + slen(sub), (const char *)fib->fib_FileName,
				    sizeof(sub) - slen(sub));
				found_work = has_work(sub, depth + 1);
			}
		UnLock(l);
	}
	FreeDosObject(DOS_FIB, fib);
	return found_work;
}

/*
 * Walk one drawer: what matches is collected (drawers whole), the rest
 * searched further.  Inside a Roadshow drawer (inrs) everything counts,
 * except a Work drawer: a Roadshow drawer that holds one is not taken
 * whole, its other contents are.  Links are never followed.
 */
static void
walk(char *path, int depth, int inrs)
{
	struct FileInfoBlock *fib;
	BPTR l;
	int pl = slen(path), innet;

	if (depth > MAXDEPTH || ovf)
		return;
	if ((fib = AllocDosObject(DOS_FIB, NULL)) == NULL)
		return;
	if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) != 0) {
		innet = devs_internet && SameLock(l, devs_internet) == LOCK_SAME;
		if (Examine(l, fib) && fib->fib_DirEntryType > 0) {
			while (ExNext(l, fib)) {
				const char *n = (const char *)fib->fib_FileName;
				LONG t = fib->fib_DirEntryType;
				int dir = t == ST_USERDIR;

				if (t == ST_SOFTLINK || t == ST_LINKDIR ||
				    t == ST_LINKFILE)
					continue;
				if (pl + slen(n) + 2 >= PATHMAX)
					continue;
				if (path[pl - 1] != ':')
					path[pl] = '/', scpy(path + pl + 1, n,
					    PATHMAX - pl - 1);
				else
					scpy(path + pl, n, PATHMAX - pl);
				if ((dir && eq(n, "Work")) || ours(n) ||
				    (dir && !inrs && other_stack_drawer(n)))
					;	/* the Roadshow installer, ours */
				else if (inrs || is_roadshow(n, dir, innet)) {
					if (dir && has_work(path, 0))
						walk(path, depth + 1, 1);
					else
						add_found(path, dir);
				} else if (dir)
					walk(path, depth + 1, 0);
				path[pl] = '\0';
			}
		}
		UnLock(l);
	}
	FreeDosObject(DOS_FIB, fib);
}

/*
 * Delete a drawer with everything in it.  Only real drawers are entered:
 * a link is deleted as a link, never followed (that would empty its
 * target).  A drawer called Work and AmiBSDNet's own are kept, also deep
 * inside; the drawers around them stay then.  Returns 0 when it is gone,
 * 1 when something was kept, -1 if something could not be deleted.
 */
#define	DELBATCH	128

static int
delete_tree(const char *path)
{
	struct FileInfoBlock *fib;
	char (*names)[108], sub[PATHMAX];
	LONG *types;
	BPTR l;
	int rv = 0, kept = 0, n, i, progress;

	names = AllocVec(DELBATCH * 108, MEMF_ANY);
	types = AllocVec(DELBATCH * sizeof(LONG), MEMF_ANY);
	fib = AllocDosObject(DOS_FIB, NULL);
	if (names == NULL || types == NULL || fib == NULL) {
		rv = -1;
		goto out;
	}
	/* collected first (deleting during ExNext() would lose its place),
	   in batches for big drawers */
	for (;;) {
		n = 0;
		if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) == 0)
			break;
		if (Examine(l, fib) && fib->fib_DirEntryType > 0)
			while (n < DELBATCH && ExNext(l, fib)) {
				scpy(names[n], (const char *)fib->fib_FileName, 108);
				types[n] = fib->fib_DirEntryType;
				n++;
			}
		UnLock(l);
		progress = 0;
		for (i = 0; i < n; i++) {
			if (eq(names[i], "Work") || ours(names[i])) {
				kept = 1;
				continue;
			}
			if (slen(path) + slen(names[i]) + 2 >= PATHMAX) {
				rv = -1;	/* (a cut path would be another) */
				continue;
			}
			scpy(sub, path, sizeof(sub));
			scpy(sub + slen(sub), "/", sizeof(sub) - slen(sub));
			scpy(sub + slen(sub), names[i], sizeof(sub) - slen(sub));
			if (types[i] == ST_USERDIR) {
				int r = delete_tree(sub);

				if (r < 0)
					rv = -1;
				else if (r > 0)
					kept = 1;
				else
					progress++;
			} else {
				/* files, and links of either kind */
				SetProtection((CONST_STRPTR)sub, 0);
				if (DeleteFile((CONST_STRPTR)sub))
					progress++;
				else
					rv = -1;
			}
		}
		/* a full batch: there may be more */
		if (n < DELBATCH || progress == 0)
			break;
	}
	if (!kept && rv == 0) {
		SetProtection((CONST_STRPTR)path, 0);
		if (!DeleteFile((CONST_STRPTR)path))
			rv = -1;
	}
out:
	if (fib)
		FreeDosObject(DOS_FIB, fib);
	if (types)
		FreeVec(types);
	if (names)
		FreeVec(names);
	return rv < 0 ? -1 : kept;
}

/* ------------------------------------------------------------------------
 * lines that mention Roadshow in text and configuration files
 */

#define	TEXTMAX		(512 * 1024)	/* bigger ones are no settings */
#define	SCRUBMAX	1000		/* entries of one drawer */

static int scrubbed, scrubfails;
static char *slist;
static int slistsize, sll;

static void
list_add(const char *s)
{
	int l = slen(s);

	if (slist && sll + l + 2 < slistsize) {
		scpy(slist + sll, s, slistsize - sll);
		sll += l;
		slist[sll++] = '\n';
		slist[sll] = '\0';
	}
}

/* a text file (no NUL bytes, mostly printable)? */
static int
is_text(const UBYTE *b, LONG n)
{
	LONG i, odd = 0;

	for (i = 0; i < n; i++) {
		if (b[i] == 0)
			return 0;
		if (b[i] < 32 && b[i] != '\n' && b[i] != '\r' && b[i] != '\t' &&
		    b[i] != 27 && b[i] != 12)
			odd++;
	}
	return odd * 20 < n + 1;
}

static int
line_mentions(const char *p, const char *e)
{
	const char *w = "roadshow";
	int i;

	for (; p + 8 <= e; p++) {
		for (i = 0; w[i] && lc((UBYTE)p[i]) == w[i]; i++)
			;
		if (!w[i])
			return 1;
	}
	return 0;
}

/*
 * What to do with a line that mentions Roadshow: 0 leave it, 1 remove
 * it, 2 keep only what comes before its comment.  A comment line goes.
 * In a script a command stays (removing an EndIf or Else would break the
 * script; Roadshow's own commands were removed by otherstacks.c): only
 * a comment after it goes.  In a configuration file the line goes.
 */
static int
line_action(const char *p, const char *e, int script, const char **cut)
{
	const char *q = p, *c;
	int quote = 0;

	if (!line_mentions(p, e))
		return 0;
	while (q < e && (*q == ' ' || *q == '\t'))
		q++;
	/* (in AmigaDOS scripts only ';' starts a comment) */
	if (q < e && (*q == ';' || (!script && *q == '#')))
		return 1;
	if (!script)
		return 1;
	for (c = q; c < e; c++) {
		if (*c == '"')
			quote = !quote;
		else if (*c == ';' && !quote)
			break;
	}
	if (c < e && !line_mentions(p, c)) {
		while (c > p && (c[-1] == ' ' || c[-1] == '\t'))
			c--;
		*cut = c;
		return 2;
	}
	return 0;
}

static void
scrub_file(const char *path, LONG size, LONG prot, int remove, int script)
{
	char *buf, *p, *e, tmp[PATHMAX], old[PATHMAX], msg[PATHMAX + 32];
	const char *cut;
	BPTR fh;
	LONG n = 0, got;
	int changes = 0, left = 0, k, a;

	if (size <= 0 || (buf = AllocVec(size + 1, MEMF_ANY)) == NULL)
		return;
	if ((fh = Open((CONST_STRPTR)path, MODE_OLDFILE)) == 0) {
		FreeVec(buf);
		return;
	}
	while (n < size && (got = Read(fh, buf + n, size - n)) > 0)
		n += got;
	Close(fh);
	if (n != size || !is_text((UBYTE *)buf, n)) {
		FreeVec(buf);
		return;
	}
	for (p = buf; p < buf + n; p = e + 1) {
		for (e = p; e < buf + n && *e != '\n'; e++)
			;
		a = line_action(p, e, script, &cut);
		if (a)
			changes++;
		else if (line_mentions(p, e))
			left++;
	}
	if (changes == 0 && left == 0) {
		FreeVec(buf);
		return;
	}
	scpy(msg, path, sizeof(msg));
	if (changes)
		scpy(msg + slen(msg), remove ? "  (lines removed)" : "  (lines)",
		    sizeof(msg) - slen(msg));
	if (left)
		scpy(msg + slen(msg), "  (commands left as they are)",
		    sizeof(msg) - slen(msg));
	list_add(msg);
	if (changes)
		scrubbed++;
	if (!remove || changes == 0) {
		FreeVec(buf);
		return;
	}

	/* the new text next to the file first; the file is never missing:
	   renamed aside, replaced, put back if that fails */
	scpy(tmp, path, sizeof(tmp));
	for (k = slen(tmp); k > 0 && tmp[k - 1] != '/' && tmp[k - 1] != ':'; k--)
		;
	scpy(old, tmp, k + 1);
	scpy(tmp + k, "AmiBSDNet.tmp", sizeof(tmp) - k);
	scpy(old + k, "AmiBSDNet.old", sizeof(old) - k);
	if ((fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE)) == 0) {
		scrubfails++;
		FreeVec(buf);
		return;
	}
	k = 1;
	for (p = buf; p < buf + n; p = e + 1) {
		LONG l;

		for (e = p; e < buf + n && *e != '\n'; e++)
			;
		a = line_action(p, e, script, &cut);
		if (a == 1)
			continue;
		if (a == 2) {
			l = cut - p;
			if ((l > 0 && Write(fh, p, l) != l) ||
			    (e < buf + n && Write(fh, "\n", 1) != 1))
				k = 0;
			continue;
		}
		l = e - p + (e < buf + n);
		if (l > 0 && Write(fh, p, l) != l)
			k = 0;
	}
	if (!Close(fh))
		k = 0;
	DeleteFile((CONST_STRPTR)old);
	if (!k || !Rename((CONST_STRPTR)path, (CONST_STRPTR)old)) {
		DeleteFile((CONST_STRPTR)tmp);
		scrubfails++;
	} else if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path)) {
		Rename((CONST_STRPTR)old, (CONST_STRPTR)path);
		DeleteFile((CONST_STRPTR)tmp);
		scrubfails++;
	} else {
		SetProtection((CONST_STRPTR)path, prot);	/* (s bit) */
		SetProtection((CONST_STRPTR)old, 0);
		DeleteFile((CONST_STRPTR)old);
	}
	FreeVec(buf);
}

/* the text files of a drawer and its subdrawers (not "Work") */
static void
scrub_walk(char *path, int depth, int remove, int script)
{
	struct FileInfoBlock *fib;
	char (*names)[108];
	LONG *sizes, *prots;
	UBYTE *isdir;
	BPTR l;
	int pl = slen(path), n = 0, i;

	if (depth > MAXDEPTH)
		return;
	fib = AllocDosObject(DOS_FIB, NULL);
	names = AllocVec(SCRUBMAX * 108, MEMF_ANY);
	sizes = AllocVec(SCRUBMAX * sizeof(LONG), MEMF_ANY);
	prots = AllocVec(SCRUBMAX * sizeof(LONG), MEMF_ANY);
	isdir = AllocVec(SCRUBMAX, MEMF_ANY);
	if (fib && names && sizes && prots && isdir &&
	    (l = Lock((CONST_STRPTR)path, ACCESS_READ)) != 0) {
		/* collected first: rewriting a file during ExNext() could
		   confuse it */
		if (Examine(l, fib) && fib->fib_DirEntryType > 0)
			while (n < SCRUBMAX && ExNext(l, fib)) {
				LONG t = fib->fib_DirEntryType;

				/* (links are not followed) */
				if (t == ST_SOFTLINK || t == ST_LINKDIR ||
				    t == ST_LINKFILE)
					continue;
				scpy(names[n], (const char *)fib->fib_FileName, 108);
				sizes[n] = fib->fib_Size;
				prots[n] = fib->fib_Protection;
				isdir[n] = t == ST_USERDIR;
				n++;
			}
		UnLock(l);
		for (i = 0; i < n; i++) {
			int ln = slen(names[i]);

			if (pl + ln + 2 >= PATHMAX)
				continue;
			if (path[pl - 1] != ':')
				path[pl] = '/', scpy(path + pl + 1, names[i],
				    PATHMAX - pl - 1);
			else
				scpy(path + pl, names[i], PATHMAX - pl);
			if (ours(names[i]))
				;
			else if (isdir[i]) {
				if (!eq(names[i], "Work"))
					scrub_walk(path, depth + 1, remove,
					    script);
			} else if (!(ln > 5 && eq(names[i] + ln - 5, ".info")) &&
			    sizes[i] <= TEXTMAX)
				scrub_file(path, sizes[i], prots[i], remove, script);
			path[pl] = '\0';
		}
	}
	if (isdir)
		FreeVec(isdir);
	if (prots)
		FreeVec(prots);
	if (sizes)
		FreeVec(sizes);
	if (names)
		FreeVec(names);
	if (fib)
		FreeDosObject(DOS_FIB, fib);
}

/*
 * Assigns: one named like Roadshow ("Assign Roadshow: ...") or pointing
 * into a drawer that is going keeps that drawer locked, so it could not
 * be deleted.  Removed first.
 */
static void
remove_assigns(void)
{
	char (*names)[40], target[PATHMAX];
	struct DosList *dl;
	int n = 0, i, j;

	if ((names = AllocVec(32 * 40, MEMF_CLEAR)) == NULL)
		return;
	dl = LockDosList(LDF_ASSIGNS | LDF_READ);
	while ((dl = NextDosEntry(dl, LDF_ASSIGNS)) != NULL && n < 32) {
		const UBYTE *b = BADDR(dl->dol_Name);
		int k, gone = 0;

		if (b == NULL || b[0] == 0 || b[0] > 36)
			continue;
		for (k = 0; k < b[0]; k++)
			names[n][k] = b[k + 1];
		names[n][k] = '\0';
		if (contains(names[n], "Roadshow"))
			gone = 1;
		else if (dl->dol_Type == DLT_DIRECTORY && dl->dol_Lock &&
		    NameFromLock(dl->dol_Lock, (STRPTR)target, sizeof(target)))
			for (j = 0; j < nfound && !gone; j++) {
				const char *f = found[j] ? found[j] + 1 : "";
				int fl = slen(f);

				/* the drawer itself, or a drawer inside it */
				if (found[j] && found[j][0] == 'D' &&
				    starts(target, f) && (target[fl] == '\0' ||
				    target[fl] == '/'))
					gone = 1;
			}
		if (gone)
			n++;
	}
	UnLockDosList(LDF_ASSIGNS | LDF_READ);
	for (i = 0; i < n; i++)
		AssignLock((CONST_STRPTR)names[i], 0);
	FreeVec(names);
}

int
roadshow_find(int remove, char *list, int listsize, int *failed)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR oldwin = me->pr_WindowPtr;
	char (*vols)[40], path[PATHMAX];
	struct DosList *dl;
	int nvols = 0, i, n, fails = 0, ll = 0;

	*failed = 0;
	if (list && listsize)
		list[0] = '\0';
	if ((found = AllocVec(MAXFOUND * sizeof(char *), MEMF_CLEAR)) == NULL ||
	    (vols = AllocVec(32 * 40, MEMF_CLEAR)) == NULL) {
		if (found)
			FreeVec(found);
		return -1;
	}
	nfound = ovf = 0;
	me->pr_WindowPtr = (APTR)-1;

	/* the mounted volumes (not a volume called Work) */
	dl = LockDosList(LDF_VOLUMES | LDF_READ);
	while ((dl = NextDosEntry(dl, LDF_VOLUMES)) != NULL && nvols < 32) {
		const UBYTE *b = BADDR(dl->dol_Name);
		int k;

		if (b == NULL || b[0] == 0 || b[0] > 36 || dl->dol_Task == NULL)
			continue;
		for (k = 0; k < b[0]; k++)
			vols[nvols][k] = b[k + 1];
		vols[nvols][k] = '\0';
		if (eq(vols[nvols], "PaulaNET"))
			continue;
		nvols++;
	}
	UnLockDosList(LDF_VOLUMES | LDF_READ);
	/* a volume called Work is skipped - unless the system itself is on
	   it (then only its Work drawers are) */
	{
		BPTR sys = Lock((CONST_STRPTR)"SYS:", ACCESS_READ);

		for (i = 0; i < nvols; i++)
			if (eq(vols[i], "Work")) {
				char v[44];
				BPTR w;
				int same = 0;

				scpy(v, vols[i], sizeof(v) - 2);
				scpy(v + slen(v), ":", 2);
				if (sys && (w = Lock((CONST_STRPTR)v, ACCESS_READ))) {
					same = SameLock(sys, w) == LOCK_SAME;
					UnLock(w);
				}
				if (!same)
					vols[i][0] = '\0';
			}
		if (sys)
			UnLock(sys);
	}
	devs_internet = Lock((CONST_STRPTR)"DEVS:Internet", ACCESS_READ);

	for (i = 0; i < nvols; i++) {
		if (!vols[i][0])
			continue;
		scpy(path, vols[i], sizeof(path) - 2);
		scpy(path + slen(path), ":", 2);
		walk(path, 0, 0);
	}
	if (devs_internet) {
		UnLock(devs_internet);
		devs_internet = 0;
	}
	if (remove)
		remove_assigns();
	n = 0;
	for (i = 0; i < nfound; i++) {
		int j, dup = 0;

		/* the same object found through an assign: once */
		for (j = 0; j < i && !dup; j++)
			if (found[j] && eq(found[j], found[i]))
				dup = 1;
		if (dup) {
			FreeVec(found[i]);
			found[i] = NULL;
			continue;
		}
		n++;
		if (list && ll + slen(found[i] + 1) + 2 < listsize) {
			scpy(list + ll, found[i] + 1, listsize - ll);
			ll += slen(list + ll);
			list[ll++] = '\n';
			list[ll] = '\0';
		}
		if (remove) {
			int r;

			if (found[i][0] == 'D')
				r = delete_tree(found[i] + 1);
			else {
				SetProtection((CONST_STRPTR)found[i] + 1, 0);
				r = DeleteFile((CONST_STRPTR)found[i] + 1) ? 0 : -1;
			}
			/* (a file deleted with its drawer through another
			   path is not a failure, nor a drawer kept for the
			   Work drawer inside) */
			if (r < 0 && Lock_exists(found[i] + 1))
				fails++;
		}
	}
	if (ovf) {
		/* more than could be listed at once: run it again */
		if (list && ll + 40 < listsize) {
			scpy(list + ll, "(more: run it again)\n", listsize - ll);
			ll += slen(list + ll);
		}
		fails++;
	}
	/* an emptied parking drawer goes too */
	if (remove)
		DeleteFile((CONST_STRPTR)PARKING);

	/* then the lines that still mention Roadshow, in the scripts and
	   configuration files of the system */
	{
		static const char *const where[] = { "S:", "ENVARC:", "ENV:",
		    "DEVS:", NULL };

		slist = list;
		slistsize = listsize;
		sll = ll;
		scrubbed = scrubfails = 0;
		for (i = 0; where[i]; i++) {
			scpy(path, where[i], sizeof(path));
			/* (S: holds scripts: commands are kept, see
			   line_action()) */
			scrub_walk(path, 0, remove, i == 0);
		}
		n += scrubbed;
		fails += scrubfails;
	}

	for (i = 0; i < nfound; i++)
		if (found[i])
			FreeVec(found[i]);
	FreeVec(found);
	FreeVec(vols);
	me->pr_WindowPtr = oldwin;
	*failed = fails;
	return n;
}
