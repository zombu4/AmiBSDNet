/*
 * AmiBSDNet: Wi-Fi interfaces.  The driver associates and WirelessManager
 * runs the WPA handshake; the stack makes sure WirelessManager is running
 * so a configured Wi-Fi network comes up at boot without any user action.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include <amibsdnet/wm.h>
#include <amibsdnet/logs.h>

#include "rumpuser_amiga.h"
#include "stack.h"

#define	P	amiga_rump_printf

/* a copy: a reconfiguration may reuse the ifaces[] entry meanwhile */
struct wmstart {
	char	name[16];
	char	device[64];
	ULONG	unit;
	ULONG	generation;
};

/* the driver and unit the WirelessManager this stack started was given
   (wm.c does not tell which driver a running one uses) */
static char started_dev[64];
static ULONG started_unit;
static int started;

static int
same_name(const char *a, const char *b)
{

	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static void
start_wm(struct wmstart *w)
{

	P("%s: starting WirelessManager\n", w->name);
	if (wm_start(w->device, w->unit) == 0) {
		sb_copy(started_dev, w->device, sizeof(started_dev));
		started_unit = w->unit;
		started = 1;
		P("%s: started WirelessManager for %s (its messages are in "
		    AMIBSDNET_WMLOG ")\n", w->name, w->device);
	} else
		P("%s: could not start WirelessManager\n", w->name);
}

static void *
wm_starter(void *arg)
{
	struct wmstart *w = arg;

	/*
	 * A fixed wait, not a condition: WirelessManager may also be
	 * started from S:User-Startup (its own installer offers to add it
	 * there: downloads/sources/prism2v2/extracted/prism2v2/Install,
	 * #ask-userstartup-help), on a line before or after AmiBSDNet's,
	 * and nothing tells whether such a line is still to come.
	 * wm_start() never starts a second one while one runs (wm.c
	 * wm_start(), "never two"), so the wait only gives a
	 * WirelessManager started a few seconds after the stack the chance
	 * to be the one that runs.
	 */
	amiga_host_sleep_ms(5000);
	if (w->generation != config_generation)
		;
	else if (!wm_running()) {
		if (!wm_installed())
			P("%s: Wi-Fi interface, but C:WirelessManager is not "
			    "installed (needed for WPA networks)\n", w->name);
		else
			start_wm(w);
	} else if (started && (!same_name(started_dev, w->device) ||
	    started_unit != w->unit)) {
		/* the one this stack started is for the old driver or unit
		   (a reconfiguration) */
		P("%s: WirelessManager runs for %s unit %lu; restarting it "
		    "for %s unit %lu\n", w->name, started_dev, started_unit,
		    w->device, w->unit);
		if (wm_stop() != 0)
			P("%s: WirelessManager did not stop\n", w->name);
		else
			start_wm(w);
	} else
		P("%s: WirelessManager is running\n", w->name);
	FreeVec(w);
	return NULL;
}

void
wireless_start(struct iface *ifc)
{
	struct wmstart *w;

	P("%s: wireless interface\n", ifc->name);
	if ((w = AllocVec(sizeof(*w), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return;
	sb_copy(w->name, ifc->name, sizeof(w->name));
	sb_copy(w->device, ifc->device, sizeof(w->device));
	w->unit = ifc->unit;
	w->generation = config_generation;
	if (rumpuser_thread_create(wm_starter, w, "AmiBSDNet Wi-Fi", 0, 0, -1,
	    NULL) != 0)
		FreeVec(w);
}
