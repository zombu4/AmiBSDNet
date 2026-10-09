/*
 * Host-side access to SANA-II interfaces (src/host/sana2.c), used by the
 * stack program, e.g. the DHCP client, which must send and receive frames
 * before the interface has an address.
 */
#ifndef SANA2_HOST_H
#define SANA2_HOST_H

#include <exec/types.h>

struct virtif_user;

/*
 * Called on the interface's I/O process for every received frame
 * (complete Ethernet frame).  Return 1 to consume it, 0 to pass it on to
 * the kernel.  Must not block.
 */
typedef int (*sana_tap_fn)(void *ctx, const UBYTE *frame, ULONG len);

struct virtif_user *sana_find(const char *devname, ULONG unit);
const UBYTE	*sana_macaddr(struct virtif_user *);
void	sana_set_tap(struct virtif_user *, sana_tap_fn, void *ctx);
void	sana_raw_send(struct virtif_user *, const UBYTE *frame, ULONG len);
void	sana_stats(struct virtif_user *, ULONG *rx, ULONG *tx,
	    ULONG *rxdrop, ULONG *txdrop);

/* kernel send entry, also usable for raw frames */
struct hiovec;
void	rumpcomp_sana_send(struct virtif_user *, struct hiovec *, size_t);

#endif /* SANA2_HOST_H */
