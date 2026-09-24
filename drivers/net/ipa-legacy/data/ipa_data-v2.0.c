// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2020 Linaro Ltd.
 */

#include <linux/log2.h>

#include "../ipa_dma.h"
#include "../ipa_data.h"
#include "../ipa_endpoint.h"
#include "../ipa_mem.h"

/* Endpoint configuration for the IPA v2 hardware. */
static const struct ipa_dma_endpoint_data ipa_endpoint_data[] = {
	[IPA_ENDPOINT_AP_COMMAND_TX] = {
		.ee_id		= IPA_EE_AP,
		.channel_id	= 3,
		.endpoint_id	= 3,
		.channel_name	= "cmd_tx",
		.toward_ipa	= true,
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 20,
		},
		.endpoint = {
			.config	= {
				.dma_mode	= true,
				.dma_endpoint	= IPA_ENDPOINT_AP_LAN_RX,
			},
		},
	},
	[IPA_ENDPOINT_AP_LAN_RX] = {
		.ee_id		= IPA_EE_AP,
		.channel_id	= 2,
		.endpoint_id	= 2,
		.channel_name	= "ap_lan_rx",
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 8,
		},
		.endpoint	= {
			.config	= {
				.aggregation	= true,
				.status_enable	= true,
				.rx = {
					.buffer_size	= 2048,
					.pad_align	= ilog2(sizeof(u32)),
					.aggr_time_limit = 500,
				},
			},
		},
	},
	[IPA_ENDPOINT_AP_MODEM_TX] = {
		.ee_id		= IPA_EE_AP,
		.channel_id	= 4,
		.endpoint_id	= 4,
		.channel_name	= "ap_modem_tx",
		.toward_ipa	= true,
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 8,
		},
		.endpoint	= {
			.config	= {
				.qmap		= true,
				.status_enable	= true,
				.tx = {
					.status_endpoint =
						IPA_ENDPOINT_AP_LAN_RX,
				},
			},
		},
	},
	[IPA_ENDPOINT_AP_MODEM_RX] = {
		.ee_id		= IPA_EE_AP,
		.channel_id	= 5,
		.endpoint_id	= 5,
		.channel_name	= "ap_modem_rx",
		.toward_ipa	= false,
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 8,
		},
		.endpoint	= {
			.config = {
				.aggregation	= true,
				.qmap		= true,
				.rx = {
					.buffer_size	= 2048,
					.aggr_time_limit = 500,
					.aggr_close_eof	= true,
				},
			},
		},
	},
	[IPA_ENDPOINT_MODEM_LAN_TX] = {
		.ee_id		= IPA_EE_MODEM,
		.channel_id	= 6,
		.endpoint_id	= 6,
		.channel_name	= "modem_lan_tx",
		.toward_ipa	= true,
	},
	[IPA_ENDPOINT_MODEM_COMMAND_TX] = {
		.ee_id		= IPA_EE_MODEM,
		.channel_id	= 7,
		.endpoint_id	= 7,
		.channel_name	= "modem_cmd_tx",
		.toward_ipa	= true,
	},
	[IPA_ENDPOINT_MODEM_LAN_RX] = {
		.ee_id		= IPA_EE_MODEM,
		.channel_id	= 8,
		.endpoint_id	= 8,
		.channel_name	= "modem_lan_rx",
		.toward_ipa	= false,
	},
	[IPA_ENDPOINT_MODEM_AP_RX] = {
		.ee_id		= IPA_EE_MODEM,
		.channel_id	= 9,
		.endpoint_id	= 9,
		.channel_name	= "modem_ap_rx",
		.toward_ipa	= false,
	},
};

/* IPA-resident memory region configuration for v2.0 */
static const struct ipa_mem ipa_mem_local_data[] = {
	{
		.id		= IPA_MEM_UC_SHARED,
		.offset         = 0,
		.size           = 0x80,
		.canary_count   = 0,
	},
	{
		.id		= IPA_MEM_V4_FILTER,
		.offset		= 0x0080,
		.size		= 0x0058,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_V6_FILTER,
		.offset		= 0x00e0,
		.size		= 0x0058,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V4_ROUTE,
		.offset		= 0x0140,
		.size		= 0x002c,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V6_ROUTE,
		.offset		= 0x0170,
		.size		= 0x002c,
		.canary_count	= 1,
	},
	{
		.id		= IPA_MEM_MODEM_HEADER,
		.offset		= 0x01a0,
		.size		= 0x0140,
		.canary_count	= 1,
	},
	{
		.id		= IPA_MEM_AP_HEADER,
		.offset		= 0x02e0,
		.size		= 0x0048,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_MODEM,
		.offset		= 0x032c,
		.size		= 0x0dcc,
		.canary_count	= 1,
	},
	{
		.id		= IPA_MEM_V4_FILTER_AP,
		.offset		= 0x10fc,
		.size		= 0x0780,
		.canary_count	= 1,
	},
	{
		.id		= IPA_MEM_V6_FILTER_AP,
		.offset		= 0x187c,
		.size		= 0x055c,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_UC_INFO,
		.offset		= 0x1ddc,
		.size		= 0x0124,
		.canary_count	= 1,
	},
};

static struct ipa_mem_data ipa_mem_data = {
	.local_count	= ARRAY_SIZE(ipa_mem_local_data),
	.local		= ipa_mem_local_data,
	.smem_id	= 497,
	.smem_size	= 0x00001f00,
};

static struct ipa_interconnect_data ipa_interconnect_data[] = {
	{
		.name = "memory",
		.peak_bandwidth	= 1300000,	/* 1300 MBps */
		.average_bandwidth = 100000,	/* 100 MBps */
	},
	{
		.name = "imem",
		.peak_bandwidth	= 1300000,	/* 1300 MBps */
		.average_bandwidth = 100000,	/* 100 MBps */
	},
};

static struct ipa_power_data ipa_power_data = {
	.core_clock_rate	= 200 * 1000 * 1000,	/* Hz */
	.interconnect_count	= ARRAY_SIZE(ipa_interconnect_data),
	.interconnect_data	= ipa_interconnect_data,
};

/* Configuration data for IPA v2.0 */
const struct ipa_data ipa_data_v2_0 = {
	.version	= IPA_VERSION_2_0,
	.modem_route_count      = 8,
	.endpoint_count	= ARRAY_SIZE(ipa_endpoint_data),
	.endpoint_data	= ipa_endpoint_data,
	.mem_data	= &ipa_mem_data,
	.power_data	= &ipa_power_data,
};
