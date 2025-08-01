// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2013 - 2018 Intel Corporation. */

#include <linux/net/intel/libie/rx.h>
#include <net/netdev_lock.h>

#include "iavf.h"
#include "iavf_ptp.h"
#include "iavf_prototype.h"
/* All iavf tracepoints are defined by the include below, which must
 * be included exactly once across the whole kernel with
 * CREATE_TRACE_POINTS defined
 */
#define CREATE_TRACE_POINTS
#include "iavf_trace.h"

static int iavf_setup_all_tx_resources(struct iavf_adapter *adapter);
static int iavf_setup_all_rx_resources(struct iavf_adapter *adapter);
static int iavf_close(struct net_device *netdev);
static void iavf_init_get_resources(struct iavf_adapter *adapter);
static int iavf_check_reset_complete(struct iavf_hw *hw);

char iavf_driver_name[] = "iavf";
static const char iavf_driver_string[] =
	"Intel(R) Ethernet Adaptive Virtual Function Network Driver";

static const char iavf_copyright[] =
	"Copyright (c) 2013 - 2018 Intel Corporation.";

/* iavf_pci_tbl - PCI Device ID Table
 *
 * Wildcard entries (PCI_ANY_ID) should come last
 * Last entry must be all 0s
 *
 * { Vendor ID, Device ID, SubVendor ID, SubDevice ID,
 *   Class, Class Mask, private data (not used) }
 */
static const struct pci_device_id iavf_pci_tbl[] = {
	{PCI_VDEVICE(INTEL, IAVF_DEV_ID_VF), 0},
	{PCI_VDEVICE(INTEL, IAVF_DEV_ID_VF_HV), 0},
	{PCI_VDEVICE(INTEL, IAVF_DEV_ID_X722_VF), 0},
	{PCI_VDEVICE(INTEL, IAVF_DEV_ID_ADAPTIVE_VF), 0},
	/* required last entry */
	{0, }
};

MODULE_DEVICE_TABLE(pci, iavf_pci_tbl);

MODULE_ALIAS("i40evf");
MODULE_DESCRIPTION("Intel(R) Ethernet Adaptive Virtual Function Network Driver");
MODULE_IMPORT_NS("LIBETH");
MODULE_IMPORT_NS("LIBIE");
MODULE_IMPORT_NS("LIBIE_ADMINQ");
MODULE_LICENSE("GPL v2");

static const struct net_device_ops iavf_netdev_ops;

int iavf_status_to_errno(enum iavf_status status)
{
	switch (status) {
	case IAVF_SUCCESS:
		return 0;
	case IAVF_ERR_PARAM:
	case IAVF_ERR_MAC_TYPE:
	case IAVF_ERR_INVALID_MAC_ADDR:
	case IAVF_ERR_INVALID_LINK_SETTINGS:
	case IAVF_ERR_INVALID_PD_ID:
	case IAVF_ERR_INVALID_QP_ID:
	case IAVF_ERR_INVALID_CQ_ID:
	case IAVF_ERR_INVALID_CEQ_ID:
	case IAVF_ERR_INVALID_AEQ_ID:
	case IAVF_ERR_INVALID_SIZE:
	case IAVF_ERR_INVALID_ARP_INDEX:
	case IAVF_ERR_INVALID_FPM_FUNC_ID:
	case IAVF_ERR_QP_INVALID_MSG_SIZE:
	case IAVF_ERR_INVALID_FRAG_COUNT:
	case IAVF_ERR_INVALID_ALIGNMENT:
	case IAVF_ERR_INVALID_PUSH_PAGE_INDEX:
	case IAVF_ERR_INVALID_IMM_DATA_SIZE:
	case IAVF_ERR_INVALID_VF_ID:
	case IAVF_ERR_INVALID_HMCFN_ID:
	case IAVF_ERR_INVALID_PBLE_INDEX:
	case IAVF_ERR_INVALID_SD_INDEX:
	case IAVF_ERR_INVALID_PAGE_DESC_INDEX:
	case IAVF_ERR_INVALID_SD_TYPE:
	case IAVF_ERR_INVALID_HMC_OBJ_INDEX:
	case IAVF_ERR_INVALID_HMC_OBJ_COUNT:
	case IAVF_ERR_INVALID_SRQ_ARM_LIMIT:
		return -EINVAL;
	case IAVF_ERR_NVM:
	case IAVF_ERR_NVM_CHECKSUM:
	case IAVF_ERR_PHY:
	case IAVF_ERR_CONFIG:
	case IAVF_ERR_UNKNOWN_PHY:
	case IAVF_ERR_LINK_SETUP:
	case IAVF_ERR_ADAPTER_STOPPED:
	case IAVF_ERR_PRIMARY_REQUESTS_PENDING:
	case IAVF_ERR_AUTONEG_NOT_COMPLETE:
	case IAVF_ERR_RESET_FAILED:
	case IAVF_ERR_BAD_PTR:
	case IAVF_ERR_SWFW_SYNC:
	case IAVF_ERR_QP_TOOMANY_WRS_POSTED:
	case IAVF_ERR_QUEUE_EMPTY:
	case IAVF_ERR_FLUSHED_QUEUE:
	case IAVF_ERR_OPCODE_MISMATCH:
	case IAVF_ERR_CQP_COMPL_ERROR:
	case IAVF_ERR_BACKING_PAGE_ERROR:
	case IAVF_ERR_NO_PBLCHUNKS_AVAILABLE:
	case IAVF_ERR_MEMCPY_FAILED:
	case IAVF_ERR_SRQ_ENABLED:
	case IAVF_ERR_ADMIN_QUEUE_ERROR:
	case IAVF_ERR_ADMIN_QUEUE_FULL:
	case IAVF_ERR_BAD_RDMA_CQE:
	case IAVF_ERR_NVM_BLANK_MODE:
	case IAVF_ERR_PE_DOORBELL_NOT_ENABLED:
	case IAVF_ERR_DIAG_TEST_FAILED:
	case IAVF_ERR_FIRMWARE_API_VERSION:
	case IAVF_ERR_ADMIN_QUEUE_CRITICAL_ERROR:
		return -EIO;
	case IAVF_ERR_DEVICE_NOT_SUPPORTED:
		return -ENODEV;
	case IAVF_ERR_NO_AVAILABLE_VSI:
	case IAVF_ERR_RING_FULL:
		return -ENOSPC;
	case IAVF_ERR_NO_MEMORY:
		return -ENOMEM;
	case IAVF_ERR_TIMEOUT:
	case IAVF_ERR_ADMIN_QUEUE_TIMEOUT:
		return -ETIMEDOUT;
	case IAVF_ERR_NOT_IMPLEMENTED:
	case IAVF_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case IAVF_ERR_ADMIN_QUEUE_NO_WORK:
		return -EALREADY;
	case IAVF_ERR_NOT_READY:
		return -EBUSY;
	case IAVF_ERR_BUF_TOO_SHORT:
		return -EMSGSIZE;
	}

	return -EIO;
}

int virtchnl_status_to_errno(enum virtchnl_status_code v_status)
{
	switch (v_status) {
	case VIRTCHNL_STATUS_SUCCESS:
		return 0;
	case VIRTCHNL_STATUS_ERR_PARAM:
	case VIRTCHNL_STATUS_ERR_INVALID_VF_ID:
		return -EINVAL;
	case VIRTCHNL_STATUS_ERR_NO_MEMORY:
		return -ENOMEM;
	case VIRTCHNL_STATUS_ERR_OPCODE_MISMATCH:
	case VIRTCHNL_STATUS_ERR_CQP_COMPL_ERROR:
	case VIRTCHNL_STATUS_ERR_ADMIN_QUEUE_ERROR:
		return -EIO;
	case VIRTCHNL_STATUS_ERR_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	}

	return -EIO;
}

/**
 * iavf_pdev_to_adapter - go from pci_dev to adapter
 * @pdev: pci_dev pointer
 */
static struct iavf_adapter *iavf_pdev_to_adapter(struct pci_dev *pdev)
{
	return netdev_priv(pci_get_drvdata(pdev));
}

/**
 * iavf_is_reset_in_progress - Check if a reset is in progress
 * @adapter: board private structure
 */
static bool iavf_is_reset_in_progress(struct iavf_adapter *adapter)
{
	if (adapter->state == __IAVF_RESETTING ||
	    adapter->flags & (IAVF_FLAG_RESET_PENDING |
			      IAVF_FLAG_RESET_NEEDED))
		return true;

	return false;
}

/**
 * iavf_wait_for_reset - Wait for reset to finish.
 * @adapter: board private structure
 *
 * Returns 0 if reset finished successfully, negative on timeout or interrupt.
 */
int iavf_wait_for_reset(struct iavf_adapter *adapter)
{
	int ret = wait_event_interruptible_timeout(adapter->reset_waitqueue,
					!iavf_is_reset_in_progress(adapter),
					msecs_to_jiffies(5000));

	/* If ret < 0 then it means wait was interrupted.
	 * If ret == 0 then it means we got a timeout while waiting
	 * for reset to finish.
	 * If ret > 0 it means reset has finished.
	 */
	if (ret > 0)
		return 0;
	else if (ret < 0)
		return -EINTR;
	else
		return -EBUSY;
}

/**
 * iavf_allocate_dma_mem_d - OS specific memory alloc for shared code
 * @hw:   pointer to the HW structure
 * @mem:  ptr to mem struct to fill out
 * @size: size of memory requested
 * @alignment: what to align the allocation to
 **/
enum iavf_status iavf_allocate_dma_mem_d(struct iavf_hw *hw,
					 struct iavf_dma_mem *mem,
					 u64 size, u32 alignment)
{
	struct iavf_adapter *adapter = (struct iavf_adapter *)hw->back;

	if (!mem)
		return IAVF_ERR_PARAM;

	mem->size = ALIGN(size, alignment);
	mem->va = dma_alloc_coherent(&adapter->pdev->dev, mem->size,
				     (dma_addr_t *)&mem->pa, GFP_KERNEL);
	if (mem->va)
		return 0;
	else
		return IAVF_ERR_NO_MEMORY;
}

/**
 * iavf_free_dma_mem - wrapper for DMA memory freeing
 * @hw:   pointer to the HW structure
 * @mem:  ptr to mem struct to free
 **/
enum iavf_status iavf_free_dma_mem(struct iavf_hw *hw, struct iavf_dma_mem *mem)
{
	struct iavf_adapter *adapter = (struct iavf_adapter *)hw->back;

	if (!mem || !mem->va)
		return IAVF_ERR_PARAM;
	dma_free_coherent(&adapter->pdev->dev, mem->size,
			  mem->va, (dma_addr_t)mem->pa);
	return 0;
}

/**
 * iavf_allocate_virt_mem - virt memory alloc wrapper
 * @hw:   pointer to the HW structure
 * @mem:  ptr to mem struct to fill out
 * @size: size of memory requested
 **/
enum iavf_status iavf_allocate_virt_mem(struct iavf_hw *hw,
					struct iavf_virt_mem *mem, u32 size)
{
	if (!mem)
		return IAVF_ERR_PARAM;

	mem->size = size;
	mem->va = kzalloc(size, GFP_KERNEL);

	if (mem->va)
		return 0;
	else
		return IAVF_ERR_NO_MEMORY;
}

/**
 * iavf_free_virt_mem - virt memory free wrapper
 * @hw:   pointer to the HW structure
 * @mem:  ptr to mem struct to free
 **/
void iavf_free_virt_mem(struct iavf_hw *hw, struct iavf_virt_mem *mem)
{
	kfree(mem->va);
}

/**
 * iavf_schedule_reset - Set the flags and schedule a reset event
 * @adapter: board private structure
 * @flags: IAVF_FLAG_RESET_PENDING or IAVF_FLAG_RESET_NEEDED
 **/
void iavf_schedule_reset(struct iavf_adapter *adapter, u64 flags)
{
	if (!test_bit(__IAVF_IN_REMOVE_TASK, &adapter->crit_section) &&
	    !(adapter->flags &
	    (IAVF_FLAG_RESET_PENDING | IAVF_FLAG_RESET_NEEDED))) {
		adapter->flags |= flags;
		queue_work(adapter->wq, &adapter->reset_task);
	}
}

/**
 * iavf_schedule_aq_request - Set the flags and schedule aq request
 * @adapter: board private structure
 * @flags: requested aq flags
 **/
void iavf_schedule_aq_request(struct iavf_adapter *adapter, u64 flags)
{
	adapter->aq_required |= flags;
	mod_delayed_work(adapter->wq, &adapter->watchdog_task, 0);
}

/**
 * iavf_tx_timeout - Respond to a Tx Hang
 * @netdev: network interface device structure
 * @txqueue: queue number that is timing out
 **/
static void iavf_tx_timeout(struct net_device *netdev, unsigned int txqueue)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	adapter->tx_timeout_count++;
	iavf_schedule_reset(adapter, IAVF_FLAG_RESET_NEEDED);
}

/**
 * iavf_misc_irq_disable - Mask off interrupt generation on the NIC
 * @adapter: board private structure
 **/
static void iavf_misc_irq_disable(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;

	if (!adapter->msix_entries)
		return;

	wr32(hw, IAVF_VFINT_DYN_CTL01, 0);

	iavf_flush(hw);

	synchronize_irq(adapter->msix_entries[0].vector);
}

/**
 * iavf_misc_irq_enable - Enable default interrupt generation settings
 * @adapter: board private structure
 **/
static void iavf_misc_irq_enable(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;

	wr32(hw, IAVF_VFINT_DYN_CTL01, IAVF_VFINT_DYN_CTL01_INTENA_MASK |
				       IAVF_VFINT_DYN_CTL01_ITR_INDX_MASK);
	wr32(hw, IAVF_VFINT_ICR0_ENA1, IAVF_VFINT_ICR0_ENA1_ADMINQ_MASK);

	iavf_flush(hw);
}

/**
 * iavf_irq_disable - Mask off interrupt generation on the NIC
 * @adapter: board private structure
 **/
static void iavf_irq_disable(struct iavf_adapter *adapter)
{
	int i;
	struct iavf_hw *hw = &adapter->hw;

	if (!adapter->msix_entries)
		return;

	for (i = 1; i < adapter->num_msix_vectors; i++) {
		wr32(hw, IAVF_VFINT_DYN_CTLN1(i - 1), 0);
		synchronize_irq(adapter->msix_entries[i].vector);
	}
	iavf_flush(hw);
}

/**
 * iavf_irq_enable_queues - Enable interrupt for all queues
 * @adapter: board private structure
 **/
static void iavf_irq_enable_queues(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;
	int i;

	for (i = 1; i < adapter->num_msix_vectors; i++) {
		wr32(hw, IAVF_VFINT_DYN_CTLN1(i - 1),
		     IAVF_VFINT_DYN_CTLN1_INTENA_MASK |
		     IAVF_VFINT_DYN_CTLN1_ITR_INDX_MASK);
	}
}

/**
 * iavf_irq_enable - Enable default interrupt generation settings
 * @adapter: board private structure
 * @flush: boolean value whether to run rd32()
 **/
void iavf_irq_enable(struct iavf_adapter *adapter, bool flush)
{
	struct iavf_hw *hw = &adapter->hw;

	iavf_misc_irq_enable(adapter);
	iavf_irq_enable_queues(adapter);

	if (flush)
		iavf_flush(hw);
}

/**
 * iavf_msix_aq - Interrupt handler for vector 0
 * @irq: interrupt number
 * @data: pointer to netdev
 **/
static irqreturn_t iavf_msix_aq(int irq, void *data)
{
	struct net_device *netdev = data;
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct iavf_hw *hw = &adapter->hw;

	/* handle non-queue interrupts, these reads clear the registers */
	rd32(hw, IAVF_VFINT_ICR01);
	rd32(hw, IAVF_VFINT_ICR0_ENA1);

	if (adapter->state != __IAVF_REMOVE)
		/* schedule work on the private workqueue */
		queue_work(adapter->wq, &adapter->adminq_task);

	return IRQ_HANDLED;
}

/**
 * iavf_msix_clean_rings - MSIX mode Interrupt Handler
 * @irq: interrupt number
 * @data: pointer to a q_vector
 **/
static irqreturn_t iavf_msix_clean_rings(int irq, void *data)
{
	struct iavf_q_vector *q_vector = data;

	if (!q_vector->tx.ring && !q_vector->rx.ring)
		return IRQ_HANDLED;

	napi_schedule_irqoff(&q_vector->napi);

	return IRQ_HANDLED;
}

/**
 * iavf_map_vector_to_rxq - associate irqs with rx queues
 * @adapter: board private structure
 * @v_idx: interrupt number
 * @r_idx: queue number
 **/
static void
iavf_map_vector_to_rxq(struct iavf_adapter *adapter, int v_idx, int r_idx)
{
	struct iavf_q_vector *q_vector = &adapter->q_vectors[v_idx];
	struct iavf_ring *rx_ring = &adapter->rx_rings[r_idx];
	struct iavf_hw *hw = &adapter->hw;

	rx_ring->q_vector = q_vector;
	rx_ring->next = q_vector->rx.ring;
	rx_ring->vsi = &adapter->vsi;
	q_vector->rx.ring = rx_ring;
	q_vector->rx.count++;
	q_vector->rx.next_update = jiffies + 1;
	q_vector->rx.target_itr = ITR_TO_REG(rx_ring->itr_setting);
	q_vector->ring_mask |= BIT(r_idx);
	wr32(hw, IAVF_VFINT_ITRN1(IAVF_RX_ITR, q_vector->reg_idx),
	     q_vector->rx.current_itr >> 1);
	q_vector->rx.current_itr = q_vector->rx.target_itr;
}

/**
 * iavf_map_vector_to_txq - associate irqs with tx queues
 * @adapter: board private structure
 * @v_idx: interrupt number
 * @t_idx: queue number
 **/
static void
iavf_map_vector_to_txq(struct iavf_adapter *adapter, int v_idx, int t_idx)
{
	struct iavf_q_vector *q_vector = &adapter->q_vectors[v_idx];
	struct iavf_ring *tx_ring = &adapter->tx_rings[t_idx];
	struct iavf_hw *hw = &adapter->hw;

	tx_ring->q_vector = q_vector;
	tx_ring->next = q_vector->tx.ring;
	tx_ring->vsi = &adapter->vsi;
	q_vector->tx.ring = tx_ring;
	q_vector->tx.count++;
	q_vector->tx.next_update = jiffies + 1;
	q_vector->tx.target_itr = ITR_TO_REG(tx_ring->itr_setting);
	q_vector->num_ringpairs++;
	wr32(hw, IAVF_VFINT_ITRN1(IAVF_TX_ITR, q_vector->reg_idx),
	     q_vector->tx.target_itr >> 1);
	q_vector->tx.current_itr = q_vector->tx.target_itr;
}

/**
 * iavf_map_rings_to_vectors - Maps descriptor rings to vectors
 * @adapter: board private structure to initialize
 *
 * This function maps descriptor rings to the queue-specific vectors
 * we were allotted through the MSI-X enabling code.  Ideally, we'd have
 * one vector per ring/queue, but on a constrained vector budget, we
 * group the rings as "efficiently" as possible.  You would add new
 * mapping configurations in here.
 **/
static void iavf_map_rings_to_vectors(struct iavf_adapter *adapter)
{
	int rings_remaining = adapter->num_active_queues;
	int ridx = 0, vidx = 0;
	int q_vectors;

	q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (; ridx < rings_remaining; ridx++) {
		iavf_map_vector_to_rxq(adapter, vidx, ridx);
		iavf_map_vector_to_txq(adapter, vidx, ridx);

		/* In the case where we have more queues than vectors, continue
		 * round-robin on vectors until all queues are mapped.
		 */
		if (++vidx >= q_vectors)
			vidx = 0;
	}

	adapter->aq_required |= IAVF_FLAG_AQ_MAP_VECTORS;
}

/**
 * iavf_request_traffic_irqs - Initialize MSI-X interrupts
 * @adapter: board private structure
 * @basename: device basename
 *
 * Allocates MSI-X vectors for tx and rx handling, and requests
 * interrupts from the kernel.
 **/
static int
iavf_request_traffic_irqs(struct iavf_adapter *adapter, char *basename)
{
	unsigned int vector, q_vectors;
	unsigned int rx_int_idx = 0, tx_int_idx = 0;
	int irq_num, err;

	iavf_irq_disable(adapter);
	/* Decrement for Other and TCP Timer vectors */
	q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (vector = 0; vector < q_vectors; vector++) {
		struct iavf_q_vector *q_vector = &adapter->q_vectors[vector];

		irq_num = adapter->msix_entries[vector + NONQ_VECS].vector;

		if (q_vector->tx.ring && q_vector->rx.ring) {
			snprintf(q_vector->name, sizeof(q_vector->name),
				 "iavf-%s-TxRx-%u", basename, rx_int_idx++);
			tx_int_idx++;
		} else if (q_vector->rx.ring) {
			snprintf(q_vector->name, sizeof(q_vector->name),
				 "iavf-%s-rx-%u", basename, rx_int_idx++);
		} else if (q_vector->tx.ring) {
			snprintf(q_vector->name, sizeof(q_vector->name),
				 "iavf-%s-tx-%u", basename, tx_int_idx++);
		} else {
			/* skip this unused q_vector */
			continue;
		}
		err = request_irq(irq_num,
				  iavf_msix_clean_rings,
				  0,
				  q_vector->name,
				  q_vector);
		if (err) {
			dev_info(&adapter->pdev->dev,
				 "Request_irq failed, error: %d\n", err);
			goto free_queue_irqs;
		}
	}

	return 0;

free_queue_irqs:
	while (vector) {
		vector--;
		irq_num = adapter->msix_entries[vector + NONQ_VECS].vector;
		free_irq(irq_num, &adapter->q_vectors[vector]);
	}
	return err;
}

/**
 * iavf_request_misc_irq - Initialize MSI-X interrupts
 * @adapter: board private structure
 *
 * Allocates MSI-X vector 0 and requests interrupts from the kernel. This
 * vector is only for the admin queue, and stays active even when the netdev
 * is closed.
 **/
static int iavf_request_misc_irq(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int err;

	snprintf(adapter->misc_vector_name,
		 sizeof(adapter->misc_vector_name) - 1, "iavf-%s:mbx",
		 dev_name(&adapter->pdev->dev));
	err = request_irq(adapter->msix_entries[0].vector,
			  &iavf_msix_aq, 0,
			  adapter->misc_vector_name, netdev);
	if (err) {
		dev_err(&adapter->pdev->dev,
			"request_irq for %s failed: %d\n",
			adapter->misc_vector_name, err);
		free_irq(adapter->msix_entries[0].vector, netdev);
	}
	return err;
}

/**
 * iavf_free_traffic_irqs - Free MSI-X interrupts
 * @adapter: board private structure
 *
 * Frees all MSI-X vectors other than 0.
 **/
static void iavf_free_traffic_irqs(struct iavf_adapter *adapter)
{
	struct iavf_q_vector *q_vector;
	int vector, irq_num, q_vectors;

	if (!adapter->msix_entries)
		return;

	q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (vector = 0; vector < q_vectors; vector++) {
		q_vector = &adapter->q_vectors[vector];
		netif_napi_set_irq_locked(&q_vector->napi, -1);
		irq_num = adapter->msix_entries[vector + NONQ_VECS].vector;
		free_irq(irq_num, q_vector);
	}
}

/**
 * iavf_free_misc_irq - Free MSI-X miscellaneous vector
 * @adapter: board private structure
 *
 * Frees MSI-X vector 0.
 **/
static void iavf_free_misc_irq(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;

	if (!adapter->msix_entries)
		return;

	free_irq(adapter->msix_entries[0].vector, netdev);
}

/**
 * iavf_configure_tx - Configure Transmit Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Tx unit of the MAC after a reset.
 **/
static void iavf_configure_tx(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;
	int i;

	for (i = 0; i < adapter->num_active_queues; i++)
		adapter->tx_rings[i].tail = hw->hw_addr + IAVF_QTX_TAIL1(i);
}

/**
 * iavf_select_rx_desc_format - Select Rx descriptor format
 * @adapter: adapter private structure
 *
 * Select what Rx descriptor format based on availability and enabled
 * features.
 *
 * Return: the desired RXDID to select for a given Rx queue, as defined by
 *         enum virtchnl_rxdid_format.
 */
