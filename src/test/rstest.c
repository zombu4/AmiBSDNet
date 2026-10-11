/*
 * NetCtrl REMOVEROADSHOW (src/tools/netctrl.c roadshow(); the work is
 * roadshow_find() in src/tools/roadshow.c) on files prepared in DH0:tw/sys.
 * SYS:, C:, S:, DEVS: and ENV:, ENVARC:, T: are assigned to drawers of
 * DH0:tw (emuenv.h); nothing outside emu/hd/tw is touched, and
 * DH0:S/Startup-Sequence never is.
 *
 * Run 1: S:User-Startup has no "C:AmiBSDNet" line, so roadshow() must
 * refuse (otherstacks_self_atboot() is 0): return code RETURN_WARN and
 * no file moved.
 * Run 2: with the line, SYS:Devs/Internet keeps hosts, hosts.info,
 * services and services.info (roadshow.c keep_internet[]), every other
 * entry there (also more than the 32 that list_dir() reads at a time)
 * is moved to SYS:Storage/AmiBSDNet-Roadshow/SYS/Devs/Internet/<name>
 * (move_item(), backup_path()), SYS:C/RoadshowControl (del_files[]) is
 * deleted and SYS:Devs/NetInterfaces (move_items[]) is moved.
 *
 *   tools/build_amiga.sh src/tools/netctrl.c build/NetCtrl \
 *       src/common/probe.c src/tools/otherstacks.c src/tools/roadshow.c \
 *       src/common/drvcheck.c
 *   tools/build_amiga.sh src/test/rstest.c build/rstest
 *   python -I tools/run_emu.py build/rstest --file build/NetCtrl
 *
 * NetCtrl runs as DH0:NetCtrl (where --file puts it) through
 * SystemTagList() (redirection is parsed by the Shell: dos.doc
 * SystemTagList), its output in DH0:tw/out.txt.
 */
#include "emuenv.h"

#define	OUT	"DH0:tw/out.txt"
#define	INET	"SYS:Devs/Internet/"
#define	BK	"SYS:Storage/AmiBSDNet-Roadshow/SYS/Devs/Internet/"
#define	NFILL	40

static void
fillname(char *d, const char *dir, int i)
{
	int n = slen(dir), k;

	for (k = 0; k < n; k++)
		d[k] = dir[k];
	d[n++] = 'f';
	d[n++] = '0' + i / 10;
	d[n++] = '0' + i % 10;
	d[n] = '\0';
}

static void
prepare(const char *usrstartup)
{
	char p[128];
	int i;

	wipe("SYS:Devs/Internet");
	wipe("SYS:Storage/AmiBSDNet-Roadshow");
	wipe("SYS:Devs/NetInterfaces");
	wipe(OUT);
	mkpath("SYS:Devs/Internet");
	wfile("S:Startup-Sequence", "; test startup\n");
	wfile("S:User-Startup", usrstartup);
	wfile(INET "hosts", "127.0.0.1 localhost\n");
	wfile(INET "hosts.info", "icon\n");
	wfile(INET "services", "echo 7/tcp\n");
	wfile(INET "services.info", "icon\n");
	wfile(INET "routing.conf", "route\n");
	for (i = 0; i < NFILL; i++) {
		fillname(p, INET, i);
		wfile(p, "filler\n");
	}
	wfile("SYS:Devs/NetInterfaces", "iface\n");
	wfile("SYS:C/RoadshowControl", "cmd\n");
}

static long
run_netctrl(void)
{
	long rc = SystemTagList((CONST_STRPTR)"DH0:NetCtrl REMOVEROADSHOW >" OUT,
	    NULL);
	long len;
	char *buf = rfile(OUT, &len);

	say("--- NetCtrl output:\n");
	if (buf) {
		say(buf);
		FreeVec(buf);
	}
	say("---\n");
	return rc;
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	char p[128];
	long rc;
	int i, allgone, allthere;

	if (!start("rstest"))
		return RETURN_FAIL;
	if (!exists("DH0:NetCtrl")) {
		say("DH0:NetCtrl is not there (run_emu.py --file build/NetCtrl)\n");
		failures++;
		return finish("rstest");
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
		return finish("rstest");
	}

	say("--- run 1: AmiBSDNet is not started at boot\n");
	prepare("; user\nEcho hello\n");
	rc = run_netctrl();
	check("NetCtrl returned 5 (RETURN_WARN)", rc == 5);
	check("message: not started at boot",
	    file_has(OUT, "AmiBSDNet is not started at boot"));
	check("routing.conf not moved", exists(INET "routing.conf"));
	check("SYS:C/RoadshowControl not deleted",
	    exists("SYS:C/RoadshowControl"));
	check("nothing in the backup drawer",
	    !exists("SYS:Storage/AmiBSDNet-Roadshow"));

	say("--- run 2: C:AmiBSDNet is in S:User-Startup\n");
	prepare("; user\nC:AmiBSDNet\nEcho hello\n");
	rc = run_netctrl();
	check("NetCtrl returned 0", rc == 0);
	check("message lists routing.conf as moved",
	    file_has(OUT, "SYS:Devs/Internet/routing.conf  (moved to "
	    "SYS:Storage/AmiBSDNet-Roadshow)"));
	check("message does not list hosts",
	    !file_has(OUT, "SYS:Devs/Internet/hosts"));
	check("message does not list services",
	    !file_has(OUT, "SYS:Devs/Internet/services"));
	check("hosts kept", file_is(INET "hosts", "127.0.0.1 localhost\n"));
	check("hosts.info kept", file_is(INET "hosts.info", "icon\n"));
	check("services kept", file_is(INET "services", "echo 7/tcp\n"));
	check("services.info kept", file_is(INET "services.info", "icon\n"));
	check("routing.conf left Devs/Internet", !exists(INET "routing.conf"));
	check("routing.conf is in the backup drawer",
	    file_is(BK "routing.conf", "route\n"));
	allgone = allthere = 1;
	for (i = 0; i < NFILL; i++) {
		fillname(p, INET, i);
		if (exists(p))
			allgone = 0;
		fillname(p, BK, i);
		if (!file_is(p, "filler\n"))
			allthere = 0;
	}
	check("all 40 other files left Devs/Internet", allgone);
	check("all 40 other files are in the backup drawer", allthere);
	check("SYS:Devs/NetInterfaces moved",
	    !exists("SYS:Devs/NetInterfaces") && file_is(
	    "SYS:Storage/AmiBSDNet-Roadshow/SYS/Devs/NetInterfaces", "iface\n"));
	check("SYS:C/RoadshowControl deleted", !exists("SYS:C/RoadshowControl"));
	check("S:User-Startup unchanged", file_is("S:User-Startup",
	    "; user\nC:AmiBSDNet\nEcho hello\n"));
	check("S:Startup-Sequence unchanged",
	    file_is("S:Startup-Sequence", "; test startup\n"));

	unassign_all();
	return finish("rstest");
}
