/* $NetBSD: if_msk.c,v 1.92 2019/10/17 05:55:18 msaitoh Exp $ */
/*	$OpenBSD: if_msk.c,v 1.79 2009/10/15 17:54:56 deraadt Exp $	*/

/*
 * Copyright (c) 1997, 1998, 1999, 2000
 *	Bill Paul <wpaul@ctr.columbia.edu>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: /c/ncvs/src/sys/pci/if_sk.c,v 1.20 2000/04/22 02:16:37 wpaul Exp $
 */

/*
 * Copyright (c) 2003 Nathan L. Binkert <binkertn@umich.edu>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: if_msk.c,v 1.92 2019/10/17 05:55:18 msaitoh Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/device.h>
#include <sys/queue.h>
#include <sys/callout.h>
#include <sys/sysctl.h>
#include <sys/endian.h>
#ifdef __NetBSD__
 #define letoh16 htole16
 #define letoh32 htole32
#endif

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <net/if_media.h>

#include <net/bpf.h>
#include <sys/rndsource.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>
#include <dev/mii/brgphyreg.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

#include <dev/pci/if_skreg.h>
#include <dev/pci/if_mskvar.h>

int mskc_probe(device_t, cfdata_t, void *);
void mskc_attach(device_t, device_t, void *);
int mskc_detach(device_t, int);
void mskc_reset(struct sk_softc *);
static bool mskc_suspend(device_t, const pmf_qual_t *);
static bool mskc_resume(device_t, const pmf_qual_t *);
int msk_probe(device_t, cfdata_t, void *);
void msk_attach(device_t, device_t, void *);
int msk_detach(device_t, int);
void msk_reset(struct sk_if_softc *);
int mskcprint(void *, const char *);
int msk_intr(void *);
void msk_intr_yukon(struct sk_if_softc *);
void msk_rxeof(struct sk_if_softc *, uint16_t, uint32_t);
void msk_txeof(struct sk_if_softc *);
int msk_encap(struct sk_if_softc *, struct mbuf *, uint32_t *);
void msk_start(struct ifnet *);
int msk_ioctl(struct ifnet *, u_long, void *);
int msk_init(struct ifnet *);
void msk_init_yukon(struct sk_if_softc *);
void msk_stop(struct ifnet *, int);
void msk_watchdog(struct ifnet *);
int msk_newbuf(struct sk_if_softc *, bus_dmamap_t);
int msk_alloc_jumbo_mem(struct sk_if_softc *);
void *msk_jalloc(struct sk_if_softc *);
void msk_jfree(struct mbuf *, void *, size_t, void *);
int msk_init_rx_ring(struct sk_if_softc *);
int msk_init_tx_ring(struct sk_if_softc *);
void msk_fill_rx_ring(struct sk_if_softc *);

void msk_update_int_mod(struct sk_softc *, int);

int msk_miibus_readreg(device_t, int, int, uint16_t *);
int msk_miibus_writereg(device_t, int, int, uint16_t);
void msk_miibus_statchg(struct ifnet *);

void msk_setmulti(struct sk_if_softc *);
void msk_setpromisc(struct sk_if_softc *);
void msk_tick(void *);
static void msk_fill_rx_tick(void *);

/* #define MSK_DEBUG 1 */
#ifdef MSK_DEBUG
#define DPRINTF(x)	if (mskdebug) printf x
#define DPRINTFN(n, x)	if (mskdebug >= (n)) printf x
int	mskdebug = MSK_DEBUG;

void msk_dump_txdesc(struct msk_tx_desc *, int);
void msk_dump_mbuf(struct mbuf *);
void msk_dump_bytes(const char *, int);
#else
#define DPRINTF(x)
#define DPRINTFN(n, x)
#endif

static int msk_sysctl_handler(SYSCTLFN_PROTO);
static int msk_root_num;

#define MSK_ADDR_LO(x)	((uint64_t) (x) & 0xffffffffUL)
#define MSK_ADDR_HI(x)	((uint64_t) (x) >> 32)

/* supported device vendors */
static const struct msk_product {
	pci_vendor_id_t		msk_vendor;
	pci_product_id_t	msk_product;
} msk_products[] = {
	{ PCI_VENDOR_DLINK,		PCI_PRODUCT_DLINK_DGE550SX },
	{ PCI_VENDOR_DLINK,		PCI_PRODUCT_DLINK_DGE550T_B1 },
	{ PCI_VENDOR_DLINK,		PCI_PRODUCT_DLINK_DGE560SX },
	{ PCI_VENDOR_DLINK,		PCI_PRODUCT_DLINK_DGE560T },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKONII_8021CU },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKONII_8021X },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKONII_8022CU },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKONII_8022X },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8035 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8036 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8038 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8039 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8040 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8040T },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8042 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8048 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8050 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8052 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8053 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8055 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8055_2 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8056 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8057 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8058 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8059 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKONII_8061CU },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKONII_8061X },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKONII_8062CU },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKONII_8062X },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8070 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8071 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8072 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8075 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_8079 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_C032 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_C033 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_C034 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_C036 },
	{ PCI_VENDOR_MARVELL,		PCI_PRODUCT_MARVELL_YUKON_C042 },
	{ PCI_VENDOR_SCHNEIDERKOCH,	PCI_PRODUCT_SCHNEIDERKOCH_SK_9SXX },
	{ PCI_VENDOR_SCHNEIDERKOCH,	PCI_PRODUCT_SCHNEIDERKOCH_SK_9E21 },
	{ 0,				0 }
};

static inline uint32_t
sk_win_read_4(struct sk_softc *sc, uint32_t reg)
{
	return CSR_READ_4(sc, reg);
}

static inline uint16_t
sk_win_read_2(struct sk_softc *sc, uint32_t reg)
{
	return CSR_READ_2(sc, reg);
}

static inline uint8_t
sk_win_read_1(struct sk_softc *sc, uint32_t reg)
{
	return CSR_READ_1(sc, reg);
}

static inline void
sk_win_write_4(struct sk_softc *sc, uint32_t reg, uint32_t x)
{
	CSR_WRITE_4(sc, reg, x);
}

static inline void
sk_win_write_2(struct sk_softc *sc, uint32_t reg, uint16_t x)
{
	CSR_WRITE_2(sc, reg, x);
}

static inline void
sk_win_write_1(struct sk_softc *sc, uint32_t reg, uint8_t x)
{
	CSR_WRITE_1(sc, reg, x);
}

int
msk_miibus_readreg(device_t dev, int phy, int reg, uint16_t *val)
{
	struct sk_if_softc *sc_if = device_private(dev);
	uint16_t data;
	int i;

	SK_YU_WRITE_2(sc_if, YUKON_SMICR, YU_SMICR_PHYAD(phy) |
		      YU_SMICR_REGAD(reg) | YU_SMICR_OP_READ);

	for (i = 0; i < SK_TIMEOUT; i++) {
		DELAY(1);
		data = SK_YU_READ_2(sc_if, YUKON_SMICR);
		if (data & YU_SMICR_READ_VALID)
			break;
	}

	if (i == SK_TIMEOUT) {
		aprint_error_dev(sc_if->sk_dev, "phy failed to come ready\n");
		return ETIMEDOUT;
	}

	DPRINTFN(9, ("msk_miibus_readreg: i=%d, timeout=%d\n", i, SK_TIMEOUT));

	*val = SK_YU_READ_2(sc_if, YUKON_SMIDR);

	DPRINTFN(9, ("msk_miibus_readreg phy=%d, reg=%#x, val=%#hx\n",
		phy, reg, *val));

	return 0;
}

int
msk_miibus_writereg(device_t dev, int phy, int reg, uint16_t val)
{
	struct sk_if_softc *sc_if = device_private(dev);
	int i;

	DPRINTFN(9, ("msk_miibus_writereg phy=%d reg=%#x val=%#hx\n",
		     phy, reg, val));

	SK_YU_WRITE_2(sc_if, YUKON_SMIDR, val);
	SK_YU_WRITE_2(sc_if, YUKON_SMICR, YU_SMICR_PHYAD(phy) |
		      YU_SMICR_REGAD(reg) | YU_SMICR_OP_WRITE);

	for (i = 0; i < SK_TIMEOUT; i++) {
		DELAY(1);
		if (!(SK_YU_READ_2(sc_if, YUKON_SMICR) & YU_SMICR_BUSY))
			break;
	}

	if (i == SK_TIMEOUT) {
		aprint_error_dev(sc_if->sk_dev, "phy write timed out\n");
		return ETIMEDOUT;
	}

	return 0;
}

void
msk_miibus_statchg(struct ifnet *ifp)
{
	struct sk_if_softc *sc_if = ifp->if_softc;
	struct mii_data *mii = &sc_if->sk_mii;
	struct ifmedia_entry *ife = mii->mii_media.ifm_cur;
	int gpcr;

	gpcr = SK_YU_READ_2(sc_if, YUKON_GPCR);
	gpcr &= (YU_GPCR_TXEN | YU_GPCR_RXEN);

	if (IFM_SUBTYPE(ife->ifm_media) != IFM_AUTO ||
	    sc_if->sk_softc->sk_type == SK_YUKON_FE_P) {
		/* Set speed. */
		gpcr |= YU_GPCR_SPEED_DIS;
		switch (IFM_SUBTYPE(mii->mii_media_active)) {
		case IFM_1000_SX:
		case IFM_1000_LX:
		case IFM_1000_CX:
		case IFM_1000_T:
			gpcr |= (YU_GPCR_GIG | YU_GPCR_SPEED);
			break;
		case IFM_100_TX:
			gpcr |= YU_GPCR_SPEED;
			break;
		}

		/* Set duplex. */
		gpcr |= YU_GPCR_DPLX_DIS;
		if ((mii->mii_media_active & IFM_FDX) != 0)
			gpcr |= YU_GPCR_DUPLEX;

		/* Disable flow control. */
		gpcr |= YU_GPCR_FCTL_DIS;
		gpcr |= (YU_GPCR_FCTL_TX_DIS | YU_GPCR_FCTL_RX_DIS);
	}

	SK_YU_WRITE_2(sc_if, YUKON_GPCR, gpcr);

	DPRINTFN(9, ("msk_miibus_statchg: gpcr=%x\n",
		     SK_YU_READ_2(sc_if, YUKON_GPCR)));
}

void
msk_setmulti(struct sk_if_softc *sc_if)
{
	struct ifnet *ifp= &sc_if->sk_ethercom.ec_if;
	uint32_t hashes[2] = { 0, 0 };
	int h;
	struct ethercom *ec = &sc_if->sk_ethercom;
	struct ether_multi *enm;
	struct ether_multistep step;
	uint16_t reg;

	/* First, zot all the existing filters. */
	SK_YU_WRITE_2(sc_if, YUKON_MCAH1, 0);
	SK_YU_WRITE_2(sc_if, YUKON_MCAH2, 0);
	SK_YU_WRITE_2(sc_if, YUKON_MCAH3, 0);
	SK_YU_WRITE_2(sc_if, YUKON_MCAH4, 0);


	/* Now program new ones. */
	reg = SK_YU_READ_2(sc_if, YUKON_RCR);
	reg |= YU_RCR_UFLEN;
allmulti:
	if (ifp->if_flags & IFF_ALLMULTI || ifp->if_flags & IFF_PROMISC) {
		if ((ifp->if_flags & IFF_PROMISC) != 0)
			reg &= ~(YU_RCR_UFLEN | YU_RCR_MUFLEN);
		else if ((ifp->if_flags & IFF_ALLMULTI) != 0) {
			hashes[0] = 0xFFFFFFFF;
			hashes[1] = 0xFFFFFFFF;
		}
	} else {
		/* First find the tail of the list. */
		ETHER_LOCK(ec);
		ETHER_FIRST_MULTI(step, ec, enm);
		while (enm != NULL) {
			if (memcmp(enm->enm_addrlo, enm->enm_addrhi,
				 ETHER_ADDR_LEN)) {
				ifp->if_flags |= IFF_ALLMULTI;
				ETHER_UNLOCK(ec);
				goto allmulti;
			}
			h = ether_crc32_be(enm->enm_addrlo, ETHER_ADDR_LEN) &
			    ((1 << SK_HASH_BITS) - 1);
			if (h < 32)
				hashes[0] |= (1 << h);
			else
				hashes[1] |= (1 << (h - 32));

			ETHER_NEXT_MULTI(step, enm);
		}
		ETHER_UNLOCK(ec);
		reg |= YU_RCR_MUFLEN;
	}

	SK_YU_WRITE_2(sc_if, YUKON_MCAH1, hashes[0] & 0xffff);
	SK_YU_WRITE_2(sc_if, YUKON_MCAH2, (hashes[0] >> 16) & 0xffff);
	SK_YU_WRITE_2(sc_if, YUKON_MCAH3, hashes[1] & 0xffff);
	SK_YU_WRITE_2(sc_if, YUKON_MCAH4, (hashes[1] >> 16) & 0xffff);
	SK_YU_WRITE_2(sc_if, YUKON_RCR, reg);
}

void
msk_setpromisc(struct sk_if_softc *sc_if)
{
	struct ifnet *ifp = &sc_if->sk_ethercom.ec_if;

	if (ifp->if_flags & IFF_PROMISC)
		SK_YU_CLRBIT_2(sc_if, YUKON_RCR,
		    YU_RCR_UFLEN | YU_RCR_MUFLEN);
	else
		SK_YU_SETBIT_2(sc_if, YUKON_RCR,
		    YU_RCR_UFLEN | YU_RCR_MUFLEN);
}

