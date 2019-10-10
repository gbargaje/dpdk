/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2019 NXP
 */

#include "pfe_logs.h"
#include "pfe_mod.h"
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

static int
pfe_hif_alloc_descr(struct pfe_hif *hif)
{
	void *addr;
	int err = 0;

	PMD_INIT_FUNC_TRACE();

	addr = rte_zmalloc(NULL, HIF_RX_DESC_NT * sizeof(struct hif_desc) +
		HIF_TX_DESC_NT * sizeof(struct hif_desc), RTE_CACHE_LINE_SIZE);
	if (!addr) {
		PFE_PMD_ERR("Could not allocate buffer descriptors!");
		err = -ENOMEM;
		goto err0;
	}

	hif->descr_baseaddr_p = pfe_mem_vtop((uintptr_t)addr);
	hif->descr_baseaddr_v = addr;
	hif->rx_ring_size = HIF_RX_DESC_NT;
	hif->tx_ring_size = HIF_TX_DESC_NT;

	return 0;

err0:
	return err;
}

static void
pfe_hif_free_descr(struct pfe_hif *hif)
{
	PMD_INIT_FUNC_TRACE();

	rte_free(hif->descr_baseaddr_v);
}

#if defined(LS1012A_PFE_RESET_WA)
static void
pfe_hif_disable_rx_desc(struct pfe_hif *hif)
{
	u32 ii;
	struct hif_desc	*desc = hif->rx_base;

	/*Mark all descriptors as LAST_BD */
	for (ii = 0; ii < hif->rx_ring_size; ii++) {
		desc->ctrl |= BD_CTRL_LAST_BD;
		desc++;
	}
}

struct class_rx_hdr_t {
	u32     next_ptr;       /* ptr to the start of the first DDR buffer */
	u16     length;         /* total packet length */
	u16     phyno;          /* input physical port number */
	u32     status;         /* gemac status bits */
	u32     status2;            /* reserved for software usage */
};

/* STATUS_BAD_FRAME_ERR is set for all errors (including checksums if enabled)
 * except overflow
 */
#define STATUS_BAD_FRAME_ERR            BIT(16)
#define STATUS_LENGTH_ERR               BIT(17)
#define STATUS_CRC_ERR                  BIT(18)
#define STATUS_TOO_SHORT_ERR            BIT(19)
#define STATUS_TOO_LONG_ERR             BIT(20)
#define STATUS_CODE_ERR                 BIT(21)
#define STATUS_MC_HASH_MATCH            BIT(22)
#define STATUS_CUMULATIVE_ARC_HIT       BIT(23)
#define STATUS_UNICAST_HASH_MATCH       BIT(24)
#define STATUS_IP_CHECKSUM_CORRECT      BIT(25)
#define STATUS_TCP_CHECKSUM_CORRECT     BIT(26)
#define STATUS_UDP_CHECKSUM_CORRECT     BIT(27)
#define STATUS_OVERFLOW_ERR             BIT(28) /* GPI error */
#define MIN_PKT_SIZE			64
#define DUMMY_PKT_COUNT			128

static inline void
copy_to_lmem(u32 *dst, u32 *src, int len)
{
	int i;

	for (i = 0; i < len; i += sizeof(u32))	{
		*dst = htonl(*src);
		dst++; src++;
	}
}
#if defined(RTE_TOOLCHAIN_GCC)
__attribute__ ((optimize(1)))
#endif
static void
send_dummy_pkt_to_hif(void)
{
	void *lmem_ptr, *ddr_ptr, *lmem_virt_addr;
	u64 physaddr;
	struct class_rx_hdr_t local_hdr;
	static u32 dummy_pkt[] =  {
		0x33221100, 0x2b785544, 0xd73093cb, 0x01000608,
		0x04060008, 0x2b780200, 0xd73093cb, 0x0a01a8c0,
		0x33221100, 0xa8c05544, 0x00000301, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0xbe86c51f };

	ddr_ptr = (void *)(size_t)readl(BMU2_BASE_ADDR + BMU_ALLOC_CTRL);
	if (!ddr_ptr)
		return;

	lmem_ptr = (void *)(size_t)readl(BMU1_BASE_ADDR + BMU_ALLOC_CTRL);
	if (!lmem_ptr)
		return;

	PFE_PMD_INFO("Sending a dummy pkt to HIF %p %p", ddr_ptr, lmem_ptr);
	physaddr = DDR_VIRT_TO_PFE(ddr_ptr);

	lmem_virt_addr = (void *)CBUS_PFE_TO_VIRT((unsigned long)lmem_ptr);

	local_hdr.phyno = htons(0); /* RX_PHY_0 */
	local_hdr.length = htons(MIN_PKT_SIZE);

	local_hdr.next_ptr = htonl((u32)physaddr);
	/*Mark checksum is correct */
	local_hdr.status = htonl((STATUS_IP_CHECKSUM_CORRECT |
				STATUS_UDP_CHECKSUM_CORRECT |
				STATUS_TCP_CHECKSUM_CORRECT |
				STATUS_UNICAST_HASH_MATCH |
				STATUS_CUMULATIVE_ARC_HIT));
	copy_to_lmem((u32 *)lmem_virt_addr, (u32 *)&local_hdr,
		     sizeof(local_hdr));

	copy_to_lmem((u32 *)(lmem_virt_addr + LMEM_HDR_SIZE), (u32 *)dummy_pkt,
		     0x40);

	writel((unsigned long)lmem_ptr, CLASS_INQ_PKTPTR);
}

