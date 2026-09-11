// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Stefan <EpicLPer@users.noreply.github.com>
 *
 * MSM8994 / MSM8992 v2 CPU clocks. Mux +0x54: LF [2:1], HF [4:3].
 * A53 mux is the APCS syscon; do not map 0xf900d000 again.
 * v2 ping-pong: program the idle PLL, then switch HF.
 * vdd-dig is MSM8994_VDDMX_AO. Wire = 3.10 KEY_CORNER enum − 1
 * (LOW 3, NOM 4, SUPER_TURBO 6). Do not send 7.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/qcom,msm8994-cpu.h>

#include "clk-regmap.h"
#include "clk-regmap-mux.h"
#include "common.h"

#define PLL_MODE		0x00
#define PLL_L_VAL		0x04
#define PLL_USER_CTL		0x10
#define PLL_CONFIG_CTL		0x14
#define PLL_TEST_CTL_LO		0x20
#define PLL_TEST_CTL_HI		0x24
#define PLL_POSTDIV_SHIFT	8
#define PLL_POSTDIV_MASK	0x3
#define PLL_OUTCTRL		BIT(0)
#define PLL_BYPASSNL		BIT(1)
#define PLL_RESET_N		BIT(2)
#define PLL_MODE_MASK		0xf
#define PLL_LOCK		BIT(31)
#define PLL_ON_MASK		(PLL_BYPASSNL | PLL_RESET_N | PLL_OUTCTRL)
#define PLL_XO_RATE		19200000UL
#define PLL_MIN_RATE		1209600000UL
#define PLL_MAX_RATE		1996800000UL
#define PLL_DIG_LOW_FMAX	1593600000UL
#define PLL_DIG_CORNER_LOW	3
#define PLL_DIG_CORNER_NOM	4
#define CPR_MX_NORMAL		4
#define CPR_MX_SUPER_TURBO	6
#define A53_CPR_MX_NORMAL	600000000UL
#define A53_CPR_MX_TURBO	864000000UL
#define A57_CPR_MX_NORMAL	768000000UL
#define A57_CPR_MX_TURBO	1248000000UL
#define A57_BIMC_HIGH		1824000000UL
#define BIMC_A_HIGH_RATE	1555200000UL
#define PLL_MIN_L		63
#define PLL_MAX_L		104
#define PLL_V2_USER_CTL		0x109
#define PLL_V2_CONFIG_CTL	0x004D6968
#define PLL_V2_TEST_CTL_LO	0x80000000
#define PLL_V2_TEST_CTL_HI	0x1
#define PLL1_OFFSET		0x40

#define MUX_OFFSET		0x54
#define LF_MUX_SHIFT		1
#define HF_MUX_SHIFT		3
#define MUX_WIDTH		2
#define ACC_TYPE1		0x84
#define ACC_TYPE2		0x88
#define ACC_TYPE3		0x8c
#define ACC_TYPE4		0x90
#define ACC_A57_LOW		0x02
#define ACC_A57_HIGH		0x06
#define ACC_A57_TYPE4		0x0b

static DEFINE_SPINLOCK(cpu_8994_mux_lock);
static phys_addr_t a57_mux_phys;
static struct clk_regmap_mux a53_hf_mux;
static struct clk_regmap_mux a57_hf_mux;
static struct device *cpu_8994_dev;
static struct clk *cpu_8994_bimc;
static unsigned long a53_cpu_rate;
static unsigned long a57_cpu_rate;
static unsigned int last_dig_corner;
static bool bimc_high;

static inline struct clk_regmap_mux *to_clk_regmap_mux(struct clk_hw *hw)
{
	return container_of(to_clk_regmap(hw), struct clk_regmap_mux, clkr);
}

struct clk_cpu_8994_pll {
	struct clk_hw pll_hw;
	struct clk_hw div_hw;
	struct clk_init_data pll_init;
	struct clk_init_data div_init;
	const struct clk_hw *div_parent[1];
	struct clk_regmap_mux *hf;
	struct clk_regmap_mux *lf;
	void __iomem *base;
	spinlock_t lock;
	bool has_div;
};

