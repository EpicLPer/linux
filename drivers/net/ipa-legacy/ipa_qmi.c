// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2022 Linaro Ltd.
 */

#include "ipa_version.h"
#include <linux/unaligned.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/qrtr.h>
#include <linux/soc/qcom/qmi.h>

#include "ipa.h"
#include "ipa_endpoint.h"
#include "ipa_mem.h"
#include "ipa_table.h"
#include "ipa_modem.h"
#include "ipa_qmi_msg.h"
#include "ipa_cmd.h"

/**
 * DOC: AP/Modem QMI Handshake
 *
 * The AP and modem perform a "handshake" at initialization time to ensure
 * both sides know when everything is ready to begin operating.  The AP
 * driver (this code) uses two QMI handles (endpoints) for this; a client
 * using a service on the modem, and server to service modem requests (and
 * to supply an indication message from the AP).  Once the handshake is
 * complete, the AP and modem may begin IPA operation.  This occurs
 * only when the AP IPA driver, modem IPA driver, and IPA microcontroller
 * are ready.
 *
 * The QMI service on the modem expects to receive an INIT_DRIVER request from
 * the AP, which contains parameters used by the modem during initialization.
 * The AP sends this request as soon as it is knows the modem side service
 * is available.  The modem responds to this request, and if this response
 * contains a success result, the AP knows the modem IPA driver is ready.
 *
 * The modem is responsible for loading firmware on the IPA microcontroller.
 * This occurs only during the initial modem boot.  The modem sends a
 * separate DRIVER_INIT_COMPLETE request to the AP to report that the
 * microcontroller is ready.  The AP may assume the microcontroller is
 * ready and remain so (even if the modem reboots) once it has received
 * and responded to this request.
 *
 * There is one final exchange involved in the handshake.  It is required
 * on the initial modem boot, but optional (but in practice does occur) on
 * subsequent boots.  The modem expects to receive a final INIT_COMPLETE
 * indication message from the AP when it is about to begin its normal
 * operation.  The AP will only send this message after it has received
 * and responded to an INDICATION_REGISTER request from the modem.
 *
 * So in summary:
 * - Whenever the AP learns the modem has booted and its IPA QMI service
 *   is available, it sends an INIT_DRIVER request to the modem.  The
 *   modem supplies a success response when it is ready to operate.
 * - On the initial boot, the modem sets up the IPA microcontroller, and
 *   sends a DRIVER_INIT_COMPLETE request to the AP when this is done.
 * - When the modem is ready to receive an INIT_COMPLETE indication from
 *   the AP, it sends an INDICATION_REGISTER request to the AP.
 * - On the initial modem boot, everything is ready when:
 *	- AP has received a success response from its INIT_DRIVER request
 *	- AP has responded to a DRIVER_INIT_COMPLETE request
 *	- AP has responded to an INDICATION_REGISTER request from the modem
 *	- AP has sent an INIT_COMPLETE indication to the modem
 * - On subsequent modem boots, everything is ready when:
 *	- AP has received a success response from its INIT_DRIVER request
 *	- AP has responded to a DRIVER_INIT_COMPLETE request
 * - The INDICATION_REGISTER request and INIT_COMPLETE indication are
 *   optional for non-initial modem boots, and have no bearing on the
 *   determination of when things are "ready"
 */

#define IPA_HOST_SERVICE_SVC_ID		0x31
#define IPA_HOST_SVC_VERS		1
#define IPA_HOST_SERVICE_INS_ID		1

#define IPA_MODEM_SERVICE_SVC_ID	0x31
#define IPA_MODEM_SERVICE_INS_ID	2
#define IPA_MODEM_SVC_VERS		1

#define QMI_INIT_DRIVER_TIMEOUT		60000	/* A minute in milliseconds */

/* Send an INIT_COMPLETE indication message to the modem */
static void ipa_server_init_complete(struct ipa_qmi *ipa_qmi)
{
	struct ipa *ipa = container_of(ipa_qmi, struct ipa, qmi);
	struct qmi_handle *qmi = &ipa_qmi->server_handle;
	struct sockaddr_qrtr *sq = &ipa_qmi->modem_sq;
	struct ipa_init_complete_ind ind = { };
	int ret;

	ind.status.result = QMI_RESULT_SUCCESS_V01;
	ind.status.error = QMI_ERR_NONE_V01;

	ret = qmi_send_indication(qmi, sq, IPA_QMI_INIT_COMPLETE,
				   IPA_QMI_INIT_COMPLETE_IND_SZ,
				   ipa_init_complete_ind_ei, &ind);
	if (ret)
		dev_err(&ipa->pdev->dev,
			"error %d sending init complete indication\n", ret);
	else
		ipa_qmi->indication_sent = true;
}

/* If requested (and not already sent) send the INIT_COMPLETE indication */
static void ipa_qmi_indication(struct ipa_qmi *ipa_qmi)
{
	if (!ipa_qmi->indication_requested)
		return;

	if (ipa_qmi->indication_sent)
		return;

	ipa_server_init_complete(ipa_qmi);
}

/* Determine whether everything is ready to start normal operation.
 * We know everything (else) is ready when we know the IPA driver on
 * the modem is ready, and the microcontroller is ready.
 *
 * When the modem boots (or reboots), the handshake sequence starts
 * with the AP sending the modem an INIT_DRIVER request.  Within
 * that request, the uc_loaded flag will be zero (false) for an
 * initial boot, non-zero (true) for a subsequent (SSR) boot.
 */
