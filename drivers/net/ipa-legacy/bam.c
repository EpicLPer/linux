// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>

#include "ipa.h"
#include "ipa_data.h"
#include "ipa_dma.h"
#include "ipa_dma_private.h"
#include "ipa_dma_trans.h"
#include "ipa_data.h"

/**
 * DOC: The IPA Smart Peripheral System Interface
 *
 * The Smart Peripheral System is a means to communicate over BAM pipes to
 * the IPA block. The Modem also uses BAM pipes to communicate with the IPA
 * core.
 *
 * Refer the GSI documentation, because BAM is a precursor to GSI and more or less
 * the same, conceptually (maybe, IDK, I have no docs to go through).
 *
 * Each channel here corresponds to 1 BAM pipe configured in BAM2BAM mode
 *
 * IPA cmds are transferred one at a time, each in one BAM transfer.
 */

/* Get and configure the BAM DMA channel */
static int bam_channel_init_one(struct ipa_dma *ipa_dma,
			 const struct ipa_dma_endpoint_data *data, bool command)
{
	struct dma_slave_config bam_config;
	u32 channel_id = data->channel_id;
	struct ipa_dma_channel *channel = &ipa_dma->channel[channel_id];
	int ret;

	channel->ipa_dma = ipa_dma;
	channel->dma_chan = dma_request_chan(ipa_dma->dev, data->channel_name);
	channel->toward_ipa = data->toward_ipa;
	channel->command = command;
	channel->trans_tre_max = data->channel.tlv_count;
	channel->tre_count = data->channel.tre_count;
	if (IS_ERR(channel->dma_chan)) {
		dev_err(ipa_dma->dev, "failed to request BAM channel %s: %d\n",
				data->channel_name,
				(int) PTR_ERR(channel->dma_chan));
		return PTR_ERR(channel->dma_chan);
	}

	ret = ipa_dma_channel_trans_init(ipa_dma, data->channel_id);
	if (ret)
		goto err_dma_chan_free;

	if (data->toward_ipa) {
		bam_config.direction = DMA_MEM_TO_DEV;
		bam_config.dst_maxburst = channel->trans_tre_max;
	} else {
		bam_config.direction = DMA_DEV_TO_MEM;
		bam_config.src_maxburst = channel->trans_tre_max;
	}

	dmaengine_slave_config(channel->dma_chan, &bam_config);

	if (command) {
		u32 tre_max = ipa_dma_channel_tre_max(ipa_dma, data->channel_id);
		ret = ipa_cmd_pool_init(channel, tre_max);
	}

	if (!ret)
		return 0;

err_dma_chan_free:
	dma_release_channel(channel->dma_chan);
	return ret;
}

static void bam_channel_exit_one(struct ipa_dma_channel *channel)
{
	if (channel->dma_chan) {
		dmaengine_terminate_sync(channel->dma_chan);
		dma_release_channel(channel->dma_chan);
	}
}

/* Get channels from BAM_DMA */
static int bam_channel_init(struct ipa_dma *ipa_dma, u32 count,
		const struct ipa_dma_endpoint_data *data)
{
	int ret = 0;
	u32 i;

	for (i = 0; i < count; ++i) {
		bool command = i == IPA_ENDPOINT_AP_COMMAND_TX;

		if (!data[i].channel_name || data[i].ee_id == IPA_EE_MODEM)
			continue;

		ret = bam_channel_init_one(ipa_dma, &data[i], command);
		if (ret)
			goto err_unwind;
	}

	return ret;

err_unwind:
	while (i--) {
		if (ipa_dma_endpoint_data_empty(&data[i]))
			continue;

		bam_channel_exit_one(&ipa_dma->channel[i]);
	}
	return ret;
}

/* Inverse of bam_channel_init() */
static void bam_channel_exit(struct ipa_dma *ipa_dma)
{
	u32 channel_id = BAM_CHANNEL_COUNT_MAX - 1;

	do
		bam_channel_exit_one(&ipa_dma->channel[channel_id]);
	while (channel_id--);
}

