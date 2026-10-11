/*
 * Host entropy pool for rumpuser_getrandom() (entropy.c).
 */
#ifndef AMIBSDNET_ENTROPY_H
#define AMIBSDNET_ENTROPY_H

#include <stddef.h>

/* event sources; each has its own timing-delta state */
#define	ENT_SRC_RX	0	/* a SANA-II CMD_READ came back with a frame */
#define	ENT_SRC_EVENT	1	/* a SANA-II S2_ONEVENT came back */
#define	ENT_SRC_TX	2	/* a SANA-II write came back */
#define	ENT_SRC_HOST	3	/* anything else (never credited) */
#define	ENT_NSRC	4

/* once, by amiga_rump_hostinit() (needs TimerBase) */
void	amiga_entropy_init(void);

/*
 * An event happened now: the E-Clock is sampled and mixed into the pool
 * together with data (may be NULL).  With 'credit', the sample may count
 * as one bit of entropy (see entropy.c).  Task context only.
 */
void	amiga_entropy_event(unsigned src, int credit, const void *data,
	    size_t len);

/*
 * Up to len bytes into buf, returns how many.  hard: only as many bytes as
 * the pool holds credited entropy for (0 if none); otherwise output of the
 * pool regardless of the credit.
 */
size_t	amiga_entropy_extract(void *buf, size_t len, int hard);

/* sleep (signal mask 'mask' of the calling task) until hard output is
   available */
void	amiga_entropy_wait(unsigned long mask);

#endif /* AMIBSDNET_ENTROPY_H */