int
msk_init_rx_ring(struct sk_if_softc *sc_if)
{
	struct msk_chain_data	*cd = &sc_if->sk_cdata;
	struct msk_ring_data	*rd = sc_if->sk_rdata;
	struct msk_rx_desc	*r;
	int			i, nexti;

	memset(rd->sk_rx_ring, 0, sizeof(struct msk_rx_desc) * MSK_RX_RING_CNT);

	for (i = 0; i < MSK_RX_RING_CNT; i++) {
		cd->sk_rx_chain[i].sk_le = &rd->sk_rx_ring[i];
		if (i == (MSK_RX_RING_CNT - 1))
			nexti = 0;
		else
			nexti = i + 1;
		cd->sk_rx_chain[i].sk_next = &cd->sk_rx_chain[nexti];
	}

	sc_if->sk_cdata.sk_rx_prod = 0;
	sc_if->sk_cdata.sk_rx_cons = 0;
	sc_if->sk_cdata.sk_rx_cnt = 0;
	sc_if->sk_cdata.sk_rx_hiaddr = 0;

	/* Mark the first ring element to initialize the high address. */
	sc_if->sk_cdata.sk_rx_hiaddr = 0;
	r = &rd->sk_rx_ring[cd->sk_rx_prod];
	r->sk_addr = htole32(cd->sk_rx_hiaddr);
	r->sk_len = 0;
	r->sk_ctl = 0;
	r->sk_opcode = SK_Y2_BMUOPC_ADDR64 | SK_Y2_RXOPC_OWN;
	MSK_CDRXSYNC(sc_if, cd->sk_rx_prod,
	    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);
	SK_INC(sc_if->sk_cdata.sk_rx_prod, MSK_RX_RING_CNT);
	sc_if->sk_cdata.sk_rx_cnt++;

	msk_fill_rx_ring(sc_if);
	return 0;
}

