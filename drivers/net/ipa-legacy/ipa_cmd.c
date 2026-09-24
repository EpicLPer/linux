// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2023 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/bitfield.h>
#include <linux/dma-direction.h>

#include "ipa_dma.h"
#include "ipa_dma_trans.h"
#include "ipa.h"
#include "ipa_endpoint.h"
#include "ipa_table.h"
#include "ipa_cmd.h"
#include "ipa_mem.h"

/**
 * DOC:  IPA Immediate Commands
 *
 * The AP command TX endpoint is used to issue immediate commands to the IPA.
 * An immediate command is generally used to request the IPA do something
 * other than data transfer to another endpoint.
 *
 * Immediate commands are represented by GSI transactions just like other
 * transfer requests, and use a single GSI TRE.  Each immediate command
 * has a well-defined format, having a payload of a known length.  This
 * allows the transfer element's length field to be used to hold an
 * immediate command's opcode.  The payload for a command resides in AP
 * memory and is described by a single scatterlist entry in its transaction.
 * Commands do not require a transaction completion callback, and are
 * always issued using ipa_dma_trans_commit_wait().
 */

/* Some commands can wait until indicated pipeline stages are clear */
enum pipeline_clear_options {
	pipeline_clear_hps		= 0x0,
	pipeline_clear_src_grp		= 0x1,
	pipeline_clear_full		= 0x2,
};

/* IPA_CMD_IP_V{4,6}_{FILTER,ROUTING}_INIT */

struct ipa_cmd_hw_ip_fltrt_init {
	__le64 hash_rules_addr;
	__le64 flags;
};

/* Field masks for ipa_cmd_hw_ip_fltrt_init structure fields */
#define IP_IPV4_FLTRT_FLAGS_SIZE_FMASK		GENMASK_ULL(11, 0)
#define IP_IPV4_FLTRT_FLAGS_ADDR_FMASK		GENMASK_ULL(27, 12)
#define IP_IPV6_FLTRT_FLAGS_SIZE_FMASK		GENMASK_ULL(15, 0)
#define IP_IPV6_FLTRT_FLAGS_ADDR_FMASK		GENMASK_ULL(31, 16)


/* IPA_CMD_HDR_INIT_LOCAL */

struct ipa_cmd_hw_hdr_init_local {
	__le64 hdr_table_addr;
	__le32 flags;
};

/* Field masks for ipa_cmd_hw_hdr_init_local structure fields */
#define HDR_INIT_LOCAL_FLAGS_TABLE_SIZE_FMASK		GENMASK(11, 0)
#define HDR_INIT_LOCAL_FLAGS_HDR_ADDR_FMASK		GENMASK(27, 12)

/* IPA_CMD_REGISTER_WRITE */

/* For IPA v4.0+, the pipeline clear options are encoded in the opcode */
#define REGISTER_WRITE_OPCODE_SKIP_CLEAR_FMASK		GENMASK(8, 8)
#define REGISTER_WRITE_OPCODE_CLEAR_OPTION_FMASK	GENMASK(10, 9)

struct ipa_cmd_register_write {
	__le16 flags;		/* Unused/reserved prior to IPA v4.0 */
	__le16 offset;
	__le32 value;
	__le32 value_mask;
	__le32 clear_options;	/* Unused/reserved for IPA v4.0+ */
};

/* Field masks for ipa_cmd_register_write structure fields */
/* The next field is present for IPA v4.0+ */
#define REGISTER_WRITE_FLAGS_OFFSET_HIGH_FMASK		GENMASK(14, 11)
/* The next field is not present for IPA v4.0+ */
#define REGISTER_WRITE_FLAGS_SKIP_CLEAR_FMASK		GENMASK(15, 15)

/* The next field and its values are not present for IPA v4.0+ */
#define REGISTER_WRITE_CLEAR_OPTIONS_FMASK		GENMASK(1, 0)

/* IPA_CMD_IP_PACKET_INIT */

struct ipa_cmd_ip_packet_init {
	u8 dest_endpoint;	/* Full 8 bits used for IPA v5.0+ */
	u8 reserved[7];
};

/* Field mask for ipa_cmd_ip_packet_init dest_endpoint field (unused v5.0+) */
#define IPA_PACKET_INIT_DEST_ENDPOINT_FMASK		GENMASK(4, 0)

/* IPA_CMD_DMA_SHARED_MEM */

/* For IPA v4.0+, this opcode gets modified with pipeline clear options */

