// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm PCIe root complex driver
 *
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 * Copyright 2015 Linaro Limited.
 *
 * Author: Stanimir Varbanov <svarbanov@mm-sol.com>
 */

#include <linux/clk.h>
#include <linux/crc8.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/gpio/consumer.h>
#include <linux/interconnect.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/pci-ecam.h>
#include <linux/pci-pwrctrl.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/phy/pcie.h>
#include <linux/phy/phy.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/pwrseq/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/units.h>

#include "../../pci.h"
#include "../pci-host-common.h"
#include "pcie-designware.h"
#include "pcie-qcom-common.h"

/* PARF registers */
#define PARF_SYS_CTRL				0x00
#define PARF_PM_CTRL				0x20
#define PARF_PCS_DEEMPH				0x34
#define PARF_PCS_SWING				0x38
#define PARF_PHY_CTRL				0x40
#define PARF_PHY_REFCLK				0x4c
#define PARF_CONFIG_BITS			0x50
#define PARF_DBI_BASE_ADDR			0x168
#define PARF_SLV_ADDR_SPACE_SIZE		0x16c
#define PARF_MHI_CLOCK_RESET_CTRL		0x174
#define PARF_AXI_MSTR_WR_ADDR_HALT		0x178
#define PARF_AXI_MSTR_WR_ADDR_HALT_V2		0x1a8
#define PARF_Q2A_FLUSH				0x1ac
#define PARF_LTSSM				0x1b0
#define PARF_INT_ALL_STATUS			0x224
#define PARF_INT_ALL_CLEAR			0x228
#define PARF_INT_ALL_MASK			0x22c
#define PARF_STATUS				0x230
#define PARF_SID_OFFSET				0x234
#define PARF_BDF_TRANSLATE_CFG			0x24c
#define PARF_DBI_BASE_ADDR_V2			0x350
#define PARF_DBI_BASE_ADDR_V2_HI		0x354
#define PARF_SLV_ADDR_SPACE_SIZE_V2		0x358
#define PARF_SLV_ADDR_SPACE_SIZE_V2_HI		0x35c
#define PARF_NO_SNOOP_OVERRIDE			0x3d4
#define PARF_ATU_BASE_ADDR			0x634
#define PARF_ATU_BASE_ADDR_HI			0x638
#define PARF_DEVICE_TYPE			0x1000
#define PARF_BDF_TO_SID_TABLE_N			0x2000
#define PARF_BDF_TO_SID_CFG			0x2c00

/* ELBI registers */
#define ELBI_SYS_CTRL				0x04
#define ELBI_SYS_STTS				0x08

/* DBI registers */
#define AXI_MSTR_RESP_COMP_CTRL0		0x818
#define AXI_MSTR_RESP_COMP_CTRL1		0x81c

/* MHI registers */
#define PARF_DEBUG_CNT_PM_LINKST_IN_L2		0xc04
#define PARF_DEBUG_CNT_PM_LINKST_IN_L1		0xc0c
#define PARF_DEBUG_CNT_PM_LINKST_IN_L0S		0xc10
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1	0xc84
#define PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2	0xc88

/* PARF_SYS_CTRL register fields */
#define MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN	BIT(29)
#define MST_WAKEUP_EN				BIT(13)
#define SLV_WAKEUP_EN				BIT(12)
#define MSTR_ACLK_CGC_DIS			BIT(10)
#define SLV_ACLK_CGC_DIS			BIT(9)
#define CORE_CLK_CGC_DIS			BIT(6)
#define AUX_PWR_DET				BIT(4)
#define L23_CLK_RMV_DIS				BIT(2)
#define L1_CLK_RMV_DIS				BIT(1)

/* PARF_PM_CTRL register fields */
#define REQ_NOT_ENTR_L1				BIT(5)

/* PARF_PCS_DEEMPH register fields */
#define PCS_DEEMPH_TX_DEEMPH_GEN1(x)		FIELD_PREP(GENMASK(21, 16), x)
#define PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(x)	FIELD_PREP(GENMASK(13, 8), x)
#define PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(x)	FIELD_PREP(GENMASK(5, 0), x)

/* PARF_PCS_SWING register fields */
#define PCS_SWING_TX_SWING_FULL(x)		FIELD_PREP(GENMASK(14, 8), x)
#define PCS_SWING_TX_SWING_LOW(x)		FIELD_PREP(GENMASK(6, 0), x)

/* PARF_PHY_CTRL register fields */
#define PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK	GENMASK(20, 16)
#define PHY_CTRL_PHY_TX0_TERM_OFFSET(x)		FIELD_PREP(PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK, x)
#define PHY_TEST_PWR_DOWN			BIT(0)

/* PARF_PHY_REFCLK register fields */
#define PHY_REFCLK_SSP_EN			BIT(16)
#define PHY_REFCLK_USE_PAD			BIT(12)

/* PARF_CONFIG_BITS register fields */
#define PHY_RX0_EQ(x)				FIELD_PREP(GENMASK(26, 24), x)

/* PARF_SLV_ADDR_SPACE_SIZE register value */
#define SLV_ADDR_SPACE_SZ			0x80000000

/* PARF_MHI_CLOCK_RESET_CTRL register fields */
#define AHB_CLK_EN				BIT(0)
#define MSTR_AXI_CLK_EN				BIT(1)
#define BYPASS					BIT(4)

/* PARF_AXI_MSTR_WR_ADDR_HALT register fields */
#define EN					BIT(31)

/* PARF_LTSSM register fields */
#define LTSSM_EN				BIT(8)
#define PARF_LTSSM_STATE_MASK			GENMASK(5, 0)
#define SW_CLEAR_FLUSH_MODE			BIT(10)
#define FLUSH_MODE				BIT(11)

/* PARF_INT_ALL_{STATUS/CLEAR/MASK} register fields */
#define INT_ALL_LINK_DOWN			1
#define PARF_INT_ALL_LINK_DOWN			BIT(INT_ALL_LINK_DOWN)
#define PARF_INT_MSI_DEV_0_7			GENMASK(30, 23)

/* PARF_NO_SNOOP_OVERRIDE register fields */
#define WR_NO_SNOOP_OVERRIDE_EN			BIT(1)
#define RD_NO_SNOOP_OVERRIDE_EN			BIT(3)

/* PARF_DEVICE_TYPE register fields */
#define DEVICE_TYPE_RC				0x4

/* PARF_BDF_TO_SID_CFG fields */
#define BDF_TO_SID_BYPASS			BIT(0)

/* PARF_STATUS fields */
#define FLUSH_COMPLETED				BIT(8)

/* ELBI_SYS_CTRL register fields */
#define ELBI_SYS_CTRL_LT_ENABLE			BIT(0)
#define ELBI_SYS_CTRL_PME_TURNOFF_MSG		BIT(4)

/* ELBI_SYS_STTS register fields */
#define ELBI_SYS_STTS_LTSSM_STATE_MASK		GENMASK(17, 12)

/* AXI_MSTR_RESP_COMP_CTRL0 register fields */
#define CFG_REMOTE_RD_REQ_BRIDGE_SIZE_2K	0x4
#define CFG_REMOTE_RD_REQ_BRIDGE_SIZE_4K	0x5

/* AXI_MSTR_RESP_COMP_CTRL1 register fields */
#define CFG_BRIDGE_SB_INIT			BIT(0)

/* PCI_EXP_SLTCAP register fields */
#define PCIE_CAP_SLOT_POWER_LIMIT_VAL		FIELD_PREP(PCI_EXP_SLTCAP_SPLV, 250)
#define PCIE_CAP_SLOT_POWER_LIMIT_SCALE		FIELD_PREP(PCI_EXP_SLTCAP_SPLS, 1)
#define PCIE_CAP_SLOT_VAL			(PCI_EXP_SLTCAP_ABP | \
						PCI_EXP_SLTCAP_PCP | \
						PCI_EXP_SLTCAP_MRLSP | \
						PCI_EXP_SLTCAP_AIP | \
						PCI_EXP_SLTCAP_PIP | \
						PCI_EXP_SLTCAP_HPS | \
						PCI_EXP_SLTCAP_EIP | \
						PCIE_CAP_SLOT_POWER_LIMIT_VAL | \
						PCIE_CAP_SLOT_POWER_LIMIT_SCALE)

#define PERST_DELAY_US				1000
#define FLUSH_TIMEOUT_US			100

#define QCOM_PCIE_CRC8_POLYNOMIAL		(BIT(2) | BIT(1) | BIT(0))

#define QCOM_PCIE_LINK_SPEED_TO_BW(speed) \
		Mbps_to_icc(PCIE_SPEED2MBS_ENC(pcie_get_link_speed(speed)))

struct qcom_pcie_resources_1_0_0 {
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *core;
	struct regulator *vdda;
};

#define QCOM_PCIE_2_1_0_MAX_RESETS		6
#define QCOM_PCIE_2_1_0_MAX_SUPPLY		3
struct qcom_pcie_resources_2_1_0 {
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control_bulk_data resets[QCOM_PCIE_2_1_0_MAX_RESETS];
	int num_resets;
	struct regulator_bulk_data supplies[QCOM_PCIE_2_1_0_MAX_SUPPLY];
};

#define QCOM_PCIE_2_3_2_MAX_SUPPLY		2
struct qcom_pcie_resources_2_3_2 {
	struct clk_bulk_data *clks;
	int num_clks;
	struct regulator_bulk_data supplies[QCOM_PCIE_2_3_2_MAX_SUPPLY];
};

#define QCOM_PCIE_2_3_3_MAX_RESETS		7
struct qcom_pcie_resources_2_3_3 {
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control_bulk_data rst[QCOM_PCIE_2_3_3_MAX_RESETS];
};

#define QCOM_PCIE_2_4_0_MAX_RESETS		12
struct qcom_pcie_resources_2_4_0 {
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control_bulk_data resets[QCOM_PCIE_2_4_0_MAX_RESETS];
	int num_resets;
};

#define QCOM_PCIE_2_7_0_MAX_SUPPLIES		2
struct qcom_pcie_resources_2_7_0 {
	struct clk_bulk_data *clks;
	int num_clks;
	struct regulator_bulk_data supplies[QCOM_PCIE_2_7_0_MAX_SUPPLIES];
	struct reset_control *rst;
};

struct qcom_pcie_resources_2_9_0 {
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *rst;
};

union qcom_pcie_resources {
	struct qcom_pcie_resources_1_0_0 v1_0_0;
	struct qcom_pcie_resources_2_1_0 v2_1_0;
	struct qcom_pcie_resources_2_3_2 v2_3_2;
	struct qcom_pcie_resources_2_3_3 v2_3_3;
	struct qcom_pcie_resources_2_4_0 v2_4_0;
	struct qcom_pcie_resources_2_7_0 v2_7_0;
	struct qcom_pcie_resources_2_9_0 v2_9_0;
};

struct qcom_pcie;

struct qcom_pcie_ops {
	int (*get_resources)(struct qcom_pcie *pcie);
	/*
	 * Runs before PERST# is asserted. msm8994 uses this to power
	 * vddpe-3v3 (i.e. bring the endpoint up before the RC asserts the
	 * reset it needs released after the endpoint is powered).
	 */
	int (*pre_init)(struct qcom_pcie *pcie);
	int (*init)(struct qcom_pcie *pcie);
	int (*post_init)(struct qcom_pcie *pcie);
	void (*host_post_init)(struct qcom_pcie *pcie);
	void (*deinit)(struct qcom_pcie *pcie);
	void (*ltssm_enable)(struct qcom_pcie *pcie);
	int (*config_sid)(struct qcom_pcie *pcie);
	enum dw_pcie_ltssm (*get_ltssm)(struct qcom_pcie *pcie);
};

 /**
  * struct qcom_pcie_cfg - Per SoC config struct
  * @ops: qcom PCIe ops structure
  * @override_no_snoop: Override NO_SNOOP attribute in TLP to enable cache
  * snooping
  * @firmware_managed: Set if the Root Complex is firmware managed
  */
struct qcom_pcie_cfg {
	const struct qcom_pcie_ops *ops;
	bool override_no_snoop;
	bool firmware_managed;
	bool no_l0s;
};

struct qcom_pcie_perst {
	struct list_head list;
	struct gpio_desc *desc;
};

struct qcom_pcie_port {
	struct list_head list;
	struct phy *phy;
	u32 l1ss_t_power_on;
	struct list_head perst;
};

struct qcom_pcie {
	struct dw_pcie *pci;
	void __iomem *parf;			/* DT parf */
	void __iomem *mhi;
	union qcom_pcie_resources res;
	struct icc_path *icc_mem;
	struct icc_path *icc_cpu;
	const struct qcom_pcie_cfg *cfg;
	struct dentry *debugfs;
	struct list_head ports;
	struct gpio_desc *reset;
	int global_irq;
	bool use_pm_opp;

	/* msm8994: 3.10 pci-msm `wake-gpio` (endpoint wake line) */
	struct gpio_desc *ep_wake;
	/* LAB testR6: QCA6174 power sequencer (pwrseq-qcom-wcn) */
	struct pwrseq_desc *ep_pwrseq;
	/* LAB testR5: endpoint WAKE# IRQ (Juliann/snaccy: "add ep wakeirq") */
	int ep_wake_irq;
	atomic_t ep_wake_events;
	/* msm8994: true while running the second, endpoint-powered bring-up */
	bool second_pass;
	/* msm8994: whether vddpe-3v3 is currently enabled */
	bool rail_on;
	/* LAB testR20: deferred (runtime) re-enumeration, cnss-style */
	struct delayed_work reenum_work;
};

#define to_qcom_pcie(x)		dev_get_drvdata((x)->dev)
static int qcom_pcie_reset_root_port(struct pci_host_bridge *bridge,
				  struct pci_dev *pdev);

static void __qcom_pcie_perst_assert(struct qcom_pcie *pcie, bool assert)
{
	struct qcom_pcie_perst *perst;
	struct qcom_pcie_port *port;
	int val = assert ? 1 : 0;

	list_for_each_entry(port, &pcie->ports, list) {
		list_for_each_entry(perst, &port->perst, list)
			gpiod_set_value_cansleep(perst->desc, val);
	}

	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);
}

static void qcom_pcie_perst_assert(struct qcom_pcie *pcie)
{
	__qcom_pcie_perst_assert(pcie, true);
}

static irqreturn_t qcom_pcie_ep_wake_irq(int irq, void *data)
{
	struct qcom_pcie *pcie = data;
	int n = atomic_inc_return(&pcie->ep_wake_events);

	dev_info(pcie->pci->dev, "LAB: EP WAKE# asserted (#%d, line=%d)\n",
		 n, gpiod_get_value_cansleep(pcie->ep_wake));

	return IRQ_HANDLED;
}