static void ipa_qmi_ready(struct ipa_qmi *ipa_qmi)
{
	struct ipa *ipa;
	int ret;

	/* We aren't ready until the modem and microcontroller are */
	if (!ipa_qmi->modem_ready || !ipa_qmi->uc_ready)
		return;

	/* Send the indication message if it was requested */
	ipa_qmi_indication(ipa_qmi);

	/* The initial boot requires us to send the indication. */
	if (ipa_qmi->initial_boot) {
		if (!ipa_qmi->indication_sent)
			return;

		/* The initial modem boot completed successfully */
		ipa_qmi->initial_boot = false;
	}

	/* We're ready.  Start up normal operation */
	ipa = container_of(ipa_qmi, struct ipa, qmi);
	ret = ipa_modem_start(ipa);
	if (ret)
		dev_err(&ipa->pdev->dev, "error %d starting modem\n", ret);
}

/* All QMI clients from the modem node are gone (modem shut down or crashed). */
static void ipa_server_bye(struct qmi_handle *qmi, unsigned int node)
{
	struct ipa_qmi *ipa_qmi;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);

	/* The modem client and server go away at the same time */
	memset(&ipa_qmi->modem_sq, 0, sizeof(ipa_qmi->modem_sq));

	/* initial_boot doesn't change when modem reboots */
	/* uc_ready doesn't change when modem reboots */
	ipa_qmi->modem_ready = false;
	ipa_qmi->indication_requested = false;
	ipa_qmi->indication_sent = false;
}

static const struct qmi_ops ipa_server_ops = {
	.bye		= ipa_server_bye,
};

/* Callback function to handle an INDICATION_REGISTER request message from the
 * modem.  This informs the AP that the modem is now ready to receive the
 * INIT_COMPLETE indication message.
 */
static void ipa_server_indication_register(struct qmi_handle *qmi,
					   struct sockaddr_qrtr *sq,
					   struct qmi_txn *txn,
					   const void *decoded)
{
	struct ipa_indication_register_rsp rsp = { };
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);
	ipa = container_of(ipa_qmi, struct ipa, qmi);

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_INDICATION_REGISTER,
				IPA_QMI_INDICATION_REGISTER_RSP_SZ,
				ipa_indication_register_rsp_ei, &rsp);
	if (!ret) {
		ipa_qmi->indication_requested = true;
		ipa_qmi_ready(ipa_qmi);		/* We might be ready now */
	} else {
		dev_err(&ipa->pdev->dev,
			"error %d sending register indication response\n", ret);
	}
}

/* Respond to a DRIVER_INIT_COMPLETE request message from the modem. */
static void ipa_server_driver_init_complete(struct qmi_handle *qmi,
					    struct sockaddr_qrtr *sq,
					    struct qmi_txn *txn,
					    const void *decoded)
{
	struct ipa_driver_init_complete_rsp rsp = { };
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);
	ipa = container_of(ipa_qmi, struct ipa, qmi);

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_DRIVER_INIT_COMPLETE,
				IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ,
				ipa_driver_init_complete_rsp_ei, &rsp);
	if (!ret) {
		ipa_qmi->uc_ready = true;
		ipa_qmi_ready(ipa_qmi);		/* We might be ready now */
	} else {
		dev_err(&ipa->pdev->dev,
			"error %d sending init complete response\n", ret);
	}
}

/*
 * The v2 IPA QMI requests.  At boot the modem asks the AP to install its
 * uplink filter rules in the IPA's filter table, reports that its own rules
 * are installed, and passes its IPA configuration.  Acknowledging these is
 * what lets the modem's data path come up: it waits for the response, times
 * out otherwise, and the data call then fails inside the modem.
 *
 * Installing the rules and applying the configuration is not implemented
 * yet; the requests are only decoded (as far as needed) and acknowledged.
 */
/* One filter rule record, as the modem sends it over QMI, is 172 bytes of
 * packed fields.  Only some fields are decoded; the rest are read through
 * byte accessors (the record arrives as an array of 32-bit words).
 *
 * Record layout (byte offsets):
 *   0   filter_spec_identifier	u32
 *   4   ip_type			u32 (0 = IPv4, 1 = IPv6)
 *   8   rule_eq_bitmap		u16
 *   10  tos_eq_present		u8
 *   11  tos_eq			u8
 *   12  protocol_eq_present	u8
 *   13  protocol_eq		u8
 *   14  num_ihl_offset_range_16	u8
 *   15  ihl_offset_range_16[2]	{ u8 offset; u16 low; u16 high; }
 *   25  num_offset_meq_32	u8
 *   26  offset_meq_32[2]	{ u8 offset; u32 mask; u32 value; }
 *   44  tc_eq_present		u8
 *   45  tc_eq			u8
 *   46  flow_eq_present		u8
 *   47  flow_eq			u32
 *   51  ihl_offset_eq_16_present	u8
 *   52  ihl_offset_eq_16	{ u8 offset; u16 value; }
 *   55  ihl_offset_eq_32_present	u8
 *   56  ihl_offset_eq_32	{ u8 offset; u32 value; }
 *   61  num_ihl_offset_meq_32	u8
 *   62  ihl_offset_meq_32[2]	{ u8 offset; u32 mask; u32 value; }
 *   80  num_offset_meq_128	u8
 *   81  offset_meq_128[2]	{ u8 offset; u8 mask[16]; u8 value[16]; }
 *   147 metadata_meq32_present	u8
 *   148 metadata_meq32		{ u8 offset; u32 mask; u32 value; }
 *   157 ipv4_frag_eq_present	u8
 *   158 filter_action		u32
 *   162 is_routing_table_index_valid u8
 *   163 route_table_index	u32
 *   167 is_mux_id_valid		u8
 *   168 mux_id			u32
 */
