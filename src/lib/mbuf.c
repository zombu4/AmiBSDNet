/*
 * bsdsocket.library: the mbuf calls (the "kernel memory API",
 * SBTC_HAVE_KERNEL_MEMORY_API): mbuf_get(), mbuf_gethdr(), mbuf_free(),
 * mbuf_freem(), mbuf_copym(), mbuf_copydata(), mbuf_copyback(),
 * mbuf_cat(), mbuf_adj(), mbuf_prepend() and mbuf_pullup().
 *
 * "the doc" is downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/doc/bsdsocket.doc.
 * Each call "is functionally identical to the BSD kernel routine" m_get(),
 * m_gethdr(), m_free(), m_freem(), m_copym(), m_copydata(),
 * m_copyback(), m_cat(), m_adj(), m_prepend(), m_pullup() (the doc).  They
 * work on Roadshow's mbuf, the 4.4BSD one, of the SDK header
 * .../netinclude/sys/mbuf.h (struct mbuf, 128 bytes, with the packet
 * header and external cluster storage), not on the NetBSD kernel's, which
 * is another structure; the behaviour of each routine is that of
 * netbsd-src/sys/kern/uipc_mbuf.c and its 4.4BSD ancestor.
 *
 * An mbuf is allocated aligned to MSIZE, so that dtom() works; a cluster
 * has a use count in front of its data (shared by mbuf_copym()).  The type
 * of an mbuf is MT_DATA (1), the value of netbsd-src/sys/sys/mbuf.h: the
 * SDK header has no mbuf types.  Storage with an ext_free routine (set
 * by whoever built such an mbuf) belongs to its creator: its release
 * routine's calling convention is not documented, so it is never called,
 * and copying such an mbuf copies the data.
 *
 * max_protohdr (mbuf_pullup()'s "up to max_protohdr-len extra bytes" to
 * avoid being called next time) is a kernel variable whose value in
 * Roadshow is not documented; no extra bytes are moved, which changes
 * only how often the routine has to be called again.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "sblib.h"

/* the SDK header: netinclude/sys/mbuf.h */
#define	MSIZE		128
#define	MCLBYTES	2048
#define	M_COPYALL	1000000000
#define	M_EXT		0x0001
#define	M_PKTHDR	0x0002
#define	M_EOR		0x0004
#define	M_BCAST		0x0100
#define	M_MCAST		0x0200
#define	M_COPYFLAGS	(M_PKTHDR | M_EOR | M_BCAST | M_MCAST)
#define	MT_DATA		1

struct m_hdr {
	struct mbuf *mh_next;
	struct mbuf *mh_nextpkt;
	APTR mh_data;
	LONG mh_len;
	WORD mh_type;
	WORD mh_flags;
};

struct pkthdr {
	APTR rcvif;
	LONG len;
};

struct m_ext {
	APTR ext_buf;
	APTR ext_free;
	ULONG ext_size;
};

#define	MLEN	(MSIZE - sizeof(struct m_hdr))
#define	MHLEN	(MLEN - sizeof(struct pkthdr))

struct mbuf {
	struct m_hdr m_hdr;
	union {
		struct {
			struct pkthdr MH_pkthdr;
			union {
				struct m_ext MH_ext;
				UBYTE MH_databuf[MHLEN];
			} MH_dat;
		} MH;
		UBYTE M_databuf[MLEN];
	} M_dat;
};
#define	m_next		m_hdr.mh_next
#define	m_len		m_hdr.mh_len
#define	m_data		m_hdr.mh_data
#define	m_type		m_hdr.mh_type
#define	m_flags		m_hdr.mh_flags
#define	m_pkthdr	M_dat.MH.MH_pkthdr
#define	m_ext		M_dat.MH.MH_dat.MH_ext
#define	m_pktdat	M_dat.MH.MH_dat.MH_databuf
#define	m_dat		M_dat.M_databuf
SB_CHECK(sizeof(struct mbuf) == MSIZE);

/* a cluster: its use count in front of the data */
struct cluster {
	ULONG refs;
	ULONG reserved;
	UBYTE data[MCLBYTES];
};

#define	MIN(a, b)	((a) < (b) ? (a) : (b))

/* data area of an mbuf, as it is when it holds no cluster */
#define	MB_END(m)	((UBYTE *)(m) + MSIZE)