int
msk_init_tx_ring(struct sk_if_softc *sc_if)
{
	struct sk_softc		*sc = sc_if->sk_softc;
	struct msk_chain_data	*cd = &sc_if->sk_cdata;
	struct msk_ring_data	*rd = sc_if->sk_rdata;
	struct msk_tx_desc	*t;
	bus_dmamap_t		dmamap;
	struct sk_txmap_entry	*entry;
	int			i, nexti;

	memset(rd->sk_tx_ring, 0, sizeof(struct msk_tx_desc) * MSK_TX_RING_CNT);

	SIMPLEQ_INIT(&sc_if->sk_txmap_head);
	for (i = 0; i < MSK_TX_RING_CNT; i++) {
		cd->sk_tx_chain[i].sk_le = &rd->sk_tx_ring[i];
		if (i == (MSK_TX_RING_CNT - 1))
			nexti = 0;
		else
			nexti = i + 1;
		cd->sk_tx_chain[i].sk_next = &cd->sk_tx_chain[nexti];

		if (bus_dmamap_create(sc->sc_dmatag, SK_JLEN, SK_NTXSEG,
		   SK_JLEN, 0, BUS_DMA_NOWAIT, &dmamap))
			return ENOBUFS;

		entry = malloc(sizeof(*entry), M_DEVBUF, M_NOWAIT);
		if (!entry) {
			bus_dmamap_destroy(sc->sc_dmatag, dmamap);
			return ENOBUFS;
		}
		entry->dmamap = dmamap;
		SIMPLEQ_INSERT_HEAD(&sc_if->sk_txmap_head, entry, link);
	}

	sc_if->sk_cdata.sk_tx_prod = 0;
	sc_if->sk_cdata.sk_tx_cons = 0;
	sc_if->sk_cdata.sk_tx_cnt = 0;
	sc_if->sk_cdata.sk_tx_hiaddr = 0;

	/* Mark the first ring element to initialize the high address. */
	sc_if->sk_cdata.sk_tx_hiaddr = 0;
	t = &rd->sk_tx_ring[cd->sk_tx_prod];
	t->sk_addr = htole32(cd->sk_tx_hiaddr);
	t->sk_len = 0;
	t->sk_ctl = 0;
	t->sk_opcode = SK_Y2_BMUOPC_ADDR64 | SK_Y2_TXOPC_OWN;
	MSK_CDTXSYNC(sc_if, 0, MSK_TX_RING_CNT,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	SK_INC(sc_if->sk_cdata.sk_tx_prod, MSK_TX_RING_CNT);
	sc_if->sk_cdata.sk_tx_cnt++;

	return 0;
}

int
msk_newbuf(struct sk_if_softc *sc_if, bus_dmamap_t dmamap)
{
	struct mbuf		*m_new = NULL;
	struct sk_chain		*c;
	struct msk_rx_desc	*r;
	void			*buf = NULL;
	bus_addr_t		addr;

	MGETHDR(m_new, M_DONTWAIT, MT_DATA);
	if (m_new == NULL)
		return ENOBUFS;

	/* Allocate the jumbo buffer */
	buf = msk_jalloc(sc_if);
	if (buf == NULL) {
		m_freem(m_new);
		DPRINTFN(1, ("%s jumbo allocation failed -- packet "
		    "dropped!\n", sc_if->sk_ethercom.ec_if.if_xname));
		return ENOBUFS;
	}

	/* Attach the buffer to the mbuf */
	m_new->m_len = m_new->m_pkthdr.len = SK_JLEN;
	MEXTADD(m_new, buf, SK_JLEN, 0, msk_jfree, sc_if);

	m_adj(m_new, ETHER_ALIGN);

	addr = dmamap->dm_segs[0].ds_addr +
		  ((vaddr_t)m_new->m_data -
		   (vaddr_t)sc_if->sk_cdata.sk_jumbo_buf);

	if (sc_if->sk_cdata.sk_rx_hiaddr != MSK_ADDR_HI(addr)) {
		c = &sc_if->sk_cdata.sk_rx_chain[sc_if->sk_cdata.sk_rx_prod];
		r = c->sk_le;
		c->sk_mbuf = NULL;
		r->sk_addr = htole32(MSK_ADDR_HI(addr));
		r->sk_len = 0;
		r->sk_ctl = 0;
		r->sk_opcode = SK_Y2_BMUOPC_ADDR64 | SK_Y2_RXOPC_OWN;
		sc_if->sk_cdata.sk_rx_hiaddr = MSK_ADDR_HI(addr);

		MSK_CDRXSYNC(sc_if, sc_if->sk_cdata.sk_rx_prod,
		    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

		SK_INC(sc_if->sk_cdata.sk_rx_prod, MSK_RX_RING_CNT);
		sc_if->sk_cdata.sk_rx_cnt++;

		DPRINTFN(10, ("%s: rx ADDR64: %#x\n",
		    sc_if->sk_ethercom.ec_if.if_xname,
			(unsigned)MSK_ADDR_HI(addr)));
	}

	c = &sc_if->sk_cdata.sk_rx_chain[sc_if->sk_cdata.sk_rx_prod];
	r = c->sk_le;
	c->sk_mbuf = m_new;
	r->sk_addr = htole32(MSK_ADDR_LO(addr));
	r->sk_len = htole16(SK_JLEN);
	r->sk_ctl = 0;
	r->sk_opcode = SK_Y2_RXOPC_PACKET | SK_Y2_RXOPC_OWN;

	MSK_CDRXSYNC(sc_if, sc_if->sk_cdata.sk_rx_prod,
	    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

	SK_INC(sc_if->sk_cdata.sk_rx_prod, MSK_RX_RING_CNT);
	sc_if->sk_cdata.sk_rx_cnt++;

	return 0;
}

/*
 * Memory management for jumbo frames.
 */

int
msk_alloc_jumbo_mem(struct sk_if_softc *sc_if)
{
	struct sk_softc		*sc = sc_if->sk_softc;
	char *ptr, *kva;
	int		i, state, error;
	struct sk_jpool_entry	*entry;

	state = error = 0;

	/* Grab a big chunk o' storage. */
	if (bus_dmamem_alloc(sc->sc_dmatag, MSK_JMEM, PAGE_SIZE, 0,
	     &sc_if->sk_cdata.sk_jumbo_seg, 1, &sc_if->sk_cdata.sk_jumbo_nseg,
	     BUS_DMA_NOWAIT)) {
		aprint_error(": can't alloc rx buffers");
		return ENOBUFS;
	}

	state = 1;
	if (bus_dmamem_map(sc->sc_dmatag, &sc_if->sk_cdata.sk_jumbo_seg,
	    sc_if->sk_cdata.sk_jumbo_nseg, MSK_JMEM, (void **)&kva,
	    BUS_DMA_NOWAIT)) {
		aprint_error(": can't map dma buffers (%d bytes)", MSK_JMEM);
		error = ENOBUFS;
		goto out;
	}

	state = 2;
	if (bus_dmamap_create(sc->sc_dmatag, MSK_JMEM, 1, MSK_JMEM, 0,
	    BUS_DMA_NOWAIT, &sc_if->sk_cdata.sk_rx_jumbo_map)) {
		aprint_error(": can't create dma map");
		error = ENOBUFS;
		goto out;
	}

	state = 3;
	if (bus_dmamap_load(sc->sc_dmatag, sc_if->sk_cdata.sk_rx_jumbo_map,
			    kva, MSK_JMEM, NULL, BUS_DMA_NOWAIT)) {
		aprint_error(": can't load dma map");
		error = ENOBUFS;
		goto out;
	}

	state = 4;
	sc_if->sk_cdata.sk_jumbo_buf = (void *)kva;
	DPRINTFN(1,("msk_jumbo_buf = %p\n",
		(void *)sc_if->sk_cdata.sk_jumbo_buf));

	LIST_INIT(&sc_if->sk_jfree_listhead);
	LIST_INIT(&sc_if->sk_jinuse_listhead);
	mutex_init(&sc_if->sk_jpool_mtx, MUTEX_DEFAULT, IPL_NET);

	/*
	 * Now divide it up into 9K pieces and save the addresses
	 * in an array.
	 */
	ptr = sc_if->sk_cdata.sk_jumbo_buf;
	for (i = 0; i < MSK_JSLOTS; i++) {
		sc_if->sk_cdata.sk_jslots[i] = ptr;
		ptr += SK_JLEN;
		entry = malloc(sizeof(struct sk_jpool_entry),
		    M_DEVBUF, M_NOWAIT);
		if (entry == NULL) {
			sc_if->sk_cdata.sk_jumbo_buf = NULL;
			aprint_error(": no memory for jumbo buffer queue!");
			error = ENOBUFS;
			goto out;
		}
		entry->slot = i;
		LIST_INSERT_HEAD(&sc_if->sk_jfree_listhead,
				 entry, jpool_entries);
	}
out:
	if (error != 0) {
		switch (state) {
		case 4:
			bus_dmamap_unload(sc->sc_dmatag,
			    sc_if->sk_cdata.sk_rx_jumbo_map);
			/* FALLTHROUGH */
		case 3:
			bus_dmamap_destroy(sc->sc_dmatag,
			    sc_if->sk_cdata.sk_rx_jumbo_map);
			/* FALLTHROUGH */
		case 2:
			bus_dmamem_unmap(sc->sc_dmatag, kva, MSK_JMEM);
			/* FALLTHROUGH */
		case 1:
			bus_dmamem_free(sc->sc_dmatag,
			    &sc_if->sk_cdata.sk_jumbo_seg,
			    sc_if->sk_cdata.sk_jumbo_nseg);
			break;
		default:
			break;
		}
	}

	return error;
}

static void
msk_free_jumbo_mem(struct sk_if_softc *sc_if)
{
	struct sk_softc		*sc = sc_if->sk_softc;

	bus_dmamap_unload(sc->sc_dmatag, sc_if->sk_cdata.sk_rx_jumbo_map);
	bus_dmamap_destroy(sc->sc_dmatag, sc_if->sk_cdata.sk_rx_jumbo_map);
	bus_dmamem_unmap(sc->sc_dmatag, sc_if->sk_cdata.sk_jumbo_buf, MSK_JMEM);
	bus_dmamem_free(sc->sc_dmatag, &sc_if->sk_cdata.sk_jumbo_seg,
	    sc_if->sk_cdata.sk_jumbo_nseg);
}

/*
 * Allocate a jumbo buffer.
 */
void *
msk_jalloc(struct sk_if_softc *sc_if)
{
	struct sk_jpool_entry	*entry;

	mutex_enter(&sc_if->sk_jpool_mtx);
	entry = LIST_FIRST(&sc_if->sk_jfree_listhead);

	if (entry == NULL) {
		mutex_exit(&sc_if->sk_jpool_mtx);
		return NULL;
	}

	LIST_REMOVE(entry, jpool_entries);
	LIST_INSERT_HEAD(&sc_if->sk_jinuse_listhead, entry, jpool_entries);
	mutex_exit(&sc_if->sk_jpool_mtx);
	return sc_if->sk_cdata.sk_jslots[entry->slot];
}

/*
 * Release a jumbo buffer.
 */
void
msk_jfree(struct mbuf *m, void *buf, size_t size, void *arg)
{
	struct sk_jpool_entry *entry;
	struct sk_if_softc *sc;
	int i;

	/* Extract the softc struct pointer. */
	sc = (struct sk_if_softc *)arg;

	if (sc == NULL)
		panic("msk_jfree: can't find softc pointer!");

	/* calculate the slot this buffer belongs to */
	i = ((vaddr_t)buf
	     - (vaddr_t)sc->sk_cdata.sk_jumbo_buf) / SK_JLEN;

	if ((i < 0) || (i >= MSK_JSLOTS))
		panic("msk_jfree: asked to free buffer that we don't manage!");

	mutex_enter(&sc->sk_jpool_mtx);
	entry = LIST_FIRST(&sc->sk_jinuse_listhead);
	if (entry == NULL)
		panic("msk_jfree: buffer not in use!");
	entry->slot = i;
	LIST_REMOVE(entry, jpool_entries);
	LIST_INSERT_HEAD(&sc->sk_jfree_listhead, entry, jpool_entries);
	mutex_exit(&sc->sk_jpool_mtx);

	if (__predict_true(m != NULL))
		pool_cache_put(mb_cache, m);

	/* Now that we know we have a free RX buffer, refill if running out */
	if ((sc->sk_ethercom.ec_if.if_flags & IFF_RUNNING) != 0
	    && sc->sk_cdata.sk_rx_cnt < (MSK_RX_RING_CNT/3))
		callout_schedule(&sc->sk_tick_rx, 0);
}

int
msk_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	struct sk_if_softc *sc = ifp->if_softc;
	int s, error;

	s = splnet();

	DPRINTFN(2, ("msk_ioctl ETHER cmd %lx\n", cmd));
	switch (cmd) {
	case SIOCSIFFLAGS:
		if ((error = ifioctl_common(ifp, cmd, data)) != 0)
			break;

		switch (ifp->if_flags & (IFF_UP | IFF_RUNNING)) {
		case IFF_RUNNING:
			msk_stop(ifp, 1);
			break;
		case IFF_UP:
			msk_init(ifp);
			break;
		case IFF_UP | IFF_RUNNING:
			if ((ifp->if_flags ^ sc->sk_if_flags) == IFF_PROMISC) {
				msk_setpromisc(sc);
				msk_setmulti(sc);
			} else
				msk_init(ifp);
			break;
		}
		sc->sk_if_flags = ifp->if_flags;
		break;
	default:
		error = ether_ioctl(ifp, cmd, data);
		if (error == ENETRESET) {
			error = 0;
			if (cmd != SIOCADDMULTI && cmd != SIOCDELMULTI)
				;
			else if (ifp->if_flags & IFF_RUNNING) {
				/*
				 * Multicast list has changed; set the hardware
				 * filter accordingly.
				 */
				msk_setmulti(sc);
			}
		}
		break;
	}

	splx(s);
	return error;
}

void
msk_update_int_mod(struct sk_softc *sc, int verbose)
{
	uint32_t imtimer_ticks;

	/*
 	 * Configure interrupt moderation. The moderation timer
	 * defers interrupts specified in the interrupt moderation
	 * timer mask based on the timeout specified in the interrupt
	 * moderation timer init register. Each bit in the timer
	 * register represents one tick, so to specify a timeout in
	 * microseconds, we have to multiply by the correct number of
	 * ticks-per-microsecond.
	 */
	switch (sc->sk_type) {
	case SK_YUKON_EC:
	case SK_YUKON_EC_U:
	case SK_YUKON_EX:
	case SK_YUKON_SUPR:
	case SK_YUKON_ULTRA2:
	case SK_YUKON_OPTIMA:
	case SK_YUKON_PRM:
	case SK_YUKON_OPTIMA2:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_EC;
		break;
	case SK_YUKON_FE:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_FE;
		break;
	case SK_YUKON_FE_P:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_FE_P;
		break;
	case SK_YUKON_XL:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_XL;
		break;
	default:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON;
	}
	if (verbose)
		aprint_verbose_dev(sc->sk_dev,
		    "interrupt moderation is %d us\n", sc->sk_int_mod);
	sk_win_write_4(sc, SK_IMTIMERINIT, SK_IM_USECS(sc->sk_int_mod));
	sk_win_write_4(sc, SK_IMMR, SK_ISR_TX1_S_EOF | SK_ISR_TX2_S_EOF |
	    SK_ISR_RX1_EOF | SK_ISR_RX2_EOF);
	sk_win_write_1(sc, SK_IMTIMERCTL, SK_IMCTL_START);
	sc->sk_int_mod_pending = 0;
}

static int
msk_lookup(const struct pci_attach_args *pa)
{
	const struct msk_product *pmsk;

	for ( pmsk = &msk_products[0]; pmsk->msk_vendor != 0; pmsk++) {
		if (PCI_VENDOR(pa->pa_id) == pmsk->msk_vendor &&
		    PCI_PRODUCT(pa->pa_id) == pmsk->msk_product)
			return 1;
	}
	return 0;
}

/*
 * Probe for a SysKonnect GEnesis chip. Check the PCI vendor and device
 * IDs against our list and return a device name if we find a match.
 */
int
mskc_probe(device_t parent, cfdata_t match, void *aux)
{
	struct pci_attach_args *pa = (struct pci_attach_args *)aux;

	return msk_lookup(pa);
}

/*
 * Force the GEnesis into reset, then bring it out of reset.
 */
void
mskc_reset(struct sk_softc *sc)
{
	uint32_t imtimer_ticks, reg1;
	int reg;

	DPRINTFN(2, ("mskc_reset\n"));

	CSR_WRITE_1(sc, SK_CSR, SK_CSR_SW_RESET);
	CSR_WRITE_1(sc, SK_CSR, SK_CSR_MASTER_RESET);

	DELAY(1000);
	CSR_WRITE_1(sc, SK_CSR, SK_CSR_SW_UNRESET);
	DELAY(2);
	CSR_WRITE_1(sc, SK_CSR, SK_CSR_MASTER_UNRESET);
	sk_win_write_1(sc, SK_TESTCTL1, 2);

	if (sc->sk_type == SK_YUKON_EC_U || sc->sk_type == SK_YUKON_EX ||
	    sc->sk_type >= SK_YUKON_FE_P) {
		uint32_t our;

		CSR_WRITE_2(sc, SK_CSR, SK_CSR_WOL_ON);

		/* enable all clocks. */
		sk_win_write_4(sc, SK_Y2_PCI_REG(SK_PCI_OURREG3), 0);
		our = sk_win_read_4(sc, SK_Y2_PCI_REG(SK_PCI_OURREG4));
		our &= (SK_Y2_REG4_FORCE_ASPM_REQUEST |
			SK_Y2_REG4_ASPM_GPHY_LINK_DOWN |
			SK_Y2_REG4_ASPM_INT_FIFO_EMPTY |
			SK_Y2_REG4_ASPM_CLKRUN_REQUEST);
		/* Set all bits to 0 except bits 15..12 */
		sk_win_write_4(sc, SK_Y2_PCI_REG(SK_PCI_OURREG4), our);
		/* Set to default value */
		sk_win_write_4(sc, SK_Y2_PCI_REG(SK_PCI_OURREG5), 0);

		/*
		 * Disable status race, workaround for Yukon EC Ultra &
		 * Yukon EX.
		 */
		reg1 = sk_win_read_4(sc, SK_GPIO);
		reg1 |= SK_Y2_GPIO_STAT_RACE_DIS;
		sk_win_write_4(sc, SK_GPIO, reg1);
		sk_win_read_4(sc, SK_GPIO);
	}

	/* release PHY from PowerDown/Coma mode. */
	reg1 = sk_win_read_4(sc, SK_Y2_PCI_REG(SK_PCI_OURREG1));
	if (sc->sk_type == SK_YUKON_XL && sc->sk_rev > SK_YUKON_XL_REV_A1)
		reg1 |= (SK_Y2_REG1_PHY1_COMA | SK_Y2_REG1_PHY2_COMA);
	else
		reg1 &= ~(SK_Y2_REG1_PHY1_COMA | SK_Y2_REG1_PHY2_COMA);
	sk_win_write_4(sc, SK_Y2_PCI_REG(SK_PCI_OURREG1), reg1);

	if (sc->sk_type == SK_YUKON_XL && sc->sk_rev > SK_YUKON_XL_REV_A1)
		sk_win_write_1(sc, SK_Y2_CLKGATE,
		    SK_Y2_CLKGATE_LINK1_GATE_DIS |
		    SK_Y2_CLKGATE_LINK2_GATE_DIS |
		    SK_Y2_CLKGATE_LINK1_CORE_DIS |
		    SK_Y2_CLKGATE_LINK2_CORE_DIS |
		    SK_Y2_CLKGATE_LINK1_PCI_DIS | SK_Y2_CLKGATE_LINK2_PCI_DIS);
	else
		sk_win_write_1(sc, SK_Y2_CLKGATE, 0);

	CSR_WRITE_2(sc, SK_LINK_CTRL, SK_LINK_RESET_SET);
	CSR_WRITE_2(sc, SK_LINK_CTRL + SK_WIN_LEN, SK_LINK_RESET_SET);
	DELAY(1000);
	CSR_WRITE_2(sc, SK_LINK_CTRL, SK_LINK_RESET_CLEAR);
	CSR_WRITE_2(sc, SK_LINK_CTRL + SK_WIN_LEN, SK_LINK_RESET_CLEAR);

	if (sc->sk_type == SK_YUKON_EX || sc->sk_type == SK_YUKON_SUPR) {
		CSR_WRITE_2(sc, SK_GMAC_CTRL, SK_GMAC_BYP_MACSECRX |
		    SK_GMAC_BYP_MACSECTX | SK_GMAC_BYP_RETR_FIFO);
	}

	sk_win_write_1(sc, SK_TESTCTL1, 1);

	DPRINTFN(2, ("mskc_reset: sk_csr=%x\n", CSR_READ_1(sc, SK_CSR)));
	DPRINTFN(2, ("mskc_reset: sk_link_ctrl=%x\n",
		     CSR_READ_2(sc, SK_LINK_CTRL)));

	/* Disable ASF */
	CSR_WRITE_1(sc, SK_Y2_ASF_CSR, SK_Y2_ASF_RESET);
	CSR_WRITE_2(sc, SK_CSR, SK_CSR_ASF_OFF);

	/* Clear I2C IRQ noise */
	CSR_WRITE_4(sc, SK_I2CHWIRQ, 1);

	/* Disable hardware timer */
	CSR_WRITE_1(sc, SK_TIMERCTL, SK_IMCTL_STOP);
	CSR_WRITE_1(sc, SK_TIMERCTL, SK_IMCTL_IRQ_CLEAR);

	/* Disable descriptor polling */
	CSR_WRITE_4(sc, SK_DPT_TIMER_CTRL, SK_DPT_TCTL_STOP);

	/* Disable time stamps */
	CSR_WRITE_1(sc, SK_TSTAMP_CTL, SK_TSTAMP_STOP);
	CSR_WRITE_1(sc, SK_TSTAMP_CTL, SK_TSTAMP_IRQ_CLEAR);

	/* Enable RAM interface */
	sk_win_write_1(sc, SK_RAMCTL, SK_RAMCTL_UNRESET);
	for (reg = SK_TO0;reg <= SK_TO11; reg++)
		sk_win_write_1(sc, reg, 36);
	sk_win_write_1(sc, SK_RAMCTL + (SK_WIN_LEN / 2), SK_RAMCTL_UNRESET);
	for (reg = SK_TO0;reg <= SK_TO11; reg++)
		sk_win_write_1(sc, reg + (SK_WIN_LEN / 2), 36);

	/*
	 * Configure interrupt moderation. The moderation timer
	 * defers interrupts specified in the interrupt moderation
	 * timer mask based on the timeout specified in the interrupt
	 * moderation timer init register. Each bit in the timer
	 * register represents one tick, so to specify a timeout in
	 * microseconds, we have to multiply by the correct number of
	 * ticks-per-microsecond.
	 */
	switch (sc->sk_type) {
	case SK_YUKON_EC:
	case SK_YUKON_EC_U:
	case SK_YUKON_EX:
	case SK_YUKON_SUPR:
	case SK_YUKON_ULTRA2:
	case SK_YUKON_OPTIMA:
	case SK_YUKON_PRM:
	case SK_YUKON_OPTIMA2:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_EC;
		break;
	case SK_YUKON_FE:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_FE;
		break;
	case SK_YUKON_FE_P:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_FE_P;
		break;
	case SK_YUKON_XL:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_XL;
		break;
	default:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON;
		break;
	}

	/* Reset status ring. */
	memset(sc->sk_status_ring, 0,
	    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc));
	bus_dmamap_sync(sc->sc_dmatag, sc->sk_status_map, 0,
	    sc->sk_status_map->dm_mapsize, BUS_DMASYNC_PREREAD);
	sc->sk_status_idx = 0;

	sk_win_write_4(sc, SK_STAT_BMU_CSR, SK_STAT_BMU_RESET);
	sk_win_write_4(sc, SK_STAT_BMU_CSR, SK_STAT_BMU_UNRESET);

	sk_win_write_2(sc, SK_STAT_BMU_LIDX, MSK_STATUS_RING_CNT - 1);
	sk_win_write_4(sc, SK_STAT_BMU_ADDRLO,
	    MSK_ADDR_LO(sc->sk_status_map->dm_segs[0].ds_addr));
	sk_win_write_4(sc, SK_STAT_BMU_ADDRHI,
	    MSK_ADDR_HI(sc->sk_status_map->dm_segs[0].ds_addr));
	if (sc->sk_type == SK_YUKON_EC &&
	    sc->sk_rev == SK_YUKON_EC_REV_A1) {
		/* WA for dev. #4.3 */
		sk_win_write_2(sc, SK_STAT_BMU_TX_THRESH,
		    SK_STAT_BMU_TXTHIDX_MSK);
		/* WA for dev. #4.18 */
		sk_win_write_1(sc, SK_STAT_BMU_FIFOWM, 0x21);
		sk_win_write_1(sc, SK_STAT_BMU_FIFOIWM, 0x07);
	} else {
		sk_win_write_2(sc, SK_STAT_BMU_TX_THRESH, 0x000a);
		sk_win_write_1(sc, SK_STAT_BMU_FIFOWM, 0x10);
		if (sc->sk_type == SK_YUKON_XL)
			sk_win_write_1(sc, SK_STAT_BMU_FIFOIWM, 0x04);
		else
			sk_win_write_1(sc, SK_STAT_BMU_FIFOIWM, 0x10);
		sk_win_write_4(sc, SK_Y2_ISR_ITIMERINIT, 0x0190); /* 3.2us on Yukon-EC */
	}

#if 0
	sk_win_write_4(sc, SK_Y2_LEV_ITIMERINIT, SK_IM_USECS(100));
#endif
	sk_win_write_4(sc, SK_Y2_TX_ITIMERINIT, SK_IM_USECS(1000));

	/* Enable status unit. */
	sk_win_write_4(sc, SK_STAT_BMU_CSR, SK_STAT_BMU_ON);

	sk_win_write_1(sc, SK_Y2_LEV_ITIMERCTL, SK_IMCTL_START);
	sk_win_write_1(sc, SK_Y2_TX_ITIMERCTL, SK_IMCTL_START);
	sk_win_write_1(sc, SK_Y2_ISR_ITIMERCTL, SK_IMCTL_START);

	msk_update_int_mod(sc, 0);
}

int
msk_probe(device_t parent, cfdata_t match, void *aux)
{
	struct skc_attach_args *sa = aux;

	if (sa->skc_port != SK_PORT_A && sa->skc_port != SK_PORT_B)
		return 0;

	switch (sa->skc_type) {
	case SK_YUKON_XL:
	case SK_YUKON_EC_U:
	case SK_YUKON_EX:
	case SK_YUKON_EC:
	case SK_YUKON_FE:
	case SK_YUKON_FE_P:
	case SK_YUKON_SUPR:
	case SK_YUKON_ULTRA2:
	case SK_YUKON_OPTIMA:
	case SK_YUKON_PRM:
	case SK_YUKON_OPTIMA2:
		return 1;
	}

	return 0;
}

void
msk_reset(struct sk_if_softc *sc_if)
{
	/* GMAC and GPHY Reset */
	SK_IF_WRITE_4(sc_if, 0, SK_GMAC_CTRL, SK_GMAC_RESET_SET);
	SK_IF_WRITE_4(sc_if, 0, SK_GPHY_CTRL, SK_GPHY_RESET_SET);
	DELAY(1000);
	SK_IF_WRITE_4(sc_if, 0, SK_GPHY_CTRL, SK_GPHY_RESET_CLEAR);
	SK_IF_WRITE_4(sc_if, 0, SK_GMAC_CTRL, SK_GMAC_LOOP_OFF |
		      SK_GMAC_PAUSE_ON | SK_GMAC_RESET_CLEAR);
}

static bool
msk_resume(device_t dv, const pmf_qual_t *qual)
{
	struct sk_if_softc *sc_if = device_private(dv);

	msk_init_yukon(sc_if);
	return true;
}

/*
 * Each XMAC chip is attached as a separate logical IP interface.
 * Single port cards will have only one logical interface of course.
 */
void
msk_attach(device_t parent, device_t self, void *aux)
{
	struct sk_if_softc *sc_if = device_private(self);
	struct sk_softc *sc = device_private(parent);
	struct skc_attach_args *sa = aux;
	struct ifnet *ifp;
	struct mii_data * const mii = &sc_if->sk_mii;
	void *kva;
	int i;
	uint32_t chunk;
	int mii_flags;

	sc_if->sk_dev = self;
	sc_if->sk_port = sa->skc_port;
	sc_if->sk_softc = sc;
	sc->sk_if[sa->skc_port] = sc_if;

	DPRINTFN(2, ("begin msk_attach: port=%d\n", sc_if->sk_port));

	/*
	 * Get station address for this interface. Note that
	 * dual port cards actually come with three station
	 * addresses: one for each port, plus an extra. The
	 * extra one is used by the SysKonnect driver software
	 * as a 'virtual' station address for when both ports
	 * are operating in failover mode. Currently we don't
	 * use this extra address.
	 */
	for (i = 0; i < ETHER_ADDR_LEN; i++)
		sc_if->sk_enaddr[i] =
		    sk_win_read_1(sc, SK_MAC0_0 + (sa->skc_port * 8) + i);

	aprint_normal(": Ethernet address %s\n",
	    ether_sprintf(sc_if->sk_enaddr));

	/*
	 * Set up RAM buffer addresses. The Yukon2 has a small amount
	 * of SRAM on it, somewhere between 4K and 48K.  We need to
	 * divide this up between the transmitter and receiver.  We
	 * give the receiver 2/3 of the memory (rounded down), and the
	 * transmitter whatever remains.
	 */
	if (sc->sk_ramsize) {
		chunk = (2 * (sc->sk_ramsize / sizeof(uint64_t)) / 3) & ~0xff;
		sc_if->sk_rx_ramstart = 0;
		sc_if->sk_rx_ramend = sc_if->sk_rx_ramstart + chunk - 1;
		chunk = (sc->sk_ramsize / sizeof(uint64_t)) - chunk;
		sc_if->sk_tx_ramstart = sc_if->sk_rx_ramend + 1;
		sc_if->sk_tx_ramend = sc_if->sk_tx_ramstart + chunk - 1;

		DPRINTFN(2, ("msk_attach: rx_ramstart=%#x rx_ramend=%#x\n"
			     "           tx_ramstart=%#x tx_ramend=%#x\n",
			     sc_if->sk_rx_ramstart, sc_if->sk_rx_ramend,
			     sc_if->sk_tx_ramstart, sc_if->sk_tx_ramend));
	}

	/* Allocate the descriptor queues. */
	if (bus_dmamem_alloc(sc->sc_dmatag, sizeof(struct msk_ring_data),
	    PAGE_SIZE, 0, &sc_if->sk_ring_seg, 1, &sc_if->sk_ring_nseg,
	    BUS_DMA_NOWAIT)) {
		aprint_error(": can't alloc rx buffers\n");
		goto fail;
	}
	if (bus_dmamem_map(sc->sc_dmatag, &sc_if->sk_ring_seg,
	    sc_if->sk_ring_nseg,
	    sizeof(struct msk_ring_data), &kva, BUS_DMA_NOWAIT)) {
		aprint_error(": can't map dma buffers (%zu bytes)\n",
		       sizeof(struct msk_ring_data));
		goto fail_1;
	}
	if (bus_dmamap_create(sc->sc_dmatag, sizeof(struct msk_ring_data), 1,
	    sizeof(struct msk_ring_data), 0, BUS_DMA_NOWAIT,
	    &sc_if->sk_ring_map)) {
		aprint_error(": can't create dma map\n");
		goto fail_2;
	}
	if (bus_dmamap_load(sc->sc_dmatag, sc_if->sk_ring_map, kva,
	    sizeof(struct msk_ring_data), NULL, BUS_DMA_NOWAIT)) {
		aprint_error(": can't load dma map\n");
		goto fail_3;
	}
	sc_if->sk_rdata = (struct msk_ring_data *)kva;
	memset(sc_if->sk_rdata, 0, sizeof(struct msk_ring_data));

	if (sc->sk_type != SK_YUKON_FE &&
	    sc->sk_type != SK_YUKON_FE_P)
		sc_if->sk_pktlen = SK_JLEN;
	else
		sc_if->sk_pktlen = MCLBYTES;

	/* Try to allocate memory for jumbo buffers. */
	if (msk_alloc_jumbo_mem(sc_if)) {
		aprint_error(": jumbo buffer allocation failed\n");
		goto fail_3;
	}

	sc_if->sk_ethercom.ec_capabilities = ETHERCAP_VLAN_MTU;
	if (sc->sk_type != SK_YUKON_FE &&
	    sc->sk_type != SK_YUKON_FE_P)
		sc_if->sk_ethercom.ec_capabilities |= ETHERCAP_JUMBO_MTU;

	ifp = &sc_if->sk_ethercom.ec_if;
	ifp->if_softc = sc_if;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = msk_ioctl;
	ifp->if_start = msk_start;
	ifp->if_stop = msk_stop;
	ifp->if_init = msk_init;
	ifp->if_watchdog = msk_watchdog;
	ifp->if_baudrate = 1000000000;
	IFQ_SET_MAXLEN(&ifp->if_snd, MSK_TX_RING_CNT - 1);
	IFQ_SET_READY(&ifp->if_snd);
	strlcpy(ifp->if_xname, device_xname(sc_if->sk_dev), IFNAMSIZ);

	msk_reset(sc_if);

	/*
	 * Do miibus setup.
	 */
	DPRINTFN(2, ("msk_attach: 1\n"));

	mii->mii_ifp = ifp;
	mii->mii_readreg = msk_miibus_readreg;
	mii->mii_writereg = msk_miibus_writereg;
	mii->mii_statchg = msk_miibus_statchg;

	sc_if->sk_ethercom.ec_mii = mii;
	ifmedia_init(&mii->mii_media, 0, ether_mediachange, ether_mediastatus);
	mii_flags = MIIF_DOPAUSE;
	if (sc->sk_fibertype)
		mii_flags |= MIIF_HAVEFIBER;
	mii_attach(self, mii, 0xffffffff, 0, MII_OFFSET_ANY, mii_flags);
	if (LIST_FIRST(&mii->mii_phys) == NULL) {
		aprint_error_dev(sc_if->sk_dev, "no PHY found!\n");
		ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_MANUAL,
			    0, NULL);
		ifmedia_set(&mii->mii_media, IFM_ETHER | IFM_MANUAL);
	} else
		ifmedia_set(&mii->mii_media, IFM_ETHER | IFM_AUTO);

	callout_init(&sc_if->sk_tick_ch, 0);
	callout_setfunc(&sc_if->sk_tick_ch, msk_tick, sc_if);
	callout_schedule(&sc_if->sk_tick_ch, hz);

	callout_init(&sc_if->sk_tick_rx, 0);
	callout_setfunc(&sc_if->sk_tick_rx, msk_fill_rx_tick, sc_if);

	/*
	 * Call MI attach routines.
	 */
	if_attach(ifp);
	if_deferred_start_init(ifp, NULL);
	ether_ifattach(ifp, sc_if->sk_enaddr);

	if (pmf_device_register(self, NULL, msk_resume))
		pmf_class_network_register(self, ifp);
	else
		aprint_error_dev(self, "couldn't establish power handler\n");

	if (sc->rnd_attached++ == 0) {
		rnd_attach_source(&sc->rnd_source, device_xname(sc->sk_dev),
			RND_TYPE_NET, RND_FLAG_DEFAULT);
	}

	DPRINTFN(2, ("msk_attach: end\n"));
	return;

fail_3:
	bus_dmamap_destroy(sc->sc_dmatag, sc_if->sk_ring_map);
fail_2:
	bus_dmamem_unmap(sc->sc_dmatag, kva, sizeof(struct msk_ring_data));
fail_1:
	bus_dmamem_free(sc->sc_dmatag, &sc_if->sk_ring_seg, sc_if->sk_ring_nseg);
fail:
	sc->sk_if[sa->skc_port] = NULL;
}

