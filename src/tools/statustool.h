/*
 * AmiBSDNetStatus internals shared between status.c and wifiwin.c.
 */
#ifndef STATUSTOOL_H
#define STATUSTOOL_H

#include <dos/dos.h>
#include <amibsdnet/control.h>

/* send a command to the stack; 0 if it answered (result in status_msg()) */
int	stack_cmd(ULONG cmd);
struct NetCtrlMsg *status_msg(void);

/* the Wi-Fi window for a wireless interface */
void	wifi_window(const struct NetCtrlIface *);
/* the program is ending: a scan still out finishes on its own and frees
   its job (wifiwin.c) */
void	wifi_scan_release(void);
/* in a process started for a scan (status.c _start): runs the scan whose
   job the argument line names; 1 if it was such a process */
int	wifi_scan_child(const char *args);
/* the program file, for the scan process (status.c): its drawer and name */
int	status_program(BPTR *dir, const char **name);

/* replaces a file safely (wm.c): the data goes to <name>.amibsdnet-new,
   every write checked and read back, then that file takes the place of
   the original (kept as <name>.amibsdnet-old until the new one is in
   place), with the original's protection bits and comment; 0 when done,
   -1 when not: the original is left as it was, or, where the file had
   to be written over in place (the RAM-Disk's ENV:, wm.c) and even
   writing the original back failed, it is in <name>.amibsdnet-old,
   which the next call takes as the original */
int	amibsdnet_replace_file(const char *name, const char *data, long len);

/* the settings window: 0 = cancelled, 1 = applied, 2 = written but the
   stack is not running (start it) */
int	settings_window(void);

/* an edit hook (GTST_EditHook) for a passphrase field that shows '*';
   real, 65 bytes, gets the text (settingswin.c) */
struct Hook;
void	secret_hook_init(struct Hook *h, char *real);

/* gadtools.library CreateContext() (wifiwin.c) */
struct Gadget;
struct Gadget *create_context(struct Gadget **);
extern struct Library *GadToolsBase;

void	*memset(void *, int, unsigned long);

/* exec list init without amiga.lib */
#define	NewList(l)	do { (l)->lh_Head = (struct Node *)&(l)->lh_Tail; \
			    (l)->lh_Tail = NULL; \
			    (l)->lh_TailPred = (struct Node *)&(l)->lh_Head; \
			} while (0)

#endif
