/*	$NetBSD: if_virt.c,v 1.59 2021/06/16 00:21:20 riastradh Exp $	*/

/*
 * AmiBSDNet: NetBSD's sys/rump/net/lib/libvirtif/if_virt.c with these
 * changes (each is marked "AmiBSDNet" below):
 *  - virtif_unclone() freed the softc, which contains the ifnet, before
 *    detaching the ifnet; it now frees the softc last.  It also handles an
 *    interface whose link never worked (no backend, never
 *    ether_ifattach()ed) and frees the link string.
 *  - a failed SIOCSLINKSTR clears the freed link string pointer.
 *  - IFLINKSTR_UNSET (NetBSD: panic) takes the backend away and keeps the
 *    ifnet attached, as if_shmem.c does (sys/rump/net/lib/libshmif/
 *    if_shmem.c:520-525: finibackend() only); a later SIOCSLINKSTR links
 *    it again.  ether_ifdetach() is only called from virtif_unclone(),
 *    which if_clone_destroy() calls without IFNET_LOCK (net/if.c:
 *    1617-1629), as ether_ifdetach() requires (IFNET_ASSERT_UNLOCKED,
 *    net/if_ethersubr.c:1086; if_ioctl runs under IFNET_LOCK,
 *    net/if.c:3539).
 *  - SIOCADDMULTI / SIOCDELMULTI program the backend's multicast filter
 *    (VIFHYPER_MCAST) when ether_ioctl() reports a change (ENETRESET,
 *    net/if_ethersubr.c:1394-1398, 1448-1452).
 *  - VIFHYPER_DYING is not used: the SANA-II backend has nothing to
 *    refuse there.
 */

/*
 * Copyright (c) 2008, 2013 Antti Kantee.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: if_virt.c,v 1.59 2021/06/16 00:21:20 riastradh Exp $");

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/cprng.h>
#include <sys/module.h>
#include <sys/mutex.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>

#include <netinet/in.h>
#include <netinet/in_var.h>

#include "if_virt.h"
#include "virtif_user.h"

/*
 * AmiBSDNet: hypercall: add (1) or remove (0) one Ethernet group address
 * in the backend's multicast filter; returns an errno.
 */
#define VIFHYPER_MCAST VIF_BASENAME3(rumpcomp_,VIRTIF_BASE,_mcast)
int	VIFHYPER_MCAST(struct virtif_user *, int, const uint8_t *);

/*
 * Virtual interface.  Uses hypercalls to shovel packets back
 * and forth.  The exact method for shoveling depends on the
 * hypercall implementation.
 */

static int	virtif_init(struct ifnet *);
static int	virtif_ioctl(struct ifnet *, u_long, void *);
static void	virtif_start(struct ifnet *);
static void	virtif_stop(struct ifnet *, int);

struct virtif_sc {
	struct ethercom sc_ec;
	struct virtif_user *sc_viu;

	int sc_num;
	char *sc_linkstr;
	size_t sc_linkstrlen;		/* AmiBSDNet: to free it */
	bool sc_ethattached;		/* AmiBSDNet: ether_ifattach() done */
	kmutex_t sc_mcastlock;		/* AmiBSDNet: list and filter agree */
};

static int  virtif_clone(struct if_clone *, int);
static int  virtif_unclone(struct ifnet *);

struct if_clone VIF_CLONER =
    IF_CLONE_INITIALIZER(VIF_NAME, virtif_clone, virtif_unclone);

/*
 * AmiBSDNet: give the backend every single-address group of the
 * interface's multicast list (add) or take them away (!add): when a
 * backend comes and before it goes.  Address ranges are never given to
 * the backend (see virtif_mcast()).  sc_mcastlock is held, so the list
 * does not change meanwhile (every change comes through virtif_ioctl()).
 */