int
msk_detach(device_t self, int flags)
{
	struct sk_if_softc *sc_if = device_private(self);
	struct sk_softc *sc = sc_if->sk_softc;
	struct ifnet *ifp = &sc_if->sk_ethercom.ec_if;

	if (sc->sk_if[sc_if->sk_port] == NULL)
		return 0;

	msk_stop(ifp, 1);

	if (--sc->rnd_attached == 0)
		rnd_detach_source(&sc->rnd_source);

	callout_halt(&sc_if->sk_tick_ch, NULL);
	callout_destroy(&sc_if->sk_tick_ch);

	callout_halt(&sc_if->sk_tick_rx, NULL);
	callout_destroy(&sc_if->sk_tick_rx);

	/* Detach any PHYs we might have. */
	if (LIST_FIRST(&sc_if->sk_mii.mii_phys) != NULL)
		mii_detach(&sc_if->sk_mii, MII_PHY_ANY, MII_OFFSET_ANY);

	/* Delete any remaining media. */
	ifmedia_delete_instance(&sc_if->sk_mii.mii_media, IFM_INST_ANY);

	pmf_device_deregister(self);

	ether_ifdetach(ifp);
	if_detach(ifp);

	msk_free_jumbo_mem(sc_if);

	bus_dmamem_unmap(sc->sc_dmatag, sc_if->sk_rdata,
	    sizeof(struct msk_ring_data));
	bus_dmamem_free(sc->sc_dmatag,
	    &sc_if->sk_ring_seg, sc_if->sk_ring_nseg);
	bus_dmamap_destroy(sc->sc_dmatag, sc_if->sk_ring_map);
	sc->sk_if[sc_if->sk_port] = NULL;

	return 0;
}

int
mskcprint(void *aux, const char *pnp)
{
	struct skc_attach_args *sa = aux;

	if (pnp)
		aprint_normal("msk port %c at %s",
		    (sa->skc_port == SK_PORT_A) ? 'A' : 'B', pnp);
	else
		aprint_normal(" port %c",
		    (sa->skc_port == SK_PORT_A) ? 'A' : 'B');
	return UNCONF;
}

/*
 * Attach the interface. Allocate softc structures, do ifmedia
 * setup and ethernet/BPF attach.
 */