#define DMA_SHARED_MEM_OPCODE_SKIP_CLEAR_FMASK		GENMASK(8, 8)
#define DMA_SHARED_MEM_OPCODE_CLEAR_OPTION_FMASK	GENMASK(10, 9)

struct ipa_cmd_hw_dma_mem_mem {
	__le16 reserved;
	__le16 size;
	__le32 system_addr;
	__le16 local_addr;
	__le16 flags; /* the least significant 14 bits are reserved */
	__le32 padding;
};

/* Flag allowing atomic clear of target region after reading data (v4.0+)*/
#define DMA_SHARED_MEM_CLEAR_AFTER_READ			GENMASK(15, 15)

/* Field masks for ipa_cmd_hw_dma_mem_mem structure fields */
#define DMA_SHARED_MEM_FLAGS_DIRECTION_FMASK		GENMASK(0, 0)
/* The next two fields are not present for IPA v4.0+ */
#define DMA_SHARED_MEM_FLAGS_SKIP_CLEAR_FMASK		GENMASK(1, 1)
#define DMA_SHARED_MEM_FLAGS_CLEAR_OPTIONS_FMASK	GENMASK(3, 2)

/* IPA_CMD_IP_PACKET_TAG_STATUS */

struct ipa_cmd_ip_packet_tag_status {
	__le64 tag;
};

#define IP_PACKET_TAG_STATUS_TAG_FMASK		GENMASK_ULL(63, 32)

/* Immediate command payload */
union ipa_cmd_payload {
	struct ipa_cmd_hw_ip_fltrt_init table_init;
	struct ipa_cmd_hw_hdr_init_local hdr_init_local;
	struct ipa_cmd_register_write register_write;
	struct ipa_cmd_ip_packet_init ip_packet_init;
	struct ipa_cmd_hw_dma_mem_mem dma_shared_mem;
	struct ipa_cmd_ip_packet_tag_status ip_packet_tag_status;
};

static void ipa_cmd_validate_build(void)
{
	/* Prior to IPA v5.0, we supported no more than 32 endpoints,
	 * and this was reflected in some 5-bit fields that held
	 * endpoint numbers.  Starting with IPA v5.0, the widths of
	 * these fields were extended to 8 bits, meaning up to 256
	 * endpoints.  If the driver claims to support more than
	 * that it's an error.
	 */
	BUILD_BUG_ON(IPA_ENDPOINT_MAX - 1 > U8_MAX);
}

/* Validate the memory region that holds headers */
static bool ipa_cmd_header_init_local_valid(struct ipa *ipa)
{
	struct device *dev = &ipa->pdev->dev;
	const struct ipa_mem *mem;
	u32 offset_max;
	u32 size_max;
	u32 offset;
	u32 size;

	/* In ipa_cmd_hdr_init_local_add() we record the offset and size of
	 * the header table memory area in an immediate command.  Make sure
	 * the offset and size fit in the fields that need to hold them, and
	 * that the entire range is within the overall IPA memory range.
	 */
	offset_max = field_max(HDR_INIT_LOCAL_FLAGS_HDR_ADDR_FMASK);
	size_max = field_max(HDR_INIT_LOCAL_FLAGS_TABLE_SIZE_FMASK);

	/* The header memory area contains both the modem and AP header
	 * regions.  The modem portion defines the address of the region.
	 */
	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_HEADER);
	offset = mem->offset;
	size = mem->size;

	/* Make sure the offset fits in the IPA command */
	if (offset > offset_max || ipa->mem_offset > offset_max - offset) {
		dev_err(dev, "header table region offset too large\n");
		dev_err(dev, "    (0x%04x + 0x%04x > 0x%04x)\n",
			ipa->mem_offset, offset, offset_max);

		return false;
	}

	/* Add the size of the AP portion (if defined) to the combined size */
	mem = ipa_mem_find(ipa, IPA_MEM_AP_HEADER);
	if (mem)
		size += mem->size;

	/* Make sure the combined size fits in the IPA command */
	if (size > size_max) {
		dev_err(dev, "header table region size too large\n");
		dev_err(dev, "    (0x%04x > 0x%08x)\n", size, size_max);

		return false;
	}

	return true;
}