/* Inverse of bam_init() */
static void bam_exit(struct ipa_dma *ipa_dma)
{
	mutex_destroy(&ipa_dma->mutex);
	bam_channel_exit(ipa_dma);
}

/**
 * bam_channel_poll_one() - Return a single completed transaction on a channel
 * @channel:	Channel to be polled
 *
 * Return:	Transaction pointer, or null if none are available
 *
 * This function returns the first of a channel's completed transactions,
 * or a null pointer if there are none.  Transactions are moved to the
 * completed state by the DMA completion callback, which also schedules
 * NAPI polling.
 */
static struct ipa_dma_trans *bam_channel_poll_one(struct ipa_dma_channel *channel)
{
	struct ipa_dma_trans *trans;

	/* Get the first transaction from the completed list */
	trans = ipa_dma_channel_trans_complete(channel);
	if (trans)
		ipa_dma_trans_move_polled(trans);

	return trans;
}

/**
 * bam_channel_poll() - NAPI poll function for a channel
 * @napi:	NAPI structure for the channel
 * @budget:	Budget supplied by NAPI core
 *
 * Return:	Number of items polled (<= budget)
 *
 * Single transactions completed by hardware are polled until either
 * the budget is exhausted, or there are no more.  Each transaction
 * polled is passed to ipa_dma_trans_complete(), to perform remaining
 * completion processing and retire/free the transaction.
 */
static int bam_channel_poll(struct napi_struct *napi, int budget)
{
	struct ipa_dma_channel *channel;
	int count = 0;

	channel = container_of(napi, struct ipa_dma_channel, napi);
	while (count < budget) {
		struct ipa_dma_trans *trans;

		count++;
		trans = bam_channel_poll_one(channel);
		if (!trans)
			break;
		ipa_dma_trans_complete(trans);
	}

	if (count < budget)
		napi_complete(&channel->napi);

	return count;
}

/* Setup function for a single channel */
static void bam_channel_setup_one(struct ipa_dma *ipa_dma, u32 channel_id)
{
	struct ipa_dma_channel *channel = &ipa_dma->channel[channel_id];

	if (!channel->ipa_dma)
		return;	/* Ignore uninitialized channels */

	if (channel->toward_ipa) {
		netif_napi_add_tx(ipa_dma->dummy_dev, &channel->napi,
				  bam_channel_poll);
	} else {
		netif_napi_add(ipa_dma->dummy_dev, &channel->napi,
			       bam_channel_poll);
	}
	napi_enable(&channel->napi);
}

static void bam_channel_teardown_one(struct ipa_dma *ipa_dma, u32 channel_id)
{
	struct ipa_dma_channel *channel = &ipa_dma->channel[channel_id];

	if (!channel->ipa_dma)
		return;		/* Ignore uninitialized channels */

	netif_napi_del(&channel->napi);
}

/* Setup function for channels */
static int bam_channel_setup(struct ipa_dma *ipa_dma)
{
	u32 channel_id = 0;
	int ret;

	mutex_lock(&ipa_dma->mutex);

	do
		bam_channel_setup_one(ipa_dma, channel_id);
	while (++channel_id < BAM_CHANNEL_COUNT_MAX);

	/* Make sure no channels were defined that hardware does not support */
	while (channel_id < BAM_CHANNEL_COUNT_MAX) {
		struct ipa_dma_channel *channel = &ipa_dma->channel[channel_id++];

		if (!channel->ipa_dma)
			continue;	/* Ignore uninitialized channels */

		dev_err(ipa_dma->dev, "channel %u not supported by hardware\n",
			channel_id - 1);
		channel_id = BAM_CHANNEL_COUNT_MAX;
		goto err_unwind;
	}

	mutex_unlock(&ipa_dma->mutex);

	return 0;

err_unwind:
	while (channel_id--)
		bam_channel_teardown_one(ipa_dma, channel_id);

	mutex_unlock(&ipa_dma->mutex);

	return ret;
}

