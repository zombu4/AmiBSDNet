/*
 * AmiBSDNet: Wi-Fi interfaces.  The driver associates and WirelessManager
 * runs the WPA handshake; the stack makes sure WirelessManager is running
 * so a configured Wi-Fi network comes up at boot without any user action.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include <amibsdnet/wm.h>

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

static void *
wm_starter(void *arg)
{
	struct wmstart *w = arg;

	/* give a WirelessManager started from S:User-Startup time to appear */
	amiga_host_sleep_ms(5000);
	if (w->generation != config_generation)
		;
	else if (wm_running())
		P("%s: WirelessManager is running\n", w->name);
	else if (!wm_installed())
		P("%s: Wi-Fi interface, but C:WirelessManager is not installed "
		    "(needed for WPA networks)\n", w->name);
	else if (P("%s: starting WirelessManager\n", w->name),
	    wm_start(w->device, w->unit) == 0)
		P("%s: started WirelessManager for %s (its messages are in "
		    "T:WirelessManager.log)\n", w->name, w->device);
	else
		P("%s: could not start WirelessManager\n", w->name);
	FreeVec(w);
	return NULL;
}

void
wireless_start(struct iface *ifc)
{
	struct wmstart *w;

	P("%s: wireless interface\n", ifc->name);
	if ((w = AllocVec(sizeof(*w), MEMF_ANY | MEMF_CLEAR)) == NULL)
		return;
	sb_copy(w->name, ifc->name, sizeof(w->name));
	sb_copy(w->device, ifc->device, sizeof(w->device));
	w->unit = ifc->unit;
	w->generation = config_generation;
	if (rumpuser_thread_create(wm_starter, w, "AmiBSDNet Wi-Fi", 0, 0, -1,
	    NULL) != 0)
		FreeVec(w);
}
