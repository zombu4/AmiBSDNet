/*
 * Diagnosis: the Lock, Open, Write, Close, Rename and DeleteFile calls
 * that amibsdnet_replace_file() (src/common/wm.c) makes, one by one, each
 * with its IoErr() (dos.doc IoErr) when it fails, and then
 * amibsdnet_replace_file() itself, on the file given.  The single calls
 * are shown only (a failure of one is what the diagnosis is for); PASS
 * says that amibsdnet_replace_file() returned 0.
 *
 *   tools/build_amiga.sh src/test/replprobe.c build/replprobe src/common/wm.c
 *   replprobe FILE=ENV:AmiBSDNet/AmiBSDNet.conf
 */
#include "emuenv.h"

#include <dos/rdargs.h>

int	amibsdnet_replace_file(const char *name, const char *data, long len);

static void
sayn(long n)
{
	char b[12];
	int i = 11, neg = n < 0;
	unsigned long u = neg ? -n : n;

	b[i] = '\0';
	do {
		b[--i] = '0' + u % 10;
		u /= 10;
	} while (u);
	if (neg)
		b[--i] = '-';
	say(b + i);
}

static void
step(const char *what, long ok)
{
	/* (read first: say() writes, and the writes set it too) */
	long e = IoErr();

	say("  ");
	say(what);
	say(ok ? ": ok" : ": failed, IoErr ");
	if (!ok)
		sayn(e);
	say("\n");
}

static void
cat3(char *d, const char *a, const char *b)
{

	while (*a)
		*d++ = *a++;
	while (*b)
		*d++ = *b++;
	*d = '\0';
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	LONG arg[1] = { 0 };
	struct RDArgs *rda;
	const char *name;
	char tmp[256], old[256];
	BPTR fh, l;
	int r;
	static const char text[] = "# replprobe\n";

	if (!start("replprobe"))
		return RETURN_FAIL;
	if ((rda = ReadArgs((CONST_STRPTR)"FILE/A", arg, NULL)) == NULL) {
		check("FILE given", 0);
		return finish("replprobe");
	}
	name = (const char *)arg[0];
	cat3(tmp, name, ".amibsdnet-new");
	cat3(old, name, ".amibsdnet-old");
	l = Lock((CONST_STRPTR)name, ACCESS_READ);
	step("Lock(file)", l != 0);
	if (l)
		UnLock(l);
	fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
	step("Open(file.amibsdnet-new, MODE_NEWFILE)", fh != 0);
	if (fh) {
		step("Write()", Write(fh, (APTR)text, sizeof(text) - 1) ==
		    (LONG)sizeof(text) - 1);
		step("Close()", Close(fh));
	}
	step("Rename(file, file.amibsdnet-old)",
	    Rename((CONST_STRPTR)name, (CONST_STRPTR)old));
	l = Lock((CONST_STRPTR)name, ACCESS_READ);
	step("Lock(file) after that (is the name there again?)", l != 0);
	if (l)
		UnLock(l);
	step("Rename(file.amibsdnet-new, file)",
	    Rename((CONST_STRPTR)tmp, (CONST_STRPTR)name));
	step("DeleteFile(file.amibsdnet-old)",
	    DeleteFile((CONST_STRPTR)old));
	say("  amibsdnet_replace_file(): ");
	r = amibsdnet_replace_file(name, text, sizeof(text) - 1);
	sayn(r);
	if (r != 0)
		failures++;
	say("\n");
	FreeArgs(rda);
	return finish("replprobe");
}
