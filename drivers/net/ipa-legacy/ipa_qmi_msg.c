// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2022 Linaro Ltd.
 */
#include <linux/stddef.h>
#include <linux/soc/qcom/qmi.h>

#include "ipa_qmi_msg.h"

/* QMI message structure definition for struct ipa_indication_register_req */
const struct qmi_elem_info ipa_indication_register_req_ei[] = {
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     master_driver_init_complete_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_indication_register_req,
					   master_driver_init_complete_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     master_driver_init_complete),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_indication_register_req,
					   master_driver_init_complete),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     data_usage_quota_reached_valid),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_indication_register_req,
					   data_usage_quota_reached_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     data_usage_quota_reached),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_indication_register_req,
					   data_usage_quota_reached),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     ipa_mhi_ready_ind_valid),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_indication_register_req,
					   ipa_mhi_ready_ind_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     ipa_mhi_ready_ind),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_indication_register_req,
					   ipa_mhi_ready_ind),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     endpoint_desc_ind_valid),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_indication_register_req,
					   endpoint_desc_ind_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     endpoint_desc_ind),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_indication_register_req,
					   endpoint_desc_ind),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     bw_change_ind_valid),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_indication_register_req,
					   bw_change_ind_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     bw_change_ind),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_indication_register_req,
					   bw_change_ind),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_indication_register_rsp */
const struct qmi_elem_info ipa_indication_register_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_rsp,
				     rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_indication_register_rsp,
					   rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_qmi_status_rsp.  Used for
 * the v2 requests the AP only acknowledges (filter rule install, filter
 * installed notification, IPA config).
 */
const struct qmi_elem_info ipa_qmi_status_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_qmi_status_rsp, rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_qmi_status_rsp, rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/*
 * The modem's v2 IPA requests carry only optional TLVs (0x10 and above),
 * whose contents the driver does not decode yet.  The decoder fails on
 * unknown mandatory TLVs, so an element list that terminates immediately
 * makes it skip every TLV instead of rejecting the message.
 */
const struct qmi_elem_info ipa_qmi_skip_req_ei[] = {
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_qmi_rule_identifier_to_handle */
const struct qmi_elem_info ipa_qmi_rule_identifier_to_handle_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_rule_identifier_to_handle,
				     filter_spec_identifier),
		.offset		= offsetof(struct ipa_qmi_rule_identifier_to_handle,
					   filter_spec_identifier),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_rule_identifier_to_handle,
				     filter_handle),
		.offset		= offsetof(struct ipa_qmi_rule_identifier_to_handle,
					   filter_handle),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* One filter rule record is IPA_QMI_FILTER_RULE_SIZE bytes of opaque data (the
 * first word is the rule identifier).  It is decoded as a static array of
 * 32-bit words: the generic decoder only understands 1/2/4/8-byte elements,
 * so a byte blob of the record size cannot be expressed directly.
 */
const struct qmi_elem_info ipa_qmi_filter_rule_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= IPA_QMI_FILTER_RULE_WORDS,
		.elem_size	= sizeof(u32),
		.array_type	= STATIC_ARRAY,
		.offset		= offsetof(struct ipa_qmi_filter_rule, words),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_qmi_install_fltr_rule_req.
 * The filter specs are decoded as opaque records of IPA_QMI_FILTER_RULE_SIZE
 * bytes so that the driver can read the rule count and each rule identifier
 * without the full filter-rule element structure.
 */
const struct qmi_elem_info ipa_qmi_install_fltr_rule_req_ei[] = {
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_install_fltr_rule_req,
				     filter_spec_list_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_qmi_install_fltr_rule_req,
					   filter_spec_list_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_qmi_install_fltr_rule_req,
					   filter_spec_list_len),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= IPA_QMI_MAX_FILTERS,
		.elem_size	= sizeof(struct ipa_qmi_filter_rule),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_qmi_install_fltr_rule_req,
					   filter_spec_list),
		.ei_array	= ipa_qmi_filter_rule_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_qmi_install_fltr_rule_rsp */
