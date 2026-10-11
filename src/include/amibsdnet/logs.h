/*
 * Where AmiBSDNet's and WirelessManager's logs go: a drawer on the boot
 * volume, so that they are still there after a reboot or a power-off
 * (T: is in RAM).  T: only when that drawer cannot be written.
 */
#ifndef AMIBSDNET_LOGS_H
#define AMIBSDNET_LOGS_H

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/dos.h>

#define	AMIBSDNET_LOG_DIR	"SYS:Storage/AmiBSDNet-Logs"
#define	AMIBSDNET_LOG		AMIBSDNET_LOG_DIR "/AmiBSDNet.log"
#define	AMIBSDNET_WMLOG		AMIBSDNET_LOG_DIR "/WirelessManager.log"
#define	AMIBSDNET_LOG_T		"T:AmiBSDNet.log"
#define	AMIBSDNET_WMLOG_T	"T:WirelessManager.log"

/*
 * A log, emptied, then open with a shared lock (MODE_READWRITE) so it can
 * be read (Type) while it is written; the drawer AMIBSDNET_LOG_DIR is made
 * if it is not there.  path first, then fallback (may be NULL); *used is
 * the one opened.  0 if neither could be opened.
 */
static inline BPTR
amibsdnet_log_create(const char *path, const char *fallback,
    const char **used)
{
	const char *try[2];
	BPTR fh, l;
	int i;

	try[0] = path;
	try[1] = fallback;
	/* (the drawer only for a path in it) */
	for (i = 0; AMIBSDNET_LOG_DIR[i] && path[i] == AMIBSDNET_LOG_DIR[i];
	    i++)
		;
	if (AMIBSDNET_LOG_DIR[i] == '\0' && path[i] == '/' &&
	    (l = CreateDir((CONST_STRPTR)AMIBSDNET_LOG_DIR)) != 0)
		UnLock(l);
	for (i = 0; i < 2; i++) {
		if (try[i] == NULL)
			continue;
		if ((fh = Open((CONST_STRPTR)try[i], MODE_NEWFILE)) != 0) {
			Close(fh);
			if ((fh = Open((CONST_STRPTR)try[i], MODE_READWRITE))
			    != 0) {
				if (used)
					*used = try[i];
				return fh;
			}
		}
	}
	return 0;
}

#endif
