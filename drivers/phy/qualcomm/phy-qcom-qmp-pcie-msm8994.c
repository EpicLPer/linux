// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm MSM8994 20nm QMP PCIe PHY
 *
 * Sequence is the 3.10 20nm QMP / 19.2 MHz table in pci-msm.c
 * (non-FSM9010, non-vbg-opt). One 4 KiB block per RC.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#define QSERDES_COM_PLL_VCOTAIL_EN		0x004
#define QSERDES_COM_IE_TRIM			0x00c
#define QSERDES_COM_IP_TRIM			0x010
#define QSERDES_COM_PLL_CNTRL			0x014
#define QSERDES_COM_PLL_IP_SETI			0x024
#define QSERDES_COM_PLL_CP_SETI			0x034
#define QSERDES_COM_PLL_IP_SETP			0x038
#define QSERDES_COM_PLL_CP_SETP			0x03c
#define QSERDES_COM_SYSCLK_EN_SEL_TXBAND	0x048
#define QSERDES_COM_RESETSM_CNTRL		0x04c
#define QSERDES_COM_RESETSM_CNTRL2		0x050
#define QSERDES_COM_PLLLOCK_CMP1		0x090
#define QSERDES_COM_PLLLOCK_CMP2		0x094
#define QSERDES_COM_PLLLOCK_CMP_EN		0x09c
#define QSERDES_COM_DEC_START1			0x0ac
#define QSERDES_COM_RES_CODE_START_SEG1		0x0e0
#define QSERDES_COM_RES_CODE_CAL_CSR		0x0e8
#define QSERDES_COM_RES_TRIM_CONTROL		0x0f0
#define QSERDES_COM_DIV_FRAC_START1		0x100
#define QSERDES_COM_DIV_FRAC_START2		0x104
#define QSERDES_COM_DIV_FRAC_START3		0x108
#define QSERDES_COM_DEC_START2			0x10c
#define QSERDES_COM_PLL_RXTXEPCLK_EN		0x110
#define QSERDES_COM_PLL_CRCTRL			0x114

#define QSERDES_TX_RCV_DETECT_LVL		0x268

#define QSERDES_RX_CDR_CONTROL1			0x400
#define QSERDES_RX_CDR_CONTROL_HALF		0x408
#define QSERDES_RX_UCDR_FO_GAIN			0x414
#define QSERDES_RX_UCDR_SO_GAIN			0x418
#define QSERDES_RX_UCDR_SO_SATURATION_AND_ENABLE 0x41c
#define QSERDES_RX_RX_EQ_GAIN1_LSB		0x4a8
#define QSERDES_RX_RX_EQ_GAIN1_MSB		0x4ac
#define QSERDES_RX_RX_EQ_GAIN2_LSB		0x4b0
#define QSERDES_RX_RX_EQ_GAIN2_MSB		0x4b4
#define QSERDES_RX_RX_EQU_ADAPTOR_CNTRL2	0x4bc
#define QSERDES_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1	0x4f0
#define QSERDES_RX_RX_OFFSET_ADAPTOR_CNTRL2	0x4f4
#define QSERDES_RX_SIGDET_ENABLES		0x4f8
#define QSERDES_RX_SIGDET_CNTRL			0x500
#define QSERDES_RX_SIGDET_DEGLITCH_CNTRL	0x504

#define PCIE_PHY_SW_RESET			0x600
#define PCIE_PHY_POWER_DOWN_CONTROL		0x604
#define PCIE_PHY_START				0x608
#define PCIE_PHY_ENDPOINT_REFCLK_DRIVE		0x648
#define PCIE_PHY_RX_IDLE_DTCT_CNTRL		0x64c
#define PCIE_PHY_POWER_STATE_CONFIG1		0x650
#define PCIE_PHY_POWER_STATE_CONFIG2		0x654
#define PCIE_PHY_PCS_STATUS			0x728

#define PHY_READY				BIT(6)
#define PHY_INIT_TIMEOUT_US			10000

