// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2023 Linaro Ltd.
 */

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/bug.h>
#include <linux/io.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/soc/qcom/mdt_loader.h>

#include "ipa.h"
#include "ipa_dma.h"
#include "ipa_power.h"
#include "ipa_data.h"
#include "ipa_endpoint.h"
#include "ipa_cmd.h"
#include "ipa_reg.h"
#include "ipa_mem.h"
#include "ipa_table.h"
#include "ipa_modem.h"
#include "ipa_uc.h"
#include "ipa_interrupt.h"
#include "ipa_dma_trans.h"
#include "ipa_sysfs.h"

/**
 * DOC: The IP Accelerator
 *
 * This driver supports the Qualcomm IP Accelerator (IPA), which is a
 * networking component found in many Qualcomm SoCs.  The IPA is connected
 * to the application processor (AP), but is also connected (and partially
 * controlled by) other "execution environments" (EEs), such as a modem.
 *
 * The IPA is the conduit between the AP and the modem that carries network
 * traffic.  This driver presents a network interface representing the
 * connection of the modem to external (e.g. LTE) networks.
 *
 * The IPA provides protocol checksum calculation, offloading this work
 * from the AP.  The IPA offers additional functionality, including routing,
 * filtering, and NAT support, but that more advanced functionality is not
 * currently supported.  Despite that, some resources--including routing
 * tables and filter tables--are defined in this driver because they must
 * be initialized even when the advanced hardware features are not used.
 *
 * There are two distinct layers that implement the IPA hardware, and this
 * is reflected in the organization of the driver.  The generic software
 * interface (GSI) is an integral component of the IPA, providing a
 * well-defined communication layer between the AP subsystem and the IPA
 * core.  The GSI implements a set of "channels" used for communication
 * between the AP and the IPA.
 *
 * The IPA layer uses GSI channels to implement its "endpoints".  And while
 * a GSI channel carries data between the AP and the IPA, a pair of IPA
 * endpoints is used to carry traffic between two EEs.  Specifically, the main
 * modem network interface is implemented by two pairs of endpoints:  a TX
 * endpoint on the AP coupled with an RX endpoint on the modem; and another
 * RX endpoint on the AP receiving data from a TX endpoint on the modem.
 */

/* The name of the GSI firmware file relative to /lib/firmware */
#define IPA_FW_PATH_DEFAULT	"ipa_fws.mdt"
#define IPA_PAS_ID		15

/* Shift of 19.2 MHz timestamp to achieve lower resolution timestamps */
#define DPL_TIMESTAMP_SHIFT	14	/* ~1.172 kHz, ~853 usec per tick */
#define TAG_TIMESTAMP_SHIFT	14
#define NAT_TIMESTAMP_SHIFT	24	/* ~1.144 Hz, ~874 msec per tick */

/* Divider for 19.2 MHz crystal oscillator clock to get common timer clock */
#define IPA_XO_CLOCK_DIVIDER	192	/* 1 is subtracted where used */

/**
 * ipa_setup() - Set up IPA hardware
 * @ipa:	IPA pointer
 *
 * Perform initialization that requires issuing immediate commands on
 * the command TX endpoint.  If the modem is doing GSI firmware load
 * and initialization, this function will be called when an SMP2P
 * interrupt has been signaled by the modem.  Otherwise it will be
 * called from ipa_probe() after GSI firmware has been successfully
 * loaded, authenticated, and started by Trust Zone.
 */
