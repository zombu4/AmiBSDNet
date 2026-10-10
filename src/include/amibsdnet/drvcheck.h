/*
 * Checking third-party drivers before use (src/common/drvcheck.c).
 */
#ifndef AMIBSDNET_DRVCHECK_H
#define AMIBSDNET_DRVCHECK_H

#include <exec/types.h>

#define	DRV_OK		0	/* a known official build */
#define	DRV_ACCEPTED	1	/* its CRC is one the user accepted */
#define	DRV_UNKNOWN	2	/* loads, but not a build we know (newer?) */
#define	DRV_DAMAGED	3	/* unreadable or does not load */
#define	DRV_MISSING	4

#define	PAULANET_NAME	"PaulaNET.device"
#define	DRV_MAXCRC	4

/* the PaulaNET lines of AmiBSDNet.conf:
     paulanet verify on|off		(default off)
     paulanet crc <8 hex digits>	(also accepted when verifying) */
struct drvprefs {
	int	verify;
	int	ncrc;
	ULONG	crc[DRV_MAXCRC];
};

/* takes a "paulanet ..." line (tokens); 1 if it was one */
int	drv_parse_line(struct drvprefs *, char **tok, int n);
/* from ENV:AmiBSDNet/AmiBSDNet.conf (else ENVARC:) */
void	drv_read_prefs(struct drvprefs *);
int	drv_parse_crc(const char *, ULONG *);
void	drv_fmt_crc(char *out, ULONG crc);	/* 9 bytes */

/* the PaulaNET.device file OpenDevice() would load: DEVS:,
   DEVS:Networks or the adapter's PaulaNET: disk.  0 if none.  No
   requesters. */
int	drv_paulanet_path(char *out, int size);

/* checks the file; *version is set for known builds, *crc always */
int	drv_check_paulanet(const char *path, const struct drvprefs *,
	    const char **version, ULONG *crc);

/* 1 if the result may be used under these preferences */
#define	DRV_USABLE(p, r) \
	((r) == DRV_OK || (r) == DRV_ACCEPTED || \
	 (!(p)->verify && (r) == DRV_UNKNOWN))

#endif