struct qmp_pcie_msm8994 {
	struct device *dev;
	void __iomem *base;
	struct clk *ref;
	struct clk *ldo;
	struct clk *pipe;
	struct reset_control *reset;
	struct regulator *vdda;
};

static void qmp_write(struct qmp_pcie_msm8994 *qmp, u32 off, u32 val)
{
	writel(val, qmp->base + off);
}

static int qmp_pcie_msm8994_power_on(struct phy *phy)
{
	struct qmp_pcie_msm8994 *qmp = phy_get_drvdata(phy);
	u32 val;
	int ret;

	/* 3.10 clk_init: LDO + PHY BCR deassert with the non-pipe clocks */
	ret = clk_prepare_enable(qmp->ldo);
	if (ret)
		return ret;

	ret = reset_control_deassert(qmp->reset);
	if (ret) {
		clk_disable_unprepare(qmp->ldo);
		return ret;
	}

	/*
	 * Do NOT replace this with a blanket copy of the vendor
	 * pcie20_phy_init_default() table: that was tried (testOW) and the PHY
	 * then fails to become ready on the second bring-up
	 * ("PHY not ready (pcs 0x68)" / "phy poweron failed --> -110"), which
	 * aborts the whole second pass - presumably because the second pass
	 * re-initialises an already-initialised PHY and the vendor PLL/RX values
	 * are not safe to apply twice. This sequence brings the PHY ready
	 * reliably on both passes and its end state matches the working vendor
	 * part register-for-register (verified, see notes/wifi-octagon.md).
	 */
	qmp_write(qmp, PCIE_PHY_POWER_DOWN_CONTROL, 0x03);
	qmp_write(qmp, QSERDES_COM_SYSCLK_EN_SEL_TXBAND, 0x08);
	qmp_write(qmp, QSERDES_COM_DEC_START1, 0x82);
	qmp_write(qmp, QSERDES_COM_DEC_START2, 0x03);
	qmp_write(qmp, QSERDES_COM_DIV_FRAC_START1, 0xd5);
	qmp_write(qmp, QSERDES_COM_DIV_FRAC_START2, 0xaa);
	qmp_write(qmp, QSERDES_COM_DIV_FRAC_START3, 0x4d);
	qmp_write(qmp, QSERDES_COM_PLLLOCK_CMP_EN, 0x03);
	qmp_write(qmp, QSERDES_COM_PLLLOCK_CMP1, 0x06);
	qmp_write(qmp, QSERDES_COM_PLLLOCK_CMP2, 0x1a);
	qmp_write(qmp, QSERDES_COM_PLL_CRCTRL, 0x7c);
	qmp_write(qmp, QSERDES_COM_PLL_CP_SETI, 0x1f);
	qmp_write(qmp, QSERDES_COM_PLL_IP_SETP, 0x12);
	qmp_write(qmp, QSERDES_COM_PLL_CP_SETP, 0x0f);
	qmp_write(qmp, QSERDES_COM_PLL_IP_SETI, 0x01);
	qmp_write(qmp, QSERDES_COM_IE_TRIM, 0x0f);
	qmp_write(qmp, QSERDES_COM_IP_TRIM, 0x0f);
	qmp_write(qmp, QSERDES_COM_PLL_CNTRL, 0x46);
	qmp_write(qmp, QSERDES_RX_CDR_CONTROL1, 0xf4);
	qmp_write(qmp, QSERDES_RX_CDR_CONTROL_HALF, 0x2c);
	qmp_write(qmp, QSERDES_COM_PLL_VCOTAIL_EN, 0xe1);
	qmp_write(qmp, QSERDES_COM_RESETSM_CNTRL, 0x91);
	qmp_write(qmp, QSERDES_COM_RESETSM_CNTRL2, 0x07);
	qmp_write(qmp, QSERDES_COM_RES_CODE_START_SEG1, 0x20);
	qmp_write(qmp, QSERDES_COM_RES_CODE_CAL_CSR, 0x77);
	qmp_write(qmp, QSERDES_COM_RES_TRIM_CONTROL, 0x15);
	qmp_write(qmp, QSERDES_TX_RCV_DETECT_LVL, 0x03);
	qmp_write(qmp, QSERDES_RX_UCDR_FO_GAIN, 0x09);
	qmp_write(qmp, QSERDES_RX_UCDR_SO_GAIN, 0x04);
	qmp_write(qmp, QSERDES_RX_UCDR_SO_SATURATION_AND_ENABLE, 0x49);
	qmp_write(qmp, QSERDES_RX_RX_EQ_GAIN1_LSB, 0xff);
	qmp_write(qmp, QSERDES_RX_RX_EQ_GAIN1_MSB, 0x1f);
	qmp_write(qmp, QSERDES_RX_RX_EQ_GAIN2_LSB, 0xff);
	qmp_write(qmp, QSERDES_RX_RX_EQ_GAIN2_MSB, 0x00);
	qmp_write(qmp, QSERDES_RX_RX_EQU_ADAPTOR_CNTRL2, 0x1e);
	qmp_write(qmp, QSERDES_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1, 0x67);
	qmp_write(qmp, QSERDES_RX_RX_OFFSET_ADAPTOR_CNTRL2, 0x80);
	qmp_write(qmp, QSERDES_RX_SIGDET_ENABLES, 0x40);
	qmp_write(qmp, QSERDES_RX_SIGDET_CNTRL, 0xb0);
	qmp_write(qmp, QSERDES_RX_SIGDET_DEGLITCH_CNTRL, 0x06);
	qmp_write(qmp, QSERDES_COM_PLL_RXTXEPCLK_EN, 0x10);
	qmp_write(qmp, PCIE_PHY_ENDPOINT_REFCLK_DRIVE, 0x10);
	qmp_write(qmp, PCIE_PHY_POWER_STATE_CONFIG1, 0xa3);
	qmp_write(qmp, PCIE_PHY_POWER_STATE_CONFIG2, 0x4b);
	qmp_write(qmp, PCIE_PHY_RX_IDLE_DTCT_CNTRL, 0x4d);
	qmp_write(qmp, PCIE_PHY_SW_RESET, 0x00);
	qmp_write(qmp, PCIE_PHY_START, 0x03);

	usleep_range(995, 1005);

	/* 3.10 msm_pcie_pipe_clk_init: set_rate 125 MHz selects pipe mux 2 */
	ret = clk_set_rate(qmp->pipe, 125000000);
	if (ret)
		goto err_reset;

	ret = clk_prepare_enable(qmp->pipe);
	if (ret)
		goto err_reset;

	ret = readl_poll_timeout(qmp->base + PCIE_PHY_PCS_STATUS, val,
				 !(val & PHY_READY), 200, PHY_INIT_TIMEOUT_US);
	if (ret) {
		dev_err(qmp->dev, "PHY not ready (pcs 0x%x)\n", val);
		clk_disable_unprepare(qmp->pipe);
		goto err_reset;
	}

	return 0;

err_reset:
	reset_control_assert(qmp->reset);
	clk_disable_unprepare(qmp->ldo);
	return ret;
}