#define RULE_IDENTIFIER		0
#define RULE_IP_TYPE		4
#define RULE_EQ_BITMAP		8
#define RULE_TOS_PRESENT	10
#define RULE_TOS		11
#define RULE_PROTO_PRESENT	12
#define RULE_PROTO		13
#define RULE_NUM_RANGE16	14
#define RULE_RANGE16		15
#define RULE_NUM_MEQ32		25
#define RULE_MEQ32		26
#define RULE_TC_PRESENT		44
#define RULE_TC			45
#define RULE_FLOW_PRESENT	46
#define RULE_FLOW		47
#define RULE_EQ16_PRESENT	51
#define RULE_EQ16		52
#define RULE_EQ32_PRESENT	55
#define RULE_EQ32		56
#define RULE_NUM_IHL_MEQ32	61
#define RULE_IHL_MEQ32		62
#define RULE_NUM_MEQ128		80
#define RULE_MEQ128		81
#define RULE_METADATA_PRESENT	147
#define RULE_METADATA		148
#define RULE_FRAG_PRESENT	157
#define RULE_ACTION		158
#define RULE_RT_VALID		162
#define RULE_RT_INDEX		163

/* Sizes of the packed rule fields (in the order they appear) */
#define RULE_RANGE16_SIZE	5
#define RULE_MEQ32_SIZE		9
#define RULE_EQ16_SIZE		3
#define RULE_EQ32_SIZE		5
#define RULE_IHL_MEQ32_SIZE	9
#define RULE_MEQ128_SIZE	33
#define RULE_METADATA_SIZE	9

/* Accessors for the packed record */
static u8 ipa_qmi_rule_byte(const struct ipa_qmi_filter_rule *rule, u32 offset)
{
	return rule->words[offset / sizeof(u32)] >> (8 * (offset % sizeof(u32)));
}

static u16 ipa_qmi_rule_u16(const struct ipa_qmi_filter_rule *rule, u32 offset)
{
	return ipa_qmi_rule_byte(rule, offset) |
	       ipa_qmi_rule_byte(rule, offset + 1) << 8;
}

static u32 ipa_qmi_rule_u32(const struct ipa_qmi_filter_rule *rule, u32 offset)
{
	return ipa_qmi_rule_byte(rule, offset) |
	       ipa_qmi_rule_byte(rule, offset + 1) << 8 |
	       ipa_qmi_rule_byte(rule, offset + 2) << 16 |
	       (u32)ipa_qmi_rule_byte(rule, offset + 3) << 24;
}

/* Writers for the hardware form of a rule.  Multi-byte fields are little
 * endian and every field is padded to a 32-bit boundary, exactly like the
 * vendor's ipa_generate_hw_rule_from_eq().
 */
static u8 *rule_write_8(u8 value, u8 *buf)
{
	*buf = value;
	return buf + 1;
}

static u8 *rule_write_16(u16 value, u8 *buf)
{
	put_unaligned_le16(value, buf);
	return buf + sizeof(u16);
}

static u8 *rule_write_32(u32 value, u8 *buf)
{
	put_unaligned_le32(value, buf);
	return buf + sizeof(u32);
}

static u8 *rule_pad_to_32(u8 *buf)
{
	return PTR_ALIGN(buf, sizeof(u32));
}

