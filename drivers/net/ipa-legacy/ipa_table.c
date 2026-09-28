// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2023 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bits.h>
#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>

#include "ipa.h"
#include "ipa_version.h"
#include "ipa_endpoint.h"
#include "ipa_table.h"
#include "ipa_reg.h"
#include "ipa_mem.h"
#include "ipa_cmd.h"
#include "ipa_dma.h"
#include "ipa_dma_trans.h"

/**
 * DOC: IPA Filter and Route Tables
 *
 * The IPA has tables defined in its local (IPA-resident) memory that define
 * filter and routing rules.  An entry in either of these tables is a little
 * endian 64-bit "slot" that holds the address of a rule definition.  (The
 * size of these slots is 64 bits regardless of the host DMA address size.)
 *
 * Separate tables (both filter and route) are used for IPv4 and IPv6.  There
 * is normally another set of "hashed" filter and route tables, which are
 * used with a hash of message metadata.  Hashed operation is not supported
 * by all IPA hardware (IPA v4.2 doesn't support hashed tables).
 *
 * Rules can be in local memory or in DRAM (system memory).  The offset of
 * an object (such as a route or filter table) in IPA-resident memory must
 * 128-byte aligned.  An object in system memory (such as a route or filter
 * rule) must be at an 8-byte aligned address.  We currently only place
 * route or filter rules in system memory.
 *
 * A rule consists of a contiguous block of 32-bit values terminated with
 * 32 zero bits.  A special "zero entry" rule consisting of 64 zero bits
 * represents "no filtering" or "no routing," and is the reset value for
 * filter or route table rules.
 *
 * Each filter rule is associated with an AP or modem TX endpoint, though
 * not all TX endpoints support filtering.  The first 64-bit slot in a
 * filter table is a bitmap indicating which endpoints have entries in
 * the table.  Each set bit in this bitmap indicates the presence of the
 * address of a filter rule in the memory following the bitmap.  Until IPA
 * v5.0,  the low-order bit (bit 0) in this bitmap represents a special
 * global filter, which applies to all traffic.  Otherwise the position of
 * each set bit represents an endpoint for which a filter rule is defined.
 *
 * The global rule is not used in current code, and support for it is
 * removed starting at IPA v5.0.  For IPA v5.0+, the endpoint bitmap
 * position defines the endpoint ID--i.e. if bit 1 is set in the endpoint
 * bitmap, endpoint 1 has a filter rule.  Older versions of IPA represent
 * the presence of a filter rule for endpoint X by bit (X + 1) being set.
 * I.e., bit 1 set indicates the presence of a filter rule for endpoint 0,
 * and bit 3 set means there is a filter rule present for endpoint 2.
 *
 * Each filter table entry has the address of a set of equations that
 * implement a filter rule.  So following the endpoint bitmap there
 * will be such an address/entry for each endpoint with a set bit in
 * the bitmap.
 *
 * The AP initializes all entries in a filter table to refer to a "zero"
 * rule.  Once initialized, the modem and AP update the entries for
 * endpoints they "own" directly.  Currently the AP does not use the IPA
 * filtering functionality.
 *
 * This diagram shows an example of a filter table with an endpoint
 * bitmap as defined prior to IPA v5.0.
 *
 *                    IPA Filter Table
 *                 ----------------------
 * endpoint bitmap | 0x0000000000000048 | Bits 3 and 6 set (endpoints 2 and 5)
 *                 |--------------------|
 * 1st endpoint    | 0x000123456789abc0 | DMA address for modem endpoint 2 rule
 *                 |--------------------|
 * 2nd endpoint    | 0x000123456789abf0 | DMA address for AP endpoint 5 rule
 *                 |--------------------|
 * (unused)        |                    | (Unused space in filter table)
 *                 |--------------------|
 *                          . . .
 *                 |--------------------|
 * (unused)        |                    | (Unused space in filter table)
 *                 ----------------------
 *
 * The set of available route rules is divided about equally between the AP
 * and modem.  The AP initializes all entries in a route table to refer to
 * a "zero entry".  Once initialized, the modem and AP are responsible for
 * updating their own entries.  All entries in a route table are usable,
 * though the AP currently does not use the IPA routing functionality.
 *
 *                    IPA Route Table
 *                 ----------------------
 * 1st modem route | 0x0001234500001100 | DMA address for first route rule
 *                 |--------------------|
 * 2nd modem route | 0x0001234500001140 | DMA address for second route rule
 *                 |--------------------|
 *                          . . .
 *                 |--------------------|
 * Last modem route| 0x0001234500002280 | DMA address for Nth route rule
 *                 |--------------------|
 * 1st AP route    | 0x0001234500001100 | DMA address for route rule (N+1)
 *                 |--------------------|
 * 2nd AP route    | 0x0001234500001140 | DMA address for next route rule
 *                 |--------------------|
 *                          . . .
 *                 |--------------------|
 * Last AP route   | 0x0001234500002280 | DMA address for last route rule
 *                 ----------------------
 */