#define to_clk_cpu_8994_pll(_hw) \
	container_of(_hw, struct clk_cpu_8994_pll, pll_hw)
#define to_clk_cpu_8994_pll_div(_hw) \
	container_of(_hw, struct clk_cpu_8994_pll, div_hw)

static struct clk_cpu_8994_pll *cpu_8994_plls[4];

static bool clk_cpu_8994_pll_enabled(struct clk_cpu_8994_pll *pll)
{
	u32 mode = readl_relaxed(pll->base + PLL_MODE);

	return (mode & PLL_ON_MASK) == PLL_ON_MASK;
}

static bool clk_cpu_8994_pll_in_use(struct clk_cpu_8994_pll *pll)
{
	struct clk_hw *hf_parent, *lf_parent;

	if (!pll->hf)
		return false;

	hf_parent = clk_hw_get_parent(&pll->hf->clkr.hw);
	if (hf_parent == &pll->pll_hw)
		return true;

	if (!pll->has_div || !pll->lf || hf_parent != &pll->lf->clkr.hw)
		return false;

	lf_parent = clk_hw_get_parent(&pll->lf->clkr.hw);
	return lf_parent == &pll->div_hw;
}

static void clk_cpu_8994_pll_disable(struct clk_cpu_8994_pll *pll)
{
	u32 mode = readl_relaxed(pll->base + PLL_MODE);

	mode &= ~PLL_MODE_MASK;
	writel_relaxed(mode, pll->base + PLL_MODE);
}

static int clk_cpu_8994_pll_enable(struct clk_cpu_8994_pll *pll)
{
	u32 mode, testlo;
	int i;

	mode = readl_relaxed(pll->base + PLL_MODE);

	mode |= BIT(3);
	writel_relaxed(mode, pll->base + PLL_MODE);

	testlo = readl_relaxed(pll->base + PLL_TEST_CTL_LO);
	testlo &= ~GENMASK(7, 6);
	testlo |= 0xc0;
	writel_relaxed(testlo, pll->base + PLL_TEST_CTL_LO);
	mb();

	mode |= PLL_BYPASSNL;
	writel_relaxed(mode, pll->base + PLL_MODE);
	mb();
	udelay(10);

	mode |= PLL_RESET_N;
	writel_relaxed(mode, pll->base + PLL_MODE);
	mb();
	udelay(200);

	for (i = 0; i < 1000; i++) {
		if (readl_relaxed(pll->base + PLL_MODE) & PLL_LOCK) {
			udelay(1);
			if (readl_relaxed(pll->base + PLL_MODE) & PLL_LOCK)
				break;
		}
		udelay(1);
	}

	if (!(readl_relaxed(pll->base + PLL_MODE) & PLL_LOCK))
		return -ETIMEDOUT;

	mode = readl_relaxed(pll->base + PLL_MODE);
	mode |= PLL_OUTCTRL;
	writel_relaxed(mode, pll->base + PLL_MODE);
	mb();
	return 0;
}

static unsigned long clk_cpu_8994_pll_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct clk_cpu_8994_pll *pll = to_clk_cpu_8994_pll(hw);
	u32 l = readl_relaxed(pll->base + PLL_L_VAL) & 0xff;

	return parent_rate * l;
}

static int clk_cpu_8994_pll_determine_rate(struct clk_hw *hw,
					   struct clk_rate_request *req)
{
	struct clk_cpu_8994_pll *pll = to_clk_cpu_8994_pll(hw);
	unsigned long parent = req->best_parent_rate;
	unsigned long rrate;

	if (!parent)
		return -EINVAL;

	if (clk_cpu_8994_pll_in_use(pll)) {
		req->rate = parent * (readl_relaxed(pll->base + PLL_L_VAL) & 0xff);
		return 0;
	}

	req->rate = clamp(req->rate, PLL_MIN_RATE, PLL_MAX_RATE);
	rrate = DIV_ROUND_UP(req->rate, parent) * parent;
	if (rrate > PLL_MAX_RATE)
		rrate -= parent;

	req->rate = rrate;
	return 0;
}

static unsigned long clk_cpu_8994_pll_l_rate(void __iomem *pll)
{
	return PLL_XO_RATE * (readl_relaxed(pll + PLL_L_VAL) & 0xff);
}