/* Append one rule to a filter chain, in the field order the hardware uses */
static u8 *ipa_qmi_rule_add(const struct ipa_qmi_filter_rule *rule, u8 *chain)
{
	u16 bitmap = ipa_qmi_rule_u16(rule, RULE_EQ_BITMAP);
	u32 action = ipa_qmi_rule_u32(rule, RULE_ACTION);
	u32 rt_index = ipa_qmi_rule_u32(rule, RULE_RT_INDEX);
	u32 hdr = IPA_FLT_RULE_HDR(bitmap, action,
				   ipa_qmi_rule_byte(rule, RULE_RT_VALID) ?
					rt_index : 0) |
		  BIT(IPA_FLT_RULE_RETAIN_HDR);
	u8 num = ipa_qmi_rule_byte(rule, RULE_NUM_MEQ32);
	int i;

	chain = rule_write_32(hdr, chain);

	if (ipa_qmi_rule_byte(rule, RULE_TOS_PRESENT))
		chain = rule_pad_to_32(
			rule_write_8(ipa_qmi_rule_byte(rule, RULE_TOS), chain));
	if (ipa_qmi_rule_byte(rule, RULE_PROTO_PRESENT))
		chain = rule_pad_to_32(
			rule_write_8(ipa_qmi_rule_byte(rule, RULE_PROTO), chain));

	for (i = 0; i < num && i < 2; i++) {
		u32 offset = RULE_MEQ32 + i * RULE_MEQ32_SIZE;

		chain = rule_write_8(ipa_qmi_rule_byte(rule, offset), chain);
		chain = rule_write_32(ipa_qmi_rule_u32(rule, offset + 1), chain);
		chain = rule_pad_to_32(
			rule_write_32(ipa_qmi_rule_u32(rule, offset + 5), chain));
	}

	num = ipa_qmi_rule_byte(rule, RULE_NUM_RANGE16);
	for (i = 0; i < num && i < 2; i++) {
		u32 offset = RULE_RANGE16 + i * RULE_RANGE16_SIZE;

		chain = rule_write_8(ipa_qmi_rule_byte(rule, offset), chain);
		chain = rule_write_16(ipa_qmi_rule_u16(rule, offset + 3), chain);
		chain = rule_pad_to_32(
			rule_write_16(ipa_qmi_rule_u16(rule, offset + 1), chain));
	}

	if (ipa_qmi_rule_byte(rule, RULE_EQ16_PRESENT)) {
		chain = rule_write_8(ipa_qmi_rule_byte(rule, RULE_EQ16), chain);
		chain = rule_pad_to_32(
			rule_write_16(ipa_qmi_rule_u16(rule, RULE_EQ16 + 1),
				      chain));
	}

	if (ipa_qmi_rule_byte(rule, RULE_EQ32_PRESENT)) {
		chain = rule_write_8(ipa_qmi_rule_byte(rule, RULE_EQ32), chain);
		chain = rule_pad_to_32(
			rule_write_32(ipa_qmi_rule_u32(rule, RULE_EQ32 + 1),
				      chain));
	}

	/* The first IHL meq32 comes before the 128-bit entries; the second
	 * one follows the traffic class and flow fields (see the vendor's
	 * ipa_generate_hw_rule_from_eq())
	 */
	if (ipa_qmi_rule_byte(rule, RULE_NUM_IHL_MEQ32) > 0) {
		u32 offset = RULE_IHL_MEQ32;

		chain = rule_write_8(ipa_qmi_rule_byte(rule, offset), chain);
		chain = rule_write_32(ipa_qmi_rule_u32(rule, offset + 1), chain);
		chain = rule_pad_to_32(
			rule_write_32(ipa_qmi_rule_u32(rule, offset + 5), chain));
	}

	num = ipa_qmi_rule_byte(rule, RULE_NUM_MEQ128);
	for (i = 0; i < num && i < 2; i++) {
		u32 offset = RULE_MEQ128 + i * RULE_MEQ128_SIZE;
		int j;

		chain = rule_write_8(ipa_qmi_rule_byte(rule, offset), chain);
		for (j = 0; j < 16; j++)
			chain = rule_write_8(
				ipa_qmi_rule_byte(rule, offset + 1 + j), chain);
		for (j = 0; j < 16; j++)
			chain = rule_write_8(
				ipa_qmi_rule_byte(rule, offset + 17 + j), chain);
		chain = rule_pad_to_32(chain);
	}

	if (ipa_qmi_rule_byte(rule, RULE_TC_PRESENT))
		chain = rule_pad_to_32(
			rule_write_8(ipa_qmi_rule_byte(rule, RULE_TC), chain));

	if (ipa_qmi_rule_byte(rule, RULE_FLOW_PRESENT))
		chain = rule_pad_to_32(
			rule_write_32(ipa_qmi_rule_u32(rule, RULE_FLOW), chain));

	/* The second IHL meq32 comes here, after traffic class and flow */
	if (ipa_qmi_rule_byte(rule, RULE_NUM_IHL_MEQ32) > 1) {
		u32 offset = RULE_IHL_MEQ32 + RULE_IHL_MEQ32_SIZE;

		chain = rule_write_8(ipa_qmi_rule_byte(rule, offset), chain);
		chain = rule_write_32(ipa_qmi_rule_u32(rule, offset + 1), chain);
		chain = rule_pad_to_32(
			rule_write_32(ipa_qmi_rule_u32(rule, offset + 5), chain));
	}

	if (ipa_qmi_rule_byte(rule, RULE_METADATA_PRESENT)) {
		chain = rule_write_8(ipa_qmi_rule_byte(rule, RULE_METADATA),
				     chain);
		chain = rule_write_32(ipa_qmi_rule_u32(rule, RULE_METADATA + 1),
				      chain);
		chain = rule_pad_to_32(
			rule_write_32(ipa_qmi_rule_u32(rule, RULE_METADATA + 5),
				      chain));
	}

	if (ipa_qmi_rule_byte(rule, RULE_FRAG_PRESENT))
		chain = rule_pad_to_32(chain);

	return chain;
}

/* DDR layout of the downlink chains in the rule buffer (word offsets).
 * The uplink chains the modem's rules are built into live at bytes 256
 * and 2048; the dump scratch uses words 0..3.
 */
#define IPA_QMI_DL_V4_RT_WORD	16	/* bytes 64..71 */
#define IPA_QMI_DL_V6_RT_WORD	40	/* bytes 160..167 */