void
pfe_hif_rx_idle(struct pfe_hif *hif)
{
	int hif_stop_loop = DUMMY_PKT_COUNT;
	u32 rx_status;

	pfe_hif_disable_rx_desc(hif);
	PFE_PMD_INFO("Bringing hif to idle state...");
	writel(0, HIF_INT_ENABLE);
	/*If HIF Rx BDP is busy send a dummy packet */
	do {
		rx_status = readl(HIF_RX_STATUS);
		if (rx_status & BDP_CSR_RX_DMA_ACTV)
			send_dummy_pkt_to_hif();

		sleep(1);
	} while (--hif_stop_loop);

	if (readl(HIF_RX_STATUS) & BDP_CSR_RX_DMA_ACTV)
		PFE_PMD_ERR("Failed\n");
	else
		PFE_PMD_INFO("Done\n");
}
#endif

/*
 * pfe_hif_init
 * This function initializes the baseaddresses and irq, etc.
 */
int
pfe_hif_init(struct pfe *pfe)
{
	struct pfe_hif *hif = &pfe->hif;
	int err;

	PMD_INIT_FUNC_TRACE();

#if defined(LS1012A_PFE_RESET_WA)
	pfe_hif_rx_idle(hif);
#endif

	err = pfe_hif_alloc_descr(hif);
	if (err)
		goto err0;

	rte_spinlock_init(&hif->tx_lock);
	rte_spinlock_init(&hif->lock);

	gpi_enable(HGPI_BASE_ADDR);
	if (getenv("PFE_INTR_SUPPORT")) {
		struct epoll_event epoll_ev;
		int event_fd = -1, epoll_fd, pfe_cdev_fd;

		pfe_cdev_fd = open(PFE_CDEV_PATH, O_RDWR);
		if (pfe_cdev_fd < 0) {
			PFE_PMD_WARN("Unable to open PFE device file (%s).\n",
				     PFE_CDEV_PATH);
			pfe->cdev_fd = PFE_CDEV_INVALID_FD;
			return -1;
		}
		pfe->cdev_fd = pfe_cdev_fd;

		event_fd = eventfd(0, EFD_NONBLOCK);
		/* hif interrupt enable */
		err = ioctl(pfe->cdev_fd, PFE_CDEV_HIF_INTR_EN, &event_fd);
		if (err) {
			PFE_PMD_ERR("\nioctl failed for intr enable err: %d\n",
					errno);
			goto err0;
		}
		epoll_fd = epoll_create(1);
		epoll_ev.events = EPOLLIN | EPOLLPRI | EPOLLET;
		epoll_ev.data.fd = event_fd;
		err = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &epoll_ev);
		if (err < 0) {
			PFE_PMD_ERR("epoll_ctl failed with err = %d\n", errno);
			goto err0;
		}
		pfe->hif.epoll_fd = epoll_fd;
	}
	return 0;
err0:
	return err;
}

/* pfe_hif_exit- */
void
pfe_hif_exit(struct pfe *pfe)
{
	struct pfe_hif *hif = &pfe->hif;

	PMD_INIT_FUNC_TRACE();

	rte_spinlock_lock(&hif->lock);
	hif->shm->g_client_status[0] = 0;
	/* Make sure all clients are disabled*/
	hif->shm->g_client_status[1] = 0;

	rte_spinlock_unlock(&hif->lock);

	if (hif->setuped) {
#if defined(LS1012A_PFE_RESET_WA)
		pfe_hif_rx_idle(hif);
#endif
		/*Disable Rx/Tx */
		hif_rx_disable();
		hif_tx_disable();

		pfe_hif_free_descr(hif);
		pfe->hif.setuped = 0;
	}
	gpi_disable(HGPI_BASE_ADDR);
}