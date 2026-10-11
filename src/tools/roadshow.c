/*
 * Removing Roadshow (NetCtrl FINDROADSHOW / REMOVEROADSHOW, the
 * installer's "Remove Roadshow" choice).
 *
 * Only the files of the Roadshow 68k archive are looked at, where its
 * installation puts them.  The list is the one of a Roadshow installer
 * script that copies the archive's Workbench/ drawer to SYS: and deletes
 * the same files again: downloads/sources/morphos-roadshow-installer/
 * Install_Roadshow_MorphOS (labels c:, devs:, libs:, s:, locale:,
 * firewall:, ppp:; destdir="SYS:").  Nothing else is searched: no other
 * volume or drawer, so the drawer holding the Roadshow installer (Work,
 * say) and every user file stay.
 *
 *   - Roadshow's own commands, its PPP drivers and catalogs: deleted
 *   - its settings and scripts (DEVS:NetInterfaces, DEVS:Internet but
 *     the hosts and services files AmiBSDNet uses, ENVARC:Roadshow, S:Network-Startup,
 *     the firewall and PPP scripts): moved to SYS:Storage/AmiBSDNet-
 *     Roadshow, so nothing the user wrote is lost
 *   - LIBS:bsdsocket.library and LIBS:usergroup.library and their
 *     catalogs (names other stacks may use too): moved there as well,
 *     and only when no other TCP/IP stack is installed
 *   - the commands of that list with general names (ping, ftp, arp,
 *     traceroute, wget, tcpdump, ipf...) are not touched: AmiBSDNet's own
 *     C:Ping, or another program, may have the name
 *   - in S:Startup-Sequence and S:User-Startup, comments that mention
 *     Roadshow (a copy of the file is kept as <name>.amibsdnet-rs first;
 *     its command lines are otherstacks_apply(OTHERS_REMOVE)'s)
 *
 * Ctrl-C stops it between two files.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "roadshow.h"
#include "otherstacks.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define	PATHMAX		256
#define	BACKUP		"SYS:Storage/AmiBSDNet-Roadshow"
#define	PARKING		"SYS:Storage/AmiBSDNet-Disabled"

/* deleted: commands with names only Roadshow uses (Install_Roadshow_MorphOS
   c: delete list), its PPP drivers (ppp:), its own catalogs (locale:) */
static const char *const del_files[] = {
	"SYS:C/AddNetInterface", "SYS:C/AddNetRoute",
	"SYS:C/ConfigureNetInterface", "SYS:C/DeleteNetRoute",
	"SYS:C/GetNetStatus", "SYS:C/NetLogViewer", "SYS:C/NetShutdown",
	"SYS:C/RemoveNetInterface", "SYS:C/RoadshowControl",
	"SYS:C/SampleNetSpeed", "SYS:C/ShowNetStatus",
	"SYS:C/ppp_connector", "SYS:C/ppp_dialer", "SYS:C/ppp_sample",
	"SYS:Devs/Networks/ppp-serial.device",
	"SYS:Devs/Networks/ppp-ethernet.device",
	"SYS:Locale/Catalogs/deutsch/roadshow.catalog",
	"SYS:Locale/Catalogs/deutsch/ppp-serial.catalog",
	"SYS:Locale/Catalogs/deutsch/ppp-ethernet.catalog",
	NULL
};

/* moved to BACKUP: settings and scripts (s:, firewall:, ppp:, devs:; the
   settings RoadshowControl saves are read from envarc:roadshow there) */
static const char *const move_items[] = {
	"SYS:S/Network-Startup", "SYS:S/Check-Firewall-Rules",
	"SYS:S/Start-Firewall", "SYS:S/Stop-Firewall", "SYS:S/IPF",
	"SYS:S/PPP-Configurations", "SYS:Devs/NetInterfaces",
	"ENVARC:Roadshow", NULL
};

/* moved to BACKUP only when no other stack is installed (libs:, locale:) */
static const char *const shared_items[] = {
	"SYS:Libs/bsdsocket.library", "SYS:Libs/usergroup.library",
	"SYS:Locale/Catalogs/deutsch/bsdsocket.catalog",
	"SYS:Locale/Catalogs/deutsch/usergroup.catalog",
	NULL
};