/* Filter or route rules consist of a set of 32-bit values followed by a
 * 32-bit all-zero rule list terminator.  The "zero rule" is simply an
 * all-zero rule followed by the list terminator.
 */
#define IPA_ZERO_RULE_SIZE		(2 * sizeof(__le32))

/* Check things that can be validated at build time. */
static void ipa_table_validate_build(void)
{
	/* Filter and route tables contain DMA addresses that refer
	 * to filter or route rules.  But the size of a table entry
	 * is 64 bits regardless of what the size of an AP DMA address
	 * is.  A fixed constant defines the size of an entry, and
	 * code in ipa_table_init() uses a pointer to __le64 to
	 * initialize tables.
	 */
	BUILD_BUG_ON(sizeof(dma_addr_t) > sizeof(__le64));

	/* A "zero rule" is used to represent no filtering or no routing.
	 * It is a 64-bit block of zeroed memory.  Code in ipa_table_init()
	 * assumes that it can be written using a pointer to __le64.
	 */
	BUILD_BUG_ON(IPA_ZERO_RULE_SIZE != sizeof(__le64));
}

const struct ipa_mem *
ipa_table_mem(struct ipa *ipa, bool filter, bool ipv6)
{
	enum ipa_mem_id mem_id;

	mem_id = filter ? ipv6  ? IPA_MEM_V6_FILTER
				: IPA_MEM_V4_FILTER
			: ipv6  ? IPA_MEM_V6_ROUTE
				: IPA_MEM_V4_ROUTE;

	return ipa_mem_find(ipa, mem_id);
}

bool ipa_filtered_valid(struct ipa *ipa, u64 filtered)
{
	struct device *dev = &ipa->pdev->dev;
	u32 count;

	if (!filtered) {
		dev_err(dev, "at least one filtering endpoint is required\n");

		return false;
	}

	count = hweight64(filtered);
	if (count > ipa->filter_count) {
		dev_err(dev, "too many filtering endpoints (%u > %u)\n",
			count, ipa->filter_count);

		return false;
	}

	return true;
}

/* Zero entry count means no table, so just return a 0 address */
static dma_addr_t ipa_table_addr(struct ipa *ipa, bool filter_mask, u16 count)
{
	u32 skip;

	if (!count)
		return 0;

	WARN_ON(count > max_t(u32, ipa->filter_count, ipa->route_count));

	/* Skip over the zero rule and possibly the filter mask */
	skip = filter_mask ? 1 : 2;

	return ipa->table_addr + skip * sizeof(*ipa->table_virt);
}