/* Indicate whether an offset can be used with a register_write command */
static bool ipa_cmd_register_write_offset_valid(struct ipa *ipa,
						const char *name, u32 offset)
{
	struct ipa_cmd_register_write *payload;
	struct device *dev = &ipa->pdev->dev;
	u32 offset_max;
	u32 bit_count;

	/* The maximum offset in a register_write immediate command depends
	 * on the version of IPA.  A 16 bit offset is always supported,
	 * but starting with IPA v4.0 some additional high-order bits are
	 * allowed.
	 */
	bit_count = BITS_PER_BYTE * sizeof(payload->offset);
	BUILD_BUG_ON(bit_count > 32);
	offset_max = ~0U >> (32 - bit_count);

	/* Make sure the offset can be represented by the field(s)
	 * that holds it.  Also make sure the offset is not outside
	 * the overall IPA memory range.
	 */
	if (offset > offset_max || ipa->mem_offset > offset_max - offset) {
		dev_err(dev, "%s offset too large 0x%04x + 0x%04x > 0x%04x)\n",
			name, ipa->mem_offset, offset, offset_max);
		return false;
	}

	return true;
}

/* Check whether offsets passed to register_write are valid */
static bool ipa_cmd_register_write_valid(struct ipa *ipa)
{
	const struct reg *reg;
	const char *name;
	u32 offset;

	/* Each endpoint can have a status endpoint associated with it,
	 * and this is recorded in an endpoint register.  If the modem
	 * crashes, we reset the status endpoint for all modem endpoints
	 * using a register write IPA immediate command.  Make sure the
	 * worst case (highest endpoint number) offset of that endpoint
	 * fits in the register write command field(s) that must hold it.
	 */
	reg = ipa_reg(ipa, ENDP_STATUS);
	offset = reg_n_offset(reg, IPA_ENDPOINT_COUNT - 1);
	name = "maximal endpoint status";
	if (!ipa_cmd_register_write_offset_valid(ipa, name, offset))
		return false;

	return true;
}

int ipa_cmd_pool_init(struct ipa_dma_channel *channel, u32 tre_max)
{
	struct ipa_dma_trans_info *trans_info = &channel->trans_info;
	struct device *dev = channel->ipa_dma->dev;

	/* Command payloads are allocated one at a time, but a single
	 * transaction can require up to the maximum supported by the
	 * channel; treat them as if they were allocated all at once.
	 */
	return ipa_dma_trans_pool_init_dma(dev, &trans_info->cmd_pool,
				       sizeof(union ipa_cmd_payload),
				       tre_max, channel->trans_tre_max);
}

void ipa_cmd_pool_exit(struct ipa_dma_channel *channel)
{
	struct ipa_dma_trans_info *trans_info = &channel->trans_info;
	struct device *dev = channel->ipa_dma->dev;

	ipa_dma_trans_pool_exit_dma(dev, &trans_info->cmd_pool);
}

static union ipa_cmd_payload *
ipa_cmd_payload_alloc(struct ipa *ipa, dma_addr_t *addr)
{
	struct ipa_dma_trans_info *trans_info;
	struct ipa_endpoint *endpoint;

	endpoint = ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX];
	trans_info = &ipa->ipa_dma.channel[endpoint->channel_id].trans_info;

	return ipa_dma_trans_pool_alloc_dma(&trans_info->cmd_pool, addr);
}

/* If hash_size is 0, hash_offset and hash_addr ignored. */
void ipa_cmd_table_init_add(struct ipa_dma_trans *trans,
			    enum ipa_cmd_opcode opcode, u16 size, u32 offset,
			    dma_addr_t addr)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	struct ipa_cmd_hw_ip_fltrt_init *payload;
	union ipa_cmd_payload *cmd_payload;
	dma_addr_t payload_addr;
	u64 val;

	/* Record the non-hash table offset and size */
	offset += ipa->mem_offset;
	if (opcode == IPA_CMD_IP_V4_FILTER_INIT ||
		   opcode == IPA_CMD_IP_V4_ROUTING_INIT) {
		val = u64_encode_bits(offset, IP_IPV4_FLTRT_FLAGS_ADDR_FMASK);
		val |= u64_encode_bits(size, IP_IPV4_FLTRT_FLAGS_SIZE_FMASK);
	} else { /* IPA <= v2.6L IPv6 */
		val = u64_encode_bits(offset, IP_IPV6_FLTRT_FLAGS_ADDR_FMASK);
		val |= u64_encode_bits(size, IP_IPV6_FLTRT_FLAGS_SIZE_FMASK);
	}

	cmd_payload = ipa_cmd_payload_alloc(ipa, &payload_addr);
	payload = &cmd_payload->table_init;

	/* Fill in all offsets and sizes */
	payload->flags = cpu_to_le32(val);

	ipa_dma_trans_cmd_add(trans, payload, sizeof(*payload), payload_addr,
			  opcode);
}