/* Roadshow's settings drawer (devs:): all of it but what AmiBSDNet uses */
#define	INTERNET	"SYS:Devs/Internet"
static const char *const keep_internet[] = {
	"hosts", "hosts.info",		/* netdb.c HOSTS_FILE */
	"services", "services.info",	/* netdb.c SERVICES_FILE */
	NULL
};

/* what an earlier switch parked of Roadshow (otherstacks.c: its WBStartup
   items, named as otherstacks.c finds them) */
static const char *const parked_wb[] = { "NetLogViewer", "Roadshow", NULL };

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

/* a + b into d; 0 if it does not fit (a cut path would be another one) */
static int
join(char *d, const char *a, const char *b, int n)
{
	int la = slen(a);

	if (la + slen(b) >= n)
		return 0;
	if (d != a)
		scpy(d, a, n);
	scpy(d + la, b, n - la);
	return 1;
}

static int
exists(const char *path)
{
	BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);

	if (l)
		UnLock(l);
	return l != 0;
}

static int
ctrl_c(void)
{

	return (CheckSignal(SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

/* ------------------------------------------------------------------------
 * the list for the caller
 */

static char *rlist;
static int rlistsize, rll, rcount, rfails, rstopped;

static void
note(const char *path, const char *what)
{
	int l = slen(path) + slen(what);

	if (rlist && rll + l + 2 < rlistsize) {
		scpy(rlist + rll, path, rlistsize - rll);
		rll += slen(rlist + rll);
		scpy(rlist + rll, what, rlistsize - rll);
		rll += slen(rlist + rll);
		rlist[rll++] = '\n';
		rlist[rll] = '\0';
	}
}

/* ------------------------------------------------------------------------
 * deleting and moving
 */

/*
 * A drawer with everything in it; 0 when it is gone.  Hard links are
 * deleted as links (of them "the data remains until there are no links
 * left", downloads/sources/NDK3.2/Autodocs/dos.doc:3544, MakeLink); a
 * soft link is not touched at all (the drawer stays then, and that is a
 * failure that is reported).
 */
static int
delete_tree(const char *path)
{
	struct FileInfoBlock *fib;
	char (*names)[108], *sub;
	LONG *types;
	BPTR l;
	int rv = 0, n, i, gone;

	names = AllocVec(32 * 108, MEMF_ANY);
	types = AllocVec(32 * sizeof(LONG), MEMF_ANY);
	sub = AllocVec(PATHMAX, MEMF_ANY);
	fib = AllocDosObject(DOS_FIB, NULL);
	if (names == NULL || types == NULL || sub == NULL || fib == NULL) {
		rv = -1;
		goto out;
	}
	/* collected first (deleting during ExNext() would lose its place),
	   in batches: each round deletes what it collected */
	do {
		n = gone = 0;
		if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) == 0)
			break;
		if (Examine(l, fib) && fib->fib_DirEntryType > 0)
			while (n < 32 && ExNext(l, fib)) {
				scpy(names[n], (const char *)fib->fib_FileName, 108);
				types[n++] = fib->fib_DirEntryType;
			}
		UnLock(l);
		for (i = 0; i < n; i++) {
			if (!join(sub, path, "/", PATHMAX) ||
			    !join(sub, sub, names[i], PATHMAX) ||
			    types[i] == ST_SOFTLINK) {
				rv = -1;
				continue;
			}
			if (types[i] == ST_USERDIR) {
				if (delete_tree(sub) != 0)
					rv = -1;
				else
					gone++;
			} else {
				SetProtection((CONST_STRPTR)sub, 0);
				if (DeleteFile((CONST_STRPTR)sub))
					gone++;
				else
					rv = -1;
			}
		}
	} while (n == 32 && gone > 0);
	if (rv == 0) {
		SetProtection((CONST_STRPTR)path, 0);
		if (!DeleteFile((CONST_STRPTR)path))
			rv = -1;
	}
out:
	if (fib)
		FreeDosObject(DOS_FIB, fib);
	if (sub)
		FreeVec(sub);
	if (types)
		FreeVec(types);
	if (names)
		FreeVec(names);
	return rv;
}

/* the drawers of a path, made where missing ("A:b/c" -> A:b) */
static void
make_parents(const char *path)
{
	char d[PATHMAX];
	BPTR l;
	int i;

	for (i = 0; path[i] && i < PATHMAX - 1; i++) {
		d[i] = path[i];
		if (path[i] == '/') {
			d[i] = '\0';
			if (!exists(d) && (l = CreateDir((CONST_STRPTR)d)) != 0)
				UnLock(l);
			d[i] = '/';
		}
	}
}

/* where an item goes in BACKUP: "SYS:Devs/Internet/x" ->
   BACKUP "/SYS/Devs/Internet/x" */
static int
backup_path(char *d, const char *path)
{
	int i, k;

	if (!join(d, BACKUP, "/", PATHMAX))
		return 0;
	k = slen(d);
	for (i = 0; path[i]; i++) {
		if (k >= PATHMAX - 1)
			return 0;
		d[k++] = path[i] == ':' ? '/' : path[i];
	}
	d[k] = '\0';
	return 1;
}

/* renamed into BACKUP.  Rename() cannot move between volumes ("it is
   impossible to Rename() a file from one volume to" another, downloads/
   sources/NDK3.2/Autodocs/dos.doc:4673): then it stays, and that is said */
static void
move_item(const char *path, int remove)
{
	char dest[PATHMAX];

	if (!exists(path))
		return;
	rcount++;
	if (!remove) {
		note(path, "  (would be moved to " BACKUP ")");
		return;
	}
	if (!backup_path(dest, path)) {
		rfails++;
		note(path, "  (not moved: the name is too long)");
		return;
	}
	if (exists(dest)) {
		/* (an earlier backup is never overwritten) */
		rfails++;
		note(path, "  (not moved: " BACKUP " has one already)");
		return;
	}
	make_parents(dest);
	if (Rename((CONST_STRPTR)path, (CONST_STRPTR)dest))
		note(path, "  (moved to " BACKUP ")");
	else {
		rfails++;
		note(path, "  (could not be moved: in use, protected, or on "
		    "another volume)");
	}
}

static void
delete_item(const char *path, int remove)
{
	BPTR l;
	struct FileInfoBlock *fib;
	int dir = 0, r;

	if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) == 0)
		return;
	if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
		if (Examine(l, fib))
			dir = fib->fib_DirEntryType == ST_USERDIR;
		FreeDosObject(DOS_FIB, fib);
	}
	UnLock(l);
	rcount++;
	if (!remove) {
		note(path, "");
		return;
	}
	if (dir)
		r = delete_tree(path);
	else {
		SetProtection((CONST_STRPTR)path, 0);
		r = DeleteFile((CONST_STRPTR)path) ? 0 : -1;
	}
	if (r == 0)
		note(path, "  (deleted)");
	else {
		rfails++;
		note(path, "  (could not be deleted)");
	}
}