static void ipa_table_reset_add(struct ipa_dma_trans *trans, bool filter,
				bool ipv6, u16 first, u16 count)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	const struct ipa_mem *mem;
	dma_addr_t addr;
	u32 offset;
	u16 size;

	/* Nothing to do if the memory region is doesn't exist or is empty */
	mem = ipa_table_mem(ipa, filter, ipv6);
	if (!mem || !mem->size)
		return;

	if (filter)
		first++;	/* skip over bitmap */

	offset = mem->offset + first * sizeof(__le32);
	size = count * sizeof(__le32);
	addr = ipa_table_addr(ipa, false, count);

	ipa_cmd_dma_shared_mem_add(trans, offset, size, addr, true);
}

/* Reset entries in a single filter table belonging to either the AP or
 * modem to refer to the zero entry.  The memory region supplied will be
 * for the IPv4 and IPv6 non-hashed and hashed filter tables.
 */
static int
ipa_filter_reset_table(struct ipa *ipa, bool ipv6, bool modem)
{
	struct ipa_dma *ipa_dma = &ipa->ipa_dma;
	enum ipa_ee_id ee_id = modem ? IPA_EE_MODEM : IPA_EE_AP;
	u64 ep_mask = ipa->filtered;
	u32 endpoint_id;

	/* Each entry that gets reset needs its own command, and a single
	 * transaction can hold only a limited number of them.  The IPA v2.x
	 * hardware supports filtering on enough endpoints to exceed that
	 * limit, so the endpoints are handled in batches.
	 */
	while (ep_mask) {
		struct ipa_dma_trans *trans;
		u32 count = 0;

		/* Skip over any endpoint not owned by this EE, so the
		 * transaction that follows has at least one command.
		 */
		while (ep_mask) {
			endpoint_id = __ffs(ep_mask);
			if (ipa->endpoint[endpoint_id].ee_id == ee_id)
				break;
			ep_mask ^= BIT(endpoint_id);
		}
		if (!ep_mask)
			break;

		trans = ipa_cmd_trans_alloc(ipa, IPA_COMMAND_TRANS_TRE_MAX);
		if (!trans) {
			dev_err(&ipa->pdev->dev,
				"no transaction for %s filter reset\n",
				modem ? "modem" : "AP");
			return -EBUSY;
		}

		while (ep_mask && count < IPA_COMMAND_TRANS_TRE_MAX) {
			endpoint_id = __ffs(ep_mask);
			ep_mask ^= BIT(endpoint_id);

			if (ipa->endpoint[endpoint_id].ee_id != ee_id)
				continue;

			ipa_table_reset_add(trans, true, ipv6, endpoint_id, 1);
			count++;
		}

		ipa_dma->ops->trans_commit_wait(trans);
	}

	return 0;
}

/* Theoretically, each filter table could have more filter slots to
 * update than the maximum number of commands in a transaction.  So
 * we do each table separately.
 */
static int ipa_filter_reset(struct ipa *ipa, bool modem)
{
	int ret;

	ret = ipa_filter_reset_table(ipa, false, modem);
	if (ret)
		return ret;

	ret = ipa_filter_reset_table(ipa, true, modem);

	return ret;
}

/* The AP routes and modem routes are each contiguous within the
 * table.  We can update each table with a single command, and we
 * won't exceed the per-transaction command limit.
 * */
static int ipa_route_reset(struct ipa *ipa, bool modem)
{
	u32 modem_route_count = ipa->modem_route_count;
	struct ipa_dma *ipa_dma = &ipa->ipa_dma;
	struct ipa_dma_trans *trans;
	u16 first;
	u16 count;

	trans = ipa_cmd_trans_alloc(ipa, 4);
	if (!trans) {
		dev_err(&ipa->pdev->dev,
			"no transaction for %s route reset\n",
			modem ? "modem" : "AP");
		return -EBUSY;
	}

	if (modem) {
		first = 0;
		count = modem_route_count;
	} else {
		first = modem_route_count;
		count = ipa->route_count - modem_route_count;
	}

	ipa_table_reset_add(trans, false, false, first, count);

	ipa_table_reset_add(trans, false, true, first, count);

	ipa_dma->ops->trans_commit_wait(trans);

	return 0;
}