const struct qmi_elem_info ipa_qmi_install_fltr_rule_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_install_fltr_rule_rsp, rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_qmi_install_fltr_rule_rsp,
					   rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_install_fltr_rule_rsp,
				     filter_handle_list_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_qmi_install_fltr_rule_rsp,
					   filter_handle_list_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_qmi_install_fltr_rule_rsp,
					   filter_handle_list_len),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= IPA_QMI_MAX_FILTERS,
		.elem_size	=
			sizeof(struct ipa_qmi_rule_identifier_to_handle),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_qmi_install_fltr_rule_rsp,
					   filter_handle_list),
		.ei_array	= ipa_qmi_rule_identifier_to_handle_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_qmi_handle_to_index */
const struct qmi_elem_info ipa_qmi_handle_to_index_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_qmi_handle_to_index,
					       filter_handle),
		.offset		= offsetof(struct ipa_qmi_handle_to_index,
					   filter_handle),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_qmi_handle_to_index,
					       filter_index),
		.offset		= offsetof(struct ipa_qmi_handle_to_index,
					   filter_index),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_qmi_fltr_installed_notif_req */
const struct qmi_elem_info ipa_qmi_fltr_installed_notif_req_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     source_pipe_index),
		.tlv_type	= 0x01,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   source_pipe_index),
	},
	{
		.data_type	= QMI_UNSIGNED_2_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     install_status),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   install_status),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x03,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   filter_index_list_len),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= IPA_QMI_MAX_FILTERS,
		.elem_size	= sizeof(struct ipa_qmi_handle_to_index),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   filter_index_list),
		.ei_array	= ipa_qmi_handle_to_index_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     embedded_pipe_index_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   embedded_pipe_index_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     embedded_pipe_index),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   embedded_pipe_index),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     retain_header_valid),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   retain_header_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     retain_header),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   retain_header),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     embedded_call_mux_id_valid),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   embedded_call_mux_id_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     embedded_call_mux_id),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   embedded_call_mux_id),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     num_ipv4_filters_valid),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   num_ipv4_filters_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     num_ipv4_filters),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   num_ipv4_filters),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     num_ipv6_filters_valid),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   num_ipv6_filters_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     num_ipv6_filters),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   num_ipv6_filters),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     start_ipv4_filter_idx_valid),
		.tlv_type	= 0x15,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   start_ipv4_filter_idx_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     start_ipv4_filter_idx),
		.tlv_type	= 0x15,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   start_ipv4_filter_idx),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     start_ipv6_filter_idx_valid),
		.tlv_type	= 0x16,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   start_ipv6_filter_idx_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_qmi_fltr_installed_notif_req,
				     start_ipv6_filter_idx),
		.tlv_type	= 0x16,
		.offset		= offsetof(struct ipa_qmi_fltr_installed_notif_req,
					   start_ipv6_filter_idx),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_driver_init_complete_req */
const struct qmi_elem_info ipa_driver_init_complete_req_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_driver_init_complete_req,
				     status),
		.tlv_type	= 0x01,
		.offset		= offsetof(struct ipa_driver_init_complete_req,
					   status),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_driver_init_complete_rsp */
const struct qmi_elem_info ipa_driver_init_complete_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_driver_init_complete_rsp,
				     rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_driver_init_complete_rsp,
					   rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_init_complete_ind */
const struct qmi_elem_info ipa_init_complete_ind_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_complete_ind,
				     status),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_init_complete_ind,
					   status),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_mem_bounds */
const struct qmi_elem_info ipa_mem_bounds_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_bounds, start),
		.offset		= offsetof(struct ipa_mem_bounds, start),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_bounds, end),
		.offset		= offsetof(struct ipa_mem_bounds, end),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_mem_array */
const struct qmi_elem_info ipa_mem_array_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_array, start),
		.offset		= offsetof(struct ipa_mem_array, start),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_array, count),
		.offset		= offsetof(struct ipa_mem_array, count),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_mem_range */
const struct qmi_elem_info ipa_mem_range_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_range, start),
		.offset		= offsetof(struct ipa_mem_range, start),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_range, size),
		.offset		= offsetof(struct ipa_mem_range, size),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_init_modem_driver_req */
