/*
 * NetCtrl UNINSTALL (src/tools/netctrl.c CMD_UNINSTALL calls
 * otherstacks_uninstall(), src/tools/otherstacks.c) on files prepared in
 * DH0:tw/sys: SYS:, C:, S:, DEVS: and ENV:, ENVARC:, T: are assigned to
 * drawers of DH0:tw (emuenv.h), so nothing outside emu/hd/tw is touched
 * and DH0:S/Startup-Sequence never is.
 *
 * Run 1 (everything works): AmiBSDNet's ";BEGIN AmiBSDNet" block leaves
 * S:User-Startup (own_startup_remove()); the install log's FILE lines and
 * own_files[] / own_dirs[] go; S:Alpha.amibsdnet-bak, identical to
 * S:Alpha, is deleted, S:Beta.amibsdnet-bak, which differs from S:Beta,
 * stays and the message says so ("S:*.amibsdnet-bak ... were kept");
 * then C:NetCtrl (SELF) and the log S:AmiBSDNet-Install.log go.
 *
 * Run 2 (a FILE line of the log that is too long, which makes
 * otherstacks_uninstall() fail: "too long: not cut short"): the identical
 * S:Alpha.amibsdnet-bak stays too ("only when all went well"), C:NetCtrl
 * and the log stay (SELF is deleted only when rv == 0), the return code is
 * RETURN_WARN and the message asks for another "NetCtrl UNINSTALL".
 *
 *   tools/build_amiga.sh src/tools/netctrl.c build/NetCtrl \
 *       src/common/probe.c src/tools/otherstacks.c src/tools/roadshow.c \
 *       src/common/drvcheck.c
 *   tools/build_amiga.sh src/test/uninsttest.c build/uninsttest
 *   python -I tools/run_emu.py build/uninsttest --file build/NetCtrl
 *
 * NetCtrl is run as DH0:NetCtrl (where --file puts it); C:NetCtrl is a
 * copy of it that the first run deletes.  The command runs through
 * SystemTagList() ("Normal Shell command-line parsing will be done
 * including redirection on 'command'", dos.doc SystemTagList), its output
 * in DH0:tw/out.txt.
 */
#include "emuenv.h"

#define	LOG	"S:AmiBSDNet-Install.log"
#define	OUT	"DH0:tw/out.txt"

static void
prepare(int toolong)
{
	char log[512];
	int i, n;

	/* leftovers of an earlier run */
	wipe("S:Alpha.amibsdnet-bak");
	wipe("S:Beta.amibsdnet-bak");
	wipe(LOG);
	wipe("C:NetCtrl");
	wipe("C:AmiBSDNet");
	wipe(OUT);
	mkpath("SYS:AmiBSDNet");
	mkpath("SYS:Storage/AmiBSDNet-Logs");
	mkpath("ENV:AmiBSDNet");
	mkpath("ENVARC:AmiBSDNet");

	wfile("S:Startup-Sequence", "; test startup\n");
	wfile("S:User-Startup", "; user\nEcho hello\n;BEGIN AmiBSDNet\n"
	    "C:AmiBSDNet CONFIG=ENVARC:AmiBSDNet/x.conf\n;END AmiBSDNet\n"
	    "Echo after\n");
	wfile("S:Alpha", "alpha\n");
	wfile("S:Alpha.amibsdnet-bak", "alpha\n");
	wfile("S:Beta", "beta new\n");
	wfile("S:Beta.amibsdnet-bak", "beta old\n");

	copyfile("DH0:NetCtrl", "C:NetCtrl");
	wfile("C:AmiBSDNet", "stack\n");
	wfile("SYS:AmiBSDNet/Doc.txt", "doc\n");
	/* (icons as the installer writes them: the drawer's, and one for
	   a file in it, dist/Install (infos)) */
	wfile("SYS:AmiBSDNet.info", "icon\n");
	wfile("SYS:AmiBSDNet/Doc.txt.info", "icon\n");
	wfile("SYS:Storage/AmiBSDNet-Logs/x.log", "log\n");
	wfile("ENV:AmiBSDNet/v", "v\n");
	wfile("ENVARC:AmiBSDNet/conf", "conf\n");

	/* the install log: a drawer is listed before what is in it */
	n = 0;
	{
		static const char head[] = "AmiBSDNet install log\n"
		    "FILE SYS:AmiBSDNet\nFILE SYS:AmiBSDNet.info\n"
		    "FILE SYS:AmiBSDNet/Doc.txt\nFILE SYS:AmiBSDNet/Doc.txt.info\n"
		    "FILE C:AmiBSDNet\nFILE C:NetCtrl\n";

		for (i = 0; head[i]; i++)
			log[n++] = head[i];
	}
	if (toolong) {
		static const char t[] = "FILE SYS:";

		for (i = 0; t[i]; i++)
			log[n++] = t[i];
		for (i = 0; i < 290; i++)
			log[n++] = 'x';
		log[n++] = '\n';
	}
	log[n] = '\0';
	wfile(LOG, log);
}

static long
run_netctrl(void)
{

	return SystemTagList((CONST_STRPTR)"DH0:NetCtrl UNINSTALL >" OUT, NULL);
}

