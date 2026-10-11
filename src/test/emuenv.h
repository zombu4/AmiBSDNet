/*
 * Helpers of the emulator tests that work on files: no libc, results by
 * PutStr(), DH0:done "PASS" / "FAIL" as tools/run_emu.py expects it (its
 * docstring).  Included by each such test program; every function is
 * static.
 *
 * The tests assign their own ENV:, ENVARC:, T:, C:, S:, SYS: and DEVS: to
 * drawers of DH0:tw (emu/hd/tw on the host), so that nothing else in
 * emu/hd is touched; S: in particular must never be DH0:S, which holds the
 * emulator's own Startup-Sequence, because the code under test edits the
 * startup scripts of S:.
 *
 * AssignLock() (downloads/sources/NDK3.2/Autodocs/dos.doc:480-499): "If an
 * assign entry of that name is already on the list, this routine replaces
 * that entry", "Passing NULL for a lock cancels any outstanding assign to
 * that name", and the lock "becomes the assign" on success.  A requester
 * for a missing volume would wait for ever on a headless boot, so
 * pr_WindowPtr is -1: "if the process pr_WindowPtr is -1" no requester is
 * put up (dos.doc:1569, 3840).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

#define	UNUSED	__attribute__((unused))

static int failures;

static UNUSED void
say(const char *s)
{

	PutStr((CONST_STRPTR)s);
	Flush(Output());	/* (a hang leaves the output so far) */
}

static UNUSED int
slen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

static UNUSED int
streq(const char *a, const char *b)
{

	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* is needle in the first n bytes of hay? */
static UNUSED int
has(const char *hay, long n, const char *needle)
{
	long i;
	int j;

	for (i = 0; i < n; i++) {
		for (j = 0; needle[j] && i + j < n && hay[i + j] == needle[j]; j++)
			;
		if (!needle[j])
			return 1;
	}
	return 0;
}

static UNUSED void
check(const char *what, int ok)
{

	say(ok ? "  ok    " : "  FAIL  ");
	say(what);
	say("\n");
	if (!ok)
		failures++;
}

static UNUSED int
exists(const char *path)
{
	BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);

	if (l)
		UnLock(l);
	return l != 0;
}

/* the drawers of a path ("A:b/c" makes A:b and A:b/c; every part is a
   drawer) */
static UNUSED void
mkpath(const char *path)
{
	char d[256];
	int i;
	BPTR l;

	for (i = 0; path[i] && i < 254; i++) {
		d[i] = path[i];
		if (path[i + 1] == '/' || path[i + 1] == '\0') {
			d[i + 1] = '\0';
			if (d[i] != ':' && (l = CreateDir((CONST_STRPTR)d)) != 0)
				UnLock(l);
		}
	}
}

static UNUSED int
wfile(const char *path, const char *text)
{
	BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
	int n = slen(text), ok;

	if (fh == 0)
		return -1;
	ok = n == 0 || Write(fh, (APTR)text, n) == n;
	if (!Close(fh))
		ok = 0;
	return ok ? 0 : -1;
}

/* the whole file, NUL-terminated (FreeVec() it); NULL if it is not there */
static UNUSED char *
rfile(const char *path, long *lenp)
{
	BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
	char *buf;
	long size, n = 0, got;

	*lenp = -1;
	if (fh == 0)
		return NULL;
	Seek(fh, 0, OFFSET_END);
	size = Seek(fh, 0, OFFSET_BEGINNING);
	if (size < 0 || (buf = AllocVec(size + 1, MEMF_ANY)) == NULL) {
		Close(fh);
		return NULL;
	}
	while (n < size && (got = Read(fh, buf + n, size - n)) > 0)
		n += got;
	Close(fh);
	buf[n] = '\0';
	*lenp = n;
	return buf;
}

/* does the file hold exactly this text?  A difference is printed. */
static UNUSED int
file_is(const char *path, const char *text)
{
	long len;
	char *buf = rfile(path, &len);
	int same = buf != NULL && len == slen(text) && has(buf, len, text);

	if (!same) {
		say("    ");
		say(path);
		if (buf == NULL)
			say(" is not there\n");
		else {
			say(" holds:\n[");
			say(buf);
			say("]\n    instead of:\n[");
			say(text);
			say("]\n");
		}
	}
	if (buf)
		FreeVec(buf);
	return same;
}