/* Inverse of bam_channel_setup() */
static void bam_channel_teardown(struct ipa_dma *ipa_dma)
{
	u32 channel_id;

	mutex_lock(&ipa_dma->mutex);

	channel_id = BAM_CHANNEL_COUNT_MAX;
	do
		bam_channel_teardown_one(ipa_dma, channel_id);
	while (channel_id--);

	mutex_unlock(&ipa_dma->mutex);
}

static int bam_setup(struct ipa_dma *ipa_dma)
{
	return bam_channel_setup(ipa_dma);
}

static void bam_teardown(struct ipa_dma *ipa_dma)
{
	bam_channel_teardown(ipa_dma);
}

static int bam_channel_start(struct ipa_dma *ipa_dma, u32 channel_id)
{
	return 0;
}

static int bam_channel_stop(struct ipa_dma *ipa_dma, u32 channel_id)
{
	struct ipa_dma_channel *channel = &ipa_dma->channel[channel_id];

	return dmaengine_terminate_sync(channel->dma_chan);
}

static void bam_channel_reset(struct ipa_dma *ipa_dma, u32 channel_id, bool doorbell)
{
	struct ipa_dma_channel *channel = &ipa_dma->channel[channel_id];

	bam_channel_stop(ipa_dma, channel_id);

	/* The dmaengine frees the descriptors it terminates without
	 * completing them; hand the affected transactions back to NAPI so
	 * their buffers and transaction resources are released.
	 */
	ipa_dma_channel_trans_cancel_pending(channel);
}

static int bam_channel_suspend(struct ipa_dma *ipa_dma, u32 channel_id)
{
	struct ipa_dma_channel *channel = &ipa_dma->channel[channel_id];

	return dmaengine_pause(channel->dma_chan);
}

static int bam_channel_resume(struct ipa_dma *ipa_dma, u32 channel_id)
{
	struct ipa_dma_channel *channel = &ipa_dma->channel[channel_id];

	return dmaengine_resume(channel->dma_chan);
}

static void bam_suspend(struct ipa_dma *ipa_dma)
{
	/* No-op for now */
}

static void bam_resume(struct ipa_dma *ipa_dma)
{
	/* No-op for now */
}

/* DMA completion callback.  It runs in the vchan tasklet, so it only records
 * the completion and schedules NAPI; delivery (handing buffers to the endpoint
 * and the network stack, then freeing the transaction) happens in NAPI, once,
 * via ipa_dma_trans_complete().
 */
static void bam_trans_callback(void *arg)
{
	struct ipa_dma_trans *trans = arg;
	struct ipa_dma_channel *channel;

	channel = &trans->ipa_dma->channel[trans->channel_id];
	ipa_dma_trans_move_complete(trans);
	napi_schedule(&channel->napi);
}

static void bam_trans_commit(struct ipa_dma_trans *trans, bool unused)
{
	struct ipa_dma_channel *channel = &trans->ipa_dma->channel[trans->channel_id];
	enum ipa_cmd_opcode opcode = IPA_CMD_NONE;
	struct scatterlist *sg;
	u8 *cmd_opcode;
	u32 i;
	enum dma_transfer_direction direction;

	if (channel->toward_ipa)
		direction = DMA_MEM_TO_DEV;
	else
		direction = DMA_DEV_TO_MEM;

	WARN_ON(!trans->used_count);

	cmd_opcode = channel->command ? &trans->cmd_opcode[0] : NULL;
	for_each_sg(trans->sgl, sg, trans->used_count, i) {
		bool last_tre = i == trans->used_count - 1;
		dma_addr_t addr = sg_dma_address(sg);
		u32 len = sg_dma_len(sg);
		u32 dma_flags = 0;
		struct dma_async_tx_descriptor *desc;

		/* Only command channel transactions carry opcodes; an entry
		 * with no opcode is a plain data transfer on that channel
		 * (used by the pipeline clear).
		 */
		if (cmd_opcode)
			opcode = *cmd_opcode++;

		if (opcode != IPA_CMD_NONE) {
			/* The size field of an immediate command descriptor
			 * holds the command opcode, not a data length.
			 */
			len = opcode;
			dma_flags |= DMA_PREP_CMD;
		}

		if (last_tre)
			dma_flags |= DMA_PREP_INTERRUPT;

		desc = dmaengine_prep_slave_single(channel->dma_chan, addr, len,
				direction, dma_flags);

		if (last_tre) {
			desc->callback = bam_trans_callback;
			desc->callback_param = trans;
		}

		desc->cookie = dmaengine_submit(desc);

		if (last_tre)
			trans->cookie = desc->cookie;

		if (direction == DMA_DEV_TO_MEM)
			dmaengine_desc_attach_metadata(desc, &trans->len, sizeof(trans->len));
	}

	ipa_dma_trans_move_pending(trans);

	dma_async_issue_pending(channel->dma_chan);
}

