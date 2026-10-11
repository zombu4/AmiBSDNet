/*
 * Removing Roadshow (roadshow.c).
 */
#ifndef ROADSHOW_H
#define ROADSHOW_H

/*
 * Finds (remove 0) or removes (remove 1) the files of the Roadshow 68k
 * archive where its installation puts them (the list in roadshow.c):
 * its own commands, drivers and catalogs are deleted, its settings,
 * scripts and (when no other stack is installed) bsdsocket.library and
 * usergroup.library are moved to SYS:Storage/AmiBSDNet-Roadshow, and the
 * comments that mention Roadshow go from S:Startup-Sequence and
 * S:User-Startup (a copy of each is kept first).  list gets one line per
 * item.  Returns how many were found; *failed counts what could not be
 * deleted, moved or rewritten (and a stop by Ctrl-C).
 */
int	roadshow_find(int remove, char *list, int listsize, int *failed);

#endif