static u8 iavf_select_rx_desc_format(const struct iavf_adapter *adapter)
{
	u64 rxdids = adapter->supp_rxdids;

	/* If we did not negotiate VIRTCHNL_VF_OFFLOAD_RX_FLEX_DESC, we must
	 * stick with the default value of the legacy 32 byte format.
	 */
	if (!IAVF_RXDID_ALLOWED(adapter))
		return VIRTCHNL_RXDID_1_32B_BASE;

	/* Rx timestamping requires the use of flexible NIC descriptors */
	if (iavf_ptp_cap_supported(adapter, VIRTCHNL_1588_PTP_CAP_RX_TSTAMP)) {
		if (rxdids & BIT(VIRTCHNL_RXDID_2_FLEX_SQ_NIC))
			return VIRTCHNL_RXDID_2_FLEX_SQ_NIC;

		pci_warn(adapter->pdev,
			 "Unable to negotiate flexible descriptor format\n");
	}

	/* Warn if the PF does not list support for the default legacy
	 * descriptor format. This shouldn't happen, as this is the format
	 * used if VIRTCHNL_VF_OFFLOAD_RX_FLEX_DESC is not supported. It is
	 * likely caused by a bug in the PF implementation failing to indicate
	 * support for the format.
	 */
	if (!(rxdids & VIRTCHNL_RXDID_1_32B_BASE_M))
		netdev_warn(adapter->netdev, "PF does not list support for default Rx descriptor format\n");

	return VIRTCHNL_RXDID_1_32B_BASE;
}

/**
 * iavf_configure_rx - Configure Receive Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Rx unit of the MAC after a reset.
 **/
static void iavf_configure_rx(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;

	adapter->rxdid = iavf_select_rx_desc_format(adapter);

	for (u32 i = 0; i < adapter->num_active_queues; i++) {
		adapter->rx_rings[i].tail = hw->hw_addr + IAVF_QRX_TAIL1(i);
		adapter->rx_rings[i].rxdid = adapter->rxdid;
	}
}

/**
 * iavf_find_vlan - Search filter list for specific vlan filter
 * @adapter: board private structure
 * @vlan: vlan tag
 *
 * Returns ptr to the filter object or NULL. Must be called while holding the
 * mac_vlan_list_lock.
 **/
static struct
iavf_vlan_filter *iavf_find_vlan(struct iavf_adapter *adapter,
				 struct iavf_vlan vlan)
{
	struct iavf_vlan_filter *f;

	list_for_each_entry(f, &adapter->vlan_filter_list, list) {
		if (f->vlan.vid == vlan.vid &&
		    f->vlan.tpid == vlan.tpid)
			return f;
	}

	return NULL;
}

/**
 * iavf_add_vlan - Add a vlan filter to the list
 * @adapter: board private structure
 * @vlan: VLAN tag
 *
 * Returns ptr to the filter object or NULL when no memory available.
 **/
static struct
iavf_vlan_filter *iavf_add_vlan(struct iavf_adapter *adapter,
				struct iavf_vlan vlan)
{
	struct iavf_vlan_filter *f = NULL;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	f = iavf_find_vlan(adapter, vlan);
	if (!f) {
		f = kzalloc(sizeof(*f), GFP_ATOMIC);
		if (!f)
			goto clearout;

		f->vlan = vlan;

		list_add_tail(&f->list, &adapter->vlan_filter_list);
		f->state = IAVF_VLAN_ADD;
		adapter->num_vlan_filters++;
		iavf_schedule_aq_request(adapter, IAVF_FLAG_AQ_ADD_VLAN_FILTER);
	} else if (f->state == IAVF_VLAN_REMOVE) {
		/* IAVF_VLAN_REMOVE means that VLAN wasn't yet removed.
		 * We can safely only change the state here.
		 */
		f->state = IAVF_VLAN_ACTIVE;
	}

clearout:
	spin_unlock_bh(&adapter->mac_vlan_list_lock);
	return f;
}

/**
 * iavf_del_vlan - Remove a vlan filter from the list
 * @adapter: board private structure
 * @vlan: VLAN tag
 **/
static void iavf_del_vlan(struct iavf_adapter *adapter, struct iavf_vlan vlan)
{
	struct iavf_vlan_filter *f;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	f = iavf_find_vlan(adapter, vlan);
	if (f) {
		/* IAVF_ADD_VLAN means that VLAN wasn't even added yet.
		 * Remove it from the list.
		 */
		if (f->state == IAVF_VLAN_ADD) {
			list_del(&f->list);
			kfree(f);
			adapter->num_vlan_filters--;
		} else {
			f->state = IAVF_VLAN_REMOVE;
			iavf_schedule_aq_request(adapter,
						 IAVF_FLAG_AQ_DEL_VLAN_FILTER);
		}
	}

	spin_unlock_bh(&adapter->mac_vlan_list_lock);
}

/**
 * iavf_restore_filters
 * @adapter: board private structure
 *
 * Restore existing non MAC filters when VF netdev comes back up
 **/
static void iavf_restore_filters(struct iavf_adapter *adapter)
{
	struct iavf_vlan_filter *f;

	/* re-add all VLAN filters */
	spin_lock_bh(&adapter->mac_vlan_list_lock);

	list_for_each_entry(f, &adapter->vlan_filter_list, list) {
		if (f->state == IAVF_VLAN_INACTIVE)
			f->state = IAVF_VLAN_ADD;
	}

	spin_unlock_bh(&adapter->mac_vlan_list_lock);
	adapter->aq_required |= IAVF_FLAG_AQ_ADD_VLAN_FILTER;
}

/**
 * iavf_get_num_vlans_added - get number of VLANs added
 * @adapter: board private structure
 */
u16 iavf_get_num_vlans_added(struct iavf_adapter *adapter)
{
	return adapter->num_vlan_filters;
}

/**
 * iavf_get_max_vlans_allowed - get maximum VLANs allowed for this VF
 * @adapter: board private structure
 *
 * This depends on the negotiated VLAN capability. For VIRTCHNL_VF_OFFLOAD_VLAN,
 * do not impose a limit as that maintains current behavior and for
 * VIRTCHNL_VF_OFFLOAD_VLAN_V2, use the maximum allowed sent from the PF.
 **/
static u16 iavf_get_max_vlans_allowed(struct iavf_adapter *adapter)
{
	/* don't impose any limit for VIRTCHNL_VF_OFFLOAD_VLAN since there has
	 * never been a limit on the VF driver side
	 */
	if (VLAN_ALLOWED(adapter))
		return VLAN_N_VID;
	else if (VLAN_V2_ALLOWED(adapter))
		return adapter->vlan_v2_caps.filtering.max_filters;

	return 0;
}

/**
 * iavf_max_vlans_added - check if maximum VLANs allowed already exist
 * @adapter: board private structure
 **/
static bool iavf_max_vlans_added(struct iavf_adapter *adapter)
{
	if (iavf_get_num_vlans_added(adapter) <
	    iavf_get_max_vlans_allowed(adapter))
		return false;

	return true;
}

/**
 * iavf_vlan_rx_add_vid - Add a VLAN filter to a device
 * @netdev: network device struct
 * @proto: unused protocol data
 * @vid: VLAN tag
 **/
static int iavf_vlan_rx_add_vid(struct net_device *netdev,
				__always_unused __be16 proto, u16 vid)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	/* Do not track VLAN 0 filter, always added by the PF on VF init */
	if (!vid)
		return 0;

	if (!VLAN_FILTERING_ALLOWED(adapter))
		return -EIO;

	if (iavf_max_vlans_added(adapter)) {
		netdev_err(netdev, "Max allowed VLAN filters %u. Remove existing VLANs or disable filtering via Ethtool if supported.\n",
			   iavf_get_max_vlans_allowed(adapter));
		return -EIO;
	}

	if (!iavf_add_vlan(adapter, IAVF_VLAN(vid, be16_to_cpu(proto))))
		return -ENOMEM;

	return 0;
}

/**
 * iavf_vlan_rx_kill_vid - Remove a VLAN filter from a device
 * @netdev: network device struct
 * @proto: unused protocol data
 * @vid: VLAN tag
 **/
static int iavf_vlan_rx_kill_vid(struct net_device *netdev,
				 __always_unused __be16 proto, u16 vid)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	/* We do not track VLAN 0 filter */
	if (!vid)
		return 0;

	iavf_del_vlan(adapter, IAVF_VLAN(vid, be16_to_cpu(proto)));
	return 0;
}

/**
 * iavf_find_filter - Search filter list for specific mac filter
 * @adapter: board private structure
 * @macaddr: the MAC address
 *
 * Returns ptr to the filter object or NULL. Must be called while holding the
 * mac_vlan_list_lock.
 **/
static struct
iavf_mac_filter *iavf_find_filter(struct iavf_adapter *adapter,
				  const u8 *macaddr)
{
	struct iavf_mac_filter *f;

	if (!macaddr)
		return NULL;

	list_for_each_entry(f, &adapter->mac_filter_list, list) {
		if (ether_addr_equal(macaddr, f->macaddr))
			return f;
	}
	return NULL;
}

/**
 * iavf_add_filter - Add a mac filter to the filter list
 * @adapter: board private structure
 * @macaddr: the MAC address
 *
 * Returns ptr to the filter object or NULL when no memory available.
 **/
struct iavf_mac_filter *iavf_add_filter(struct iavf_adapter *adapter,
					const u8 *macaddr)
{
	struct iavf_mac_filter *f;

	if (!macaddr)
		return NULL;

	f = iavf_find_filter(adapter, macaddr);
	if (!f) {
		f = kzalloc(sizeof(*f), GFP_ATOMIC);
		if (!f)
			return f;

		ether_addr_copy(f->macaddr, macaddr);

		list_add_tail(&f->list, &adapter->mac_filter_list);
		f->add = true;
		f->add_handled = false;
		f->is_new_mac = true;
		f->is_primary = ether_addr_equal(macaddr, adapter->hw.mac.addr);
		adapter->aq_required |= IAVF_FLAG_AQ_ADD_MAC_FILTER;
	} else {
		f->remove = false;
	}

	return f;
}

/**
 * iavf_replace_primary_mac - Replace current primary address
 * @adapter: board private structure
 * @new_mac: new MAC address to be applied
 *
 * Replace current dev_addr and send request to PF for removal of previous
 * primary MAC address filter and addition of new primary MAC filter.
 * Return 0 for success, -ENOMEM for failure.
 *
 * Do not call this with mac_vlan_list_lock!
 **/
static int iavf_replace_primary_mac(struct iavf_adapter *adapter,
				    const u8 *new_mac)
{
	struct iavf_hw *hw = &adapter->hw;
	struct iavf_mac_filter *new_f;
	struct iavf_mac_filter *old_f;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	new_f = iavf_add_filter(adapter, new_mac);
	if (!new_f) {
		spin_unlock_bh(&adapter->mac_vlan_list_lock);
		return -ENOMEM;
	}

	old_f = iavf_find_filter(adapter, hw->mac.addr);
	if (old_f) {
		old_f->is_primary = false;
		old_f->remove = true;
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_MAC_FILTER;
	}
	/* Always send the request to add if changing primary MAC,
	 * even if filter is already present on the list
	 */
	new_f->is_primary = true;
	new_f->add = true;
	ether_addr_copy(hw->mac.addr, new_mac);

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	/* schedule the watchdog task to immediately process the request */
	iavf_schedule_aq_request(adapter, IAVF_FLAG_AQ_ADD_MAC_FILTER);
	return 0;
}

/**
 * iavf_is_mac_set_handled - wait for a response to set MAC from PF
 * @netdev: network interface device structure
 * @macaddr: MAC address to set
 *
 * Returns true on success, false on failure
 */
static bool iavf_is_mac_set_handled(struct net_device *netdev,
				    const u8 *macaddr)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct iavf_mac_filter *f;
	bool ret = false;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	f = iavf_find_filter(adapter, macaddr);

	if (!f || (!f->add && f->add_handled))
		ret = true;

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	return ret;
}

/**
 * iavf_set_mac - NDO callback to set port MAC address
 * @netdev: network interface device structure
 * @p: pointer to an address structure
 *
 * Returns 0 on success, negative on failure
 */
static int iavf_set_mac(struct net_device *netdev, void *p)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct sockaddr *addr = p;
	int ret;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	ret = iavf_replace_primary_mac(adapter, addr->sa_data);

	if (ret)
		return ret;

	ret = wait_event_interruptible_timeout(adapter->vc_waitqueue,
					       iavf_is_mac_set_handled(netdev, addr->sa_data),
					       msecs_to_jiffies(2500));

	/* If ret < 0 then it means wait was interrupted.
	 * If ret == 0 then it means we got a timeout.
	 * else it means we got response for set MAC from PF,
	 * check if netdev MAC was updated to requested MAC,
	 * if yes then set MAC succeeded otherwise it failed return -EACCES
	 */
	if (ret < 0)
		return ret;

	if (!ret)
		return -EAGAIN;

	if (!ether_addr_equal(netdev->dev_addr, addr->sa_data))
		return -EACCES;

	return 0;
}

/**
 * iavf_addr_sync - Callback for dev_(mc|uc)_sync to add address
 * @netdev: the netdevice
 * @addr: address to add
 *
 * Called by __dev_(mc|uc)_sync when an address needs to be added. We call
 * __dev_(uc|mc)_sync from .set_rx_mode and guarantee to hold the hash lock.
 */
static int iavf_addr_sync(struct net_device *netdev, const u8 *addr)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	if (iavf_add_filter(adapter, addr))
		return 0;
	else
		return -ENOMEM;
}

/**
 * iavf_addr_unsync - Callback for dev_(mc|uc)_sync to remove address
 * @netdev: the netdevice
 * @addr: address to add
 *
 * Called by __dev_(mc|uc)_sync when an address needs to be removed. We call
 * __dev_(uc|mc)_sync from .set_rx_mode and guarantee to hold the hash lock.
 */
static int iavf_addr_unsync(struct net_device *netdev, const u8 *addr)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct iavf_mac_filter *f;

	/* Under some circumstances, we might receive a request to delete
	 * our own device address from our uc list. Because we store the
	 * device address in the VSI's MAC/VLAN filter list, we need to ignore
	 * such requests and not delete our device address from this list.
	 */
	if (ether_addr_equal(addr, netdev->dev_addr))
		return 0;

	f = iavf_find_filter(adapter, addr);
	if (f) {
		f->remove = true;
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_MAC_FILTER;
	}
	return 0;
}

/**
 * iavf_promiscuous_mode_changed - check if promiscuous mode bits changed
 * @adapter: device specific adapter
 */
bool iavf_promiscuous_mode_changed(struct iavf_adapter *adapter)
{
	return (adapter->current_netdev_promisc_flags ^ adapter->netdev->flags) &
		(IFF_PROMISC | IFF_ALLMULTI);
}

/**
 * iavf_set_rx_mode - NDO callback to set the netdev filters
 * @netdev: network interface device structure
 **/
static void iavf_set_rx_mode(struct net_device *netdev)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	spin_lock_bh(&adapter->mac_vlan_list_lock);
	__dev_uc_sync(netdev, iavf_addr_sync, iavf_addr_unsync);
	__dev_mc_sync(netdev, iavf_addr_sync, iavf_addr_unsync);
	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	spin_lock_bh(&adapter->current_netdev_promisc_flags_lock);
	if (iavf_promiscuous_mode_changed(adapter))
		adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_PROMISC_MODE;
	spin_unlock_bh(&adapter->current_netdev_promisc_flags_lock);
}

/**
 * iavf_napi_enable_all - enable NAPI on all queue vectors
 * @adapter: board private structure
 **/
static void iavf_napi_enable_all(struct iavf_adapter *adapter)
{
	int q_idx;
	struct iavf_q_vector *q_vector;
	int q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (q_idx = 0; q_idx < q_vectors; q_idx++) {
		struct napi_struct *napi;

		q_vector = &adapter->q_vectors[q_idx];
		napi = &q_vector->napi;
		napi_enable_locked(napi);
	}
}

/**
 * iavf_napi_disable_all - disable NAPI on all queue vectors
 * @adapter: board private structure
 **/
static void iavf_napi_disable_all(struct iavf_adapter *adapter)
{
	int q_idx;
	struct iavf_q_vector *q_vector;
	int q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (q_idx = 0; q_idx < q_vectors; q_idx++) {
		q_vector = &adapter->q_vectors[q_idx];
		napi_disable_locked(&q_vector->napi);
	}
}

/**
 * iavf_configure - set up transmit and receive data structures
 * @adapter: board private structure
 **/
static void iavf_configure(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	iavf_set_rx_mode(netdev);

	iavf_configure_tx(adapter);
	iavf_configure_rx(adapter);
	adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_QUEUES;

	for (i = 0; i < adapter->num_active_queues; i++) {
		struct iavf_ring *ring = &adapter->rx_rings[i];

		iavf_alloc_rx_buffers(ring, IAVF_DESC_UNUSED(ring));
	}
}

/**
 * iavf_up_complete - Finish the last steps of bringing up a connection
 * @adapter: board private structure
 */
static void iavf_up_complete(struct iavf_adapter *adapter)
{
	netdev_assert_locked(adapter->netdev);

	iavf_change_state(adapter, __IAVF_RUNNING);
	clear_bit(__IAVF_VSI_DOWN, adapter->vsi.state);

	iavf_napi_enable_all(adapter);

	iavf_schedule_aq_request(adapter, IAVF_FLAG_AQ_ENABLE_QUEUES);
}

/**
 * iavf_clear_mac_vlan_filters - Remove mac and vlan filters not sent to PF
 * yet and mark other to be removed.
 * @adapter: board private structure
 **/
static void iavf_clear_mac_vlan_filters(struct iavf_adapter *adapter)
{
	struct iavf_vlan_filter *vlf, *vlftmp;
	struct iavf_mac_filter *f, *ftmp;

	spin_lock_bh(&adapter->mac_vlan_list_lock);
	/* clear the sync flag on all filters */
	__dev_uc_unsync(adapter->netdev, NULL);
	__dev_mc_unsync(adapter->netdev, NULL);

	/* remove all MAC filters */
	list_for_each_entry_safe(f, ftmp, &adapter->mac_filter_list,
				 list) {
		if (f->add) {
			list_del(&f->list);
			kfree(f);
		} else {
			f->remove = true;
		}
	}

	/* disable all VLAN filters */
	list_for_each_entry_safe(vlf, vlftmp, &adapter->vlan_filter_list,
				 list)
		vlf->state = IAVF_VLAN_DISABLE;

	spin_unlock_bh(&adapter->mac_vlan_list_lock);
}

/**
 * iavf_clear_cloud_filters - Remove cloud filters not sent to PF yet and
 * mark other to be removed.
 * @adapter: board private structure
 **/
static void iavf_clear_cloud_filters(struct iavf_adapter *adapter)
{
	struct iavf_cloud_filter *cf, *cftmp;

	/* remove all cloud filters */
	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_for_each_entry_safe(cf, cftmp, &adapter->cloud_filter_list,
				 list) {
		if (cf->add) {
			list_del(&cf->list);
			kfree(cf);
			adapter->num_cloud_filters--;
		} else {
			cf->del = true;
		}
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);
}

/**
 * iavf_clear_fdir_filters - Remove fdir filters not sent to PF yet and mark
 * other to be removed.
 * @adapter: board private structure
 **/
static void iavf_clear_fdir_filters(struct iavf_adapter *adapter)
{
	struct iavf_fdir_fltr *fdir;

	/* remove all Flow Director filters */
	spin_lock_bh(&adapter->fdir_fltr_lock);
	list_for_each_entry(fdir, &adapter->fdir_list_head, list) {
		if (fdir->state == IAVF_FDIR_FLTR_ADD_REQUEST) {
			/* Cancel a request, keep filter as inactive */
			fdir->state = IAVF_FDIR_FLTR_INACTIVE;
		} else if (fdir->state == IAVF_FDIR_FLTR_ADD_PENDING ||
			 fdir->state == IAVF_FDIR_FLTR_ACTIVE) {
			/* Disable filters which are active or have a pending
			 * request to PF to be added
			 */
			fdir->state = IAVF_FDIR_FLTR_DIS_REQUEST;
		}
	}
	spin_unlock_bh(&adapter->fdir_fltr_lock);
}

/**
 * iavf_clear_adv_rss_conf - Remove adv rss conf not sent to PF yet and mark
 * other to be removed.
 * @adapter: board private structure
 **/
static void iavf_clear_adv_rss_conf(struct iavf_adapter *adapter)
{
	struct iavf_adv_rss *rss, *rsstmp;

	/* remove all advance RSS configuration */
	spin_lock_bh(&adapter->adv_rss_lock);
	list_for_each_entry_safe(rss, rsstmp, &adapter->adv_rss_list_head,
				 list) {
		if (rss->state == IAVF_ADV_RSS_ADD_REQUEST) {
			list_del(&rss->list);
			kfree(rss);
		} else {
			rss->state = IAVF_ADV_RSS_DEL_REQUEST;
		}
	}
	spin_unlock_bh(&adapter->adv_rss_lock);
}

/**
 * iavf_down - Shutdown the connection processing
 * @adapter: board private structure
 */
void iavf_down(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;

	netdev_assert_locked(netdev);

	if (adapter->state <= __IAVF_DOWN_PENDING)
		return;

	netif_carrier_off(netdev);
	netif_tx_disable(netdev);
	adapter->link_up = false;
	iavf_napi_disable_all(adapter);
	iavf_irq_disable(adapter);

	iavf_clear_mac_vlan_filters(adapter);
	iavf_clear_cloud_filters(adapter);
	iavf_clear_fdir_filters(adapter);
	iavf_clear_adv_rss_conf(adapter);

	if (adapter->flags & IAVF_FLAG_PF_COMMS_FAILED)
		return;

	if (!test_bit(__IAVF_IN_REMOVE_TASK, &adapter->crit_section)) {
		/* cancel any current operation */
		adapter->current_op = VIRTCHNL_OP_UNKNOWN;
		/* Schedule operations to close down the HW. Don't wait
		 * here for this to complete. The watchdog is still running
		 * and it will take care of this.
		 */
		if (!list_empty(&adapter->mac_filter_list))
			adapter->aq_required |= IAVF_FLAG_AQ_DEL_MAC_FILTER;
		if (!list_empty(&adapter->vlan_filter_list))
			adapter->aq_required |= IAVF_FLAG_AQ_DEL_VLAN_FILTER;
		if (!list_empty(&adapter->cloud_filter_list))
			adapter->aq_required |= IAVF_FLAG_AQ_DEL_CLOUD_FILTER;
		if (!list_empty(&adapter->fdir_list_head))
			adapter->aq_required |= IAVF_FLAG_AQ_DEL_FDIR_FILTER;
		if (!list_empty(&adapter->adv_rss_list_head))
			adapter->aq_required |= IAVF_FLAG_AQ_DEL_ADV_RSS_CFG;
	}

	iavf_schedule_aq_request(adapter, IAVF_FLAG_AQ_DISABLE_QUEUES);
}

/**
 * iavf_acquire_msix_vectors - Setup the MSIX capability
 * @adapter: board private structure
 * @vectors: number of vectors to request
 *
 * Work with the OS to set up the MSIX vectors needed.
 *
 * Returns 0 on success, negative on failure
 **/
static int
iavf_acquire_msix_vectors(struct iavf_adapter *adapter, int vectors)
{
	int err, vector_threshold;

	/* We'll want at least 3 (vector_threshold):
	 * 0) Other (Admin Queue and link, mostly)
	 * 1) TxQ[0] Cleanup
	 * 2) RxQ[0] Cleanup
	 */
	vector_threshold = MIN_MSIX_COUNT;

	/* The more we get, the more we will assign to Tx/Rx Cleanup
	 * for the separate queues...where Rx Cleanup >= Tx Cleanup.
	 * Right now, we simply care about how many we'll get; we'll
	 * set them up later while requesting irq's.
	 */
	err = pci_enable_msix_range(adapter->pdev, adapter->msix_entries,
				    vector_threshold, vectors);
	if (err < 0) {
		dev_err(&adapter->pdev->dev, "Unable to allocate MSI-X interrupts\n");
		kfree(adapter->msix_entries);
		adapter->msix_entries = NULL;
		return err;
	}

	/* Adjust for only the vectors we'll use, which is minimum
	 * of max_msix_q_vectors + NONQ_VECS, or the number of
	 * vectors we were allocated.
	 */
	adapter->num_msix_vectors = err;
	return 0;
}

/**
 * iavf_free_queues - Free memory for all rings
 * @adapter: board private structure to initialize
 *
 * Free all of the memory associated with queue pairs.
 **/
static void iavf_free_queues(struct iavf_adapter *adapter)
{
	if (!adapter->vsi_res)
		return;
	adapter->num_active_queues = 0;
	kfree(adapter->tx_rings);
	adapter->tx_rings = NULL;
	kfree(adapter->rx_rings);
	adapter->rx_rings = NULL;
}