static void
virtif_mcast_all(struct virtif_sc *sc, struct virtif_user *viu, int add)
{
	struct ethercom *ec = &sc->sc_ec;
	struct ether_multistep step;
	struct ether_multi *enm;
	uint8_t (*list)[ETHER_ADDR_LEN];
	int max, n = 0, i, error;

	KASSERT(mutex_owned(&sc->sc_mcastlock));
	if ((max = ec->ec_multicnt) <= 0)
		return;
	list = kmem_alloc(max * sizeof(*list), KM_SLEEP);
	ETHER_LOCK(ec);
	ETHER_FIRST_MULTI(step, ec, enm);
	while (enm != NULL && n < max) {
		if (memcmp(enm->enm_addrlo, enm->enm_addrhi,
		    ETHER_ADDR_LEN) == 0)
			memcpy(list[n++], enm->enm_addrlo, ETHER_ADDR_LEN);
		ETHER_NEXT_MULTI(step, enm);
	}
	ETHER_UNLOCK(ec);
	/* (the hypercall may sleep: not under ETHER_LOCK) */
	for (i = 0; i < n; i++)
		if ((error = VIFHYPER_MCAST(viu, add, list[i])) != 0)
			aprint_error_ifnet(&ec->ec_if, "multicast %s of "
			    "%s failed: %d\n", add ? "add" : "delete",
			    ether_sprintf(list[i]), error);
	kmem_free(list, max * sizeof(*list));
}

/*
 * AmiBSDNet: one group joined or left (ENETRESET from ether_ioctl()).
 * A range (from a join of INADDR_ANY or the unspecified IPv6 address,
 * net/if_ethersubr.c:1302-1334) is refused: SANA-II drivers expand a
 * range address by address (wifipi unit.c:1169-1208 builds a list of
 * every address in it), so the IPv4 range of 2^23 addresses cannot be
 * handed to them.
 */
static int
virtif_mcast(struct virtif_sc *sc, int add, const uint8_t *lo,
    const uint8_t *hi)
{

	if (sc->sc_viu == NULL)
		return 0;	/* given to the next backend when it comes */
	if (memcmp(lo, hi, ETHER_ADDR_LEN) != 0)
		return add ? EOPNOTSUPP : 0;	/* (never given: nothing to
						   take away) */
	return VIFHYPER_MCAST(sc->sc_viu, add, lo);
}

static int
virtif_create(struct ifnet *ifp)
{
	uint8_t enaddr[ETHER_ADDR_LEN] = { 0xb2, 0x0a, 0x00, 0x0b, 0x0e, 0x01 };
	char enaddrstr[3*ETHER_ADDR_LEN];
	struct virtif_sc *sc = ifp->if_softc;
	struct virtif_user *viu;
	int error;

	if (sc->sc_viu)
		panic("%s: already created", ifp->if_xname);

	enaddr[2] = cprng_fast32() & 0xff;
	enaddr[5] = sc->sc_num & 0xff;

	if ((error = VIFHYPER_CREATE(sc->sc_linkstr,
	    sc, enaddr, &viu)) != 0) {
		/* (ENOENT: the driver did not open or set up, which
		   src/host/sana2.c has logged already in plain words) */
		if (error != ENOENT)
			printf("VIFHYPER_CREATE failed: %d\n", error);
		return error;
	}

	if (!sc->sc_ethattached) {
		ether_ifattach(ifp, enaddr);
		sc->sc_ethattached = true;
	} else if (memcmp(enaddr, CLLADDR(ifp->if_sadl),
	    ETHER_ADDR_LEN) != 0) {
		/* AmiBSDNet: linked again, to hardware with another address
		   (as if_tap.c:536 changes the address of an attached ifnet) */
		if_set_sadl(ifp, enaddr, ETHER_ADDR_LEN, false);
	}
	ether_snprintf(enaddrstr, sizeof(enaddrstr), enaddr);
	aprint_normal_ifnet(ifp, "Ethernet address %s\n", enaddrstr);

	/* AmiBSDNet: groups joined while there was no backend */
	mutex_enter(&sc->sc_mcastlock);
	virtif_mcast_all(sc, viu, 1);
	sc->sc_viu = viu;
	mutex_exit(&sc->sc_mcastlock);

	IFQ_SET_READY(&ifp->if_snd);

	return 0;
}

/* AmiBSDNet: the backend goes; the ifnet stays as it is */
static void
virtif_unlink(struct virtif_sc *sc)
{
	struct virtif_user *viu;

	mutex_enter(&sc->sc_mcastlock);
	viu = sc->sc_viu;
	virtif_mcast_all(sc, viu, 0);
	sc->sc_viu = NULL;
	mutex_exit(&sc->sc_mcastlock);
	VIFHYPER_DESTROY(viu);
}