static void qcom_pcie_perst_deassert(struct qcom_pcie *pcie)
{
	/* Ensure that PERST# has been asserted for at least 100 ms */
	msleep(PCIE_T_PVPERL_MS);
	__qcom_pcie_perst_assert(pcie, false);
}

static int qcom_pcie_start_link(struct dw_pcie *pci)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pci);

	qcom_pcie_common_set_equalization(pci);

	if (pcie_get_link_speed(pci->max_link_speed) == PCIE_SPEED_16_0GT)
		qcom_pcie_common_set_16gt_lane_margining(pci);

	/* Enable Link Training state machine */
	if (pcie->cfg->ops->ltssm_enable)
		pcie->cfg->ops->ltssm_enable(pcie);

	return 0;
}

static void qcom_pcie_clear_aspm_l0s(struct dw_pcie *pci)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pci);
	u16 offset;
	u32 val;

	if (!pcie->cfg->no_l0s)
		return;

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);

	dw_pcie_dbi_ro_wr_en(pci);

	val = readl(pci->dbi_base + offset + PCI_EXP_LNKCAP);
	val &= ~PCI_EXP_LNKCAP_ASPM_L0S;
	writel(val, pci->dbi_base + offset + PCI_EXP_LNKCAP);

	dw_pcie_dbi_ro_wr_dis(pci);
}

static void qcom_pcie_set_slot_cap(struct dw_pcie *pci)
{
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	u32 val;

	dw_pcie_dbi_ro_wr_en(pci);

	/*
	 * Qcom PCIe Root Ports do not support generating command completion
	 * notifications for the Hot-Plug commands. So set the NCCS field to
	 * avoid waiting for the completions.
	 */
	val = readl(pci->dbi_base + offset + PCI_EXP_SLTCAP);
	val |= PCI_EXP_SLTCAP_NCCS;

	/*
	 * Qcom PCIe Root Ports do not support Attention Button, so clear
	 * Attention Button Present in Slot Capabilities.
	 */
	val &= ~PCI_EXP_SLTCAP_ABP;
	writel(val, pci->dbi_base + offset + PCI_EXP_SLTCAP);

	dw_pcie_dbi_ro_wr_dis(pci);
}

static void qcom_pcie_configure_dbi_base(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;

	if (pci->dbi_phys_addr) {
		/*
		 * PARF_DBI_BASE_ADDR register is in CPU domain and require to
		 * be programmed with CPU physical address.
		 */
		writel(lower_32_bits(pci->dbi_phys_addr), pcie->parf +
							PARF_DBI_BASE_ADDR);
		writel(SLV_ADDR_SPACE_SZ, pcie->parf +
						PARF_SLV_ADDR_SPACE_SIZE);
	}
}

static void qcom_pcie_configure_dbi_atu_base(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;

	if (pci->dbi_phys_addr) {
		/*
		 * PARF_DBI_BASE_ADDR_V2 and PARF_ATU_BASE_ADDR registers are
		 * in CPU domain and require to be programmed with CPU
		 * physical addresses.
		 */
		writel(lower_32_bits(pci->dbi_phys_addr), pcie->parf +
							PARF_DBI_BASE_ADDR_V2);
		writel(upper_32_bits(pci->dbi_phys_addr), pcie->parf +
						PARF_DBI_BASE_ADDR_V2_HI);

		if (pci->atu_phys_addr) {
			writel(lower_32_bits(pci->atu_phys_addr), pcie->parf +
							PARF_ATU_BASE_ADDR);
			writel(upper_32_bits(pci->atu_phys_addr), pcie->parf +
							PARF_ATU_BASE_ADDR_HI);
		}

		writel(0x0, pcie->parf + PARF_SLV_ADDR_SPACE_SIZE_V2);
		writel(SLV_ADDR_SPACE_SZ, pcie->parf +
					PARF_SLV_ADDR_SPACE_SIZE_V2_HI);
	}
}

static void qcom_pcie_2_1_0_ltssm_enable(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	u32 val;

	if (!pci->elbi_base) {
		dev_err(pci->dev, "ELBI is not present\n");
		return;
	}
	/* enable link training */
	val = readl(pci->elbi_base + ELBI_SYS_CTRL);
	val |= ELBI_SYS_CTRL_LT_ENABLE;
	writel(val, pci->elbi_base + ELBI_SYS_CTRL);
}

static enum dw_pcie_ltssm qcom_pcie_2_1_0_get_ltssm(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	u32 val;

	val = readl(pci->elbi_base + ELBI_SYS_STTS);
	return (enum dw_pcie_ltssm)FIELD_GET(ELBI_SYS_STTS_LTSSM_STATE_MASK, val);
}

static int qcom_pcie_get_resources_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	bool is_apq = of_device_is_compatible(dev->of_node, "qcom,pcie-apq8064");
	int ret;

	res->supplies[0].supply = "vdda";
	res->supplies[1].supply = "vdda_phy";
	res->supplies[2].supply = "vdda_refclk";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies);
	if (ret)
		return ret;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks);
	if (res->num_clks < 0) {
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->resets[0].id = "pci";
	res->resets[1].id = "axi";
	res->resets[2].id = "ahb";
	res->resets[3].id = "por";
	res->resets[4].id = "phy";
	res->resets[5].id = "ext";

	/* ext is optional on APQ8016 */
	res->num_resets = is_apq ? 5 : 6;
	ret = devm_reset_control_bulk_get_exclusive(dev, res->num_resets, res->resets);
	if (ret < 0)
		return ret;

	return 0;
}

static void qcom_pcie_deinit_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0;

	clk_bulk_disable_unprepare(res->num_clks, res->clks);
	reset_control_bulk_assert(res->num_resets, res->resets);

	writel(1, pcie->parf + PARF_PHY_CTRL);

	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies);
}

static int qcom_pcie_init_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	int ret;

	/* reset the PCIe interface as uboot can leave it undefined state */
	ret = reset_control_bulk_assert(res->num_resets, res->resets);
	if (ret < 0) {
		dev_err(dev, "cannot assert resets\n");
		return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies), res->supplies);
	if (ret < 0) {
		dev_err(dev, "cannot enable regulators\n");
		return ret;
	}

	ret = reset_control_bulk_deassert(res->num_resets, res->resets);
	if (ret < 0) {
		dev_err(dev, "cannot deassert resets\n");
		regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies);
		return ret;
	}

	return 0;
}

static int qcom_pcie_post_init_2_1_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_1_0 *res = &pcie->res.v2_1_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	struct device_node *node = dev->of_node;
	u32 val;
	int ret;

	/* Force PHY out of lowest power state */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val &= ~PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks);
	if (ret)
		return ret;

	if (of_device_is_compatible(node, "qcom,pcie-ipq8064") ||
	    of_device_is_compatible(node, "qcom,pcie-ipq8064-v2")) {
		writel(PCS_DEEMPH_TX_DEEMPH_GEN1(24) |
			       PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(24) |
			       PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(34),
		       pcie->parf + PARF_PCS_DEEMPH);
		writel(PCS_SWING_TX_SWING_FULL(120) |
			       PCS_SWING_TX_SWING_LOW(120),
		       pcie->parf + PARF_PCS_SWING);
		writel(PHY_RX0_EQ(4), pcie->parf + PARF_CONFIG_BITS);
	}

	if (of_device_is_compatible(node, "qcom,pcie-ipq8064")) {
		/* set TX termination offset */
		val = readl(pcie->parf + PARF_PHY_CTRL);
		val &= ~PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK;
		val |= PHY_CTRL_PHY_TX0_TERM_OFFSET(7);
		writel(val, pcie->parf + PARF_PHY_CTRL);
	}

	/* enable external reference clock */
	val = readl(pcie->parf + PARF_PHY_REFCLK);
	/* USE_PAD is required only for ipq806x */
	if (!of_device_is_compatible(node, "qcom,pcie-apq8064"))
		val &= ~PHY_REFCLK_USE_PAD;
	val |= PHY_REFCLK_SSP_EN;
	writel(val, pcie->parf + PARF_PHY_REFCLK);

	/* wait for clock acquisition */
	usleep_range(1000, 1500);

	/* Set the Max TLP size to 2K, instead of using default of 4K */
	writel(CFG_REMOTE_RD_REQ_BRIDGE_SIZE_2K,
	       pci->dbi_base + AXI_MSTR_RESP_COMP_CTRL0);
	writel(CFG_BRIDGE_SB_INIT,
	       pci->dbi_base + AXI_MSTR_RESP_COMP_CTRL1);

	qcom_pcie_set_slot_cap(pcie->pci);

	return 0;
}

static int qcom_pcie_get_resources_1_0_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_1_0_0 *res = &pcie->res.v1_0_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;

	res->vdda = devm_regulator_get(dev, "vdda");
	if (IS_ERR(res->vdda))
		return PTR_ERR(res->vdda);

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks);
	if (res->num_clks < 0) {
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->core = devm_reset_control_get_exclusive(dev, "core");
	return PTR_ERR_OR_ZERO(res->core);
}

static void qcom_pcie_deinit_1_0_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_1_0_0 *res = &pcie->res.v1_0_0;

	reset_control_assert(res->core);
	clk_bulk_disable_unprepare(res->num_clks, res->clks);
	regulator_disable(res->vdda);
}

static int qcom_pcie_init_1_0_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_1_0_0 *res = &pcie->res.v1_0_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	int ret;

	ret = reset_control_deassert(res->core);
	if (ret) {
		dev_err(dev, "cannot deassert core reset\n");
		return ret;
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks);
	if (ret) {
		dev_err(dev, "cannot prepare/enable clocks\n");
		goto err_assert_reset;
	}

	ret = regulator_enable(res->vdda);
	if (ret) {
		dev_err(dev, "cannot enable vdda regulator\n");
		goto err_disable_clks;
	}

	return 0;

err_disable_clks:
	clk_bulk_disable_unprepare(res->num_clks, res->clks);
err_assert_reset:
	reset_control_assert(res->core);

	return ret;
}

static int qcom_pcie_post_init_1_0_0(struct qcom_pcie *pcie)
{
	qcom_pcie_configure_dbi_base(pcie);

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		u32 val = readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT);

		val |= EN;
		writel(val, pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT);
	}

	qcom_pcie_set_slot_cap(pcie->pci);

	return 0;
}

static void qcom_pcie_2_3_2_ltssm_enable(struct qcom_pcie *pcie)
{
	u32 val;

	/* enable link training */
	val = readl(pcie->parf + PARF_LTSSM);
	val |= LTSSM_EN;
	writel(val, pcie->parf + PARF_LTSSM);
}

static int qcom_pcie_get_resources_2_3_2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	int ret;

	res->supplies[0].supply = "vdda";
	res->supplies[1].supply = "vddpe-3v3";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies);
	if (ret)
		return ret;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks);
	if (res->num_clks < 0) {
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	return 0;
}

static void qcom_pcie_deinit_2_3_2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2;
	u32 val;

	/* Force PHY to lowest power state*/
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val |= PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	clk_bulk_disable_unprepare(res->num_clks, res->clks);
	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies);
}

static int qcom_pcie_init_2_3_2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies), res->supplies);
	if (ret < 0) {
		dev_err(dev, "cannot enable regulators\n");
		return ret;
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks);
	if (ret) {
		dev_err(dev, "cannot prepare/enable clocks\n");
		regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies);
		return ret;
	}

	return 0;
}

static int qcom_pcie_post_init_2_3_2(struct qcom_pcie *pcie)
{
	u32 val;

	/* Force PHY out of lowest power state */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val &= ~PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	qcom_pcie_configure_dbi_base(pcie);

	/* MAC PHY_POWERDOWN MUX DISABLE  */
	val = readl(pcie->parf + PARF_SYS_CTRL);
	val &= ~MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN;
	writel(val, pcie->parf + PARF_SYS_CTRL);

	val = readl(pcie->parf + PARF_MHI_CLOCK_RESET_CTRL);
	val |= BYPASS;
	writel(val, pcie->parf + PARF_MHI_CLOCK_RESET_CTRL);

	val = readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2);
	val |= EN;
	writel(val, pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2);

	qcom_pcie_set_slot_cap(pcie->pci);

	return 0;
}

/*
 * msm8994: keep the endpoint-rail refcount and the actual regulator state in
 * sync by hand, because pre_init/deinit are not paired the way
 * qcom_pcie_init_2_3_2()/qcom_pcie_deinit_2_3_2() assume (pre_init is not run
 * on the suspend/resume path's deinit).
 */
static int qcom_pcie_msm8994_rail(struct qcom_pcie *pcie, bool on)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2;
	int ret = 0;

	if (on == pcie->rail_on)
		return 0;

	if (on)
		ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies),
					    res->supplies);
	else
		regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies);
	if (ret)
		return ret;

	pcie->rail_on = on;

	return 0;
}

/*
 * MSM8994: the endpoint is the QCA6174 on pcie1, powered by vddpe-3v3 (WLAN_EN).
 *
 * The endpoint needs a PERST# cycle while it is *unpowered*, then its power-up,
 * then a second PERST# cycle - only then does it train a link. That was measured
 * the hard way: powering the rail up front and running a single bring-up (the
 * "obvious simplification") leaves the link untrained entirely ("Device not
 * found", testOY), which is worse than the two-pass, and it is also why the
 * vendor runs two enables (its first fails because nothing has powered the
 * endpoint yet; cnss powers it and the second succeeds).
 *
 * So the first pass is deliberately left unpowered and simply fails, and
 * ops->host_post_init redoes the bring-up with the rail on.
 */
static int qcom_pcie_pre_init_msm8994(struct qcom_pcie *pcie)
{
	/* vddpe-3v3 is left off for the first pass, see ops->host_post_init */
	return 0;
}

