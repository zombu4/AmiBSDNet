/*
 * The Install script (dist/Install, as tools/package.py stages it in
 * build/dist/AmiBSDNet) run by the real Installer 43.3 at the Novice
 * level: "If the user chooses NOVICE, they will not be asked any more
 * questions" (downloads/sources/Installer-43_3/extracted/Installer43_3/
 * Installer.guide:246-249), started from the Shell with its template
 * SCRIPT,APPNAME,MINUSER,DEFUSER,LOGFILE,LANGUAGE,NOPRETEND/S,NOLOG/S,
 * NOPRINT/S (Installer.guide:217-218).  Whatever its windows wait for is
 * clicked from the host (tools/uaeinput.ps1).  Its return code is shown,
 * not checked (the guide does not say what it is).
 *
 * The assigns the script writes to are drawers of Test:tw (src/test/
 * emuenv.h).  MODE NORMAL: the installation runs to the end; then the
 * install log lists the programs, the documentation, the hosts file and
 * the startup block, the boot volume's S/User-Startup starts AmiBSDNet
 * (and is put back as it was afterwards), and the configuration exists.
 * MODE FAIL: DEVS:Internet is a file, so the hosts file cannot be copied
 * there; the log written at (complete 20) and (complete 40) must be on
 * disk without it (dist/Install P_WRITELOG).
 *
 * Copied to the volume Test: (emu/hd) before the run: build/dist/AmiBSDNet
 * as Test:pkg/AmiBSDNet and the Installer as Test:Installer.  Run on
 * Workbench 3.2 (emu/wb32.uae, whose S:User-Startup executes
 * Test:check.script):
 *
 *   tools/build_amiga.sh src/test/insttest.c build/insttest
 *   check.script: Test:insttest NORMAL >Test:stdout.txt (or FAIL)
 */
#include "emuenv.h"

#include <dos/rdargs.h>

#define	TW	"Test:tw"
#define	LOGF	"S:AmiBSDNet-Install.log"

static void
setup(int fail)
{
	static const char *const dirs[] = {
		TW "/env", TW "/envarc", TW "/t", TW "/s", TW "/sys",
		TW "/sys/C", TW "/sys/Devs", TW "/sys/Devs/Networks",
		TW "/sys/WBStartup", TW "/sys/Storage", NULL
	};
	int i;

	wipe(TW);
	for (i = 0; dirs[i]; i++)
		mkpath(dirs[i]);
	wfile(TW "/s/User-Startup", "; the user's own startup lines\n");
	wfile(TW "/s/Startup-Sequence", "Execute S:User-Startup\n");
	if (fail)
		wfile(TW "/sys/Devs/Internet", "not a drawer\n");
	check("ENV: assigned", assign("ENV", TW "/env"));
	check("ENVARC: assigned", assign("ENVARC", TW "/envarc"));
	check("T: assigned", assign("T", TW "/t"));
	check("S: assigned", assign("S", TW "/s"));
	check("SYS: assigned", assign("SYS", TW "/sys"));
	check("C: assigned", assign("C", TW "/sys/C"));
	check("DEVS: assigned", assign("DEVS", TW "/sys/Devs"));
}

static void
copy_str(char *d, const char *s)
{

	while ((*d++ = *s++))
		;
}

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

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	LONG arg[1] = { 0 };
	struct RDArgs *rda;
	BPTR dir, old;
	long rc = -1, savedlen = 0;
	int fail;
	static char bootus[256];
	char *saved;

	if (!start("insttest"))
		return RETURN_FAIL;
	if ((rda = ReadArgs((CONST_STRPTR)"MODE/A", arg, NULL)) == NULL) {
		check("MODE NORMAL or FAIL", 0);
		return finish("insttest");
	}
	fail = streq((const char *)arg[0], "FAIL");
	/* the Installer adds its startup lines to S/User-Startup of the boot
	   volume, and to S:User-Startup only if there is none
	   (Installer.guide:702-712, 1205-1207): that file is SYS: before
	   setup() moves the assign (dos.doc NameFromLock), and it is put
	   back as it was at the end */
	bootus[0] = '\0';
	if ((dir = Lock((CONST_STRPTR)"SYS:", ACCESS_READ)) != 0) {
		if (NameFromLock(dir, (STRPTR)bootus, sizeof(bootus) - 16)) {
			int n = slen(bootus);

			if (n > 0 && bootus[n - 1] != ':')
				bootus[n++] = '/';
			copy_str(bootus + n, "S/User-Startup");
		}
		UnLock(dir);
	}
	saved = bootus[0] ? rfile(bootus, &savedlen) : NULL;
	say("  the boot volume's User-Startup: ");
	say(bootus);
	say("\n");
	setup(fail);
	/* the script's own files are relative to its drawer
	   (dist/Install: (source "C/") etc.) */
	if ((dir = Lock((CONST_STRPTR)"Test:pkg/AmiBSDNet", ACCESS_READ))) {
		old = CurrentDir(dir);
		/* (by position, SCRIPT APPNAME MINUSER DEFUSER: in a run of
		   this test Installer 43.3 showed "APPNAME=AmiBSDNet" as the
		   name itself) */
		rc = SystemTagList((CONST_STRPTR)"Test:Installer Install "
		    "AmiBSDNet NOVICE NOVICE NOLOG", NULL);
		CurrentDir(old);
		UnLock(dir);
	}
	say("  Installer returned ");
	sayn(rc);
	say("\n");
	check("the install log exists", exists(LOGF));
	check("... lists C:AmiBSDNet", file_has(LOGF, "FILE C:AmiBSDNet "));
	check("... lists C:NetCtrl", file_has(LOGF, "FILE C:NetCtrl "));
	check("... lists the startup block",
	    file_has(LOGF, "STARTUP S:User-Startup ;BEGIN AmiBSDNet"));
	check("... lists the documentation (written at complete 40)",
	    file_has(LOGF, "AmiBSDNet/AmiBSDNet.txt "));
	if (fail) {
		check("... and has no hosts file (not copied)",
		    !file_has(LOGF, "DEVS:Internet/hosts"));
	} else {
		check("the log lists DEVS:Internet/hosts",
		    file_has(LOGF, "FILE DEVS:Internet/hosts "));
		check("the boot volume's User-Startup starts C:AmiBSDNet",
		    bootus[0] && file_has(bootus, ";BEGIN AmiBSDNet") &&
		    file_has(bootus, "C:AmiBSDNet"));
		check("ENVARC:AmiBSDNet/AmiBSDNet.conf exists",
		    exists("ENVARC:AmiBSDNet/AmiBSDNet.conf"));
	}
	if (saved) {
		BPTR f = Open((CONST_STRPTR)bootus, MODE_NEWFILE);
		int ok = f != 0 && Write(f, saved, savedlen) == savedlen;

		if (f && !Close(f))
			ok = 0;
		check("the boot volume's User-Startup put back", ok);
		FreeVec(saved);
	} else if (bootus[0] && exists(bootus))
		/* (there was none before: the one the Installer made goes) */
		check("the boot volume's new User-Startup deleted",
		    DeleteFile((CONST_STRPTR)bootus) != 0);
	unassign_all();
	FreeArgs(rda);
	return finish("insttest");
}
