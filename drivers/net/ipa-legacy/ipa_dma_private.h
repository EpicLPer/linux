/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2022 Linaro Ltd.
 */
#ifndef _IPA_DMA_PRIVATE_H_
#define _IPA_DMA_PRIVATE_H_

/* === Only "gsi.c" and "ipa_dma_trans.c" should include this file === */

#include <linux/types.h>

struct ipa_dma_trans;
struct ipa_dma_ring;
struct ipa_dma_channel;

#define GSI_RING_ELEMENT_SIZE	16	/* bytes; must be a power of 2 */

/**
 * ipa_dma_trans_move_pending() - Mark a DMA transaction pending
 * @trans:	Transaction whose state is to be updated
 */
void ipa_dma_trans_move_pending(struct ipa_dma_trans *trans);

/**
 * ipa_dma_trans_move_complete() - Mark a DMA transaction completed
 * @trans:	Transaction whose state is to be updated
 */
void ipa_dma_trans_move_complete(struct ipa_dma_trans *trans);

/**
 * ipa_dma_trans_move_polled() - Mark a transaction polled
 * @trans:	Transaction whose state is to be updated
 */
void ipa_dma_trans_move_polled(struct ipa_dma_trans *trans);

/**
 * ipa_dma_trans_complete() - Complete a DMA transaction
 * @trans:	Transaction to complete
 *
 * Marks a transaction complete (including freeing it).
 */
void ipa_dma_trans_complete(struct ipa_dma_trans *trans);

/**
 * ipa_dma_channel_trans_mapped() - Return a transaction mapped to a TRE index
 * @channel:	Channel associated with the transaction
 * @index:	Index of the TRE having a transaction
 *
 * Return:	The DMA transaction pointer associated with the TRE index
 */
struct ipa_dma_trans *ipa_dma_channel_trans_mapped(struct ipa_dma_channel *channel,
						   u32 index);

/**
 * ipa_dma_channel_trans_complete() - Return a channel's next completed transaction
 * @channel:	Channel whose next transaction is to be returned
 *
 * Return:	The next completed transaction, or NULL if nothing new
 */
struct ipa_dma_trans *ipa_dma_channel_trans_complete(struct ipa_dma_channel *channel);

/**
 * ipa_dma_channel_trans_cancel_pending() - Cancel pending transactions
 * @channel:	Channel whose pending transactions should be cancelled
 *
 * Cancel all pending transactions on a channel.  These are transactions
 * that have been committed but not yet completed.  This is required when
 * the channel gets reset.  At that time all pending transactions will be
 * marked as cancelled.
 *
 * NOTE:  Transactions already complete at the time of this call are
 *	  unaffected.
 */
void ipa_dma_channel_trans_cancel_pending(struct ipa_dma_channel *channel);

/**
 * ipa_dma_channel_trans_init() - Initialize a channel's DMA transaction info
 * @ipa_dma:	IPA DMA pointer
 * @channel_id:	Channel number
 *
 * Return:	0 if successful, or -ENOMEM on allocation failure
 *
 * Creates and sets up information for managing transactions on a channel
 */
int ipa_dma_channel_trans_init(struct ipa_dma *ipa_dma, u32 channel_id);

/**
 * ipa_dma_channel_trans_exit() - Inverse of ipa_dma_channel_trans_init()
 * @channel:	Channel whose transaction information is to be cleaned up
 */
void ipa_dma_channel_trans_exit(struct ipa_dma_channel *channel);

/* 
 * ipa_dma_channel_tre_max() - Channel maximum number of in-flight TREs
 * @ipa_dma:	IPA DMA pointer
 * @channel_id:	Channel whose limit is to be returned
 *
 * Return:	The maximum number of TREs outstanding on the channel
 *
 * Calculate the maximum number of outstanding TREs on a channel.
 * This limits a channel's maximum number of transactions outstanding
 * (worst case is one TRE per transaction).
 *
 * The absolute limit is the number of TREs in the channel's TRE ring,
 * and in theory we should be able use all of them.  But in practice,
 * doing that led to the hardware reporting exhaustion of event ring
 * slots for writing completion information.  So the hardware limit
 * would be (tre_count - 1).
 *
 * We reduce it a bit further though.  Transaction resource pools are
 * sized to be a little larger than this maximum, to allow resource
 * allocations to always be contiguous.  The number of entries in a
 * TRE ring buffer is a power of 2, and the extra resources in a pool
 * tends to nearly double the memory allocated for it.  Reducing the
 * maximum number of outstanding TREs allows the number of entries in
 * a pool to avoid crossing that power-of-2 boundary, and this can
 * substantially reduce pool memory requirements.  The number we
 * reduce it by matches the number added in ipa_dma_trans_pool_init().
 */
static inline u32
ipa_dma_channel_tre_max(struct ipa_dma *ipa_dma, u32 channel_id)
{
	struct ipa_dma_channel *channel = &ipa_dma->channel[channel_id];

	/* Hardware limit is channel->tre_count - 1 */
	return channel->tre_count - (channel->trans_tre_max - 1);
}

#endif /* _IPA_DMA_PRIVATE_H_ */