int ipa_setup(struct ipa *ipa)
{
	struct ipa_dma *ipa_dma = &ipa->ipa_dma;
	struct ipa_endpoint *exception_endpoint;
	struct ipa_endpoint *command_endpoint;
	struct device *dev = &ipa->pdev->dev;
	int ret;

	ret = ipa_dma->ops->setup(ipa_dma);
	if (ret)
		return ret;

	ret = ipa_power_setup(ipa);
	if (ret)
		goto err_gsi_teardown;

	ipa_endpoint_setup(ipa);

	/* We need to use the AP command TX endpoint to perform other
	 * initialization, so we enable first.
	 */
	command_endpoint = ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX];
	ret = ipa_endpoint_enable_one(command_endpoint);
	if (ret)
		goto err_endpoint_teardown;

	ret = ipa_mem_setup(ipa);	/* No matching teardown required */
	if (ret)
		goto err_command_disable;

	ret = ipa_table_setup(ipa);	/* No matching teardown required */
	if (ret)
		goto err_command_disable;

	/* Enable the exception handling endpoint, and tell the hardware
	 * to use it by default.
	 */
	exception_endpoint = ipa->name_map[IPA_ENDPOINT_AP_LAN_RX];
	ret = ipa_endpoint_enable_one(exception_endpoint);
	if (ret)
		goto err_command_disable;

	ipa_endpoint_default_route_set(ipa, exception_endpoint->endpoint_id);

	/* We're all set.  Now prepare for communication with the modem */
	ret = ipa_qmi_setup(ipa);
	if (ret)
		goto err_default_route_clear;

	ipa->setup_complete = true;

	dev_info(dev, "IPA driver setup completed successfully\n");

	return 0;

err_default_route_clear:
	ipa_endpoint_default_route_clear(ipa);
	ipa_endpoint_disable_one(exception_endpoint);
err_command_disable:
	ipa_endpoint_disable_one(command_endpoint);
err_endpoint_teardown:
	ipa_endpoint_teardown(ipa);
	ipa_power_teardown(ipa);
err_gsi_teardown:
	ipa_dma->ops->teardown(ipa_dma);

	return ret;
}

/**
 * ipa_teardown() - Inverse of ipa_setup()
 * @ipa:	IPA pointer
 */
static void ipa_teardown(struct ipa *ipa)
{
	struct ipa_dma *ipa_dma = &ipa->ipa_dma;
	struct ipa_endpoint *exception_endpoint;
	struct ipa_endpoint *command_endpoint;

	/* We're going to tear everything down, as if setup never completed */
	ipa->setup_complete = false;

	ipa_qmi_teardown(ipa);
	ipa_endpoint_default_route_clear(ipa);
	exception_endpoint = ipa->name_map[IPA_ENDPOINT_AP_LAN_RX];
	ipa_endpoint_disable_one(exception_endpoint);
	command_endpoint = ipa->name_map[IPA_ENDPOINT_AP_COMMAND_TX];
	ipa_endpoint_disable_one(command_endpoint);
	ipa_endpoint_teardown(ipa);
	ipa_power_teardown(ipa);
	ipa_dma->ops->teardown(ipa_dma);
}

static void
ipa_hardware_config_bcr(struct ipa *ipa, const struct ipa_data *data)
{
	const struct reg *reg;
	u32 val;

	/* IPA v2.0 has no backward compatibility register */
	if (ipa->version == IPA_VERSION_2_0)
		return;

	reg = ipa_reg(ipa, IPA_BCR);
	val = data->backward_compat;
	iowrite32(val, ipa->reg_virt + reg_offset(reg));
}

/* Configure bus access behavior for IPA components */
static void ipa_hardware_config_comp(struct ipa *ipa)
{
	const struct reg *reg;
	u32 offset;
	u32 val;

	reg = ipa_reg(ipa, COMP_SW_RESET);
	offset = reg_offset(reg);

	iowrite32(1, ipa->reg_virt + offset);
	iowrite32(0, ipa->reg_virt + offset);

	reg = ipa_reg(ipa, COMP_CFG);
	offset = reg_offset(reg);

	val = ioread32(ipa->reg_virt + offset);
	val |= reg_bit(reg, COMP_CFG_ENABLE);
	iowrite32(val, ipa->reg_virt + offset);
}

/* The internal inactivity timer clock is used for the aggregation timer */
#define TIMER_FREQUENCY	32000		/* 32 KHz inactivity timer clock */

/* Compute the value to use in the COUNTER_CFG register AGGR_GRANULARITY
 * field to represent the given number of microseconds.  The value is one
 * less than the number of timer ticks in the requested period.  0 is not
 * a valid granularity value (so for example @usec must be at least 16 for
 * a TIMER_FREQUENCY of 32000).
 */
static __always_inline u32 ipa_aggr_granularity_val(u32 usec)
{
	return DIV_ROUND_CLOSEST(usec * TIMER_FREQUENCY, USEC_PER_SEC) - 1;
}