static int qmp_pcie_msm8994_power_off(struct phy *phy)
{
	struct qmp_pcie_msm8994 *qmp = phy_get_drvdata(phy);

	qmp_write(qmp, PCIE_PHY_SW_RESET, 0x01);
	qmp_write(qmp, PCIE_PHY_START, 0x00);
	qmp_write(qmp, PCIE_PHY_POWER_DOWN_CONTROL, 0x00);
	clk_disable_unprepare(qmp->pipe);
	reset_control_assert(qmp->reset);
	clk_disable_unprepare(qmp->ldo);

	return 0;
}

static int qmp_pcie_msm8994_init(struct phy *phy)
{
	struct qmp_pcie_msm8994 *qmp = phy_get_drvdata(phy);
	int ret;

	/* 3.10 vreg-1.8 (L12) + ln_bb; LDO/BCR wait for RC clocks */
	ret = regulator_enable(qmp->vdda);
	if (ret)
		return ret;

	ret = clk_prepare_enable(qmp->ref);
	if (ret) {
		regulator_disable(qmp->vdda);
		return ret;
	}

	return 0;
}

static int qmp_pcie_msm8994_exit(struct phy *phy)
{
	struct qmp_pcie_msm8994 *qmp = phy_get_drvdata(phy);

	clk_disable_unprepare(qmp->ref);
	regulator_disable(qmp->vdda);
	return 0;
}