static void ipa_server_install_filter_rule(struct qmi_handle *qmi,
					   struct sockaddr_qrtr *sq,
					   struct qmi_txn *txn,
					   const void *decoded)
{
	const struct ipa_qmi_install_fltr_rule_req *req = decoded;
	struct ipa_qmi_install_fltr_rule_rsp rsp = { };
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	u32 count;
	u32 i;
	int ret;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);
	ipa = container_of(ipa_qmi, struct ipa, qmi);

	count = min_t(u32, req->filter_spec_list_len, IPA_QMI_MAX_FILTERS);
	dev_dbg(&ipa->pdev->dev, "modem asked to install %u filter rules\n",
		count);

	/* The modem's rules classify the AP's uplink traffic so that the IPA
	 * routes it to the modem.  Build one filter chain per IP version from
	 * all of the rules, in the order the modem sent them, and point the
	 * uplink endpoint's filter entry at it.
	 */
	if (count) {
		u8 *chain[2] = { NULL, NULL };	/* IPv4, IPv6 */
		u8 *next[2];

		next[0] = (u8 *)ipa->rule_virt + 256;
		next[1] = (u8 *)ipa->rule_virt + 2048;
		chain[0] = next[0];
		chain[1] = next[1];

		for (i = 0; i < count; i++) {
			const struct ipa_qmi_filter_rule *rule =
				&req->filter_spec_list[i];
			u32 ip_type = ipa_qmi_rule_u32(rule, RULE_IP_TYPE);

			if (ip_type > 1)
				continue;	/* unknown IP type */

			next[ip_type] = ipa_qmi_rule_add(rule, next[ip_type]);
		}

		for (i = 0; i < 2; i++) {
			bool ipv6 = i == 1;
			u32 endpoint_id;

			*next[i] = 0;		/* rule list terminator */

			if (next[i] == chain[i])
				continue;	/* no rules of this type */

			endpoint_id =
				ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX]->endpoint_id;
			ret = ipa_table_filter_rule_set(ipa, ipv6, endpoint_id,
						ipa->rule_addr +
						((u8 *)chain[i] -
						 (u8 *)ipa->rule_virt));
			if (ret) {
				dev_err(&ipa->pdev->dev,
					"error %d installing uplink filter rules\n",
					ret);
				continue;
			}

			if (ipv6)
				ipa_qmi->v6_rule_installed = true;
			else
				ipa_qmi->v4_rule_installed = true;
		}
	}

	/* Set up the downlink route: packets the modem routes to the AP
	 * get their QMAP header inserted (from the system header table)
	 * so the AP can demultiplex them by mux id.  The filter entry of
	 * the modem's TX pipe belongs to the modem: the downlink rules
	 * that populate it are sent to the modem over the IPA QMI service
	 * (the vendor's ipacm does the same from userspace).
	 */
	for (i = 0; i < 2; i++) {
		bool ipv6 = i == 1;
		u32 *rt_rule;
		u32 rt_word;

		if (ipv6 ? ipa_qmi->v6_dl_installed : ipa_qmi->v4_dl_installed)
			continue;

		/* Route the packets to the AP; the header table is in system
		 * memory and its first entry is the QMAP template.  The route
		 * chain lives next to the filter chains in the rule buffer,
		 * and the table entry points at it with a system (DDR)
		 * address, the same form the uplink filter entry uses.
		 */
		rt_word = ipv6 ? IPA_QMI_DL_V6_RT_WORD :
				 IPA_QMI_DL_V4_RT_WORD;
		rt_rule = (u32 *)ipa->rule_virt + rt_word;
		rt_rule[0] = IPA_RT_RULE_HDR(0,
				ipa->name_map[IPA_ENDPOINT_AP_LAN_RX]->endpoint_id,
				1, 0);
		rt_rule[1] = 0;			/* rule list terminator */

		ret = ipa_table_route_rule_set(ipa, ipv6,
					       IPA_QMI_AP_ROUTE_TABLE,
					       ipa->rule_addr +
					       rt_word * sizeof(u32));

		if (ret) {
			dev_err(&ipa->pdev->dev,
				"error %d installing downlink route rule\n",
				ret);
			continue;
		}

		if (ipv6)
			ipa_qmi->v6_dl_installed = true;
		else
			ipa_qmi->v4_dl_installed = true;
	}

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;
	rsp.filter_handle_list_valid = 1;
	rsp.filter_handle_list_len = count;

	/* Tell the modem its rules are installed.  The vendor does the same
	 * right after installing them, and the modem's rmnet-meta state
	 * machine waits for this notification before it will bring up a data
	 * call.  The rules are not programmed into the IPA hardware yet, so
	 * the indexes follow the vendor's convention of a per-IP-type counter.
	 */
	memset(&ipa_qmi->fltr_installed_notif, 0,
	       sizeof(ipa_qmi->fltr_installed_notif));
	ipa_qmi->fltr_installed_notif.source_pipe_index =
		ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX]->endpoint_id;
	ipa_qmi->fltr_installed_notif.install_status = 0;	/* success */
	ipa_qmi->fltr_installed_notif.filter_index_list_len = count;
	/* The modem's uplink rule accounting needs the index list and the
	 * counts to agree with the hardware's numbering.  With only the
	 * mandatory fields the modem asserts (fltr_rule_idx <
	 * ipa_ipfltr.fltr.ul.active.num_rules) as soon as it processes an
	 * uplink frame; the fields below were measured to avoid that.
	 */
	ipa_qmi->fltr_installed_notif.embedded_pipe_index_valid = 1;
	ipa_qmi->fltr_installed_notif.embedded_pipe_index =
		ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX]->endpoint_id;
	ipa_qmi->fltr_installed_notif.retain_header_valid = 1;
	ipa_qmi->fltr_installed_notif.retain_header = 1;
	ipa_qmi->fltr_installed_notif.embedded_call_mux_id_valid = 1;
	/* The AP's data call is bound with mux id 1 (netmgrd's first link),
	 * and the uplink frames carry that mux in the QMAP header.  The
	 * notification must name the same mux or the modem's DS cannot map
	 * the source pipe's traffic to the call.
	 */
	ipa_qmi->fltr_installed_notif.embedded_call_mux_id = 1;

	{
		u32 num_v4 = 0;
		u32 num_v6 = 0;

		for (i = 0; i < count; i++) {
			u32 ip_type = req->filter_spec_list[i].words[1];
			u32 identifier = req->filter_spec_list[i].words[0];

			rsp.filter_handle_list[i].filter_spec_identifier =
				identifier;
			rsp.filter_handle_list[i].filter_handle =
				IPA_QMI_UL_RULE_HANDLE_START + i;

			ipa_qmi->fltr_installed_notif.filter_index_list[i]
				.filter_handle = IPA_QMI_UL_RULE_HANDLE_START + i;
			/* The hardware numbers a pipe's rules across both IP
			 * versions (IPv4 chain first), so report the rules in
			 * that same global order.
			 */
			ipa_qmi->fltr_installed_notif
				.filter_index_list[i].filter_index = i;
			if (ip_type == 0)
				num_v4++;
			else
				num_v6++;
		}
		ipa_qmi->fltr_installed_notif.num_ipv4_filters_valid = 1;
		ipa_qmi->fltr_installed_notif.num_ipv4_filters = num_v4;
		ipa_qmi->fltr_installed_notif.num_ipv6_filters_valid = 1;
		ipa_qmi->fltr_installed_notif.num_ipv6_filters = num_v6;
		ipa_qmi->fltr_installed_notif.start_ipv4_filter_idx_valid = 1;
		ipa_qmi->fltr_installed_notif.start_ipv4_filter_idx = 0;
		ipa_qmi->fltr_installed_notif.start_ipv6_filter_idx_valid = 1;
		ipa_qmi->fltr_installed_notif.start_ipv6_filter_idx = num_v4;
	}

	schedule_work(&ipa_qmi->fltr_installed_notif_work);

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_INSTALL_FILTER_RULE,
				IPA_QMI_INSTALL_FILTER_RULE_RSP_SZ,
				ipa_qmi_install_fltr_rule_rsp_ei, &rsp);
	if (ret)
		dev_err(&ipa->pdev->dev,
			"error %d sending filter rule install response\n",
			ret);
}