static int
virtif_clone(struct if_clone *ifc, int num)
{
	struct virtif_sc *sc;
	struct ifnet *ifp;
	int error = 0;

	sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
	sc->sc_num = num;
	mutex_init(&sc->sc_mcastlock, MUTEX_DEFAULT, IPL_NONE);
	ifp = &sc->sc_ec.ec_if;

	if_initname(ifp, VIF_NAME, num);
	ifp->if_softc = sc;

	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = virtif_init;
	ifp->if_ioctl = virtif_ioctl;
	ifp->if_start = virtif_start;
	ifp->if_stop = virtif_stop;
	ifp->if_mtu = ETHERMTU;
	ifp->if_dlt = DLT_EN10MB;

	if_initialize(ifp);
	if_register(ifp);

#ifndef RUMP_VIF_LINKSTR
	/*
	 * if the underlying interface does not expect linkstr, we can
	 * create everything now.  Otherwise, we need to wait for
	 * SIOCSLINKSTR.
	 */
#define LINKSTRNUMLEN 16
	sc->sc_linkstr = kmem_alloc(LINKSTRNUMLEN, KM_SLEEP);
	if (sc->sc_linkstr == NULL) {
		error = ENOMEM;
		goto fail;
	}
	snprintf(sc->sc_linkstr, LINKSTRNUMLEN, "%d", sc->sc_num);
	error = virtif_create(ifp);
	if (error) {
fail:
		if_detach(ifp);
		if (sc->sc_linkstr != NULL)
			kmem_free(sc->sc_linkstr, LINKSTRNUMLEN);
#undef LINKSTRNUMLEN
		mutex_destroy(&sc->sc_mcastlock);
		kmem_free(sc, sizeof(*sc));
		ifp->if_softc = NULL;
	}
#endif /* !RUMP_VIF_LINKSTR */

	return error;
}

static int
virtif_unclone(struct ifnet *ifp)
{
	struct virtif_sc *sc = ifp->if_softc;

	if (ifp->if_flags & IFF_UP)
		return EBUSY;

	if (sc->sc_viu != NULL) {
		virtif_stop(ifp, 1);
		if_down(ifp);
		virtif_unlink(sc);
	}

	/* AmiBSDNet: only an ifnet that was ether_ifattach()ed (a link
	   string that never worked leaves it without) */
	if (sc->sc_ethattached)
		ether_ifdetach(ifp);
	if_detach(ifp);

	if (sc->sc_linkstr != NULL && sc->sc_linkstrlen)
		kmem_free(sc->sc_linkstr, sc->sc_linkstrlen);
	mutex_destroy(&sc->sc_mcastlock);
	kmem_free(sc, sizeof(*sc));	/* the ifnet is part of it */

	return 0;
}

static int
virtif_init(struct ifnet *ifp)
{
	struct virtif_sc *sc = ifp->if_softc;

	if (sc->sc_viu == NULL)
		return ENXIO;

	ifp->if_flags |= IFF_RUNNING;
	return 0;
}