/* Point a filter or route table entry at a rule chain in system memory.
 *
 * A filter or route table lives in IPA-resident memory.  Its entries are the
 * addresses of rule chains in system memory, and they are programmed by
 * copying the entry from the table image in system memory (see
 * ipa_table_init()).  So update the image and copy the entry to the IPA.
 *
 * The image starts with the zero rule, and the table it represents begins
 * one entry later (the filter table bitmap).  Filter table entry 0 is the
 * bitmap, entry 1 the global filter entry, and the entry for endpoint N
 * follows at index N + 2.
 */
static int ipa_table_entry_set(struct ipa *ipa, bool filter, bool ipv6,
			       u32 index, dma_addr_t rule_addr)
{
	struct ipa_dma *ipa_dma = &ipa->ipa_dma;
	const struct ipa_mem *mem;
	struct ipa_dma_trans *trans;
	dma_addr_t entry_addr;
	__le32 *entry;
	u32 offset;

	mem = ipa_table_mem(ipa, filter, ipv6);
	if (!mem || !mem->size)
		return -EINVAL;

	/* Filter and route table images start with the zero rule */
	entry = (__le32 *)ipa->table_virt + index + 1;

	offset = mem->offset + index * sizeof(*entry);
	entry_addr = ipa->table_addr + (index + 1) * sizeof(*entry);

	trans = ipa_cmd_trans_alloc(ipa, 1);
	if (!trans) {
		dev_err(&ipa->pdev->dev,
			"no transaction for table entry set\n");
		return -EBUSY;
	}

	*entry = cpu_to_le32((u32)rule_addr);
	ipa_cmd_dma_shared_mem_add(trans, offset, sizeof(*entry),
				   entry_addr, true);

	ipa_dma->ops->trans_commit_wait(trans);

	return 0;
}

/* Point the filter table entry for an endpoint at a rule chain */
int ipa_table_filter_rule_set(struct ipa *ipa, bool ipv6, u32 endpoint_id,
			      dma_addr_t rule_addr)
{
	/* Filter table entry 0 is the endpoint bitmap, entry 1 the global
	 * filter entry, and endpoint N's entry follows at index N + 2.
	 */
	if (endpoint_id + 2 > ipa->filter_count)
		return -EINVAL;

	/* The modem owns the filter entries of its own pipes */
	if (ipa->endpoint[endpoint_id].ee_id == IPA_EE_MODEM)
		return -EINVAL;

	return ipa_table_entry_set(ipa, true, ipv6, endpoint_id + 2,
				   rule_addr);
}

/* Point a route table entry at a rule chain */
int ipa_table_route_rule_set(struct ipa *ipa, bool ipv6, u32 table_index,
			     dma_addr_t rule_addr)
{
	if (table_index >= ipa->route_count)
		return -EINVAL;

	/* The modem owns the first entries in a route table */
	if (table_index < ipa->modem_route_count)
		return -EINVAL;

	return ipa_table_entry_set(ipa, false, ipv6, table_index, rule_addr);
}

void ipa_table_reset(struct ipa *ipa, bool modem)
{
	struct device *dev = &ipa->pdev->dev;
	const char *ee_name;
	int ret;

	ee_name = modem ? "modem" : "AP";

	/* Report errors, but reset filter and route tables */
	ret = ipa_filter_reset(ipa, modem);
	if (ret)
		dev_err(dev, "error %d resetting filter table for %s\n",
				ret, ee_name);

	ret = ipa_route_reset(ipa, modem);
	if (ret)
		dev_err(dev, "error %d resetting route table for %s\n",
				ret, ee_name);
}