static void
show_output(void)
{
	long len;
	char *buf = rfile(OUT, &len);

	say("--- NetCtrl output:\n");
	if (buf) {
		say(buf);
		FreeVec(buf);
	}
	say("---\n");
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	long rc;

	if (!start("uninsttest"))
		return RETURN_FAIL;
	if (!exists("DH0:NetCtrl")) {
		say("DH0:NetCtrl is not there (run_emu.py --file build/NetCtrl)\n");
		failures++;
		return finish("uninsttest");
	}
	wipe("DH0:tw");
	mkpath("DH0:tw/sys/C");
	mkpath("DH0:tw/sys/S");
	mkpath("DH0:tw/sys/Devs");
	mkpath("DH0:tw/sys/Storage");
	mkpath("DH0:tw/env");
	mkpath("DH0:tw/envarc");
	mkpath("DH0:tw/t");
	check("SYS: assigned", assign("SYS", "DH0:tw/sys"));
	check("C: assigned", assign("C", "DH0:tw/sys/C"));
	check("S: assigned", assign("S", "DH0:tw/sys/S"));
	check("DEVS: assigned", assign("DEVS", "DH0:tw/sys/Devs"));
	check("ENV: assigned", assign("ENV", "DH0:tw/env"));
	check("ENVARC: assigned", assign("ENVARC", "DH0:tw/envarc"));
	check("T: assigned", assign("T", "DH0:tw/t"));
	if (failures) {
		unassign_all();
		return finish("uninsttest");
	}

	say("--- run 1: everything works\n");
	prepare(0);
	check("C:NetCtrl copied", exists("C:NetCtrl"));
	rc = run_netctrl();
	show_output();
	check("NetCtrl returned 0 (RETURN_OK)", rc == 0);
	check("message: AmiBSDNet is removed",
	    file_has(OUT, "AmiBSDNet is removed. Reboot"));
	/* (src/tools/otherstacks.c: the text for a drawer that is not
	   empty; nothing else was put into SYS:AmiBSDNet) */
	check("message: no drawer kept", !file_has(OUT, "has other files"));
	check("S:Alpha.amibsdnet-bak (same as S:Alpha) deleted",
	    !exists("S:Alpha.amibsdnet-bak"));
	check("S:Alpha kept", file_is("S:Alpha", "alpha\n"));
	check("S:Beta.amibsdnet-bak (differs from S:Beta) kept",
	    file_is("S:Beta.amibsdnet-bak", "beta old\n"));
	check("S:Beta kept", file_is("S:Beta", "beta new\n"));
	check("message mentions S:*.amibsdnet-bak being kept",
	    file_has(OUT, "S:*.amibsdnet-bak (the startup files as they were "
	    "before) were kept"));
	check("S:User-Startup: block gone, the rest kept",
	    file_is("S:User-Startup", "; user\nEcho hello\nEcho after\n"));
	check("S:Startup-Sequence unchanged",
	    file_is("S:Startup-Sequence", "; test startup\n"));
	check("C:AmiBSDNet deleted", !exists("C:AmiBSDNet"));
	check("SYS:AmiBSDNet (Doc.txt, its icon and the drawer) deleted",
	    !exists("SYS:AmiBSDNet"));
	check("... and the drawer's icon", !exists("SYS:AmiBSDNet.info"));
	check("SYS:Storage/AmiBSDNet-Logs deleted",
	    !exists("SYS:Storage/AmiBSDNet-Logs"));
	check("ENV:AmiBSDNet deleted", !exists("ENV:AmiBSDNet"));
	check("ENVARC:AmiBSDNet deleted", !exists("ENVARC:AmiBSDNet"));
	check("C:NetCtrl deleted (all went well)", !exists("C:NetCtrl"));
	check("install log deleted (all went well)", !exists(LOG));

	say("--- run 2: a log line too long, so not everything succeeds\n");
	prepare(1);
	rc = run_netctrl();
	show_output();
	check("NetCtrl returned 5 (RETURN_WARN)", rc == 5);
	check("message: some files could not be deleted",
	    file_has(OUT, "some files could not be deleted"));
	check("message asks for another UNINSTALL",
	    file_has(OUT, "run \"NetCtrl UNINSTALL\" again"));
	check("S:Alpha.amibsdnet-bak kept (not all went well)",
	    exists("S:Alpha.amibsdnet-bak"));
	check("S:Beta.amibsdnet-bak kept", exists("S:Beta.amibsdnet-bak"));
	check("C:AmiBSDNet still deleted", !exists("C:AmiBSDNet"));
	check("C:NetCtrl kept", exists("C:NetCtrl"));
	check("install log kept", exists(LOG));
	check("S:User-Startup: block gone",
	    file_is("S:User-Startup", "; user\nEcho hello\nEcho after\n"));

	say("--- run 3: no install log: the drawer is SYS:AmiBSDNet, and only "
	    "the installer's files in it go (drawer_files[])\n");
	prepare(0);
	wipe(LOG);
	wfile("SYS:AmiBSDNet/NetBSD.txt", "netbsd\n");
	wfile("SYS:AmiBSDNet/NetBSD.txt.info", "icon\n");
	wfile("SYS:AmiBSDNet/AmiBSDNet.txt", "doc\n");
	wipe("SYS:AmiBSDNet/Doc.txt");
	wipe("SYS:AmiBSDNet/Doc.txt.info");
	rc = run_netctrl();
	show_output();
	check("SYS:AmiBSDNet/NetBSD.txt deleted",
	    !exists("SYS:AmiBSDNet/NetBSD.txt"));
	check("... and its icon", !exists("SYS:AmiBSDNet/NetBSD.txt.info"));
	check("SYS:AmiBSDNet (empty then) deleted", !exists("SYS:AmiBSDNet"));

	unassign_all();
	return finish("uninsttest");
}
