/* Emulator pipeline check: write a file to the host-mounted DH0:. */
#include <proto/exec.h>
#include <proto/dos.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	static const char msg[] = "hello from m68k\n";
	BPTR f;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
	if (DOSBase == NULL)
		return 20;
	f = Open("DH0:result.txt", MODE_NEWFILE);
	if (f) {
		Write(f, msg, sizeof(msg) - 1);
		Close(f);
	}
	CloseLibrary((struct Library *)DOSBase);
	return 0;
}
