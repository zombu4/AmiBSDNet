/*
 * Other TCP/IP stacks: detection, and disabling/removing them for the
 * installer (otherstacks.c).
 */
#ifndef OTHERSTACKS_H
#define OTHERSTACKS_H

#define	OTHERS_DISABLE	0
#define	OTHERS_REMOVE	1
#define	OTHERS_RESTORE	2

/* if set, called for every file and startup line that was matched
   (len -1: what is a C string) */
extern void (*otherstacks_say)(const char *where, const char *what, int len);

/* names of the installed stacks ("Roadshow, Miami"); returns how many */
int	otherstacks_check(char *names, int size);
/* after otherstacks_check(): those still started at boot */
int	otherstacks_atboot(char *names, int size);
/* if set, otherstacks_apply() handles only the stack of this name */
extern const char *otherstacks_only;
/* returns 0, or -1 if something could not be changed */
int	otherstacks_apply(int mode);
/* S:User-Startup starts AmiBSDNet */
int	otherstacks_self_atboot(void);
/* back to the previous stack and Wi-Fi driver, AmiBSDNet out of the
   boot, logs kept; msg gets a text for the user */
int	otherstacks_fallback(char *msg, int size);

#endif
