/*
 * Prints where SYS:, C:, S:, ENV:, ENVARC:, T:... resolve to on the
 * emulator's boot (tools/run_emu.py: DH0: is the host directory emu/hd)
 * and lists the dos list.
 *
 *   tools/build_amiga.sh src/test/bootprobe.c build/bootprobe
 *   python -I tools/run_emu.py build/bootprobe
 */
#include "emuenv.h"

static void
show(const char *n)
{
	char buf[256];
	BPTR l = Lock((CONST_STRPTR)n, ACCESS_READ);

	say(n);
	if (l == 0) {
		say(" -> no lock\n");
		return;
	}
	if (NameFromLock(l, (STRPTR)buf, sizeof(buf))) {
		say(" -> ");
		say(buf);
	}
	say("\n");
	UnLock(l);
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	static const char *const names[] = { "SYS:", "C:", "S:", "ENV:",
	    "ENVARC:", "T:", "DEVS:", "LIBS:", "SYS:S", "ENV:Sys",
	    "ENVARC:Sys", "DH0:", "SYS:Storage", "SYS:Devs/Internet", NULL };
	struct DosList *dl;
	int i;

	if (!start("bootprobe"))
		return RETURN_FAIL;
	for (i = 0; names[i]; i++)
		show(names[i]);
	dl = LockDosList(LDF_ALL | LDF_READ);
	while ((dl = NextDosEntry(dl, LDF_ALL)) != NULL) {
		say("dos entry: ");
		say((const char *)BADDR(dl->dol_Name) + 1);
		say(dl->dol_Type == DLT_DIRECTORY ? " (assign dir)\n" :
		    dl->dol_Type == DLT_DEVICE ? " (device)\n" :
		    dl->dol_Type == DLT_VOLUME ? " (volume)\n" : "\n");
	}
	UnLockDosList(LDF_ALL | LDF_READ);
	return finish("bootprobe");
}
