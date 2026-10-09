/* Wireless.prefs editing check: replace "Home", keep "Cafe", add "Lab". */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <amibsdnet/wm.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	BPTR fh;
	char buf[1024];
	LONG n;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	AssignLock((CONST_STRPTR)"ENV", Lock((CONST_STRPTR)"DH0:env", ACCESS_READ));
	AssignLock((CONST_STRPTR)"ENVARC", Lock((CONST_STRPTR)"DH0:envarc",
	    ACCESS_READ));
	wm_set_network("Home", "newpass99");
	wm_set_network("Lab", NULL);
	PutStr((CONST_STRPTR)(wm_has_network("Cafe") ? "has Cafe\n" : "no Cafe\n"));
	PutStr((CONST_STRPTR)(wm_running() ? "wm running\n" : "wm not running\n"));
	PutStr((CONST_STRPTR)(wm_installed() ? "wm installed\n" : "wm not installed\n"));
	if ((fh = Open((CONST_STRPTR)"ENVARC:Sys/Wireless.prefs", MODE_OLDFILE))) {
		while ((n = Read(fh, buf, sizeof(buf))) > 0)
			Write(Output(), buf, n);
		Close(fh);
	}
	return 0;
}