static unsigned long clk_cpu_8994_pll_div_rate(void __iomem *pll)
{
	u32 post = (readl_relaxed(pll + PLL_USER_CTL) >> PLL_POSTDIV_SHIFT) &
		   PLL_POSTDIV_MASK;
	unsigned int div = (post == 1) ? 2 : (post == 3) ? 4 : 1;

	return clk_cpu_8994_pll_l_rate(pll) / div;
}

static unsigned long clk_cpu_8994_rate_from_mux(void __iomem *pll_base, u32 mux,
						unsigned long aux_rate)
{
	u32 hf = (mux >> HF_MUX_SHIFT) & GENMASK(MUX_WIDTH - 1, 0);
	u32 lf = (mux >> LF_MUX_SHIFT) & GENMASK(MUX_WIDTH - 1, 0);

	if (hf == 1)
		return clk_cpu_8994_pll_l_rate(pll_base + PLL1_OFFSET);
	if (hf == 3)
		return clk_cpu_8994_pll_l_rate(pll_base);

	if (lf == 0)
		return PLL_XO_RATE;
	if (lf == 1)
		return clk_cpu_8994_pll_div_rate(pll_base + PLL1_OFFSET);
	if (lf == 2)
		return clk_cpu_8994_pll_div_rate(pll_base);
	if (lf == 3)
		return aux_rate;

	return 0;
}

static unsigned int clk_cpu_8994_mx_for_cpu(unsigned long a53,
					    unsigned long a57)
{
	if (a53 >= A53_CPR_MX_TURBO || a57 >= A57_CPR_MX_TURBO)
		return CPR_MX_SUPER_TURBO;
	if (a53 >= A53_CPR_MX_NORMAL || a57 >= A57_CPR_MX_NORMAL)
		return CPR_MX_NORMAL;
	return PLL_DIG_CORNER_LOW;
}

static unsigned int clk_cpu_8994_pll_corner(unsigned long pll_hint)
{
	unsigned long pll_max = pll_hint;
	int i;

	for (i = 0; i < ARRAY_SIZE(cpu_8994_plls); i++) {
		struct clk_cpu_8994_pll *p = cpu_8994_plls[i];
		unsigned long r;

		if (!p || !p->base || !clk_cpu_8994_pll_enabled(p))
			continue;
		r = clk_cpu_8994_pll_l_rate(p->base);
		if (r > pll_max)
			pll_max = r;
	}

	return pll_max > PLL_DIG_LOW_FMAX ? PLL_DIG_CORNER_NOM :
					    PLL_DIG_CORNER_LOW;
}

static int clk_cpu_8994_vote_mx(unsigned long pll_hint)
{
	unsigned int corner;
	int ret;

	if (!cpu_8994_dev)
		return -ENODEV;

	corner = max(clk_cpu_8994_mx_for_cpu(a53_cpu_rate, a57_cpu_rate),
		     clk_cpu_8994_pll_corner(pll_hint));

	ret = dev_pm_genpd_set_performance_state(cpu_8994_dev, corner);
	if (ret)
		return ret;

	if (corner != last_dig_corner) {
		pr_debug("clk-cpu-8994: vdd-mx corner %u a53=%lu a57=%lu\n",
			 corner, a53_cpu_rate, a57_cpu_rate);
		last_dig_corner = corner;
	}
	return 0;
}

/*
 * 3.10 cpubw 11863 = 1555 MHz bimc_a at A57 1824/1958.4.
 * Vote before the mux rises. Do not vote AMPSS→EBI through ICC.
 */
static int clk_cpu_8994_vote_bimc(unsigned long a57_rate)
{
	int ret;

	if (!cpu_8994_bimc || bimc_high || a57_rate < A57_BIMC_HIGH)
		return 0;

	ret = clk_set_rate(cpu_8994_bimc, BIMC_A_HIGH_RATE);
	if (ret)
		return ret;
	ret = clk_prepare_enable(cpu_8994_bimc);
	if (ret)
		return ret;
	bimc_high = true;
	pr_debug("clk-cpu-8994: bimc_a %lu Hz a57=%lu\n",
		 BIMC_A_HIGH_RATE, a57_rate);
	return 0;
}

