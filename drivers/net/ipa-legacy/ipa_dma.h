/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2022 Linaro Ltd.
 */
#ifndef _IPA_DMA_H_
#define _IPA_DMA_H_

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>

#include "ipa_version.h"

/* Maximum number of channels and event rings supported by the driver */
#define GSI_CHANNEL_COUNT_MAX		23
#define BAM_CHANNEL_COUNT_MAX		20

/* Maximum TLV FIFO size for a channel; 64 here is arbitrary (and high) */
#define GSI_TLV_MAX		64

struct device;
struct scatterlist;
struct platform_device;

struct ipa_dma;
struct ipa_dma_trans;
struct ipa_dma_channel_data;
struct ipa_dma_endpoint_data;

/* Transactions use several resources that can be allocated dynamically
 * but taken from a fixed-size pool.  The number of elements required for
 * the pool is limited by the total number of TREs that can be outstanding.
 *
 * If sufficient TREs are available to reserve for a transaction,
 * allocation from these pools is guaranteed to succeed.  Furthermore,
 * these resources are implicitly freed whenever the TREs in the
 * transaction they're associated with are released.
 *
 * The result of a pool allocation of multiple elements is always
 * contiguous.
 */
struct ipa_dma_trans_pool {
	void *base;			/* base address of element pool */
	u32 count;			/* # elements in the pool */
	u32 free;			/* next free element in pool (modulo) */
	u32 size;			/* size (bytes) of an element */
	u32 max_alloc;			/* max allocation request */
	dma_addr_t addr;		/* DMA address if DMA pool (or 0) */
};

struct ipa_dma_trans_info {
	atomic_t tre_avail;		/* TREs available for allocation */

	u16 free_id;			/* first free trans in array */
	u16 committed_id;		/* first committed transaction */
	u16 pending_id;			/* first pending transaction */
	u16 completed_id;		/* first completed transaction */
	struct ipa_dma_trans *trans;	/* transaction array */

	struct ipa_dma_trans_pool sg_pool;	/* scatterlist pool */
	struct ipa_dma_trans_pool cmd_pool;	/* command payload DMA pool */
};

/* Hardware values signifying the state of a channel */
enum ipa_dma_channel_state {
	IPA_DMA_CHANNEL_STATE_NOT_ALLOCATED	= 0x0,
	IPA_DMA_CHANNEL_STATE_ALLOCATED		= 0x1,
	IPA_DMA_CHANNEL_STATE_STARTED		= 0x2,
	IPA_DMA_CHANNEL_STATE_STOPPED		= 0x3,
	IPA_DMA_CHANNEL_STATE_STOP_IN_PROC	= 0x4,
	IPA_DMA_CHANNEL_STATE_FLOW_CONTROLLED	= 0x5,	/* IPA v4.2-v4.9 */
	IPA_DMA_CHANNEL_STATE_ERROR		= 0xf,
};

/* We only care about channels between IPA and AP */
struct ipa_dma_channel {
	struct ipa_dma *ipa_dma;
	bool toward_ipa;
	bool command;			/* AP command TX channel or not */

	u8 trans_tre_max;		/* max TREs in a transaction */
	u16 tre_count;
	u16 event_count;

	struct dma_chan *dma_chan;

	struct ipa_dma_trans_info trans_info;

	struct napi_struct napi;
};

struct ipa_dma_ops {
	int (*init)(struct ipa_dma *ipa_dma, struct platform_device *pdev,
		    enum ipa_version version, u32 count,
		    const struct ipa_dma_endpoint_data *data);
	void (*exit)(struct ipa_dma *ipa_dma);
	int (*setup)(struct ipa_dma *ipa_dma);
	void (*teardown)(struct ipa_dma *ipa_dma);
	void (*suspend)(struct ipa_dma *ipa_dma);
	void (*resume)(struct ipa_dma *ipa_dma);

	int (*channel_start)(struct ipa_dma *ipa_dma, u32 channel_id);
	int (*channel_stop)(struct ipa_dma *ipa_dma, u32 channel_id);
	void (*channel_reset)(struct ipa_dma *ipa_dma, u32 channel_id, bool doorbell);
	int (*channel_suspend)(struct ipa_dma *ipa_dma, u32 channel_id);
	int (*channel_resume)(struct ipa_dma *ipa_dma, u32 channel_id);

	void (*trans_commit)(struct ipa_dma_trans *trans, bool ring_db);
	void (*trans_commit_wait)(struct ipa_dma_trans *trans);
};

struct ipa_dma {
	struct device *dev;		/* Same as IPA device */
	enum ipa_version version;
	void __iomem *virt;		/* Adjusted for most registers */
	u32 irq;
	u32 channel_count;
	struct completion completion;	/* Signals command completion */
	struct mutex mutex;		/* protects commands, programming */
	struct ipa_dma_channel channel[GSI_CHANNEL_COUNT_MAX];
	struct net_device *dummy_dev;	/* needed for NAPI */
	struct ipa_dma_ops *ops;
};

extern struct ipa_dma_ops bam_ops;

#endif /* _IPA_DMA_H_ */