static void ipa_table_init_add(struct ipa_dma_trans *trans, bool filter, bool ipv6)
{
	struct ipa *ipa = container_of(trans->ipa_dma, struct ipa, ipa_dma);
	enum ipa_cmd_opcode opcode;
	const struct ipa_mem *mem;
	dma_addr_t addr;
	u32 zero_offset;
	u32 zero_size;
	u16 count;
	u16 size;

	opcode = filter ? ipv6 ? IPA_CMD_IP_V6_FILTER_INIT
			       : IPA_CMD_IP_V4_FILTER_INIT
			: ipv6 ? IPA_CMD_IP_V6_ROUTING_INIT
			       : IPA_CMD_IP_V4_ROUTING_INIT;

	/* The non-hashed region will exist (see ipa_table_mem_valid()) */
	mem = ipa_table_mem(ipa, filter, ipv6);

	/* Compute the number of table entries to initialize */
	if (filter) {
		/* The number of filtering endpoints determines number of
		 * entries in the filter table; we also add one more "slot"
		 * to hold the bitmap itself.  The size of the hashed filter
		 * table is either the same as the non-hashed one, or zero.
		 */
		count = 1 + hweight32(ipa->filtered);
	} else {
		/* The size of a route table region determines the number
		 * of entries it has.
		 */
		count = mem->size / sizeof(__le32);
	}
	size = count * sizeof(__le32);

	addr = ipa_table_addr(ipa, filter, count);

	ipa_cmd_table_init_add(trans, opcode, size, mem->offset, addr);
	if (!filter)
		return;

	/* Zero the unused space in the filter table */
	zero_offset = mem->offset + size;
	zero_size = mem->size - size;
	ipa_cmd_dma_shared_mem_add(trans, zero_offset, zero_size,
				   ipa->zero_addr, true);
}

/* The hardware consults a filter table entry for an endpoint only when the
 * endpoint's bit is set in the table's bitmap word (entry 0), so program
 * the bitmap from the table image.
 */
static int ipa_table_filter_bitmap_set(struct ipa *ipa, bool ipv6)
{
	struct ipa_dma *ipa_dma = &ipa->ipa_dma;
	const struct ipa_mem *mem;
	struct ipa_dma_trans *trans;
	__le32 *bitmap;
	dma_addr_t bitmap_addr;

	mem = ipa_table_mem(ipa, true, ipv6);
	if (!mem || !mem->size)
		return -EINVAL;

	/* The bitmap follows the zero rule in the table image */
	bitmap = ipa->table_virt + 1;
	bitmap_addr = ipa->table_addr + sizeof(*bitmap);

	trans = ipa_cmd_trans_alloc(ipa, 1);
	if (!trans)
		return -EBUSY;

	ipa_cmd_dma_shared_mem_add(trans, mem->offset, sizeof(*bitmap),
				   bitmap_addr, true);
	ipa_dma->ops->trans_commit_wait(trans);

	return 0;
}

int ipa_table_setup(struct ipa *ipa)
{
	struct ipa_dma *ipa_dma = &ipa->ipa_dma;
	struct ipa_dma_trans *trans;

	/* We will need at most 8 TREs:
	 * - IPv4:
	 *     - One for route table initialization (non-hashed and hashed)
	 *     - One for filter table initialization (non-hashed and hashed)
	 *     - One to zero unused entries in the non-hashed filter table
	 *     - One to zero unused entries in the hashed filter table
	 * - IPv6:
	 *     - One for route table initialization (non-hashed and hashed)
	 *     - One for filter table initialization (non-hashed and hashed)
	 *     - One to zero unused entries in the non-hashed filter table
	 *     - One to zero unused entries in the hashed filter table
	 * All platforms support at least 8 TREs in a transaction.
	 */
	trans = ipa_cmd_trans_alloc(ipa, 8);
	if (!trans) {
		dev_err(&ipa->pdev->dev, "no transaction for table setup\n");
		return -EBUSY;
	}

	ipa_table_init_add(trans, false, false);
	ipa_table_init_add(trans, false, true);
	ipa_table_init_add(trans, true, false);
	ipa_table_init_add(trans, true, true);

	ipa_dma->ops->trans_commit_wait(trans);

	/* Program the filter table bitmaps */
	ipa_table_filter_bitmap_set(ipa, false);
	ipa_table_filter_bitmap_set(ipa, true);

	return 0;
}

