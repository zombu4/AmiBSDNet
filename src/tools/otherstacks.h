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
/* removes AmiBSDNet: what S:AmiBSDNet-Install.log lists ("FILE <path>",
   or "FILE <path> <size> <crc32>": then only while the file is still
   so; never anything in S: or a startup script), its own files, its
   S:User-Startup block (another stack it had switched from comes back
   first); msg gets a text for the user */
int	otherstacks_uninstall(char *msg, int size);
/* S:User-Startup starts AmiBSDNet */
int	otherstacks_self_atboot(void);
/* back to the previous stack (the Wi-Fi driver is not touched), AmiBSDNet
   out of the boot, logs kept; msg gets a text for the user */
int	otherstacks_fallback(char *msg, int size);
/* the way back from otherstacks_fallback(): the lines of AmiBSDNet's own
   S:User-Startup block that it commented out are active again; 0 when
   done (or nothing to do), -1 if S:User-Startup could not be changed */
int	otherstacks_self_on(void);

/* size and CRC-32 (the zlib/PNG one, as drvcheck.c computes it for
   PaulaNET.device) of a file; 0 if it could be read (drvcheck.c) */
int	drv_file_sum(const char *path, unsigned long *size, unsigned long *crc);

#endif