/* Before IPA v4.5 timing is controlled by a counter register */
static void ipa_hardware_config_counter(struct ipa *ipa)
{
	u32 granularity = ipa_aggr_granularity_val(IPA_AGGR_GRANULARITY);
	const struct reg *reg;
	u32 val;

	reg = ipa_reg(ipa, COUNTER_CFG);
	/* If defined, EOT_COAL_GRANULARITY is 0 */
	val = reg_encode(reg, AGGR_GRANULARITY, granularity);
	iowrite32(val, ipa->reg_virt + reg_offset(reg));
}

static void ipa_hardware_config_timing(struct ipa *ipa)
{
	ipa_hardware_config_counter(ipa);
}

/**
 * ipa_hardware_dcd_config() - Enable dynamic clock division on IPA
 * @ipa:	IPA pointer
 *
 * Configures when the IPA signals it is idle to the global clock
 * controller, which can respond by scaling down the clock to save
 * power.
 */
// static void ipa_hardware_dcd_config(struct ipa *ipa)
// {
// 	/* Recommended values for IPA 3.5 and later according to IPA HPG */
// 	ipa_idle_indication_cfg(ipa, 256, false);
// }

// static void ipa_hardware_dcd_deconfig(struct ipa *ipa)
// {
// 	/* Power-on reset values */
// 	ipa_idle_indication_cfg(ipa, 0, true);
// }

/**
 * ipa_hardware_config() - Primitive hardware initialization
 * @ipa:	IPA pointer
 * @data:	IPA configuration data
 */
static void ipa_hardware_config(struct ipa *ipa, const struct ipa_data *data)
{
	ipa_hardware_config_bcr(ipa, data);
	// ipa_hardware_config_tx(ipa);
	// ipa_hardware_config_clkon(ipa);
	ipa_hardware_config_comp(ipa);
	ipa_hardware_config_timing(ipa);
	// ipa_hardware_config_hashing(ipa);
	// ipa_hardware_dcd_config(ipa);
}

/**
 * ipa_hardware_deconfig() - Inverse of ipa_hardware_config()
 * @ipa:	IPA pointer
 *
 * This restores the power-on reset values (even if they aren't different)
 */
static void ipa_hardware_deconfig(struct ipa *ipa)
{
	/* Mostly we just leave things as we set them. */
	/* ipa_hardware_dcd_deconfig(ipa); */
	/* NOOP */
}

/**
 * ipa_config() - Configure IPA hardware
 * @ipa:	IPA pointer
 * @data:	IPA configuration data
 *
 * Perform initialization requiring IPA power to be enabled.
 */
static int ipa_config(struct ipa *ipa, const struct ipa_data *data)
{
	int ret;

	ipa_hardware_config(ipa, data);

	ret = ipa_mem_config(ipa);
	if (ret)
		goto err_hardware_deconfig;

	ipa->interrupt = ipa_interrupt_config(ipa);
	if (IS_ERR(ipa->interrupt)) {
		ret = PTR_ERR(ipa->interrupt);
		ipa->interrupt = NULL;
		goto err_mem_deconfig;
	}

	ipa_uc_config(ipa);

	ret = ipa_endpoint_config(ipa);
	if (ret)
		goto err_uc_deconfig;

	ret = ipa_modem_config(ipa);
	if (ret)
		goto err_endpoint_deconfig;

	return 0;

err_endpoint_deconfig:
	ipa_endpoint_deconfig(ipa);
err_uc_deconfig:
	ipa_uc_deconfig(ipa);
	ipa_interrupt_deconfig(ipa->interrupt);
	ipa->interrupt = NULL;
err_mem_deconfig:
	ipa_mem_deconfig(ipa);
err_hardware_deconfig:
	ipa_hardware_deconfig(ipa);

	return ret;
}

/**
 * ipa_deconfig() - Inverse of ipa_config()
 * @ipa:	IPA pointer
 */
static void ipa_deconfig(struct ipa *ipa)
{
	ipa_modem_deconfig(ipa);
	ipa_endpoint_deconfig(ipa);
	ipa_uc_deconfig(ipa);
	ipa_interrupt_deconfig(ipa->interrupt);
	ipa->interrupt = NULL;
	ipa_mem_deconfig(ipa);
	ipa_hardware_deconfig(ipa);
}

