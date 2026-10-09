/*
 * Icon format check: load the generated .info files with icon.library and
 * print what it sees.  Usage: icontest <name> ... (without ".info")
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *IconBase;

static void
putn(LONG v)
{
	char b[12];
	int i = 11, neg = v < 0;
	ULONG u = neg ? -v : v;

	b[i] = '\0';
	do {
		b[--i] = '0' + u % 10;
		u /= 10;
	} while (u);
	if (neg)
		b[--i] = '-';
	PutStr((CONST_STRPTR)(b + i));
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct RDArgs *rda;
	LONG arg[1] = { 0 };
	STRPTR *names;
	int bad = 0;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	IconBase = OpenLibrary("icon.library", 37);
	if (!DOSBase || !IconBase)
		return RETURN_FAIL;
	if ((rda = ReadArgs((CONST_STRPTR)"NAMES/M", arg, NULL)) == NULL)
		return RETURN_FAIL;
	for (names = (STRPTR *)arg[0]; names && *names; names++) {
		struct DiskObject *d = GetDiskObject(*names);
		char **tt;

		PutStr((CONST_STRPTR)*names);
		if (d == NULL) {
			PutStr((CONST_STRPTR)": FAIL (not loadable)\n");
			bad++;
			continue;
		}
		PutStr((CONST_STRPTR)": type ");
		putn(d->do_Type);
		PutStr((CONST_STRPTR)" size ");
		putn(d->do_Gadget.Width);
		PutStr((CONST_STRPTR)"x");
		putn(d->do_Gadget.Height);
		if (d->do_Gadget.GadgetRender) {
			struct Image *im = d->do_Gadget.GadgetRender;

			PutStr((CONST_STRPTR)" image ");
			putn(im->Width);
			PutStr((CONST_STRPTR)"x");
			putn(im->Height);
			PutStr((CONST_STRPTR)"x");
			putn(im->Depth);
		}
		if (d->do_DefaultTool) {
			PutStr((CONST_STRPTR)" tool=");
			PutStr((CONST_STRPTR)d->do_DefaultTool);
		}
		for (tt = (char **)d->do_ToolTypes; tt && *tt; tt++) {
			PutStr((CONST_STRPTR)" [");
			PutStr((CONST_STRPTR)*tt);
			PutStr((CONST_STRPTR)"]");
		}
		PutStr((CONST_STRPTR)"\n");
		FreeDiskObject(d);
	}
	FreeArgs(rda);
	PutStr((CONST_STRPTR)(bad ? "icontest: FAIL\n" : "icontest: PASS\n"));
	CloseLibrary(IconBase);
	CloseLibrary((struct Library *)DOSBase);
	return bad ? RETURN_ERROR : RETURN_OK;
}
