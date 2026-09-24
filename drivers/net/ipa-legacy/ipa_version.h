/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2022 Linaro Ltd.
 */
#ifndef _IPA_VERSION_H_
#define _IPA_VERSION_H_

#include <linux/types.h>

/**
 * enum ipa_version
 * @IPA_VERSION_2_0:	IPA version 2.0
 * @IPA_VERSION_2_5:	IPA version 2.5
 * @IPA_VERSION_2_6L:	IPA version 2.6L
 * @IPA_VERSION_COUNT:	Number of defined IPA versions
 *
 * Defines the version of IPA (and GSI) hardware present on the platform.
 * Please update ipa_version_string() whenever a new version is added.
 */
enum ipa_version {
	IPA_VERSION_2_0,
	IPA_VERSION_2_5,
	IPA_VERSION_2_6L,
	IPA_VERSION_COUNT,			/* Last; not a version */
};

static inline bool ipa_version_supported(enum ipa_version version)
{
	switch (version) {
	case IPA_VERSION_2_0:
	case IPA_VERSION_2_5:
	case IPA_VERSION_2_6L:
		return true;
	default:
		return false;
	}
}

/* Execution environment IDs */
enum ipa_ee_id {
	IPA_EE_AP		= 0x0,
	IPA_EE_MODEM		= 0x1,
	IPA_EE_UC		= 0x2,
	IPA_EE_TZ		= 0x3,
};

#endif /* _IPA_VERSION_H_ */
