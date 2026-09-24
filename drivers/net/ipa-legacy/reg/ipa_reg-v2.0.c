// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2022, Yassine Oudjana <y.oudjana@protonmail.com>
 */

#include <linux/types.h>

#include "../ipa.h"
#include "../ipa_reg.h"

static const u32 reg_comp_cfg_fmask[] = {
	[COMP_CFG_ENABLE]				= BIT(0),
						/* Bits 1-31 reserved */
};
REG_FIELDS(COMP_CFG, comp_cfg, 0x00000038);

REG(COMP_SW_RESET, comp_sw_reset, 0x0000003c);


static const u32 reg_route_fmask[] = {
	[ROUTE_DIS]					= BIT(0),
	[ROUTE_DEF_PIPE]				= GENMASK(5, 1),
	[ROUTE_DEF_HDR_TABLE]				= BIT(6),
	[ROUTE_DEF_HDR_OFST]				= GENMASK(16, 7),
	[ROUTE_FRAG_DEF_PIPE]				= GENMASK(21, 17),
						/* Bits 22-31 reserved */
};

REG_FIELDS(ROUTE, route, 0x00000044);

static const u32 reg_shared_mem_size_fmask[] = {
	[MEM_SIZE]					= GENMASK(15, 0),
	[MEM_BADDR]					= GENMASK(31, 16),
};

REG_FIELDS(SHARED_MEM_SIZE, shared_mem_size, 0x00000050);

static const u32 reg_endp_init_ctrl_fmask[] = {
	[ENDP_SUSPEND]					= BIT(0),
	[ENDP_DELAY]					= BIT(1),
						/* Bits 2-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_INIT_CTRL, endp_init_ctrl, 0x00000070, 0x0004);

static const u32 reg_endp_init_cfg_fmask[] = {
	[FRAG_OFFLOAD_EN]				= BIT(0),
	[CS_OFFLOAD_EN]					= GENMASK(2, 1),
	[CS_METADATA_HDR_OFFSET]			= GENMASK(6, 3),
						/* Bits 7-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_INIT_CFG, endp_init_cfg, 0x000000c0, 0x0004);

static const u32 reg_endp_init_nat_fmask[] = {
	[NAT_EN]					= GENMASK(1, 0),
						/* Bits 2-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_INIT_NAT, endp_init_nat, 0x00000120, 0x0004);

static const u32 reg_endp_init_hdr_fmask[] = {
	[HDR_LEN]					= GENMASK(5, 0),
	[HDR_OFST_METADATA_VALID]			= BIT(6),
	[HDR_OFST_METADATA]				= GENMASK(12, 7),
	[HDR_ADDITIONAL_CONST_LEN]			= GENMASK(18, 13),
	[HDR_OFST_PKT_SIZE_VALID]			= BIT(19),
	[HDR_OFST_PKT_SIZE]				= GENMASK(25, 20),
	[HDR_A5_MUX]					= BIT(26),
	[HDR_LEN_INC_DEAGG_HDR]				= BIT(27),
	[HDR_METADATA_REG_VALID]			= BIT(28),
						/* Bits 29-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_INIT_HDR, endp_init_hdr, 0x00000170, 0x0004);

static const u32 reg_endp_init_hdr_ext_fmask[] = {
	[HDR_ENDIANNESS]				= BIT(0),
	[HDR_TOTAL_LEN_OR_PAD_VALID]			= BIT(1),
	[HDR_TOTAL_LEN_OR_PAD]				= BIT(2),
	[HDR_PAYLOAD_LEN_INC_PADDING]			= BIT(3),
	[HDR_TOTAL_LEN_OR_PAD_OFFSET]			= GENMASK(9, 4),
	[HDR_PAD_TO_ALIGNMENT]				= GENMASK(13, 10),
						/* Bits 14-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_INIT_HDR_EXT, endp_init_hdr_ext, 0x000001c0, 0x0004);

REG_STRIDE(ENDP_INIT_HDR_METADATA_MASK, endp_init_hdr_metadata_mask, 0x00000220, 0x0004);

static const u32 reg_endp_init_mode_fmask[] = {
	[ENDP_MODE]					= GENMASK(2, 0),
						/* Bit 3 reserved */
	[DEST_PIPE_INDEX]				= GENMASK(8, 4),
						/* Bits 9-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_INIT_MODE, endp_init_mode, 0x000002c0, 0x0004);

static const u32 reg_endp_init_aggr_fmask[] = {
	[AGGR_EN]					= GENMASK(1, 0),
	[AGGR_TYPE]					= GENMASK(4, 2),
	[BYTE_LIMIT]					= GENMASK(9, 5),
	[TIME_LIMIT]					= GENMASK(14, 10),
	[PKT_LIMIT]					= GENMASK(20, 15),
	[SW_EOF_ACTIVE]					= BIT(21),
	[FORCE_CLOSE]					= BIT(22),
						/* Bits 23-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_INIT_AGGR, endp_init_aggr, 0x00000320, 0x0004);

static const u32 reg_endp_init_hol_block_en_fmask[] = {
	[HOL_BLOCK_EN]					= BIT(0),
						/* Bits 1-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_INIT_HOL_BLOCK_EN, endp_init_hol_block_en, 0x000003c0, 0x0004);

static const u32 reg_endp_init_hol_block_timer_fmask[] = {
	[TIMER_BASE_VALUE]				= GENMASK(8, 0),
						/* Bits 9-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_INIT_HOL_BLOCK_TIMER, endp_init_hol_block_timer, 0x00000420, 0x0004);

static const u32 reg_endp_init_deaggr_fmask[] = {
	[DEAGGR_HDR_LEN]				= GENMASK(5, 0),
	[PACKET_OFFSET_VALID]				= BIT(6),
						/* Bit 7 reserved */
	[PACKET_OFFSET_LOCATION]			= GENMASK(13, 8),
						/* Bits 14-15 reserved */
	[MAX_PACKET_LEN]				= GENMASK(31, 16),
};