static const struct phy_ops qmp_pcie_msm8994_ops = {
	.init		= qmp_pcie_msm8994_init,
	.exit		= qmp_pcie_msm8994_exit,
	.power_on	= qmp_pcie_msm8994_power_on,
	.power_off	= qmp_pcie_msm8994_power_off,
	.owner		= THIS_MODULE,
};

static void phy_clk_release_provider(void *np)
{
	of_clk_del_provider(np);
}

static int qmp_pcie_msm8994_register_pipe(struct qmp_pcie_msm8994 *qmp)
{
	struct clk_fixed_rate *fixed;
	struct clk_init_data init = { };
	int ret;

	ret = of_property_read_string(qmp->dev->of_node, "clock-output-names",
				      &init.name);
	if (ret)
		return ret;

	fixed = devm_kzalloc(qmp->dev, sizeof(*fixed), GFP_KERNEL);
	if (!fixed)
		return -ENOMEM;

	init.ops = &clk_fixed_rate_ops;
	fixed->fixed_rate = 125000000;
	fixed->hw.init = &init;

	ret = devm_clk_hw_register(qmp->dev, &fixed->hw);
	if (ret)
		return ret;

	ret = of_clk_add_hw_provider(qmp->dev->of_node, of_clk_hw_simple_get,
				     &fixed->hw);
	if (ret)
		return ret;

	return devm_add_action_or_reset(qmp->dev, phy_clk_release_provider,
					qmp->dev->of_node);
}

static int qmp_pcie_msm8994_probe(struct platform_device *pdev)
{
	struct qmp_pcie_msm8994 *qmp;
	struct phy_provider *provider;
	struct phy *phy;
	int ret;

	qmp = devm_kzalloc(&pdev->dev, sizeof(*qmp), GFP_KERNEL);
	if (!qmp)
		return -ENOMEM;

	qmp->dev = &pdev->dev;
	qmp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(qmp->base))
		return PTR_ERR(qmp->base);

	qmp->ref = devm_clk_get(&pdev->dev, "ref");
	if (IS_ERR(qmp->ref))
		return PTR_ERR(qmp->ref);

	qmp->ldo = devm_clk_get(&pdev->dev, "ldo");
	if (IS_ERR(qmp->ldo))
		return PTR_ERR(qmp->ldo);

	qmp->pipe = devm_clk_get(&pdev->dev, "pipe");
	if (IS_ERR(qmp->pipe))
		return PTR_ERR(qmp->pipe);

	qmp->reset = devm_reset_control_get_exclusive(&pdev->dev, "phy");
	if (IS_ERR(qmp->reset))
		return PTR_ERR(qmp->reset);

	qmp->vdda = devm_regulator_get(&pdev->dev, "vdda");
	if (IS_ERR(qmp->vdda))
		return PTR_ERR(qmp->vdda);

	ret = qmp_pcie_msm8994_register_pipe(qmp);
	if (ret)
		return ret;

	phy = devm_phy_create(&pdev->dev, NULL, &qmp_pcie_msm8994_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	phy_set_drvdata(phy, qmp);

	provider = devm_of_phy_provider_register(&pdev->dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id qmp_pcie_msm8994_of_match[] = {
	{ .compatible = "qcom,msm8994-qmp-pcie-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, qmp_pcie_msm8994_of_match);

static struct platform_driver qmp_pcie_msm8994_driver = {
	.probe	= qmp_pcie_msm8994_probe,
	.driver	= {
		.name = "qcom-qmp-msm8994-pcie-phy",
		.of_match_table = qmp_pcie_msm8994_of_match,
	},
};
module_platform_driver(qmp_pcie_msm8994_driver);

MODULE_DESCRIPTION("Qualcomm MSM8994 20nm QMP PCIe PHY");
MODULE_LICENSE("GPL");
