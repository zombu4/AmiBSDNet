/*
 * WirelessManager helpers (src/common/wm.c).
 */
#ifndef AMIBSDNET_WM_H
#define AMIBSDNET_WM_H

int	wm_installed(void);		/* C:WirelessManager exists */
int	wm_running(void);
int	wm_stop(void);			/* 0 when it is gone */
int	wm_start(const char *device, unsigned long unit);
/* add or replace a network in ENV: and ENVARC:Sys/Wireless.prefs, first in
   the file so it is preferred; psk NULL or "" for an open network */
int	wm_set_network(const char *ssid, const char *psk);
int	wm_has_network(const char *ssid);

#endif
