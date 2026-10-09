/* Print free Fast and Chip RAM in KB (for measuring the stack's footprint). */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

static void
putn(ULONG v)
{
	char b[12];
	int i = 11;

	b[i] = '\0';
	do {
		b[--i] = '0' + v % 10;
		v /= 10;
	} while (v);
	PutStr((CONST_STRPTR)(b + i));
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	SysBase = *(struct ExecBase **)4;
	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37)) == NULL)
		return RETURN_FAIL;
	PutStr((CONST_STRPTR)"free fast KB: ");
	putn(AvailMem(MEMF_FAST) / 1024);
	PutStr((CONST_STRPTR)"  free chip KB: ");
	putn(AvailMem(MEMF_CHIP) / 1024);
	PutStr((CONST_STRPTR)"\n");
	CloseLibrary((struct Library *)DOSBase);
	return RETURN_OK;
}