void
mskc_attach(device_t parent, device_t self, void *aux)
{
	struct sk_softc *sc = device_private(self);
	struct pci_attach_args *pa = aux;
	struct skc_attach_args skca;
	pci_chipset_tag_t pc = pa->pa_pc;
	pcireg_t command, memtype;
	const char *intrstr = NULL;
	int rc, sk_nodenum;
	uint8_t hw, pmd;
	const char *revstr = NULL;
	const struct sysctlnode *node;
	void *kva;
	char intrbuf[PCI_INTRSTR_LEN];

	DPRINTFN(2, ("begin mskc_attach\n"));

	sc->sk_dev = self;
	/*
	 * Handle power management nonsense.
	 */
	command = pci_conf_read(pc, pa->pa_tag, SK_PCI_CAPID) & 0x000000FF;

	if (command == 0x01) {
		command = pci_conf_read(pc, pa->pa_tag, SK_PCI_PWRMGMTCTRL);
		if (command & SK_PSTATE_MASK) {
			uint32_t		iobase, membase, irq;

			/* Save important PCI config data. */
			iobase = pci_conf_read(pc, pa->pa_tag, SK_PCI_LOIO);
			membase = pci_conf_read(pc, pa->pa_tag, SK_PCI_LOMEM);
			irq = pci_conf_read(pc, pa->pa_tag, SK_PCI_INTLINE);

			/* Reset the power state. */
			aprint_normal_dev(sc->sk_dev, "chip is in D%d power "
			    "mode -- setting to D0\n",
			    command & SK_PSTATE_MASK);
			command &= 0xFFFFFFFC;
			pci_conf_write(pc, pa->pa_tag,
			    SK_PCI_PWRMGMTCTRL, command);

			/* Restore PCI config data. */
			pci_conf_write(pc, pa->pa_tag, SK_PCI_LOIO, iobase);
			pci_conf_write(pc, pa->pa_tag, SK_PCI_LOMEM, membase);
			pci_conf_write(pc, pa->pa_tag, SK_PCI_INTLINE, irq);
		}
	}

	/*
	 * Map control/status registers.
	 */
	memtype = pci_mapreg_type(pc, pa->pa_tag, SK_PCI_LOMEM);
	if (pci_mapreg_map(pa, SK_PCI_LOMEM, memtype, 0, &sc->sk_btag,
	    &sc->sk_bhandle, NULL, &sc->sk_bsize)) {
		aprint_error(": can't map mem space\n");
		return;
	}

	if (pci_dma64_available(pa))
		sc->sc_dmatag = pa->pa_dmat64;
	else
		sc->sc_dmatag = pa->pa_dmat;

	command = pci_conf_read(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG);
	command |= PCI_COMMAND_MASTER_ENABLE;
	pci_conf_write(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG, command);

	sc->sk_type = sk_win_read_1(sc, SK_CHIPVER);
	sc->sk_rev = (sk_win_read_1(sc, SK_CONFIG) >> 4);

	/* bail out here if chip is not recognized */
	if (!(SK_IS_YUKON2(sc))) {
		aprint_error(": unknown chip type: %d\n", sc->sk_type);
		goto fail_1;
	}
	DPRINTFN(2, ("mskc_attach: allocate interrupt\n"));

	/* Allocate interrupt */
	if (pci_intr_alloc(pa, &sc->sk_pihp, NULL, 0)) {
		aprint_error(": couldn't map interrupt\n");
		goto fail_1;
	}

	intrstr = pci_intr_string(pc, sc->sk_pihp[0], intrbuf, sizeof(intrbuf));
	sc->sk_intrhand = pci_intr_establish_xname(pc, sc->sk_pihp[0], IPL_NET,
	    msk_intr, sc, device_xname(sc->sk_dev));
	if (sc->sk_intrhand == NULL) {
		aprint_error(": couldn't establish interrupt");
		if (intrstr != NULL)
			aprint_error(" at %s", intrstr);
		aprint_error("\n");
		goto fail_1;
	}
	sc->sk_pc = pc;

	if (bus_dmamem_alloc(sc->sc_dmatag,
	    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc),
	    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc),
	    0, &sc->sk_status_seg, 1, &sc->sk_status_nseg, BUS_DMA_NOWAIT)) {
		aprint_error(": can't alloc status buffers\n");
		goto fail_2;
	}

	if (bus_dmamem_map(sc->sc_dmatag,
	    &sc->sk_status_seg, sc->sk_status_nseg,
	    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc),
	    &kva, BUS_DMA_NOWAIT)) {
		aprint_error(": can't map dma buffers (%zu bytes)\n",
		    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc));
		goto fail_3;
	}
	if (bus_dmamap_create(sc->sc_dmatag,
	    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc), 1,
	    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc), 0,
	    BUS_DMA_NOWAIT, &sc->sk_status_map)) {
		aprint_error(": can't create dma map\n");
		goto fail_4;
	}
	if (bus_dmamap_load(sc->sc_dmatag, sc->sk_status_map, kva,
	    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc),
	    NULL, BUS_DMA_NOWAIT)) {
		aprint_error(": can't load dma map\n");
		goto fail_5;
	}
	sc->sk_status_ring = (struct msk_status_desc *)kva;

	sc->sk_int_mod = SK_IM_DEFAULT;
	sc->sk_int_mod_pending = 0;

	/* Reset the adapter. */
	mskc_reset(sc);

	sc->sk_ramsize = sk_win_read_1(sc, SK_EPROM0) * 4096;
	DPRINTFN(2, ("mskc_attach: ramsize=%dK\n", sc->sk_ramsize / 1024));

	pmd = sk_win_read_1(sc, SK_PMDTYPE);
	if (pmd == 'L' || pmd == 'S' || pmd == 'P')
		sc->sk_fibertype = 1;

	switch (sc->sk_type) {
	case SK_YUKON_XL:
		sc->sk_name = "Yukon-2 XL";
		break;
	case SK_YUKON_EC_U:
		sc->sk_name = "Yukon-2 EC Ultra";
		break;
	case SK_YUKON_EX:
		sc->sk_name = "Yukon-2 Extreme";
		break;
	case SK_YUKON_EC:
		sc->sk_name = "Yukon-2 EC";
		break;
	case SK_YUKON_FE:
		sc->sk_name = "Yukon-2 FE";
		break;
	case SK_YUKON_FE_P:
		sc->sk_name = "Yukon-2 FE+";
		break;
	case SK_YUKON_SUPR:
		sc->sk_name = "Yukon-2 Supreme";
		break;
	case SK_YUKON_ULTRA2:
		sc->sk_name = "Yukon-2 Ultra 2";
		break;
	case SK_YUKON_OPTIMA:
		sc->sk_name = "Yukon-2 Optima";
		break;
	case SK_YUKON_PRM:
		sc->sk_name = "Yukon-2 Optima Prime";
		break;
	case SK_YUKON_OPTIMA2:
		sc->sk_name = "Yukon-2 Optima 2";
		break;
	default:
		sc->sk_name = "Yukon (Unknown)";
	}

	if (sc->sk_type == SK_YUKON_XL) {
		switch (sc->sk_rev) {
		case SK_YUKON_XL_REV_A0:
			revstr = "A0";
			break;
		case SK_YUKON_XL_REV_A1:
			revstr = "A1";
			break;
		case SK_YUKON_XL_REV_A2:
			revstr = "A2";
			break;
		case SK_YUKON_XL_REV_A3:
			revstr = "A3";
			break;
		default:
			break;
		}
	}

	if (sc->sk_type == SK_YUKON_EC) {
		switch (sc->sk_rev) {
		case SK_YUKON_EC_REV_A1:
			revstr = "A1";
			break;
		case SK_YUKON_EC_REV_A2:
			revstr = "A2";
			break;
		case SK_YUKON_EC_REV_A3:
			revstr = "A3";
			break;
		default:
			break;
		}
	}

	if (sc->sk_type == SK_YUKON_FE) {
		switch (sc->sk_rev) {
		case SK_YUKON_FE_REV_A1:
			revstr = "A1";
			break;
		case SK_YUKON_FE_REV_A2:
			revstr = "A2";
			break;
		default:
			break;
		}
	}

	if (sc->sk_type == SK_YUKON_EC_U) {
		switch (sc->sk_rev) {
		case SK_YUKON_EC_U_REV_A0:
			revstr = "A0";
			break;
		case SK_YUKON_EC_U_REV_A1:
			revstr = "A1";
			break;
		case SK_YUKON_EC_U_REV_B0:
			revstr = "B0";
			break;
		case SK_YUKON_EC_U_REV_B1:
			revstr = "B1";
			break;
		default:
			break;
		}
	}

	if (sc->sk_type == SK_YUKON_FE) {
		switch (sc->sk_rev) {
		case SK_YUKON_FE_REV_A1:
			revstr = "A1";
			break;
		case SK_YUKON_FE_REV_A2:
			revstr = "A2";
			break;
		default:
			;
		}
	}

	if (sc->sk_type == SK_YUKON_FE_P && sc->sk_rev == SK_YUKON_FE_P_REV_A0)
		revstr = "A0";

	if (sc->sk_type == SK_YUKON_EX) {
		switch (sc->sk_rev) {
		case SK_YUKON_EX_REV_A0:
			revstr = "A0";
			break;
		case SK_YUKON_EX_REV_B0:
			revstr = "B0";
			break;
		default:
			;
		}
	}

	if (sc->sk_type == SK_YUKON_SUPR) {
		switch (sc->sk_rev) {
		case SK_YUKON_SUPR_REV_A0:
			revstr = "A0";
			break;
		case SK_YUKON_SUPR_REV_B0:
			revstr = "B0";
			break;
		case SK_YUKON_SUPR_REV_B1:
			revstr = "B1";
			break;
		default:
			;
		}
	}

	if (sc->sk_type == SK_YUKON_PRM) {
		switch (sc->sk_rev) {
		case SK_YUKON_PRM_REV_Z1:
			revstr = "Z1";
			break;
		case SK_YUKON_PRM_REV_A0:
			revstr = "A0";
			break;
		default:
			;
		}
	}

	/* Announce the product name. */
	aprint_normal(", %s", sc->sk_name);
	if (revstr != NULL)
		aprint_normal(" rev. %s", revstr);
	aprint_normal(" (0x%x): %s\n", sc->sk_rev, intrstr);

	sc->sk_macs = 1;

	hw = sk_win_read_1(sc, SK_Y2_HWRES);
	if ((hw & SK_Y2_HWRES_LINK_MASK) == SK_Y2_HWRES_LINK_DUAL) {
		if ((sk_win_read_1(sc, SK_Y2_CLKGATE) &
		    SK_Y2_CLKGATE_LINK2_INACTIVE) == 0)
			sc->sk_macs++;
	}

	skca.skc_port = SK_PORT_A;
	skca.skc_type = sc->sk_type;
	skca.skc_rev = sc->sk_rev;
	(void)config_found(sc->sk_dev, &skca, mskcprint);

	if (sc->sk_macs > 1) {
		skca.skc_port = SK_PORT_B;
		skca.skc_type = sc->sk_type;
		skca.skc_rev = sc->sk_rev;
		(void)config_found(sc->sk_dev, &skca, mskcprint);
	}

	/* Turn on the 'driver is loaded' LED. */
	CSR_WRITE_2(sc, SK_LED, SK_LED_GREEN_ON);

	/* skc sysctl setup */

	if ((rc = sysctl_createv(&sc->sk_clog, 0, NULL, &node,
	    0, CTLTYPE_NODE, device_xname(sc->sk_dev),
	    SYSCTL_DESCR("mskc per-controller controls"),
	    NULL, 0, NULL, 0, CTL_HW, msk_root_num, CTL_CREATE,
	    CTL_EOL)) != 0) {
		aprint_normal_dev(sc->sk_dev, "couldn't create sysctl node\n");
		goto fail_6;
	}

	sk_nodenum = node->sysctl_num;

	/* interrupt moderation time in usecs */
	if ((rc = sysctl_createv(&sc->sk_clog, 0, NULL, &node,
	    CTLFLAG_READWRITE,
	    CTLTYPE_INT, "int_mod",
	    SYSCTL_DESCR("msk interrupt moderation timer"),
	    msk_sysctl_handler, 0, (void *)sc,
	    0, CTL_HW, msk_root_num, sk_nodenum, CTL_CREATE,
	    CTL_EOL)) != 0) {
		aprint_normal_dev(sc->sk_dev,
		    "couldn't create int_mod sysctl node\n");
		goto fail_6;
	}

	if (!pmf_device_register(self, mskc_suspend, mskc_resume))
		aprint_error_dev(self, "couldn't establish power handler\n");

	return;

fail_6:
	bus_dmamap_unload(sc->sc_dmatag, sc->sk_status_map);
fail_4:
	bus_dmamem_unmap(sc->sc_dmatag, kva,
	    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc));
fail_3:
	bus_dmamem_free(sc->sc_dmatag,
	    &sc->sk_status_seg, sc->sk_status_nseg);
	sc->sk_status_nseg = 0;
fail_5:
	bus_dmamap_destroy(sc->sc_dmatag, sc->sk_status_map);
fail_2:
	pci_intr_disestablish(pc, sc->sk_intrhand);
	sc->sk_intrhand = NULL;
fail_1:
	bus_space_unmap(sc->sk_btag, sc->sk_bhandle, sc->sk_bsize);
	sc->sk_bsize = 0;
}

int
mskc_detach(device_t self, int flags)
{
	struct sk_softc *sc = device_private(self);
	int rv;

	if (sc->sk_intrhand) {
		pci_intr_disestablish(sc->sk_pc, sc->sk_intrhand);
		sc->sk_intrhand = NULL;
	}

	if (sc->sk_pihp != NULL) {
		pci_intr_release(sc->sk_pc, sc->sk_pihp, 1);
		sc->sk_pihp = NULL;
	}

	rv = config_detach_children(self, flags);
	if (rv != 0)
		return rv;

	if (sc->sk_status_nseg > 0) {
		bus_dmamap_destroy(sc->sc_dmatag, sc->sk_status_map);
		bus_dmamem_unmap(sc->sc_dmatag, sc->sk_status_ring,
		    MSK_STATUS_RING_CNT * sizeof(struct msk_status_desc));
		bus_dmamem_free(sc->sc_dmatag,
		    &sc->sk_status_seg, sc->sk_status_nseg);
	}

	if (sc->sk_bsize > 0)
		bus_space_unmap(sc->sk_btag, sc->sk_bhandle, sc->sk_bsize);

	return 0;
}

