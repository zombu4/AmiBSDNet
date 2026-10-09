/*
 * Network adapter detection: finds the SANA-II drivers in DEVS:Networks
 * (and drivers built into ROM, such as WinUAE's uaenet.device), opens
 * each one and tells Ethernet from Wi-Fi.
 */
#ifndef AMIBSDNET_PROBE_H
#define AMIBSDNET_PROBE_H

#include <exec/types.h>

#define	PROBE_MAX	8

struct probe_adapter {
	char	device[64];		/* "wifipi.device" */
	ULONG	unit;
	int	wireless;		/* Wi-Fi (scans for networks) */
};

/* returns the number of adapters found, Ethernet ones first */
int	probe_adapters(struct probe_adapter *, int max);

#endif /* AMIBSDNET_PROBE_H */