/* Initialize header space in IPA-local memory */
void ipa_cmd_hdr_init_local_add(struct ipa_dma_trans *trans, u32 offset, u16 size,
				dma_addr_t addr)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	enum ipa_cmd_opcode opcode = IPA_CMD_HDR_INIT_LOCAL;
	struct ipa_cmd_hw_hdr_init_local *payload;
	union ipa_cmd_payload *cmd_payload;
	dma_addr_t payload_addr;
	u32 flags;

	offset += ipa->mem_offset;

	/* With this command we tell the IPA where in its local memory the
	 * header tables reside.  The content of the buffer provided is
	 * also written via DMA into that space.  The IPA hardware owns
	 * the table, but the AP must initialize it.
	 */
	cmd_payload = ipa_cmd_payload_alloc(ipa, &payload_addr);
	payload = &cmd_payload->hdr_init_local;

	payload->hdr_table_addr = cpu_to_le32(addr);
	flags = u32_encode_bits(size, HDR_INIT_LOCAL_FLAGS_TABLE_SIZE_FMASK);
	flags |= u32_encode_bits(offset, HDR_INIT_LOCAL_FLAGS_HDR_ADDR_FMASK);
	payload->flags = cpu_to_le32(flags);

	ipa_dma_trans_cmd_add(trans, payload, sizeof(*payload), payload_addr,
			  opcode);
}

void ipa_cmd_register_write_add(struct ipa_dma_trans *trans, u32 offset, u32 value,
				u32 mask, bool clear_full)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	struct ipa_cmd_register_write *payload;
	union ipa_cmd_payload *cmd_payload;
	u32 opcode = IPA_CMD_REGISTER_WRITE;
	dma_addr_t payload_addr;
	u32 clear_option;
	u32 options;
	u16 flags;

	/* pipeline_clear_src_grp is not used */
	clear_option = clear_full ? pipeline_clear_full : pipeline_clear_hps;

	flags = 0;	/* SKIP_CLEAR flag is always 0 */
	options = 0;

	cmd_payload = ipa_cmd_payload_alloc(ipa, &payload_addr);
	payload = &cmd_payload->register_write;

	payload->flags = cpu_to_le16(flags);
	payload->offset = cpu_to_le16((u16)offset);
	payload->value = cpu_to_le32(value);
	payload->value_mask = cpu_to_le32(mask);
	payload->clear_options = cpu_to_le32(options);

	ipa_dma_trans_cmd_add(trans, payload, sizeof(*payload), payload_addr,
			  opcode);
}

/* Skip IP packet processing on the next data transfer on a TX channel */
static void ipa_cmd_ip_packet_init_add(struct ipa_dma_trans *trans, u8 endpoint_id)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	enum ipa_cmd_opcode opcode = IPA_CMD_IP_PACKET_INIT;
	struct ipa_cmd_ip_packet_init *payload;
	union ipa_cmd_payload *cmd_payload;
	dma_addr_t payload_addr;

	cmd_payload = ipa_cmd_payload_alloc(ipa, &payload_addr);
	payload = &cmd_payload->ip_packet_init;

	payload->dest_endpoint = u8_encode_bits(endpoint_id,
						IPA_PACKET_INIT_DEST_ENDPOINT_FMASK);

	ipa_dma_trans_cmd_add(trans, payload, sizeof(*payload), payload_addr,
			  opcode);
}

/* Use a DMA command to read or write a block of IPA-resident memory */
void ipa_cmd_dma_shared_mem_add(struct ipa_dma_trans *trans, u32 offset, u16 size,
				dma_addr_t addr, bool toward_ipa)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	enum ipa_cmd_opcode opcode = IPA_CMD_DMA_SHARED_MEM;
	struct ipa_cmd_hw_dma_mem_mem *payload;
	union ipa_cmd_payload *cmd_payload;
	dma_addr_t payload_addr;
	u16 flags;

	/* size and offset must fit in 16 bit fields */
	WARN_ON(!size);
	WARN_ON(size > U16_MAX);
	WARN_ON(offset > U16_MAX || ipa->mem_offset > U16_MAX - offset);

	offset += ipa->mem_offset;

	cmd_payload = ipa_cmd_payload_alloc(ipa, &payload_addr);
	payload = &cmd_payload->dma_shared_mem;

	/* payload->clear_after_read was reserved prior to IPA v4.0.  It's
	 * never needed for current code, so it's 0 regardless of version.
	 */
	payload->size = cpu_to_le16(size);
	payload->local_addr = cpu_to_le16(offset);
	/* payload->flags:
	 *   direction:		0 = write to IPA, 1 read from IPA
	 * Starting at v4.0 these are reserved; either way, all zero:
	 *   pipeline clear:	0 = wait for pipeline clear (don't skip)
	 *   clear_options:	0 = pipeline_clear_hps
	 * Instead, for v4.0+ these are encoded in the opcode.  But again
	 * since both values are 0 we won't bother OR'ing them in.
	 */
	flags = toward_ipa ? 0 : DMA_SHARED_MEM_FLAGS_DIRECTION_FMASK;
	payload->flags = cpu_to_le16(flags);
	payload->system_addr = cpu_to_le32(addr);

	ipa_dma_trans_cmd_add(trans, payload, sizeof(*payload), payload_addr,
			  opcode);
}