static const struct of_device_id ipa_match[] = {
	{
		.compatible	= "qcom,msm8994-ipa",
		.data		= &ipa_data_v2_0,
	},
	{
		.compatible	= "qcom,msm8992-ipa",
		.data		= &ipa_data_v2_0,
	},
	{
		.compatible	= "qcom,msm8953-ipa",
		.data		= &ipa_data_v2_6l,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, ipa_match);

/* Check things that can be validated at build time.  This just
 * groups these things BUILD_BUG_ON() calls don't clutter the rest
 * of the code.
 * */
static void ipa_validate_build(void)
{
	/* At one time we assumed a 64-bit build, allowing some do_div()
	 * calls to be replaced by simple division or modulo operations.
	 * We currently only perform divide and modulo operations on u32,
	 * u16, or size_t objects, and of those only size_t has any chance
	 * of being a 64-bit value.  (It should be guaranteed 32 bits wide
	 * on a 32-bit build, but there is no harm in verifying that.)
	 */
	BUILD_BUG_ON(!IS_ENABLED(CONFIG_64BIT) && sizeof(size_t) != 4);

	/* Code assumes the EE ID for the AP is 0 (zeroed structure field) */
	BUILD_BUG_ON(IPA_EE_AP != 0);

	/* There's no point if we have no channels or event rings */
	BUILD_BUG_ON(!GSI_CHANNEL_COUNT_MAX);
	BUILD_BUG_ON(!IPA_DMA_EVT_RING_COUNT_MAX);

	/* GSI hardware design limits */
	BUILD_BUG_ON(GSI_CHANNEL_COUNT_MAX > 32);
	BUILD_BUG_ON(IPA_DMA_EVT_RING_COUNT_MAX > 31);

	/* The number of TREs in a transaction is limited by the channel's
	 * TLV FIFO size.  A transaction structure uses 8-bit fields
	 * to represents the number of TREs it has allocated and used.
	 */
	BUILD_BUG_ON(GSI_TLV_MAX > U8_MAX);

	/* This is used as a divisor */
	BUILD_BUG_ON(!IPA_AGGR_GRANULARITY);

	/* Aggregation granularity value can't be 0, and must fit */
	BUILD_BUG_ON(!ipa_aggr_granularity_val(IPA_AGGR_GRANULARITY));
}

/**
 * ipa_probe() - IPA platform driver probe function
 * @pdev:	Platform device pointer
 *
 * Return:	0 if successful, or a negative error code (possibly
 *		EPROBE_DEFER)
 *
 * This is the main entry point for the IPA driver.  Initialization proceeds
 * in several stages:
 *   - The "init" stage involves activities that can be initialized without
 *     access to the IPA hardware.
 *   - The "config" stage requires IPA power to be active so IPA registers
 *     can be accessed, but does not require the use of IPA immediate commands.
 *   - The "setup" stage uses IPA immediate commands, and so requires the GSI
 *     layer to be initialized.
 *
 * A Boolean Device Tree "modem-init" property determines whether GSI
 * initialization will be performed by the AP (Trust Zone) or the modem.
 * If the AP does GSI initialization, the setup phase is entered after
 * this has completed successfully.  Otherwise the modem initializes
 * the GSI layer and signals it has finished by sending an SMP2P interrupt
 * to the AP; this triggers the start if IPA setup.
 */
static int ipa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct ipa_data *data;
	struct ipa_power *power;
	struct ipa_dma *ipa_dma;
	struct ipa *ipa;
	int ret;

	ipa_validate_build();

	/* Get configuration data early; needed for power initialization */
	data = of_device_get_match_data(dev);
	if (!data) {
		dev_err(dev, "matched hardware not supported\n");
		return -ENODEV;
	}

	if (!ipa_version_supported(data->version)) {
		dev_err(dev, "unsupported IPA version %u\n", data->version);
		return -EINVAL;
	}

	if (!data->modem_route_count) {
		dev_err(dev, "modem_route_count cannot be zero\n");
		return -EINVAL;
	}

	/* The clock and interconnects might not be ready when we're
	 * probed, so might return -EPROBE_DEFER.
	 */
	power = ipa_power_init(dev, data->power_data);
	if (IS_ERR(power))
		return PTR_ERR(power);

	/* No more EPROBE_DEFER.  Allocate and initialize the IPA structure */
	ipa = kzalloc(sizeof(*ipa), GFP_KERNEL);
	if (!ipa) {
		ret = -ENOMEM;
		goto err_power_exit;
	}

	ipa->pdev = pdev;
	dev_set_drvdata(dev, ipa);
	ipa->power = power;
	ipa->version = data->version;
	ipa->modem_route_count = data->modem_route_count;
	init_completion(&ipa->completion);

	ret = ipa_reg_init(ipa);
	if (ret)
		goto err_kfree_ipa;

	ret = ipa_mem_init(ipa, data->mem_data);
	if (ret)
		goto err_reg_exit;

	ipa->ipa_dma.ops = &bam_ops;
	ipa_dma = &ipa->ipa_dma;
	ret = ipa_dma->ops->init(ipa_dma, pdev, ipa->version, data->endpoint_count,
		       data->endpoint_data);
	if (ret)
		goto err_mem_exit;

	/* Result is a non-zero mask of endpoints that support filtering */
	ret = ipa_endpoint_init(ipa, data->endpoint_count, data->endpoint_data);
	if (ret)
		goto err_gsi_exit;

	ret = ipa_table_init(ipa);
	if (ret)
		goto err_endpoint_exit;

	/* Power needs to be active for config and setup */
	ret = pm_runtime_get_sync(dev);
	if (WARN_ON(ret < 0))
		goto err_power_put;

	ret = ipa_config(ipa, data);
	if (ret)
		goto err_power_put;

	dev_info(dev, "IPA driver initialized");

	/* GSI firmware is loaded; proceed to setup */
	ret = ipa_setup(ipa);
	if (ret)
		goto err_deconfig;

	pm_runtime_mark_last_busy(dev);
	(void)pm_runtime_put_autosuspend(dev);

	return 0;

err_deconfig:
	ipa_deconfig(ipa);
err_power_put:
	pm_runtime_put_noidle(dev);
	ipa_table_exit(ipa);
err_endpoint_exit:
	ipa_endpoint_exit(ipa);
err_gsi_exit:
	ipa_dma->ops->exit(ipa_dma);
err_mem_exit:
	ipa_mem_exit(ipa);
err_reg_exit:
	ipa_reg_exit(ipa);
err_kfree_ipa:
	kfree(ipa);
err_power_exit:
	ipa_power_exit(power);

	return ret;
}

