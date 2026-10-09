/* Visual check of the Wi-Fi window (opens it for uaenet.device unit 0). */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "../tools/statustool.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;
static struct NetCtrlMsg msg;

int stack_cmd(ULONG cmd) { return -1; }
struct NetCtrlMsg *status_msg(void) { return &msg; }
void *memset(void *d, int c, unsigned long n)
{ char *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned long n)
{ char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	struct NetCtrlIface ifc;
	const char *dev = "uaenet.device";
	int i;

	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
	IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
	memset(&ifc, 0, sizeof(ifc));
	for (i = 0; dev[i]; i++)
		ifc.device[i] = dev[i];
	wifi_window(&ifc);
	return 0;
}