static void ipa_server_filter_installed_notif(struct qmi_handle *qmi,
					      struct sockaddr_qrtr *sq,
					      struct qmi_txn *txn,
					      const void *decoded)
{
	struct ipa_qmi_status_rsp rsp = { };
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);
	ipa = container_of(ipa_qmi, struct ipa, qmi);

	dev_dbg(&ipa->pdev->dev, "modem reported filter rules installed\n");

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_FILTER_INSTALLED_NOTIF,
				IPA_QMI_FILTER_INSTALLED_NOTIF_RSP_SZ,
				ipa_qmi_status_rsp_ei, &rsp);
	if (ret)
		dev_err(&ipa->pdev->dev,
			"error %d sending filter installed response\n", ret);
}

static void ipa_server_config(struct qmi_handle *qmi,
			      struct sockaddr_qrtr *sq,
			      struct qmi_txn *txn,
			      const void *decoded)
{
	struct ipa_qmi_status_rsp rsp = { };
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);
	ipa = container_of(ipa_qmi, struct ipa, qmi);

	dev_dbg(&ipa->pdev->dev, "modem sent its IPA configuration\n");

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_CONFIG,
				IPA_QMI_CONFIG_RSP_SZ,
				ipa_qmi_status_rsp_ei, &rsp);
	if (ret)
		dev_err(&ipa->pdev->dev,
			"error %d sending config response\n", ret);
}

/* The server handles request message types sent by the modem. */
static const struct qmi_msg_handler ipa_server_msg_handlers[] = {
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_INDICATION_REGISTER,
		.ei		= ipa_indication_register_req_ei,
		.decoded_size	= IPA_QMI_INDICATION_REGISTER_REQ_SZ,
		.fn		= ipa_server_indication_register,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_DRIVER_INIT_COMPLETE,
		.ei		= ipa_driver_init_complete_req_ei,
		.decoded_size	= IPA_QMI_DRIVER_INIT_COMPLETE_REQ_SZ,
		.fn		= ipa_server_driver_init_complete,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_INSTALL_FILTER_RULE,
		.ei		= ipa_qmi_install_fltr_rule_req_ei,
		.decoded_size	= sizeof(struct ipa_qmi_install_fltr_rule_req),
		.fn		= ipa_server_install_filter_rule,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_FILTER_INSTALLED_NOTIF,
		.ei		= ipa_qmi_fltr_installed_notif_req_ei,
		.decoded_size	= sizeof(struct ipa_qmi_fltr_installed_notif_req),
		.fn		= ipa_server_filter_installed_notif,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_CONFIG,
		.ei		= ipa_qmi_skip_req_ei,
		.decoded_size	= 1,
		.fn		= ipa_server_config,
	},
	{ },
};

/* Handle an INIT_DRIVER response message from the modem. */
static void ipa_client_init_driver(struct qmi_handle *qmi,
				   struct sockaddr_qrtr *sq,
				   struct qmi_txn *txn, const void *decoded)
{
	txn->result = 0;	/* IPA_QMI_INIT_DRIVER request was successful */
	complete(&txn->completion);
}

/* Downlink LAN rule installation.
 *
 * On Android the AP never writes the filter entries of the modem's own TX
 * pipes; userspace (ipacm) sends the downlink rules to the modem over the
 * IPA QMI service instead, and the modem installs them on its own pipes.
 * Each rule compares the packet metadata against the call's QMAP mux id and
 * routes matching packets into the AP's WAN route table (which sends them to
 * APPS_WAN_CONS with a QMAP header inserted).
 *
 * The record layout is the IPA v2 IDL ipa_filter_spec_type_v01 (see the
 * vendor's include/uapi/linux/ipa_qmi_service_v01.h).
 */
/* Handle the modem's response to our filter-installed notification. */
static void ipa_client_fltr_installed_notif(struct qmi_handle *qmi,
					    struct sockaddr_qrtr *sq,
					    struct qmi_txn *txn,
					    const void *decoded)
{
	txn->result = 0;	/* FILTER_INSTALLED_NOTIF request successful */
	complete(&txn->completion);
}

/* The client handles one response message type sent by the modem. */
static const struct qmi_msg_handler ipa_client_msg_handlers[] = {
	{
		.type		= QMI_RESPONSE,
		.msg_id		= IPA_QMI_INIT_DRIVER,
		.ei		= ipa_init_modem_driver_rsp_ei,
		.decoded_size	= IPA_QMI_INIT_DRIVER_RSP_SZ,
		.fn		= ipa_client_init_driver,
	},
	{
		.type		= QMI_RESPONSE,
		.msg_id		= IPA_QMI_FILTER_INSTALLED_NOTIF,
		.ei		= ipa_qmi_status_rsp_ei,
		.decoded_size	= IPA_QMI_FILTER_INSTALLED_NOTIF_RSP_SZ,
		.fn		= ipa_client_fltr_installed_notif,
	},
	{ },
};

/* Return a pointer to an init modem driver request structure, which contains
 * configuration parameters for the modem.  The modem may be started multiple
 * times, but generally these parameters don't change so we can reuse the
 * request structure once it's initialized.  The only exception is the
 * is_ssr_bootup field.
 */
