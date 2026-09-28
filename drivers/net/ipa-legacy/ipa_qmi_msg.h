/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2022 Linaro Ltd.
 */
#ifndef _IPA_QMI_MSG_H_
#define _IPA_QMI_MSG_H_

/* === Only "ipa_qmi" and "ipa_qmi_msg.c" should include this file === */

#include <linux/types.h>
#include <linux/soc/qcom/qmi.h>

/* Request/response/indication QMI message ids used for IPA.  Receiving
 * end issues a response for requests; indications require no response.
 */
#define IPA_QMI_INDICATION_REGISTER	0x20	/* modem -> AP request */
#define IPA_QMI_INIT_DRIVER		0x21	/* AP -> modem request */
#define IPA_QMI_INIT_COMPLETE		0x22	/* AP -> modem indication */
#define IPA_QMI_DRIVER_INIT_COMPLETE	0x35	/* modem -> AP request */

/*
 * The IPA v2 era service uses more of the QMI interface than the modern
 * driver: at boot the modem asks the AP to install its uplink filter rules
 * and passes its own IPA configuration.
 */
#define IPA_QMI_INSTALL_FILTER_RULE	0x23	/* modem -> AP request */
#define IPA_QMI_FILTER_INSTALLED_NOTIF	0x24	/* modem -> AP request */
#define IPA_QMI_CONFIG			0x27	/* modem -> AP request */

/* The maximum size required for message types.  These sizes include
 * the message data, along with type (1 byte) and length (2 byte)
 * information for each field.  The qmi_send_*() interfaces require
 * the message size to be provided.
 */
#define IPA_QMI_INDICATION_REGISTER_REQ_SZ	20	/* -> server handle */
#define IPA_QMI_INDICATION_REGISTER_RSP_SZ	7	/* <- server handle */
#define IPA_QMI_INIT_DRIVER_REQ_SZ		162	/* client handle -> */
#define IPA_QMI_INIT_DRIVER_RSP_SZ		25	/* client handle <- */
#define IPA_QMI_INIT_COMPLETE_IND_SZ		7	/* <- server handle */
#define IPA_QMI_DRIVER_INIT_COMPLETE_REQ_SZ	4	/* -> server handle */
#define IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ	7	/* <- server handle */
#define IPA_QMI_INSTALL_FILTER_RULE_RSP_SZ	523	/* <- server handle */
#define IPA_QMI_FILTER_INSTALLED_NOTIF_REQ_SZ	574	/* client handle -> */
#define IPA_QMI_FILTER_INSTALLED_NOTIF_RSP_SZ	7	/* <- server handle */
#define IPA_QMI_CONFIG_RSP_SZ			7	/* <- server handle */

/*
 * Maximum size of messages we expect the AP to receive (max of above).
 *
 * The modem's INSTALL_FILTER_RULE request can carry up to 64 rules; the
 * vendor's QMI_IPA_INSTALL_FILTER_RULE_REQ_MAX_MSG_LEN_V01 is 11293 bytes.
 * A smaller receive buffer truncates the request and it is then dropped,
 * which leaves the modem's data path unconfigured.
 */
#define IPA_QMI_SERVER_MAX_RCV_SZ		11293
#define IPA_QMI_CLIENT_MAX_RCV_SZ		25

/* Request message for the IPA_QMI_INDICATION_REGISTER request */
struct ipa_indication_register_req {
	u8 master_driver_init_complete_valid;
	u8 master_driver_init_complete;
	u8 data_usage_quota_reached_valid;
	u8 data_usage_quota_reached;
	u8 ipa_mhi_ready_ind_valid;
	u8 ipa_mhi_ready_ind;
	u8 endpoint_desc_ind_valid;
	u8 endpoint_desc_ind;
	u8 bw_change_ind_valid;
	u8 bw_change_ind;
};

/* The response to a IPA_QMI_INDICATION_REGISTER request consists only of
 * a standard QMI response.
 */
struct ipa_indication_register_rsp {
	struct qmi_response_type_v01 rsp;
};

/* Request message for the IPA_QMI_DRIVER_INIT_COMPLETE request */
struct ipa_driver_init_complete_req {
	u8 status;
};

/* The response to a IPA_QMI_DRIVER_INIT_COMPLETE request consists only
 * of a standard QMI response.
 */
struct ipa_driver_init_complete_rsp {
	struct qmi_response_type_v01 rsp;
};

/* The v2 IPA requests (filter rule install, filter installed notification,
 * IPA configuration) are acknowledged with a standard QMI response only.
 */