static int
virtif_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	struct virtif_sc *sc = ifp->if_softc;
	int rv;

	switch (cmd) {
#ifdef RUMP_VIF_LINKSTR
	struct ifdrv *ifd;
	size_t linkstrlen;

#ifndef RUMP_VIF_LINKSTRMAX
#define RUMP_VIF_LINKSTRMAX 4096
#endif

	case SIOCGLINKSTR:
		ifd = data;

		if (!sc->sc_linkstr) {
			rv = ENOENT;
			break;
		}
		linkstrlen = strlen(sc->sc_linkstr)+1;

		if (ifd->ifd_cmd == IFLINKSTR_QUERYLEN) {
			ifd->ifd_len = linkstrlen;
			rv = 0;
			break;
		}
		if (ifd->ifd_cmd != 0) {
			rv = ENOTTY;
			break;
		}

		rv = copyoutstr(sc->sc_linkstr,
		    ifd->ifd_data, MIN(ifd->ifd_len,linkstrlen), NULL);
		break;
	case SIOCSLINKSTR:
		if (ifp->if_flags & IFF_UP) {
			rv = EBUSY;
			break;
		}

		ifd = data;

		if (ifd->ifd_cmd == IFLINKSTR_UNSET) {
			/*
			 * AmiBSDNet: the backend (the SANA-II driver) goes
			 * and the ifnet stays attached, as if_shmem.c does
			 * (see the top of the file); NetBSD's if_virt.c
			 * panics here, and a bsdsocket program can send this.
			 */
			if (sc->sc_viu != NULL) {
				virtif_stop(ifp, 1);
				virtif_unlink(sc);
			}
			if (sc->sc_linkstr != NULL && sc->sc_linkstrlen)
				kmem_free(sc->sc_linkstr, sc->sc_linkstrlen);
			sc->sc_linkstr = NULL;
			sc->sc_linkstrlen = 0;
			rv = 0;
			break;
		} else if (ifd->ifd_cmd != 0) {
			rv = ENOTTY;
			break;
		} else if (sc->sc_linkstr) {
			rv = EBUSY;
			break;
		}

		if (ifd->ifd_len > RUMP_VIF_LINKSTRMAX) {
			rv = E2BIG;
			break;
		} else if (ifd->ifd_len < 1) {
			rv = EINVAL;
			break;
		}


		sc->sc_linkstr = kmem_alloc(ifd->ifd_len, KM_SLEEP);
		sc->sc_linkstrlen = ifd->ifd_len;
		rv = copyinstr(ifd->ifd_data, sc->sc_linkstr,
		    ifd->ifd_len, NULL);
		if (rv) {
			kmem_free(sc->sc_linkstr, ifd->ifd_len);
			sc->sc_linkstr = NULL;	/* AmiBSDNet: no dangling */
			sc->sc_linkstrlen = 0;
			break;
		}

		rv = virtif_create(ifp);
		if (rv) {
			kmem_free(sc->sc_linkstr, ifd->ifd_len);
			sc->sc_linkstr = NULL;
			sc->sc_linkstrlen = 0;
		}
		break;
#endif /* RUMP_VIF_LINKSTR */
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		/*
		 * AmiBSDNet: the list is ethercom's (it exists from
		 * ether_ifattach() on, also without a backend); a change is
		 * given to the backend.  A join the backend refuses is
		 * taken off the list again, so that list and filter agree.
		 */
		if (!sc->sc_ethattached) {
			rv = ENXIO;
			break;
		}
		mutex_enter(&sc->sc_mcastlock);
		rv = ether_ioctl(ifp, cmd, data);
		if (rv == ENETRESET) {
			const struct sockaddr *sa =
			    ifreq_getaddr(cmd, (struct ifreq *)data);
			uint8_t lo[ETHER_ADDR_LEN], hi[ETHER_ADDR_LEN];

			rv = ether_multiaddr(sa, lo, hi);
			if (rv == 0)
				rv = virtif_mcast(sc, cmd == SIOCADDMULTI,
				    lo, hi);
			if (rv != 0 && cmd == SIOCADDMULTI)
				(void)ether_delmulti(sa, &sc->sc_ec);
		}
		mutex_exit(&sc->sc_mcastlock);
		break;
	default:
		if (!sc->sc_linkstr)
			rv = ENXIO;
		else
			rv = ether_ioctl(ifp, cmd, data);
		if (rv == ENETRESET)
			rv = 0;
		break;
	}

	return rv;
}

/*
 * Output packets in-context until outgoing queue is empty.
 * Leave responsibility of choosing whether or not to drop the
 * kernel lock to VIPHYPER_SEND().
 */
