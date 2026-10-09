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

static void *
wm_starter(void *arg)
{
	struct iface *ifc = arg;
	ULONG gen = config_generation;

	/* give a WirelessManager started from S:User-Startup time to appear */
	amiga_host_sleep_ms(5000);
	if (gen != config_generation)
		return NULL;
	if (wm_running())
		P("%s: WirelessManager is running\n", ifc->name);
	else if (!wm_installed())
		P("%s: Wi-Fi interface, but C:WirelessManager is not installed "
		    "(needed for WPA networks)\n", ifc->name);
	else if (wm_start(ifc->device, ifc->unit) == 0)
		P("%s: started WirelessManager for %s\n", ifc->name, ifc->device);
	else
		P("%s: could not start WirelessManager\n", ifc->name);
	return NULL;
}

void
wireless_start(struct iface *ifc)
{

	P("%s: wireless interface\n", ifc->name);
	rumpuser_thread_create(wm_starter, ifc, "AmiBSDNet Wi-Fi", 0, 0, -1,
	    NULL);
}
