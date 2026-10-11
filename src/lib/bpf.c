/*
 * bsdsocket.library: the Berkeley packet filter calls (bpf_open() and
 * the other bpf_* functions).
 *
 * "the doc" is downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc.
 *
 * The packet filter is NetBSD's netbsd-src/sys/net/bpf.c with
 * bpf_filter.c, which tools/build.py does not compile: the rump kernel
 * has only bpf_stub.c (netbsd-src/sys/rump/librump/rumpnet/
 * Makefile.rumpnet).  So this library has SB_BPF_CHANNELS (sblib.h) = 0
 * channels, which SocketBaseTagList() reports through
 * SBTC_NUM_PACKET_FILTER_CHANNELS; the doc (bpf_open) says that tag
 * tells "how many channels are available and whether this
 * "bsdsocket.library" version supports the packet filter mechanism in
 * the first place".  Every channel number and handle is then outside the
 * channels there are, which each of the calls reports with the error the
 * doc gives for a channel that "does not refer to a valid packet filter
 * channel": ENXIO.
 *
 * To offer channels, the kernel build needs bpf.c and bpf_filter.c in the
 * components (with the device and its /dev/bpf file operations reachable
 * from the server threads), and this file then maps the handles to those
 * descriptors.
 */

#include <exec/types.h>

#include "sblib.h"

/* a channel number or handle that is none of the SB_BPF_CHANNELS */
static LONG
no_channel(struct SocketBase *sb)
{

	sb_set_errno(sb, ENXIO);
	return -1;
}

LONG
sb_bpf_open(struct SocketBase *sb, LONG channel)
{

	return no_channel(sb);
}

LONG
sb_bpf_close(struct SocketBase *sb, LONG channel)
{

	return no_channel(sb);
}

LONG
sb_bpf_read(struct SocketBase *sb, LONG channel, APTR buffer, LONG len)
{

	return no_channel(sb);
}

LONG
sb_bpf_write(struct SocketBase *sb, LONG channel, APTR buffer, LONG len)
{

	return no_channel(sb);
}

LONG
sb_bpf_set_notify_mask(struct SocketBase *sb, LONG channel, ULONG signal_mask)
{

	return no_channel(sb);
}

LONG
sb_bpf_set_interrupt_mask(struct SocketBase *sb, LONG channel,
    ULONG signal_mask)
{

	return no_channel(sb);
}

LONG
sb_bpf_ioctl(struct SocketBase *sb, LONG channel, ULONG command, APTR buffer)
{

	return no_channel(sb);
}

LONG
sb_bpf_data_waiting(struct SocketBase *sb, LONG channel)
{

	return no_channel(sb);
}