static int qcom_pcie_get_resources_msm8994(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2;
	struct device *dev = pcie->pci->dev;
	int ret;

	res->supplies[0].supply = "vdda";
	res->supplies[1].supply = "vddpe-3v3";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies);
	if (ret)
		return ret;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks);
	if (res->num_clks < 0) {
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	/* 3.10 pci-msm `wake-gpio` (endpoint wake line) */
	pcie->ep_wake = devm_gpiod_get_optional(dev, "wake", GPIOD_IN);
	if (IS_ERR(pcie->ep_wake)) {
		dev_err(dev, "Failed to get the endpoint wake gpio\n");
		return PTR_ERR(pcie->ep_wake);
	}

	/*
	 * LAB testR5: vendor monitor_mode registers a falling-edge IRQ on the
	 * endpoint WAKE# line (tlmm37) and only enumerates "upon WAKE signal
	 * from Endpoint". Mainline got the gpio but never the IRQ. Request it
	 * (both edges) and log every assertion so we can see if the QCA6174
	 * ever raises WAKE# on this board.
	 */
	if (pcie->ep_wake) {
		pcie->ep_wake_irq = gpiod_to_irq(pcie->ep_wake);
		if (pcie->ep_wake_irq > 0) {
			int ret2 = devm_request_irq(dev, pcie->ep_wake_irq,
					qcom_pcie_ep_wake_irq,
					IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
					"qcom-pcie-wake", pcie);
			if (ret2)
				dev_warn(dev, "LAB: could not request EP wake irq %d: %d\n",
					 pcie->ep_wake_irq, ret2);
			else
				dev_info(dev, "LAB: EP wake irq %d registered (WAKE# tlmm37)\n",
					 pcie->ep_wake_irq);
		} else {
			dev_info(dev, "LAB: EP wake gpio has no irq (%d)\n",
				 pcie->ep_wake_irq);
		}
	}

	/* LAB testR6: endpoint power sequencer (pwrseq-qcom-wcn / qca6174) */
	pcie->ep_pwrseq = devm_pwrseq_get(dev, "wlan");
	if (IS_ERR(pcie->ep_pwrseq)) {
		dev_info(dev, "LAB: no pwrseq (wlan): %ld\n",
			 PTR_ERR(pcie->ep_pwrseq));
		pcie->ep_pwrseq = NULL;
	}

	return 0;
}

static void qcom_pcie_deinit_msm8994(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2;
	u32 val;

	/* Force PHY to lowest power state */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val |= PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	clk_bulk_disable_unprepare(res->num_clks, res->clks);

	/* 2_3_2 would disable the rail unconditionally; ours is conditional */
	qcom_pcie_msm8994_rail(pcie, false);
}

static int qcom_pcie_init_msm8994(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_2 *res = &pcie->res.v2_3_2;
	struct device *dev = pcie->pci->dev;
	u32 scm_dev_id, val;
	int i, ret;

	/*
	 * The second pass powers the endpoint here, i.e. while PERST# is still
	 * asserted and before it is released. The first pass leaves the rail off
	 * on purpose: that unpowered PERST# cycle is what the endpoint needs
	 * before it will train (see qcom_pcie_pre_init_msm8994()).
	 *
	 * 3.10 sets aux to 1.011 MHz before clk_prepare_enable.
	 */
	if (pcie->second_pass) {
		/*
		 * QCA6174A power-up order (per the vendor device documentation):
		 *   (1) VDDIO_AO/XTAL, (2) VDDIO_GPIO0/1/2,
		 *   (3) all 3.3 V rails LAST.
		 * Our DT has the VDDIO rails (PM8994 s4/l30, 1.8V) always-on, so
		 * they are already up before wlan_vreg (3.3V, PM8994 gpio9) ->
		 * order is VDDIO-first, 3.3V-last as required. Make it explicit:
		 * wait for the 1.8V I/O rails to settle before raising 3.3V.
		 */
		usleep_range(2000, 2500);	/* VDDIO (1.8V) settled */

		ret = qcom_pcie_msm8994_rail(pcie, true);
		if (ret) {
			dev_err(dev, "cannot enable the endpoint rail\n");
			return ret;
		}
		dev_info(dev, "LAB: §3.3 VDDIO(1.8V) up, then 3.3V rail up\n");

		/* LAB testR6: explicit QCA6174 power sequence (Val's suggestion) */
		if (pcie->ep_pwrseq) {
			ret = pwrseq_enable(pcie->ep_pwrseq);
			dev_info(dev, "LAB: pwrseq_enable(wlan) = %d\n", ret);
			if (ret)
				return ret;
		}

		/*
		 * LAB testR10: §3.4 Table 3-3 minimums:
		 *   Tpwlen  >= 10 us   (power valid -> WLAN_EN active)
		 *   Tprst   >= 10 ms   (power valid -> PCIE_RST_L asserted)
		 * PERST# is already asserted here (before this point), and the
		 * pre-PERST wait elsewhere is >>10 ms, so Tprst/Tpwlen are met.
		 * We are about to release PERST#; ensure refclk has been stable
		 * >= Tclkrst (100 us) by waiting, then let the caller deassert.
		 */
		usleep_range(2000, 2500);	/* VDDIO (1.8V) settled */
		usleep_range(2000, 2500);	/* VDDIO (1.8V) settled */
	}

	for (i = 0; i < res->num_clks; i++) {
		if (res->clks[i].id && !strcmp(res->clks[i].id, "aux")) {
			ret = clk_set_rate(res->clks[i].clk, 1011000);
			if (ret) {
				dev_err(dev, "cannot set aux rate\n");
				goto err_disable_regulators;
			}
			break;
		}
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks);
	if (ret) {
		dev_err(dev, "cannot prepare/enable clocks\n");
		goto err_disable_regulators;
	}

	/* 3.10 pci-msm.c restores TZ sec cfg (scm-dev-id) before PARF. */
	if (!of_property_read_u32(dev->of_node, "qcom,scm-dev-id",
				  &scm_dev_id) &&
	    qcom_scm_restore_sec_cfg_available()) {
		ret = qcom_scm_restore_sec_cfg(scm_dev_id, 0);
		if (ret) {
			dev_err_probe(dev, ret, "restore sec cfg %u\n",
				      scm_dev_id);
			goto err_disable_clks;
		}
	}

	/* 3.10 PARF: PHY_CTRL bit0 clear, DBI base 0, SYS_CTRL 0x365E. */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val &= ~PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	writel(0, pcie->parf + PARF_DBI_BASE_ADDR);
	writel(0x365e, pcie->parf + PARF_SYS_CTRL);

	return 0;

err_disable_clks:
	clk_bulk_disable_unprepare(res->num_clks, res->clks);
err_disable_regulators:
	qcom_pcie_msm8994_rail(pcie, false);

	return ret;
}

static int qcom_pcie_post_init_msm8994(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	u32 val;

	/*
	 * LAB: the vendor's endpoint is READY the instant PERST# is released
	 * (PCIE_CRS t=0ms vid=0x003e168c), while ours answers CRS ("not ready")
	 * indefinitely. The vendor raises WLAN_EN ~30 ms before
	 * msm_pcie_enable() even starts, so its chip has tens of ms with the
	 * rail up before PERST# is released; ours has only the ep-latency below.
	 * Give the chip time to finish its internal power-up while PERST# is
	 * still asserted (3.10 qcom,ep-latency is 10 ms; try 200 ms).
	 */
	usleep_range(200000, 205000);

	/*
	 * LAB: the vendor's pcie_phy_init() programs TX amplitude / de-emphasis /
	 * RX equalisation / TX termination offset via PCIe20 PHY registers that
	 * are named PCIE20_PARF_* and written at dev->phy == the PARF base:
	 *   0x3c PHY_STTS, 0x44 PHY_RESET_CTRL, 0x74/0x78/0x7c PCS_DEEMPH1/2/3,
	 *   0x80 PCS_CTRL, 0x84 CONFIGBITS, 0x88/0x8c PCS_SWING_CTRL1/2,
	 *   0x94 PHY_CTRL3, 0xa0/0xa4 PHY_REFCLK_CTRL2/3.
	 * Vendor targets: TX_AMP=127, DEEMPH1=0x22, DEEMPH2=0x18, DEEMPH3=0x18,
	 * TX0_TERM_OFFST=0, RX0_EQ=0. Mainline writes NONE of these.
	 */
	dev_info(pci->dev, "LAB PHYREG: 3c=%#010x 44=%#010x 74=%#010x 78=%#010x 7c=%#010x 80=%#010x 84=%#010x 88=%#010x 8c=%#010x 94=%#010x a0=%#010x a4=%#010x\n",
		 readl(pcie->parf + 0x3c), readl(pcie->parf + 0x44),
		 readl(pcie->parf + 0x74), readl(pcie->parf + 0x78),
		 readl(pcie->parf + 0x7c), readl(pcie->parf + 0x80),
		 readl(pcie->parf + 0x84), readl(pcie->parf + 0x88),
		 readl(pcie->parf + 0x8c), readl(pcie->parf + 0x94),
		 readl(pcie->parf + 0xa0), readl(pcie->parf + 0xa4));

	/*
	 * LAB: full PARF/PHY dump so it can be diffed against the working 3.10
	 * (notes/wifi-octagon.md golden dump): PM_STTS/PCS_DEEMPH/PCS_SWING/
	 * CONFIG_BITS/TEST_BUS are the settings the mainline driver never
	 * programmes, and PCS_SWING/PCS_DEEMPH are the TX electrical knobs.
	 */
	val = readl(pcie->parf + PARF_SYS_CTRL);
	dev_info(pci->dev, "PARF SYS_CTRL=%#010x PM_STTS=%#010x PCS_DEEMPH=%#010x PCS_SWING=%#010x PHY_CTRL=%#010x PHY_REFCLK=%#010x CONFIG_BITS=%#010x TEST_BUS=%#010x DBI_BASE=%#010x SLV_SIZE=%#010x AXI_HALT=%#010x LTSSM=%#010x\n",
		 val,
		 readl(pcie->parf + PARF_PM_CTRL),
		 readl(pcie->parf + PARF_PCS_DEEMPH),
		 readl(pcie->parf + PARF_PCS_SWING),
		 readl(pcie->parf + PARF_PHY_CTRL),
		 readl(pcie->parf + PARF_PHY_REFCLK),
		 readl(pcie->parf + PARF_CONFIG_BITS),
		 readl(pcie->parf + 0xe4),
		 readl(pcie->parf + PARF_DBI_BASE_ADDR),
		 readl(pcie->parf + PARF_SLV_ADDR_SPACE_SIZE),
		 readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT),
		 readl(pcie->parf + PARF_LTSSM));

	if (pcie->ep_wake)
		dev_info(pci->dev, "endpoint wake line is %s\n",
			 gpiod_get_value_cansleep(pcie->ep_wake) ? "asserted" : "idle");


	return 0;
}

static int qcom_pcie_host_init(struct dw_pcie_rp *pp);
static void qcom_pcie_host_deinit(struct dw_pcie_rp *pp);

/*
 * Second pass, reproducing the vendor's two-enable shape on msm8994.
 *
 * msm_pcie_enable() runs once during boot before anything powers the QCA6174
 * and fails ("link initialization failed"); cnss_wlan_get_resources() then
 * powers WLAN_EN and the second msm_pcie_enable() redoes assert-PERST# ->
 * clocks/PARF/PHY -> release-PERST# with the endpoint already powered, which is
 * what makes 168c:003e answer config space.
 *
 * This runs from qcom_pcie_host_post_init(), i.e. after dw_pcie_host_init()
 * has already run the first link attempt and pci_host_probe(), so the failed
 * state is observable here and the scanned-but-empty bus 1 can be rescanned.
 *
 * A PERST# pulse here is only safe because the first pass really did fail to
 * train a link (vddpe-3v3 was never enabled): re-pulsing PERST# on a live link
 * resets QCA9xxx silicon on this unit (see notes/wifi-octagon.md).
 */
/*
 * 3.10 msm_pcie_config_controller()'s two error-reporting *enables*, applied
 * once the link is up and the bus has been scanned - the point where 3.10 runs
 * them inline in msm_pcie_enable(). msm_pcie_write_mask(reg, 0, val) ORs val
 * in, so both are enables:
 *
 *   PCIE20_ACK_F_ASPM_CTRL_REG (0x70C) BIT(15)  - no qcom,n-fts
 *   PCIE20_CAP_DEVCTRLSTATUS   (0x78)  BIT(3..0) - CERE|NFERE|FERE|URRE
 *
 * The third 3.10 write there, PCIE20_BRIDGE_CTRL (0x3C) BIT(16|17), is Bridge
 * Control Parity Error Response Enable + **SERR# Enable**; enabling SERR# on a
 * mainline ARM kernel with no SERR# handler escalates any downstream error to
 * SError and resets this SoC (measured, testOO), so it is deliberately omitted.
 *
 * DevCtl reporting matters: with it clear an Unsupported-Request completion is
 * silently discarded, which is exactly the signature before this change
 * (rv=0, all-1s data, no AER record).
 */
/*
 * LAB testQX: the working vendor re-does the ENTIRE bring-up a second time
 * (its first enable fails to reach link-up, then a disable+re-enable with the
 * same register sequence succeeds and the EP answers). Our first pass does
 * reach link-up, so the guarded second pass never runs. ath10k similarly notes
 * "QCA6174 requires cold + warm reset to work". Force the full reset cycle
 * regardless of first-pass link state. Default 0 = unchanged behavior.
 */
static unsigned int always_second_pass;
module_param(always_second_pass, uint, 0644);

/*
 * LAB testR20 (vector #1: replicate the vendor's RUNTIME on-demand context).
 * 3.10 does not enumerate at probe: the RC init fails, and cnss later powers the
 * EP and calls msm_pcie_enumerate() at runtime (~0.7s) - a *deferred* powered
 * bring-up with the rails/EN already stable. Mainline's probe-time second pass is
 * close, but let us ALSO run one more identical powered bring-up ~2.5s after boot
 * from a workqueue, exactly like cnss, and rescan. If the EP answers here but not
 * in host_post, the differentiator is the runtime context/ordering.
 */
static void qcom_pcie_reenum_work(struct work_struct *work)
{
	struct qcom_pcie *pcie = container_of(to_delayed_work(work),
					      struct qcom_pcie, reenum_work);
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct pci_dev *ep;
	int ret;

	pr_emerg("\n##### WIFI: [R20] deferred (cnss-style) re-enumeration now #####\n");

	/* ensure endpoint powered + settled, exactly like cnss before enumerate */
	qcom_pcie_msm8994_rail(pcie, true);
	usleep_range(70000, 71000);

	qcom_pcie_host_deinit(pp);
	qcom_pcie_msm8994_rail(pcie, false);
	usleep_range(20000, 21000);
	pcie->second_pass = true;

	ret = qcom_pcie_host_init(pp);
	if (!ret) {
		ret = dw_pcie_setup_rc(pp);
		if (!ret) {
			qcom_pcie_start_link(pci);
			ret = dw_pcie_wait_for_link(pci);
		}
	}
	if (ret) {
		pr_emerg("##### WIFI: [R20] link not up (%d) #####\n", ret);
		return;
	}

	pci_rescan_bus(pp->bridge->bus);
	ep = pci_get_domain_bus_and_slot(1, 1, 0);
	if (!ep)
		ep = pci_get_domain_bus_and_slot(1, 1, PCI_DEVFN(0, 0));
	pr_emerg("##### WIFI: [R20] deferred result: %s #####\n",
		 ep ? "EP PRESENT (QCA answers in runtime context!)"
		    : "still no EP");
	if (ep)
		pci_dev_put(ep);
}