static void ipa_remove(struct platform_device *pdev)
{
	struct ipa *ipa = dev_get_drvdata(&pdev->dev);
	struct ipa_dma *ipa_dma = &ipa->ipa_dma;
	struct ipa_power *power = ipa->power;
	struct device *dev = &pdev->dev;
	int ret;

	ret = pm_runtime_get_sync(dev);
	if (WARN_ON(ret < 0))
		goto out_power_put;

	if (ipa->setup_complete) {
		ret = ipa_modem_stop(ipa);
		/* If starting or stopping is in progress, try once more */
		if (ret == -EBUSY) {
			usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
			ret = ipa_modem_stop(ipa);
		}
		if (ret)
			return;

		ipa_teardown(ipa);
	}

	ipa_deconfig(ipa);
out_power_put:
	pm_runtime_put_noidle(dev);
	ipa_table_exit(ipa);
	ipa_endpoint_exit(ipa);
	ipa_dma->ops->exit(&ipa->ipa_dma);
	ipa_mem_exit(ipa);
	ipa_reg_exit(ipa);
	kfree(ipa);
	ipa_power_exit(power);

	dev_info(dev, "IPA driver removed");
}

static void ipa_shutdown(struct platform_device *pdev)
{
	ipa_remove(pdev);
}

static const struct attribute_group *ipa_attribute_groups[] = {
	&ipa_attribute_group,
	&ipa_feature_attribute_group,
	&ipa_endpoint_id_attribute_group,
	&ipa_modem_attribute_group,
	NULL,
};

static struct platform_driver ipa_driver = {
	.probe		= ipa_probe,
	.remove		= ipa_remove,
	.shutdown	= ipa_shutdown,
	.driver	= {
		.name		= "ipa",
		.pm		= &ipa_pm_ops,
		.of_match_table	= ipa_match,
		.dev_groups	= ipa_attribute_groups,
	},
};

module_platform_driver(ipa_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm IP Accelerator device driver");