const struct qmi_elem_info ipa_init_modem_driver_req_ei[] = {
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     platform_type_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   platform_type_valid),
	},
	{
		.data_type	= QMI_SIGNED_4_BYTE_ENUM,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     platform_type),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   platform_type),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hdr_tbl_info_valid),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hdr_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hdr_tbl_info),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hdr_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_route_tbl_info_valid),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_route_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_route_tbl_info),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_route_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_route_tbl_info_valid),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_route_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_route_tbl_info),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_route_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_filter_tbl_start_valid),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_filter_tbl_start_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_filter_tbl_start),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_filter_tbl_start),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_filter_tbl_start_valid),
		.tlv_type	= 0x15,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_filter_tbl_start_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_filter_tbl_start),
		.tlv_type	= 0x15,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_filter_tbl_start),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     modem_mem_info_valid),
		.tlv_type	= 0x16,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   modem_mem_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     modem_mem_info),
		.tlv_type	= 0x16,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   modem_mem_info),
		.ei_array	= ipa_mem_range_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     ctrl_comm_dest_end_pt_valid),
		.tlv_type	= 0x17,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   ctrl_comm_dest_end_pt_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     ctrl_comm_dest_end_pt),
		.tlv_type	= 0x17,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   ctrl_comm_dest_end_pt),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     is_ssr_bootup_valid),
		.tlv_type	= 0x18,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   is_ssr_bootup_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     is_ssr_bootup),
		.tlv_type	= 0x18,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   is_ssr_bootup),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hdr_proc_ctx_tbl_info_valid),
		.tlv_type	= 0x19,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hdr_proc_ctx_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hdr_proc_ctx_tbl_info),
		.tlv_type	= 0x19,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hdr_proc_ctx_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     zip_tbl_info_valid),
		.tlv_type	= 0x1a,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   zip_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     zip_tbl_info),
		.tlv_type	= 0x1a,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   zip_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_hash_route_tbl_info_valid),
		.tlv_type	= 0x1b,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_hash_route_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_hash_route_tbl_info),
		.tlv_type	= 0x1b,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_hash_route_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_hash_route_tbl_info_valid),
		.tlv_type	= 0x1c,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_hash_route_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_hash_route_tbl_info),
		.tlv_type	= 0x1c,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_hash_route_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_hash_filter_tbl_start_valid),
		.tlv_type	= 0x1d,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_hash_filter_tbl_start_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_hash_filter_tbl_start),
		.tlv_type	= 0x1d,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_hash_filter_tbl_start),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_hash_filter_tbl_start_valid),
		.tlv_type	= 0x1e,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_hash_filter_tbl_start_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_hash_filter_tbl_start),
		.tlv_type	= 0x1e,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_hash_filter_tbl_start),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_quota_base_addr_valid),
		.tlv_type	= 0x1f,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_quota_base_addr_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_quota_base_addr),
		.tlv_type	= 0x1f,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_quota_base_addr),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_quota_size_valid),
		.tlv_type	= 0x20,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_quota_size_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_quota_size),
		.tlv_type	= 0x20,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_quota_size),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_drop_base_addr_valid),
		.tlv_type	= 0x21,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_drop_base_addr_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_drop_base_addr),
		.tlv_type	= 0x21,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_drop_base_addr),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_drop_size_valid),
		.tlv_type	= 0x22,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_drop_size_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_drop_size),
		.tlv_type	= 0x22,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_drop_size),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_init_modem_driver_rsp */
const struct qmi_elem_info ipa_init_modem_driver_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     ctrl_comm_dest_end_pt_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   ctrl_comm_dest_end_pt_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     ctrl_comm_dest_end_pt),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   ctrl_comm_dest_end_pt),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     default_end_pt_valid),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   default_end_pt_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     default_end_pt),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   default_end_pt),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     modem_driver_init_pending_valid),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   modem_driver_init_pending_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     modem_driver_init_pending),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   modem_driver_init_pending),
	},
	{
		.data_type	= QMI_EOTI,
	},
};