int
msk_encap(struct sk_if_softc *sc_if, struct mbuf *m_head, uint32_t *txidx)
{
	struct sk_softc		*sc = sc_if->sk_softc;
	struct msk_tx_desc		*f = NULL;
	uint32_t		frag, cur, hiaddr, old_hiaddr, total;
	uint32_t		entries = 0;
	size_t			i;
	struct sk_txmap_entry	*entry;
	bus_dmamap_t		txmap;
	bus_addr_t		addr;

	DPRINTFN(2, ("msk_encap\n"));

	entry = SIMPLEQ_FIRST(&sc_if->sk_txmap_head);
	if (entry == NULL) {
		DPRINTFN(2, ("msk_encap: no txmap available\n"));
		return ENOBUFS;
	}
	txmap = entry->dmamap;

	cur = frag = *txidx;

#ifdef MSK_DEBUG
	if (mskdebug >= 2)
		msk_dump_mbuf(m_head);
#endif

	/*
	 * Start packing the mbufs in this chain into
	 * the fragment pointers. Stop when we run out
	 * of fragments or hit the end of the mbuf chain.
	 */
	if (bus_dmamap_load_mbuf(sc->sc_dmatag, txmap, m_head,
	    BUS_DMA_NOWAIT)) {
		DPRINTFN(2, ("msk_encap: dmamap failed\n"));
		return ENOBUFS;
	}

	/* Count how many tx descriptors needed. */
	hiaddr = sc_if->sk_cdata.sk_tx_hiaddr;
	for (total = i = 0; i < txmap->dm_nsegs; i++) {
		if (hiaddr != MSK_ADDR_HI(txmap->dm_segs[i].ds_addr)) {
			hiaddr = MSK_ADDR_HI(txmap->dm_segs[i].ds_addr);
			total++;
		}
		total++;
	}

	if (total > MSK_TX_RING_CNT - sc_if->sk_cdata.sk_tx_cnt - 2) {
		DPRINTFN(2, ("msk_encap: too few descriptors free\n"));
		bus_dmamap_unload(sc->sc_dmatag, txmap);
		return ENOBUFS;
	}

	DPRINTFN(2, ("msk_encap: dm_nsegs=%d total desc=%u\n",
	    txmap->dm_nsegs, total));

	/* Sync the DMA map. */
	bus_dmamap_sync(sc->sc_dmatag, txmap, 0, txmap->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	old_hiaddr = sc_if->sk_cdata.sk_tx_hiaddr;
	for (i = 0; i < txmap->dm_nsegs; i++) {
		addr = txmap->dm_segs[i].ds_addr;
		DPRINTFN(2, ("msk_encap: addr %llx\n",
		    (unsigned long long)addr));
		hiaddr = MSK_ADDR_HI(addr);

		if (sc_if->sk_cdata.sk_tx_hiaddr != hiaddr) {
			f = &sc_if->sk_rdata->sk_tx_ring[frag];
			f->sk_addr = htole32(hiaddr);
			f->sk_len = 0;
			f->sk_ctl = 0;
			if (i == 0)
				f->sk_opcode = SK_Y2_BMUOPC_ADDR64;
			else
				f->sk_opcode = SK_Y2_BMUOPC_ADDR64 | SK_Y2_TXOPC_OWN;
			sc_if->sk_cdata.sk_tx_hiaddr = hiaddr;
			SK_INC(frag, MSK_TX_RING_CNT);
			entries++;
			DPRINTFN(10, ("%s: tx ADDR64: %#x\n",
			    sc_if->sk_ethercom.ec_if.if_xname, hiaddr));
		}

		f = &sc_if->sk_rdata->sk_tx_ring[frag];
		f->sk_addr = htole32(MSK_ADDR_LO(addr));
		f->sk_len = htole16(txmap->dm_segs[i].ds_len);
		f->sk_ctl = 0;
		if (i == 0) {
			if (hiaddr != old_hiaddr)
				f->sk_opcode = SK_Y2_TXOPC_PACKET | SK_Y2_TXOPC_OWN;
			else
				f->sk_opcode = SK_Y2_TXOPC_PACKET;
		} else
			f->sk_opcode = SK_Y2_TXOPC_BUFFER | SK_Y2_TXOPC_OWN;
		cur = frag;
		SK_INC(frag, MSK_TX_RING_CNT);
		entries++;
	}
	KASSERTMSG(entries == total, "entries %u total %u", entries, total);

	sc_if->sk_cdata.sk_tx_chain[cur].sk_mbuf = m_head;
	SIMPLEQ_REMOVE_HEAD(&sc_if->sk_txmap_head, link);

	sc_if->sk_cdata.sk_tx_map[cur] = entry;
	sc_if->sk_rdata->sk_tx_ring[cur].sk_ctl |= SK_Y2_TXCTL_LASTFRAG;

	/* Sync descriptors before handing to chip */
	MSK_CDTXSYNC(sc_if, *txidx, entries,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	sc_if->sk_rdata->sk_tx_ring[*txidx].sk_opcode |= SK_Y2_TXOPC_OWN;

	/* Sync first descriptor to hand it off */
	MSK_CDTXSYNC(sc_if, *txidx, 1,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	sc_if->sk_cdata.sk_tx_cnt += entries;

#ifdef MSK_DEBUG
	if (mskdebug >= 2) {
		struct msk_tx_desc *le;
		uint32_t idx;
		for (idx = *txidx; idx != frag; SK_INC(idx, MSK_TX_RING_CNT)) {
			le = &sc_if->sk_rdata->sk_tx_ring[idx];
			msk_dump_txdesc(le, idx);
		}
	}
#endif

	*txidx = frag;

	DPRINTFN(2, ("msk_encap: successful: %u entries\n", entries));

	return 0;
}

void
msk_start(struct ifnet *ifp)
{
	struct sk_if_softc	*sc_if = ifp->if_softc;
	struct mbuf		*m_head = NULL;
	uint32_t		idx = sc_if->sk_cdata.sk_tx_prod;
	int			pkts = 0;

	DPRINTFN(2, ("msk_start\n"));

	while (sc_if->sk_cdata.sk_tx_chain[idx].sk_mbuf == NULL) {
		IFQ_POLL(&ifp->if_snd, m_head);
		if (m_head == NULL)
			break;

		/*
		 * Pack the data into the transmit ring. If we
		 * don't have room, set the OACTIVE flag and wait
		 * for the NIC to drain the ring.
		 */
		if (msk_encap(sc_if, m_head, &idx)) {
			ifp->if_flags |= IFF_OACTIVE;
			break;
		}

		/* now we are committed to transmit the packet */
		IFQ_DEQUEUE(&ifp->if_snd, m_head);
		pkts++;

		/*
		 * If there's a BPF listener, bounce a copy of this frame
		 * to him.
		 */
		bpf_mtap(ifp, m_head, BPF_D_OUT);
	}
	if (pkts == 0)
		return;

	/* Transmit */
	if (idx != sc_if->sk_cdata.sk_tx_prod) {
		sc_if->sk_cdata.sk_tx_prod = idx;
		SK_IF_WRITE_2(sc_if, 1, SK_TXQA1_Y2_PREF_PUTIDX, idx);

		/* Set a timeout in case the chip goes out to lunch. */
		ifp->if_timer = 5;
	}
}

void
msk_watchdog(struct ifnet *ifp)
{
	struct sk_if_softc *sc_if = ifp->if_softc;

	/*
	 * Reclaim first as there is a possibility of losing Tx completion
	 * interrupts.
	 */
	msk_txeof(sc_if);
	if (sc_if->sk_cdata.sk_tx_cnt != 0) {
		aprint_error_dev(sc_if->sk_dev, "watchdog timeout\n");

		ifp->if_oerrors++;

		/* XXX Resets both ports; we shouldn't do that. */
		mskc_reset(sc_if->sk_softc);
		msk_reset(sc_if);
		msk_init(ifp);
	}
}

static bool
mskc_suspend(device_t dv, const pmf_qual_t *qual)
{
	struct sk_softc *sc = device_private(dv);

	DPRINTFN(2, ("mskc_suspend\n"));

	/* Turn off the 'driver is loaded' LED. */
	CSR_WRITE_2(sc, SK_LED, SK_LED_GREEN_OFF);

	return true;
}

static bool
mskc_resume(device_t dv, const pmf_qual_t *qual)
{
	struct sk_softc *sc = device_private(dv);

	DPRINTFN(2, ("mskc_resume\n"));

	mskc_reset(sc);
	CSR_WRITE_2(sc, SK_LED, SK_LED_GREEN_ON);

	return true;
}

static __inline int
msk_rxvalid(struct sk_softc *sc, uint32_t stat, uint32_t len)
{
	if ((stat & (YU_RXSTAT_CRCERR | YU_RXSTAT_LONGERR |
	    YU_RXSTAT_MIIERR | YU_RXSTAT_BADFC | YU_RXSTAT_GOODFC |
	    YU_RXSTAT_JABBER)) != 0 ||
	    (stat & YU_RXSTAT_RXOK) != YU_RXSTAT_RXOK ||
	    YU_RXSTAT_BYTES(stat) != len)
		return 0;

	return 1;
}

void
msk_rxeof(struct sk_if_softc *sc_if, uint16_t len, uint32_t rxstat)
{
	struct sk_softc		*sc = sc_if->sk_softc;
	struct ifnet		*ifp = &sc_if->sk_ethercom.ec_if;
	struct mbuf		*m;
	unsigned		cur, prod, tail, total_len = len;
	bus_dmamap_t		dmamap;

	cur = sc_if->sk_cdata.sk_rx_cons;
	prod = sc_if->sk_cdata.sk_rx_prod;

	/* Sync the descriptor */
	MSK_CDRXSYNC(sc_if, cur, BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	DPRINTFN(2, ("msk_rxeof: cur %u prod %u rx_cnt %u\n", cur, prod,
		sc_if->sk_cdata.sk_rx_cnt));

	while (prod != cur) {
		tail = cur;
		SK_INC(cur, MSK_RX_RING_CNT);

		sc_if->sk_cdata.sk_rx_cnt--;
		m = sc_if->sk_cdata.sk_rx_chain[tail].sk_mbuf;
		sc_if->sk_cdata.sk_rx_chain[tail].sk_mbuf = NULL;
		if (m != NULL)
			break;	/* found it */
	}
	sc_if->sk_cdata.sk_rx_cons = cur;
	DPRINTFN(2, ("msk_rxeof: cur %u rx_cnt %u m %p\n", cur,
		sc_if->sk_cdata.sk_rx_cnt, m));

	if (m == NULL)
		return;

	dmamap = sc_if->sk_cdata.sk_rx_jumbo_map;

	bus_dmamap_sync(sc_if->sk_softc->sc_dmatag, dmamap, 0,
	    dmamap->dm_mapsize, BUS_DMASYNC_POSTREAD);

	if (total_len < SK_MIN_FRAMELEN ||
	    total_len > ETHER_MAX_LEN_JUMBO ||
	    msk_rxvalid(sc, rxstat, total_len) == 0) {
		ifp->if_ierrors++;
		m_freem(m);
		return;
	}

	m_set_rcvif(m, ifp);
	m->m_pkthdr.len = m->m_len = total_len;

	/* pass it on. */
	if_percpuq_enqueue(ifp->if_percpuq, m);
}

void
msk_txeof(struct sk_if_softc *sc_if)
{
	struct sk_softc		*sc = sc_if->sk_softc;
	struct msk_tx_desc	*cur_tx;
	struct ifnet		*ifp = &sc_if->sk_ethercom.ec_if;
	uint32_t		idx, reg, sk_ctl;
	struct sk_txmap_entry	*entry;

	DPRINTFN(2, ("msk_txeof\n"));

	if (sc_if->sk_port == SK_PORT_A)
		reg = SK_STAT_BMU_TXA1_RIDX;
	else
		reg = SK_STAT_BMU_TXA2_RIDX;

	/*
	 * Go through our tx ring and free mbufs for those
	 * frames that have been sent.
	 */
	idx = sc_if->sk_cdata.sk_tx_cons;
	while (idx != sk_win_read_2(sc, reg)) {
		MSK_CDTXSYNC(sc_if, idx, 1,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

		cur_tx = &sc_if->sk_rdata->sk_tx_ring[idx];
		sk_ctl = cur_tx->sk_ctl;
#ifdef MSK_DEBUG
		if (mskdebug >= 2)
			msk_dump_txdesc(cur_tx, idx);
#endif
		if (sk_ctl & SK_Y2_TXCTL_LASTFRAG)
			ifp->if_opackets++;
		if (sc_if->sk_cdata.sk_tx_chain[idx].sk_mbuf != NULL) {
			entry = sc_if->sk_cdata.sk_tx_map[idx];

			bus_dmamap_sync(sc->sc_dmatag, entry->dmamap, 0,
			    entry->dmamap->dm_mapsize, BUS_DMASYNC_POSTWRITE);

			bus_dmamap_unload(sc->sc_dmatag, entry->dmamap);
			SIMPLEQ_INSERT_TAIL(&sc_if->sk_txmap_head, entry,
					  link);
			sc_if->sk_cdata.sk_tx_map[idx] = NULL;
			m_freem(sc_if->sk_cdata.sk_tx_chain[idx].sk_mbuf);
			sc_if->sk_cdata.sk_tx_chain[idx].sk_mbuf = NULL;
		}
		sc_if->sk_cdata.sk_tx_cnt--;
		SK_INC(idx, MSK_TX_RING_CNT);
	}
	if (idx == sc_if->sk_cdata.sk_tx_cons)
		return;

	ifp->if_timer = sc_if->sk_cdata.sk_tx_cnt > 0 ? 5 : 0;

	if (sc_if->sk_cdata.sk_tx_cnt < MSK_TX_RING_CNT - 2)
		ifp->if_flags &= ~IFF_OACTIVE;

	sc_if->sk_cdata.sk_tx_cons = idx;
}

void
msk_fill_rx_ring(struct sk_if_softc *sc_if)
{
	/* Make sure to not completely wrap around */
	while (sc_if->sk_cdata.sk_rx_cnt < (MSK_RX_RING_CNT - 1)) {
		if (msk_newbuf(sc_if,
		    sc_if->sk_cdata.sk_rx_jumbo_map) == ENOBUFS) {
			goto schedretry;
		}
	}

	return;

schedretry:
	/* Try later */
	callout_schedule(&sc_if->sk_tick_rx, hz/2);
}

static void
msk_fill_rx_tick(void *xsc_if)
{
	struct sk_if_softc *sc_if = xsc_if;
	int s, rx_prod;

	KASSERT(KERNEL_LOCKED_P());	/* XXXSMP */

	s = splnet();
	rx_prod = sc_if->sk_cdata.sk_rx_prod;
	msk_fill_rx_ring(sc_if);
	if (rx_prod != sc_if->sk_cdata.sk_rx_prod) {
		SK_IF_WRITE_2(sc_if, 0, SK_RXQ1_Y2_PREF_PUTIDX,
		    sc_if->sk_cdata.sk_rx_prod);
	}
	splx(s);
}

void
msk_tick(void *xsc_if)
{
	struct sk_if_softc *sc_if = xsc_if;
	struct mii_data *mii = &sc_if->sk_mii;
	int s;

	s = splnet();
	mii_tick(mii);
	splx(s);

	callout_schedule(&sc_if->sk_tick_ch, hz);
}

void
msk_intr_yukon(struct sk_if_softc *sc_if)
{
	uint8_t status;

	status = SK_IF_READ_1(sc_if, 0, SK_GMAC_ISR);
	/* RX overrun */
	if ((status & SK_GMAC_INT_RX_OVER) != 0) {
		SK_IF_WRITE_1(sc_if, 0, SK_RXMF1_CTRL_TEST,
		    SK_RFCTL_RX_FIFO_OVER);
	}
	/* TX underrun */
	if ((status & SK_GMAC_INT_TX_UNDER) != 0) {
		SK_IF_WRITE_1(sc_if, 0, SK_TXMF1_CTRL_TEST,
		    SK_TFCTL_TX_FIFO_UNDER);
	}

	DPRINTFN(2, ("msk_intr_yukon status=%#x\n", status));
}

int
msk_intr(void *xsc)
{
	struct sk_softc		*sc = xsc;
	struct sk_if_softc	*sc_if;
	struct sk_if_softc	*sc_if0 = sc->sk_if[SK_PORT_A];
	struct sk_if_softc	*sc_if1 = sc->sk_if[SK_PORT_B];
	struct ifnet		*ifp0 = NULL, *ifp1 = NULL;
	int			claimed = 0;
	uint32_t		status;
	struct msk_status_desc	*cur_st;

	status = CSR_READ_4(sc, SK_Y2_ISSR2);
	if (status == 0xffffffff)
		return 0;
	if (status == 0) {
		CSR_WRITE_4(sc, SK_Y2_ICR, 2);
		return 0;
	}

	status = CSR_READ_4(sc, SK_ISR);

	if (sc_if0 != NULL)
		ifp0 = &sc_if0->sk_ethercom.ec_if;
	if (sc_if1 != NULL)
		ifp1 = &sc_if1->sk_ethercom.ec_if;

	if (sc_if0 && (status & SK_Y2_IMR_MAC1) &&
	    (ifp0->if_flags & IFF_RUNNING)) {
		msk_intr_yukon(sc_if0);
	}

	if (sc_if1 && (status & SK_Y2_IMR_MAC2) &&
	    (ifp1->if_flags & IFF_RUNNING)) {
		msk_intr_yukon(sc_if1);
	}

	MSK_CDSTSYNC(sc, sc->sk_status_idx,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	cur_st = &sc->sk_status_ring[sc->sk_status_idx];

	while (cur_st->sk_opcode & SK_Y2_STOPC_OWN) {
		cur_st->sk_opcode &= ~SK_Y2_STOPC_OWN;
		switch (cur_st->sk_opcode) {
		case SK_Y2_STOPC_RXSTAT:
			sc_if = sc->sk_if[cur_st->sk_link & 0x01];
			if (sc_if) {
				msk_rxeof(sc_if, letoh16(cur_st->sk_len),
				    letoh32(cur_st->sk_status));
				if (sc_if->sk_cdata.sk_rx_cnt < (MSK_RX_RING_CNT/3))
					msk_fill_rx_tick(sc_if);
			}
			break;
		case SK_Y2_STOPC_TXSTAT:
			if (sc_if0)
				msk_txeof(sc_if0);
			if (sc_if1)
				msk_txeof(sc_if1);
			break;
		default:
			aprint_error("opcode=0x%x\n", cur_st->sk_opcode);
			break;
		}
		SK_INC(sc->sk_status_idx, MSK_STATUS_RING_CNT);

		MSK_CDSTSYNC(sc, sc->sk_status_idx,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		cur_st = &sc->sk_status_ring[sc->sk_status_idx];
	}

	if (status & SK_Y2_IMR_BMU) {
		CSR_WRITE_4(sc, SK_STAT_BMU_CSR, SK_STAT_BMU_IRQ_CLEAR);
		claimed = 1;
	}

	CSR_WRITE_4(sc, SK_Y2_ICR, 2);

	if (ifp0 != NULL && !IFQ_IS_EMPTY(&ifp0->if_snd))
		if_schedule_deferred_start(ifp0);
	if (ifp1 != NULL && !IFQ_IS_EMPTY(&ifp1->if_snd))
		if_schedule_deferred_start(ifp1);

	KASSERT(sc->rnd_attached > 0);
	rnd_add_uint32(&sc->rnd_source, status);

	if (sc->sk_int_mod_pending)
		msk_update_int_mod(sc, 1);

	return claimed;
}

void
msk_init_yukon(struct sk_if_softc *sc_if)
{
	uint32_t		v;
	uint16_t		reg;
	struct sk_softc		*sc;
	int			i;

	sc = sc_if->sk_softc;

	DPRINTFN(2, ("msk_init_yukon: start: sk_csr=%#x\n",
		     CSR_READ_4(sc_if->sk_softc, SK_CSR)));

	DPRINTFN(6, ("msk_init_yukon: 1\n"));

	DPRINTFN(3, ("msk_init_yukon: gmac_ctrl=%#x\n",
		     SK_IF_READ_4(sc_if, 0, SK_GMAC_CTRL)));

	DPRINTFN(6, ("msk_init_yukon: 3\n"));

	/* unused read of the interrupt source register */
	DPRINTFN(6, ("msk_init_yukon: 4\n"));
	SK_IF_READ_2(sc_if, 0, SK_GMAC_ISR);

	DPRINTFN(6, ("msk_init_yukon: 4a\n"));
	reg = SK_YU_READ_2(sc_if, YUKON_PAR);
	DPRINTFN(6, ("msk_init_yukon: YUKON_PAR=%#x\n", reg));

	/* MIB Counter Clear Mode set */
	reg |= YU_PAR_MIB_CLR;
	DPRINTFN(6, ("msk_init_yukon: YUKON_PAR=%#x\n", reg));
	DPRINTFN(6, ("msk_init_yukon: 4b\n"));
	SK_YU_WRITE_2(sc_if, YUKON_PAR, reg);

	/* MIB Counter Clear Mode clear */
	DPRINTFN(6, ("msk_init_yukon: 5\n"));
	reg &= ~YU_PAR_MIB_CLR;
	SK_YU_WRITE_2(sc_if, YUKON_PAR, reg);

	/* receive control reg */
	DPRINTFN(6, ("msk_init_yukon: 7\n"));
	SK_YU_WRITE_2(sc_if, YUKON_RCR, YU_RCR_CRCR);

	/* transmit control register */
	SK_YU_WRITE_2(sc_if, YUKON_TCR, (0x04 << 10));

	/* transmit flow control register */
	SK_YU_WRITE_2(sc_if, YUKON_TFCR, 0xffff);

	/* transmit parameter register */
	DPRINTFN(6, ("msk_init_yukon: 8\n"));
	SK_YU_WRITE_2(sc_if, YUKON_TPR, YU_TPR_JAM_LEN(0x3) |
		      YU_TPR_JAM_IPG(0xb) | YU_TPR_JAM2DATA_IPG(0x1c) | 0x04);

	/* serial mode register */
	DPRINTFN(6, ("msk_init_yukon: 9\n"));
	reg = YU_SMR_DATA_BLIND(0x1c) |
	      YU_SMR_MFL_VLAN |
	      YU_SMR_IPG_DATA(0x1e);

	if (sc->sk_type != SK_YUKON_FE &&
	    sc->sk_type != SK_YUKON_FE_P)
		reg |= YU_SMR_MFL_JUMBO;

	SK_YU_WRITE_2(sc_if, YUKON_SMR, reg);

	DPRINTFN(6, ("msk_init_yukon: 10\n"));
	struct ifnet *ifp = &sc_if->sk_ethercom.ec_if;
	/* msk_attach calls me before ether_ifattach so check null */
	if (ifp != NULL && ifp->if_sadl != NULL)
		memcpy(sc_if->sk_enaddr, CLLADDR(ifp->if_sadl),
		    sizeof(sc_if->sk_enaddr));
	/* Setup Yukon's address */
	for (i = 0; i < 3; i++) {
		/* Write Source Address 1 (unicast filter) */
		SK_YU_WRITE_2(sc_if, YUKON_SAL1 + i * 4,
			      sc_if->sk_enaddr[i * 2] |
			      sc_if->sk_enaddr[i * 2 + 1] << 8);
	}

	for (i = 0; i < 3; i++) {
		reg = sk_win_read_2(sc_if->sk_softc,
				    SK_MAC1_0 + i * 2 + sc_if->sk_port * 8);
		SK_YU_WRITE_2(sc_if, YUKON_SAL2 + i * 4, reg);
	}

	/* Set promiscuous mode */
	msk_setpromisc(sc_if);

	/* Set multicast filter */
	DPRINTFN(6, ("msk_init_yukon: 11\n"));
	msk_setmulti(sc_if);

	/* enable interrupt mask for counter overflows */
	DPRINTFN(6, ("msk_init_yukon: 12\n"));
	SK_YU_WRITE_2(sc_if, YUKON_TIMR, 0);
	SK_YU_WRITE_2(sc_if, YUKON_RIMR, 0);
	SK_YU_WRITE_2(sc_if, YUKON_TRIMR, 0);

	/* Configure RX MAC FIFO Flush Mask */
	v = YU_RXSTAT_FOFL | YU_RXSTAT_CRCERR | YU_RXSTAT_MIIERR |
	    YU_RXSTAT_BADFC | YU_RXSTAT_GOODFC | YU_RXSTAT_RUNT |
	    YU_RXSTAT_JABBER;
	SK_IF_WRITE_2(sc_if, 0, SK_RXMF1_FLUSH_MASK, v);

	/* Configure RX MAC FIFO */
	SK_IF_WRITE_1(sc_if, 0, SK_RXMF1_CTRL_TEST, SK_RFCTL_RESET_CLEAR);
	SK_IF_WRITE_2(sc_if, 0, SK_RXMF1_CTRL_TEST, SK_RFCTL_OPERATION_ON |
	    SK_RFCTL_FIFO_FLUSH_ON);

	/* Increase flush threshold to 64 bytes */
	SK_IF_WRITE_2(sc_if, 0, SK_RXMF1_FLUSH_THRESHOLD,
	    SK_RFCTL_FIFO_THRESHOLD + 1);

	/* Configure TX MAC FIFO */
	SK_IF_WRITE_1(sc_if, 0, SK_TXMF1_CTRL_TEST, SK_TFCTL_RESET_CLEAR);
	SK_IF_WRITE_2(sc_if, 0, SK_TXMF1_CTRL_TEST, SK_TFCTL_OPERATION_ON);

#if 1
	SK_YU_WRITE_2(sc_if, YUKON_GPCR, YU_GPCR_TXEN | YU_GPCR_RXEN);
#endif
	DPRINTFN(6, ("msk_init_yukon: end\n"));
}

/*
 * Note that to properly initialize any part of the GEnesis chip,
 * you first have to take it out of reset mode.
 */
int
msk_init(struct ifnet *ifp)
{
	struct sk_if_softc	*sc_if = ifp->if_softc;
	struct sk_softc		*sc = sc_if->sk_softc;
	int			rc = 0, s;
	uint32_t		imr, imtimer_ticks;


	DPRINTFN(2, ("msk_init\n"));

	s = splnet();

	/* Cancel pending I/O and free all RX/TX buffers. */
	msk_stop(ifp, 1);

	/* Configure I2C registers */

	/* Configure XMAC(s) */
	msk_init_yukon(sc_if);
	if ((rc = ether_mediachange(ifp)) != 0)
		goto out;

	/* Configure transmit arbiter(s) */
	SK_IF_WRITE_1(sc_if, 0, SK_TXAR1_COUNTERCTL, SK_TXARCTL_ON);
#if 0
	    SK_TXARCTL_ON | SK_TXARCTL_FSYNC_ON);
#endif

	if (sc->sk_ramsize) {
		/* Configure RAMbuffers */
		SK_IF_WRITE_4(sc_if, 0, SK_RXRB1_CTLTST, SK_RBCTL_UNRESET);
		SK_IF_WRITE_4(sc_if, 0, SK_RXRB1_START, sc_if->sk_rx_ramstart);
		SK_IF_WRITE_4(sc_if, 0, SK_RXRB1_WR_PTR, sc_if->sk_rx_ramstart);
		SK_IF_WRITE_4(sc_if, 0, SK_RXRB1_RD_PTR, sc_if->sk_rx_ramstart);
		SK_IF_WRITE_4(sc_if, 0, SK_RXRB1_END, sc_if->sk_rx_ramend);
		SK_IF_WRITE_4(sc_if, 0, SK_RXRB1_CTLTST, SK_RBCTL_ON);

		SK_IF_WRITE_4(sc_if, 1, SK_TXRBA1_CTLTST, SK_RBCTL_UNRESET);
		SK_IF_WRITE_4(sc_if, 1, SK_TXRBA1_CTLTST, SK_RBCTL_STORENFWD_ON);
		SK_IF_WRITE_4(sc_if, 1, SK_TXRBA1_START, sc_if->sk_tx_ramstart);
		SK_IF_WRITE_4(sc_if, 1, SK_TXRBA1_WR_PTR, sc_if->sk_tx_ramstart);
		SK_IF_WRITE_4(sc_if, 1, SK_TXRBA1_RD_PTR, sc_if->sk_tx_ramstart);
		SK_IF_WRITE_4(sc_if, 1, SK_TXRBA1_END, sc_if->sk_tx_ramend);
		SK_IF_WRITE_4(sc_if, 1, SK_TXRBA1_CTLTST, SK_RBCTL_ON);
	}

	/* Configure BMUs */
	SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_BMU_CSR, 0x00000016);
	SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_BMU_CSR, 0x00000d28);
	SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_BMU_CSR, 0x00000080);
	SK_IF_WRITE_2(sc_if, 0, SK_RXQ1_Y2_WM, 0x0600);	/* XXX ??? */

	SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_BMU_CSR, 0x00000016);
	SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_BMU_CSR, 0x00000d28);
	SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_BMU_CSR, 0x00000080);
	SK_IF_WRITE_2(sc_if, 1, SK_TXQA1_Y2_WM, 0x0600);	/* XXX ??? */

	/* Make sure the sync transmit queue is disabled. */
	SK_IF_WRITE_4(sc_if, 1, SK_TXRBS1_CTLTST, SK_RBCTL_RESET);

	/* Init descriptors */
	if (msk_init_rx_ring(sc_if) == ENOBUFS) {
		aprint_error_dev(sc_if->sk_dev, "initialization failed: no "
		    "memory for rx buffers\n");
		msk_stop(ifp, 1);
		splx(s);
		return ENOBUFS;
	}

	if (msk_init_tx_ring(sc_if) == ENOBUFS) {
		aprint_error_dev(sc_if->sk_dev, "initialization failed: no "
		    "memory for tx buffers\n");
		msk_stop(ifp, 1);
		splx(s);
		return ENOBUFS;
	}

	/* Set interrupt moderation if changed via sysctl. */
	switch (sc->sk_type) {
	case SK_YUKON_EC:
	case SK_YUKON_EC_U:
	case SK_YUKON_EX:
	case SK_YUKON_SUPR:
	case SK_YUKON_ULTRA2:
	case SK_YUKON_OPTIMA:
	case SK_YUKON_PRM:
	case SK_YUKON_OPTIMA2:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_EC;
		break;
	case SK_YUKON_FE:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_FE;
		break;
	case SK_YUKON_FE_P:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_FE_P;
		break;
	case SK_YUKON_XL:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON_XL;
		break;
	default:
		imtimer_ticks = SK_IMTIMER_TICKS_YUKON;
	}
	imr = sk_win_read_4(sc, SK_IMTIMERINIT);
	if (imr != SK_IM_USECS(sc->sk_int_mod)) {
		sk_win_write_4(sc, SK_IMTIMERINIT,
		    SK_IM_USECS(sc->sk_int_mod));
		aprint_verbose_dev(sc->sk_dev,
		    "interrupt moderation is %d us\n", sc->sk_int_mod);
	}

	/* Initialize prefetch engine. */
	SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_Y2_PREF_CSR, 0x00000001);
	SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_Y2_PREF_CSR, 0x00000002);
	SK_IF_WRITE_2(sc_if, 0, SK_RXQ1_Y2_PREF_LIDX, MSK_RX_RING_CNT - 1);
	SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_Y2_PREF_ADDRLO,
	    MSK_RX_RING_ADDR(sc_if, 0));
	SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_Y2_PREF_ADDRHI,
	    (uint64_t)MSK_RX_RING_ADDR(sc_if, 0) >> 32);
	SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_Y2_PREF_CSR, 0x00000008);
	SK_IF_READ_4(sc_if, 0, SK_RXQ1_Y2_PREF_CSR);

	SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_Y2_PREF_CSR, 0x00000001);
	SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_Y2_PREF_CSR, 0x00000002);
	SK_IF_WRITE_2(sc_if, 1, SK_TXQA1_Y2_PREF_LIDX, MSK_TX_RING_CNT - 1);
	SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_Y2_PREF_ADDRLO,
	    MSK_TX_RING_ADDR(sc_if, 0));
	SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_Y2_PREF_ADDRHI,
	    (uint64_t)MSK_TX_RING_ADDR(sc_if, 0) >> 32);
	SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_Y2_PREF_CSR, 0x00000008);
	SK_IF_READ_4(sc_if, 1, SK_TXQA1_Y2_PREF_CSR);

	SK_IF_WRITE_2(sc_if, 0, SK_RXQ1_Y2_PREF_PUTIDX,
	    sc_if->sk_cdata.sk_rx_prod);

	/* Configure interrupt handling */
	if (sc_if->sk_port == SK_PORT_A)
		sc->sk_intrmask |= SK_Y2_INTRS1;
	else
		sc->sk_intrmask |= SK_Y2_INTRS2;
	sc->sk_intrmask |= SK_Y2_IMR_BMU;
	CSR_WRITE_4(sc, SK_IMR, sc->sk_intrmask);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;

	callout_schedule(&sc_if->sk_tick_ch, hz);