static int clk_cpu_8994_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	struct clk_cpu_8994_pll *pll = to_clk_cpu_8994_pll(hw);
	unsigned long flags;
	u32 l_val, old_l;
	int ret;

	if (!parent_rate)
		return -EINVAL;

	l_val = rate / parent_rate;
	if (l_val < PLL_MIN_L || l_val > PLL_MAX_L)
		return -EINVAL;

	old_l = readl_relaxed(pll->base + PLL_L_VAL) & 0xff;
	if (clk_cpu_8994_pll_in_use(pll) && old_l != l_val)
		return -EBUSY;

	ret = clk_cpu_8994_vote_mx(rate);
	if (ret)
		return ret;

	if (old_l == l_val && clk_cpu_8994_pll_enabled(pll))
		return 0;

	spin_lock_irqsave(&pll->lock, flags);
	clk_cpu_8994_pll_disable(pll);
	writel_relaxed(l_val, pll->base + PLL_L_VAL);
	ret = clk_cpu_8994_pll_enable(pll);
	if (ret) {
		writel_relaxed(old_l, pll->base + PLL_L_VAL);
		if (old_l)
			clk_cpu_8994_pll_enable(pll);
	}
	spin_unlock_irqrestore(&pll->lock, flags);

	return ret;
}

static const struct clk_ops clk_cpu_8994_pll_ops = {
	.recalc_rate = clk_cpu_8994_pll_recalc_rate,
	.determine_rate = clk_cpu_8994_pll_determine_rate,
	.set_rate = clk_cpu_8994_pll_set_rate,
};

static u32 clk_cpu_8994_pll_div_get(struct clk_cpu_8994_pll *pll)
{
	u32 post = (readl_relaxed(pll->base + PLL_USER_CTL) >>
		    PLL_POSTDIV_SHIFT) & PLL_POSTDIV_MASK;

	if (post == 1)
		return 2;
	if (post == 3)
		return 4;
	return 1;
}

static unsigned long clk_cpu_8994_pll_div_recalc_rate(struct clk_hw *hw,
						      unsigned long parent_rate)
{
	struct clk_cpu_8994_pll *pll = to_clk_cpu_8994_pll_div(hw);

	return parent_rate / clk_cpu_8994_pll_div_get(pll);
}

static int clk_cpu_8994_pll_div_determine_rate(struct clk_hw *hw,
					       struct clk_rate_request *req)
{
	struct clk_cpu_8994_pll *pll = to_clk_cpu_8994_pll_div(hw);
	struct clk_hw *parent = clk_hw_get_parent(hw);
	u32 div = clk_cpu_8994_pll_div_get(pll);
	unsigned long pr;

	if (!parent || !div)
		return -EINVAL;

	pr = clk_hw_round_rate(parent, req->rate * div);
	req->best_parent_hw = parent;
	req->best_parent_rate = pr;
	req->rate = pr / div;
	return 0;
}

static const struct clk_ops clk_cpu_8994_pll_div_ops = {
	.recalc_rate = clk_cpu_8994_pll_div_recalc_rate,
	.determine_rate = clk_cpu_8994_pll_div_determine_rate,
};

static u8 clk_cpu_8994_mux_get_parent(struct clk_hw *hw)
{
	struct clk_regmap_mux *mux = to_clk_regmap_mux(hw);
	struct clk_regmap *clkr = to_clk_regmap(hw);
	unsigned int val;

	regmap_read(clkr->regmap, mux->reg, &val);
	val = (val >> mux->shift) & GENMASK(mux->width - 1, 0);

	if (mux->parent_map)
		return qcom_find_cfg_index(hw, mux->parent_map, val);

	return val;
}

static int clk_cpu_8994_a57_acc(bool pll_early)
{
	u32 t1_3 = pll_early ? ACC_A57_HIGH : ACC_A57_LOW;
	int ret;

	ret = qcom_scm_io_writel(a57_mux_phys + ACC_TYPE1, t1_3);
	if (ret)
		return ret;
	ret = qcom_scm_io_writel(a57_mux_phys + ACC_TYPE2, t1_3);
	if (ret)
		return ret;
	ret = qcom_scm_io_writel(a57_mux_phys + ACC_TYPE3, t1_3);
	if (ret)
		return ret;
	return qcom_scm_io_writel(a57_mux_phys + ACC_TYPE4, ACC_A57_TYPE4);
}