/* a cleared mbuf, with or without packet header */
static struct mbuf *
mb_alloc(int hdr)
{
	UBYTE *raw;
	ULONG a;
	struct mbuf *m;

	/* room to align to MSIZE with the address of the block just before */
	if ((raw = AllocVec(2 * MSIZE + 4, MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
		return NULL;
	a = ((ULONG)raw + 4 + MSIZE - 1) & ~(ULONG)(MSIZE - 1);
	*(UBYTE **)(a - 4) = raw;
	m = (struct mbuf *)a;
	m->m_type = MT_DATA;
	if (hdr) {
		m->m_flags = M_PKTHDR;
		m->m_data = m->m_pktdat;
	} else
		m->m_data = m->m_dat;
	return m;
}

static void
mb_dealloc(struct mbuf *m)
{

	FreeVec(*(UBYTE **)((ULONG)m - 4));
}

/* give an mbuf a cluster of its own */
static int
mb_cluster(struct mbuf *m)
{
	struct cluster *c;

	if ((c = AllocVec(sizeof(*c), MEMF_PUBLIC)) == NULL)
		return 0;
	c->refs = 1;
	m->m_ext.ext_buf = c->data;
	m->m_ext.ext_free = NULL;
	m->m_ext.ext_size = MCLBYTES;
	m->m_data = c->data;
	m->m_flags |= M_EXT;
	return 1;
}

/* is the storage of this mbuf one of our clusters (shareable)? */
static int
mb_ours(const struct mbuf *m)
{

	return (m->m_flags & M_EXT) && m->m_ext.ext_free == NULL;
}

static struct cluster *
mb_cl(const struct mbuf *m)
{

	return (struct cluster *)((UBYTE *)m->m_ext.ext_buf -
	    __builtin_offsetof(struct cluster, data));
}

static void
mb_release(struct mbuf *m)
{
	struct cluster *c;
	int last;

	if (!mb_ours(m))
		return;
	c = mb_cl(m);
	Forbid();
	last = --c->refs == 0;
	Permit();
	if (last)
		FreeVec(c);
}

/* M_COPY_PKTHDR(to, from) of 4.4BSD */
static void
mb_copy_pkthdr(struct mbuf *to, const struct mbuf *from)
{

	to->m_flags = from->m_flags & M_COPYFLAGS;
	to->m_data = to->m_pktdat;
	to->m_pkthdr = from->m_pkthdr;
}

struct mbuf *
sb_mbuf_get(struct SocketBase *sb)
{

	return mb_alloc(0);
}

struct mbuf *
sb_mbuf_gethdr(struct SocketBase *sb)
{

	return mb_alloc(1);
}

/* m_free(): one mbuf; the next one of the chain is returned */
struct mbuf *
sb_mbuf_free(struct SocketBase *sb, struct mbuf *m)
{
	struct mbuf *next;

	if (m == NULL)
		return NULL;
	next = m->m_next;
	mb_release(m);
	mb_dealloc(m);
	return next;
}

void
sb_mbuf_freem(struct SocketBase *sb, struct mbuf *m)
{

	while (m != NULL)
		m = sb_mbuf_free(sb, m);
}

/* m_copym(): a cluster is shared, other data copied */
struct mbuf *
sb_mbuf_copym(struct SocketBase *sb, struct mbuf *m, LONG off, LONG len)
{
	struct mbuf *n, *top = NULL, **np = &top;
	LONG off0 = off, len0 = len, take, chunk;
	int copyhdr = 0;
	const UBYTE *src;

	if (m == NULL || off < 0 || len < 0)
		return NULL;
	if (off == 0 && (m->m_flags & M_PKTHDR))
		copyhdr = 1;
	while (off > 0) {
		if (m == NULL)
			return NULL;
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	while (len > 0) {
		if (m == NULL) {
			if (len != M_COPYALL)
				goto fail;	/* the chain is shorter */
			break;
		}
		take = MIN(len, m->m_len - off);
		if (mb_ours(m)) {
			if ((n = mb_alloc(copyhdr)) == NULL)
				goto fail;
			*np = n;
			np = &n->m_next;
			n->m_type = m->m_type;
			if (copyhdr) {
				mb_copy_pkthdr(n, m);
				n->m_pkthdr.len = len0 == M_COPYALL ?
				    n->m_pkthdr.len - off0 : len0;
				copyhdr = 0;
			}
			n->m_len = take;
			n->m_ext = m->m_ext;
			Forbid();
			mb_cl(m)->refs++;
			Permit();
			n->m_data = (UBYTE *)m->m_data + off;
			n->m_flags |= M_EXT;
		} else {
			src = (const UBYTE *)m->m_data + off;
			for (chunk = take; chunk > 0;) {
				LONG c = MIN(chunk, MCLBYTES);

				if ((n = mb_alloc(copyhdr)) == NULL)
					goto fail;
				*np = n;
				np = &n->m_next;
				n->m_type = m->m_type;
				if (copyhdr) {
					mb_copy_pkthdr(n, m);
					n->m_pkthdr.len = len0 == M_COPYALL ?
					    n->m_pkthdr.len - off0 : len0;
					copyhdr = 0;
				}
				if (c > (LONG)(n->m_flags & M_PKTHDR ? MHLEN :
				    MLEN) && !mb_cluster(n))
					goto fail;
				CopyMem((APTR)src, n->m_data, c);
				n->m_len = c;
				src += c;
				chunk -= c;
			}
		}
		if (len != M_COPYALL)
			len -= take;
		off = 0;
		m = m->m_next;
	}
	return top;
fail:
	sb_mbuf_freem(sb, top);
	return NULL;
}

/* m_copydata(): the whole range must be in the chain */
LONG
sb_mbuf_copydata(struct SocketBase *sb, struct mbuf *m, LONG off, LONG len,
    APTR cp)
{
	struct mbuf *p;
	LONG have = 0, count;
	UBYTE *dst = cp;

	/* (off + len must not overflow: it is compared below) */
	if (m == NULL || cp == NULL || off < 0 || len < 0 ||
	    len > 0x7fffffffL - off)
		return -1;
	for (p = m; p != NULL && have < off + len; p = p->m_next)
		have += p->m_len;
	if (have < off + len)
		return -1;
	while (off > 0 && off >= m->m_len) {
		off -= m->m_len;
		m = m->m_next;
	}
	while (len > 0) {
		count = MIN(m->m_len - off, len);
		CopyMem((UBYTE *)m->m_data + off, dst, count);
		len -= count;
		dst += count;
		off = 0;
		m = m->m_next;
	}
	return 0;
}

/* m_copyback(): the chain grows (new mbufs, cleared) when the data reach
   past its end, and a packet header's length with it */
LONG
sb_mbuf_copyback(struct SocketBase *sb, struct mbuf *m0, LONG off, LONG len,
    APTR cp)
{
	struct mbuf *m = m0, *n;
	LONG mlen, totlen = 0;
	const UBYTE *src = cp;
	int rv = 0;

	/* (len + off must not overflow: it sizes new mbufs below) */
	if (m0 == NULL || cp == NULL || off < 0 || len < 0 ||
	    len > 0x7fffffffL - off)
		return -1;
	while (off > (mlen = m->m_len)) {
		off -= mlen;
		totlen += mlen;
		if (m->m_next == NULL) {
			if ((n = mb_alloc(0)) == NULL) {
				rv = -1;
				goto out;
			}
			n->m_type = m->m_type;
			n->m_len = MIN((LONG)MLEN, len + off);
			m->m_next = n;
		}
		m = m->m_next;
	}
	while (len > 0) {
		mlen = MIN(m->m_len - off, len);
		CopyMem((APTR)src, (UBYTE *)m->m_data + off, mlen);
		src += mlen;
		len -= mlen;
		mlen += off;
		off = 0;
		totlen += mlen;
		if (len == 0)
			break;
		if (m->m_next == NULL) {
			if ((n = mb_alloc(0)) == NULL) {
				rv = -1;
				break;
			}
			n->m_type = m->m_type;
			n->m_len = MIN((LONG)MLEN, len);
			m->m_next = n;
		}
		m = m->m_next;
	}
out:
	if ((m0->m_flags & M_PKTHDR) && m0->m_pkthdr.len < totlen)
		m0->m_pkthdr.len = totlen;
	return rv;
}

/* m_cat(): the second chain is joined to the first, or its data moved
   into the first one's last mbuf if they fit there; the second chain is
   consumed; "The 'first_chain->m_pkthdr' field will not be updated" */
LONG
sb_mbuf_cat(struct SocketBase *sb, struct mbuf *m, struct mbuf *n)
{

	if (m == NULL || n == NULL)
		return -1;
	while (m->m_next)
		m = m->m_next;
	while (n) {
		if ((m->m_flags & M_EXT) ||
		    (UBYTE *)m->m_data + m->m_len + n->m_len >= MB_END(m)) {
			m->m_next = n;
			return 0;
		}
		CopyMem(n->m_data, (UBYTE *)m->m_data + m->m_len, n->m_len);
		m->m_len += n->m_len;
		n = sb_mbuf_free(sb, n);
	}
	return 0;
}

/* m_adj(): positive, from the head; negative, from the tail */
LONG
sb_mbuf_adj(struct SocketBase *sb, struct mbuf *mp, LONG req_len)
{
	LONG len = req_len, count;
	struct mbuf *m;

	if ((m = mp) == NULL)
		return -1;
	if (len >= 0) {
		while (m != NULL && len > 0) {
			if (m->m_len <= len) {
				len -= m->m_len;
				m->m_len = 0;
				m = m->m_next;
			} else {
				m->m_len -= len;
				m->m_data = (UBYTE *)m->m_data + len;
				len = 0;
			}
		}
		if (mp->m_flags & M_PKTHDR)
			mp->m_pkthdr.len -= req_len - len;
		return 0;
	}
	/* from the tail: find the last mbuf and the length of the chain */
	len = -len;
	count = 0;
	for (;;) {
		count += m->m_len;
		if (m->m_next == NULL)
			break;
		m = m->m_next;
	}
	if (m->m_len >= len) {
		m->m_len -= len;
		if (mp->m_flags & M_PKTHDR)
			mp->m_pkthdr.len -= len;
		return 0;
	}
	/* the chain is to be `count' long: cut it there */
	count -= len;
	if (count < 0)
		count = 0;
	m = mp;
	if (m->m_flags & M_PKTHDR)
		m->m_pkthdr.len = count;
	for (; m; m = m->m_next) {
		if (m->m_len >= count) {
			m->m_len = count;
			break;
		}
		count -= m->m_len;
	}
	if (m)
		while (m->m_next)
			(m = m->m_next)->m_len = 0;
	return 0;
}

/* m_prepend(): a new mbuf in front (the packet header moves to it); the
   chain is freed if there is none */
struct mbuf *
sb_mbuf_prepend(struct SocketBase *sb, struct mbuf *m, LONG len)
{
	struct mbuf *mn;

	if (m == NULL)
		return NULL;
	if (len < 0 || len > (LONG)(m->m_flags & M_PKTHDR ? MHLEN : MLEN) ||
	    (mn = mb_alloc(0)) == NULL) {
		sb_mbuf_freem(sb, m);
		return NULL;
	}
	mn->m_type = m->m_type;
	if (m->m_flags & M_PKTHDR) {
		mn->m_flags = M_PKTHDR;
		mb_copy_pkthdr(mn, m);
		m->m_flags &= ~M_PKTHDR;
	}
	mn->m_next = m;
	/* MH_ALIGN(): the data at the end, aligned, leaving room in front */
	if (len < (LONG)(mn->m_flags & M_PKTHDR ? MHLEN : MLEN))
		mn->m_data = (UBYTE *)mn->m_data + ((((mn->m_flags & M_PKTHDR) ?
		    MHLEN : MLEN) - len) & ~(sizeof(LONG) - 1));
	mn->m_len = len;
	return mn;
}

/* m_pullup(): the first len bytes in the data area of the first mbuf; the
   chain is freed if that cannot be done */
struct mbuf *
sb_mbuf_pullup(struct SocketBase *sb, struct mbuf *n, LONG len)
{
	struct mbuf *m;
	LONG space, count;

	if (n == NULL)
		return NULL;
	if (len < 0)
		goto bad;
	/* if the first mbuf has no cluster and room for len bytes without
	   shifting, pull up into it, otherwise a new mbuf goes in front */
	if (!(n->m_flags & M_EXT) && (UBYTE *)n->m_data + len < MB_END(n) &&
	    n->m_next) {
		if (n->m_len >= len)
			return n;
		m = n;
		n = n->m_next;
		len -= m->m_len;
	} else {
		if (len > (LONG)MHLEN)
			goto bad;
		if ((m = mb_alloc(0)) == NULL)
			goto bad;
		m->m_type = n->m_type;
		m->m_len = 0;
		if (n->m_flags & M_PKTHDR) {
			m->m_flags = M_PKTHDR;
			mb_copy_pkthdr(m, n);
			n->m_flags &= ~M_PKTHDR;
		}
	}
	space = MB_END(m) - ((UBYTE *)m->m_data + m->m_len);
	do {
		count = MIN(MIN(len, space), n->m_len);
		CopyMem(n->m_data, (UBYTE *)m->m_data + m->m_len, count);
		len -= count;
		m->m_len += count;
		n->m_len -= count;
		space -= count;
		if (n->m_len)
			n->m_data = (UBYTE *)n->m_data + count;
		else
			n = sb_mbuf_free(sb, n);
	} while (len > 0 && n);
	if (len > 0) {
		sb_mbuf_free(sb, m);
		goto bad;
	}
	m->m_next = n;
	return m;
bad:
	sb_mbuf_freem(sb, n);
	return NULL;
}