/* Tell the hardware where the header table lives.  The hardware inserts
 * headers from it for packets routed with a header.
 */
int ipa_table_header_setup(struct ipa *ipa)
{
	struct ipa_dma *ipa_dma = &ipa->ipa_dma;
	struct ipa_dma_trans *trans;

	trans = ipa_cmd_trans_alloc(ipa, 1);
	if (!trans) {
		dev_err(&ipa->pdev->dev,
			"no transaction for header table setup\n");
		return -EBUSY;
	}

	ipa_cmd_hdr_init_system_add(trans, ipa->hdr_addr);
	ipa_dma->ops->trans_commit_wait(trans);

	return 0;
}

/* Verify the sizes of all IPA table filter or routing table memory regions
 * are valid.  If valid, this records the size of the routing table.
 */
bool ipa_table_mem_valid(struct ipa *ipa, bool filter)
{
	const struct ipa_mem *mem_ipv4;
	const struct ipa_mem *mem_ipv6;
	u32 count;

	/* IPv4 and IPv6 non-hashed tables are expected to be defined and
	 * have the same size.  Both must have at least two entries (and
	 * would normally have more than that).
	 */
	mem_ipv4 = ipa_table_mem(ipa, filter, false);
	if (!mem_ipv4)
		return false;

	mem_ipv6 = ipa_table_mem(ipa, filter, true);
	if (!mem_ipv6)
		return false;

	if (mem_ipv4->size != mem_ipv6->size)
		return false;

	/* Compute and record the number of entries for each table type */
	count = mem_ipv4->size / sizeof(__le32);
	if (count < 2)
		return false;
	if (filter)
		ipa->filter_count = count - 1;	/* Filter map in first entry */
	else
		ipa->route_count = count;

	/* Make sure the regions are big enough */
	if (filter) {
		/* Filter tables must able to hold the endpoint bitmap plus
		 * an entry for each endpoint that supports filtering
		 */
		if (count < 1 + hweight32(ipa->filtered))
			return false;
	} else {
		/* Routing tables must be able to hold all modem entries,
		 * plus at least one entry for the AP.
		 */
		if (count < ipa->modem_route_count + 1)
			return false;
	}

	return true;
}

/* Initialize a coherent DMA allocation containing initialized filter and
 * route table data.  This is used when initializing or resetting the IPA
 * filter or route table.
 *
 * The first entry in a filter table contains a bitmap indicating which
 * endpoints contain entries in the table.  In addition to that first entry,
 * there is a fixed maximum number of entries that follow.  Filter table
 * entries are 64 bits wide, and (other than the bitmap) contain the DMA
 * address of a filter rule.  A "zero rule" indicates no filtering, and
 * consists of 64 bits of zeroes.  When a filter table is initialized (or
 * reset) its entries are made to refer to the zero rule.
 *
 * Each entry in a route table is the DMA address of a routing rule.  For
 * routing there is also a 64-bit "zero rule" that means no routing, and
 * when a route table is initialized or reset, its entries are made to refer
 * to the zero rule.  The zero rule is shared for route and filter tables.
 *
 *	     +-------------------+
 *	 --> |     zero rule     |
 *	/    |-------------------|
 *	|    |     filter mask   |
 *	|\   |-------------------|
 *	| ---- zero rule address | \
 *	|\   |-------------------|  |
 *	| ---- zero rule address |  |	Max IPA filter count
 *	|    |-------------------|   >	or IPA route count,
 *	|	      ...	    |	whichever is greater
 *	 \   |-------------------|  |
 *	  ---- zero rule address | /
 *	     +-------------------+
 */