/**
 * iavf_set_queue_vlan_tag_loc - set location for VLAN tag offload
 * @adapter: board private structure
 *
 * Based on negotiated capabilities, the VLAN tag needs to be inserted and/or
 * stripped in certain descriptor fields. Instead of checking the offload
 * capability bits in the hot path, cache the location the ring specific
 * flags.
 */
void iavf_set_queue_vlan_tag_loc(struct iavf_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_active_queues; i++) {
		struct iavf_ring *tx_ring = &adapter->tx_rings[i];
		struct iavf_ring *rx_ring = &adapter->rx_rings[i];

		/* prevent multiple L2TAG bits being set after VFR */
		tx_ring->flags &=
			~(IAVF_TXRX_FLAGS_VLAN_TAG_LOC_L2TAG1 |
			  IAVF_TXR_FLAGS_VLAN_TAG_LOC_L2TAG2);
		rx_ring->flags &=
			~(IAVF_TXRX_FLAGS_VLAN_TAG_LOC_L2TAG1 |
			  IAVF_RXR_FLAGS_VLAN_TAG_LOC_L2TAG2_2);

		if (VLAN_ALLOWED(adapter)) {
			tx_ring->flags |= IAVF_TXRX_FLAGS_VLAN_TAG_LOC_L2TAG1;
			rx_ring->flags |= IAVF_TXRX_FLAGS_VLAN_TAG_LOC_L2TAG1;
		} else if (VLAN_V2_ALLOWED(adapter)) {
			struct virtchnl_vlan_supported_caps *stripping_support;
			struct virtchnl_vlan_supported_caps *insertion_support;

			stripping_support =
				&adapter->vlan_v2_caps.offloads.stripping_support;
			insertion_support =
				&adapter->vlan_v2_caps.offloads.insertion_support;

			if (stripping_support->outer) {
				if (stripping_support->outer &
				    VIRTCHNL_VLAN_TAG_LOCATION_L2TAG1)
					rx_ring->flags |=
						IAVF_TXRX_FLAGS_VLAN_TAG_LOC_L2TAG1;
				else if (stripping_support->outer &
					 VIRTCHNL_VLAN_TAG_LOCATION_L2TAG2_2)
					rx_ring->flags |=
						IAVF_RXR_FLAGS_VLAN_TAG_LOC_L2TAG2_2;
			} else if (stripping_support->inner) {
				if (stripping_support->inner &
				    VIRTCHNL_VLAN_TAG_LOCATION_L2TAG1)
					rx_ring->flags |=
						IAVF_TXRX_FLAGS_VLAN_TAG_LOC_L2TAG1;
				else if (stripping_support->inner &
					 VIRTCHNL_VLAN_TAG_LOCATION_L2TAG2_2)
					rx_ring->flags |=
						IAVF_RXR_FLAGS_VLAN_TAG_LOC_L2TAG2_2;
			}

			if (insertion_support->outer) {
				if (insertion_support->outer &
				    VIRTCHNL_VLAN_TAG_LOCATION_L2TAG1)
					tx_ring->flags |=
						IAVF_TXRX_FLAGS_VLAN_TAG_LOC_L2TAG1;
				else if (insertion_support->outer &
					 VIRTCHNL_VLAN_TAG_LOCATION_L2TAG2)
					tx_ring->flags |=
						IAVF_TXR_FLAGS_VLAN_TAG_LOC_L2TAG2;
			} else if (insertion_support->inner) {
				if (insertion_support->inner &
				    VIRTCHNL_VLAN_TAG_LOCATION_L2TAG1)
					tx_ring->flags |=
						IAVF_TXRX_FLAGS_VLAN_TAG_LOC_L2TAG1;
				else if (insertion_support->inner &
					 VIRTCHNL_VLAN_TAG_LOCATION_L2TAG2)
					tx_ring->flags |=
						IAVF_TXR_FLAGS_VLAN_TAG_LOC_L2TAG2;
			}
		}
	}
}

/**
 * iavf_alloc_queues - Allocate memory for all rings
 * @adapter: board private structure to initialize
 *
 * We allocate one ring per queue at run-time since we don't know the
 * number of queues at compile-time.  The polling_netdev array is
 * intended for Multiqueue, but should work fine with a single queue.
 **/
static int iavf_alloc_queues(struct iavf_adapter *adapter)
{
	int i, num_active_queues;

	/* If we're in reset reallocating queues we don't actually know yet for
	 * certain the PF gave us the number of queues we asked for but we'll
	 * assume it did.  Once basic reset is finished we'll confirm once we
	 * start negotiating config with PF.
	 */
	if (adapter->num_req_queues)
		num_active_queues = adapter->num_req_queues;
	else if ((adapter->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ADQ) &&
		 adapter->num_tc)
		num_active_queues = adapter->ch_config.total_qps;
	else
		num_active_queues = min_t(int,
					  adapter->vsi_res->num_queue_pairs,
					  (int)(num_online_cpus()));


	adapter->tx_rings = kcalloc(num_active_queues,
				    sizeof(struct iavf_ring), GFP_KERNEL);
	if (!adapter->tx_rings)
		goto err_out;
	adapter->rx_rings = kcalloc(num_active_queues,
				    sizeof(struct iavf_ring), GFP_KERNEL);
	if (!adapter->rx_rings)
		goto err_out;

	for (i = 0; i < num_active_queues; i++) {
		struct iavf_ring *tx_ring;
		struct iavf_ring *rx_ring;

		tx_ring = &adapter->tx_rings[i];

		tx_ring->queue_index = i;
		tx_ring->netdev = adapter->netdev;
		tx_ring->dev = &adapter->pdev->dev;
		tx_ring->count = adapter->tx_desc_count;
		tx_ring->itr_setting = IAVF_ITR_TX_DEF;
		if (adapter->flags & IAVF_FLAG_WB_ON_ITR_CAPABLE)
			tx_ring->flags |= IAVF_TXR_FLAGS_WB_ON_ITR;

		rx_ring = &adapter->rx_rings[i];
		rx_ring->queue_index = i;
		rx_ring->netdev = adapter->netdev;
		rx_ring->count = adapter->rx_desc_count;
		rx_ring->itr_setting = IAVF_ITR_RX_DEF;
	}

	adapter->num_active_queues = num_active_queues;

	iavf_set_queue_vlan_tag_loc(adapter);

	return 0;

err_out:
	iavf_free_queues(adapter);
	return -ENOMEM;
}

/**
 * iavf_set_interrupt_capability - set MSI-X or FAIL if not supported
 * @adapter: board private structure to initialize
 *
 * Attempt to configure the interrupts using the best available
 * capabilities of the hardware and the kernel.
 **/
static int iavf_set_interrupt_capability(struct iavf_adapter *adapter)
{
	int vector, v_budget;
	int pairs = 0;
	int err = 0;

	if (!adapter->vsi_res) {
		err = -EIO;
		goto out;
	}
	pairs = adapter->num_active_queues;

	/* It's easy to be greedy for MSI-X vectors, but it really doesn't do
	 * us much good if we have more vectors than CPUs. However, we already
	 * limit the total number of queues by the number of CPUs so we do not
	 * need any further limiting here.
	 */
	v_budget = min_t(int, pairs + NONQ_VECS,
			 (int)adapter->vf_res->max_vectors);

	adapter->msix_entries = kcalloc(v_budget,
					sizeof(struct msix_entry), GFP_KERNEL);
	if (!adapter->msix_entries) {
		err = -ENOMEM;
		goto out;
	}

	for (vector = 0; vector < v_budget; vector++)
		adapter->msix_entries[vector].entry = vector;

	err = iavf_acquire_msix_vectors(adapter, v_budget);
	if (!err)
		iavf_schedule_finish_config(adapter);

out:
	return err;
}

/**
 * iavf_config_rss_aq - Configure RSS keys and lut by using AQ commands
 * @adapter: board private structure
 *
 * Return 0 on success, negative on failure
 **/
static int iavf_config_rss_aq(struct iavf_adapter *adapter)
{
	struct iavf_aqc_get_set_rss_key_data *rss_key =
		(struct iavf_aqc_get_set_rss_key_data *)adapter->rss_key;
	struct iavf_hw *hw = &adapter->hw;
	enum iavf_status status;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot configure RSS, command %d pending\n",
			adapter->current_op);
		return -EBUSY;
	}

	status = iavf_aq_set_rss_key(hw, adapter->vsi.id, rss_key);
	if (status) {
		dev_err(&adapter->pdev->dev, "Cannot set RSS key, err %s aq_err %s\n",
			iavf_stat_str(hw, status),
			libie_aq_str(hw->aq.asq_last_status));
		return iavf_status_to_errno(status);

	}

	status = iavf_aq_set_rss_lut(hw, adapter->vsi.id, false,
				     adapter->rss_lut, adapter->rss_lut_size);
	if (status) {
		dev_err(&adapter->pdev->dev, "Cannot set RSS lut, err %s aq_err %s\n",
			iavf_stat_str(hw, status),
			libie_aq_str(hw->aq.asq_last_status));
		return iavf_status_to_errno(status);
	}

	return 0;

}

/**
 * iavf_config_rss_reg - Configure RSS keys and lut by writing registers
 * @adapter: board private structure
 *
 * Returns 0 on success, negative on failure
 **/
static int iavf_config_rss_reg(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;
	u32 *dw;
	u16 i;

	dw = (u32 *)adapter->rss_key;
	for (i = 0; i <= adapter->rss_key_size / 4; i++)
		wr32(hw, IAVF_VFQF_HKEY(i), dw[i]);

	dw = (u32 *)adapter->rss_lut;
	for (i = 0; i <= adapter->rss_lut_size / 4; i++)
		wr32(hw, IAVF_VFQF_HLUT(i), dw[i]);

	iavf_flush(hw);

	return 0;
}

/**
 * iavf_config_rss - Configure RSS keys and lut
 * @adapter: board private structure
 *
 * Returns 0 on success, negative on failure
 **/
int iavf_config_rss(struct iavf_adapter *adapter)
{

	if (RSS_PF(adapter)) {
		adapter->aq_required |= IAVF_FLAG_AQ_SET_RSS_LUT |
					IAVF_FLAG_AQ_SET_RSS_KEY;
		return 0;
	} else if (RSS_AQ(adapter)) {
		return iavf_config_rss_aq(adapter);
	} else {
		return iavf_config_rss_reg(adapter);
	}
}

/**
 * iavf_fill_rss_lut - Fill the lut with default values
 * @adapter: board private structure
 **/
static void iavf_fill_rss_lut(struct iavf_adapter *adapter)
{
	u16 i;

	for (i = 0; i < adapter->rss_lut_size; i++)
		adapter->rss_lut[i] = i % adapter->num_active_queues;
}

/**
 * iavf_init_rss - Prepare for RSS
 * @adapter: board private structure
 *
 * Return 0 on success, negative on failure
 **/
static int iavf_init_rss(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;

	if (!RSS_PF(adapter)) {
		/* Enable PCTYPES for RSS, TCP/UDP with IPv4/IPv6 */
		if (adapter->vf_res->vf_cap_flags &
		    VIRTCHNL_VF_OFFLOAD_RSS_PCTYPE_V2)
			adapter->rss_hashcfg =
				IAVF_DEFAULT_RSS_HASHCFG_EXPANDED;
		else
			adapter->rss_hashcfg = IAVF_DEFAULT_RSS_HASHCFG;

		wr32(hw, IAVF_VFQF_HENA(0), (u32)adapter->rss_hashcfg);
		wr32(hw, IAVF_VFQF_HENA(1), (u32)(adapter->rss_hashcfg >> 32));
	}

	iavf_fill_rss_lut(adapter);
	netdev_rss_key_fill((void *)adapter->rss_key, adapter->rss_key_size);

	return iavf_config_rss(adapter);
}

/**
 * iavf_alloc_q_vectors - Allocate memory for interrupt vectors
 * @adapter: board private structure to initialize
 *
 * We allocate one q_vector per queue interrupt.  If allocation fails we
 * return -ENOMEM.
 **/
static int iavf_alloc_q_vectors(struct iavf_adapter *adapter)
{
	int q_idx = 0, num_q_vectors, irq_num;
	struct iavf_q_vector *q_vector;

	num_q_vectors = adapter->num_msix_vectors - NONQ_VECS;
	adapter->q_vectors = kcalloc(num_q_vectors, sizeof(*q_vector),
				     GFP_KERNEL);
	if (!adapter->q_vectors)
		return -ENOMEM;

	for (q_idx = 0; q_idx < num_q_vectors; q_idx++) {
		irq_num = adapter->msix_entries[q_idx + NONQ_VECS].vector;
		q_vector = &adapter->q_vectors[q_idx];
		q_vector->adapter = adapter;
		q_vector->vsi = &adapter->vsi;
		q_vector->v_idx = q_idx;
		q_vector->reg_idx = q_idx;
		netif_napi_add_config_locked(adapter->netdev, &q_vector->napi,
					     iavf_napi_poll, q_idx);
		netif_napi_set_irq_locked(&q_vector->napi, irq_num);
	}

	return 0;
}

/**
 * iavf_free_q_vectors - Free memory allocated for interrupt vectors
 * @adapter: board private structure to initialize
 *
 * This function frees the memory allocated to the q_vectors.  In addition if
 * NAPI is enabled it will delete any references to the NAPI struct prior
 * to freeing the q_vector.
 **/
static void iavf_free_q_vectors(struct iavf_adapter *adapter)
{
	int q_idx, num_q_vectors;

	if (!adapter->q_vectors)
		return;

	num_q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (q_idx = 0; q_idx < num_q_vectors; q_idx++) {
		struct iavf_q_vector *q_vector = &adapter->q_vectors[q_idx];

		netif_napi_del_locked(&q_vector->napi);
	}
	kfree(adapter->q_vectors);
	adapter->q_vectors = NULL;
}

/**
 * iavf_reset_interrupt_capability - Reset MSIX setup
 * @adapter: board private structure
 *
 **/
static void iavf_reset_interrupt_capability(struct iavf_adapter *adapter)
{
	if (!adapter->msix_entries)
		return;

	pci_disable_msix(adapter->pdev);
	kfree(adapter->msix_entries);
	adapter->msix_entries = NULL;
}

/**
 * iavf_init_interrupt_scheme - Determine if MSIX is supported and init
 * @adapter: board private structure to initialize
 *
 **/
static int iavf_init_interrupt_scheme(struct iavf_adapter *adapter)
{
	int err;

	err = iavf_alloc_queues(adapter);
	if (err) {
		dev_err(&adapter->pdev->dev,
			"Unable to allocate memory for queues\n");
		goto err_alloc_queues;
	}

	err = iavf_set_interrupt_capability(adapter);
	if (err) {
		dev_err(&adapter->pdev->dev,
			"Unable to setup interrupt capabilities\n");
		goto err_set_interrupt;
	}

	err = iavf_alloc_q_vectors(adapter);
	if (err) {
		dev_err(&adapter->pdev->dev,
			"Unable to allocate memory for queue vectors\n");
		goto err_alloc_q_vectors;
	}

	/* If we've made it so far while ADq flag being ON, then we haven't
	 * bailed out anywhere in middle. And ADq isn't just enabled but actual
	 * resources have been allocated in the reset path.
	 * Now we can truly claim that ADq is enabled.
	 */
	if ((adapter->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ADQ) &&
	    adapter->num_tc)
		dev_info(&adapter->pdev->dev, "ADq Enabled, %u TCs created",
			 adapter->num_tc);

	dev_info(&adapter->pdev->dev, "Multiqueue %s: Queue pair count = %u",
		 (adapter->num_active_queues > 1) ? "Enabled" : "Disabled",
		 adapter->num_active_queues);

	return 0;
err_alloc_q_vectors:
	iavf_reset_interrupt_capability(adapter);
err_set_interrupt:
	iavf_free_queues(adapter);
err_alloc_queues:
	return err;
}

/**
 * iavf_free_interrupt_scheme - Undo what iavf_init_interrupt_scheme does
 * @adapter: board private structure
 **/
static void iavf_free_interrupt_scheme(struct iavf_adapter *adapter)
{
	iavf_free_q_vectors(adapter);
	iavf_reset_interrupt_capability(adapter);
	iavf_free_queues(adapter);
}

/**
 * iavf_free_rss - Free memory used by RSS structs
 * @adapter: board private structure
 **/
static void iavf_free_rss(struct iavf_adapter *adapter)
{
	kfree(adapter->rss_key);
	adapter->rss_key = NULL;

	kfree(adapter->rss_lut);
	adapter->rss_lut = NULL;
}

/**
 * iavf_reinit_interrupt_scheme - Reallocate queues and vectors
 * @adapter: board private structure
 * @running: true if adapter->state == __IAVF_RUNNING
 *
 * Returns 0 on success, negative on failure
 **/
static int iavf_reinit_interrupt_scheme(struct iavf_adapter *adapter, bool running)
{
	struct net_device *netdev = adapter->netdev;
	int err;

	if (running)
		iavf_free_traffic_irqs(adapter);
	iavf_free_misc_irq(adapter);
	iavf_free_interrupt_scheme(adapter);

	err = iavf_init_interrupt_scheme(adapter);
	if (err)
		goto err;

	netif_tx_stop_all_queues(netdev);

	err = iavf_request_misc_irq(adapter);
	if (err)
		goto err;

	set_bit(__IAVF_VSI_DOWN, adapter->vsi.state);

	iavf_map_rings_to_vectors(adapter);
err:
	return err;
}

/**
 * iavf_finish_config - do all netdev work that needs RTNL
 * @work: our work_struct
 *
 * Do work that needs RTNL.
 */
static void iavf_finish_config(struct work_struct *work)
{
	struct iavf_adapter *adapter;
	bool netdev_released = false;
	int pairs, err;

	adapter = container_of(work, struct iavf_adapter, finish_config);

	/* Always take RTNL first to prevent circular lock dependency;
	 * the dev->lock (== netdev lock) is needed to update the queue number.
	 */
	rtnl_lock();
	netdev_lock(adapter->netdev);

	if ((adapter->flags & IAVF_FLAG_SETUP_NETDEV_FEATURES) &&
	    adapter->netdev->reg_state == NETREG_REGISTERED &&
	    !test_bit(__IAVF_IN_REMOVE_TASK, &adapter->crit_section)) {
		netdev_update_features(adapter->netdev);
		adapter->flags &= ~IAVF_FLAG_SETUP_NETDEV_FEATURES;
	}

	switch (adapter->state) {
	case __IAVF_DOWN:
		/* Set the real number of queues when reset occurs while
		 * state == __IAVF_DOWN
		 */
		pairs = adapter->num_active_queues;
		netif_set_real_num_rx_queues(adapter->netdev, pairs);
		netif_set_real_num_tx_queues(adapter->netdev, pairs);

		if (adapter->netdev->reg_state != NETREG_REGISTERED) {
			netdev_unlock(adapter->netdev);
			netdev_released = true;
			err = register_netdevice(adapter->netdev);
			if (err) {
				dev_err(&adapter->pdev->dev, "Unable to register netdev (%d)\n",
					err);

				/* go back and try again.*/
				netdev_lock(adapter->netdev);
				iavf_free_rss(adapter);
				iavf_free_misc_irq(adapter);
				iavf_reset_interrupt_capability(adapter);
				iavf_change_state(adapter,
						  __IAVF_INIT_CONFIG_ADAPTER);
				netdev_unlock(adapter->netdev);
				goto out;
			}
		}
		break;
	case __IAVF_RUNNING:
		pairs = adapter->num_active_queues;
		netif_set_real_num_rx_queues(adapter->netdev, pairs);
		netif_set_real_num_tx_queues(adapter->netdev, pairs);
		break;

	default:
		break;
	}

out:
	if (!netdev_released)
		netdev_unlock(adapter->netdev);
	rtnl_unlock();
}

/**
 * iavf_schedule_finish_config - Set the flags and schedule a reset event
 * @adapter: board private structure
 **/
void iavf_schedule_finish_config(struct iavf_adapter *adapter)
{
	if (!test_bit(__IAVF_IN_REMOVE_TASK, &adapter->crit_section))
		queue_work(adapter->wq, &adapter->finish_config);
}

/**
 * iavf_process_aq_command - process aq_required flags
 * and sends aq command
 * @adapter: pointer to iavf adapter structure
 *
 * Returns 0 on success
 * Returns error code if no command was sent
 * or error code if the command failed.
 **/