static void qcom_pcie_host_post_msm8994(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	u32 val;
	int ret;

	/*
	 * Second pass. The probe-time attempt ran with the endpoint unpowered and
	 * therefore did not train a link; that unpowered PERST# cycle is required
	 * (see qcom_pcie_pre_init_msm8994()). Power the rail and redo the whole
	 * bring-up, then rescan the bus that pass 1 left empty.
	 *
	 * Re-pulsing PERST# here is only safe because pass 1 genuinely failed to
	 * train: a PERST# pulse on a live link resets QCA9xxx silicon on this unit.
	 */
	if (!dw_pcie_link_up(pci) || always_second_pass) {
		dev_info(pci->dev, "first pass trained no link, redoing it powered (link_up=%d force=%u)\n",
			 dw_pcie_link_up(pci), always_second_pass);

		qcom_pcie_host_deinit(pp);

		/*
		 * LAB: give the endpoint a clean power-cycle - force the rail off
		 * (in case it was already on, which would mean pass 1 never gave
		 * the chip a reset edge), then let init() power it back up. The
		 * vendor's disable/enable pair power-cycles the rails too
		 * (msm_pcie_vreg_deinit() then msm_pcie_vreg_init()).
		 */
		qcom_pcie_msm8994_rail(pcie, false);
		usleep_range(20000, 21000);

		pcie->second_pass = true;

		ret = qcom_pcie_host_init(pp);
		if (ret) {
			dev_err(pci->dev, "second bring-up failed (%d)\n", ret);
			pr_emerg("\n##### WIFI: FAIL (host_init %d) #####\n\n", ret);
			return;
		}

		ret = dw_pcie_setup_rc(pp);
		if (ret) {
			pr_emerg("\n##### WIFI: FAIL (setup_rc %d) #####\n\n", ret);
			return;
		}

		qcom_pcie_start_link(pci);

		ret = dw_pcie_wait_for_link(pci);
		if (ret) {
			dev_err(pci->dev, "endpoint did not answer (%d)\n", ret);
			pr_emerg("\n##### WIFI: FAIL (link wait %d) #####\n\n", ret);
			return;
		}

		/*
		 * LAB (decisive): replicate the vendor's EXACT manual outbound
		 * region-0 program and read the endpoint straight out of the
		 * config window, bypassing the DWC abstraction entirely.
		 *
		 * 3.10 msm_pcie_cfg_bdf() -> msm_pcie_iatu_config(dev, 0, CFG0,
		 *   axi_conf->start, axi_conf->start + SZ_4K - 1, 0x01000000)
		 * writes, in this order: viewport 0, CTRL2=0 (disable), CTRL1=type,
		 * LBAR, UBAR, LAR, LTAR, UTAR, then CTRL2=BIT(31).
		 * Its live dump reads CTRL1=00000004 LAR=f8801fff LTAR=01000000 -
		 * i.e. a 4 KiB window, where the DWC code programs 0x7f000.
		 *
		 * If this returns 003e168c the endpoint IS answering and the
		 * difference is in the DWC path/window size; if it returns all-1s
		 * the hardware genuinely is not responding even to the vendor's
		 * own sequence.
		 */
		{
			void __iomem *atu = pci->atu_base;

			/* clear AER first, so we can tell whether the READ causes it */
			writel(0xffffffff, pci->dbi_base + 0x104);
			writel(0xffffffff, pci->dbi_base + 0x110);
			wmb();
			dev_info(pci->dev, "LAB AER0: before read uncorr=%#010x corr=%#010x\n",
				 readl(pci->dbi_base + 0x104),
				 readl(pci->dbi_base + 0x110));

			writel(0, pci->dbi_base + PCIE_ATU_VIEWPORT);
			wmb();
			writel(0, atu + PCIE_ATU_REGION_CTRL2);
			wmb();
			writel(PCIE_TLP_TYPE_CFG0_RDWR, atu + PCIE_ATU_REGION_CTRL1);
			writel(pp->cfg0_base, atu + PCIE_ATU_LOWER_BASE);
			writel(0, atu + PCIE_ATU_UPPER_BASE);
			writel(pp->cfg0_base + SZ_4K - 1, atu + PCIE_ATU_LIMIT);
			writel(0x01000000, atu + PCIE_ATU_LOWER_TARGET);
			writel(0, atu + PCIE_ATU_UPPER_TARGET);
			wmb();
			writel(PCIE_ATU_ENABLE, atu + PCIE_ATU_REGION_CTRL2);
			wmb();

			/* LNKSTA lives at cap 0x12 -> DBI 0x82 (0x80 is LNKCTL) */
			val = readw(pci->dbi_base + 0x82);
			dev_info(pci->dev, "LAB MANUAL: pre-read LNKSTA=%#06x DLLLA=%u PARF_LTSSM=%#x SecSta=%#06x BridgeCtl=%#06x\n",
				 val, !!(val & PCI_EXP_LNKSTA_DLLLA),
				 readl(pcie->parf + PARF_LTSSM),
				 readw(pci->dbi_base + 0x1e),
				 readw(pci->dbi_base + 0x3e));

			/*
			 * LAB testR21: the "low-power link" clue. If the link/EP is
			 * gated in L1/L1SS, config reads can come back all-1s. Force
			 * FULL L0 before reading: clear ASPM (LNKCTL bits0-1), set the
			 * link-retrain bit (LNKCTL bit5), clear the RC + EP L1SS
			 * control registers, wait for retrain, then read config.
			 */
			{
				u32 lc = readl(pci->dbi_base + 0x80);
				u32 l1s1 = readl(pci->dbi_base + 0x158);
				u32 l1s2 = readl(pci->dbi_base + 0x15c);
				u32 l1cap = readl(pci->dbi_base + 0x154);

				dev_info(pci->dev, "LAB R21: pre LNKCTL=%#010x L1SScap=%#010x L1S1=%#010x L1S2=%#010x\n",
					 lc, l1cap, l1s1, l1s2);
				/* disable ASPM + set retrain + clear L1SS */
				writel(lc & ~0x3, pci->dbi_base + 0x80);
				writel(0, pci->dbi_base + 0x158);
				writel(0, pci->dbi_base + 0x15c);
				wmb();
				/* trigger link retrain */
				writel((lc & ~0x3) | BIT(5), pci->dbi_base + 0x80);
				usleep_range(20000, 21000);
				dev_info(pci->dev, "LAB R21: post LNKCTL=%#010x LNKSTA=%#06x\n",
					 readl(pci->dbi_base + 0x80),
					 readw(pci->dbi_base + 0x82));
				dev_info(pci->dev, "LAB R21: forced-L0 config read vid_did=%#010x\n",
					 readl(pp->va_cfg0_base + 0x00));
			}

			dev_info(pci->dev, "LAB MANUAL: entered, cfg0_base=%#llx va_cfg0=%px\n",
				 (u64)pp->cfg0_base, pp->va_cfg0_base);

			val = readl(pp->va_cfg0_base + 0x00);
			dev_info(pci->dev, "LAB MANUAL: vid_did=%#010x\n", val);
			val = readl(pp->va_cfg0_base + 0x04);
			dev_info(pci->dev, "LAB MANUAL: cmd_stat=%#010x\n", val);
			val = readl(pp->va_cfg0_base + 0x08);
			dev_info(pci->dev, "LAB MANUAL: class=%#010x\n", val);
			val = readl(pp->va_cfg0_base + 0x0c);
			dev_info(pci->dev, "LAB MANUAL: hdr=%#010x\n", val);

			/*
			 * The vendor reads dev->conf, obtained with a plain
			 * ioremap/devm_ioremap_resource on the same "conf"
			 * resource. Mainline reads pp->va_cfg0_base, obtained with
			 * devm_pci_remap_cfg_resource(). Compare the two mappings
			 * directly - if the plain mapping answers, the "PCI config"
			 * remap is the difference.
			 */
			{
				void __iomem *plain;

				plain = ioremap(pp->cfg0_base, SZ_4K);
				if (plain) {
					dev_info(pci->dev, "LAB MAP: plain ioremap vid_did=%#010x cmd=%#010x class=%#010x\n",
						 readl(plain + 0x00),
						 readl(plain + 0x04),
						 readl(plain + 0x08));
					iounmap(plain);
				} else {
					dev_info(pci->dev, "LAB MAP: ioremap failed\n");
				}
				dev_info(pci->dev, "LAB MAP: cfg0_base=%pa va_cfg0_base=%px dbi=%px\n",
					 &pp->cfg0_base, pp->va_cfg0_base, pci->dbi_base);
			}

			/*
				* LAB testR19 (decisive, untried): we have ONLY EVER done
				* config READS. Do a config WRITE to the EP's Command
				* register (offset 0x04) and watch:
				*  - does it complete, raise UR, or master-abort?
				*  - does the DW TX TLP counter advance (did the TLP
				*    physically egress the RC)?
				* Plus dump the DW link-debug + vendor-specific TLP/ERR
				* counters around the access, and the RC DevSta completion-
				* timeout bit. This discriminates "CFG0 TLP never sent" vs
				* "EP receives and stays silent".
				*/
			{
				u32 pre = readl(pp->va_cfg0_base + 0x04);
				u32 tx0 = readl(pci->dbi_base + 0x78c);
				u32 ldbg0 = readl(pci->dbi_base + 0x72c);
				u32 c0 = readl(pci->dbi_base + 0x710);

				dev_info(pci->dev, "LAB R19: pre cmd=%#010x TX0=%#010x DBG(0x72c)=%#010x CTRL0(0x710)=%#010x\n",
					 pre, tx0, ldbg0, c0);

				/* write EP Command: keep existing, set MEM+BM bits */
				writel(pre | 0x6, pp->va_cfg0_base + 0x04);
				wmb();
				dev_info(pci->dev, "LAB R19: post cmd=%#010x TX=%#010x\n",
					 readl(pp->va_cfg0_base + 0x04),
					 readl(pci->dbi_base + 0x78c));
				dev_info(pci->dev, "LAB R19: RCBAR=%#010x 0x73c(link dbg)=%#010x 0x74c=%#010x\n",
					 readl(pp->va_cfg0_base + 0x10),
					 readl(pci->dbi_base + 0x73c),
					 readl(pci->dbi_base + 0x74c));
				dev_info(pci->dev, "LAB R19: RC DevSta(0x0a)=%#06x (CmplTimeout=bit15)\n",
					 readw(pci->dbi_base + 0x0a));
				dev_info(pci->dev, "LAB R19: SecSta(0x1e)=%#06x Cert=%u\?\n",
					 readw(pci->dbi_base + 0x1e));
			}

			val = readw(pci->dbi_base + 0x82);
			dev_info(pci->dev, "LAB MANUAL: post-read LNKSTA=%#06x DLLLA=%u PARF_LTSSM=%#x SecSta=%#06x (MasterAbort=%u UR=%u)\n",
				 val, !!(val & PCI_EXP_LNKSTA_DLLLA),
				 readl(pcie->parf + PARF_LTSSM),
				 readw(pci->dbi_base + 0x1e),
				 !!(readw(pci->dbi_base + 0x1e) & BIT(13)),
				 !!(readw(pci->dbi_base + 0x1e) & BIT(12)));
			val = readl(pci->dbi_base + 0x104);
			dev_info(pci->dev, "LAB AER1: after read uncorr=%#010x corr=%#010x\n",
				 val, readl(pci->dbi_base + 0x110));
			/* read the config window a second time, no ATU change */
			val = readl(pp->va_cfg0_base + 0x00);
			dev_info(pci->dev, "LAB AER2: re-read vid=%#010x uncorr=%#010x corr=%#010x\n",
				 val, readl(pci->dbi_base + 0x104),
				 readl(pci->dbi_base + 0x110));
		}

		pci_rescan_bus(pp->bridge->bus);
	}

	val = readl(pci->dbi_base + 0x70c);
	val |= BIT(15);
	writel(val, pci->dbi_base + 0x70c);

	val = readl(pci->dbi_base + 0x78);
	val |= BIT(3) | BIT(2) | BIT(1) | BIT(0);
	writel(val, pci->dbi_base + 0x78);

	/*
	 * The full RC-core (DBI) dump showed exactly one writable difference
	 * mainline does not already match: Bridge Control bit 0, "Parity Error
	 * Response Enable". 3.10 reads 0x0003 at 0x3c, mainline 0x0002 - note
	 * SERR# (bit 1) is *already* set on mainline, so the earlier testOO crash
	 * was from this parity bit, not SERR#.  Set it and see.
	 */
	val = readl(pci->dbi_base + 0x3c);
	dev_info(pci->dev, "LAB BRIDGECTL was %#010x\n", val);
	val |= BIT(16);
	writel(val, pci->dbi_base + 0x3c);
	dev_info(pci->dev, "LAB BRIDGECTL now %#010x\n",
		 readl(pci->dbi_base + 0x3c));

	/*
	 * LAB: the vendor's working RC reads LNKCTL (0x80) = 0x0002, i.e. ASPM
	 * **L1 Enable** (bit 1), while mainline reads 0x0000. Only the L0s
	 * *capability* bit was tested before (testOA - negative); the L1 enable
	 * itself never was. Match it.
	 */
	val = readl(pci->dbi_base + 0x80);
	dev_info(pci->dev, "LAB LNKCTL was %#010x\n", val);
	val |= BIT(1);
	writel(val, pci->dbi_base + 0x80);
	dev_info(pci->dev, "LAB LNKCTL now %#010x\n",
		 readl(pci->dbi_base + 0x80));


	/*
	 * LAB testQC: print the WiFi result BIG on the console so it can be
	 * read on the device screen without telnet/USB.
	 *
	 * WATERPROOF LOGIC (rev 2): do NOT assume the endpoint is at bus1/devfn0.
	 * Scan the entire domain for a QCA Atheros endpoint (vendor 0x168c) and
	 * also record any device found on the root bus's secondary bus. This way
	 * we only print FAIL when there really is no QCA endpoint anywhere.
	 */
	{
		struct pci_dev *ep = NULL, *tmp;
		u32 lnk = readw(pci->dbi_base + 0x82);
		struct pci_bus *bus;
		int found_any = 0;
		unsigned int found_bus = 0, found_devfn = 0;
		u32 found_id = 0;

		/* Prefer a 168c (QCA Atheros) function; else any non-bridge EP. */
		for (bus = pci_find_bus(1, 0); bus; bus = pci_find_next_bus(bus)) {
			list_for_each_entry(tmp, &bus->devices, bus_list) {
				found_any++;
				if (!ep && (tmp->vendor == PCI_VENDOR_ID_ATHEROS ||
					    !tmp->hdr_type)) {
					ep = tmp;
					found_bus = tmp->bus->number;
					found_devfn = tmp->devfn;
					found_id = (tmp->vendor << 16) | tmp->device;
				}
			}
		}

		pr_emerg("\n");
		pr_emerg("############################################\n");
		if (ep) {
			pr_emerg("#####  WIFI: SUCCESS - EP %04x:%04x @ %u:%02x.%u  #####\n",
				 ep->vendor, ep->device, found_bus,
				 PCI_SLOT(found_devfn), PCI_FUNC(found_devfn));
		} else if (found_any) {
			pr_emerg("#####  WIFI: BUS OK but no QCA EP (%d devs)  #####\n",
				 found_any);
		} else {
			pr_emerg("#####  WIFI: FAIL - NO DEVICE ON bus1  #####\n");
		}
		pr_emerg("#####  link LNKSTA=%#06x DLLLA=%u  #####\n",
			 lnk, !!(lnk & PCI_EXP_LNKSTA_DLLLA));
		pr_emerg("#####  raw cfg0[0]=%#010x (ffffffff=no answer)  #####\n",
			 readl(pci->dbi_base));  /* DBI VID/DID on this RC */
		pr_emerg("############################################\n\n");

		/*
		 * LAB: hold the banner on the console for 5 s so it can be read
		 * on the device screen before later boot messages scroll past.
		 */
		{
			int b;
			for (b = 0; b < 50; b++) {
				mdelay(100);
				if (!(b % 10))
					pr_emerg("#####  [WiFi banner: elapsed %ds/5s: %s]  #####\n",
						 b / 10,
						 ep ? "QCA EP PRESENT" :
						      (found_any ? "no QCA EP"
								 : "nothing on bus1"));
			}
		}

		/* LAB testR20: schedule the deferred (cnss-style) re-enumeration. */
		INIT_DELAYED_WORK(&pcie->reenum_work, qcom_pcie_reenum_work);
		schedule_delayed_work(&pcie->reenum_work, msecs_to_jiffies(2500));

		(void)found_id;
	}
}