static void ipa_cmd_ip_tag_status_add(struct ipa_dma_trans *trans)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	enum ipa_cmd_opcode opcode = IPA_CMD_IP_PACKET_TAG_STATUS;
	struct ipa_cmd_ip_packet_tag_status *payload;
	union ipa_cmd_payload *cmd_payload;
	dma_addr_t payload_addr;

	cmd_payload = ipa_cmd_payload_alloc(ipa, &payload_addr);
	payload = &cmd_payload->ip_packet_tag_status;

	payload->tag = le64_encode_bits(0, IP_PACKET_TAG_STATUS_TAG_FMASK);

	ipa_dma_trans_cmd_add(trans, payload, sizeof(*payload), payload_addr,
			  opcode);
}

/* Issue a small command TX data transfer */
static void ipa_cmd_transfer_add(struct ipa_dma_trans *trans)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	enum ipa_cmd_opcode opcode = IPA_CMD_NONE;
	union ipa_cmd_payload *payload;
	dma_addr_t payload_addr;

	/* Just transfer a zero-filled payload structure */
	payload = ipa_cmd_payload_alloc(ipa, &payload_addr);

	ipa_dma_trans_cmd_add(trans, payload, sizeof(*payload), payload_addr,
			  opcode);
}

/* Add immediate commands to a transaction to clear the hardware pipeline */
void ipa_cmd_pipeline_clear_add(struct ipa_dma_trans *trans)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	struct ipa_endpoint *endpoint;

	/* This will complete when the transfer is received */
	reinit_completion(&ipa->completion);

	/* Issue a no-op register write command (mask 0 means no write) */
	ipa_cmd_register_write_add(trans, 0, 0, 0, true);

	/* Send a data packet through the IPA pipeline.  The packet_init
	 * command says to send the next packet directly to the exception
	 * endpoint without any other IPA processing.  The tag_status
	 * command requests that status be generated on completion of
	 * that transfer, and that it will be tagged with a value.
	 * Finally, the transfer command sends a small packet of data
	 * (instead of a command) using the command endpoint.
	 */
	endpoint = ipa->name_map[IPA_ENDPOINT_AP_LAN_RX];
	ipa_cmd_ip_packet_init_add(trans, endpoint->endpoint_id);
	ipa_cmd_ip_tag_status_add(trans);
	ipa_cmd_transfer_add(trans);
}

/* Returns the number of commands required to clear the pipeline */
u32 ipa_cmd_pipeline_clear_count(void)
{
	return 4;
}

void ipa_cmd_pipeline_clear_wait(struct ipa *ipa)
{
	unsigned long timeout_jiffies = msecs_to_jiffies(1000);

	if (!wait_for_completion_timeout(&ipa->completion, timeout_jiffies))
		dev_err(&ipa->pdev->dev, "%s time out\n", __func__);
}

/* Allocate a transaction for the command TX endpoint */
struct ipa_dma_trans *ipa_cmd_trans_alloc(struct ipa *ipa, u32 tre_count)
{
	struct ipa_endpoint *endpoint;

	if (WARN_ON(tre_count > IPA_COMMAND_TRANS_TRE_MAX))
		return NULL;

	endpoint = ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX];

	return ipa_dma_channel_trans_alloc(&ipa->ipa_dma, endpoint->channel_id,
				       tre_count, DMA_NONE);
}

/* Init function for immediate commands; there is no ipa_cmd_exit() */
int ipa_cmd_init(struct ipa *ipa)
{
	ipa_cmd_validate_build();

	if (!ipa_cmd_header_init_local_valid(ipa))
		return -EINVAL;

	if (!ipa_cmd_register_write_valid(ipa))
		return -EINVAL;

	return 0;
}