static int iavf_process_aq_command(struct iavf_adapter *adapter)
{
	if (adapter->aq_required & IAVF_FLAG_AQ_GET_CONFIG)
		return iavf_send_vf_config_msg(adapter);
	if (adapter->aq_required & IAVF_FLAG_AQ_GET_OFFLOAD_VLAN_V2_CAPS)
		return iavf_send_vf_offload_vlan_v2_msg(adapter);
	if (adapter->aq_required & IAVF_FLAG_AQ_GET_SUPPORTED_RXDIDS)
		return iavf_send_vf_supported_rxdids_msg(adapter);
	if (adapter->aq_required & IAVF_FLAG_AQ_GET_PTP_CAPS)
		return iavf_send_vf_ptp_caps_msg(adapter);
	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_QUEUES) {
		iavf_disable_queues(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_MAP_VECTORS) {
		iavf_map_queues(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_ADD_MAC_FILTER) {
		iavf_add_ether_addrs(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_ADD_VLAN_FILTER) {
		iavf_add_vlans(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_DEL_MAC_FILTER) {
		iavf_del_ether_addrs(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_DEL_VLAN_FILTER) {
		iavf_del_vlans(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_VLAN_STRIPPING) {
		iavf_enable_vlan_stripping(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_VLAN_STRIPPING) {
		iavf_disable_vlan_stripping(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_CONFIGURE_QUEUES_BW) {
		iavf_cfg_queues_bw(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_GET_QOS_CAPS) {
		iavf_get_qos_caps(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_CFG_QUEUES_QUANTA_SIZE) {
		iavf_cfg_queues_quanta_size(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_CONFIGURE_QUEUES) {
		iavf_configure_queues(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_QUEUES) {
		iavf_enable_queues(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_CONFIGURE_RSS) {
		/* This message goes straight to the firmware, not the
		 * PF, so we don't have to set current_op as we will
		 * not get a response through the ARQ.
		 */
		adapter->aq_required &= ~IAVF_FLAG_AQ_CONFIGURE_RSS;
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_GET_RSS_HASHCFG) {
		iavf_get_rss_hashcfg(adapter);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_SET_RSS_HASHCFG) {
		iavf_set_rss_hashcfg(adapter);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_SET_RSS_KEY) {
		iavf_set_rss_key(adapter);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_SET_RSS_LUT) {
		iavf_set_rss_lut(adapter);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_SET_RSS_HFUNC) {
		iavf_set_rss_hfunc(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_CONFIGURE_PROMISC_MODE) {
		iavf_set_promiscuous(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_CHANNELS) {
		iavf_enable_channels(adapter);
		return 0;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_CHANNELS) {
		iavf_disable_channels(adapter);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ADD_CLOUD_FILTER) {
		iavf_add_cloud_filter(adapter);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DEL_CLOUD_FILTER) {
		iavf_del_cloud_filter(adapter);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ADD_FDIR_FILTER) {
		iavf_add_fdir_filter(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DEL_FDIR_FILTER) {
		iavf_del_fdir_filter(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ADD_ADV_RSS_CFG) {
		iavf_add_adv_rss_cfg(adapter);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DEL_ADV_RSS_CFG) {
		iavf_del_adv_rss_cfg(adapter);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_CTAG_VLAN_STRIPPING) {
		iavf_disable_vlan_stripping_v2(adapter, ETH_P_8021Q);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_STAG_VLAN_STRIPPING) {
		iavf_disable_vlan_stripping_v2(adapter, ETH_P_8021AD);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_CTAG_VLAN_STRIPPING) {
		iavf_enable_vlan_stripping_v2(adapter, ETH_P_8021Q);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_STAG_VLAN_STRIPPING) {
		iavf_enable_vlan_stripping_v2(adapter, ETH_P_8021AD);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_CTAG_VLAN_INSERTION) {
		iavf_disable_vlan_insertion_v2(adapter, ETH_P_8021Q);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_STAG_VLAN_INSERTION) {
		iavf_disable_vlan_insertion_v2(adapter, ETH_P_8021AD);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_CTAG_VLAN_INSERTION) {
		iavf_enable_vlan_insertion_v2(adapter, ETH_P_8021Q);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_STAG_VLAN_INSERTION) {
		iavf_enable_vlan_insertion_v2(adapter, ETH_P_8021AD);
		return 0;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_SEND_PTP_CMD) {
		iavf_virtchnl_send_ptp_cmd(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_REQUEST_STATS) {
		iavf_request_stats(adapter);
		return 0;
	}

	return -EAGAIN;
}

/**
 * iavf_set_vlan_offload_features - set VLAN offload configuration
 * @adapter: board private structure
 * @prev_features: previous features used for comparison
 * @features: updated features used for configuration
 *
 * Set the aq_required bit(s) based on the requested features passed in to
 * configure VLAN stripping and/or VLAN insertion if supported. Also, schedule
 * the watchdog if any changes are requested to expedite the request via
 * virtchnl.
 **/
static void
iavf_set_vlan_offload_features(struct iavf_adapter *adapter,
			       netdev_features_t prev_features,
			       netdev_features_t features)
{
	bool enable_stripping = true, enable_insertion = true;
	u16 vlan_ethertype = 0;
	u64 aq_required = 0;

	/* keep cases separate because one ethertype for offloads can be
	 * disabled at the same time as another is disabled, so check for an
	 * enabled ethertype first, then check for disabled. Default to
	 * ETH_P_8021Q so an ethertype is specified if disabling insertion and
	 * stripping.
	 */
	if (features & (NETIF_F_HW_VLAN_STAG_RX | NETIF_F_HW_VLAN_STAG_TX))
		vlan_ethertype = ETH_P_8021AD;
	else if (features & (NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX))
		vlan_ethertype = ETH_P_8021Q;
	else if (prev_features & (NETIF_F_HW_VLAN_STAG_RX | NETIF_F_HW_VLAN_STAG_TX))
		vlan_ethertype = ETH_P_8021AD;
	else if (prev_features & (NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX))
		vlan_ethertype = ETH_P_8021Q;
	else
		vlan_ethertype = ETH_P_8021Q;

	if (!(features & (NETIF_F_HW_VLAN_STAG_RX | NETIF_F_HW_VLAN_CTAG_RX)))
		enable_stripping = false;
	if (!(features & (NETIF_F_HW_VLAN_STAG_TX | NETIF_F_HW_VLAN_CTAG_TX)))
		enable_insertion = false;

	if (VLAN_ALLOWED(adapter)) {
		/* VIRTCHNL_VF_OFFLOAD_VLAN only has support for toggling VLAN
		 * stripping via virtchnl. VLAN insertion can be toggled on the
		 * netdev, but it doesn't require a virtchnl message
		 */
		if (enable_stripping)
			aq_required |= IAVF_FLAG_AQ_ENABLE_VLAN_STRIPPING;
		else
			aq_required |= IAVF_FLAG_AQ_DISABLE_VLAN_STRIPPING;

	} else if (VLAN_V2_ALLOWED(adapter)) {
		switch (vlan_ethertype) {
		case ETH_P_8021Q:
			if (enable_stripping)
				aq_required |= IAVF_FLAG_AQ_ENABLE_CTAG_VLAN_STRIPPING;
			else
				aq_required |= IAVF_FLAG_AQ_DISABLE_CTAG_VLAN_STRIPPING;

			if (enable_insertion)
				aq_required |= IAVF_FLAG_AQ_ENABLE_CTAG_VLAN_INSERTION;
			else
				aq_required |= IAVF_FLAG_AQ_DISABLE_CTAG_VLAN_INSERTION;
			break;
		case ETH_P_8021AD:
			if (enable_stripping)
				aq_required |= IAVF_FLAG_AQ_ENABLE_STAG_VLAN_STRIPPING;
			else
				aq_required |= IAVF_FLAG_AQ_DISABLE_STAG_VLAN_STRIPPING;

			if (enable_insertion)
				aq_required |= IAVF_FLAG_AQ_ENABLE_STAG_VLAN_INSERTION;
			else
				aq_required |= IAVF_FLAG_AQ_DISABLE_STAG_VLAN_INSERTION;
			break;
		}
	}

	if (aq_required)
		iavf_schedule_aq_request(adapter, aq_required);
}

/**
 * iavf_startup - first step of driver startup
 * @adapter: board private structure
 *
 * Function process __IAVF_STARTUP driver state.
 * When success the state is changed to __IAVF_INIT_VERSION_CHECK
 * when fails the state is changed to __IAVF_INIT_FAILED
 **/
static void iavf_startup(struct iavf_adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;
	struct iavf_hw *hw = &adapter->hw;
	enum iavf_status status;
	int ret;

	WARN_ON(adapter->state != __IAVF_STARTUP);

	/* driver loaded, probe complete */
	adapter->flags &= ~IAVF_FLAG_PF_COMMS_FAILED;
	adapter->flags &= ~IAVF_FLAG_RESET_PENDING;

	ret = iavf_check_reset_complete(hw);
	if (ret) {
		dev_info(&pdev->dev, "Device is still in reset (%d), retrying\n",
			 ret);
		goto err;
	}
	hw->aq.num_arq_entries = IAVF_AQ_LEN;
	hw->aq.num_asq_entries = IAVF_AQ_LEN;
	hw->aq.arq_buf_size = IAVF_MAX_AQ_BUF_SIZE;
	hw->aq.asq_buf_size = IAVF_MAX_AQ_BUF_SIZE;

	status = iavf_init_adminq(hw);
	if (status) {
		dev_err(&pdev->dev, "Failed to init Admin Queue (%d)\n",
			status);
		goto err;
	}
	ret = iavf_send_api_ver(adapter);
	if (ret) {
		dev_err(&pdev->dev, "Unable to send to PF (%d)\n", ret);
		iavf_shutdown_adminq(hw);
		goto err;
	}
	iavf_change_state(adapter, __IAVF_INIT_VERSION_CHECK);
	return;
err:
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

/**
 * iavf_init_version_check - second step of driver startup
 * @adapter: board private structure
 *
 * Function process __IAVF_INIT_VERSION_CHECK driver state.
 * When success the state is changed to __IAVF_INIT_GET_RESOURCES
 * when fails the state is changed to __IAVF_INIT_FAILED
 **/
static void iavf_init_version_check(struct iavf_adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;
	struct iavf_hw *hw = &adapter->hw;
	int err = -EAGAIN;

	WARN_ON(adapter->state != __IAVF_INIT_VERSION_CHECK);

	if (!iavf_asq_done(hw)) {
		dev_err(&pdev->dev, "Admin queue command never completed\n");
		iavf_shutdown_adminq(hw);
		iavf_change_state(adapter, __IAVF_STARTUP);
		goto err;
	}

	/* aq msg sent, awaiting reply */
	err = iavf_verify_api_ver(adapter);
	if (err) {
		if (err == -EALREADY)
			err = iavf_send_api_ver(adapter);
		else
			dev_err(&pdev->dev, "Unsupported PF API version %d.%d, expected %d.%d\n",
				adapter->pf_version.major,
				adapter->pf_version.minor,
				VIRTCHNL_VERSION_MAJOR,
				VIRTCHNL_VERSION_MINOR);
		goto err;
	}
	err = iavf_send_vf_config_msg(adapter);
	if (err) {
		dev_err(&pdev->dev, "Unable to send config request (%d)\n",
			err);
		goto err;
	}
	iavf_change_state(adapter, __IAVF_INIT_GET_RESOURCES);
	return;
err:
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

/**
 * iavf_parse_vf_resource_msg - parse response from VIRTCHNL_OP_GET_VF_RESOURCES
 * @adapter: board private structure
 */
int iavf_parse_vf_resource_msg(struct iavf_adapter *adapter)
{
	int i, num_req_queues = adapter->num_req_queues;
	struct iavf_vsi *vsi = &adapter->vsi;

	for (i = 0; i < adapter->vf_res->num_vsis; i++) {
		if (adapter->vf_res->vsi_res[i].vsi_type == VIRTCHNL_VSI_SRIOV)
			adapter->vsi_res = &adapter->vf_res->vsi_res[i];
	}
	if (!adapter->vsi_res) {
		dev_err(&adapter->pdev->dev, "No LAN VSI found\n");
		return -ENODEV;
	}

	if (num_req_queues &&
	    num_req_queues > adapter->vsi_res->num_queue_pairs) {
		/* Problem.  The PF gave us fewer queues than what we had
		 * negotiated in our request.  Need a reset to see if we can't
		 * get back to a working state.
		 */
		dev_err(&adapter->pdev->dev,
			"Requested %d queues, but PF only gave us %d.\n",
			num_req_queues,
			adapter->vsi_res->num_queue_pairs);
		adapter->flags |= IAVF_FLAG_REINIT_MSIX_NEEDED;
		adapter->num_req_queues = adapter->vsi_res->num_queue_pairs;
		iavf_schedule_reset(adapter, IAVF_FLAG_RESET_NEEDED);

		return -EAGAIN;
	}
	adapter->num_req_queues = 0;
	adapter->vsi.id = adapter->vsi_res->vsi_id;

	adapter->vsi.back = adapter;
	adapter->vsi.base_vector = 1;
	vsi->netdev = adapter->netdev;
	vsi->qs_handle = adapter->vsi_res->qset_handle;
	if (adapter->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RSS_PF) {
		adapter->rss_key_size = adapter->vf_res->rss_key_size;
		adapter->rss_lut_size = adapter->vf_res->rss_lut_size;
	} else {
		adapter->rss_key_size = IAVF_HKEY_ARRAY_SIZE;
		adapter->rss_lut_size = IAVF_HLUT_ARRAY_SIZE;
	}

	return 0;
}

/**
 * iavf_init_get_resources - third step of driver startup
 * @adapter: board private structure
 *
 * Function process __IAVF_INIT_GET_RESOURCES driver state and
 * finishes driver initialization procedure.
 * When success the state is changed to __IAVF_DOWN
 * when fails the state is changed to __IAVF_INIT_FAILED
 **/
static void iavf_init_get_resources(struct iavf_adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;
	struct iavf_hw *hw = &adapter->hw;
	int err;

	WARN_ON(adapter->state != __IAVF_INIT_GET_RESOURCES);
	/* aq msg sent, awaiting reply */
	if (!adapter->vf_res) {
		adapter->vf_res = kzalloc(IAVF_VIRTCHNL_VF_RESOURCE_SIZE,
					  GFP_KERNEL);
		if (!adapter->vf_res) {
			err = -ENOMEM;
			goto err;
		}
	}
	err = iavf_get_vf_config(adapter);
	if (err == -EALREADY) {
		err = iavf_send_vf_config_msg(adapter);
		goto err;
	} else if (err == -EINVAL) {
		/* We only get -EINVAL if the device is in a very bad
		 * state or if we've been disabled for previous bad
		 * behavior. Either way, we're done now.
		 */
		iavf_shutdown_adminq(hw);
		dev_err(&pdev->dev, "Unable to get VF config due to PF error condition, not retrying\n");
		return;
	}
	if (err) {
		dev_err(&pdev->dev, "Unable to get VF config (%d)\n", err);
		goto err_alloc;
	}

	err = iavf_parse_vf_resource_msg(adapter);
	if (err) {
		dev_err(&pdev->dev, "Failed to parse VF resource message from PF (%d)\n",
			err);
		goto err_alloc;
	}
	/* Some features require additional messages to negotiate extended
	 * capabilities. These are processed in sequence by the
	 * __IAVF_INIT_EXTENDED_CAPS driver state.
	 */
	adapter->extended_caps = IAVF_EXTENDED_CAPS;

	iavf_change_state(adapter, __IAVF_INIT_EXTENDED_CAPS);
	return;

err_alloc:
	kfree(adapter->vf_res);
	adapter->vf_res = NULL;
err:
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

/**
 * iavf_init_send_offload_vlan_v2_caps - part of initializing VLAN V2 caps
 * @adapter: board private structure
 *
 * Function processes send of the extended VLAN V2 capability message to the
 * PF. Must clear IAVF_EXTENDED_CAP_RECV_VLAN_V2 if the message is not sent,
 * e.g. due to PF not negotiating VIRTCHNL_VF_OFFLOAD_VLAN_V2.
 */
static void iavf_init_send_offload_vlan_v2_caps(struct iavf_adapter *adapter)
{
	int ret;

	WARN_ON(!(adapter->extended_caps & IAVF_EXTENDED_CAP_SEND_VLAN_V2));

	ret = iavf_send_vf_offload_vlan_v2_msg(adapter);
	if (ret && ret == -EOPNOTSUPP) {
		/* PF does not support VIRTCHNL_VF_OFFLOAD_V2. In this case,
		 * we did not send the capability exchange message and do not
		 * expect a response.
		 */
		adapter->extended_caps &= ~IAVF_EXTENDED_CAP_RECV_VLAN_V2;
	}

	/* We sent the message, so move on to the next step */
	adapter->extended_caps &= ~IAVF_EXTENDED_CAP_SEND_VLAN_V2;
}

/**
 * iavf_init_recv_offload_vlan_v2_caps - part of initializing VLAN V2 caps
 * @adapter: board private structure
 *
 * Function processes receipt of the extended VLAN V2 capability message from
 * the PF.
 **/
static void iavf_init_recv_offload_vlan_v2_caps(struct iavf_adapter *adapter)
{
	int ret;

	WARN_ON(!(adapter->extended_caps & IAVF_EXTENDED_CAP_RECV_VLAN_V2));

	memset(&adapter->vlan_v2_caps, 0, sizeof(adapter->vlan_v2_caps));

	ret = iavf_get_vf_vlan_v2_caps(adapter);
	if (ret)
		goto err;

	/* We've processed receipt of the VLAN V2 caps message */
	adapter->extended_caps &= ~IAVF_EXTENDED_CAP_RECV_VLAN_V2;
	return;
err:
	/* We didn't receive a reply. Make sure we try sending again when
	 * __IAVF_INIT_FAILED attempts to recover.
	 */
	adapter->extended_caps |= IAVF_EXTENDED_CAP_SEND_VLAN_V2;
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

/**
 * iavf_init_send_supported_rxdids - part of querying for supported RXDID
 * formats
 * @adapter: board private structure
 *
 * Function processes send of the request for supported RXDIDs to the PF.
 * Must clear IAVF_EXTENDED_CAP_RECV_RXDID if the message is not sent, e.g.
 * due to the PF not negotiating VIRTCHNL_VF_OFFLOAD_RX_FLEX_DESC.
 */
static void iavf_init_send_supported_rxdids(struct iavf_adapter *adapter)
{
	int ret;

	ret = iavf_send_vf_supported_rxdids_msg(adapter);
	if (ret == -EOPNOTSUPP) {
		/* PF does not support VIRTCHNL_VF_OFFLOAD_RX_FLEX_DESC. In this
		 * case, we did not send the capability exchange message and
		 * do not expect a response.
		 */
		adapter->extended_caps &= ~IAVF_EXTENDED_CAP_RECV_RXDID;
	}

	/* We sent the message, so move on to the next step */
	adapter->extended_caps &= ~IAVF_EXTENDED_CAP_SEND_RXDID;
}

/**
 * iavf_init_recv_supported_rxdids - part of querying for supported RXDID
 * formats
 * @adapter: board private structure
 *
 * Function processes receipt of the supported RXDIDs message from the PF.
 **/
static void iavf_init_recv_supported_rxdids(struct iavf_adapter *adapter)
{
	int ret;

	memset(&adapter->supp_rxdids, 0, sizeof(adapter->supp_rxdids));

	ret = iavf_get_vf_supported_rxdids(adapter);
	if (ret)
		goto err;

	/* We've processed the PF response to the
	 * VIRTCHNL_OP_GET_SUPPORTED_RXDIDS message we sent previously.
	 */
	adapter->extended_caps &= ~IAVF_EXTENDED_CAP_RECV_RXDID;
	return;

err:
	/* We didn't receive a reply. Make sure we try sending again when
	 * __IAVF_INIT_FAILED attempts to recover.
	 */
	adapter->extended_caps |= IAVF_EXTENDED_CAP_SEND_RXDID;
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

/**
 * iavf_init_send_ptp_caps - part of querying for extended PTP capabilities
 * @adapter: board private structure
 *
 * Function processes send of the request for 1588 PTP capabilities to the PF.
 * Must clear IAVF_EXTENDED_CAP_SEND_PTP if the message is not sent, e.g.
 * due to the PF not negotiating VIRTCHNL_VF_PTP_CAP
 */
static void iavf_init_send_ptp_caps(struct iavf_adapter *adapter)
{
	if (iavf_send_vf_ptp_caps_msg(adapter) == -EOPNOTSUPP) {
		/* PF does not support VIRTCHNL_VF_PTP_CAP. In this case, we
		 * did not send the capability exchange message and do not
		 * expect a response.
		 */
		adapter->extended_caps &= ~IAVF_EXTENDED_CAP_RECV_PTP;
	}

	/* We sent the message, so move on to the next step */
	adapter->extended_caps &= ~IAVF_EXTENDED_CAP_SEND_PTP;
}

/**
 * iavf_init_recv_ptp_caps - part of querying for supported PTP capabilities
 * @adapter: board private structure
 *
 * Function processes receipt of the PTP capabilities supported on this VF.
 **/
static void iavf_init_recv_ptp_caps(struct iavf_adapter *adapter)
{
	memset(&adapter->ptp.hw_caps, 0, sizeof(adapter->ptp.hw_caps));

	if (iavf_get_vf_ptp_caps(adapter))
		goto err;

	/* We've processed the PF response to the VIRTCHNL_OP_1588_PTP_GET_CAPS
	 * message we sent previously.
	 */
	adapter->extended_caps &= ~IAVF_EXTENDED_CAP_RECV_PTP;
	return;

err:
	/* We didn't receive a reply. Make sure we try sending again when
	 * __IAVF_INIT_FAILED attempts to recover.
	 */
	adapter->extended_caps |= IAVF_EXTENDED_CAP_SEND_PTP;
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

/**
 * iavf_init_process_extended_caps - Part of driver startup
 * @adapter: board private structure
 *
 * Function processes __IAVF_INIT_EXTENDED_CAPS driver state. This state
 * handles negotiating capabilities for features which require an additional
 * message.
 *
 * Once all extended capabilities exchanges are finished, the driver will
 * transition into __IAVF_INIT_CONFIG_ADAPTER.
 */
static void iavf_init_process_extended_caps(struct iavf_adapter *adapter)
{
	WARN_ON(adapter->state != __IAVF_INIT_EXTENDED_CAPS);

	/* Process capability exchange for VLAN V2 */
	if (adapter->extended_caps & IAVF_EXTENDED_CAP_SEND_VLAN_V2) {
		iavf_init_send_offload_vlan_v2_caps(adapter);
		return;
	} else if (adapter->extended_caps & IAVF_EXTENDED_CAP_RECV_VLAN_V2) {
		iavf_init_recv_offload_vlan_v2_caps(adapter);
		return;
	}

	/* Process capability exchange for RXDID formats */
	if (adapter->extended_caps & IAVF_EXTENDED_CAP_SEND_RXDID) {
		iavf_init_send_supported_rxdids(adapter);
		return;
	} else if (adapter->extended_caps & IAVF_EXTENDED_CAP_RECV_RXDID) {
		iavf_init_recv_supported_rxdids(adapter);
		return;
	}

	/* Process capability exchange for PTP features */
	if (adapter->extended_caps & IAVF_EXTENDED_CAP_SEND_PTP) {
		iavf_init_send_ptp_caps(adapter);
		return;
	} else if (adapter->extended_caps & IAVF_EXTENDED_CAP_RECV_PTP) {
		iavf_init_recv_ptp_caps(adapter);
		return;
	}

	/* When we reach here, no further extended capabilities exchanges are
	 * necessary, so we finally transition into __IAVF_INIT_CONFIG_ADAPTER
	 */
	iavf_change_state(adapter, __IAVF_INIT_CONFIG_ADAPTER);
}

/**
 * iavf_init_config_adapter - last part of driver startup
 * @adapter: board private structure
 *
 * After all the supported capabilities are negotiated, then the
 * __IAVF_INIT_CONFIG_ADAPTER state will finish driver initialization.
 */
static void iavf_init_config_adapter(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct pci_dev *pdev = adapter->pdev;
	int err;

	WARN_ON(adapter->state != __IAVF_INIT_CONFIG_ADAPTER);

	if (iavf_process_config(adapter))
		goto err;

	adapter->current_op = VIRTCHNL_OP_UNKNOWN;

	adapter->flags |= IAVF_FLAG_RX_CSUM_ENABLED;

	netdev->netdev_ops = &iavf_netdev_ops;
	iavf_set_ethtool_ops(netdev);
	netdev->watchdog_timeo = 5 * HZ;

	netdev->min_mtu = ETH_MIN_MTU;
	netdev->max_mtu = LIBIE_MAX_MTU;

	if (!is_valid_ether_addr(adapter->hw.mac.addr)) {
		dev_info(&pdev->dev, "Invalid MAC address %pM, using random\n",
			 adapter->hw.mac.addr);
		eth_hw_addr_random(netdev);
		ether_addr_copy(adapter->hw.mac.addr, netdev->dev_addr);
	} else {
		eth_hw_addr_set(netdev, adapter->hw.mac.addr);
		ether_addr_copy(netdev->perm_addr, adapter->hw.mac.addr);
	}

	adapter->tx_desc_count = IAVF_DEFAULT_TXD;
	adapter->rx_desc_count = IAVF_DEFAULT_RXD;
	err = iavf_init_interrupt_scheme(adapter);
	if (err)
		goto err_sw_init;
	iavf_map_rings_to_vectors(adapter);
	if (adapter->vf_res->vf_cap_flags &
		VIRTCHNL_VF_OFFLOAD_WB_ON_ITR)
		adapter->flags |= IAVF_FLAG_WB_ON_ITR_CAPABLE;

	err = iavf_request_misc_irq(adapter);
	if (err)
		goto err_sw_init;

	netif_carrier_off(netdev);
	adapter->link_up = false;
	netif_tx_stop_all_queues(netdev);

	dev_info(&pdev->dev, "MAC address: %pM\n", adapter->hw.mac.addr);
	if (netdev->features & NETIF_F_GRO)
		dev_info(&pdev->dev, "GRO is enabled\n");

	iavf_change_state(adapter, __IAVF_DOWN);
	set_bit(__IAVF_VSI_DOWN, adapter->vsi.state);

	iavf_misc_irq_enable(adapter);
	wake_up(&adapter->down_waitqueue);

	adapter->rss_key = kzalloc(adapter->rss_key_size, GFP_KERNEL);
	adapter->rss_lut = kzalloc(adapter->rss_lut_size, GFP_KERNEL);
	if (!adapter->rss_key || !adapter->rss_lut) {
		err = -ENOMEM;
		goto err_mem;
	}
	if (RSS_AQ(adapter))
		adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_RSS;
	else
		iavf_init_rss(adapter);

	if (VLAN_V2_ALLOWED(adapter))
		/* request initial VLAN offload settings */
		iavf_set_vlan_offload_features(adapter, 0, netdev->features);

	if (QOS_ALLOWED(adapter))
		adapter->aq_required |= IAVF_FLAG_AQ_GET_QOS_CAPS;

	/* Setup initial PTP configuration */
	iavf_ptp_init(adapter);

	iavf_schedule_finish_config(adapter);
	return;

err_mem:
	iavf_free_rss(adapter);
	iavf_free_misc_irq(adapter);
err_sw_init:
	iavf_reset_interrupt_capability(adapter);
err:
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

static const int IAVF_NO_RESCHED = -1;

/* return: msec delay for requeueing itself */
static int iavf_watchdog_step(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;
	u32 reg_val;

	netdev_assert_locked(adapter->netdev);

	if (adapter->flags & IAVF_FLAG_PF_COMMS_FAILED)
		iavf_change_state(adapter, __IAVF_COMM_FAILED);

	switch (adapter->state) {
	case __IAVF_STARTUP:
		iavf_startup(adapter);
		return 30;
	case __IAVF_INIT_VERSION_CHECK:
		iavf_init_version_check(adapter);
		return 30;
	case __IAVF_INIT_GET_RESOURCES:
		iavf_init_get_resources(adapter);
		return 1;
	case __IAVF_INIT_EXTENDED_CAPS:
		iavf_init_process_extended_caps(adapter);
		return 1;
	case __IAVF_INIT_CONFIG_ADAPTER:
		iavf_init_config_adapter(adapter);
		return 1;
	case __IAVF_INIT_FAILED:
		if (test_bit(__IAVF_IN_REMOVE_TASK,
			     &adapter->crit_section)) {
			/* Do not update the state and do not reschedule
			 * watchdog task, iavf_remove should handle this state
			 * as it can loop forever
			 */
			return IAVF_NO_RESCHED;
		}
		if (++adapter->aq_wait_count > IAVF_AQ_MAX_ERR) {
			dev_err(&adapter->pdev->dev,
				"Failed to communicate with PF; waiting before retry\n");
			adapter->flags |= IAVF_FLAG_PF_COMMS_FAILED;
			iavf_shutdown_adminq(hw);
			return 5000;
		}
		/* Try again from failed step*/
		iavf_change_state(adapter, adapter->last_state);
		return 1000;
	case __IAVF_COMM_FAILED:
		if (test_bit(__IAVF_IN_REMOVE_TASK,
			     &adapter->crit_section)) {
			/* Set state to __IAVF_INIT_FAILED and perform remove
			 * steps. Remove IAVF_FLAG_PF_COMMS_FAILED so the task
			 * doesn't bring the state back to __IAVF_COMM_FAILED.
			 */
			iavf_change_state(adapter, __IAVF_INIT_FAILED);
			adapter->flags &= ~IAVF_FLAG_PF_COMMS_FAILED;
			return IAVF_NO_RESCHED;
		}
		reg_val = rd32(hw, IAVF_VFGEN_RSTAT) &
			  IAVF_VFGEN_RSTAT_VFR_STATE_MASK;
		if (reg_val == VIRTCHNL_VFR_VFACTIVE ||
		    reg_val == VIRTCHNL_VFR_COMPLETED) {
			/* A chance for redemption! */
			dev_err(&adapter->pdev->dev,
				"Hardware came out of reset. Attempting reinit.\n");
			/* When init task contacts the PF and
			 * gets everything set up again, it'll restart the
			 * watchdog for us. Down, boy. Sit. Stay. Woof.
			 */
			iavf_change_state(adapter, __IAVF_STARTUP);
			adapter->flags &= ~IAVF_FLAG_PF_COMMS_FAILED;
		}
		adapter->aq_required = 0;
		adapter->current_op = VIRTCHNL_OP_UNKNOWN;
		return 10;
	case __IAVF_RESETTING:
		return 2000;
	case __IAVF_DOWN:
	case __IAVF_DOWN_PENDING:
	case __IAVF_TESTING:
	case __IAVF_RUNNING:
		if (adapter->current_op) {
			if (!iavf_asq_done(hw)) {
				dev_dbg(&adapter->pdev->dev,
					"Admin queue timeout\n");
				iavf_send_api_ver(adapter);
			}
		} else {
			int ret = iavf_process_aq_command(adapter);

			/* An error will be returned if no commands were
			 * processed; use this opportunity to update stats
			 * if the error isn't -ENOTSUPP
			 */
			if (ret && ret != -EOPNOTSUPP &&
			    adapter->state == __IAVF_RUNNING)
				iavf_request_stats(adapter);
		}
		if (adapter->state == __IAVF_RUNNING)
			iavf_detect_recover_hung(&adapter->vsi);
		break;
	case __IAVF_REMOVE:
	default:
		return IAVF_NO_RESCHED;
	}

	/* check for hw reset */
	reg_val = rd32(hw, IAVF_VF_ARQLEN1) & IAVF_VF_ARQLEN1_ARQENABLE_MASK;
	if (!reg_val) {
		adapter->aq_required = 0;
		adapter->current_op = VIRTCHNL_OP_UNKNOWN;
		dev_err(&adapter->pdev->dev, "Hardware reset detected\n");
		iavf_schedule_reset(adapter, IAVF_FLAG_RESET_PENDING);
	}

	return adapter->aq_required ? 20 : 2000;
}

static void iavf_watchdog_task(struct work_struct *work)
{
	struct iavf_adapter *adapter = container_of(work,
						    struct iavf_adapter,
						    watchdog_task.work);
	struct net_device *netdev = adapter->netdev;
	int msec_delay;

	netdev_lock(netdev);
	msec_delay = iavf_watchdog_step(adapter);
	/* note that we schedule a different task */
	if (adapter->state >= __IAVF_DOWN)
		queue_work(adapter->wq, &adapter->adminq_task);

	if (msec_delay != IAVF_NO_RESCHED)
		queue_delayed_work(adapter->wq, &adapter->watchdog_task,
				   msecs_to_jiffies(msec_delay));
	netdev_unlock(netdev);
}

/**
 * iavf_disable_vf - disable VF
 * @adapter: board private structure
 *
 * Set communication failed flag and free all resources.
 */
static void iavf_disable_vf(struct iavf_adapter *adapter)
{
	struct iavf_mac_filter *f, *ftmp;
	struct iavf_vlan_filter *fv, *fvtmp;
	struct iavf_cloud_filter *cf, *cftmp;

	netdev_assert_locked(adapter->netdev);

	adapter->flags |= IAVF_FLAG_PF_COMMS_FAILED;

	/* We don't use netif_running() because it may be true prior to
	 * ndo_open() returning, so we can't assume it means all our open
	 * tasks have finished, since we're not holding the rtnl_lock here.
	 */
	if (adapter->state == __IAVF_RUNNING) {
		set_bit(__IAVF_VSI_DOWN, adapter->vsi.state);
		netif_carrier_off(adapter->netdev);
		netif_tx_disable(adapter->netdev);
		adapter->link_up = false;
		iavf_napi_disable_all(adapter);
		iavf_irq_disable(adapter);
		iavf_free_traffic_irqs(adapter);
		iavf_free_all_tx_resources(adapter);
		iavf_free_all_rx_resources(adapter);
	}

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	/* Delete all of the filters */
	list_for_each_entry_safe(f, ftmp, &adapter->mac_filter_list, list) {
		list_del(&f->list);
		kfree(f);
	}

	list_for_each_entry_safe(fv, fvtmp, &adapter->vlan_filter_list, list) {
		list_del(&fv->list);
		kfree(fv);
	}
	adapter->num_vlan_filters = 0;

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_for_each_entry_safe(cf, cftmp, &adapter->cloud_filter_list, list) {
		list_del(&cf->list);
		kfree(cf);
		adapter->num_cloud_filters--;
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);

	iavf_free_misc_irq(adapter);
	iavf_free_interrupt_scheme(adapter);
	memset(adapter->vf_res, 0, IAVF_VIRTCHNL_VF_RESOURCE_SIZE);
	iavf_shutdown_adminq(&adapter->hw);
	adapter->flags &= ~IAVF_FLAG_RESET_PENDING;
	iavf_change_state(adapter, __IAVF_DOWN);
	wake_up(&adapter->down_waitqueue);
	dev_info(&adapter->pdev->dev, "Reset task did not complete, VF disabled\n");
}

/**
 * iavf_reconfig_qs_bw - Call-back task to handle hardware reset
 * @adapter: board private structure
 *
 * After a reset, the shaper parameters of queues need to be replayed again.
 * Since the net_shaper object inside TX rings persists across reset,
 * set the update flag for all queues so that the virtchnl message is triggered
 * for all queues.
 **/
static void iavf_reconfig_qs_bw(struct iavf_adapter *adapter)
{
	int i, num = 0;

	for (i = 0; i < adapter->num_active_queues; i++)
		if (adapter->tx_rings[i].q_shaper.bw_min ||
		    adapter->tx_rings[i].q_shaper.bw_max) {
			adapter->tx_rings[i].q_shaper_update = true;
			num++;
		}

	if (num)
		adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_QUEUES_BW;
}

/**
 * iavf_reset_task - Call-back task to handle hardware reset
 * @work: pointer to work_struct
 *
 * During reset we need to shut down and reinitialize the admin queue
 * before we can use it to communicate with the PF again. We also clear
 * and reinit the rings because that context is lost as well.
 **/
static void iavf_reset_task(struct work_struct *work)
{
	struct iavf_adapter *adapter = container_of(work,
						      struct iavf_adapter,
						      reset_task);
	struct virtchnl_vf_resource *vfres = adapter->vf_res;
	struct net_device *netdev = adapter->netdev;
	struct iavf_hw *hw = &adapter->hw;
	struct iavf_mac_filter *f, *ftmp;
	struct iavf_cloud_filter *cf;
	enum iavf_status status;
	u32 reg_val;
	int i = 0, err;
	bool running;

	netdev_lock(netdev);

	iavf_misc_irq_disable(adapter);
	if (adapter->flags & IAVF_FLAG_RESET_NEEDED) {
		adapter->flags &= ~IAVF_FLAG_RESET_NEEDED;
		/* Restart the AQ here. If we have been reset but didn't
		 * detect it, or if the PF had to reinit, our AQ will be hosed.
		 */
		iavf_shutdown_adminq(hw);
		iavf_init_adminq(hw);
		iavf_request_reset(adapter);
	}
	adapter->flags |= IAVF_FLAG_RESET_PENDING;

	/* poll until we see the reset actually happen */
	for (i = 0; i < IAVF_RESET_WAIT_DETECTED_COUNT; i++) {
		reg_val = rd32(hw, IAVF_VF_ARQLEN1) &
			  IAVF_VF_ARQLEN1_ARQENABLE_MASK;
		if (!reg_val)
			break;
		usleep_range(5000, 10000);
	}
	if (i == IAVF_RESET_WAIT_DETECTED_COUNT) {
		dev_info(&adapter->pdev->dev, "Never saw reset\n");
		goto continue_reset; /* act like the reset happened */
	}

	/* wait until the reset is complete and the PF is responding to us */
	for (i = 0; i < IAVF_RESET_WAIT_COMPLETE_COUNT; i++) {
		/* sleep first to make sure a minimum wait time is met */
		msleep(IAVF_RESET_WAIT_MS);

		reg_val = rd32(hw, IAVF_VFGEN_RSTAT) &
			  IAVF_VFGEN_RSTAT_VFR_STATE_MASK;
		if (reg_val == VIRTCHNL_VFR_VFACTIVE)
			break;
	}

	pci_set_master(adapter->pdev);
	pci_restore_msi_state(adapter->pdev);

	if (i == IAVF_RESET_WAIT_COMPLETE_COUNT) {
		dev_err(&adapter->pdev->dev, "Reset never finished (%x)\n",
			reg_val);
		iavf_disable_vf(adapter);
		netdev_unlock(netdev);
		return; /* Do not attempt to reinit. It's dead, Jim. */
	}

continue_reset:
	/* If we are still early in the state machine, just restart. */
	if (adapter->state <= __IAVF_INIT_FAILED) {
		iavf_shutdown_adminq(hw);
		iavf_change_state(adapter, __IAVF_STARTUP);
		iavf_startup(adapter);
		queue_delayed_work(adapter->wq, &adapter->watchdog_task,
				   msecs_to_jiffies(30));
		netdev_unlock(netdev);
		return;
	}

	/* We don't use netif_running() because it may be true prior to
	 * ndo_open() returning, so we can't assume it means all our open
	 * tasks have finished, since we're not holding the rtnl_lock here.
	 */
	running = adapter->state == __IAVF_RUNNING;

	if (running) {
		netif_carrier_off(netdev);
		netif_tx_stop_all_queues(netdev);
		adapter->link_up = false;
		iavf_napi_disable_all(adapter);
	}
	iavf_irq_disable(adapter);

	iavf_change_state(adapter, __IAVF_RESETTING);
	adapter->flags &= ~IAVF_FLAG_RESET_PENDING;

	/* free the Tx/Rx rings and descriptors, might be better to just
	 * re-use them sometime in the future
	 */
	iavf_free_all_rx_resources(adapter);
	iavf_free_all_tx_resources(adapter);

	adapter->flags |= IAVF_FLAG_QUEUES_DISABLED;
	/* kill and reinit the admin queue */
	iavf_shutdown_adminq(hw);
	adapter->current_op = VIRTCHNL_OP_UNKNOWN;
	status = iavf_init_adminq(hw);
	if (status) {
		dev_info(&adapter->pdev->dev, "Failed to init adminq: %d\n",
			 status);
		goto reset_err;
	}
	adapter->aq_required = 0;

	if ((adapter->flags & IAVF_FLAG_REINIT_MSIX_NEEDED) ||
	    (adapter->flags & IAVF_FLAG_REINIT_ITR_NEEDED)) {
		err = iavf_reinit_interrupt_scheme(adapter, running);
		if (err)
			goto reset_err;
	}

	if (RSS_AQ(adapter)) {
		adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_RSS;
	} else {
		err = iavf_init_rss(adapter);
		if (err)
			goto reset_err;
	}

	adapter->aq_required |= IAVF_FLAG_AQ_GET_CONFIG;
	adapter->aq_required |= IAVF_FLAG_AQ_MAP_VECTORS;

	/* Certain capabilities require an extended negotiation process using
	 * extra messages that must be processed after getting the VF
	 * configuration. The related checks such as VLAN_V2_ALLOWED() are not
	 * reliable here, since the configuration has not yet been negotiated.
	 *
	 * Always set these flags, since them related VIRTCHNL messages won't
	 * be sent until after VIRTCHNL_OP_GET_VF_RESOURCES.
	 */
	adapter->aq_required |= IAVF_FLAG_AQ_EXTENDED_CAPS;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	/* Delete filter for the current MAC address, it could have
	 * been changed by the PF via administratively set MAC.
	 * Will be re-added via VIRTCHNL_OP_GET_VF_RESOURCES.
	 */
	list_for_each_entry_safe(f, ftmp, &adapter->mac_filter_list, list) {
		if (ether_addr_equal(f->macaddr, adapter->hw.mac.addr)) {
			list_del(&f->list);
			kfree(f);
		}
	}
	/* re-add all MAC filters */
	list_for_each_entry(f, &adapter->mac_filter_list, list) {
		f->add = true;
	}
	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	/* check if TCs are running and re-add all cloud filters */
	spin_lock_bh(&adapter->cloud_filter_list_lock);
	if ((vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ADQ) &&
	    adapter->num_tc) {
		list_for_each_entry(cf, &adapter->cloud_filter_list, list) {
			cf->add = true;
		}
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);

	adapter->aq_required |= IAVF_FLAG_AQ_ADD_MAC_FILTER;
	adapter->aq_required |= IAVF_FLAG_AQ_ADD_CLOUD_FILTER;
	iavf_misc_irq_enable(adapter);

	mod_delayed_work(adapter->wq, &adapter->watchdog_task, 2);

	/* We were running when the reset started, so we need to restore some
	 * state here.
	 */
	if (running) {
		/* allocate transmit descriptors */
		err = iavf_setup_all_tx_resources(adapter);
		if (err)
			goto reset_err;

		/* allocate receive descriptors */
		err = iavf_setup_all_rx_resources(adapter);
		if (err)
			goto reset_err;

		if ((adapter->flags & IAVF_FLAG_REINIT_MSIX_NEEDED) ||
		    (adapter->flags & IAVF_FLAG_REINIT_ITR_NEEDED)) {
			err = iavf_request_traffic_irqs(adapter, netdev->name);
			if (err)
				goto reset_err;

			adapter->flags &= ~IAVF_FLAG_REINIT_MSIX_NEEDED;
		}

		iavf_configure(adapter);

		/* iavf_up_complete() will switch device back
		 * to __IAVF_RUNNING
		 */
		iavf_up_complete(adapter);

		iavf_irq_enable(adapter, true);

		iavf_reconfig_qs_bw(adapter);
	} else {
		iavf_change_state(adapter, __IAVF_DOWN);
		wake_up(&adapter->down_waitqueue);
	}

	adapter->flags &= ~IAVF_FLAG_REINIT_ITR_NEEDED;

	wake_up(&adapter->reset_waitqueue);
	netdev_unlock(netdev);

	return;
reset_err:
	if (running) {
		set_bit(__IAVF_VSI_DOWN, adapter->vsi.state);
		iavf_free_traffic_irqs(adapter);
	}
	iavf_disable_vf(adapter);

	netdev_unlock(netdev);
	dev_err(&adapter->pdev->dev, "failed to allocate resources during reinit\n");
}

/**
 * iavf_adminq_task - worker thread to clean the admin queue
 * @work: pointer to work_struct containing our data
 **/
static void iavf_adminq_task(struct work_struct *work)
{
	struct iavf_adapter *adapter =
		container_of(work, struct iavf_adapter, adminq_task);
	struct net_device *netdev = adapter->netdev;
	struct iavf_hw *hw = &adapter->hw;
	struct iavf_arq_event_info event;
	enum virtchnl_ops v_op;
	enum iavf_status ret, v_ret;
	u32 val, oldval;
	u16 pending;

	netdev_lock(netdev);

	if (adapter->flags & IAVF_FLAG_PF_COMMS_FAILED)
		goto unlock;

	event.buf_len = IAVF_MAX_AQ_BUF_SIZE;
	event.msg_buf = kzalloc(event.buf_len, GFP_KERNEL);
	if (!event.msg_buf)
		goto unlock;

	do {
		ret = iavf_clean_arq_element(hw, &event, &pending);
		v_op = (enum virtchnl_ops)le32_to_cpu(event.desc.cookie_high);
		v_ret = (enum iavf_status)le32_to_cpu(event.desc.cookie_low);

		if (ret || !v_op)
			break; /* No event to process or error cleaning ARQ */

		iavf_virtchnl_completion(adapter, v_op, v_ret, event.msg_buf,
					 event.msg_len);
		if (pending != 0)
			memset(event.msg_buf, 0, IAVF_MAX_AQ_BUF_SIZE);
	} while (pending);

	if (iavf_is_reset_in_progress(adapter))
		goto freedom;

	/* check for error indications */
	val = rd32(hw, IAVF_VF_ARQLEN1);
	if (val == 0xdeadbeef || val == 0xffffffff) /* device in reset */
		goto freedom;
	oldval = val;
	if (val & IAVF_VF_ARQLEN1_ARQVFE_MASK) {
		dev_info(&adapter->pdev->dev, "ARQ VF Error detected\n");
		val &= ~IAVF_VF_ARQLEN1_ARQVFE_MASK;
	}
	if (val & IAVF_VF_ARQLEN1_ARQOVFL_MASK) {
		dev_info(&adapter->pdev->dev, "ARQ Overflow Error detected\n");
		val &= ~IAVF_VF_ARQLEN1_ARQOVFL_MASK;
	}
	if (val & IAVF_VF_ARQLEN1_ARQCRIT_MASK) {
		dev_info(&adapter->pdev->dev, "ARQ Critical Error detected\n");
		val &= ~IAVF_VF_ARQLEN1_ARQCRIT_MASK;
	}
	if (oldval != val)
		wr32(hw, IAVF_VF_ARQLEN1, val);

	val = rd32(hw, IAVF_VF_ATQLEN1);
	oldval = val;
	if (val & IAVF_VF_ATQLEN1_ATQVFE_MASK) {
		dev_info(&adapter->pdev->dev, "ASQ VF Error detected\n");
		val &= ~IAVF_VF_ATQLEN1_ATQVFE_MASK;
	}
	if (val & IAVF_VF_ATQLEN1_ATQOVFL_MASK) {
		dev_info(&adapter->pdev->dev, "ASQ Overflow Error detected\n");
		val &= ~IAVF_VF_ATQLEN1_ATQOVFL_MASK;
	}
	if (val & IAVF_VF_ATQLEN1_ATQCRIT_MASK) {
		dev_info(&adapter->pdev->dev, "ASQ Critical Error detected\n");
		val &= ~IAVF_VF_ATQLEN1_ATQCRIT_MASK;
	}
	if (oldval != val)
		wr32(hw, IAVF_VF_ATQLEN1, val);

freedom:
	kfree(event.msg_buf);
unlock:
	netdev_unlock(netdev);
	/* re-enable Admin queue interrupt cause */
	iavf_misc_irq_enable(adapter);
}

/**
 * iavf_free_all_tx_resources - Free Tx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all transmit software resources
 **/
void iavf_free_all_tx_resources(struct iavf_adapter *adapter)
{
	int i;

	if (!adapter->tx_rings)
		return;

	for (i = 0; i < adapter->num_active_queues; i++)
		if (adapter->tx_rings[i].desc)
			iavf_free_tx_resources(&adapter->tx_rings[i]);
}

/**
 * iavf_setup_all_tx_resources - allocate all queues Tx resources
 * @adapter: board private structure
 *
 * If this function returns with an error, then it's possible one or
 * more of the rings is populated (while the rest are not).  It is the
 * callers duty to clean those orphaned rings.
 *
 * Return 0 on success, negative on failure
 **/
static int iavf_setup_all_tx_resources(struct iavf_adapter *adapter)
{
	int i, err = 0;

	for (i = 0; i < adapter->num_active_queues; i++) {
		adapter->tx_rings[i].count = adapter->tx_desc_count;
		err = iavf_setup_tx_descriptors(&adapter->tx_rings[i]);
		if (!err)
			continue;
		dev_err(&adapter->pdev->dev,
			"Allocation for Tx Queue %u failed\n", i);
		break;
	}

	return err;
}

/**
 * iavf_setup_all_rx_resources - allocate all queues Rx resources
 * @adapter: board private structure
 *
 * If this function returns with an error, then it's possible one or
 * more of the rings is populated (while the rest are not).  It is the
 * callers duty to clean those orphaned rings.
 *
 * Return 0 on success, negative on failure
 **/
static int iavf_setup_all_rx_resources(struct iavf_adapter *adapter)
{
	int i, err = 0;

	for (i = 0; i < adapter->num_active_queues; i++) {
		adapter->rx_rings[i].count = adapter->rx_desc_count;
		err = iavf_setup_rx_descriptors(&adapter->rx_rings[i]);
		if (!err)
			continue;
		dev_err(&adapter->pdev->dev,
			"Allocation for Rx Queue %u failed\n", i);
		break;
	}
	return err;
}

/**
 * iavf_free_all_rx_resources - Free Rx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all receive software resources
 **/
void iavf_free_all_rx_resources(struct iavf_adapter *adapter)
{
	int i;

	if (!adapter->rx_rings)
		return;

	for (i = 0; i < adapter->num_active_queues; i++)
		if (adapter->rx_rings[i].desc)
			iavf_free_rx_resources(&adapter->rx_rings[i]);
}

/**
 * iavf_validate_tx_bandwidth - validate the max Tx bandwidth
 * @adapter: board private structure
 * @max_tx_rate: max Tx bw for a tc
 **/
static int iavf_validate_tx_bandwidth(struct iavf_adapter *adapter,
				      u64 max_tx_rate)
{
	int speed = 0, ret = 0;

	if (ADV_LINK_SUPPORT(adapter)) {
		if (adapter->link_speed_mbps < U32_MAX) {
			speed = adapter->link_speed_mbps;
			goto validate_bw;
		} else {
			dev_err(&adapter->pdev->dev, "Unknown link speed\n");
			return -EINVAL;
		}
	}

	switch (adapter->link_speed) {
	case VIRTCHNL_LINK_SPEED_40GB:
		speed = SPEED_40000;
		break;
	case VIRTCHNL_LINK_SPEED_25GB:
		speed = SPEED_25000;
		break;
	case VIRTCHNL_LINK_SPEED_20GB:
		speed = SPEED_20000;
		break;
	case VIRTCHNL_LINK_SPEED_10GB:
		speed = SPEED_10000;
		break;
	case VIRTCHNL_LINK_SPEED_5GB:
		speed = SPEED_5000;
		break;
	case VIRTCHNL_LINK_SPEED_2_5GB:
		speed = SPEED_2500;
		break;
	case VIRTCHNL_LINK_SPEED_1GB:
		speed = SPEED_1000;
		break;
	case VIRTCHNL_LINK_SPEED_100MB:
		speed = SPEED_100;
		break;
	default:
		break;
	}

validate_bw:
	if (max_tx_rate > speed) {
		dev_err(&adapter->pdev->dev,
			"Invalid tx rate specified\n");
		ret = -EINVAL;
	}

	return ret;
}

/**
 * iavf_validate_ch_config - validate queue mapping info
 * @adapter: board private structure
 * @mqprio_qopt: queue parameters
 *
 * This function validates if the config provided by the user to
 * configure queue channels is valid or not. Returns 0 on a valid
 * config.
 **/
static int iavf_validate_ch_config(struct iavf_adapter *adapter,
				   struct tc_mqprio_qopt_offload *mqprio_qopt)
{
	u64 total_max_rate = 0;
	u32 tx_rate_rem = 0;
	int i, num_qps = 0;
	u64 tx_rate = 0;
	int ret = 0;

	if (mqprio_qopt->qopt.num_tc > IAVF_MAX_TRAFFIC_CLASS ||
	    mqprio_qopt->qopt.num_tc < 1)
		return -EINVAL;

	for (i = 0; i <= mqprio_qopt->qopt.num_tc - 1; i++) {
		if (!mqprio_qopt->qopt.count[i] ||
		    mqprio_qopt->qopt.offset[i] != num_qps)
			return -EINVAL;
		if (mqprio_qopt->min_rate[i]) {
			dev_err(&adapter->pdev->dev,
				"Invalid min tx rate (greater than 0) specified for TC%d\n",
				i);
			return -EINVAL;
		}

		/* convert to Mbps */
		tx_rate = div_u64(mqprio_qopt->max_rate[i],
				  IAVF_MBPS_DIVISOR);

		if (mqprio_qopt->max_rate[i] &&
		    tx_rate < IAVF_MBPS_QUANTA) {
			dev_err(&adapter->pdev->dev,
				"Invalid max tx rate for TC%d, minimum %dMbps\n",
				i, IAVF_MBPS_QUANTA);
			return -EINVAL;
		}

		(void)div_u64_rem(tx_rate, IAVF_MBPS_QUANTA, &tx_rate_rem);

		if (tx_rate_rem != 0) {
			dev_err(&adapter->pdev->dev,
				"Invalid max tx rate for TC%d, not divisible by %d\n",
				i, IAVF_MBPS_QUANTA);
			return -EINVAL;
		}

		total_max_rate += tx_rate;
		num_qps += mqprio_qopt->qopt.count[i];
	}
	if (num_qps > adapter->num_active_queues) {
		dev_err(&adapter->pdev->dev,
			"Cannot support requested number of queues\n");
		return -EINVAL;
	}

	ret = iavf_validate_tx_bandwidth(adapter, total_max_rate);
	return ret;
}

/**
 * iavf_del_all_cloud_filters - delete all cloud filters on the traffic classes
 * @adapter: board private structure
 **/
static void iavf_del_all_cloud_filters(struct iavf_adapter *adapter)
{
	struct iavf_cloud_filter *cf, *cftmp;

	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_for_each_entry_safe(cf, cftmp, &adapter->cloud_filter_list,
				 list) {
		list_del(&cf->list);
		kfree(cf);
		adapter->num_cloud_filters--;
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);
}

/**
 * iavf_is_tc_config_same - Compare the mqprio TC config with the
 * TC config already configured on this adapter.
 * @adapter: board private structure
 * @mqprio_qopt: TC config received from kernel.
 *
 * This function compares the TC config received from the kernel
 * with the config already configured on the adapter.
 *
 * Return: True if configuration is same, false otherwise.
 **/
static bool iavf_is_tc_config_same(struct iavf_adapter *adapter,
				   struct tc_mqprio_qopt *mqprio_qopt)
{
	struct virtchnl_channel_info *ch = &adapter->ch_config.ch_info[0];
	int i;

	if (adapter->num_tc != mqprio_qopt->num_tc)
		return false;

	for (i = 0; i < adapter->num_tc; i++) {
		if (ch[i].count != mqprio_qopt->count[i] ||
		    ch[i].offset != mqprio_qopt->offset[i])
			return false;
	}
	return true;
}

/**
 * __iavf_setup_tc - configure multiple traffic classes
 * @netdev: network interface device structure
 * @type_data: tc offload data
 *
 * This function processes the config information provided by the
 * user to configure traffic classes/queue channels and packages the
 * information to request the PF to setup traffic classes.
 *
 * Returns 0 on success.
 **/
static int __iavf_setup_tc(struct net_device *netdev, void *type_data)
{
	struct tc_mqprio_qopt_offload *mqprio_qopt = type_data;
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct virtchnl_vf_resource *vfres = adapter->vf_res;
	u8 num_tc = 0, total_qps = 0;
	int ret = 0, netdev_tc = 0;
	u64 max_tx_rate;
	u16 mode;
	int i;

	num_tc = mqprio_qopt->qopt.num_tc;
	mode = mqprio_qopt->mode;

	/* delete queue_channel */
	if (!mqprio_qopt->qopt.hw) {
		if (adapter->ch_config.state == __IAVF_TC_RUNNING) {
			/* reset the tc configuration */
			netdev_reset_tc(netdev);
			adapter->num_tc = 0;
			netif_tx_stop_all_queues(netdev);
			netif_tx_disable(netdev);
			iavf_del_all_cloud_filters(adapter);
			adapter->aq_required = IAVF_FLAG_AQ_DISABLE_CHANNELS;
			total_qps = adapter->orig_num_active_queues;
			goto exit;
		} else {
			return -EINVAL;
		}
	}

	/* add queue channel */
	if (mode == TC_MQPRIO_MODE_CHANNEL) {
		if (!(vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ADQ)) {
			dev_err(&adapter->pdev->dev, "ADq not supported\n");
			return -EOPNOTSUPP;
		}
		if (adapter->ch_config.state != __IAVF_TC_INVALID) {
			dev_err(&adapter->pdev->dev, "TC configuration already exists\n");
			return -EINVAL;
		}

		ret = iavf_validate_ch_config(adapter, mqprio_qopt);
		if (ret)
			return ret;
		/* Return if same TC config is requested */
		if (iavf_is_tc_config_same(adapter, &mqprio_qopt->qopt))
			return 0;
		adapter->num_tc = num_tc;

		for (i = 0; i < IAVF_MAX_TRAFFIC_CLASS; i++) {
			if (i < num_tc) {
				adapter->ch_config.ch_info[i].count =
					mqprio_qopt->qopt.count[i];
				adapter->ch_config.ch_info[i].offset =
					mqprio_qopt->qopt.offset[i];
				total_qps += mqprio_qopt->qopt.count[i];
				max_tx_rate = mqprio_qopt->max_rate[i];
				/* convert to Mbps */
				max_tx_rate = div_u64(max_tx_rate,
						      IAVF_MBPS_DIVISOR);
				adapter->ch_config.ch_info[i].max_tx_rate =
					max_tx_rate;
			} else {
				adapter->ch_config.ch_info[i].count = 1;
				adapter->ch_config.ch_info[i].offset = 0;
			}
		}

		/* Take snapshot of original config such as "num_active_queues"
		 * It is used later when delete ADQ flow is exercised, so that
		 * once delete ADQ flow completes, VF shall go back to its
		 * original queue configuration
		 */

		adapter->orig_num_active_queues = adapter->num_active_queues;

		/* Store queue info based on TC so that VF gets configured
		 * with correct number of queues when VF completes ADQ config
		 * flow
		 */
		adapter->ch_config.total_qps = total_qps;

		netif_tx_stop_all_queues(netdev);
		netif_tx_disable(netdev);
		adapter->aq_required |= IAVF_FLAG_AQ_ENABLE_CHANNELS;
		netdev_reset_tc(netdev);
		/* Report the tc mapping up the stack */
		netdev_set_num_tc(adapter->netdev, num_tc);
		for (i = 0; i < IAVF_MAX_TRAFFIC_CLASS; i++) {
			u16 qcount = mqprio_qopt->qopt.count[i];
			u16 qoffset = mqprio_qopt->qopt.offset[i];

			if (i < num_tc)
				netdev_set_tc_queue(netdev, netdev_tc++, qcount,
						    qoffset);
		}
	}
exit:
	if (test_bit(__IAVF_IN_REMOVE_TASK, &adapter->crit_section))
		return 0;

	netif_set_real_num_rx_queues(netdev, total_qps);
	netif_set_real_num_tx_queues(netdev, total_qps);

	return ret;
}

/**
 * iavf_parse_cls_flower - Parse tc flower filters provided by kernel
 * @adapter: board private structure
 * @f: pointer to struct flow_cls_offload
 * @filter: pointer to cloud filter structure
 */
static int iavf_parse_cls_flower(struct iavf_adapter *adapter,
				 struct flow_cls_offload *f,
				 struct iavf_cloud_filter *filter)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_dissector *dissector = rule->match.dissector;
	u16 n_proto_mask = 0;
	u16 n_proto_key = 0;
	u8 field_flags = 0;
	u16 addr_type = 0;
	u16 n_proto = 0;
	int i = 0;
	struct virtchnl_filter *vf = &filter->f;

	if (dissector->used_keys &
	    ~(BIT_ULL(FLOW_DISSECTOR_KEY_CONTROL) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_BASIC) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_VLAN) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_PORTS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ENC_KEYID))) {
		dev_err(&adapter->pdev->dev, "Unsupported key used: 0x%llx\n",
			dissector->used_keys);
		return -EOPNOTSUPP;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_KEYID)) {
		struct flow_match_enc_keyid match;

		flow_rule_match_enc_keyid(rule, &match);
		if (match.mask->keyid != 0)
			field_flags |= IAVF_CLOUD_FIELD_TEN_ID;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		n_proto_key = ntohs(match.key->n_proto);
		n_proto_mask = ntohs(match.mask->n_proto);

		if (n_proto_key == ETH_P_ALL) {
			n_proto_key = 0;
			n_proto_mask = 0;
		}
		n_proto = n_proto_key & n_proto_mask;
		if (n_proto != ETH_P_IP && n_proto != ETH_P_IPV6)
			return -EINVAL;
		if (n_proto == ETH_P_IPV6) {
			/* specify flow type as TCP IPv6 */
			vf->flow_type = VIRTCHNL_TCP_V6_FLOW;
		}

		if (match.key->ip_proto != IPPROTO_TCP) {
			dev_info(&adapter->pdev->dev, "Only TCP transport is supported\n");
			return -EINVAL;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		/* use is_broadcast and is_zero to check for all 0xf or 0 */
		if (!is_zero_ether_addr(match.mask->dst)) {
			if (is_broadcast_ether_addr(match.mask->dst)) {
				field_flags |= IAVF_CLOUD_FIELD_OMAC;
			} else {
				dev_err(&adapter->pdev->dev, "Bad ether dest mask %pM\n",
					match.mask->dst);
				return -EINVAL;
			}
		}

		if (!is_zero_ether_addr(match.mask->src)) {
			if (is_broadcast_ether_addr(match.mask->src)) {
				field_flags |= IAVF_CLOUD_FIELD_IMAC;
			} else {
				dev_err(&adapter->pdev->dev, "Bad ether src mask %pM\n",
					match.mask->src);
				return -EINVAL;
			}
		}

		if (!is_zero_ether_addr(match.key->dst))
			if (is_valid_ether_addr(match.key->dst) ||
			    is_multicast_ether_addr(match.key->dst)) {
				/* set the mask if a valid dst_mac address */
				for (i = 0; i < ETH_ALEN; i++)
					vf->mask.tcp_spec.dst_mac[i] |= 0xff;
				ether_addr_copy(vf->data.tcp_spec.dst_mac,
						match.key->dst);
			}

		if (!is_zero_ether_addr(match.key->src))
			if (is_valid_ether_addr(match.key->src) ||
			    is_multicast_ether_addr(match.key->src)) {
				/* set the mask if a valid dst_mac address */
				for (i = 0; i < ETH_ALEN; i++)
					vf->mask.tcp_spec.src_mac[i] |= 0xff;
				ether_addr_copy(vf->data.tcp_spec.src_mac,
						match.key->src);
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);
		if (match.mask->vlan_id) {
			if (match.mask->vlan_id == VLAN_VID_MASK) {
				field_flags |= IAVF_CLOUD_FIELD_IVLAN;
			} else {
				dev_err(&adapter->pdev->dev, "Bad vlan mask %u\n",
					match.mask->vlan_id);
				return -EINVAL;
			}
		}
		vf->mask.tcp_spec.vlan_id |= cpu_to_be16(0xffff);
		vf->data.tcp_spec.vlan_id = cpu_to_be16(match.key->vlan_id);
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;

		if (flow_rule_has_control_flags(match.mask->flags,
						f->common.extack))
			return -EOPNOTSUPP;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);
		if (match.mask->dst) {
			if (match.mask->dst == cpu_to_be32(0xffffffff)) {
				field_flags |= IAVF_CLOUD_FIELD_IIP;
			} else {
				dev_err(&adapter->pdev->dev, "Bad ip dst mask 0x%08x\n",
					be32_to_cpu(match.mask->dst));
				return -EINVAL;
			}
		}

		if (match.mask->src) {
			if (match.mask->src == cpu_to_be32(0xffffffff)) {
				field_flags |= IAVF_CLOUD_FIELD_IIP;
			} else {
				dev_err(&adapter->pdev->dev, "Bad ip src mask 0x%08x\n",
					be32_to_cpu(match.mask->src));
				return -EINVAL;
			}
		}

		if (field_flags & IAVF_CLOUD_FIELD_TEN_ID) {
			dev_info(&adapter->pdev->dev, "Tenant id not allowed for ip filter\n");
			return -EINVAL;
		}
		if (match.key->dst) {
			vf->mask.tcp_spec.dst_ip[0] |= cpu_to_be32(0xffffffff);
			vf->data.tcp_spec.dst_ip[0] = match.key->dst;
		}
		if (match.key->src) {
			vf->mask.tcp_spec.src_ip[0] |= cpu_to_be32(0xffffffff);
			vf->data.tcp_spec.src_ip[0] = match.key->src;
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_ipv6_addrs(rule, &match);

		/* validate mask, make sure it is not IPV6_ADDR_ANY */
		if (ipv6_addr_any(&match.mask->dst)) {
			dev_err(&adapter->pdev->dev, "Bad ipv6 dst mask 0x%02x\n",
				IPV6_ADDR_ANY);
			return -EINVAL;
		}

		/* src and dest IPv6 address should not be LOOPBACK
		 * (0:0:0:0:0:0:0:1) which can be represented as ::1
		 */
		if (ipv6_addr_loopback(&match.key->dst) ||
		    ipv6_addr_loopback(&match.key->src)) {
			dev_err(&adapter->pdev->dev,
				"ipv6 addr should not be loopback\n");
			return -EINVAL;
		}
		if (!ipv6_addr_any(&match.mask->dst) ||
		    !ipv6_addr_any(&match.mask->src))
			field_flags |= IAVF_CLOUD_FIELD_IIP;

		for (i = 0; i < 4; i++)
			vf->mask.tcp_spec.dst_ip[i] |= cpu_to_be32(0xffffffff);
		memcpy(&vf->data.tcp_spec.dst_ip, &match.key->dst.s6_addr32,
		       sizeof(vf->data.tcp_spec.dst_ip));
		for (i = 0; i < 4; i++)
			vf->mask.tcp_spec.src_ip[i] |= cpu_to_be32(0xffffffff);
		memcpy(&vf->data.tcp_spec.src_ip, &match.key->src.s6_addr32,
		       sizeof(vf->data.tcp_spec.src_ip));
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);
		if (match.mask->src) {
			if (match.mask->src == cpu_to_be16(0xffff)) {
				field_flags |= IAVF_CLOUD_FIELD_IIP;
			} else {
				dev_err(&adapter->pdev->dev, "Bad src port mask %u\n",
					be16_to_cpu(match.mask->src));
				return -EINVAL;
			}
		}

		if (match.mask->dst) {
			if (match.mask->dst == cpu_to_be16(0xffff)) {
				field_flags |= IAVF_CLOUD_FIELD_IIP;
			} else {
				dev_err(&adapter->pdev->dev, "Bad dst port mask %u\n",
					be16_to_cpu(match.mask->dst));
				return -EINVAL;
			}
		}
		if (match.key->dst) {
			vf->mask.tcp_spec.dst_port |= cpu_to_be16(0xffff);
			vf->data.tcp_spec.dst_port = match.key->dst;
		}

		if (match.key->src) {
			vf->mask.tcp_spec.src_port |= cpu_to_be16(0xffff);
			vf->data.tcp_spec.src_port = match.key->src;
		}
	}
	vf->field_flags = field_flags;

	return 0;
}

/**
 * iavf_handle_tclass - Forward to a traffic class on the device
 * @adapter: board private structure
 * @tc: traffic class index on the device
 * @filter: pointer to cloud filter structure
 */
static int iavf_handle_tclass(struct iavf_adapter *adapter, u32 tc,
			      struct iavf_cloud_filter *filter)
{
	if (tc == 0)
		return 0;
	if (tc < adapter->num_tc) {
		if (!filter->f.data.tcp_spec.dst_port) {
			dev_err(&adapter->pdev->dev,
				"Specify destination port to redirect to traffic class other than TC0\n");
			return -EINVAL;
		}
	}
	/* redirect to a traffic class on the same device */
	filter->f.action = VIRTCHNL_ACTION_TC_REDIRECT;
	filter->f.action_meta = tc;
	return 0;
}

/**
 * iavf_find_cf - Find the cloud filter in the list
 * @adapter: Board private structure
 * @cookie: filter specific cookie
 *
 * Returns ptr to the filter object or NULL. Must be called while holding the
 * cloud_filter_list_lock.
 */
static struct iavf_cloud_filter *iavf_find_cf(struct iavf_adapter *adapter,
					      unsigned long *cookie)
{
	struct iavf_cloud_filter *filter = NULL;

	if (!cookie)
		return NULL;

	list_for_each_entry(filter, &adapter->cloud_filter_list, list) {
		if (!memcmp(cookie, &filter->cookie, sizeof(filter->cookie)))
			return filter;
	}
	return NULL;
}

/**
 * iavf_configure_clsflower - Add tc flower filters
 * @adapter: board private structure
 * @cls_flower: Pointer to struct flow_cls_offload
 */
static int iavf_configure_clsflower(struct iavf_adapter *adapter,
				    struct flow_cls_offload *cls_flower)
{
	int tc = tc_classid_to_hwtc(adapter->netdev, cls_flower->classid);
	struct iavf_cloud_filter *filter;
	int err;

	if (tc < 0) {
		dev_err(&adapter->pdev->dev, "Invalid traffic class\n");
		return -EINVAL;
	}

	filter = kzalloc(sizeof(*filter), GFP_KERNEL);
	if (!filter)
		return -ENOMEM;
	filter->cookie = cls_flower->cookie;

	netdev_lock(adapter->netdev);

	/* bail out here if filter already exists */
	spin_lock_bh(&adapter->cloud_filter_list_lock);
	if (iavf_find_cf(adapter, &cls_flower->cookie)) {
		dev_err(&adapter->pdev->dev, "Failed to add TC Flower filter, it already exists\n");
		err = -EEXIST;
		goto spin_unlock;
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);

	/* set the mask to all zeroes to begin with */
	memset(&filter->f.mask.tcp_spec, 0, sizeof(struct virtchnl_l4_spec));
	/* start out with flow type and eth type IPv4 to begin with */
	filter->f.flow_type = VIRTCHNL_TCP_V4_FLOW;
	err = iavf_parse_cls_flower(adapter, cls_flower, filter);
	if (err)
		goto err;

	err = iavf_handle_tclass(adapter, tc, filter);
	if (err)
		goto err;

	/* add filter to the list */
	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_add_tail(&filter->list, &adapter->cloud_filter_list);
	adapter->num_cloud_filters++;
	filter->add = true;
	adapter->aq_required |= IAVF_FLAG_AQ_ADD_CLOUD_FILTER;
spin_unlock:
	spin_unlock_bh(&adapter->cloud_filter_list_lock);
err:
	if (err)
		kfree(filter);

	netdev_unlock(adapter->netdev);
	return err;
}

/**
 * iavf_delete_clsflower - Remove tc flower filters
 * @adapter: board private structure
 * @cls_flower: Pointer to struct flow_cls_offload
 */
static int iavf_delete_clsflower(struct iavf_adapter *adapter,
				 struct flow_cls_offload *cls_flower)
{
	struct iavf_cloud_filter *filter = NULL;
	int err = 0;

	spin_lock_bh(&adapter->cloud_filter_list_lock);
	filter = iavf_find_cf(adapter, &cls_flower->cookie);
	if (filter) {
		filter->del = true;
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_CLOUD_FILTER;
	} else {
		err = -EINVAL;
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);

	return err;
}

/**
 * iavf_setup_tc_cls_flower - flower classifier offloads
 * @adapter: pointer to iavf adapter structure
 * @cls_flower: pointer to flow_cls_offload struct with flow info
 */
static int iavf_setup_tc_cls_flower(struct iavf_adapter *adapter,
				    struct flow_cls_offload *cls_flower)
{
	switch (cls_flower->command) {
	case FLOW_CLS_REPLACE:
		return iavf_configure_clsflower(adapter, cls_flower);
	case FLOW_CLS_DESTROY:
		return iavf_delete_clsflower(adapter, cls_flower);
	case FLOW_CLS_STATS:
		return -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * iavf_add_cls_u32 - Add U32 classifier offloads
 * @adapter: pointer to iavf adapter structure
 * @cls_u32: pointer to tc_cls_u32_offload struct with flow info
 *
 * Return: 0 on success or negative errno on failure.
 */
static int iavf_add_cls_u32(struct iavf_adapter *adapter,
			    struct tc_cls_u32_offload *cls_u32)
{
	struct netlink_ext_ack *extack = cls_u32->common.extack;
	struct virtchnl_fdir_rule *rule_cfg;
	struct virtchnl_filter_action *vact;
	struct virtchnl_proto_hdrs *hdrs;
	struct ethhdr *spec_h, *mask_h;
	const struct tc_action *act;
	struct iavf_fdir_fltr *fltr;
	struct tcf_exts *exts;
	unsigned int q_index;
	int i, status = 0;
	int off_base = 0;

	if (cls_u32->knode.link_handle) {
		NL_SET_ERR_MSG_MOD(extack, "Linking not supported");
		return -EOPNOTSUPP;
	}

	fltr = kzalloc(sizeof(*fltr), GFP_KERNEL);
	if (!fltr)
		return -ENOMEM;

	rule_cfg = &fltr->vc_add_msg.rule_cfg;
	hdrs = &rule_cfg->proto_hdrs;
	hdrs->count = 0;

	/* The parser lib at the PF expects the packet starting with MAC hdr */
	switch (ntohs(cls_u32->common.protocol)) {
	case ETH_P_802_3:
		break;
	case ETH_P_IP:
		spec_h = (struct ethhdr *)hdrs->raw.spec;
		mask_h = (struct ethhdr *)hdrs->raw.mask;
		spec_h->h_proto = htons(ETH_P_IP);
		mask_h->h_proto = htons(0xFFFF);
		off_base += ETH_HLEN;
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack, "Only 802_3 and ip filter protocols are supported");
		status = -EOPNOTSUPP;
		goto free_alloc;
	}

	for (i = 0; i < cls_u32->knode.sel->nkeys; i++) {
		__be32 val, mask;
		int off;

		off = off_base + cls_u32->knode.sel->keys[i].off;
		val = cls_u32->knode.sel->keys[i].val;
		mask = cls_u32->knode.sel->keys[i].mask;

		if (off >= sizeof(hdrs->raw.spec)) {
			NL_SET_ERR_MSG_MOD(extack, "Input exceeds maximum allowed.");
			status = -EINVAL;
			goto free_alloc;
		}

		memcpy(&hdrs->raw.spec[off], &val, sizeof(val));
		memcpy(&hdrs->raw.mask[off], &mask, sizeof(mask));
		hdrs->raw.pkt_len = off + sizeof(val);
	}

	/* Only one action is allowed */
	rule_cfg->action_set.count = 1;
	vact = &rule_cfg->action_set.actions[0];
	exts = cls_u32->knode.exts;

	tcf_exts_for_each_action(i, act, exts) {
		/* FDIR queue */
		if (is_tcf_skbedit_rx_queue_mapping(act)) {
			q_index = tcf_skbedit_rx_queue_mapping(act);
			if (q_index >= adapter->num_active_queues) {
				status = -EINVAL;
				goto free_alloc;
			}

			vact->type = VIRTCHNL_ACTION_QUEUE;
			vact->act_conf.queue.index = q_index;
			break;
		}

		/* Drop */
		if (is_tcf_gact_shot(act)) {
			vact->type = VIRTCHNL_ACTION_DROP;
			break;
		}

		/* Unsupported action */
		NL_SET_ERR_MSG_MOD(extack, "Unsupported action.");
		status = -EOPNOTSUPP;
		goto free_alloc;
	}

	fltr->vc_add_msg.vsi_id = adapter->vsi.id;
	fltr->cls_u32_handle = cls_u32->knode.handle;
	return iavf_fdir_add_fltr(adapter, fltr);

free_alloc:
	kfree(fltr);
	return status;
}

/**
 * iavf_del_cls_u32 - Delete U32 classifier offloads
 * @adapter: pointer to iavf adapter structure
 * @cls_u32: pointer to tc_cls_u32_offload struct with flow info
 *
 * Return: 0 on success or negative errno on failure.
 */
static int iavf_del_cls_u32(struct iavf_adapter *adapter,
			    struct tc_cls_u32_offload *cls_u32)
{
	return iavf_fdir_del_fltr(adapter, true, cls_u32->knode.handle);
}

/**
 * iavf_setup_tc_cls_u32 - U32 filter offloads
 * @adapter: pointer to iavf adapter structure
 * @cls_u32: pointer to tc_cls_u32_offload struct with flow info
 *
 * Return: 0 on success or negative errno on failure.
 */
static int iavf_setup_tc_cls_u32(struct iavf_adapter *adapter,
				 struct tc_cls_u32_offload *cls_u32)
{
	if (!TC_U32_SUPPORT(adapter) || !FDIR_FLTR_SUPPORT(adapter))
		return -EOPNOTSUPP;

	switch (cls_u32->command) {
	case TC_CLSU32_NEW_KNODE:
	case TC_CLSU32_REPLACE_KNODE:
		return iavf_add_cls_u32(adapter, cls_u32);
	case TC_CLSU32_DELETE_KNODE:
		return iavf_del_cls_u32(adapter, cls_u32);
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * iavf_setup_tc_block_cb - block callback for tc
 * @type: type of offload
 * @type_data: offload data
 * @cb_priv:
 *
 * This function is the block callback for traffic classes
 **/
static int iavf_setup_tc_block_cb(enum tc_setup_type type, void *type_data,
				  void *cb_priv)
{
	struct iavf_adapter *adapter = cb_priv;

	if (!tc_cls_can_offload_and_chain0(adapter->netdev, type_data))
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return iavf_setup_tc_cls_flower(cb_priv, type_data);
	case TC_SETUP_CLSU32:
		return iavf_setup_tc_cls_u32(cb_priv, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

static LIST_HEAD(iavf_block_cb_list);

/**
 * iavf_setup_tc - configure multiple traffic classes
 * @netdev: network interface device structure
 * @type: type of offload
 * @type_data: tc offload data
 *
 * This function is the callback to ndo_setup_tc in the
 * netdev_ops.
 *
 * Returns 0 on success
 **/
static int iavf_setup_tc(struct net_device *netdev, enum tc_setup_type type,
			 void *type_data)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	switch (type) {
	case TC_SETUP_QDISC_MQPRIO:
		return __iavf_setup_tc(netdev, type_data);
	case TC_SETUP_BLOCK:
		return flow_block_cb_setup_simple(type_data,
						  &iavf_block_cb_list,
						  iavf_setup_tc_block_cb,
						  adapter, adapter, true);
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * iavf_restore_fdir_filters
 * @adapter: board private structure
 *
 * Restore existing FDIR filters when VF netdev comes back up.
 **/
static void iavf_restore_fdir_filters(struct iavf_adapter *adapter)
{
	struct iavf_fdir_fltr *f;

	spin_lock_bh(&adapter->fdir_fltr_lock);
	list_for_each_entry(f, &adapter->fdir_list_head, list) {
		if (f->state == IAVF_FDIR_FLTR_DIS_REQUEST) {
			/* Cancel a request, keep filter as active */
			f->state = IAVF_FDIR_FLTR_ACTIVE;
		} else if (f->state == IAVF_FDIR_FLTR_DIS_PENDING ||
			   f->state == IAVF_FDIR_FLTR_INACTIVE) {
			/* Add filters which are inactive or have a pending
			 * request to PF to be deleted
			 */
			f->state = IAVF_FDIR_FLTR_ADD_REQUEST;
			adapter->aq_required |= IAVF_FLAG_AQ_ADD_FDIR_FILTER;
		}
	}
	spin_unlock_bh(&adapter->fdir_fltr_lock);
}

/**
 * iavf_open - Called when a network interface is made active
 * @netdev: network interface device structure
 *
 * Returns 0 on success, negative value on failure
 *
 * The open entry point is called when a network interface is made
 * active by the system (IFF_UP).  At this point all resources needed
 * for transmit and receive operations are allocated, the interrupt
 * handler is registered with the OS, the watchdog is started,
 * and the stack is notified that the interface is ready.
 **/
static int iavf_open(struct net_device *netdev)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	int err;

	netdev_assert_locked(netdev);

	if (adapter->flags & IAVF_FLAG_PF_COMMS_FAILED) {
		dev_err(&adapter->pdev->dev, "Unable to open device due to PF driver failure.\n");
		return -EIO;
	}

	if (adapter->state != __IAVF_DOWN)
		return -EBUSY;

	if (adapter->state == __IAVF_RUNNING &&
	    !test_bit(__IAVF_VSI_DOWN, adapter->vsi.state)) {
		dev_dbg(&adapter->pdev->dev, "VF is already open.\n");
		return 0;
	}

	/* allocate transmit descriptors */
	err = iavf_setup_all_tx_resources(adapter);
	if (err)
		goto err_setup_tx;

	/* allocate receive descriptors */
	err = iavf_setup_all_rx_resources(adapter);
	if (err)
		goto err_setup_rx;

	/* clear any pending interrupts, may auto mask */
	err = iavf_request_traffic_irqs(adapter, netdev->name);
	if (err)
		goto err_req_irq;

	spin_lock_bh(&adapter->mac_vlan_list_lock);
	iavf_add_filter(adapter, adapter->hw.mac.addr);
	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	/* Restore filters that were removed with IFF_DOWN */
	iavf_restore_filters(adapter);
	iavf_restore_fdir_filters(adapter);

	iavf_configure(adapter);

	iavf_up_complete(adapter);

	iavf_irq_enable(adapter, true);

	return 0;

err_req_irq:
	iavf_down(adapter);
	iavf_free_traffic_irqs(adapter);
err_setup_rx:
	iavf_free_all_rx_resources(adapter);
err_setup_tx:
	iavf_free_all_tx_resources(adapter);

	return err;
}

/**
 * iavf_close - Disables a network interface
 * @netdev: network interface device structure
 *
 * Returns 0, this is not allowed to fail
 *
 * The close entry point is called when an interface is de-activated
 * by the OS.  The hardware is still under the drivers control, but
 * needs to be disabled. All IRQs except vector 0 (reserved for admin queue)
 * are freed, along with all transmit and receive resources.
 **/
static int iavf_close(struct net_device *netdev)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	u64 aq_to_restore;
	int status;

	netdev_assert_locked(netdev);

	if (adapter->state <= __IAVF_DOWN_PENDING)
		return 0;

	set_bit(__IAVF_VSI_DOWN, adapter->vsi.state);
	/* We cannot send IAVF_FLAG_AQ_GET_OFFLOAD_VLAN_V2_CAPS before
	 * IAVF_FLAG_AQ_DISABLE_QUEUES because in such case there is rtnl
	 * deadlock with adminq_task() until iavf_close timeouts. We must send
	 * IAVF_FLAG_AQ_GET_CONFIG before IAVF_FLAG_AQ_DISABLE_QUEUES to make
	 * disable queues possible for vf. Give only necessary flags to
	 * iavf_down and save other to set them right before iavf_close()
	 * returns, when IAVF_FLAG_AQ_DISABLE_QUEUES will be already sent and
	 * iavf will be in DOWN state.
	 */
	aq_to_restore = adapter->aq_required;
	adapter->aq_required &= IAVF_FLAG_AQ_GET_CONFIG;

	/* Remove flags which we do not want to send after close or we want to
	 * send before disable queues.
	 */
	aq_to_restore &= ~(IAVF_FLAG_AQ_GET_CONFIG		|
			   IAVF_FLAG_AQ_ENABLE_QUEUES		|
			   IAVF_FLAG_AQ_CONFIGURE_QUEUES	|
			   IAVF_FLAG_AQ_ADD_VLAN_FILTER		|
			   IAVF_FLAG_AQ_ADD_MAC_FILTER		|
			   IAVF_FLAG_AQ_ADD_CLOUD_FILTER	|
			   IAVF_FLAG_AQ_ADD_FDIR_FILTER		|
			   IAVF_FLAG_AQ_ADD_ADV_RSS_CFG);

	iavf_down(adapter);
	iavf_change_state(adapter, __IAVF_DOWN_PENDING);
	iavf_free_traffic_irqs(adapter);

	netdev_unlock(netdev);

	/* We explicitly don't free resources here because the hardware is
	 * still active and can DMA into memory. Resources are cleared in
	 * iavf_virtchnl_completion() after we get confirmation from the PF
	 * driver that the rings have been stopped.
	 *
	 * Also, we wait for state to transition to __IAVF_DOWN before
	 * returning. State change occurs in iavf_virtchnl_completion() after
	 * VF resources are released (which occurs after PF driver processes and
	 * responds to admin queue commands).
	 */

	status = wait_event_timeout(adapter->down_waitqueue,
				    adapter->state == __IAVF_DOWN,
				    msecs_to_jiffies(500));
	if (!status)
		netdev_warn(netdev, "Device resources not yet released\n");
	netdev_lock(netdev);

	adapter->aq_required |= aq_to_restore;

	return 0;
}

/**
 * iavf_change_mtu - Change the Maximum Transfer Unit
 * @netdev: network interface device structure
 * @new_mtu: new value for maximum frame size
 *
 * Returns 0 on success, negative on failure
 **/
static int iavf_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	int ret = 0;

	netdev_dbg(netdev, "changing MTU from %d to %d\n",
		   netdev->mtu, new_mtu);
	WRITE_ONCE(netdev->mtu, new_mtu);

	if (netif_running(netdev)) {
		iavf_schedule_reset(adapter, IAVF_FLAG_RESET_NEEDED);
		ret = iavf_wait_for_reset(adapter);
		if (ret < 0)
			netdev_warn(netdev, "MTU change interrupted waiting for reset");
		else if (ret)
			netdev_warn(netdev, "MTU change timed out waiting for reset");
	}

	return ret;
}

/**
 * iavf_disable_fdir - disable Flow Director and clear existing filters
 * @adapter: board private structure
 **/
static void iavf_disable_fdir(struct iavf_adapter *adapter)
{
	struct iavf_fdir_fltr *fdir, *fdirtmp;
	bool del_filters = false;

	adapter->flags &= ~IAVF_FLAG_FDIR_ENABLED;

	/* remove all Flow Director filters */
	spin_lock_bh(&adapter->fdir_fltr_lock);
	list_for_each_entry_safe(fdir, fdirtmp, &adapter->fdir_list_head,
				 list) {
		if (fdir->state == IAVF_FDIR_FLTR_ADD_REQUEST ||
		    fdir->state == IAVF_FDIR_FLTR_INACTIVE) {
			/* Delete filters not registered in PF */
			list_del(&fdir->list);
			iavf_dec_fdir_active_fltr(adapter, fdir);
			kfree(fdir);
		} else if (fdir->state == IAVF_FDIR_FLTR_ADD_PENDING ||
			   fdir->state == IAVF_FDIR_FLTR_DIS_REQUEST ||
			   fdir->state == IAVF_FDIR_FLTR_ACTIVE) {
			/* Filters registered in PF, schedule their deletion */
			fdir->state = IAVF_FDIR_FLTR_DEL_REQUEST;
			del_filters = true;
		} else if (fdir->state == IAVF_FDIR_FLTR_DIS_PENDING) {
			/* Request to delete filter already sent to PF, change
			 * state to DEL_PENDING to delete filter after PF's
			 * response, not set as INACTIVE
			 */
			fdir->state = IAVF_FDIR_FLTR_DEL_PENDING;
		}
	}
	spin_unlock_bh(&adapter->fdir_fltr_lock);

	if (del_filters) {
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_FDIR_FILTER;
		mod_delayed_work(adapter->wq, &adapter->watchdog_task, 0);
	}
}

#define NETIF_VLAN_OFFLOAD_FEATURES	(NETIF_F_HW_VLAN_CTAG_RX | \
					 NETIF_F_HW_VLAN_CTAG_TX | \
					 NETIF_F_HW_VLAN_STAG_RX | \
					 NETIF_F_HW_VLAN_STAG_TX)

/**
 * iavf_set_features - set the netdev feature flags
 * @netdev: ptr to the netdev being adjusted
 * @features: the feature set that the stack is suggesting
 * Note: expects to be called while under rtnl_lock()
 **/
static int iavf_set_features(struct net_device *netdev,
			     netdev_features_t features)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	/* trigger update on any VLAN feature change */
	if ((netdev->features & NETIF_VLAN_OFFLOAD_FEATURES) ^
	    (features & NETIF_VLAN_OFFLOAD_FEATURES))
		iavf_set_vlan_offload_features(adapter, netdev->features,
					       features);
	if (CRC_OFFLOAD_ALLOWED(adapter) &&
	    ((netdev->features & NETIF_F_RXFCS) ^ (features & NETIF_F_RXFCS)))
		iavf_schedule_reset(adapter, IAVF_FLAG_RESET_NEEDED);

	if ((netdev->features & NETIF_F_NTUPLE) ^ (features & NETIF_F_NTUPLE)) {
		if (features & NETIF_F_NTUPLE)
			adapter->flags |= IAVF_FLAG_FDIR_ENABLED;
		else
			iavf_disable_fdir(adapter);
	}

	return 0;
}

/**
 * iavf_features_check - Validate encapsulated packet conforms to limits
 * @skb: skb buff
 * @dev: This physical port's netdev
 * @features: Offload features that the stack believes apply
 **/
static netdev_features_t iavf_features_check(struct sk_buff *skb,
					     struct net_device *dev,
					     netdev_features_t features)
{
	size_t len;

	/* No point in doing any of this if neither checksum nor GSO are
	 * being requested for this frame.  We can rule out both by just
	 * checking for CHECKSUM_PARTIAL
	 */
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return features;

	/* We cannot support GSO if the MSS is going to be less than
	 * 64 bytes.  If it is then we need to drop support for GSO.
	 */
	if (skb_is_gso(skb) && (skb_shinfo(skb)->gso_size < 64))
		features &= ~NETIF_F_GSO_MASK;

	/* MACLEN can support at most 63 words */
	len = skb_network_offset(skb);
	if (len & ~(63 * 2))
		goto out_err;

	/* IPLEN and EIPLEN can support at most 127 dwords */
	len = skb_network_header_len(skb);
	if (len & ~(127 * 4))
		goto out_err;

	if (skb->encapsulation) {
		/* L4TUNLEN can support 127 words */
		len = skb_inner_network_header(skb) - skb_transport_header(skb);
		if (len & ~(127 * 2))
			goto out_err;

		/* IPLEN can support at most 127 dwords */
		len = skb_inner_transport_header(skb) -
		      skb_inner_network_header(skb);
		if (len & ~(127 * 4))
			goto out_err;
	}

	/* No need to validate L4LEN as TCP is the only protocol with a
	 * flexible value and we support all possible values supported
	 * by TCP, which is at most 15 dwords
	 */

	return features;
out_err:
	return features & ~(NETIF_F_CSUM_MASK | NETIF_F_GSO_MASK);
}

/**
 * iavf_get_netdev_vlan_hw_features - get NETDEV VLAN features that can toggle on/off
 * @adapter: board private structure
 *
 * Depending on whether VIRTHCNL_VF_OFFLOAD_VLAN or VIRTCHNL_VF_OFFLOAD_VLAN_V2
 * were negotiated determine the VLAN features that can be toggled on and off.
 **/
static netdev_features_t
iavf_get_netdev_vlan_hw_features(struct iavf_adapter *adapter)
{
	netdev_features_t hw_features = 0;

	if (!adapter->vf_res || !adapter->vf_res->vf_cap_flags)
		return hw_features;

	/* Enable VLAN features if supported */
	if (VLAN_ALLOWED(adapter)) {
		hw_features |= (NETIF_F_HW_VLAN_CTAG_TX |
				NETIF_F_HW_VLAN_CTAG_RX);
	} else if (VLAN_V2_ALLOWED(adapter)) {
		struct virtchnl_vlan_caps *vlan_v2_caps =
			&adapter->vlan_v2_caps;
		struct virtchnl_vlan_supported_caps *stripping_support =
			&vlan_v2_caps->offloads.stripping_support;
		struct virtchnl_vlan_supported_caps *insertion_support =
			&vlan_v2_caps->offloads.insertion_support;

		if (stripping_support->outer != VIRTCHNL_VLAN_UNSUPPORTED &&
		    stripping_support->outer & VIRTCHNL_VLAN_TOGGLE) {
			if (stripping_support->outer &
			    VIRTCHNL_VLAN_ETHERTYPE_8100)
				hw_features |= NETIF_F_HW_VLAN_CTAG_RX;
			if (stripping_support->outer &
			    VIRTCHNL_VLAN_ETHERTYPE_88A8)
				hw_features |= NETIF_F_HW_VLAN_STAG_RX;
		} else if (stripping_support->inner !=
			   VIRTCHNL_VLAN_UNSUPPORTED &&
			   stripping_support->inner & VIRTCHNL_VLAN_TOGGLE) {
			if (stripping_support->inner &
			    VIRTCHNL_VLAN_ETHERTYPE_8100)
				hw_features |= NETIF_F_HW_VLAN_CTAG_RX;
		}

		if (insertion_support->outer != VIRTCHNL_VLAN_UNSUPPORTED &&
		    insertion_support->outer & VIRTCHNL_VLAN_TOGGLE) {
			if (insertion_support->outer &
			    VIRTCHNL_VLAN_ETHERTYPE_8100)
				hw_features |= NETIF_F_HW_VLAN_CTAG_TX;
			if (insertion_support->outer &
			    VIRTCHNL_VLAN_ETHERTYPE_88A8)
				hw_features |= NETIF_F_HW_VLAN_STAG_TX;
		} else if (insertion_support->inner &&
			   insertion_support->inner & VIRTCHNL_VLAN_TOGGLE) {
			if (insertion_support->inner &
			    VIRTCHNL_VLAN_ETHERTYPE_8100)
				hw_features |= NETIF_F_HW_VLAN_CTAG_TX;
		}
	}

	if (CRC_OFFLOAD_ALLOWED(adapter))
		hw_features |= NETIF_F_RXFCS;

	return hw_features;
}

/**
 * iavf_get_netdev_vlan_features - get the enabled NETDEV VLAN fetures
 * @adapter: board private structure
 *
 * Depending on whether VIRTHCNL_VF_OFFLOAD_VLAN or VIRTCHNL_VF_OFFLOAD_VLAN_V2
 * were negotiated determine the VLAN features that are enabled by default.
 **/
static netdev_features_t
iavf_get_netdev_vlan_features(struct iavf_adapter *adapter)
{
	netdev_features_t features = 0;

	if (!adapter->vf_res || !adapter->vf_res->vf_cap_flags)
		return features;

	if (VLAN_ALLOWED(adapter)) {
		features |= NETIF_F_HW_VLAN_CTAG_FILTER |
			NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX;
	} else if (VLAN_V2_ALLOWED(adapter)) {
		struct virtchnl_vlan_caps *vlan_v2_caps =
			&adapter->vlan_v2_caps;
		struct virtchnl_vlan_supported_caps *filtering_support =
			&vlan_v2_caps->filtering.filtering_support;
		struct virtchnl_vlan_supported_caps *stripping_support =
			&vlan_v2_caps->offloads.stripping_support;
		struct virtchnl_vlan_supported_caps *insertion_support =
			&vlan_v2_caps->offloads.insertion_support;
		u32 ethertype_init;

		/* give priority to outer stripping and don't support both outer
		 * and inner stripping
		 */
		ethertype_init = vlan_v2_caps->offloads.ethertype_init;
		if (stripping_support->outer != VIRTCHNL_VLAN_UNSUPPORTED) {
			if (stripping_support->outer &
			    VIRTCHNL_VLAN_ETHERTYPE_8100 &&
			    ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_8100)
				features |= NETIF_F_HW_VLAN_CTAG_RX;
			else if (stripping_support->outer &
				 VIRTCHNL_VLAN_ETHERTYPE_88A8 &&
				 ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_88A8)
				features |= NETIF_F_HW_VLAN_STAG_RX;
		} else if (stripping_support->inner !=
			   VIRTCHNL_VLAN_UNSUPPORTED) {
			if (stripping_support->inner &
			    VIRTCHNL_VLAN_ETHERTYPE_8100 &&
			    ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_8100)
				features |= NETIF_F_HW_VLAN_CTAG_RX;
		}

		/* give priority to outer insertion and don't support both outer
		 * and inner insertion
		 */
		if (insertion_support->outer != VIRTCHNL_VLAN_UNSUPPORTED) {
			if (insertion_support->outer &
			    VIRTCHNL_VLAN_ETHERTYPE_8100 &&
			    ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_8100)
				features |= NETIF_F_HW_VLAN_CTAG_TX;
			else if (insertion_support->outer &
				 VIRTCHNL_VLAN_ETHERTYPE_88A8 &&
				 ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_88A8)
				features |= NETIF_F_HW_VLAN_STAG_TX;
		} else if (insertion_support->inner !=
			   VIRTCHNL_VLAN_UNSUPPORTED) {
			if (insertion_support->inner &
			    VIRTCHNL_VLAN_ETHERTYPE_8100 &&
			    ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_8100)
				features |= NETIF_F_HW_VLAN_CTAG_TX;
		}

		/* give priority to outer filtering and don't bother if both
		 * outer and inner filtering are enabled
		 */
		ethertype_init = vlan_v2_caps->filtering.ethertype_init;
		if (filtering_support->outer != VIRTCHNL_VLAN_UNSUPPORTED) {
			if (filtering_support->outer &
			    VIRTCHNL_VLAN_ETHERTYPE_8100 &&
			    ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_8100)
				features |= NETIF_F_HW_VLAN_CTAG_FILTER;
			if (filtering_support->outer &
			    VIRTCHNL_VLAN_ETHERTYPE_88A8 &&
			    ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_88A8)
				features |= NETIF_F_HW_VLAN_STAG_FILTER;
		} else if (filtering_support->inner !=
			   VIRTCHNL_VLAN_UNSUPPORTED) {
			if (filtering_support->inner &
			    VIRTCHNL_VLAN_ETHERTYPE_8100 &&
			    ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_8100)
				features |= NETIF_F_HW_VLAN_CTAG_FILTER;
			if (filtering_support->inner &
			    VIRTCHNL_VLAN_ETHERTYPE_88A8 &&
			    ethertype_init & VIRTCHNL_VLAN_ETHERTYPE_88A8)
				features |= NETIF_F_HW_VLAN_STAG_FILTER;
		}
	}

	return features;
}

#define IAVF_NETDEV_VLAN_FEATURE_ALLOWED(requested, allowed, feature_bit) \
	(!(((requested) & (feature_bit)) && \
	   !((allowed) & (feature_bit))))

/**
 * iavf_fix_netdev_vlan_features - fix NETDEV VLAN features based on support
 * @adapter: board private structure
 * @requested_features: stack requested NETDEV features
 **/
static netdev_features_t
iavf_fix_netdev_vlan_features(struct iavf_adapter *adapter,
			      netdev_features_t requested_features)
{
	netdev_features_t allowed_features;

	allowed_features = iavf_get_netdev_vlan_hw_features(adapter) |
		iavf_get_netdev_vlan_features(adapter);

	if (!IAVF_NETDEV_VLAN_FEATURE_ALLOWED(requested_features,
					      allowed_features,
					      NETIF_F_HW_VLAN_CTAG_TX))
		requested_features &= ~NETIF_F_HW_VLAN_CTAG_TX;

	if (!IAVF_NETDEV_VLAN_FEATURE_ALLOWED(requested_features,
					      allowed_features,
					      NETIF_F_HW_VLAN_CTAG_RX))
		requested_features &= ~NETIF_F_HW_VLAN_CTAG_RX;

	if (!IAVF_NETDEV_VLAN_FEATURE_ALLOWED(requested_features,
					      allowed_features,
					      NETIF_F_HW_VLAN_STAG_TX))
		requested_features &= ~NETIF_F_HW_VLAN_STAG_TX;
	if (!IAVF_NETDEV_VLAN_FEATURE_ALLOWED(requested_features,
					      allowed_features,
					      NETIF_F_HW_VLAN_STAG_RX))
		requested_features &= ~NETIF_F_HW_VLAN_STAG_RX;

	if (!IAVF_NETDEV_VLAN_FEATURE_ALLOWED(requested_features,
					      allowed_features,
					      NETIF_F_HW_VLAN_CTAG_FILTER))
		requested_features &= ~NETIF_F_HW_VLAN_CTAG_FILTER;

	if (!IAVF_NETDEV_VLAN_FEATURE_ALLOWED(requested_features,
					      allowed_features,
					      NETIF_F_HW_VLAN_STAG_FILTER))
		requested_features &= ~NETIF_F_HW_VLAN_STAG_FILTER;

	if ((requested_features &
	     (NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX)) &&
	    (requested_features &
	     (NETIF_F_HW_VLAN_STAG_RX | NETIF_F_HW_VLAN_STAG_TX)) &&
	    adapter->vlan_v2_caps.offloads.ethertype_match ==
	    VIRTCHNL_ETHERTYPE_STRIPPING_MATCHES_INSERTION) {
		netdev_warn(adapter->netdev, "cannot support CTAG and STAG VLAN stripping and/or insertion simultaneously since CTAG and STAG offloads are mutually exclusive, clearing STAG offload settings\n");
		requested_features &= ~(NETIF_F_HW_VLAN_STAG_RX |
					NETIF_F_HW_VLAN_STAG_TX);
	}

	return requested_features;
}

/**
 * iavf_fix_strip_features - fix NETDEV CRC and VLAN strip features
 * @adapter: board private structure
 * @requested_features: stack requested NETDEV features
 *
 * Returns fixed-up features bits
 **/
static netdev_features_t
iavf_fix_strip_features(struct iavf_adapter *adapter,
			netdev_features_t requested_features)
{
	struct net_device *netdev = adapter->netdev;
	bool crc_offload_req, is_vlan_strip;
	netdev_features_t vlan_strip;
	int num_non_zero_vlan;

	crc_offload_req = CRC_OFFLOAD_ALLOWED(adapter) &&
			  (requested_features & NETIF_F_RXFCS);
	num_non_zero_vlan = iavf_get_num_vlans_added(adapter);
	vlan_strip = (NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_STAG_RX);
	is_vlan_strip = requested_features & vlan_strip;

	if (!crc_offload_req)
		return requested_features;

	if (!num_non_zero_vlan && (netdev->features & vlan_strip) &&
	    !(netdev->features & NETIF_F_RXFCS) && is_vlan_strip) {
		requested_features &= ~vlan_strip;
		netdev_info(netdev, "Disabling VLAN stripping as FCS/CRC stripping is also disabled and there is no VLAN configured\n");
		return requested_features;
	}

	if ((netdev->features & NETIF_F_RXFCS) && is_vlan_strip) {
		requested_features &= ~vlan_strip;
		if (!(netdev->features & vlan_strip))
			netdev_info(netdev, "To enable VLAN stripping, first need to enable FCS/CRC stripping");

		return requested_features;
	}

	if (num_non_zero_vlan && is_vlan_strip &&
	    !(netdev->features & NETIF_F_RXFCS)) {
		requested_features &= ~NETIF_F_RXFCS;
		netdev_info(netdev, "To disable FCS/CRC stripping, first need to disable VLAN stripping");
	}

	return requested_features;
}

/**
 * iavf_fix_features - fix up the netdev feature bits
 * @netdev: our net device
 * @features: desired feature bits
 *
 * Returns fixed-up features bits
 **/
static netdev_features_t iavf_fix_features(struct net_device *netdev,
					   netdev_features_t features)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	features = iavf_fix_netdev_vlan_features(adapter, features);

	if (!FDIR_FLTR_SUPPORT(adapter))
		features &= ~NETIF_F_NTUPLE;

	return iavf_fix_strip_features(adapter, features);
}

static int iavf_hwstamp_get(struct net_device *netdev,
			    struct kernel_hwtstamp_config *config)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	*config = adapter->ptp.hwtstamp_config;

	return 0;
}

static int iavf_hwstamp_set(struct net_device *netdev,
			    struct kernel_hwtstamp_config *config,
			    struct netlink_ext_ack *extack)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	return iavf_ptp_set_ts_config(adapter, config, extack);
}

static int
iavf_verify_shaper(struct net_shaper_binding *binding,
		   const struct net_shaper *shaper,
		   struct netlink_ext_ack *extack)
{
	struct iavf_adapter *adapter = netdev_priv(binding->netdev);
	u64 vf_max;

	if (shaper->handle.scope == NET_SHAPER_SCOPE_QUEUE) {
		vf_max = adapter->qos_caps->cap[0].shaper.peak;
		if (vf_max && shaper->bw_max > vf_max) {
			NL_SET_ERR_MSG_FMT(extack, "Max rate (%llu) of queue %d can't exceed max TX rate of VF (%llu kbps)",
					   shaper->bw_max, shaper->handle.id,
					   vf_max);
			return -EINVAL;
		}
	}
	return 0;
}

static int
iavf_shaper_set(struct net_shaper_binding *binding,
		const struct net_shaper *shaper,
		struct netlink_ext_ack *extack)
{
	struct iavf_adapter *adapter = netdev_priv(binding->netdev);
	const struct net_shaper_handle *handle = &shaper->handle;
	struct iavf_ring *tx_ring;
	int ret;

	netdev_assert_locked(adapter->netdev);

	if (handle->id >= adapter->num_active_queues)
		return 0;

	ret = iavf_verify_shaper(binding, shaper, extack);
	if (ret)
		return ret;

	tx_ring = &adapter->tx_rings[handle->id];

	tx_ring->q_shaper.bw_min = div_u64(shaper->bw_min, 1000);
	tx_ring->q_shaper.bw_max = div_u64(shaper->bw_max, 1000);
	tx_ring->q_shaper_update = true;

	adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_QUEUES_BW;

	return 0;
}

static int iavf_shaper_del(struct net_shaper_binding *binding,
			   const struct net_shaper_handle *handle,
			   struct netlink_ext_ack *extack)
{
	struct iavf_adapter *adapter = netdev_priv(binding->netdev);
	struct iavf_ring *tx_ring;

	netdev_assert_locked(adapter->netdev);

	if (handle->id >= adapter->num_active_queues)
		return 0;

	tx_ring = &adapter->tx_rings[handle->id];
	tx_ring->q_shaper.bw_min = 0;
	tx_ring->q_shaper.bw_max = 0;
	tx_ring->q_shaper_update = true;

	adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_QUEUES_BW;

	return 0;
}

static void iavf_shaper_cap(struct net_shaper_binding *binding,
			    enum net_shaper_scope scope,
			    unsigned long *flags)
{
	if (scope != NET_SHAPER_SCOPE_QUEUE)
		return;

	*flags = BIT(NET_SHAPER_A_CAPS_SUPPORT_BW_MIN) |
		 BIT(NET_SHAPER_A_CAPS_SUPPORT_BW_MAX) |
		 BIT(NET_SHAPER_A_CAPS_SUPPORT_METRIC_BPS);
}

static const struct net_shaper_ops iavf_shaper_ops = {
	.set = iavf_shaper_set,
	.delete = iavf_shaper_del,
	.capabilities = iavf_shaper_cap,
};

static const struct net_device_ops iavf_netdev_ops = {
	.ndo_open		= iavf_open,
	.ndo_stop		= iavf_close,
	.ndo_start_xmit		= iavf_xmit_frame,
	.ndo_set_rx_mode	= iavf_set_rx_mode,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= iavf_set_mac,
	.ndo_change_mtu		= iavf_change_mtu,
	.ndo_tx_timeout		= iavf_tx_timeout,
	.ndo_vlan_rx_add_vid	= iavf_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= iavf_vlan_rx_kill_vid,
	.ndo_features_check	= iavf_features_check,
	.ndo_fix_features	= iavf_fix_features,
	.ndo_set_features	= iavf_set_features,
	.ndo_setup_tc		= iavf_setup_tc,
	.net_shaper_ops		= &iavf_shaper_ops,
	.ndo_hwtstamp_get	= iavf_hwstamp_get,
	.ndo_hwtstamp_set	= iavf_hwstamp_set,
};

/**
 * iavf_check_reset_complete - check that VF reset is complete
 * @hw: pointer to hw struct
 *
 * Returns 0 if device is ready to use, or -EBUSY if it's in reset.
 **/
static int iavf_check_reset_complete(struct iavf_hw *hw)
{
	u32 rstat;
	int i;

	for (i = 0; i < IAVF_RESET_WAIT_COMPLETE_COUNT; i++) {
		rstat = rd32(hw, IAVF_VFGEN_RSTAT) &
			     IAVF_VFGEN_RSTAT_VFR_STATE_MASK;
		if ((rstat == VIRTCHNL_VFR_VFACTIVE) ||
		    (rstat == VIRTCHNL_VFR_COMPLETED))
			return 0;
		msleep(IAVF_RESET_WAIT_MS);
	}
	return -EBUSY;
}

/**
 * iavf_process_config - Process the config information we got from the PF
 * @adapter: board private structure
 *
 * Verify that we have a valid config struct, and set up our netdev features
 * and our VSI struct.
 **/
int iavf_process_config(struct iavf_adapter *adapter)
{
	struct virtchnl_vf_resource *vfres = adapter->vf_res;
	netdev_features_t hw_vlan_features, vlan_features;
	struct net_device *netdev = adapter->netdev;
	netdev_features_t hw_enc_features;
	netdev_features_t hw_features;

	hw_enc_features = NETIF_F_SG			|
			  NETIF_F_IP_CSUM		|
			  NETIF_F_IPV6_CSUM		|
			  NETIF_F_HIGHDMA		|
			  NETIF_F_SOFT_FEATURES	|
			  NETIF_F_TSO			|
			  NETIF_F_TSO_ECN		|
			  NETIF_F_TSO6			|
			  NETIF_F_SCTP_CRC		|
			  NETIF_F_RXHASH		|
			  NETIF_F_RXCSUM		|
			  0;

	/* advertise to stack only if offloads for encapsulated packets is
	 * supported
	 */
	if (vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ENCAP) {
		hw_enc_features |= NETIF_F_GSO_UDP_TUNNEL	|
				   NETIF_F_GSO_GRE		|
				   NETIF_F_GSO_GRE_CSUM		|
				   NETIF_F_GSO_IPXIP4		|
				   NETIF_F_GSO_IPXIP6		|
				   NETIF_F_GSO_UDP_TUNNEL_CSUM	|
				   NETIF_F_GSO_PARTIAL		|
				   0;

		if (!(vfres->vf_cap_flags &
		      VIRTCHNL_VF_OFFLOAD_ENCAP_CSUM))
			netdev->gso_partial_features |=
				NETIF_F_GSO_UDP_TUNNEL_CSUM;

		netdev->gso_partial_features |= NETIF_F_GSO_GRE_CSUM;
		netdev->hw_enc_features |= NETIF_F_TSO_MANGLEID;
		netdev->hw_enc_features |= hw_enc_features;
	}
	/* record features VLANs can make use of */
	netdev->vlan_features |= hw_enc_features | NETIF_F_TSO_MANGLEID;

	/* Write features and hw_features separately to avoid polluting
	 * with, or dropping, features that are set when we registered.
	 */
	hw_features = hw_enc_features;

	/* get HW VLAN features that can be toggled */
	hw_vlan_features = iavf_get_netdev_vlan_hw_features(adapter);

	/* Enable HW TC offload if ADQ or tc U32 is supported */
	if (vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ADQ ||
	    TC_U32_SUPPORT(adapter))
		hw_features |= NETIF_F_HW_TC;

	if (vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_USO)
		hw_features |= NETIF_F_GSO_UDP_L4;

	netdev->hw_features |= hw_features | hw_vlan_features;
	vlan_features = iavf_get_netdev_vlan_features(adapter);

	netdev->features |= hw_features | vlan_features;

	if (vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_VLAN)
		netdev->features |= NETIF_F_HW_VLAN_CTAG_FILTER;

	if (FDIR_FLTR_SUPPORT(adapter)) {
		netdev->hw_features |= NETIF_F_NTUPLE;
		netdev->features |= NETIF_F_NTUPLE;
		adapter->flags |= IAVF_FLAG_FDIR_ENABLED;
	}

	netdev->priv_flags |= IFF_UNICAST_FLT;

	/* Do not turn on offloads when they are requested to be turned off.
	 * TSO needs minimum 576 bytes to work correctly.
	 */
	if (netdev->wanted_features) {
		if (!(netdev->wanted_features & NETIF_F_TSO) ||
		    netdev->mtu < 576)
			netdev->features &= ~NETIF_F_TSO;
		if (!(netdev->wanted_features & NETIF_F_TSO6) ||
		    netdev->mtu < 576)
			netdev->features &= ~NETIF_F_TSO6;
		if (!(netdev->wanted_features & NETIF_F_TSO_ECN))
			netdev->features &= ~NETIF_F_TSO_ECN;
		if (!(netdev->wanted_features & NETIF_F_GRO))
			netdev->features &= ~NETIF_F_GRO;
		if (!(netdev->wanted_features & NETIF_F_GSO))
			netdev->features &= ~NETIF_F_GSO;
	}

	return 0;
}

/**
 * iavf_probe - Device Initialization Routine
 * @pdev: PCI device information struct
 * @ent: entry in iavf_pci_tbl
 *
 * Returns 0 on success, negative on failure
 *
 * iavf_probe initializes an adapter identified by a pci_dev structure.
 * The OS initialization, configuring of the adapter private structure,
 * and a hardware reset occur.
 **/
static int iavf_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct net_device *netdev;
	struct iavf_adapter *adapter = NULL;
	struct iavf_hw *hw = NULL;
	int err, len;

	err = pci_enable_device(pdev);
	if (err)
		return err;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(&pdev->dev,
			"DMA configuration failed: 0x%x\n", err);
		goto err_dma;
	}

	err = pci_request_regions(pdev, iavf_driver_name);
	if (err) {
		dev_err(&pdev->dev,
			"pci_request_regions failed 0x%x\n", err);
		goto err_pci_reg;
	}

	pci_set_master(pdev);

	netdev = alloc_etherdev_mq(sizeof(struct iavf_adapter),
				   IAVF_MAX_REQ_QUEUES);
	if (!netdev) {
		err = -ENOMEM;
		goto err_alloc_etherdev;
	}

	netif_set_affinity_auto(netdev);
	SET_NETDEV_DEV(netdev, &pdev->dev);

	pci_set_drvdata(pdev, netdev);
	adapter = netdev_priv(netdev);

	adapter->netdev = netdev;
	adapter->pdev = pdev;

	hw = &adapter->hw;
	hw->back = adapter;

	adapter->wq = alloc_ordered_workqueue("%s", WQ_MEM_RECLAIM,
					      iavf_driver_name);
	if (!adapter->wq) {
		err = -ENOMEM;
		goto err_alloc_wq;
	}

	adapter->msg_enable = BIT(DEFAULT_DEBUG_LEVEL_SHIFT) - 1;
	iavf_change_state(adapter, __IAVF_STARTUP);

	/* Call save state here because it relies on the adapter struct. */
	pci_save_state(pdev);

	hw->hw_addr = ioremap(pci_resource_start(pdev, 0),
			      pci_resource_len(pdev, 0));
	if (!hw->hw_addr) {
		err = -EIO;
		goto err_ioremap;
	}
	hw->vendor_id = pdev->vendor;
	hw->device_id = pdev->device;
	pci_read_config_byte(pdev, PCI_REVISION_ID, &hw->revision_id);
	hw->subsystem_vendor_id = pdev->subsystem_vendor;
	hw->subsystem_device_id = pdev->subsystem_device;
	hw->bus.device = PCI_SLOT(pdev->devfn);
	hw->bus.func = PCI_FUNC(pdev->devfn);
	hw->bus.bus_id = pdev->bus->number;

	len = struct_size(adapter->qos_caps, cap, IAVF_MAX_QOS_TC_NUM);
	adapter->qos_caps = kzalloc(len, GFP_KERNEL);
	if (!adapter->qos_caps) {
		err = -ENOMEM;
		goto err_alloc_qos_cap;
	}

	mutex_init(&hw->aq.asq_mutex);
	mutex_init(&hw->aq.arq_mutex);

	spin_lock_init(&adapter->mac_vlan_list_lock);
	spin_lock_init(&adapter->cloud_filter_list_lock);
	spin_lock_init(&adapter->fdir_fltr_lock);
	spin_lock_init(&adapter->adv_rss_lock);
	spin_lock_init(&adapter->current_netdev_promisc_flags_lock);

	INIT_LIST_HEAD(&adapter->mac_filter_list);
	INIT_LIST_HEAD(&adapter->vlan_filter_list);
	INIT_LIST_HEAD(&adapter->cloud_filter_list);
	INIT_LIST_HEAD(&adapter->fdir_list_head);
	INIT_LIST_HEAD(&adapter->adv_rss_list_head);

	INIT_WORK(&adapter->reset_task, iavf_reset_task);
	INIT_WORK(&adapter->adminq_task, iavf_adminq_task);
	INIT_WORK(&adapter->finish_config, iavf_finish_config);
	INIT_DELAYED_WORK(&adapter->watchdog_task, iavf_watchdog_task);

	/* Setup the wait queue for indicating transition to down status */
	init_waitqueue_head(&adapter->down_waitqueue);

	/* Setup the wait queue for indicating transition to running state */
	init_waitqueue_head(&adapter->reset_waitqueue);

	/* Setup the wait queue for indicating virtchannel events */
	init_waitqueue_head(&adapter->vc_waitqueue);

	INIT_LIST_HEAD(&adapter->ptp.aq_cmds);
	init_waitqueue_head(&adapter->ptp.phc_time_waitqueue);
	mutex_init(&adapter->ptp.aq_cmd_lock);

	queue_delayed_work(adapter->wq, &adapter->watchdog_task,
			   msecs_to_jiffies(5 * (pdev->devfn & 0x07)));
	/* Initialization goes on in the work. Do not add more of it below. */
	return 0;

err_alloc_qos_cap:
	iounmap(hw->hw_addr);
err_ioremap:
	destroy_workqueue(adapter->wq);
err_alloc_wq:
	free_netdev(netdev);
err_alloc_etherdev:
	pci_release_regions(pdev);
err_pci_reg:
err_dma:
	pci_disable_device(pdev);
	return err;
}

/**
 * iavf_suspend - Power management suspend routine
 * @dev_d: device info pointer
 *
 * Called when the system (VM) is entering sleep/suspend.
 **/
static int iavf_suspend(struct device *dev_d)
{
	struct net_device *netdev = dev_get_drvdata(dev_d);
	struct iavf_adapter *adapter = netdev_priv(netdev);
	bool running;

	netif_device_detach(netdev);

	running = netif_running(netdev);
	if (running)
		rtnl_lock();
	netdev_lock(netdev);

	if (running)
		iavf_down(adapter);

	iavf_free_misc_irq(adapter);
	iavf_reset_interrupt_capability(adapter);

	netdev_unlock(netdev);
	if (running)
		rtnl_unlock();

	return 0;
}

/**
 * iavf_resume - Power management resume routine
 * @dev_d: device info pointer
 *
 * Called when the system (VM) is resumed from sleep/suspend.
 **/
static int iavf_resume(struct device *dev_d)
{
	struct pci_dev *pdev = to_pci_dev(dev_d);
	struct iavf_adapter *adapter;
	u32 err;

	adapter = iavf_pdev_to_adapter(pdev);

	pci_set_master(pdev);

	rtnl_lock();
	err = iavf_set_interrupt_capability(adapter);
	if (err) {
		rtnl_unlock();
		dev_err(&pdev->dev, "Cannot enable MSI-X interrupts.\n");
		return err;
	}
	err = iavf_request_misc_irq(adapter);
	rtnl_unlock();
	if (err) {
		dev_err(&pdev->dev, "Cannot get interrupt vector.\n");
		return err;
	}

	queue_work(adapter->wq, &adapter->reset_task);

	netif_device_attach(adapter->netdev);

	return err;
}

/**
 * iavf_remove - Device Removal Routine
 * @pdev: PCI device information struct
 *
 * iavf_remove is called by the PCI subsystem to alert the driver
 * that it should release a PCI device.  The could be caused by a
 * Hot-Plug event, or because the driver is going to be removed from
 * memory.
 **/
static void iavf_remove(struct pci_dev *pdev)
{
	struct iavf_fdir_fltr *fdir, *fdirtmp;
	struct iavf_vlan_filter *vlf, *vlftmp;
	struct iavf_cloud_filter *cf, *cftmp;
	struct iavf_adv_rss *rss, *rsstmp;
	struct iavf_mac_filter *f, *ftmp;
	struct iavf_adapter *adapter;
	struct net_device *netdev;
	struct iavf_hw *hw;

	/* Don't proceed with remove if netdev is already freed */
	netdev = pci_get_drvdata(pdev);
	if (!netdev)
		return;

	adapter = iavf_pdev_to_adapter(pdev);
	hw = &adapter->hw;

	if (test_and_set_bit(__IAVF_IN_REMOVE_TASK, &adapter->crit_section))
		return;

	/* Wait until port initialization is complete.
	 * There are flows where register/unregister netdev may race.
	 */
	while (1) {
		netdev_lock(netdev);
		if (adapter->state == __IAVF_RUNNING ||
		    adapter->state == __IAVF_DOWN ||
		    adapter->state == __IAVF_INIT_FAILED) {
			netdev_unlock(netdev);
			break;
		}
		/* Simply return if we already went through iavf_shutdown */
		if (adapter->state == __IAVF_REMOVE) {
			netdev_unlock(netdev);
			return;
		}

		netdev_unlock(netdev);
		usleep_range(500, 1000);
	}
	cancel_delayed_work_sync(&adapter->watchdog_task);
	cancel_work_sync(&adapter->finish_config);

	if (netdev->reg_state == NETREG_REGISTERED)
		unregister_netdev(netdev);

	netdev_lock(netdev);
	dev_info(&adapter->pdev->dev, "Removing device\n");
	iavf_change_state(adapter, __IAVF_REMOVE);

	iavf_request_reset(adapter);
	msleep(50);
	/* If the FW isn't responding, kick it once, but only once. */
	if (!iavf_asq_done(hw)) {
		iavf_request_reset(adapter);
		msleep(50);
	}

	iavf_ptp_release(adapter);

	iavf_misc_irq_disable(adapter);
	/* Shut down all the garbage mashers on the detention level */
	netdev_unlock(netdev);
	cancel_work_sync(&adapter->reset_task);
	cancel_delayed_work_sync(&adapter->watchdog_task);
	cancel_work_sync(&adapter->adminq_task);
	netdev_lock(netdev);

	adapter->aq_required = 0;
	adapter->flags &= ~IAVF_FLAG_REINIT_ITR_NEEDED;

	iavf_free_all_tx_resources(adapter);
	iavf_free_all_rx_resources(adapter);
	iavf_free_misc_irq(adapter);
	iavf_free_interrupt_scheme(adapter);

	iavf_free_rss(adapter);

	if (hw->aq.asq.count)
		iavf_shutdown_adminq(hw);

	/* destroy the locks only once, here */
	mutex_destroy(&hw->aq.arq_mutex);
	mutex_destroy(&hw->aq.asq_mutex);
	netdev_unlock(netdev);

	iounmap(hw->hw_addr);
	pci_release_regions(pdev);
	kfree(adapter->vf_res);
	spin_lock_bh(&adapter->mac_vlan_list_lock);
	/* If we got removed before an up/down sequence, we've got a filter
	 * hanging out there that we need to get rid of.
	 */
	list_for_each_entry_safe(f, ftmp, &adapter->mac_filter_list, list) {
		list_del(&f->list);
		kfree(f);
	}
	list_for_each_entry_safe(vlf, vlftmp, &adapter->vlan_filter_list,
				 list) {
		list_del(&vlf->list);
		kfree(vlf);
	}

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_for_each_entry_safe(cf, cftmp, &adapter->cloud_filter_list, list) {
		list_del(&cf->list);
		kfree(cf);
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);

	spin_lock_bh(&adapter->fdir_fltr_lock);
	list_for_each_entry_safe(fdir, fdirtmp, &adapter->fdir_list_head, list) {
		list_del(&fdir->list);
		kfree(fdir);
	}
	spin_unlock_bh(&adapter->fdir_fltr_lock);

	spin_lock_bh(&adapter->adv_rss_lock);
	list_for_each_entry_safe(rss, rsstmp, &adapter->adv_rss_list_head,
				 list) {
		list_del(&rss->list);
		kfree(rss);
	}
	spin_unlock_bh(&adapter->adv_rss_lock);

	destroy_workqueue(adapter->wq);

	pci_set_drvdata(pdev, NULL);

	free_netdev(netdev);

	pci_disable_device(pdev);
}

/**
 * iavf_shutdown - Shutdown the device in preparation for a reboot
 * @pdev: pci device structure
 **/
static void iavf_shutdown(struct pci_dev *pdev)
{
	iavf_remove(pdev);

	if (system_state == SYSTEM_POWER_OFF)
		pci_set_power_state(pdev, PCI_D3hot);
}

static DEFINE_SIMPLE_DEV_PM_OPS(iavf_pm_ops, iavf_suspend, iavf_resume);

static struct pci_driver iavf_driver = {
	.name      = iavf_driver_name,
	.id_table  = iavf_pci_tbl,
	.probe     = iavf_probe,
	.remove    = iavf_remove,
	.driver.pm = pm_sleep_ptr(&iavf_pm_ops),
	.shutdown  = iavf_shutdown,
};

/**
 * iavf_init_module - Driver Registration Routine
 *
 * iavf_init_module is the first routine called when the driver is
 * loaded. All it does is register with the PCI subsystem.
 **/
static int __init iavf_init_module(void)
{
	pr_info("iavf: %s\n", iavf_driver_string);

	pr_info("%s\n", iavf_copyright);

	return pci_register_driver(&iavf_driver);
}

module_init(iavf_init_module);

/**
 * iavf_exit_module - Driver Exit Cleanup Routine
 *
 * iavf_exit_module is called just before the driver is removed
 * from memory.
 **/
static void __exit iavf_exit_module(void)
{
	pci_unregister_driver(&iavf_driver);
}

module_exit(iavf_exit_module);

/* iavf_main.c */