static int qcom_pcie_get_resources_2_4_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_4_0 *res = &pcie->res.v2_4_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	bool is_ipq = of_device_is_compatible(dev->of_node, "qcom,pcie-ipq4019");
	int ret;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks);
	if (res->num_clks < 0) {
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->resets[0].id = "axi_m";
	res->resets[1].id = "axi_s";
	res->resets[2].id = "axi_m_sticky";
	res->resets[3].id = "pipe_sticky";
	res->resets[4].id = "pwr";
	res->resets[5].id = "ahb";
	res->resets[6].id = "pipe";
	res->resets[7].id = "axi_m_vmid";
	res->resets[8].id = "axi_s_xpu";
	res->resets[9].id = "parf";
	res->resets[10].id = "phy";
	res->resets[11].id = "phy_ahb";

	res->num_resets = is_ipq ? 12 : 6;

	ret = devm_reset_control_bulk_get_exclusive(dev, res->num_resets, res->resets);
	if (ret < 0)
		return ret;

	return 0;
}

static void qcom_pcie_deinit_2_4_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_4_0 *res = &pcie->res.v2_4_0;
	u32 val;

	/* Force PHY to lowest power state*/
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val |= PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	reset_control_bulk_assert(res->num_resets, res->resets);
	clk_bulk_disable_unprepare(res->num_clks, res->clks);
}

static int qcom_pcie_init_2_4_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_4_0 *res = &pcie->res.v2_4_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	int ret;

	ret = reset_control_bulk_assert(res->num_resets, res->resets);
	if (ret < 0) {
		dev_err(dev, "cannot assert resets\n");
		return ret;
	}

	usleep_range(10000, 12000);

	ret = reset_control_bulk_deassert(res->num_resets, res->resets);
	if (ret < 0) {
		dev_err(dev, "cannot deassert resets\n");
		return ret;
	}

	usleep_range(10000, 12000);

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks);
	if (ret) {
		reset_control_bulk_assert(res->num_resets, res->resets);
		return ret;
	}

	return 0;
}

static int qcom_pcie_get_resources_2_3_3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_3 *res = &pcie->res.v2_3_3;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	int ret;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks);
	if (res->num_clks < 0) {
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->rst[0].id = "axi_m";
	res->rst[1].id = "axi_s";
	res->rst[2].id = "pipe";
	res->rst[3].id = "axi_m_sticky";
	res->rst[4].id = "sticky";
	res->rst[5].id = "ahb";
	res->rst[6].id = "sleep";

	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(res->rst), res->rst);
	if (ret < 0)
		return ret;

	return 0;
}

static void qcom_pcie_deinit_2_3_3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_3 *res = &pcie->res.v2_3_3;
	u32 val;

	/* Force PHY to lowest power state */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val |= PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	clk_bulk_disable_unprepare(res->num_clks, res->clks);
}

static int qcom_pcie_init_2_3_3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_3_3 *res = &pcie->res.v2_3_3;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	int ret;

	ret = reset_control_bulk_assert(ARRAY_SIZE(res->rst), res->rst);
	if (ret < 0) {
		dev_err(dev, "cannot assert resets\n");
		return ret;
	}

	usleep_range(2000, 2500);

	ret = reset_control_bulk_deassert(ARRAY_SIZE(res->rst), res->rst);
	if (ret < 0) {
		dev_err(dev, "cannot deassert resets\n");
		return ret;
	}

	/*
	 * Don't have a way to see if the reset has completed.
	 * Wait for some time.
	 */
	usleep_range(2000, 2500);

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks);
	if (ret) {
		dev_err(dev, "cannot prepare/enable clocks\n");
		goto err_assert_resets;
	}

	return 0;

err_assert_resets:
	/*
	 * Not checking for failure, will anyway return
	 * the original failure in 'ret'.
	 */
	reset_control_bulk_assert(ARRAY_SIZE(res->rst), res->rst);

	return ret;
}

static int qcom_pcie_post_init_2_3_3(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	u32 val;

	/* Force PHY out of lowest power state */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val &= ~PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	qcom_pcie_configure_dbi_atu_base(pcie);

	writel(MST_WAKEUP_EN | SLV_WAKEUP_EN | MSTR_ACLK_CGC_DIS
		| SLV_ACLK_CGC_DIS | CORE_CLK_CGC_DIS |
		AUX_PWR_DET | L23_CLK_RMV_DIS | L1_CLK_RMV_DIS,
		pcie->parf + PARF_SYS_CTRL);
	writel(0, pcie->parf + PARF_Q2A_FLUSH);

	writel(PCI_COMMAND_MASTER, pci->dbi_base + PCI_COMMAND);

	dw_pcie_dbi_ro_wr_en(pci);

	writel(PCIE_CAP_SLOT_VAL, pci->dbi_base + offset + PCI_EXP_SLTCAP);

	val = readl(pci->dbi_base + offset + PCI_EXP_LNKCAP);
	val &= ~PCI_EXP_LNKCAP_ASPMS;
	writel(val, pci->dbi_base + offset + PCI_EXP_LNKCAP);

	writel(PCI_EXP_DEVCTL2_COMP_TMOUT_DIS, pci->dbi_base + offset +
		PCI_EXP_DEVCTL2);

	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

static int qcom_pcie_get_resources_2_7_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_7_0 *res = &pcie->res.v2_7_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	int ret;

	res->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(res->rst))
		return PTR_ERR(res->rst);

	res->supplies[0].supply = "vdda";
	res->supplies[1].supply = "vddpe-3v3";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(res->supplies),
				      res->supplies);
	if (ret)
		return ret;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks);
	if (res->num_clks < 0) {
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	return 0;
}

static int qcom_pcie_init_2_7_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_7_0 *res = &pcie->res.v2_7_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	u32 val;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(res->supplies), res->supplies);
	if (ret < 0) {
		dev_err(dev, "cannot enable regulators\n");
		return ret;
	}

	ret = clk_bulk_prepare_enable(res->num_clks, res->clks);
	if (ret < 0)
		goto err_disable_regulators;

	ret = reset_control_assert(res->rst);
	if (ret) {
		dev_err(dev, "reset assert failed (%d)\n", ret);
		goto err_disable_clocks;
	}

	usleep_range(1000, 1500);

	ret = reset_control_deassert(res->rst);
	if (ret) {
		dev_err(dev, "reset deassert failed (%d)\n", ret);
		goto err_disable_clocks;
	}

	/* Wait for reset to complete, required on SM8450 */
	usleep_range(1000, 1500);

	/* configure PCIe to RC mode */
	writel(DEVICE_TYPE_RC, pcie->parf + PARF_DEVICE_TYPE);

	/* Force PHY out of lowest power state */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val &= ~PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	qcom_pcie_configure_dbi_atu_base(pcie);

	/* MAC PHY_POWERDOWN MUX DISABLE  */
	val = readl(pcie->parf + PARF_SYS_CTRL);
	val &= ~MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN;
	writel(val, pcie->parf + PARF_SYS_CTRL);

	val = readl(pcie->parf + PARF_MHI_CLOCK_RESET_CTRL);
	val |= BYPASS;
	writel(val, pcie->parf + PARF_MHI_CLOCK_RESET_CTRL);

	/* Enable L1 and L1SS */
	val = readl(pcie->parf + PARF_PM_CTRL);
	val &= ~REQ_NOT_ENTR_L1;
	writel(val, pcie->parf + PARF_PM_CTRL);

	pci->l1ss_support = true;

	val = readl(pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2);
	val |= EN;
	writel(val, pcie->parf + PARF_AXI_MSTR_WR_ADDR_HALT_V2);

	return 0;
err_disable_clocks:
	clk_bulk_disable_unprepare(res->num_clks, res->clks);
err_disable_regulators:
	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies);

	return ret;
}

static int qcom_pcie_post_init_2_7_0(struct qcom_pcie *pcie)
{
	const struct qcom_pcie_cfg *pcie_cfg = pcie->cfg;

	if (pcie_cfg->override_no_snoop)
		writel(WR_NO_SNOOP_OVERRIDE_EN | RD_NO_SNOOP_OVERRIDE_EN,
				pcie->parf + PARF_NO_SNOOP_OVERRIDE);

	qcom_pcie_set_slot_cap(pcie->pci);

	return 0;
}

static int qcom_pcie_enable_aspm(struct pci_dev *pdev, void *userdata)
{
	/*
	 * Downstream devices need to be in D0 state before enabling PCI PM
	 * substates.
	 */
	pci_set_power_state_locked(pdev, PCI_D0);
	pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL);

	return 0;
}

static void qcom_pcie_host_post_init_2_7_0(struct qcom_pcie *pcie)
{
	struct dw_pcie_rp *pp = &pcie->pci->pp;

	pci_walk_bus(pp->bridge->bus, qcom_pcie_enable_aspm, NULL);
}

static void qcom_pcie_deinit_2_7_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_7_0 *res = &pcie->res.v2_7_0;
	u32 val;

	/* Force PHY to lowest power state */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val |= PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	clk_bulk_disable_unprepare(res->num_clks, res->clks);

	regulator_bulk_disable(ARRAY_SIZE(res->supplies), res->supplies);
}

static int qcom_pcie_config_sid_1_9_0(struct qcom_pcie *pcie)
{
	/* iommu map structure */
	struct {
		u32 bdf;
		u32 phandle;
		u32 smmu_sid;
		u32 smmu_sid_len;
	} *map;
	void __iomem *bdf_to_sid_base = pcie->parf + PARF_BDF_TO_SID_TABLE_N;
	struct device *dev = pcie->pci->dev;
	u8 qcom_pcie_crc8_table[CRC8_TABLE_SIZE];
	int i, nr_map, size = 0;
	u32 smmu_sid_base;
	u32 val;

	of_get_property(dev->of_node, "iommu-map", &size);
	if (!size)
		return 0;

	/* Enable BDF to SID translation by disabling bypass mode (default) */
	val = readl(pcie->parf + PARF_BDF_TO_SID_CFG);
	val &= ~BDF_TO_SID_BYPASS;
	writel(val, pcie->parf + PARF_BDF_TO_SID_CFG);

	map = kzalloc(size, GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	of_property_read_u32_array(dev->of_node, "iommu-map", (u32 *)map,
				   size / sizeof(u32));

	nr_map = size / (sizeof(*map));

	crc8_populate_msb(qcom_pcie_crc8_table, QCOM_PCIE_CRC8_POLYNOMIAL);

	/* Registers need to be zero out first */
	memset_io(bdf_to_sid_base, 0, CRC8_TABLE_SIZE * sizeof(u32));

	/* Extract the SMMU SID base from the first entry of iommu-map */
	smmu_sid_base = map[0].smmu_sid;

	/* Look for an available entry to hold the mapping */
	for (i = 0; i < nr_map; i++) {
		__be16 bdf_be = cpu_to_be16(map[i].bdf);
		u32 val;
		u8 hash;

		hash = crc8(qcom_pcie_crc8_table, (u8 *)&bdf_be, sizeof(bdf_be), 0);

		val = readl(bdf_to_sid_base + hash * sizeof(u32));

		/* If the register is already populated, look for next available entry */
		while (val) {
			u8 current_hash = hash++;
			u8 next_mask = 0xff;

			/* If NEXT field is NULL then update it with next hash */
			if (!(val & next_mask)) {
				val |= (u32)hash;
				writel(val, bdf_to_sid_base + current_hash * sizeof(u32));
			}

			val = readl(bdf_to_sid_base + hash * sizeof(u32));
		}

		/* BDF [31:16] | SID [15:8] | NEXT [7:0] */
		val = map[i].bdf << 16 | (map[i].smmu_sid - smmu_sid_base) << 8 | 0;
		writel(val, bdf_to_sid_base + hash * sizeof(u32));
	}

	kfree(map);

	return 0;
}

static int qcom_pcie_get_resources_2_9_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_9_0 *res = &pcie->res.v2_9_0;
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;

	res->num_clks = devm_clk_bulk_get_all(dev, &res->clks);
	if (res->num_clks < 0) {
		dev_err(dev, "Failed to get clocks\n");
		return res->num_clks;
	}

	res->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(res->rst))
		return PTR_ERR(res->rst);

	return 0;
}

static void qcom_pcie_deinit_2_9_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_9_0 *res = &pcie->res.v2_9_0;
	u32 val;

	/* Force PHY to lowest power state */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val |= PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	clk_bulk_disable_unprepare(res->num_clks, res->clks);
}