static int clk_cpu_8994_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_regmap_mux *mux = to_clk_regmap_mux(hw);
	struct clk_regmap *clkr = to_clk_regmap(hw);
	unsigned int mask = GENMASK(mux->width + mux->shift - 1, mux->shift);
	unsigned long flags, new_rate = 0;
	unsigned long *cluster_rate = NULL;
	bool a57_hf = hw == &a57_hf_mux.clkr.hw;
	bool pll_early;
	struct clk_hw *parent;
	u8 clk_index = index;
	int ret;

	parent = clk_hw_get_parent_by_index(hw, clk_index);
	if (parent) {
		new_rate = clk_hw_get_rate(parent);
		if (hw == &a53_hf_mux.clkr.hw)
			cluster_rate = &a53_cpu_rate;
		else if (hw == &a57_hf_mux.clkr.hw)
			cluster_rate = &a57_cpu_rate;

		if (a57_hf) {
			ret = clk_cpu_8994_vote_bimc(new_rate);
			if (ret)
				return ret;
		}

		if (cluster_rate && new_rate > *cluster_rate) {
			*cluster_rate = new_rate;
			ret = clk_cpu_8994_vote_mx(0);
			if (ret)
				return ret;
		}
	}

	if (mux->parent_map)
		index = mux->parent_map[index].cfg;

	pll_early = a57_hf && index != 0;

	if (pll_early) {
		ret = clk_cpu_8994_a57_acc(true);
		if (ret)
			return ret;
	}

	spin_lock_irqsave(&cpu_8994_mux_lock, flags);
	ret = regmap_update_bits(clkr->regmap, mux->reg, mask,
				 index << mux->shift);
	spin_unlock_irqrestore(&cpu_8994_mux_lock, flags);
	if (ret)
		return ret;

	if (a57_hf && !pll_early) {
		ret = clk_cpu_8994_a57_acc(false);
		if (ret)
			return ret;
	}

	mb();
	udelay(5);

	if (cluster_rate && new_rate && new_rate < *cluster_rate) {
		*cluster_rate = new_rate;
		ret = clk_cpu_8994_vote_mx(0);
		if (ret)
			return ret;
	}

	return 0;
}

static int clk_cpu_8994_mux_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	unsigned long *cluster_rate = NULL;
	int ret;

	if (hw == &a53_hf_mux.clkr.hw)
		cluster_rate = &a53_cpu_rate;
	else if (hw == &a57_hf_mux.clkr.hw)
		cluster_rate = &a57_cpu_rate;
	else
		return 0;

	if (hw == &a57_hf_mux.clkr.hw) {
		ret = clk_cpu_8994_vote_bimc(rate);
		if (ret)
			return ret;
	}

	*cluster_rate = rate;
	return clk_cpu_8994_vote_mx(0);
}

static int clk_cpu_8994_mux_determine_rate(struct clk_hw *hw,
					   struct clk_rate_request *req)
{
	struct clk_hw *parent, *cur = clk_hw_get_parent(hw), *best = NULL;
	unsigned long best_rate = 0;
	int i, num = clk_hw_get_num_parents(hw);

	for (i = 0; i < num; i++) {
		unsigned long pr;

		parent = clk_hw_get_parent_by_index(hw, i);
		if (!parent)
			continue;

		pr = clk_hw_round_rate(parent, req->rate);
		if (pr > req->rate)
			continue;
		if (pr > best_rate || (pr == best_rate && parent == cur)) {
			best_rate = pr;
			best = parent;
		}
	}

	if (!best)
		return -EINVAL;

	req->best_parent_hw = best;
	req->best_parent_rate = best_rate;
	req->rate = best_rate;
	return 0;
}

static const struct clk_ops clk_cpu_8994_mux_ops = {
	.get_parent = clk_cpu_8994_mux_get_parent,
	.set_parent = clk_cpu_8994_mux_set_parent,
	.set_rate = clk_cpu_8994_mux_set_rate,
	.determine_rate = clk_cpu_8994_mux_determine_rate,
};

