/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2022 Linaro Ltd.
 */
#ifndef _IPA_TABLE_H_
#define _IPA_TABLE_H_

#include <linux/types.h>

struct ipa;

/**
 * ipa_filtered_valid() - Validate a filter table endpoint bitmap
 * @ipa:	IPA pointer
 * @filtered:	Filter table endpoint bitmap to check
 *
 * Return:	true if all regions are valid, false otherwise
 */
bool ipa_filtered_valid(struct ipa *ipa, u64 filtered);

/**
 * ipa_table_reset() - Reset filter and route tables entries to "none"
 * @ipa:	IPA pointer
 * @modem:	Whether to reset modem or AP entries
 */
void ipa_table_reset(struct ipa *ipa, bool modem);

/**
 * ipa_table_filter_rule_set() - Point a filter table entry at a rule chain
 * @ipa:	IPA pointer
 * @ipv6:	Rule applies to the IPv6 filter table
 * @endpoint_id: Endpoint whose entry is updated
 * @rule_addr:	DMA address of the rule chain in system memory
 *
 * Return:	0 if successful, or a negative error code
 */
int ipa_table_filter_rule_set(struct ipa *ipa, bool ipv6, u32 endpoint_id,
			      dma_addr_t rule_addr);

/**
 * ipa_table_header_setup() - Tell the hardware where the header table is
 * @ipa:	IPA pointer
 *
 * The header table holds the headers the hardware inserts for packets that
 * are routed with a header (for example the QMAP header of a downlink
 * packet).  Its entries are read from system memory.
 *
 * Return:	0 if successful, or a negative error code
 */
int ipa_table_header_setup(struct ipa *ipa);

/**
 * ipa_table_route_rule_set() - Point a route table entry at a rule chain
 * @ipa:	IPA pointer
 * @ipv6:	Rule applies to the IPv6 route table
 * @table_index: Route table index whose entry is updated
 * @rule_addr:	DMA address of the rule chain in system memory
 *
 * Return:	0 if successful, or a negative error code
 */
int ipa_table_route_rule_set(struct ipa *ipa, bool ipv6, u32 table_index,
			     dma_addr_t rule_addr);


/**
 * ipa_table_setup() - Set up filter and route tables
 * @ipa:	IPA pointer
 *
 * There is no need for a matching ipa_table_teardown() function.
 */
int ipa_table_setup(struct ipa *ipa);

/**
 * ipa_table_init() - Do early initialization of filter and route tables
 * @ipa:	IPA pointer
 */
int ipa_table_init(struct ipa *ipa);

/**
 * ipa_table_exit() - Inverse of ipa_table_init()
 * @ipa:	IPA pointer
 */
void ipa_table_exit(struct ipa *ipa);

/**
 * ipa_table_mem_valid() - Validate sizes of table memory regions
 * @ipa:	IPA pointer
 * @filter:	Whether to check filter or routing tables
 */
bool ipa_table_mem_valid(struct ipa *ipa, bool filter);

/* TEMP (dump) */
const struct ipa_mem *ipa_table_mem(struct ipa *ipa, bool filter, bool ipv6);

#endif /* _IPA_TABLE_H_ */