static int qcom_pcie_init_2_9_0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_2_9_0 *res = &pcie->res.v2_9_0;
	struct device *dev = pcie->pci->dev;
	int ret;

	ret = reset_control_assert(res->rst);
	if (ret) {
		dev_err(dev, "reset assert failed (%d)\n", ret);
		return ret;
	}

	/*
	 * Delay periods before and after reset deassert are working values
	 * from downstream Codeaurora kernel
	 */
	usleep_range(2000, 2500);

	ret = reset_control_deassert(res->rst);
	if (ret) {
		dev_err(dev, "reset deassert failed (%d)\n", ret);
		return ret;
	}

	usleep_range(2000, 2500);

	return clk_bulk_prepare_enable(res->num_clks, res->clks);
}

static int qcom_pcie_post_init_2_9_0(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	u32 val;
	int i;

	/* Force PHY out of lowest power state */
	val = readl(pcie->parf + PARF_PHY_CTRL);
	val &= ~PHY_TEST_PWR_DOWN;
	writel(val, pcie->parf + PARF_PHY_CTRL);

	qcom_pcie_configure_dbi_atu_base(pcie);

	writel(DEVICE_TYPE_RC, pcie->parf + PARF_DEVICE_TYPE);
	writel(BYPASS | MSTR_AXI_CLK_EN | AHB_CLK_EN,
		pcie->parf + PARF_MHI_CLOCK_RESET_CTRL);
	writel(GEN3_RELATED_OFF_RXEQ_RGRDLESS_RXTS |
		GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL,
		pci->dbi_base + GEN3_RELATED_OFF);

	writel(MST_WAKEUP_EN | SLV_WAKEUP_EN | MSTR_ACLK_CGC_DIS |
		SLV_ACLK_CGC_DIS | CORE_CLK_CGC_DIS |
		AUX_PWR_DET | L23_CLK_RMV_DIS | L1_CLK_RMV_DIS,
		pcie->parf + PARF_SYS_CTRL);

	writel(0, pcie->parf + PARF_Q2A_FLUSH);

	dw_pcie_dbi_ro_wr_en(pci);

	writel(PCIE_CAP_SLOT_VAL, pci->dbi_base + offset + PCI_EXP_SLTCAP);

	val = readl(pci->dbi_base + offset + PCI_EXP_LNKCAP);
	val &= ~PCI_EXP_LNKCAP_ASPMS;
	writel(val, pci->dbi_base + offset + PCI_EXP_LNKCAP);

	writel(PCI_EXP_DEVCTL2_COMP_TMOUT_DIS, pci->dbi_base + offset +
			PCI_EXP_DEVCTL2);

	dw_pcie_dbi_ro_wr_dis(pci);

	for (i = 0; i < 256; i++)
		writel(0, pcie->parf + PARF_BDF_TO_SID_TABLE_N + (4 * i));

	return 0;
}

/*
 * LAB testQY: msm8994 has no dw_pcie_ops.link_up, so this is the generic
 * qcom link-up used for it. The vendor's authoritative link-up check is
 * ELBI XMLH_LINK_UP (ELBI_SYS_STTS BIT(10)) - working ELBI=0x00011401 - in
 * ADDITION to the DWC DLLLA. Log both and require both (only when ELBI is
 * mapped): a DWC-up-but-ELBI-not-up state would explain config reads returning
 * all-1s while mainline believes the link is fine.
 */
static bool qcom_pcie_link_up(struct dw_pcie *pci)
{
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	u16 val = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA);

	return val & PCI_EXP_LNKSTA_DLLLA;
}

static enum dw_pcie_ltssm qcom_pcie_get_ltssm(struct dw_pcie *pci)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pci);
	u32 val;

	if (pcie->cfg->ops->get_ltssm)
		return pcie->cfg->ops->get_ltssm(pcie);

	val = readl(pcie->parf + PARF_LTSSM);

	return (enum dw_pcie_ltssm)FIELD_GET(PARF_LTSSM_STATE_MASK, val);
}

static void qcom_pcie_phy_power_off(struct qcom_pcie *pcie)
{
	struct qcom_pcie_port *port;

	list_for_each_entry(port, &pcie->ports, list)
		phy_power_off(port->phy);
}

static int qcom_pcie_phy_power_on(struct qcom_pcie *pcie)
{
	struct qcom_pcie_port *port;
	int ret;

	list_for_each_entry(port, &pcie->ports, list) {
		ret = phy_set_mode_ext(port->phy, PHY_MODE_PCIE, PHY_MODE_PCIE_RC);
		if (ret)
			return ret;

		ret = phy_power_on(port->phy);
		if (ret) {
			qcom_pcie_phy_power_off(pcie);
			return ret;
		}
	}

	return 0;
}

static void qcom_pcie_configure_ports(struct qcom_pcie *pcie)
{
	struct qcom_pcie_port *port;

	list_for_each_entry(port, &pcie->ports, list)
		dw_pcie_program_t_power_on(pcie->pci, port->l1ss_t_power_on);
}

/*
 * LAB: delay between PERST# deassert (link up) and the first config scan.
 * The working vendor leaves ~600 ms after releasing PERST before it first
 * reads the EP (which then answers immediately); mainline scans at once. If
 * the QCA6174 needs settle/CRS time after link-up this closes it.
 * Set via cmdline: qcom_pcie.ep_settle_ms=NNN  (default 0 = unchanged).
 */
static unsigned int ep_settle_ms;
module_param(ep_settle_ms, uint, 0644);
MODULE_PARM_DESC(ep_settle_ms, "ms to wait after PERST# deassert before config scan");

static int qcom_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct qcom_pcie *pcie = to_qcom_pcie(pci);
	int ret;

	/*
	 * msm8994: power the endpoint rail before the RC asserts PERST#, so the
	 * QCA6174 is up when the reset is released (3.10 cnss powers WLAN_EN
	 * before msm_pcie_enumerate()).
	 */
	if (pcie->cfg->ops->pre_init) {
		ret = pcie->cfg->ops->pre_init(pcie);
		if (ret)
			return ret;
	}

	qcom_pcie_perst_assert(pcie);

	ret = pcie->cfg->ops->init(pcie);
	if (ret)
		return ret;

	ret = qcom_pcie_phy_power_on(pcie);
	if (ret)
		goto err_deinit;

	if (!pci->suspended) {
		ret = pci_pwrctrl_create_devices(pci->dev);
		if (ret)
			goto err_disable_phy;
	}

	if (!pp->skip_pwrctrl_off) {
		ret = pci_pwrctrl_power_on_devices(pci->dev);
		if (ret)
			goto err_pwrctrl_destroy;
	}

	if (pcie->cfg->ops->post_init) {
		ret = pcie->cfg->ops->post_init(pcie);
		if (ret)
			goto err_pwrctrl_power_off;
	}

	qcom_pcie_clear_aspm_l0s(pcie->pci);
	dw_pcie_remove_capability(pcie->pci, PCI_CAP_ID_MSIX);
	dw_pcie_remove_ext_capability(pcie->pci, PCI_EXT_CAP_ID_DPC);

	qcom_pcie_configure_ports(pcie);

	qcom_pcie_perst_deassert(pcie);

	if (pcie->cfg->ops->config_sid) {
		ret = pcie->cfg->ops->config_sid(pcie);
		if (ret)
			goto err_assert_reset;
	}

	if (ep_settle_ms) {
		dev_info(pci->dev, "LAB: waiting %u ms after PERST# before config scan\n",
			 ep_settle_ms);
		msleep(ep_settle_ms);
	}

	pp->bridge->reset_root_port = qcom_pcie_reset_root_port;

	return 0;

err_assert_reset:
	qcom_pcie_perst_assert(pcie);
err_pwrctrl_power_off:
	if (!pp->skip_pwrctrl_off)
		pci_pwrctrl_power_off_devices(pci->dev);
err_pwrctrl_destroy:
	if (ret != -EPROBE_DEFER && !pci->suspended)
		pci_pwrctrl_destroy_devices(pci->dev);
err_disable_phy:
	qcom_pcie_phy_power_off(pcie);
err_deinit:
	pcie->cfg->ops->deinit(pcie);

	return ret;
}

static void qcom_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct qcom_pcie *pcie = to_qcom_pcie(pci);

	qcom_pcie_perst_assert(pcie);

	if (!pci->pp.skip_pwrctrl_off) {
		/*
		 * No need to destroy pwrctrl devices as this function only
		 * gets called during system suspend as of now.
		 */
		pci_pwrctrl_power_off_devices(pci->dev);
	}

	qcom_pcie_phy_power_off(pcie);
	pcie->cfg->ops->deinit(pcie);
}

static void qcom_pcie_host_post_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct qcom_pcie *pcie = to_qcom_pcie(pci);

	/*
	 * During system suspend, the Qcom RC driver may turn off the
	 * analog circuitry of PHY and remove controller votes to save
	 * power. If the link is in L1SS and the endpoint asserts CLKREQ#
	 * to exit L1SS, the time required to wake the system and restore
	 * the PHY/REFCLK may exceed the L1SS exit timing (L10_REFCLK_ON +
	 * T_COMMONMODE), resulting in Link Down (LDn) and a reset of the
	 * endpoint. Set this flag to indicate this limitation to client
	 * drivers so that they can avoid relying on device state being
	 * preserved during system suspend.
	 */
	pp->bridge->broken_l1ss_resume = true;

	if (pcie->cfg->ops->host_post_init)
		pcie->cfg->ops->host_post_init(pcie);
}

static void qcom_pcie_host_pme_turn_off(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);

	writel(ELBI_SYS_CTRL_PME_TURNOFF_MSG, pci->elbi_base + ELBI_SYS_CTRL);
}

static const struct dw_pcie_host_ops qcom_pcie_dw_ops = {
	.init		= qcom_pcie_host_init,
	.deinit		= qcom_pcie_host_deinit,
	.post_init	= qcom_pcie_host_post_init,
	.pme_turn_off	= qcom_pcie_host_pme_turn_off,
};

/* Qcom IP rev.: 2.1.0	Synopsys IP rev.: 4.01a */
static const struct qcom_pcie_ops ops_2_1_0 = {
	.get_resources = qcom_pcie_get_resources_2_1_0,
	.init = qcom_pcie_init_2_1_0,
	.post_init = qcom_pcie_post_init_2_1_0,
	.deinit = qcom_pcie_deinit_2_1_0,
	.ltssm_enable = qcom_pcie_2_1_0_ltssm_enable,
	.get_ltssm = qcom_pcie_2_1_0_get_ltssm,
};

/* Qcom IP rev.: 1.0.0	Synopsys IP rev.: 4.11a */
static const struct qcom_pcie_ops ops_1_0_0 = {
	.get_resources = qcom_pcie_get_resources_1_0_0,
	.init = qcom_pcie_init_1_0_0,
	.post_init = qcom_pcie_post_init_1_0_0,
	.deinit = qcom_pcie_deinit_1_0_0,
	.ltssm_enable = qcom_pcie_2_1_0_ltssm_enable,
	.get_ltssm = qcom_pcie_2_1_0_get_ltssm,
};

/* Qcom IP rev.: 2.3.2	Synopsys IP rev.: 4.21a */
static const struct qcom_pcie_ops ops_2_3_2 = {
	.get_resources = qcom_pcie_get_resources_2_3_2,
	.init = qcom_pcie_init_2_3_2,
	.post_init = qcom_pcie_post_init_2_3_2,
	.deinit = qcom_pcie_deinit_2_3_2,
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable,
};

/* MSM8994: 2.3.2 PARF, endpoint rail powered on the second bring-up pass */
static const struct qcom_pcie_ops ops_msm8994 = {
	.get_resources = qcom_pcie_get_resources_msm8994,
	.pre_init = qcom_pcie_pre_init_msm8994,
	.init = qcom_pcie_init_msm8994,
	.post_init = qcom_pcie_post_init_msm8994,
	.deinit = qcom_pcie_deinit_msm8994,
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable,
	.host_post_init = qcom_pcie_host_post_msm8994,
};

/* Qcom IP rev.: 2.4.0	Synopsys IP rev.: 4.20a */
static const struct qcom_pcie_ops ops_2_4_0 = {
	.get_resources = qcom_pcie_get_resources_2_4_0,
	.init = qcom_pcie_init_2_4_0,
	.post_init = qcom_pcie_post_init_2_3_2,
	.deinit = qcom_pcie_deinit_2_4_0,
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable,
};

/* Qcom IP rev.: 2.3.3	Synopsys IP rev.: 4.30a */
static const struct qcom_pcie_ops ops_2_3_3 = {
	.get_resources = qcom_pcie_get_resources_2_3_3,
	.init = qcom_pcie_init_2_3_3,
	.post_init = qcom_pcie_post_init_2_3_3,
	.deinit = qcom_pcie_deinit_2_3_3,
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable,
};

/* Qcom IP rev.: 2.7.0	Synopsys IP rev.: 4.30a */
static const struct qcom_pcie_ops ops_2_7_0 = {
	.get_resources = qcom_pcie_get_resources_2_7_0,
	.init = qcom_pcie_init_2_7_0,
	.post_init = qcom_pcie_post_init_2_7_0,
	.deinit = qcom_pcie_deinit_2_7_0,
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable,
};

/* Qcom IP rev.: 1.9.0 */
static const struct qcom_pcie_ops ops_1_9_0 = {
	.get_resources = qcom_pcie_get_resources_2_7_0,
	.init = qcom_pcie_init_2_7_0,
	.post_init = qcom_pcie_post_init_2_7_0,
	.host_post_init = qcom_pcie_host_post_init_2_7_0,
	.deinit = qcom_pcie_deinit_2_7_0,
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable,
	.config_sid = qcom_pcie_config_sid_1_9_0,
};

/* Qcom IP rev.: 1.21.0  Synopsys IP rev.: 5.60a */
static const struct qcom_pcie_ops ops_1_21_0 = {
	.get_resources = qcom_pcie_get_resources_2_7_0,
	.init = qcom_pcie_init_2_7_0,
	.post_init = qcom_pcie_post_init_2_7_0,
	.host_post_init = qcom_pcie_host_post_init_2_7_0,
	.deinit = qcom_pcie_deinit_2_7_0,
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable,
};

/* Qcom IP rev.: 2.9.0  Synopsys IP rev.: 5.00a */
static const struct qcom_pcie_ops ops_2_9_0 = {
	.get_resources = qcom_pcie_get_resources_2_9_0,
	.init = qcom_pcie_init_2_9_0,
	.post_init = qcom_pcie_post_init_2_9_0,
	.deinit = qcom_pcie_deinit_2_9_0,
	.ltssm_enable = qcom_pcie_2_3_2_ltssm_enable,
};

static const struct qcom_pcie_cfg cfg_1_0_0 = {
	.ops = &ops_1_0_0,
};