/* the entries of a drawer, a batch at a time: the names (each 108 bytes)
   after `skip' of them; returns how many */
static int
list_dir(const char *path, char (*names)[108], int max, int skip)
{
	struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
	BPTR l;
	int n = 0, seen = 0;

	if (fib == NULL)
		return 0;
	if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) != 0) {
		if (Examine(l, fib) && fib->fib_DirEntryType > 0)
			while (n < max && ExNext(l, fib))
				if (seen++ >= skip)
					scpy(names[n++],
					    (const char *)fib->fib_FileName, 108);
		UnLock(l);
	}
	FreeDosObject(DOS_FIB, fib);
	return n;
}

/* SYS:Devs/Internet: everything but what AmiBSDNet keeps */
static void
internet(int remove)
{
	char (*names)[108], path[PATHMAX];
	int n, i, k, keep, skip = 0;

	if ((names = AllocVec(32 * 108, MEMF_ANY)) == NULL) {
		rfails++;
		return;
	}
	/* (entries that stay are skipped the next round; moved ones are gone
	   from the drawer) */
	while (!rstopped && (n = list_dir(INTERNET, names, 32, skip)) > 0) {
		int stayed = 0;

		for (i = 0; i < n; i++) {
			for (keep = 0, k = 0; keep_internet[k]; k++)
				if (eq(names[i], keep_internet[k]))
					keep = 1;
			if (!keep && join(path, INTERNET "/", names[i],
			    sizeof(path))) {
				int before = rfails;

				move_item(path, remove);
				if (!remove || rfails != before)
					stayed++;
			} else
				stayed++;
			if (ctrl_c()) {
				rstopped = 1;
				break;
			}
		}
		skip += stayed;
		if (n < 32)
			break;
	}
	FreeVec(names);
}