static const struct ipa_init_modem_driver_req *
init_modem_driver_req(struct ipa_qmi *ipa_qmi)
{
	struct ipa *ipa = container_of(ipa_qmi, struct ipa, qmi);
	u32 modem_route_count = ipa->modem_route_count;
	static struct ipa_init_modem_driver_req req;
	const struct ipa_mem *mem;

	/* The driver has no SSR tracking: this is always a cold boot, and
	 * the vendor leaves the field unset in that case.
	 */
	req.is_ssr_bootup_valid = 0;
	req.is_ssr_bootup = 0;

	/* We only have to initialize most of it once */
	if (req.platform_type_valid)
		return &req;

	req.platform_type_valid = 1;
	/* The vendor code sends MSM_ANDROID only when the AP has loaded the
	 * IPA microcontroller, and LE otherwise.  This driver does not load
	 * the uC (there is no IPA firmware for it on this device), so
	 * declare the LE platform; with Android the modem expects uC
	 * services that are not there.
	 */
	req.platform_type = IPA_QMI_PLATFORM_TYPE_LE;

	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_HEADER);
	if (mem->size) {
		req.hdr_tbl_info_valid = 1;
		req.hdr_tbl_info.start = ipa->mem_offset + mem->offset;
		req.hdr_tbl_info.end = req.hdr_tbl_info.start + mem->size - 1;
	}

	mem = ipa_mem_find(ipa, IPA_MEM_V4_ROUTE);
	req.v4_route_tbl_info_valid = 1;
	req.v4_route_tbl_info.start = ipa->mem_offset + mem->offset;
	req.v4_route_tbl_info.end = modem_route_count - 1;

	mem = ipa_mem_find(ipa, IPA_MEM_V6_ROUTE);
	req.v6_route_tbl_info_valid = 1;
	req.v6_route_tbl_info.start = ipa->mem_offset + mem->offset;
	req.v6_route_tbl_info.end = modem_route_count - 1;

	mem = ipa_mem_find(ipa, IPA_MEM_V4_FILTER);
	req.v4_filter_tbl_start_valid = 1;
	req.v4_filter_tbl_start = ipa->mem_offset + mem->offset;

	mem = ipa_mem_find(ipa, IPA_MEM_V6_FILTER);
	req.v6_filter_tbl_start_valid = 1;
	req.v6_filter_tbl_start = ipa->mem_offset + mem->offset;

	mem = ipa_mem_find(ipa, IPA_MEM_MODEM);
	if (mem->size) {
		req.modem_mem_info_valid = 1;
		req.modem_mem_info.start = ipa->mem_offset + mem->offset;
		req.modem_mem_info.size = mem->size;
	}

	req.ctrl_comm_dest_end_pt_valid = 1;
	req.ctrl_comm_dest_end_pt =
		ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX]->endpoint_id;

	/* is_ssr_bootup_valid and is_ssr_bootup are set above */

	if (ipa->version != IPA_VERSION_2_6L && ipa->version != IPA_VERSION_2_0) {
		mem = ipa_mem_find(ipa, IPA_MEM_MODEM_PROC_CTX);
		if (mem->size) {
			req.hdr_proc_ctx_tbl_info_valid = 1;
			req.hdr_proc_ctx_tbl_info.start =
				ipa->mem_offset + mem->offset;
			req.hdr_proc_ctx_tbl_info.end =
				req.hdr_proc_ctx_tbl_info.start + mem->size - 1;
		}
	}
	if (ipa->version == IPA_VERSION_2_6L) {
		mem = ipa_mem_find(ipa, IPA_MEM_ZIP);
		if (mem->size) {
			req.zip_tbl_info_valid = 1;
			req.zip_tbl_info.start = ipa->mem_offset + mem->offset;
			req.zip_tbl_info.end = ipa->mem_offset + mem->size - 1;
		}
	}

	return &req;
}

/* Send an INIT_DRIVER request to the modem, and wait for it to complete. */
static void ipa_client_init_driver_work(struct work_struct *work)
{
	unsigned long timeout = msecs_to_jiffies(QMI_INIT_DRIVER_TIMEOUT);
	const struct ipa_init_modem_driver_req *req;
	struct ipa_qmi *ipa_qmi;
	struct qmi_handle *qmi;
	struct qmi_txn txn;
	struct device *dev;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(work, struct ipa_qmi, init_driver_work);
	qmi = &ipa_qmi->client_handle;

	ipa = container_of(ipa_qmi, struct ipa, qmi);
	dev = &ipa->pdev->dev;

	ret = qmi_txn_init(qmi, &txn, NULL, NULL);
	if (ret < 0) {
		dev_err(dev, "error %d preparing init driver request\n", ret);
		return;
	}

	/* Send the request, and if successful wait for its response */
	req = init_modem_driver_req(ipa_qmi);
	ret = qmi_send_request(qmi, &ipa_qmi->modem_sq, &txn,
			       IPA_QMI_INIT_DRIVER, IPA_QMI_INIT_DRIVER_REQ_SZ,
			       ipa_init_modem_driver_req_ei, req);
	if (ret)
		dev_err(dev, "error %d sending init driver request\n", ret);
	else if ((ret = qmi_txn_wait(&txn, timeout)))
		dev_err(dev, "error %d awaiting init driver response\n", ret);

	if (!ret) {
		ipa_qmi->modem_ready = true;
		ipa_qmi_ready(ipa_qmi);		/* We might be ready now */
	} else {
		/* If any error occurs we need to cancel the transaction */
		qmi_txn_cancel(&txn);
	}
}

/* The modem server is now available.  We will send an INIT_DRIVER request
 * to the modem, but can't wait for it to complete in this callback thread.
 * Schedule a worker on the global workqueue to do that for us.
 */
static int
ipa_client_new_server(struct qmi_handle *qmi, struct qmi_service *svc)
{
	struct ipa_qmi *ipa_qmi;

	ipa_qmi = container_of(qmi, struct ipa_qmi, client_handle);

	ipa_qmi->modem_sq.sq_family = AF_QIPCRTR;
	ipa_qmi->modem_sq.sq_node = svc->node;
	ipa_qmi->modem_sq.sq_port = svc->port;

	/*
	 * The modem loads the IPA microcontroller firmware on its first boot
	 * and the microcontroller stays ready across modem restarts (the vendor
	 * documents this explicitly: "The AP may assume the microcontroller is
	 * ready and remain so (even if the modem reboots)").  Sending
	 * INIT_DRIVER again on a later boot makes this modem's IPA firmware
	 * assert (ipa_bam.c:736: IPA Assert:
	 * ipa_bam_hw_pipe_get_sw_ofst_reg(pipe_num) == 0 failed), so it is only
	 * sent for the initial boot.
	 */
	if (!ipa_qmi->initial_boot) {
		ipa_qmi->modem_ready = true;
		ipa_qmi_ready(ipa_qmi);		/* Might not be ready yet */
		return 0;
	}

	schedule_work(&ipa_qmi->init_driver_work);

	return 0;
}