out:
	splx(s);
	return rc;
}

/*
 * Note: the logic of second parameter is inverted compared to OpenBSD
 * code, since this code uses the function as if_stop hook too.
 */
void
msk_stop(struct ifnet *ifp, int disable)
{
	struct sk_if_softc	*sc_if = ifp->if_softc;
	struct sk_softc		*sc = sc_if->sk_softc;
	struct sk_txmap_entry	*dma;
	int			i;

	DPRINTFN(2, ("msk_stop\n"));

	callout_stop(&sc_if->sk_tick_ch);
	callout_stop(&sc_if->sk_tick_rx);

	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);

	/* Stop transfer of Tx descriptors */

	/* Stop transfer of Rx descriptors */

	if (disable) {
		/* Turn off various components of this interface. */
		SK_IF_WRITE_1(sc_if, 0, SK_RXMF1_CTRL_TEST, SK_RFCTL_RESET_SET);
		SK_IF_WRITE_1(sc_if, 0, SK_TXMF1_CTRL_TEST, SK_TFCTL_RESET_SET);
		SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_BMU_CSR, SK_RXBMU_OFFLINE);
		SK_IF_WRITE_4(sc_if, 0, SK_RXRB1_CTLTST, SK_RBCTL_RESET | SK_RBCTL_OFF);
		SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_BMU_CSR, SK_TXBMU_OFFLINE);
		SK_IF_WRITE_4(sc_if, 1, SK_TXRBA1_CTLTST, SK_RBCTL_RESET | SK_RBCTL_OFF);
		SK_IF_WRITE_1(sc_if, 0, SK_TXAR1_COUNTERCTL, SK_TXARCTL_OFF);
		SK_IF_WRITE_1(sc_if, 0, SK_RXLED1_CTL, SK_RXLEDCTL_COUNTER_STOP);
		SK_IF_WRITE_1(sc_if, 0, SK_TXLED1_CTL, SK_TXLEDCTL_COUNTER_STOP);
		SK_IF_WRITE_1(sc_if, 0, SK_LINKLED1_CTL, SK_LINKLED_OFF);
		SK_IF_WRITE_1(sc_if, 0, SK_LINKLED1_CTL, SK_LINKLED_LINKSYNC_OFF);

		SK_IF_WRITE_4(sc_if, 0, SK_RXQ1_Y2_PREF_CSR, 0x00000001);
		SK_IF_WRITE_4(sc_if, 1, SK_TXQA1_Y2_PREF_CSR, 0x00000001);

		/* Disable interrupts */
		if (sc_if->sk_port == SK_PORT_A)
			sc->sk_intrmask &= ~SK_Y2_INTRS1;
		else
			sc->sk_intrmask &= ~SK_Y2_INTRS2;
		CSR_WRITE_4(sc, SK_IMR, sc->sk_intrmask);
	}

	/* Free RX and TX mbufs still in the queues. */
	for (i = 0; i < MSK_RX_RING_CNT; i++) {
		if (sc_if->sk_cdata.sk_rx_chain[i].sk_mbuf != NULL) {
			m_freem(sc_if->sk_cdata.sk_rx_chain[i].sk_mbuf);
			sc_if->sk_cdata.sk_rx_chain[i].sk_mbuf = NULL;
		}
	}

	sc_if->sk_cdata.sk_rx_prod = 0;
	sc_if->sk_cdata.sk_rx_cons = 0;
	sc_if->sk_cdata.sk_rx_cnt = 0;

	for (i = 0; i < MSK_TX_RING_CNT; i++) {
		if (sc_if->sk_cdata.sk_tx_chain[i].sk_mbuf != NULL) {
			dma = sc_if->sk_cdata.sk_tx_map[i];

			bus_dmamap_sync(sc->sc_dmatag, dma->dmamap, 0,
			    dma->dmamap->dm_mapsize, BUS_DMASYNC_POSTWRITE);

			bus_dmamap_unload(sc->sc_dmatag, dma->dmamap);
#if 1
			SIMPLEQ_INSERT_HEAD(&sc_if->sk_txmap_head,
			    sc_if->sk_cdata.sk_tx_map[i], link);
			sc_if->sk_cdata.sk_tx_map[i] = 0;
#endif
			m_freem(sc_if->sk_cdata.sk_tx_chain[i].sk_mbuf);
			sc_if->sk_cdata.sk_tx_chain[i].sk_mbuf = NULL;
		}
	}