struct ipa_qmi_status_rsp {
	struct qmi_response_type_v01 rsp;
};

/*
 * The modem's uplink filter rules.  The driver does not program them into the
 * hardware yet, but it must answer the install request the way the vendor does:
 * with one handle per rule.  To keep that answer possible without porting the
 * (large) filter-rule element structure, each rule record is decoded as an
 * opaque blob whose size is that of the IPA v2 IDL rule record:
 *
 *	filter_spec_identifier	u32	(4 bytes, the first field)
 *	ip_type			u32
 *	filter_rule		struct ipa_filter_rule_type_v01
 *	filter_action		u32
 *	is_routing_table_index_valid	u8 (+ padding)
 *	route_table_index	u32
 *	is_mux_id_valid		u8 (+ padding)
 *	mux_id			u32
 *
 * A 20-rule install request is exactly 20 * 172 + 4 bytes long, which pins the
 * record size down.  Only the identifier (the first four bytes of a record) is
 * needed by the driver.
 */
/* IPA v2.x filter rule hardware header: the equation bitmap occupies bits
 * 0-15, the post-match action bits 16-20 and the route table index bits
 * 21-25 (see struct ipa_flt_rule_hw_hdr in the vendor's ipa_hw_defs.h).
 */
#define IPA_FLT_RULE_HDR(en_rule, action, rt_tbl_idx)			\
	((u32)(en_rule) | ((u32)(action) << 16) | ((u32)(rt_tbl_idx) << 21))

/* Equation bitmap bit for the protocol match (legacy IPA attribute bit):
 * see include/uapi/linux/msm_ipa.h IPA_FLT_PROTOCOL.
 */
#define IPA_FLT_PROTO_EQ			0x0002
/* retain hdr bit in the filter rule header (vendor sets retain_hdr=true) */
#define IPA_FLT_RULE_RETAIN_HDR			26

/* IPA v2.x route rule hardware header: the attribute bitmap occupies bits
 * 0-15, the destination pipe index bits 16-20, the "rule is in system
 * memory" flag bit 21 and the header table offset bits 22-31 (see struct
 * ipa_rt_rule_hw_hdr in the vendor's ipa_hw_defs.h).
 */
#define IPA_RT_RULE_HDR(en_rule, pipe_idx, system, hdr_ofst)		\
	((u32)(en_rule) | ((u32)(pipe_idx) << 16) |			\
	 ((u32)(system) << 21) | ((u32)(hdr_ofst) << 22))

/* The first route table entries belong to the modem; the modem's rules use
 * its data table, and the AP's tables start at index 5.
 */
#define IPA_QMI_MODEM_ROUTE_TABLE_DATA		2
#define IPA_QMI_AP_ROUTE_TABLE			5

#define IPA_QMI_FILTER_RULE_SIZE		172
#define IPA_QMI_FILTER_RULE_WORDS		(IPA_QMI_FILTER_RULE_SIZE / sizeof(u32))
#define IPA_QMI_MAX_FILTERS			64	/* QMI_IPA_MAX_FILTERS_V01 */
#define IPA_QMI_UL_RULE_HANDLE_START		69	/* vendor UL_FILTER_RULE_HANDLE_START */

struct ipa_qmi_filter_rule {
	u32 words[IPA_QMI_FILTER_RULE_WORDS];
};

struct ipa_qmi_install_fltr_rule_req {
	u8 filter_spec_list_valid;
	u32 filter_spec_list_len;	/* u32: the QMI encode/decode layer reads and
					 * writes data-length fields as 32-bit values
					 */
	struct ipa_qmi_filter_rule filter_spec_list[IPA_QMI_MAX_FILTERS];
};

struct ipa_qmi_rule_identifier_to_handle {
	u32 filter_spec_identifier;
	u32 filter_handle;
};

struct ipa_qmi_install_fltr_rule_rsp {
	struct qmi_response_type_v01 rsp;
	u8 filter_handle_list_valid;
	u32 filter_handle_list_len;	/* u32: see filter_spec_list_len */
	struct ipa_qmi_rule_identifier_to_handle
		filter_handle_list[IPA_QMI_MAX_FILTERS];
};

struct ipa_qmi_handle_to_index {
	u32 filter_handle;
	u32 filter_index;
};

