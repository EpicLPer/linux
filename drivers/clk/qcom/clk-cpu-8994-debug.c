// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only dump of MSM8994 A53/A57 cluster PLLs.
 * Downstream clock-cpu-8994.c programs these; mainline has no CPU clock
 * driver yet. Do not writel — wrong mux/PLL sequence hangs the 810.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define PLL_MODE	0x00
#define PLL_L		0x04
#define PLL_ALPHA	0x08
#define PLL_USER	0x10
#define PLL_CONFIG	0x14
#define PLL_STATUS	0x1c
#define PLL1_OFF	0x40

#define XO_HZ		19200000ULL

static unsigned long pll_hz(u32 l, u32 alpha)
{
	/* Integer alpha-PLL: XO * L when ALPHA is 0. */
	if (alpha)
		return 0;
	return (unsigned long)(XO_HZ * (l & 0xff));
}

static void dump_pll(struct device *dev, const char *name, void __iomem *base)
{
	u32 mode = readl_relaxed(base + PLL_MODE);
	u32 l = readl_relaxed(base + PLL_L);
	u32 alpha = readl_relaxed(base + PLL_ALPHA);
	u32 user = readl_relaxed(base + PLL_USER);
	u32 cfg = readl_relaxed(base + PLL_CONFIG);
	u32 st = readl_relaxed(base + PLL_STATUS);
	unsigned long hz = pll_hz(l, alpha);

	dev_info(dev, "%s mode=0x%08x L=%u alpha=0x%x user=0x%x cfg=0x%x status=0x%x%s%lu%s\n",
		 name, mode, l & 0xff, alpha, user, cfg, st,
		 hz ? " ~" : " (alpha-frac, raw L only) ",
		 hz ? hz : (unsigned long)(l & 0xff),
		 hz ? " Hz" : "");
}

static int cpu_8994_debug_probe(struct platform_device *pdev)
{
	void __iomem *c0, *c1;

	c0 = devm_platform_ioremap_resource_byname(pdev, "c0_pll");
	if (IS_ERR(c0))
		return PTR_ERR(c0);
	c1 = devm_platform_ioremap_resource_byname(pdev, "c1_pll");
	if (IS_ERR(c1))
		return PTR_ERR(c1);

	dev_info(&pdev->dev, "read-only PLL dump (no set_rate)\n");
	dump_pll(&pdev->dev, "a53.pll0", c0);
	dump_pll(&pdev->dev, "a53.pll1", c0 + PLL1_OFF);
	dump_pll(&pdev->dev, "a57.pll0", c1);
	dump_pll(&pdev->dev, "a57.pll1", c1 + PLL1_OFF);
	return 0;
}

static const struct of_device_id cpu_8994_debug_match[] = {
	{ .compatible = "qcom,cpu-clock-8994-debug" },
	{ }
};
MODULE_DEVICE_TABLE(of, cpu_8994_debug_match);

static struct platform_driver cpu_8994_debug_driver = {
	.probe = cpu_8994_debug_probe,
	.driver = {
		.name = "clk-cpu-8994-debug",
		.of_match_table = cpu_8994_debug_match,
	},
};
module_platform_driver(cpu_8994_debug_driver);

MODULE_DESCRIPTION("MSM8994 CPU PLL read-only dump");
MODULE_LICENSE("GPL");