#if 1
	while ((dma = SIMPLEQ_FIRST(&sc_if->sk_txmap_head))) {
		SIMPLEQ_REMOVE_HEAD(&sc_if->sk_txmap_head, link);
		bus_dmamap_destroy(sc->sc_dmatag, dma->dmamap);
		free(dma, M_DEVBUF);
	}
#endif
}

CFATTACH_DECL3_NEW(mskc, sizeof(struct sk_softc), mskc_probe, mskc_attach,
	mskc_detach, NULL, NULL, NULL, DVF_DETACH_SHUTDOWN);

CFATTACH_DECL3_NEW(msk, sizeof(struct sk_if_softc), msk_probe, msk_attach,
	msk_detach, NULL, NULL, NULL, DVF_DETACH_SHUTDOWN);

#ifdef MSK_DEBUG
void
msk_dump_txdesc(struct msk_tx_desc *le, int idx)
{
#define DESC_PRINT(X)					\
	if (X)						\
		printf("txdesc[%d]." #X "=%#x\n",	\
		       idx, X);

	DESC_PRINT(letoh32(le->sk_addr));
	DESC_PRINT(letoh16(le->sk_len));
	DESC_PRINT(le->sk_ctl);
	DESC_PRINT(le->sk_opcode);
#undef DESC_PRINT
}

void
msk_dump_bytes(const char *data, int len)
{
	int c, i, j;

	for (i = 0; i < len; i += 16) {
		printf("%08x  ", i);
		c = len - i;
		if (c > 16) c = 16;

		for (j = 0; j < c; j++) {
			printf("%02x ", data[i + j] & 0xff);
			if ((j & 0xf) == 7 && j > 0)
				printf(" ");
		}

		for (; j < 16; j++)
			printf("   ");
		printf("  ");

		for (j = 0; j < c; j++) {
			int ch = data[i + j] & 0xff;
			printf("%c", ' ' <= ch && ch <= '~' ? ch : ' ');
		}

		printf("\n");

		if (c < 16)
			break;
	}
}

void
msk_dump_mbuf(struct mbuf *m)
{
	int count = m->m_pkthdr.len;

	printf("m=%p, m->m_pkthdr.len=%d\n", m, m->m_pkthdr.len);

	while (count > 0 && m) {
		printf("m=%p, m->m_data=%p, m->m_len=%d\n",
		       m, m->m_data, m->m_len);
		if (mskdebug >= 4)
			msk_dump_bytes(mtod(m, char *), m->m_len);

		count -= m->m_len;
		m = m->m_next;
	}
}
#endif

static int
msk_sysctl_handler(SYSCTLFN_ARGS)
{
	int error, t;
	struct sysctlnode node;
	struct sk_softc *sc;

	node = *rnode;
	sc = node.sysctl_data;
	t = sc->sk_int_mod;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	if (t < SK_IM_MIN || t > SK_IM_MAX)
		return EINVAL;

	/* update the softc with sysctl-changed value, and mark
	   for hardware update */
	sc->sk_int_mod = t;
	sc->sk_int_mod_pending = 1;
	return 0;
}

/*
 * Set up sysctl(3) MIB, hw.msk.* - Individual controllers will be
 * set up in mskc_attach()
 */
SYSCTL_SETUP(sysctl_msk, "sysctl msk subtree setup")
{
	int rc;
	const struct sysctlnode *node;

	if ((rc = sysctl_createv(clog, 0, NULL, &node,
	    0, CTLTYPE_NODE, "msk",
	    SYSCTL_DESCR("msk interface controls"),
	    NULL, 0, NULL, 0, CTL_HW, CTL_CREATE, CTL_EOL)) != 0) {
		goto err;
	}

	msk_root_num = node->sysctl_num;
	return;

err:
	aprint_error("%s: syctl_createv failed (rc = %d)\n", __func__, rc);
}
