/*
 * AmiBSDNetStatus internals shared between status.c and wifiwin.c.
 */
#ifndef STATUSTOOL_H
#define STATUSTOOL_H

#include <amibsdnet/control.h>

/* send a command to the stack; 0 if it answered (result in status_msg()) */
int	stack_cmd(ULONG cmd);
struct NetCtrlMsg *status_msg(void);

/* the Wi-Fi window for a wireless interface */
void	wifi_window(const struct NetCtrlIface *);

void	*memset(void *, int, unsigned long);

/* exec list init without amiga.lib */
#define	NewList(l)	do { (l)->lh_Head = (struct Node *)&(l)->lh_Tail; \
			    (l)->lh_Tail = NULL; \
			    (l)->lh_TailPred = (struct Node *)&(l)->lh_Head; \
			} while (0)

#endif