static const struct clk_parent_data xo_parent[] = {
	{ .fw_name = "xo" },
};

static const struct parent_map lf_parent_map[] = {
	{ .cfg = 0 },
	{ .cfg = 1 },
	{ .cfg = 2 },
	{ .cfg = 3 },
};

static const struct parent_map hf_parent_map[] = {
	{ .cfg = 0 },
	{ .cfg = 1 },
	{ .cfg = 3 },
};

static struct clk_cpu_8994_pll a53_pll0;
static struct clk_cpu_8994_pll a53_pll1;
static struct clk_cpu_8994_pll a57_pll0;
static struct clk_cpu_8994_pll a57_pll1;

static const struct clk_parent_data a53_lf_parents[] = {
	{ .fw_name = "xo" },
	{ .hw = &a53_pll1.div_hw },
	{ .hw = &a53_pll0.div_hw },
	{ .fw_name = "aux" },
};

static const struct clk_parent_data a57_lf_parents[] = {
	{ .fw_name = "xo" },
	{ .hw = &a57_pll1.div_hw },
	{ .hw = &a57_pll0.div_hw },
	{ .fw_name = "aux" },
};

static struct clk_regmap_mux a53_lf_mux = {
	.reg = MUX_OFFSET,
	.shift = LF_MUX_SHIFT,
	.width = MUX_WIDTH,
	.parent_map = lf_parent_map,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "a53_lf_mux",
		.parent_data = a53_lf_parents,
		.num_parents = ARRAY_SIZE(a53_lf_parents),
		.ops = &clk_cpu_8994_mux_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap_mux a57_lf_mux = {
	.reg = MUX_OFFSET,
	.shift = LF_MUX_SHIFT,
	.width = MUX_WIDTH,
	.parent_map = lf_parent_map,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "a57_lf_mux",
		.parent_data = a57_lf_parents,
		.num_parents = ARRAY_SIZE(a57_lf_parents),
		.ops = &clk_cpu_8994_mux_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data a53_hf_parents[] = {
	{ .hw = &a53_lf_mux.clkr.hw },
	{ .hw = &a53_pll1.pll_hw },
	{ .hw = &a53_pll0.pll_hw },
};

static const struct clk_parent_data a57_hf_parents[] = {
	{ .hw = &a57_lf_mux.clkr.hw },
	{ .hw = &a57_pll1.pll_hw },
	{ .hw = &a57_pll0.pll_hw },
};

static struct clk_regmap_mux a53_hf_mux = {
	.reg = MUX_OFFSET,
	.shift = HF_MUX_SHIFT,
	.width = MUX_WIDTH,
	.parent_map = hf_parent_map,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "a53_clk",
		.parent_data = a53_hf_parents,
		.num_parents = ARRAY_SIZE(a53_hf_parents),
		.ops = &clk_cpu_8994_mux_ops,
		.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
	},
};

static struct clk_regmap_mux a57_hf_mux = {
	.reg = MUX_OFFSET,
	.shift = HF_MUX_SHIFT,
	.width = MUX_WIDTH,
	.parent_map = hf_parent_map,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "a57_clk",
		.parent_data = a57_hf_parents,
		.num_parents = ARRAY_SIZE(a57_hf_parents),
		.ops = &clk_cpu_8994_mux_ops,
		.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
	},
};

static void clk_cpu_8994_pll1_init_hw(struct clk_cpu_8994_pll *pll)
{
	if (clk_cpu_8994_pll_enabled(pll))
		return;

	writel_relaxed(PLL_V2_USER_CTL, pll->base + PLL_USER_CTL);
	writel_relaxed(PLL_V2_CONFIG_CTL, pll->base + PLL_CONFIG_CTL);
	writel_relaxed(PLL_V2_TEST_CTL_LO, pll->base + PLL_TEST_CTL_LO);
	writel_relaxed(PLL_V2_TEST_CTL_HI, pll->base + PLL_TEST_CTL_HI);
	mb();
}