REG_STRIDE_FIELDS(ENDP_INIT_DEAGGR, endp_init_deaggr, 0x00000470, 0x0004);

static const u32 reg_endp_status_fmask[] = {
	[STATUS_EN]					= BIT(0),
	[STATUS_ENDP]					= GENMASK(5, 1),
						/* Bits 6-31 reserved */
};

REG_STRIDE_FIELDS(ENDP_STATUS, endp_status, 0x000004c0, 0x0004);

REG(IPA_IRQ_STTS, ipa_irq_stts, 0x00001008 + 0x1000 * IPA_EE_AP);

REG(IPA_IRQ_EN, ipa_irq_en, 0x0000100c + 0x1000 * IPA_EE_AP);

REG(IPA_IRQ_CLR, ipa_irq_clr, 0x00001010 + 0x1000 * IPA_EE_AP);

REG(IRQ_SUSPEND_INFO, irq_suspend_info, 0x00001098 + 0x1000 * IPA_EE_AP);

static const struct reg *reg_array[] = {
	[COMP_CFG]			= &reg_comp_cfg,
	[COMP_SW_RESET]			= &reg_comp_sw_reset,
	[ROUTE]				= &reg_route,
	[SHARED_MEM_SIZE]		= &reg_shared_mem_size,
	[ENDP_INIT_CTRL]		= &reg_endp_init_ctrl,
	[ENDP_INIT_CFG]			= &reg_endp_init_cfg,
	[ENDP_INIT_NAT]			= &reg_endp_init_nat,
	[ENDP_INIT_HDR]			= &reg_endp_init_hdr,
	[ENDP_INIT_HDR_EXT]		= &reg_endp_init_hdr_ext,
	[ENDP_INIT_HDR_METADATA_MASK]	= &reg_endp_init_hdr_metadata_mask,
	[ENDP_INIT_MODE]		= &reg_endp_init_mode,
	[ENDP_INIT_AGGR]		= &reg_endp_init_aggr,
	[ENDP_INIT_HOL_BLOCK_EN]	= &reg_endp_init_hol_block_en,
	[ENDP_INIT_HOL_BLOCK_TIMER]	= &reg_endp_init_hol_block_timer,
	[ENDP_INIT_DEAGGR]		= &reg_endp_init_deaggr,
	[ENDP_STATUS]			= &reg_endp_status,
	[IPA_IRQ_STTS]			= &reg_ipa_irq_stts,
	[IPA_IRQ_EN]			= &reg_ipa_irq_en,
	[IPA_IRQ_CLR]			= &reg_ipa_irq_clr,
	[IRQ_SUSPEND_INFO]		= &reg_irq_suspend_info,
};

const struct regs ipa_regs_v2_0 = {
	.reg_count	= ARRAY_SIZE(reg_array),
	.reg		= reg_array,
};