/* does the file contain this text? (0 if it is not there) */
static UNUSED int
file_has(const char *path, const char *text)
{
	long len;
	char *buf = rfile(path, &len);
	int r = buf != NULL && has(buf, len, text);

	if (buf)
		FreeVec(buf);
	return r;
}

static UNUSED int
copyfile(const char *src, const char *dst)
{
	long len;
	char *buf = rfile(src, &len);
	BPTR fh;
	int ok = 0;

	if (buf == NULL)
		return -1;
	if ((fh = Open((CONST_STRPTR)dst, MODE_NEWFILE)) != 0) {
		ok = len == 0 || Write(fh, buf, len) == len;
		if (!Close(fh))
			ok = 0;
	}
	FreeVec(buf);
	return ok ? 0 : -1;
}

/* a file or a drawer with everything in it; 0 when it is gone */
static UNUSED int
wipe(const char *path)
{
	struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
	char sub[256];
	BPTR l;
	int rv = 0, dir = 0;

	if (fib == NULL)
		return -1;
	if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) == 0) {
		FreeDosObject(DOS_FIB, fib);
		return 0;			/* not there */
	}
	if (Examine(l, fib) && fib->fib_DirEntryType > 0)
		dir = 1;
	UnLock(l);
	while (dir) {
		int n, i;

		sub[0] = '\0';
		if ((l = Lock((CONST_STRPTR)path, ACCESS_READ)) == 0)
			break;
		if (Examine(l, fib) && ExNext(l, fib)) {
			n = slen(path);
			if (n + 1 + slen((const char *)fib->fib_FileName) >= 255) {
				UnLock(l);
				rv = -1;
				break;
			}
			for (i = 0; i < n; i++)
				sub[i] = path[i];
			if (path[n - 1] != ':' && path[n - 1] != '/')
				sub[n++] = '/';
			for (i = 0; fib->fib_FileName[i]; i++)
				sub[n + i] = fib->fib_FileName[i];
			sub[n + i] = '\0';
		}
		UnLock(l);
		if (!sub[0])
			break;
		if (wipe(sub) != 0) {
			rv = -1;
			break;
		}
	}
	FreeDosObject(DOS_FIB, fib);
	SetProtection((CONST_STRPTR)path, 0);
	if (!DeleteFile((CONST_STRPTR)path))
		rv = -1;
	return rv;
}

/* NAME: now means the drawer path; 1 if it worked */
static UNUSED int
assign(const char *name, const char *path)
{
	BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);

	if (l == 0)
		return 0;
	if (!AssignLock((CONST_STRPTR)name, l)) {
		UnLock(l);
		return 0;
	}
	return 1;
}

/* at the end: ENV:, ENVARC: and T: are cancelled; C:, SYS: and DEVS: are
   the root of DH0: again and S: is DH0:S, so that the Shell commands the
   Startup-Sequence runs after the program find what they did before */
static UNUSED void
unassign_all(void)
{
	static const char *const own[] = { "ENV", "ENVARC", "T", NULL };
	static const char *const vol[] = { "C", "SYS", "DEVS", NULL };
	int i;

	for (i = 0; own[i]; i++)
		AssignLock((CONST_STRPTR)own[i], 0);
	for (i = 0; vol[i]; i++)
		assign(vol[i], "DH0:");
	assign("S", "DH0:S");
}

static UNUSED int
start(const char *name)
{

	SysBase = *(struct ExecBase **)4;
	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37))
	    == NULL)
		return 0;
	((struct Process *)SysBase->ThisTask)->pr_WindowPtr = (APTR)-1;
	say(name);
	say(": start\n");
	return 1;
}

static UNUSED int
finish(const char *name)
{
	BPTR f;

	say(name);
	say(failures ? ": FAILED\n" : ": PASS\n");
	if ((f = Open((CONST_STRPTR)"DH0:done", MODE_NEWFILE)) != 0) {
		Write(f, failures ? "FAIL\n" : "PASS\n", 5);
		Close(f);
	}
	CloseLibrary((struct Library *)DOSBase);
	return failures ? RETURN_ERROR : RETURN_OK;
}
