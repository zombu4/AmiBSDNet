/*
 * wm_set_network() (src/common/wm.c): the text it writes to ENV: and
 * ENVARC:Sys/Wireless.prefs, compared byte for byte.  ENV: and ENVARC:
 * are assigned to DH0:tw/env and DH0:tw/envarc (emuenv.h).
 *
 *   tools/build_amiga.sh src/test/wmtest.c build/wmtest src/common/wm.c
 *   python -I tools/run_emu.py build/wmtest
 *
 * What the expected texts follow (wm.c new_prefs()): the chosen network
 * first ("network={", a tab and ssid=, the security lines, the old
 * block's other lines, "}" and an empty line), then every other block
 * with the empty line after it that goes with it; psk NULL with an old
 * block keeps all its lines; psk "" or a passphrase drops its old
 * key_keys lines and writes key_mgmt=NONE or psk= and key_mgmt=WPA-PSK.
 */
#include "emuenv.h"

#include <amibsdnet/wm.h>

#define	ENVF	"ENV:Sys/Wireless.prefs"
#define	ARCF	"ENVARC:Sys/Wireless.prefs"

#define	OTHER	"network={\n\tssid=\"Other\"\n\tpsk=\"otherpassphrase\"\n" \
		"\tkey_mgmt=WPA-PSK\n}\n\n"
#define	HOME_OLD "network={\n\tssid=\"Home\"\n\tscan_ssid=1\n\tpriority=5\n" \
		"\tpsk=\"oldpassphrase\"\n\tkey_mgmt=WPA-PSK\n}\n"
#define	INPUT	OTHER HOME_OLD

/* the prefs files as a case starts: ENVARC: always, ENV: if env != NULL */
static void
setup(const char *arc, const char *env)
{

	DeleteFile((CONST_STRPTR)ENVF);
	DeleteFile((CONST_STRPTR)ARCF);
	if (wfile(ARCF, arc) != 0)
		say("  cannot write ENVARC:Sys/Wireless.prefs\n");
	if (env && wfile(ENVF, env) != 0)
		say("  cannot write ENV:Sys/Wireless.prefs\n");
}

__attribute__((section(".text.unlikely.0_start"), used)) int
_start(void)
{
	int rv;

	if (!start("wmtest"))
		return RETURN_FAIL;
	wipe("DH0:tw");
	mkpath("DH0:tw/env");
	mkpath("DH0:tw/envarc/Sys");
	mkpath("DH0:tw/env/Sys");
	check("ENV: assigned", assign("ENV", "DH0:tw/env"));
	check("ENVARC: assigned", assign("ENVARC", "DH0:tw/envarc"));

	say("(a) Home with scan_ssid, priority, psk, key_mgmt, new passphrase\n");
	setup(INPUT, NULL);
	rv = wm_set_network("Home", "newpassphrase", 1);
	check("returns 0", rv == 0);
	{
		static const char want[] =
		    "network={\n\tssid=\"Home\"\n\tpsk=\"newpassphrase\"\n"
		    "\tkey_mgmt=WPA-PSK\n\tscan_ssid=1\n\tpriority=5\n}\n\n"
		    OTHER;

		check("ENVARC: Home first, new psk, old kept settings, old psk "
		    "gone, Other kept", file_is(ARCF, want));
		check("ENV: the same", file_is(ENVF, want));
		check("old psk line gone",
		    !file_has(ARCF, "oldpassphrase"));
	}

	say("(b) psk NULL with a block\n");
	setup(INPUT, NULL);
	rv = wm_set_network("Home", NULL, 1);
	check("returns 0", rv == 0);
	{
		static const char want[] =
		    "network={\n\tssid=\"Home\"\n\tscan_ssid=1\n\tpriority=5\n"
		    "\tpsk=\"oldpassphrase\"\n\tkey_mgmt=WPA-PSK\n}\n\n" OTHER;

		check("ENVARC: Home first with all its lines",
		    file_is(ARCF, want));
		check("ENV: the same", file_is(ENVF, want));
	}

	say("(c) psk NULL without a block\n");
	setup(OTHER, NULL);
	rv = wm_set_network("Home", NULL, 1);
	check("returns 0", rv == 0);
	{
		static const char want[] =
		    "network={\n\tssid=\"Home\"\n\tkey_mgmt=NONE\n}\n\n" OTHER;

		check("ENVARC: Home with key_mgmt=NONE, then Other",
		    file_is(ARCF, want));
		check("ENV: the same", file_is(ENVF, want));
	}

	say("(d) psk \"\" with a block\n");
	setup(INPUT, NULL);
	rv = wm_set_network("Home", "", 1);
	check("returns 0", rv == 0);
	{
		static const char want[] =
		    "network={\n\tssid=\"Home\"\n\tkey_mgmt=NONE\n"
		    "\tscan_ssid=1\n\tpriority=5\n}\n\n" OTHER;

		check("ENVARC: key_mgmt=NONE, old security lines gone, "
		    "scan_ssid and priority kept", file_is(ARCF, want));
		check("ENV: the same", file_is(ENVF, want));
	}

	say("(e) save 0: ENV: only\n");
	setup(INPUT, NULL);
	rv = wm_set_network("Home", "newpassphrase", 0);
	check("returns 0", rv == 0);
	check("ENVARC: unchanged", file_is(ARCF, INPUT));
	check("ENV: changed", file_is(ENVF,
	    "network={\n\tssid=\"Home\"\n\tpsk=\"newpassphrase\"\n"
	    "\tkey_mgmt=WPA-PSK\n\tscan_ssid=1\n\tpriority=5\n}\n\n" OTHER));

	say("(f) ENV: has a network ENVARC: has not (each file from its own "
	    "text)\n");
	setup(INPUT, INPUT "\nnetwork={\n\tssid=\"Extra\"\n"
	    "\tkey_mgmt=NONE\n}\n");
	rv = wm_set_network("Home", "newpassphrase", 1);
	check("returns 0", rv == 0);
	check("ENVARC: no Extra", file_is(ARCF,
	    "network={\n\tssid=\"Home\"\n\tpsk=\"newpassphrase\"\n"
	    "\tkey_mgmt=WPA-PSK\n\tscan_ssid=1\n\tpriority=5\n}\n\n" OTHER));
	check("ENV: Home first, Other, Extra kept", file_is(ENVF,
	    "network={\n\tssid=\"Home\"\n\tpsk=\"newpassphrase\"\n"
	    "\tkey_mgmt=WPA-PSK\n\tscan_ssid=1\n\tpriority=5\n}\n\n" OTHER
	    "network={\n\tssid=\"Extra\"\n\tkey_mgmt=NONE\n}\n"));

	say("(g) a passphrase that is not valid\n");
	setup(INPUT, NULL);
	check("short passphrase refused",
	    wm_set_network("Home", "short", 1) == -1);
	check("ENVARC: unchanged", file_is(ARCF, INPUT));
	/* (setup(INPUT, NULL) leaves no ENV: file) */
	check("ENV: still none", !exists(ENVF));

	unassign_all();
	return finish("wmtest");
}