struct ipa_qmi_fltr_installed_notif_req {
	u32 source_pipe_index;
	u16 install_status;
	u32 filter_index_list_len;	/* u32: see filter_spec_list_len */
	struct ipa_qmi_handle_to_index filter_index_list[IPA_QMI_MAX_FILTERS];
	u8 embedded_pipe_index_valid;
	u32 embedded_pipe_index;
	u8 retain_header_valid;
	u8 retain_header;
	u8 embedded_call_mux_id_valid;
	u32 embedded_call_mux_id;
	u8 num_ipv4_filters_valid;
	u32 num_ipv4_filters;
	u8 num_ipv6_filters_valid;
	u32 num_ipv6_filters;
	u8 start_ipv4_filter_idx_valid;
	u32 start_ipv4_filter_idx;
	u8 start_ipv6_filter_idx_valid;
	u32 start_ipv6_filter_idx;
};

/* The message for the IPA_QMI_INIT_COMPLETE_IND indication consists
 * only of a standard QMI response.
 */
struct ipa_init_complete_ind {
	struct qmi_response_type_v01 status;
};

/* The AP tells the modem its platform type.  We assume Android. */
enum ipa_platform_type {
	IPA_QMI_PLATFORM_TYPE_INVALID		= 0x0,	/* Invalid */
	IPA_QMI_PLATFORM_TYPE_TN		= 0x1,	/* Data card */
	IPA_QMI_PLATFORM_TYPE_LE		= 0x2,	/* Data router */
	IPA_QMI_PLATFORM_TYPE_MSM_ANDROID	= 0x3,	/* Android MSM */
	IPA_QMI_PLATFORM_TYPE_MSM_WINDOWS	= 0x4,	/* Windows MSM */
	IPA_QMI_PLATFORM_TYPE_MSM_QNX_V01	= 0x5,	/* QNX MSM */
};

/* This defines the start and end offset of a range of memory.  The start
 * value is a byte offset relative to the start of IPA shared memory.  The
 * end value is the last addressable unit *within* the range.  Typically
 * the end value is in units of bytes, however it can also be a maximum
 * array index value.
 */
struct ipa_mem_bounds {
	u32 start;
	u32 end;
};

/* This defines the location and size of an array.  The start value
 * is an offset relative to the start of IPA shared memory.  The
 * size of the array is implied by the number of entries (the entry
 * size is assumed to be known).
 */
struct ipa_mem_array {
	u32 start;
	u32 count;
};

/* This defines the location and size of a range of memory.  The
 * start is an offset relative to the start of IPA shared memory.
 * This differs from the ipa_mem_bounds structure in that the size
 * (in bytes) of the memory region is specified rather than the
 * offset of its last byte.
 */
struct ipa_mem_range {
	u32 start;
	u32 size;
};

/* The message for the IPA_QMI_INIT_DRIVER request contains information
 * from the AP that affects modem initialization.
 */
struct ipa_init_modem_driver_req {
	u8			platform_type_valid;
	u32			platform_type;	/* enum ipa_platform_type */

	/* Modem header table information.  This defines the IPA shared
	 * memory in which the modem may insert header table entries.
	 */
	u8			hdr_tbl_info_valid;
	struct ipa_mem_bounds	hdr_tbl_info;

	/* Routing table information.  These define the location and maximum
	 * *index* (not byte) for the modem portion of non-hashable IPv4 and
	 * IPv6 routing tables.  The start values are byte offsets relative
	 * to the start of IPA shared memory.
	 */
	u8			v4_route_tbl_info_valid;
	struct ipa_mem_bounds	v4_route_tbl_info;
	u8			v6_route_tbl_info_valid;
	struct ipa_mem_bounds	v6_route_tbl_info;

	/* Filter table information.  These define the location of the
	 * non-hashable IPv4 and IPv6 filter tables.  The start values are
	 * byte offsets relative to the start of IPA shared memory.
	 */
	u8			v4_filter_tbl_start_valid;
	u32			v4_filter_tbl_start;
	u8			v6_filter_tbl_start_valid;
	u32			v6_filter_tbl_start;

	/* Modem memory information.  This defines the location and
	 * size of memory available for the modem to use.
	 */
	u8			modem_mem_info_valid;
	struct ipa_mem_range	modem_mem_info;

	/* This defines the destination endpoint on the AP to which
	 * the modem driver can send control commands.  Must be less
	 * than ipa_endpoint_max().
	 */
	u8			ctrl_comm_dest_end_pt_valid;
	u32			ctrl_comm_dest_end_pt;

	/* This defines whether the modem should load the microcontroller
	 * or not.  It is unnecessary to reload it if the modem is being
	 * restarted.
	 *
	 * NOTE: this field is named "is_ssr_bootup" elsewhere.
	 */
	u8			is_ssr_bootup_valid;
	u8			is_ssr_bootup;