static const struct qcom_pcie_cfg cfg_1_9_0 = {
	.ops = &ops_1_9_0,
};

static const struct qcom_pcie_cfg cfg_1_34_0 = {
	.ops = &ops_1_9_0,
	.override_no_snoop = true,
	.no_l0s = true,
};

static const struct qcom_pcie_cfg cfg_2_1_0 = {
	.ops = &ops_2_1_0,
};

static const struct qcom_pcie_cfg cfg_2_3_2 = {
	.ops = &ops_2_3_2,
	.no_l0s = true,
};

static const struct qcom_pcie_cfg cfg_msm8994 = {
	.ops = &ops_msm8994,
};

static const struct qcom_pcie_cfg cfg_2_3_3 = {
	.ops = &ops_2_3_3,
};

static const struct qcom_pcie_cfg cfg_2_4_0 = {
	.ops = &ops_2_4_0,
};

static const struct qcom_pcie_cfg cfg_2_7_0 = {
	.ops = &ops_2_7_0,
};

static const struct qcom_pcie_cfg cfg_2_9_0 = {
	.ops = &ops_2_9_0,
};

static const struct qcom_pcie_cfg cfg_sc8280xp = {
	.ops = &ops_1_21_0,
	.no_l0s = true,
};

static const struct qcom_pcie_cfg cfg_fw_managed = {
	.firmware_managed = true,
};

static const struct dw_pcie_ops dw_pcie_ops = {
	.link_up = qcom_pcie_link_up,
	.start_link = qcom_pcie_start_link,
	.get_ltssm = qcom_pcie_get_ltssm,
};

static int qcom_pcie_icc_init(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	int ret;

	pcie->icc_mem = devm_of_icc_get(pci->dev, "pcie-mem");
	if (IS_ERR(pcie->icc_mem))
		return PTR_ERR(pcie->icc_mem);

	pcie->icc_cpu = devm_of_icc_get(pci->dev, "cpu-pcie");
	if (IS_ERR(pcie->icc_cpu))
		return PTR_ERR(pcie->icc_cpu);
	/*
	 * Some Qualcomm platforms require interconnect bandwidth constraints
	 * to be set before enabling interconnect clocks.
	 *
	 * Set an initial peak bandwidth corresponding to single-lane Gen 1
	 * for the pcie-mem path.
	 */
	ret = icc_set_bw(pcie->icc_mem, 0, QCOM_PCIE_LINK_SPEED_TO_BW(1));
	if (ret) {
		dev_err(pci->dev, "Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
			ret);
		return ret;
	}

	/*
	 * Since the CPU-PCIe path is only used for activities like register
	 * access of the host controller and endpoint Config/BAR space access,
	 * HW team has recommended to use a minimal bandwidth of 1KBps just to
	 * keep the path active.
	 */
	ret = icc_set_bw(pcie->icc_cpu, 0, kBps_to_icc(1));
	if (ret) {
		dev_err(pci->dev, "Failed to set bandwidth for CPU-PCIe interconnect path: %d\n",
			ret);
		icc_set_bw(pcie->icc_mem, 0, 0);
		return ret;
	}

	return 0;
}

static void qcom_pcie_icc_opp_update(struct qcom_pcie *pcie)
{
	u32 offset, status, width, speed;
	struct dw_pcie *pci = pcie->pci;
	struct dev_pm_opp_key key = {};
	unsigned long freq_kbps;
	struct dev_pm_opp *opp;
	int ret, freq_mbps;

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	status = readw(pci->dbi_base + offset + PCI_EXP_LNKSTA);

	/* Only update constraints if link is up. */
	if (!(status & PCI_EXP_LNKSTA_DLLLA))
		return;

	speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, status);
	width = FIELD_GET(PCI_EXP_LNKSTA_NLW, status);

	if (pcie->icc_mem) {
		ret = icc_set_bw(pcie->icc_mem, 0,
				 width * QCOM_PCIE_LINK_SPEED_TO_BW(speed));
		if (ret) {
			dev_err(pci->dev, "Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
				ret);
		}
	} else if (pcie->use_pm_opp) {
		freq_mbps = pcie_dev_speed_mbps(pcie_get_link_speed(speed));
		if (freq_mbps < 0)
			return;

		freq_kbps = freq_mbps * KILO;
		opp = dev_pm_opp_find_level_exact(pci->dev, speed);
		if (IS_ERR(opp)) {
			 /* opp-level is not defined use only frequency */
			opp = dev_pm_opp_find_freq_exact(pci->dev, freq_kbps * width,
							 true);
		} else {
			/* put opp-level OPP */
			dev_pm_opp_put(opp);

			key.freq = freq_kbps * width;
			key.level = speed;
			key.bw = 0;
			opp = dev_pm_opp_find_key_exact(pci->dev, &key, true);
		}
		if (!IS_ERR(opp)) {
			ret = dev_pm_opp_set_opp(pci->dev, opp);
			if (ret)
				dev_err(pci->dev, "Failed to set OPP for freq (%lu): %d\n",
					freq_kbps * width, ret);
			dev_pm_opp_put(opp);
		}
	}
}

static int qcom_pcie_set_max_opp(struct device *dev)
{
	unsigned long max_freq = ULONG_MAX;
	struct dev_pm_opp *opp;
	int ret;

	opp = dev_pm_opp_find_freq_floor(dev, &max_freq);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	ret = dev_pm_opp_set_opp(dev, opp);
	dev_pm_opp_put(opp);

	return ret;
}

/*
 * Qcom PCIe controllers only support one Root Port per controller instance. So
 * this function ignores the 'pci_dev' associated with the Root Port and just
 * resets the host bridge, which in turn resets the Root Port also.
 */
static int qcom_pcie_reset_root_port(struct pci_host_bridge *bridge,
				  struct pci_dev *pdev)
{
	struct device *dev = bridge->dev.parent;
	struct qcom_pcie *pcie = dev_get_drvdata(dev);
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	u32 val;
	int ret;

	/* Wait for the pending transactions to be completed */
	ret = readl_relaxed_poll_timeout(pcie->parf + PARF_STATUS, val,
					 val & FLUSH_COMPLETED, 10,
					 FLUSH_TIMEOUT_US);
	if (ret) {
		dev_err(dev, "Flush completion failed: %d\n", ret);
		return ret;
	}

	/* Clear the FLUSH_MODE to allow the core to be reset */
	val = readl(pcie->parf + PARF_LTSSM);
	val |= SW_CLEAR_FLUSH_MODE;
	writel(val, pcie->parf + PARF_LTSSM);

	/* Wait for the FLUSH_MODE to clear */
	ret = readl_relaxed_poll_timeout(pcie->parf + PARF_LTSSM, val,
					 !(val & FLUSH_MODE), 10,
					 FLUSH_TIMEOUT_US);
	if (ret) {
		dev_err(dev, "Flush mode clear failed: %d\n", ret);
		return ret;
	}

	qcom_pcie_host_deinit(pp);

	ret = qcom_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Host init failed\n");
		return ret;
	}

	ret = dw_pcie_setup_rc(pp);
	if (ret)
		return ret;

	/*
	 * Re-enable global IRQ events as the PARF_INT_ALL_MASK register is
	 * non-sticky.
	 */
	if (pcie->global_irq)
		writel_relaxed(PARF_INT_ALL_LINK_DOWN | PARF_INT_MSI_DEV_0_7,
				pcie->parf + PARF_INT_ALL_MASK);

	qcom_pcie_start_link(pci);

	ret = dw_pcie_wait_for_link(pci);
	if (ret)
		return ret;

	dev_dbg(dev, "Root Port reset completed\n");

	return 0;
}