static int clk_cpu_8994_init_pll(struct device *dev, struct clk_cpu_8994_pll *pll,
				 void __iomem *base, const char *pll_name,
				 const char *div_name)
{
	int ret;

	pll->base = base;
	spin_lock_init(&pll->lock);
	pll->pll_init = (struct clk_init_data){
		.name = pll_name,
		.parent_data = xo_parent,
		.num_parents = ARRAY_SIZE(xo_parent),
		.ops = &clk_cpu_8994_pll_ops,
	};
	pll->pll_hw.init = &pll->pll_init;

	ret = devm_clk_hw_register(dev, &pll->pll_hw);
	if (ret)
		return ret;

	if (!div_name)
		return 0;

	pll->has_div = true;
	pll->div_parent[0] = &pll->pll_hw;
	pll->div_init = (struct clk_init_data){
		.name = div_name,
		.parent_hws = pll->div_parent,
		.num_parents = 1,
		.ops = &clk_cpu_8994_pll_div_ops,
		.flags = CLK_SET_RATE_PARENT,
	};
	pll->div_hw.init = &pll->div_init;

	return devm_clk_hw_register(dev, &pll->div_hw);
}

static const struct regmap_config c1_mux_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = MUX_OFFSET,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.fast_io = true,
};

static int clk_cpu_8994_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct regmap *apcs, *c1_map;
	void __iomem *c0_pll, *c1_pll, *c1_mux;
	int ret;

	data = devm_kzalloc(dev, struct_size(data, hws, 2), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	c0_pll = devm_platform_ioremap_resource_byname(pdev, "c0-pll");
	if (IS_ERR(c0_pll))
		return PTR_ERR(c0_pll);

	c1_pll = devm_platform_ioremap_resource_byname(pdev, "c1-pll");
	if (IS_ERR(c1_pll))
		return PTR_ERR(c1_pll);

	c1_mux = devm_platform_ioremap_resource_byname(pdev, "c1-mux");
	if (IS_ERR(c1_mux))
		return PTR_ERR(c1_mux);

	{
		struct resource *res;

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   "c1-mux");
		if (!res)
			return -EINVAL;
		a57_mux_phys = res->start;
	}

	if (!dev->pm_domain)
		return dev_err_probe(dev, -ENODEV, "vdd-dig mx ao\n");

	apcs = syscon_regmap_lookup_by_phandle(dev->of_node, "qcom,apcs");
	if (IS_ERR(apcs))
		return dev_err_probe(dev, PTR_ERR(apcs), "apcs syscon\n");

	{
		struct clk *aux;
		unsigned long aux_rate;
		unsigned int a53_mux, a57_mux;

		aux = devm_clk_get(dev, "aux");
		if (IS_ERR(aux))
			return dev_err_probe(dev, PTR_ERR(aux), "aux clk\n");
		aux_rate = clk_get_rate(aux);

		cpu_8994_bimc = devm_clk_get_optional(dev, "bimc");
		if (IS_ERR(cpu_8994_bimc))
			return dev_err_probe(dev, PTR_ERR(cpu_8994_bimc),
					     "bimc clk\n");

		ret = regmap_read(apcs, MUX_OFFSET, &a53_mux);
		if (ret)
			return ret;
		a57_mux = readl_relaxed(c1_mux + MUX_OFFSET);

		a53_cpu_rate = clk_cpu_8994_rate_from_mux(c0_pll, a53_mux,
							  aux_rate);
		a57_cpu_rate = clk_cpu_8994_rate_from_mux(c1_pll, a57_mux,
							  aux_rate);
		if (!a53_cpu_rate || !a57_cpu_rate)
			return dev_err_probe(dev, -EINVAL, "cpu mux rate\n");
	}

	a53_pll0.base = c0_pll;
	a53_pll1.base = c0_pll + PLL1_OFFSET;
	a57_pll0.base = c1_pll;
	a57_pll1.base = c1_pll + PLL1_OFFSET;
	cpu_8994_plls[0] = &a53_pll0;
	cpu_8994_plls[1] = &a53_pll1;
	cpu_8994_plls[2] = &a57_pll0;
	cpu_8994_plls[3] = &a57_pll1;

	cpu_8994_dev = dev;
	pm_runtime_enable(dev);
	/* Vote MX before AO resume; corner 0 on resume drops S2. */
	ret = clk_cpu_8994_vote_mx(0);
	if (ret)
		return dev_err_probe(dev, ret, "vdd-mx vote\n");
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "vdd-dig mx ao resume\n");

	c1_map = devm_regmap_init_mmio(dev, c1_mux, &c1_mux_regmap_config);
	if (IS_ERR(c1_map))
		return PTR_ERR(c1_map);

	a53_pll0.hf = &a53_hf_mux;
	a53_pll0.lf = &a53_lf_mux;
	a53_pll1.hf = &a53_hf_mux;
	a53_pll1.lf = &a53_lf_mux;
	a57_pll0.hf = &a57_hf_mux;
	a57_pll0.lf = &a57_lf_mux;
	a57_pll1.hf = &a57_hf_mux;
	a57_pll1.lf = &a57_lf_mux;

	ret = clk_cpu_8994_init_pll(dev, &a53_pll0, c0_pll, "a53_pll0",
				    "a53_pll0_div");
	if (ret)
		return ret;

	ret = clk_cpu_8994_init_pll(dev, &a53_pll1, c0_pll + PLL1_OFFSET,
				    "a53_pll1", "a53_pll1_div");
	if (ret)
		return ret;

	ret = clk_cpu_8994_init_pll(dev, &a57_pll0, c1_pll, "a57_pll0",
				    "a57_pll0_div");
	if (ret)
		return ret;

	ret = clk_cpu_8994_init_pll(dev, &a57_pll1, c1_pll + PLL1_OFFSET,
				    "a57_pll1", "a57_pll1_div");
	if (ret)
		return ret;

	clk_cpu_8994_pll1_init_hw(&a53_pll1);
	clk_cpu_8994_pll1_init_hw(&a57_pll1);

	a53_lf_mux.clkr.regmap = apcs;
	a53_hf_mux.clkr.regmap = apcs;
	a57_lf_mux.clkr.regmap = c1_map;
	a57_hf_mux.clkr.regmap = c1_map;

	ret = devm_clk_hw_register(dev, &a53_lf_mux.clkr.hw);
	if (ret)
		return ret;
	ret = devm_clk_hw_register(dev, &a53_hf_mux.clkr.hw);
	if (ret)
		return ret;
	ret = devm_clk_hw_register(dev, &a57_lf_mux.clkr.hw);
	if (ret)
		return ret;
	ret = devm_clk_hw_register(dev, &a57_hf_mux.clkr.hw);
	if (ret)
		return ret;

	data->num = 2;
	data->hws[APCS_CPU_A53_CLK] = &a53_hf_mux.clkr.hw;
	data->hws[APCS_CPU_A57_CLK] = &a57_hf_mux.clkr.hw;

	ret = clk_cpu_8994_vote_mx(0);
	if (ret)
		return ret;

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (ret)
		return ret;

	/* 3.10 v2 probe parks A53 600 MHz / A57 384 MHz until cpufreq. */
	ret = clk_set_rate(a53_hf_mux.clkr.hw.clk, 600000000);
	if (ret)
		return dev_err_probe(dev, ret, "a53 safe rate\n");
	ret = clk_set_rate(a57_hf_mux.clkr.hw.clk, 384000000);
	if (ret)
		return dev_err_probe(dev, ret, "a57 safe rate\n");

	return 0;
}

static const struct of_device_id clk_cpu_8994_match_table[] = {
	{ .compatible = "qcom,msm8994-cpu-clk" },
	{ .compatible = "qcom,msm8992-cpu-clk" },
	{ }
};
MODULE_DEVICE_TABLE(of, clk_cpu_8994_match_table);

static struct platform_driver clk_cpu_8994_driver = {
	.probe = clk_cpu_8994_probe,
	.driver = {
		.name = "qcom-cpu-clk-8994",
		.of_match_table = clk_cpu_8994_match_table,
	},
};
module_platform_driver(clk_cpu_8994_driver);

MODULE_DESCRIPTION("Qualcomm MSM8994/MSM8992 CPU clock driver");
MODULE_LICENSE("GPL");