static const struct qmi_ops ipa_client_ops = {
	.new_server	= ipa_client_new_server,
};

/* Send the filter rule install notification to the modem.  The modem's
 * rmnet-meta state machine waits for it before it will bring up a data call.
 * The request must be sent from a work context: its response would otherwise
 * have to be waited for on the QMI receive workqueue.
 */
static void ipa_client_fltr_installed_notif_work(struct work_struct *work)
{
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	struct qmi_handle *qmi;
	struct qmi_txn txn;
	int ret;

	ipa_qmi = container_of(work, struct ipa_qmi,
			       fltr_installed_notif_work);
	ipa = container_of(ipa_qmi, struct ipa, qmi);
	qmi = &ipa_qmi->client_handle;

	ret = qmi_txn_init(qmi, &txn, NULL, NULL);
	if (ret < 0) {
		dev_err(&ipa->pdev->dev,
			"error %d preparing filter installed notif\n", ret);
		return;
	}

	ret = qmi_send_request(qmi, &ipa_qmi->modem_sq, &txn,
			       IPA_QMI_FILTER_INSTALLED_NOTIF,
			       IPA_QMI_FILTER_INSTALLED_NOTIF_REQ_SZ,
			       ipa_qmi_fltr_installed_notif_req_ei,
			       &ipa_qmi->fltr_installed_notif);
	if (ret) {
		dev_err(&ipa->pdev->dev,
			"error %d sending filter installed notif\n", ret);
		qmi_txn_cancel(&txn);
		return;
	}

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(1000));
	if (ret)
		dev_dbg(&ipa->pdev->dev,
			"filter installed notif response error %d\n", ret);
}

/* Set up for QMI message exchange */
int ipa_qmi_setup(struct ipa *ipa)
{
	struct ipa_qmi *ipa_qmi = &ipa->qmi;
	int ret;

	ipa_qmi->initial_boot = true;

	/* The server handle is used to handle the DRIVER_INIT_COMPLETE
	 * request on the first modem boot.  It also receives the
	 * INDICATION_REGISTER request on the first boot and (optionally)
	 * subsequent boots.  The INIT_COMPLETE indication message is
	 * sent over the server handle if requested.
	 */
	ret = qmi_handle_init(&ipa_qmi->server_handle,
			      IPA_QMI_SERVER_MAX_RCV_SZ, &ipa_server_ops,
			      ipa_server_msg_handlers);
	if (ret)
		return ret;

	ret = qmi_add_server(&ipa_qmi->server_handle, IPA_HOST_SERVICE_SVC_ID,
			     IPA_HOST_SVC_VERS, IPA_HOST_SERVICE_INS_ID);
	if (ret)
		goto err_server_handle_release;

	/* The client handle is only used for sending an INIT_DRIVER request
	 * to the modem, and receiving its response message.
	 */
	ret = qmi_handle_init(&ipa_qmi->client_handle,
			      IPA_QMI_CLIENT_MAX_RCV_SZ, &ipa_client_ops,
			      ipa_client_msg_handlers);
	if (ret)
		goto err_server_handle_release;

	/* We need this ready before the service lookup is added */
	INIT_WORK(&ipa_qmi->init_driver_work, ipa_client_init_driver_work);
	INIT_WORK(&ipa_qmi->fltr_installed_notif_work,
		  ipa_client_fltr_installed_notif_work);

	ret = qmi_add_lookup(&ipa_qmi->client_handle, IPA_MODEM_SERVICE_SVC_ID,
			     IPA_MODEM_SVC_VERS, IPA_MODEM_SERVICE_INS_ID);
	if (ret)
		goto err_client_handle_release;

	return 0;

err_client_handle_release:
	/* Releasing the handle also removes registered lookups */
	qmi_handle_release(&ipa_qmi->client_handle);
	memset(&ipa_qmi->client_handle, 0, sizeof(ipa_qmi->client_handle));
err_server_handle_release:
	/* Releasing the handle also removes registered services */
	qmi_handle_release(&ipa_qmi->server_handle);
	memset(&ipa_qmi->server_handle, 0, sizeof(ipa_qmi->server_handle));

	return ret;
}

/* Tear down IPA QMI handles */
void ipa_qmi_teardown(struct ipa *ipa)
{
	cancel_work_sync(&ipa->qmi.init_driver_work);
	cancel_work_sync(&ipa->qmi.fltr_installed_notif_work);

	qmi_handle_release(&ipa->qmi.client_handle);
	memset(&ipa->qmi.client_handle, 0, sizeof(ipa->qmi.client_handle));

	qmi_handle_release(&ipa->qmi.server_handle);
	memset(&ipa->qmi.server_handle, 0, sizeof(ipa->qmi.server_handle));
}

/* With IPA v2 modem is not required to send DRIVER_INIT_COMPLETE request to AP.
 * We start operation as soon as IPA_UC_RESPONSE_INIT_COMPLETED irq is triggered.
 */
void ipa_qmi_signal_uc_loaded(struct ipa *ipa)
{
	struct ipa_qmi *ipa_qmi = &ipa->qmi;

	/* This is needed only on IPA 2.x */
	if (ipa->version > IPA_VERSION_2_6L)
		return;

	ipa_qmi->uc_ready = true;
	ipa_qmi_ready(ipa_qmi);
}
