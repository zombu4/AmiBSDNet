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
   the file so it is preferred; psk "" for an open network, NULL to keep
   the security settings it has there (open if it is not there).  Its
   other settings there (scan_ssid...) are kept.  save 0: ENV: only */
int	wm_set_network(const char *ssid, const char *psk, int save);
/* ENV:Sys/Wireless.prefs copied to ENVARC: as it is; 0 if done or there
   is none */
int	wm_save_env(void);
/* a passphrase this project writes for WirelessManager: 8 to 63 printable
   ASCII characters, or 64 hex digits (the rule and its source are in
   src/common/wm.c, wm_passphrase_ok()) */
int	wm_passphrase_ok(const char *psk);
/* asks WirelessManager to end, without waiting (wm_stop() waits) */
void	wm_signal_stop(void);
/* the preferred network (ssid, psk; psk empty if open): 0 if any */
int	wm_get_network(char *ssid, int ssidlen, char *psk, int psklen);

#endif