#define LB_SH 32
static void
virtif_start(struct ifnet *ifp)
{
	struct virtif_sc *sc = ifp->if_softc;
	struct mbuf *m, *m0;
	struct iovec io[LB_SH];
	int i;

	/* AmiBSDNet: no backend (link string unset): nothing can be sent */
	if (sc->sc_viu == NULL) {
		IFQ_PURGE(&ifp->if_snd);
		return;
	}

	ifp->if_flags |= IFF_OACTIVE;

	for (;;) {
		IF_DEQUEUE(&ifp->if_snd, m0);
		if (!m0) {
			break;
		}

		m = m0;
		for (i = 0; i < LB_SH && m; ) {
			if (m->m_len) {
				io[i].iov_base = mtod(m, void *);
				io[i].iov_len = m->m_len;
				i++;
			}
			m = m->m_next;
		}
		if (i == LB_SH && m)
			panic("lazy bum");
		bpf_mtap(ifp, m0, BPF_D_OUT);

		VIFHYPER_SEND(sc->sc_viu, io, i);

		m_freem(m0);
		if_statinc(ifp, if_opackets);
	}

	ifp->if_flags &= ~IFF_OACTIVE;
}

static void
virtif_stop(struct ifnet *ifp, int disable)
{

	/* AmiBSDNet: the host side needs no call: with IFF_RUNNING clear,
	   VIF_DELIVERPKT() drops every frame the driver delivers, and
	   ether_output() refuses to send (ENETDOWN, net/if_ethersubr.c:
	   "(IFF_UP | IFF_RUNNING) != (IFF_UP | IFF_RUNNING)"); the driver
	   stays open until VIFHYPER_DESTROY */
	ifp->if_flags &= ~IFF_RUNNING;
}

void
VIF_DELIVERPKT(struct virtif_sc *sc, struct iovec *iov, size_t iovlen)
{
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	struct ether_header *eth;
	struct mbuf *m;
	size_t i;
	int off, olen;
	bool passup;
	const int align
	    = ALIGN(sizeof(struct ether_header)) - sizeof(struct ether_header);

	if ((ifp->if_flags & IFF_RUNNING) == 0)
		return;

	m = m_gethdr(M_NOWAIT, MT_DATA);
	if (m == NULL)
		return; /* drop packet */
	m->m_len = m->m_pkthdr.len = 0;

	for (i = 0, off = align; i < iovlen; i++) {
		olen = m->m_pkthdr.len;
		m_copyback(m, off, iov[i].iov_len, iov[i].iov_base);
		off += iov[i].iov_len;
		if (olen + off != m->m_pkthdr.len) {
			aprint_verbose_ifnet(ifp, "m_copyback failed\n");
			m_freem(m);
			return;
		}
	}
	m->m_data += align;
	m->m_pkthdr.len -= align;
	m->m_len -= align;

	eth = mtod(m, struct ether_header *);
	if (memcmp(eth->ether_dhost, CLLADDR(ifp->if_sadl),
	    ETHER_ADDR_LEN) == 0) {
		passup = true;
	} else if (ETHER_IS_MULTICAST(eth->ether_dhost)) {
		passup = true;
	} else if (ifp->if_flags & IFF_PROMISC) {
		m->m_flags |= M_PROMISC;
		passup = true;
	} else {
		passup = false;
	}

	if (passup) {
		int bound;
		m_set_rcvif(m, ifp);
		KERNEL_LOCK(1, NULL);
		/* Prevent LWP migrations between CPUs for psref(9) */
		bound = curlwp_bind();
		if_input(ifp, m);
		curlwp_bindx(bound);
		KERNEL_UNLOCK_LAST(NULL);
	} else {
		m_freem(m);
	}
	m = NULL;
}

/*
 * The following ensures that no two modules using if_virt end up with
 * the same module name.  MODULE() and modcmd wrapped in ... bad mojo.
 */
#define VIF_MOJO(x) MODULE(MODULE_CLASS_DRIVER,x,NULL);
#define VIF_MODULE() VIF_MOJO(VIF_BASENAME(if_virt_,VIRTIF_BASE))
#define VIF_MODCMD VIF_BASENAME3(if_virt_,VIRTIF_BASE,_modcmd)
VIF_MODULE();
static int
VIF_MODCMD(modcmd_t cmd, void *opaque)
{
	int error = 0;

	switch (cmd) {
	case MODULE_CMD_INIT:
		if_clone_attach(&VIF_CLONER);
		break;
	case MODULE_CMD_FINI:
		/*
		 * not sure if interfaces are refcounted
		 * and properly protected
		 */
#if 0
		if_clone_detach(&VIF_CLONER);
#else
		error = ENOTTY;
#endif
		break;
	default:
		error = ENOTTY;
	}
	return error;
}