int ipa_table_init(struct ipa *ipa)
{
	struct device *dev = &ipa->pdev->dev;
	__le32 filtered = ipa->filtered << 1;
	dma_addr_t addr;
	__le32 le_addr;
	__le32 *ptr;
	void *virt;
	size_t size;
	u32 count;

	ipa_table_validate_build();

	count = max_t(u32, ipa->filter_count, ipa->route_count);

	/* The IPA hardware requires route and filter table rules to be
	 * aligned on a 128-byte boundary.  We put the "zero rule" at the
	 * base of the table area allocated here.  The DMA address returned
	 * by dma_alloc_coherent() is guaranteed to be a power-of-2 number
	 * of pages, which satisfies the rule alignment requirement.
	 */
	size = IPA_ZERO_RULE_SIZE + (1 + count) * sizeof(__le32);
	virt = dma_alloc_coherent(dev, size, &addr, GFP_KERNEL);
	if (!virt)
		return -ENOMEM;

	ipa->table_virt = virt;
	ipa->table_addr = addr;

	/* First slot is the zero rule */
	ptr = ipa->table_virt;
	*ptr++ = 0;

	/* Next is the filter table bitmap.  The "soft" bitmap value might
	 * need to be converted to the hardware representation by shifting
	 * it left one position.  Prior to IPA v5.0, bit 0 repesents global
	 * filtering, which is possible but not used.  IPA v5.0+ eliminated
	 * that option, so there's no shifting required.
	 */
	filtered |= 1;
	*ptr++ = cpu_to_le32(filtered);

	/* All the rest contain the DMA address of the zero rule */
	le_addr = cpu_to_le32(addr);
	while (count--)
		*ptr++ = le_addr;

	/* Memory for filter rules installed at the modem's request.  The
	 * rules live in system memory and the filter table refers to them.
	 */
	ipa->rule_size = PAGE_SIZE;
	ipa->rule_virt = dma_alloc_coherent(dev, ipa->rule_size,
					    &ipa->rule_addr, GFP_KERNEL);
	if (!ipa->rule_virt)
		goto err_free_table;

	/* The header table holds the headers the hardware inserts (for
	 * example the QMAP header of a downlink packet).  It is read from
	 * system memory; the hardware only needs to be told its address.
	 */
	ipa->hdr_size = PAGE_SIZE;
	ipa->hdr_virt = dma_alloc_coherent(dev, ipa->hdr_size,
					   &ipa->hdr_addr, GFP_KERNEL);
	if (!ipa->hdr_virt)
		goto err_free_rules;

	memset(ipa->hdr_virt, 0, ipa->hdr_size);

	return 0;

err_free_rules:
	dma_free_coherent(dev, ipa->rule_size, ipa->rule_virt, ipa->rule_addr);
	ipa->rule_virt = NULL;

err_free_table:
	dma_free_coherent(dev, size, ipa->table_virt, ipa->table_addr);
	ipa->table_addr = 0;
	ipa->table_virt = NULL;

	return -ENOMEM;
}

void ipa_table_exit(struct ipa *ipa)
{
	u32 count = max_t(u32, 1 + ipa->filter_count, ipa->route_count);
	struct device *dev = &ipa->pdev->dev;
	size_t size;

	if (ipa->hdr_virt) {
		dma_free_coherent(dev, ipa->hdr_size, ipa->hdr_virt,
				  ipa->hdr_addr);
		ipa->hdr_virt = NULL;
		ipa->hdr_addr = 0;
		ipa->hdr_size = 0;
	}

	if (ipa->rule_virt) {
		dma_free_coherent(dev, ipa->rule_size, ipa->rule_virt,
				  ipa->rule_addr);
		ipa->rule_virt = NULL;
		ipa->rule_addr = 0;
		ipa->rule_size = 0;
	}

	size = IPA_ZERO_RULE_SIZE + (1 + count) * sizeof(__le32);

	dma_free_coherent(dev, size, ipa->table_virt, ipa->table_addr);
	ipa->table_addr = 0;
	ipa->table_virt = NULL;
}