/* the WBStartup items of Roadshow an earlier switch parked */
static void
parked(int remove)
{
	char (*names)[108], path[PATHMAX];
	int n, i, k, skip = 0;

	if ((names = AllocVec(32 * 108, MEMF_ANY)) == NULL) {
		rfails++;
		return;
	}
	while (!rstopped && (n = list_dir(PARKING "/WBStartup", names, 32,
	    skip)) > 0) {
		int stayed = 0;

		for (i = 0; i < n; i++) {
			int mine = 0;

			for (k = 0; parked_wb[k]; k++)
				if (starts(names[i], parked_wb[k]))
					mine = 1;
			if (mine && join(path, PARKING "/WBStartup/", names[i],
			    sizeof(path))) {
				int before = rfails;

				delete_item(path, remove);
				if (!remove || rfails != before)
					stayed++;
			} else
				stayed++;
		}
		skip += stayed;
		if (n < 32)
			break;
	}
	FreeVec(names);
}

/* another TCP/IP stack is installed (otherstacks.c finds them by name) */
static int
other_stack(void)
{
	char names[128];

	otherstacks_check(names, sizeof(names));
	return contains(names, "Miami") || contains(names, "AmiTCP") ||
	    contains(names, "Genesis");
}

/* ------------------------------------------------------------------------
 * comments that mention Roadshow in the two startup files
 */

#define	TEXTMAX		(512 * 1024)

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
 * 0 leave the line, 1 remove it (a comment line), 2 keep what comes
 * before its comment.  A command stays: removing an EndIf or Else would
 * break the script.  A comment starts at a ';' outside double quotes,
 * as otherstacks.c reads script lines.
 */