static int qcom_pcie_link_transition_count(struct seq_file *s, void *data)
{
	struct qcom_pcie *pcie = (struct qcom_pcie *)dev_get_drvdata(s->private);

	seq_printf(s, "L0s transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_PM_LINKST_IN_L0S));

	seq_printf(s, "L1 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_PM_LINKST_IN_L1));

	seq_printf(s, "L1.1 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L1));

	seq_printf(s, "L1.2 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_AUX_CLK_IN_L1SUB_L2));

	seq_printf(s, "L2 transition count: %u\n",
		   readl_relaxed(pcie->mhi + PARF_DEBUG_CNT_PM_LINKST_IN_L2));

	return 0;
}

static void qcom_pcie_init_debugfs(struct qcom_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	char *name;

	name = devm_kasprintf(dev, GFP_KERNEL, "%pOFP", dev->of_node);
	if (!name)
		return;

	pcie->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_devm_seqfile(dev, "link_transition_count", pcie->debugfs,
				    qcom_pcie_link_transition_count);
}

static irqreturn_t qcom_pcie_global_irq_thread(int irq, void *data)
{
	struct qcom_pcie *pcie = data;
	struct dw_pcie_rp *pp = &pcie->pci->pp;
	struct device *dev = pcie->pci->dev;
	struct pci_dev *port;
	unsigned long status = readl_relaxed(pcie->parf + PARF_INT_ALL_STATUS);

	writel_relaxed(status, pcie->parf + PARF_INT_ALL_CLEAR);

	if (test_and_clear_bit(INT_ALL_LINK_DOWN, &status)) {
		dev_dbg(dev, "Received Link down event\n");
		for_each_pci_bridge(port, pp->bridge->bus) {
			if (pci_pcie_type(port) == PCI_EXP_TYPE_ROOT_PORT)
				pci_host_handle_link_down(port);
		}
	}

	return IRQ_HANDLED;
}

static void qcom_pci_free_msi(void *ptr)
{
	struct dw_pcie_rp *pp = (struct dw_pcie_rp *)ptr;

	if (pp && pp->use_imsi_rx)
		dw_pcie_free_msi(pp);
}

static int qcom_pcie_ecam_host_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent;
	struct dw_pcie_rp *pp;
	struct dw_pcie *pci;
	int ret;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pp = &pci->pp;
	pci->dbi_base = cfg->win;
	pp->num_vectors = MSI_DEF_NUM_VECTORS;

	/*
	 * dw_pcie_msi_host_init() is called directly here, bypassing
	 * dw_pcie_host_init() where pp->lock is normally initialized.
	 */
	raw_spin_lock_init(&pp->lock);

	ret = dw_pcie_msi_host_init(pp);
	if (ret)
		return ret;

	pp->use_imsi_rx = true;
	dw_pcie_msi_init(pp);

	return devm_add_action_or_reset(dev, qcom_pci_free_msi, pp);
}

static const struct pci_ecam_ops pci_qcom_ecam_ops = {
	.init		= qcom_pcie_ecam_host_init,
	.pci_ops	= {
		.map_bus	= pci_ecam_map_bus,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	}
};

/* Check if @node is a child of @dev in DT */
static bool qcom_pcie_is_child_node(struct device *dev,
				    struct device_node *node)
{
	struct device_node *parent;

	for (parent = of_get_parent(node); parent;
	     parent = of_get_next_parent(parent)) {
		if (parent == dev->of_node) {
			of_node_put(parent);
			return true;
		}
	}

	return false;
}

/* Parse PERST# from all nodes in depth first manner starting from @np */
static int qcom_pcie_parse_perst(struct qcom_pcie *pcie,
				 struct qcom_pcie_port *port,
				 struct device_node *np)
{
	struct device *dev = pcie->pci->dev;
	struct qcom_pcie_perst *perst;
	struct device_node *gpio_np;
	struct gpio_desc *reset;
	int ret;

	if (pcie->reset) {
		dev_warn_once(dev,
			      "Reusing PERST# from Root Complex node. DT needs to be fixed!\n");
		reset = pcie->reset;
		goto skip_perst_parsing;
	}

	if (!of_find_property(np, "reset-gpios", NULL))
		goto parse_child_node;

	/*
	 * Skip GPIOs provided by a PCIe device which is a child of the Root
	 * Complex (e.g., a PCIe switch with GPIO controller capability). Such
	 * controllers won't be available at RC probe time and their PERST#
	 * should be controlled by the respective PCI client driver
	 * implementation.
	 */
	gpio_np = of_parse_phandle(np, "reset-gpios", 0);
	if (!gpio_np) {
		dev_err(dev, "Failed to parse GPIO provider\n");
		return -EINVAL;
	}

	if (qcom_pcie_is_child_node(dev, gpio_np)) {
		of_node_put(gpio_np);
		goto parse_child_node;
	}
	of_node_put(gpio_np);

	reset = devm_fwnode_gpiod_get(dev, of_fwnode_handle(np), "reset",
				      GPIOD_OUT_HIGH, "PERST#");
	if (IS_ERR(reset)) {
		/*
		 * FIXME: GPIOLIB currently supports exclusive GPIO access only.
		 * Non exclusive access is broken. But shared PERST# requires
		 * non-exclusive access. So once GPIOLIB properly supports it,
		 * implement it here.
		 */
		if (PTR_ERR(reset) == -EBUSY)
			dev_err(dev, "Shared PERST# is not supported\n");

		return PTR_ERR(reset);
	}

skip_perst_parsing:
	perst = devm_kzalloc(dev, sizeof(*perst), GFP_KERNEL);
	if (!perst)
		return -ENOMEM;

	INIT_LIST_HEAD(&perst->list);
	perst->desc = reset;
	list_add_tail(&perst->list, &port->perst);

parse_child_node:
	for_each_available_child_of_node_scoped(np, child) {
		ret = qcom_pcie_parse_perst(pcie, port, child);
		if (ret)
			return ret;
	}

	return 0;
}

static int qcom_pcie_parse_port(struct qcom_pcie *pcie, struct device_node *node)
{
	struct device *dev = pcie->pci->dev;
	struct qcom_pcie_port *port;
	struct phy *phy;
	int ret;

	phy = devm_of_phy_get(dev, node, NULL);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	ret = phy_init(phy);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&port->perst);

	ret = qcom_pcie_parse_perst(pcie, port, node);
	if (ret)
		return ret;

	/* TODO: Move to DWC core after multi Root Port support is added */
	of_property_read_u32(node, "t-power-on-us", &port->l1ss_t_power_on);

	port->phy = phy;
	INIT_LIST_HEAD(&port->list);
	list_add_tail(&port->list, &pcie->ports);

	return 0;
}

static int qcom_pcie_parse_ports(struct qcom_pcie *pcie)
{
	struct qcom_pcie_perst *perst, *tmp_perst;
	struct qcom_pcie_port *port, *tmp_port;
	struct device *dev = pcie->pci->dev;
	int ret = -ENODEV;

	if (of_find_property(dev->of_node, "perst-gpios", NULL)) {
		pcie->reset = devm_gpiod_get_optional(dev, "perst",
						      GPIOD_OUT_HIGH);
		if (IS_ERR(pcie->reset))
			return PTR_ERR(pcie->reset);
	}

	for_each_available_child_of_node_scoped(dev->of_node, of_port) {
		if (!of_node_is_type(of_port, "pci"))
			continue;
		ret = qcom_pcie_parse_port(pcie, of_port);
		if (ret)
			goto err_port_del;
	}

	return ret;

err_port_del:
	list_for_each_entry_safe(port, tmp_port, &pcie->ports, list) {
		list_for_each_entry_safe(perst, tmp_perst, &port->perst, list)
			list_del(&perst->list);
		phy_exit(port->phy);
		list_del(&port->list);
	}

	return ret;
}

static int qcom_pcie_parse_legacy_binding(struct qcom_pcie *pcie)
{
	struct device *dev = pcie->pci->dev;
	struct qcom_pcie_perst *perst;
	struct qcom_pcie_port *port;
	struct phy *phy;
	int ret;

	phy = devm_phy_optional_get(dev, "pciephy");
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	ret = phy_init(phy);
	if (ret)
		return ret;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	perst = devm_kzalloc(dev, sizeof(*perst), GFP_KERNEL);
	if (!perst)
		return -ENOMEM;

	port->phy = phy;
	INIT_LIST_HEAD(&port->list);
	list_add_tail(&port->list, &pcie->ports);

	perst->desc = pcie->reset;
	INIT_LIST_HEAD(&port->perst);
	INIT_LIST_HEAD(&perst->list);
	list_add_tail(&perst->list, &port->perst);

	return 0;
}

static int qcom_pcie_probe(struct platform_device *pdev)
{
	struct qcom_pcie_perst *perst, *tmp_perst;
	struct qcom_pcie_port *port, *tmp_port;
	const struct qcom_pcie_cfg *pcie_cfg;
	struct device *dev = &pdev->dev;
	struct qcom_pcie *pcie;
	struct dw_pcie_rp *pp;
	struct resource *res;
	struct dw_pcie *pci;
	int ret, irq;

	pcie_cfg = of_device_get_match_data(dev);
	if (!pcie_cfg) {
		dev_err(dev, "No platform data\n");
		return -ENODATA;
	}

	if (!pcie_cfg->firmware_managed && !pcie_cfg->ops) {
		dev_err(dev, "No platform ops\n");
		return -ENODATA;
	}

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		goto err_pm_runtime_put;

	if (pcie_cfg->firmware_managed) {
		struct pci_host_bridge *bridge;
		struct pci_config_window *cfg;

		bridge = devm_pci_alloc_host_bridge(dev, 0);
		if (!bridge) {
			ret = -ENOMEM;
			goto err_pm_runtime_put;
		}

		/* Parse and map our ECAM configuration space area */
		cfg = pci_host_common_ecam_create(dev, bridge,
				&pci_qcom_ecam_ops);
		if (IS_ERR(cfg)) {
			ret = PTR_ERR(cfg);
			goto err_pm_runtime_put;
		}

		bridge->sysdata = cfg;
		bridge->ops = (struct pci_ops *)&pci_qcom_ecam_ops.pci_ops;
		bridge->msi_domain = true;

		ret = pci_host_probe(bridge);
		if (ret)
			goto err_pm_runtime_put;

		return 0;
	}

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie) {
		ret = -ENOMEM;
		goto err_pm_runtime_put;
	}

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci) {
		ret = -ENOMEM;
		goto err_pm_runtime_put;
	}

	/*
	 * The DWC ATU 'TD' bit in outbound Control Register 1 is an override for
	 * the TLP Digest (ECRC) setting: for cores older than 5.10A the core
	 * appends a TLP Digest to *every* TLP whose address is translated by the
	 * ATU. The vendor 3.10 stack never sets it (msm_pcie_iatu_config() writes
	 * only the transaction type, PCIE20_CTRL1_TYPE_CFG0 = 0x4), so we are
	 * forcing a TLP Digest onto the QCA6174 that stock firmware never sees.
	 * Let the DT turn that override off.
	 */
	pci->no_ecrc = of_property_read_bool(dev->of_node, "qcom,no-ecrc");

	INIT_LIST_HEAD(&pcie->ports);

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;
	pp = &pci->pp;

	pcie->pci = pci;

	pcie->cfg = pcie_cfg;

	pcie->parf = devm_platform_ioremap_resource_byname(pdev, "parf");
	if (IS_ERR(pcie->parf)) {
		ret = PTR_ERR(pcie->parf);
		goto err_pm_runtime_put;
	}

	/* MHI region is optional */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mhi");
	if (res) {
		pcie->mhi = devm_ioremap_resource(dev, res);
		if (IS_ERR(pcie->mhi)) {
			ret = PTR_ERR(pcie->mhi);
			goto err_pm_runtime_put;
		}
	}

	/* OPP table is optional */
	ret = devm_pm_opp_of_add_table(dev);
	if (ret && ret != -ENODEV) {
		dev_err_probe(dev, ret, "Failed to add OPP table\n");
		goto err_pm_runtime_put;
	}

	/*
	 * Before the PCIe link is initialized, vote for highest OPP in the OPP
	 * table, so that we are voting for maximum voltage corner for the
	 * link to come up in maximum supported speed. At the end of the
	 * probe(), OPP will be updated using qcom_pcie_icc_opp_update().
	 */
	if (!ret) {
		ret = qcom_pcie_set_max_opp(dev);
		if (ret) {
			dev_err_probe(dev, ret, "Failed to set max OPP\n");
			goto err_pm_runtime_put;
		}

		pcie->use_pm_opp = true;
	} else {
		/* Skip ICC init if OPP is supported as it is handled by OPP */
		ret = qcom_pcie_icc_init(pcie);
		if (ret)
			goto err_pm_runtime_put;
	}

	ret = pcie->cfg->ops->get_resources(pcie);
	if (ret)
		goto err_pm_runtime_put;

	pp->ops = &qcom_pcie_dw_ops;

	ret = qcom_pcie_parse_ports(pcie);
	if (ret) {
		if (ret != -ENODEV) {
			dev_err_probe(pci->dev, ret,
				      "Failed to parse Root Port: %d\n", ret);
			goto err_pm_runtime_put;
		}

		/*
		 * In the case of properties not populated in Root Port node,
		 * fallback to the legacy method of parsing the Host Bridge
		 * node. This is to maintain DT backwards compatibility.
		 */
		ret = qcom_pcie_parse_legacy_binding(pcie);
		if (ret)
			goto err_pm_runtime_put;
	}

	platform_set_drvdata(pdev, pcie);

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err_probe(dev, ret, "cannot initialize host\n");
		goto err_phy_exit;
	}

	irq = platform_get_irq_byname_optional(pdev, "global");
	if (irq > 0) {
		const char *name;

		name = devm_kasprintf(dev, GFP_KERNEL, "qcom_pcie_global_irq%d",
				      pci_domain_nr(pp->bridge->bus));
		if (!name) {
			ret = -ENOMEM;
			goto err_host_deinit;
		}

		ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
						qcom_pcie_global_irq_thread,
						IRQF_ONESHOT, name, pcie);
		if (ret) {
			dev_err_probe(&pdev->dev, ret,
				      "Failed to request Global IRQ\n");
			goto err_host_deinit;
		}

		writel_relaxed(PARF_INT_ALL_LINK_DOWN | PARF_INT_MSI_DEV_0_7,
				pcie->parf + PARF_INT_ALL_MASK);

		pcie->global_irq = irq;
	}

	qcom_pcie_icc_opp_update(pcie);

	if (pcie->mhi)
		qcom_pcie_init_debugfs(pcie);

	return 0;

err_host_deinit:
	dw_pcie_host_deinit(pp);
err_phy_exit:
	list_for_each_entry_safe(port, tmp_port, &pcie->ports, list) {
		list_for_each_entry_safe(perst, tmp_perst, &port->perst, list)
			list_del(&perst->list);
		phy_exit(port->phy);
		list_del(&port->list);
	}
err_pm_runtime_put:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);

	return ret;
}

static int qcom_pcie_suspend_noirq(struct device *dev)
{
	struct qcom_pcie *pcie;
	int ret = 0;

	pcie = dev_get_drvdata(dev);
	if (!pcie)
		return 0;

	ret = dw_pcie_suspend_noirq(pcie->pci);
	if (ret)
		return ret;

	if (pcie->pci->suspended) {
		ret = icc_disable(pcie->icc_mem);
		if (ret)
			dev_err(dev, "Failed to disable PCIe-MEM interconnect path: %d\n", ret);

		ret = icc_disable(pcie->icc_cpu);
		if (ret)
			dev_err(dev, "Failed to disable CPU-PCIe interconnect path: %d\n", ret);

		if (pcie->use_pm_opp)
			dev_pm_opp_set_opp(pcie->pci->dev, NULL);
	} else {
		/*
		 * Set minimum bandwidth required to keep data path
		 * functional during suspend.
		 */
		if (pcie->icc_mem) {
			ret = icc_set_bw(pcie->icc_mem, 0, kBps_to_icc(1));
			if (ret) {
				dev_err(dev,
					"Failed to set bandwidth for PCIe-MEM interconnect path: %d\n",
					ret);
				return ret;
			}
		}

		/*
		 * Only disable CPU-PCIe interconnect path if the suspend
		 * is non-S2RAM.  On some platforms, DBI access can happen
		 * very late during S2RAM and a non-active CPU-PCIe
		 * interconnect path may lead to NoC error.
		 */
		if (pm_suspend_target_state != PM_SUSPEND_MEM) {
			ret = icc_disable(pcie->icc_cpu);
			if (ret)
				dev_err(dev, "Failed to disable CPU-PCIe interconnect path: %d\n",
					ret);

			if (pcie->use_pm_opp)
				dev_pm_opp_set_opp(pcie->pci->dev, NULL);
		}
	}
	return ret;
}

static int qcom_pcie_resume_noirq(struct device *dev)
{
	struct qcom_pcie *pcie;
	int ret;

	pcie = dev_get_drvdata(dev);
	if (!pcie)
		return 0;

	if (pcie->pci->suspended) {
		if (pcie->use_pm_opp) {
			ret = qcom_pcie_set_max_opp(dev);
			if (ret) {
				dev_err(dev, "Failed to set max OPP: %d\n", ret);
				return ret;
			}
		}

		ret = icc_enable(pcie->icc_cpu);
		if (ret) {
			dev_err(dev, "Failed to enable CPU-PCIe interconnect path: %d\n", ret);
			return ret;
		}

		ret = icc_enable(pcie->icc_mem);
		if (ret) {
			dev_err(dev, "Failed to enable PCIe-MEM interconnect path: %d\n", ret);
			goto disable_icc_cpu;
		}

		/*
		 * Ignore -ENODEV & -EIO here since it is expected when no
		 * endpoint is connected to the PCIe link.
		 */
		ret = dw_pcie_resume_noirq(pcie->pci);
		if (ret && ret != -ENODEV && ret != -EIO)
			goto disable_icc_mem;
	} else {
		if (pm_suspend_target_state != PM_SUSPEND_MEM) {
			if (pcie->use_pm_opp) {
				ret = qcom_pcie_set_max_opp(dev);
				if (ret) {
					dev_err(dev, "Failed to set max OPP: %d\n", ret);
					return ret;
				}
			}

			ret = icc_enable(pcie->icc_cpu);
			if (ret) {
				dev_err(dev, "Failed to enable CPU-PCIe interconnect path: %d\n",
					ret);
				return ret;
			}
		}
	}

	qcom_pcie_icc_opp_update(pcie);

	return 0;
disable_icc_mem:
	icc_disable(pcie->icc_mem);
disable_icc_cpu:
	icc_disable(pcie->icc_cpu);

	return ret;
}

static const struct of_device_id qcom_pcie_match[] = {
	{ .compatible = "qcom,hawi-pcie", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-apq8064", .data = &cfg_2_1_0 },
	{ .compatible = "qcom,pcie-apq8084", .data = &cfg_1_0_0 },
	{ .compatible = "qcom,pcie-ipq4019", .data = &cfg_2_4_0 },
	{ .compatible = "qcom,pcie-ipq5018", .data = &cfg_2_9_0 },
	{ .compatible = "qcom,pcie-ipq6018", .data = &cfg_2_9_0 },
	{ .compatible = "qcom,pcie-ipq8064", .data = &cfg_2_1_0 },
	{ .compatible = "qcom,pcie-ipq8064-v2", .data = &cfg_2_1_0 },
	{ .compatible = "qcom,pcie-ipq8074", .data = &cfg_2_3_3 },
	{ .compatible = "qcom,pcie-ipq8074-gen3", .data = &cfg_2_9_0 },
	{ .compatible = "qcom,pcie-ipq9574", .data = &cfg_2_9_0 },
	{ .compatible = "qcom,pcie-msm8994", .data = &cfg_msm8994 },
	{ .compatible = "qcom,pcie-msm8996", .data = &cfg_2_3_2 },
	{ .compatible = "qcom,pcie-qcs404", .data = &cfg_2_4_0 },
	{ .compatible = "qcom,pcie-sa8255p", .data = &cfg_fw_managed },
	{ .compatible = "qcom,pcie-sa8540p", .data = &cfg_sc8280xp },
	{ .compatible = "qcom,pcie-sa8775p", .data = &cfg_1_34_0},
	{ .compatible = "qcom,pcie-sc7280", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-sc8180x", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-sc8280xp", .data = &cfg_sc8280xp },
	{ .compatible = "qcom,pcie-sdm845", .data = &cfg_2_7_0 },
	{ .compatible = "qcom,pcie-sdx55", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-sm8150", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-sm8250", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-sm8350", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-sm8450-pcie0", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-sm8450-pcie1", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-sm8550", .data = &cfg_1_9_0 },
	{ .compatible = "qcom,pcie-x1e80100", .data = &cfg_sc8280xp },
	{ }
};

static void qcom_fixup_class(struct pci_dev *dev)
{
	dev->class = PCI_CLASS_BRIDGE_PCI_NORMAL;
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0101, qcom_fixup_class);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0104, qcom_fixup_class);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0106, qcom_fixup_class);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0107, qcom_fixup_class);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x0302, qcom_fixup_class);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x1000, qcom_fixup_class);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_QCOM, 0x1001, qcom_fixup_class);

static const struct dev_pm_ops qcom_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(qcom_pcie_suspend_noirq, qcom_pcie_resume_noirq)
};

static struct platform_driver qcom_pcie_driver = {
	.probe = qcom_pcie_probe,
	.driver = {
		.name = "qcom-pcie",
		.suppress_bind_attrs = true,
		.of_match_table = qcom_pcie_match,
		.pm = &qcom_pcie_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
builtin_platform_driver(qcom_pcie_driver);