	/* Processing context memory information.  This defines the memory in
	 * which the modem may insert header processing context table entries.
	 */
	u8			hdr_proc_ctx_tbl_info_valid;
	struct ipa_mem_bounds	hdr_proc_ctx_tbl_info;

	/* Compression command memory information.  This defines the memory
	 * in which the modem may insert compression/decompression commands.
	 */
	u8			zip_tbl_info_valid;
	struct ipa_mem_bounds	zip_tbl_info;

	/* Routing table information.  These define the location and maximum
	 * *index* (not byte) for the modem portion of hashable IPv4 and IPv6
	 * routing tables (if supported by hardware).  The start values are
	 * byte offsets relative to the start of IPA shared memory.
	 */
	u8			v4_hash_route_tbl_info_valid;
	struct ipa_mem_bounds	v4_hash_route_tbl_info;
	u8			v6_hash_route_tbl_info_valid;
	struct ipa_mem_bounds	v6_hash_route_tbl_info;

	/* Filter table information.  These define the location and size
	 * of hashable IPv4 and IPv6 filter tables (if supported by hardware).
	 * The start values are byte offsets relative to the start of IPA
	 * shared memory.
	 */
	u8			v4_hash_filter_tbl_start_valid;
	u32			v4_hash_filter_tbl_start;
	u8			v6_hash_filter_tbl_start_valid;
	u32			v6_hash_filter_tbl_start;

	/* Statistics information.  These define the locations of the
	 * first and last statistics sub-regions.  (IPA v4.0 and above)
	 */
	u8			hw_stats_quota_base_addr_valid;
	u32			hw_stats_quota_base_addr;
	u8			hw_stats_quota_size_valid;
	u32			hw_stats_quota_size;
	u8			hw_stats_drop_base_addr_valid;
	u32			hw_stats_drop_base_addr;
	u8			hw_stats_drop_size_valid;
	u32			hw_stats_drop_size;
};

/* The response to a IPA_QMI_INIT_DRIVER request begins with a standard
 * QMI response, but contains other information as well.  Currently we
 * simply wait for the INIT_DRIVER transaction to complete and
 * ignore any other data that might be returned.
 */
struct ipa_init_modem_driver_rsp {
	struct qmi_response_type_v01	rsp;

	/* This defines the destination endpoint on the modem to which
	 * the AP driver can send control commands.  Must be less than
	 * ipa_endpoint_max().
	 */
	u8				ctrl_comm_dest_end_pt_valid;
	u32				ctrl_comm_dest_end_pt;

	/* This defines the default endpoint.  The AP driver is not
	 * required to configure the hardware with this value.  Must
	 * be less than ipa_endpoint_max().
	 */
	u8				default_end_pt_valid;
	u32				default_end_pt;

	/* This defines whether a second handshake is required to complete
	 * initialization.
	 */
	u8				modem_driver_init_pending_valid;
	u8				modem_driver_init_pending;
};

/* Message structure definitions defined in "ipa_qmi_msg.c" */
extern const struct qmi_elem_info ipa_indication_register_req_ei[];
extern const struct qmi_elem_info ipa_indication_register_rsp_ei[];
extern const struct qmi_elem_info ipa_driver_init_complete_req_ei[];
extern const struct qmi_elem_info ipa_driver_init_complete_rsp_ei[];
extern const struct qmi_elem_info ipa_qmi_status_rsp_ei[];
extern const struct qmi_elem_info ipa_qmi_skip_req_ei[];
extern const struct qmi_elem_info ipa_qmi_filter_rule_ei[];
extern const struct qmi_elem_info ipa_qmi_install_fltr_rule_req_ei[];
extern const struct qmi_elem_info ipa_qmi_rule_identifier_to_handle_ei[];
extern const struct qmi_elem_info ipa_qmi_install_fltr_rule_rsp_ei[];
extern const struct qmi_elem_info ipa_qmi_handle_to_index_ei[];
extern const struct qmi_elem_info ipa_qmi_fltr_installed_notif_req_ei[];
extern const struct qmi_elem_info ipa_init_complete_ind_ei[];
extern const struct qmi_elem_info ipa_mem_bounds_ei[];
extern const struct qmi_elem_info ipa_mem_array_ei[];
extern const struct qmi_elem_info ipa_mem_range_ei[];
extern const struct qmi_elem_info ipa_init_modem_driver_req_ei[];
extern const struct qmi_elem_info ipa_init_modem_driver_rsp_ei[];

#endif /* !_IPA_QMI_MSG_H_ */
