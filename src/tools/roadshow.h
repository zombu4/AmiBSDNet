/*
 * Removing Roadshow completely (roadshow.c).
 */
#ifndef ROADSHOW_H
#define ROADSHOW_H

/*
 * Finds (remove 0) or deletes (remove 1) everything of Roadshow on the
 * mounted volumes except Work, and the lines mentioning Roadshow in the
 * system's text and configuration files.  list gets one path per line.
 * Returns how many were found, -1 if out of memory; *failed counts what
 * could not be deleted or rewritten.
 */
int	roadshow_find(int remove, char *list, int listsize, int *failed);

#endif