static int
line_action(const char *p, const char *e, const char **cut)
{
	const char *q = p, *c;
	int quote = 0;

	if (!line_mentions(p, e))
		return 0;
	while (q < e && (*q == ' ' || *q == '\t'))
		q++;
	if (q < e && *q == ';')
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

static char *
read_all(const char *path, LONG *lenp)
{
	BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
	char *buf = NULL;
	LONG size, n = 0, got;

	*lenp = -1;
	if (fh == 0)
		return NULL;
	Seek(fh, 0, OFFSET_END);
	size = Seek(fh, 0, OFFSET_BEGINNING);
	if (size >= 0 && size <= TEXTMAX &&
	    (buf = AllocVec(size + 1, MEMF_ANY)) != NULL) {
		while (n < size && (got = Read(fh, buf + n, size - n)) > 0)
			n += got;
		if (n != size) {
			FreeVec(buf);
			buf = NULL;
		} else
			*lenp = n;
	}
	Close(fh);
	return buf;
}

/* len bytes to a new file; 0 when all went out and it closed */
static int
write_all(const char *path, const char *buf, LONG len)
{
	BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
	int ok;

	if (fh == 0)
		return -1;
	ok = len == 0 || Write(fh, (APTR)buf, len) == len;
	if (!Close(fh))
		ok = 0;
	return ok ? 0 : -1;
}

/* the first copy of the original (an older one is kept: it is the first
   original); 0 if there is one */
static int
keep_copy(const char *path, const char *bak)
{
	char *buf;
	LONG n;
	int r;

	if (exists(bak))
		return 0;
	if ((buf = read_all(path, &n)) == NULL)
		return -1;
	r = write_all(bak, buf, n);
	FreeVec(buf);
	if (r != 0)
		DeleteFile((CONST_STRPTR)bak);
	return r;
}

static void
scrub_file(const char *path, int remove)
{
	char *buf, *out, *p, *e, bak[PATHMAX], tmp[PATHMAX], old[PATHMAX];
	const char *cut;
	struct FileInfoBlock *fib;
	LONG n, ol = 0, prot = -1;
	BPTR l;
	int changes = 0, left = 0, a;

	if ((buf = read_all(path, &n)) == NULL)
		return;
	for (p = buf; p < buf + n; p = e + 1) {
		for (e = p; e < buf + n && *e != '\n'; e++)
			;
		if ((a = line_action(p, e, &cut)) != 0)
			changes++;
		else if (line_mentions(p, e))
			left++;
	}
	if (left)
		note(path, "  (commands that mention Roadshow: left as they "
		    "are)");
	if (changes == 0) {
		FreeVec(buf);
		return;
	}
	rcount++;
	if (!remove) {
		note(path, "  (comments that mention Roadshow)");
		FreeVec(buf);
		return;
	}
	if ((out = AllocVec(n + 1, MEMF_ANY)) == NULL) {
		rfails++;
		note(path, "  (not changed: no memory)");
		FreeVec(buf);
		return;
	}
	for (p = buf; p < buf + n; p = e + 1) {
		LONG k;

		for (e = p; e < buf + n && *e != '\n'; e++)
			;
		a = line_action(p, e, &cut);
		if (a == 1)
			continue;
		k = a == 2 ? cut - p : e - p;
		CopyMem(p, out + ol, k);
		ol += k;
		if (e < buf + n)
			out[ol++] = '\n';
	}
	FreeVec(buf);
	if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
		if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) != 0) {
			if (Examine(l, fib))
				prot = fib->fib_Protection;
			UnLock(l);
		}
		FreeDosObject(DOS_FIB, fib);
	}
	/* a copy of the original first, then the new text next to the file;
	   the file is never missing: renamed aside, replaced, put back if
	   that fails */
	if (!join(bak, path, ".amibsdnet-rs", sizeof(bak)) ||
	    !join(tmp, path, ".amibsdnet-new", sizeof(tmp)) ||
	    !join(old, path, ".amibsdnet-old", sizeof(old)) ||
	    keep_copy(path, bak) != 0 || write_all(tmp, out, ol) != 0) {
		DeleteFile((CONST_STRPTR)tmp);
		rfails++;
		note(path, "  (not changed: its copy could not be written)");
		FreeVec(out);
		return;
	}
	FreeVec(out);
	DeleteFile((CONST_STRPTR)old);
	if (!Rename((CONST_STRPTR)path, (CONST_STRPTR)old)) {
		DeleteFile((CONST_STRPTR)tmp);
		rfails++;
		note(path, "  (not changed: it is in use)");
	} else if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path)) {
		Rename((CONST_STRPTR)old, (CONST_STRPTR)path);
		DeleteFile((CONST_STRPTR)tmp);
		rfails++;
		note(path, "  (not changed)");
	} else {
		/* (the s bit of a script) */
		if (prot != -1)
			SetProtection((CONST_STRPTR)path, prot);
		DeleteFile((CONST_STRPTR)old);
		note(path, "  (comments removed; the original is "
		    "<name>.amibsdnet-rs)");
	}
}

/* ------------------------------------------------------------------------ */

int
roadshow_find(int remove, char *list, int listsize, int *failed)
{
	struct Process *me = (struct Process *)SysBase->ThisTask;
	APTR oldwin = me->pr_WindowPtr;
	int i, others;

	*failed = 0;
	if (list && listsize)
		list[0] = '\0';
	rlist = list;
	rlistsize = listsize;
	rll = rcount = rfails = rstopped = 0;
	me->pr_WindowPtr = (APTR)-1;	/* no "insert volume" requesters */

	for (i = 0; del_files[i] && !rstopped; i++) {
		delete_item(del_files[i], remove);
		rstopped = ctrl_c();
	}
	for (i = 0; move_items[i] && !rstopped; i++) {
		move_item(move_items[i], remove);
		rstopped = ctrl_c();
	}
	if (!rstopped)
		internet(remove);
	if (!rstopped) {
		others = other_stack();
		for (i = 0; shared_items[i] && !rstopped; i++) {
			if (others) {
				if (exists(shared_items[i]))
					note(shared_items[i], "  (left: another "
					    "TCP/IP stack is installed)");
			} else
				move_item(shared_items[i], remove);
			rstopped = ctrl_c();
		}
		if (!rstopped && !others)
			move_item(PARKING "/Libs/bsdsocket.library", remove);
	}
	if (!rstopped)
		parked(remove);
	if (!rstopped) {
		scrub_file("S:Startup-Sequence", remove);
		scrub_file("S:User-Startup", remove);
	}
	if (rstopped) {
		rfails++;
		note("(stopped: Ctrl-C)", "");
	}
	me->pr_WindowPtr = oldwin;
	*failed = rfails;
	return rcount;
}
