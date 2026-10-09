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
/* returns 0, or -1 if something could not be changed */
int	otherstacks_apply(int mode);

#endif