static void bam_trans_commit_wait(struct ipa_dma_trans *trans)
{
	struct ipa_dma *ipa_dma = trans->ipa_dma;
	struct ipa_dma_channel *channel = &ipa_dma->channel[trans->channel_id];
	enum dma_status status;

	bam_trans_commit(trans, false);

	/* Diagnostic timeout: the final code uses an uninterruptible wait */
	if (wait_for_completion_timeout(&trans->completion, 3 * HZ))
		return;

	status = dma_async_is_tx_complete(channel->dma_chan, trans->cookie,
					  NULL, NULL);
	dev_err(ipa_dma->dev,
		"bam: tx wait timed out (chan %u cookie %d status %d)\n",
		trans->channel_id, trans->cookie, status);
	{
		void __iomem *bam = ioremap(0xfd4c4000, 0x20000);
		int n;

		if (!bam)
			return;
		dev_err(ipa_dma->dev,
			"bam: ctrl=%08x irq_stts=%08x srcs0=%08x msk0=%08x\n",
			readl(bam + 0x0), readl(bam + 0x14),
			readl(bam + 0x800), readl(bam + 0x804));
		for (n = 2; n <= 5; n++)
			dev_err(ipa_dma->dev,
				"bam p%d: ctrl=%08x irq=%08x en=%08x sw=%08x fifo=%08x\n",
				n, readl(bam + 0x1000 + 0x1000 * n),
				readl(bam + 0x1010 + 0x1000 * n),
				readl(bam + 0x1018 + 0x1000 * n),
				readl(bam + 0x1800 + 0x1000 * n),
				readl(bam + 0x1820 + 0x1000 * n));
		iounmap(bam);
	}
}

/* Initialize the BAM DMA channels
 * Actual hw init is handled by the BAM_DMA driver
 */
static int bam_init(struct ipa_dma *ipa_dma, struct platform_device *pdev,
		enum ipa_version version, u32 count,
		const struct ipa_dma_endpoint_data *data)
{
	struct device *dev = &pdev->dev;
	int ret;

	ipa_dma->dev = dev;
	ipa_dma->version = version;

	/* All channels use NAPI, so we need a network device for the
	 * NAPI contexts to be associated with.  A dummy device serves
	 * that purpose and is never registered.
	 */
	ipa_dma->dummy_dev = alloc_netdev_dummy(0);
	if (!ipa_dma->dummy_dev)
		return -ENOMEM;

	ret = bam_channel_init(ipa_dma, count, data);
	if (ret)
		return ret;

	mutex_init(&ipa_dma->mutex);

	return 0;
}

struct ipa_dma_ops bam_ops = {
	.init = bam_init,
	.exit = bam_exit,
	.setup = bam_setup,
	.teardown = bam_teardown,
	.suspend = bam_suspend,
	.resume = bam_resume,

	.channel_start = bam_channel_start,
	.channel_stop = bam_channel_stop,
	.channel_reset = bam_channel_reset,
	.channel_suspend = bam_channel_suspend,
	.channel_resume = bam_channel_resume,

	.trans_commit = bam_trans_commit,
	.trans_commit_wait = bam_trans_commit_wait,
};
