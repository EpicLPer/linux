// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/delay.h>
#include <linux/interconnect.h>
#include <linux/iopoll.h>
#include <linux/of_irq.h>
#include <linux/firmware/qcom/qcom_scm.h>

#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_vblank.h>

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_mmu.h"
#include "mdp5_kms.h"

void qcom_iommu_restore_ctx_after_pc(struct device *master);

/*
 * 3.10 SEC_DEVICE_MDSS. GDSC notifier and overlay_start both
 * scm_restore_sec_cfg before programming VBIF / mdp-settings.
 */
#define MDP5_SCM_MDSS_DEV_ID	1

/*
 * 3.10 msm8994-mdss.dtsi xin-id + vbif-qos-rt-setting. Written by
 * mdss_mdp_qos_vbif_remapper_setup on every pipe; mdp5 never did.
 */
static const u8 msm8994_vbif_rt_xins[] = {
	0, 1, 2, 4, 5, 7, 8, 9, 10, 12, 13
};
static const u8 msm8994_vbif_rt_qos[] = { 1, 2, 2, 2 };

/* 3.10 msm8994-mdss.dtsi pipe-dma-xin-id / clk-ctrl-offsets. */
#define MMSS_VBIF_XIN_HALT_CTRL0	0x200
#define MMSS_VBIF_XIN_HALT_CTRL1	0x204
#define XIN_HALT_TIMEOUT_US		0x4000
#define MDP5_SSPP_CLK_CTRL0		0x2ac
#define MDP5_SSPP_CLK_CTRL1		0x2b4
#define MDP5_DMA_CLK_FORCE_ON		BIT(8)

static void mdp5_vbif_xin_halt_cycle(struct mdp5_kms *mdp5_kms, u32 xin_id)
{
	u32 val, status;
	int rc;

	val = readl_relaxed(mdp5_kms->vbif + MMSS_VBIF_XIN_HALT_CTRL0);
	writel_relaxed(val | BIT(xin_id),
		       mdp5_kms->vbif + MMSS_VBIF_XIN_HALT_CTRL0);
	wmb();
	rc = readl_poll_timeout(mdp5_kms->vbif + MMSS_VBIF_XIN_HALT_CTRL1,
				status, status & BIT(xin_id), 1000,
				XIN_HALT_TIMEOUT_US);
	if (rc)
		pr_info("talkman-mdss: xin %u halt timeout st=%08x\n",
			xin_id,
			readl_relaxed(mdp5_kms->vbif + MMSS_VBIF_XIN_HALT_CTRL1));
	val = readl_relaxed(mdp5_kms->vbif + MMSS_VBIF_XIN_HALT_CTRL0);
	writel_relaxed(val & ~BIT(xin_id),
		       mdp5_kms->vbif + MMSS_VBIF_XIN_HALT_CTRL0);
}

static void mdp5_restore_vbif_qos(struct mdp5_kms *mdp5_kms)
{
	unsigned int i, x;
	u32 val, mask, qos, clk0, clk1;

	if (!mdp5_kms->vbif)
		return;

	/*
	 * 8974 mdss_hw_init writes VBIF:0x004 = 1 (CLKON). 8994 DT
	 * has no vbif-settings; after GDSC AXI gating can drop OT
	 * writes on the fetch path while APB still reads them back.
	 */
	writel_relaxed(1, mdp5_kms->vbif + 0x004);

	for (i = 0; i < ARRAY_SIZE(msm8994_vbif_rt_qos); i++) {
		val = readl_relaxed(mdp5_kms->vbif + 0x20 + i * 4);
		qos = msm8994_vbif_rt_qos[i];
		for (x = 0; x < ARRAY_SIZE(msm8994_vbif_rt_xins); x++) {
			mask = 0x3u << (msm8994_vbif_rt_xins[x] * 2);
			val &= ~mask;
			val |= qos << (msm8994_vbif_rt_xins[x] * 2);
		}
		writel_relaxed(val, mdp5_kms->vbif + 0x20 + i * 4);
	}
	/*
	 * 3.10 msm8994-mdss.dtsi qcom,mdss-default-ot-rd-limit
	 * and -wr-limit are 16. mdss_mdp_set_ot_limit writes
	 * MMSS_VBIF_RD_LIM_CONF (0xb0) / WR_LIM_CONF (0xc0) on
	 * every pipe queue. After GDSC those regs are 0: no
	 * outstanding AXI beats, SSPP fetch stalls, PP_DONE
	 * never latches. First boot kept lk OT=16.
	 */
	clk0 = mdp5_read(mdp5_kms, MDP5_SSPP_CLK_CTRL0);
	clk1 = mdp5_read(mdp5_kms, MDP5_SSPP_CLK_CTRL1);
	pr_info("talkman-mdss: vbif qos0=%08x rdlim0=%08x wrlim0=%08x clk0=%08x halt0=%08x halt1=%08x\n",
		readl_relaxed(mdp5_kms->vbif + 0x20),
		readl_relaxed(mdp5_kms->vbif + 0xb0),
		readl_relaxed(mdp5_kms->vbif + 0xc0),
		clk0,
		readl_relaxed(mdp5_kms->vbif + MMSS_VBIF_XIN_HALT_CTRL0),
		readl_relaxed(mdp5_kms->vbif + MMSS_VBIF_XIN_HALT_CTRL1));
	for (i = 0; i < 4; i++) {
		writel_relaxed(0x10101010, mdp5_kms->vbif + 0xb0 + i * 4);
		writel_relaxed(0x10101010, mdp5_kms->vbif + 0xc0 + i * 4);
	}
	/*
	 * 3.10 mdss_mdp_set_ot_limit: force_on_xin_clk then
	 * XIN_HALT_CTRL0 set / wait CTRL1 / clear. DMA0 xin 2
	 * clk 0x2AC bit 8; DMA1 xin 10 clk 0x2B4 bit 8.
	 * Drop force-on after the cycle (hw_settings 0xc0000ccc).
	 */
	mdp5_write(mdp5_kms, MDP5_SSPP_CLK_CTRL0, clk0 | MDP5_DMA_CLK_FORCE_ON);
	mdp5_write(mdp5_kms, MDP5_SSPP_CLK_CTRL1, clk1 | MDP5_DMA_CLK_FORCE_ON);
	wmb();
	mdp5_vbif_xin_halt_cycle(mdp5_kms, 2);
	mdp5_vbif_xin_halt_cycle(mdp5_kms, 10);
	/*
	 * 3.10 set_ot_limit drops force-on only because pipe_queue
	 * calls force_on_xin_clk again. MDP5 never did. Writing
	 * hw_settings 0xc0000ccc back gated DMA0 (bit 8): AHB still
	 * sees SRC_ADDR, CURRENT stays 0, DSI sits CMD_MDP_BUSY.
	 */
	mdp5_write(mdp5_kms, MDP5_SSPP_CLK_CTRL0, clk0 | MDP5_DMA_CLK_FORCE_ON);
	mdp5_write(mdp5_kms, MDP5_SSPP_CLK_CTRL1, clk1 | MDP5_DMA_CLK_FORCE_ON);
}

void mdp5_sspp_clk_force_on(struct mdp5_kms *mdp5_kms, enum mdp5_pipe pipe)
{
	u32 off, bit, val;

	switch (pipe) {
	case SSPP_DMA0:
		off = MDP5_SSPP_CLK_CTRL0;
		bit = MDP5_DMA_CLK_FORCE_ON;
		break;
	case SSPP_DMA1:
		off = MDP5_SSPP_CLK_CTRL1;
		bit = MDP5_DMA_CLK_FORCE_ON;
		break;
	case SSPP_VIG0:
		off = MDP5_SSPP_CLK_CTRL0;
		bit = BIT(0);
		break;
	case SSPP_VIG1:
		off = MDP5_SSPP_CLK_CTRL1;
		bit = BIT(0);
		break;
	case SSPP_RGB0:
		off = MDP5_SSPP_CLK_CTRL0;
		bit = BIT(4);
		break;
	case SSPP_RGB1:
		off = MDP5_SSPP_CLK_CTRL1;
		bit = BIT(4);
		break;
	default:
		return;
	}

	val = mdp5_read(mdp5_kms, off);
	if (val & bit)
		return;
	mdp5_write(mdp5_kms, off, val | bit);
	pr_info("talkman-mdss: sspp clk force-on pipe=%d %03x=%08x\n",
		pipe, off, val | bit);
}

/*
 * 3.10 overlay_start after POWER_OFF (not idle_pc): iommu attach
 * (TZ restore_sec_cfg) then mdss_hw_init then ctl_start.
 * mdss_hw_init does not zero CTL_OP; ctl_start programs it.
 */
void mdp5_hw_reset_after_pc(struct mdp5_kms *mdp5_kms)
{
	const struct mdp5_cfg_hw *hw;
	unsigned int i;
	int ret;

	/*
	 * First pm_runtime_get is read_mdp_hw_revision, before
	 * mdp5_ctlm_init. 3.10 overlay_start / idle_pc_restore
	 * run mdss_hw_init only after MDP is fully up.
	 */
	if (!mdp5_kms->ctlm)
		return;

	hw = mdp5_cfg_get_hw_config(mdp5_kms->cfg);

	if (qcom_scm_restore_sec_cfg_available()) {
		ret = qcom_scm_restore_sec_cfg(MDP5_SCM_MDSS_DEV_ID, 0);
		pr_info("talkman-mdss: restore_sec_cfg mdss ret=%d\n", ret);
	}

	/*
	 * 3.10 overlay_start: iommu_ctrl(1) before mdss_hw_init.
	 * Reprogram MDP CB0 after GDSC; domain stays attached.
	 */
	qcom_iommu_restore_ctx_after_pc(&mdp5_kms->pdev->dev);

	if (mdp5_kms->smp)
		mdp5_smp_reset_cache(mdp5_kms->smp);

	/*
	 * 3.10 mdss_hw_init: replay qcom,mdp-settings. It does
	 * not write INTF_SEL=0; ctl_start / ctl_restore OR it.
	 * Wiping INTF_SEL here was dual-LM scramble after GDSC.
	 */
	if (hw && hw->hw_settings) {
		for (i = 0; i < hw->nhw_settings; i++)
			mdp5_write(mdp5_kms, hw->hw_settings[i].off,
				   hw->hw_settings[i].val);
		pr_info("talkman-mdss: mdss_hw_init %u mdp-settings\n",
			hw->nhw_settings);
		/*
		 * 3.10 PANIC_ROBUST_CTRL 0x178 (COMMON_REG, rev 105).
		 * pipe-dma-panic-ctrl-offsets 8/9. hw_settings write
		 * LUT0 at 0x17c, not this enable. pipe_queue sets the
		 * bit; after GDSC it is 0. DMA clk force-on (testGV)
		 * left CURRENT=0 with DSI CMD_MDP_BUSY.
		 */
		mdp5_write(mdp5_kms, 0x178,
			   mdp5_read(mdp5_kms, 0x178) | BIT(8) | BIT(9));
		mdp5_restore_vbif_qos(mdp5_kms);
	}

	/*
	 * 3.10 mdss_hw_init after mdp-settings: identity enhist
	 * LUT + swap on every DSPP and VIG. First boot kept lk
	 * LUTs; POWER_OFF GDSC zeros them.
	 */
	if (hw) {
		static const enum mdp5_pipe vig[] = {
			SSPP_VIG0, SSPP_VIG1, SSPP_VIG2, SSPP_VIG3
		};

		for (i = 0; i < hw->dspp.count; i++) {
			unsigned int j;

			for (j = 0; j < 256; j++)
				mdp5_write(mdp5_kms,
					   REG_MDP5_DSPP_HIST_LUT_BASE(i), j);
			mdp5_write(mdp5_kms,
				   REG_MDP5_DSPP_HIST_LUT_SWAP(i), 1);
		}
		for (i = 0; i < ARRAY_SIZE(vig) && i < hw->pipe_vig.count; i++) {
			unsigned int j;

			for (j = 0; j < 256; j++)
				mdp5_write(mdp5_kms,
					   REG_MDP5_PIPE_HIST_LUT_BASE(vig[i]),
					   j);
			mdp5_write(mdp5_kms,
				   REG_MDP5_PIPE_HIST_LUT_SWAP(vig[i]), 1);
		}
		pr_info("talkman-mdss: mdss_hw_init hist dspp=%u vig=%u\n",
			hw->dspp.count, hw->pipe_vig.count);
	}
}

static int mdp5_hw_init(struct msm_kms *kms)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	struct device *dev = &mdp5_kms->pdev->dev;

	pm_runtime_get_sync(dev);

	/* Magic unknown register writes:
	 *
	 *    W VBIF:0x004 00000001      (mdss_mdp.c:839)
	 *    W MDP5:0x2e0 0xe9          (mdss_mdp.c:839)
	 *    W MDP5:0x2e4 0x55          (mdss_mdp.c:839)
	 *    W MDP5:0x3ac 0xc0000ccc    (mdss_mdp.c:839)
	 *    W MDP5:0x3b4 0xc0000ccc    (mdss_mdp.c:839)
	 *    W MDP5:0x3bc 0xcccccc      (mdss_mdp.c:839)
	 *    W MDP5:0x4a8 0xcccc0c0     (mdss_mdp.c:839)
	 *    W MDP5:0x4b0 0xccccc0c0    (mdss_mdp.c:839)
	 *    W MDP5:0x4b8 0xccccc000    (mdss_mdp.c:839)
	 *
	 * Downstream fbdev driver gets these register offsets/values
	 * from DT.. not really sure what these registers are or if
	 * different values for different boards/SoC's, etc.  I guess
	 * they are the golden registers.
	 *
	 * Not setting these does not seem to cause any problem.  But
	 * we may be getting lucky with the bootloader initializing
	 * them for us.  OTOH, if we can always count on the bootloader
	 * setting the golden registers, then perhaps we don't need to
	 * care.
	 */

	mdp5_hw_reset_after_pc(mdp5_kms);

	pm_runtime_put_sync(dev);

	return 0;
}

/* Global/shared object state funcs */

/*
 * This is a helper that returns the private state currently in operation.
 * Note that this would return the "old_state" if called in the atomic check
 * path, and the "new_state" after the atomic swap has been done.
 */
struct mdp5_global_state *
mdp5_get_existing_global_state(struct mdp5_kms *mdp5_kms)
{
	return to_mdp5_global_state(mdp5_kms->glob_state.state);
}

/*
 * This acquires the modeset lock set aside for global state, creates
 * a new duplicated private object state.
 */
struct mdp5_global_state *mdp5_get_global_state(struct drm_atomic_commit *s)
{
	struct msm_drm_private *priv = s->dev->dev_private;
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));
	struct drm_private_state *priv_state;

	priv_state = drm_atomic_get_private_obj_state(s, &mdp5_kms->glob_state);
	if (IS_ERR(priv_state))
		return ERR_CAST(priv_state);

	return to_mdp5_global_state(priv_state);
}

static struct drm_private_state *
mdp5_global_duplicate_state(struct drm_private_obj *obj)
{
	struct mdp5_global_state *state;

	state = kmemdup(obj->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	return &state->base;
}

static void mdp5_global_destroy_state(struct drm_private_obj *obj,
				      struct drm_private_state *state)
{
	struct mdp5_global_state *mdp5_state = to_mdp5_global_state(state);

	kfree(mdp5_state);
}

static struct drm_private_state *
mdp5_global_create_state(struct drm_private_obj *obj)
{
	struct drm_device *dev = obj->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));
	struct mdp5_global_state *mdp5_state;

	mdp5_state = kzalloc_obj(*mdp5_state);
	if (!mdp5_state)
		return ERR_PTR(-ENOMEM);

	__drm_atomic_helper_private_obj_create_state(obj, &mdp5_state->base);
	mdp5_state->mdp5_kms = mdp5_kms;

	return &mdp5_state->base;
}

static void mdp5_global_print_state(struct drm_printer *p,
				    const struct drm_private_state *state)
{
	struct mdp5_global_state *mdp5_state = to_mdp5_global_state(state);

	if (mdp5_state->mdp5_kms->smp)
		mdp5_smp_dump(mdp5_state->mdp5_kms->smp, p, mdp5_state);
}

static const struct drm_private_state_funcs mdp5_global_state_funcs = {
	.atomic_create_state = mdp5_global_create_state,
	.atomic_duplicate_state = mdp5_global_duplicate_state,
	.atomic_destroy_state = mdp5_global_destroy_state,
	.atomic_print_state = mdp5_global_print_state,
};

static void mdp5_enable_commit(struct msm_kms *kms)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	pm_runtime_get_sync(&mdp5_kms->pdev->dev);
}

static void mdp5_disable_commit(struct msm_kms *kms)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	pm_runtime_put_sync(&mdp5_kms->pdev->dev);
}

/*
 * 3.10 overlay_start runs before pipes are queued. msm_atomic
 * commit_tail writes planes, then modeset enable. After GDSC the
 * plane flush hits CTL_OP=0; crtc enable then restores INTF and
 * STARTs without that flush. Do ctl_start here, clocks already on
 * from enable_commit.
 */
static void mdp5_overlay_start_before_planes(struct mdp5_kms *mdp5_kms,
					     struct drm_atomic_commit *state)
{
	const struct mdp5_cfg_hw *hw =
		mdp5_cfg_get_hw_config(mdp5_kms->cfg);
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_s, *new_s;
	int i;
	bool restored = false;

	if (!mdp5_hw_dual_lm_no_ppb(hw))
		return;

	/*
	 * 3.10 overlay_start only when ctl was power-off. Do not
	 * replay TZ/hw_init on every page flip.
	 */
	for_each_oldnew_crtc_in_state(state, crtc, old_s, new_s, i) {
		struct mdp5_crtc_state *cs;

		if (!new_s || !new_s->active || (old_s && old_s->active))
			continue;
		if (!restored) {
			mdp5_hw_reset_after_pc(mdp5_kms);
			restored = true;
		}
		cs = to_mdp5_crtc_state(new_s);
		if (!cs->ctl || !cs->pipeline.intf)
			continue;
		mdp5_ctl_set_pipeline(cs->ctl, &cs->pipeline);
		if (cs->pipeline.intf->mode == MDP5_INTF_DSI_MODE_COMMAND)
			mdp5_cmd_tearcheck_setup_crtc(crtc);
		pr_info("talkman-mdss: overlay_start before planes\n");
		pr_info("talkman-mdss: restore smp0=%08x lyr0_2=%08x lyr1_5=%08x pp2_h=%08x pp3_h=%08x te2=%08x te3=%08x\n",
			mdp5_read(mdp5_kms, REG_MDP5_SMP_ALLOC_W_REG(0)),
			mdp5_read(mdp5_kms, REG_MDP5_CTL_LAYER_REG(0, 2)),
			mdp5_read(mdp5_kms, REG_MDP5_CTL_LAYER_REG(1, 5)),
			mdp5_read(mdp5_kms, REG_MDP5_PP_SYNC_CONFIG_HEIGHT(2)),
			mdp5_read(mdp5_kms, REG_MDP5_PP_SYNC_CONFIG_HEIGHT(3)),
			mdp5_read(mdp5_kms, REG_MDP5_PP_TEAR_CHECK_EN(2)),
			mdp5_read(mdp5_kms, REG_MDP5_PP_TEAR_CHECK_EN(3)));
	}
}

static unsigned long mdp5_smp_staged_pipes(struct drm_atomic_commit *state)
{
	struct drm_plane *plane;
	struct drm_plane_state *new_s;
	unsigned long pipes = 0;
	int i;

	for_each_new_plane_in_state(state, plane, new_s, i) {
		struct mdp5_plane_state *ps;

		if (!new_s || !new_s->crtc || !new_s->fb)
			continue;
		ps = to_mdp5_plane_state(new_s);
		if (ps->hwpipe)
			pipes |= BIT(ps->hwpipe->pipe);
		if (ps->r_hwpipe)
			pipes |= BIT(ps->r_hwpipe->pipe);
	}
	return pipes;
}

static void mdp5_prepare_commit(struct msm_kms *kms, struct drm_atomic_commit *state)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	struct mdp5_global_state *global_state;

	mdp5_overlay_start_before_planes(mdp5_kms, state);

	global_state = mdp5_get_existing_global_state(mdp5_kms);

	if (mdp5_kms->smp)
		mdp5_smp_prepare_commit(mdp5_kms->smp, &global_state->smp,
					mdp5_smp_staged_pipes(state));
}

static void mdp5_flush_commit(struct msm_kms *kms, unsigned crtc_mask)
{
	/* TODO */
}

static void mdp5_wait_flush(struct msm_kms *kms, unsigned crtc_mask)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	struct drm_crtc *crtc;

	for_each_crtc_mask(mdp5_kms->dev, crtc, crtc_mask)
		mdp5_crtc_wait_for_commit_done(crtc);
}

static void mdp5_complete_commit(struct msm_kms *kms, unsigned crtc_mask)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));
	struct mdp5_global_state *global_state;

	global_state = mdp5_get_existing_global_state(mdp5_kms);

	if (mdp5_kms->smp)
		mdp5_smp_complete_commit(mdp5_kms->smp, &global_state->smp);
}

static void mdp5_destroy(struct mdp5_kms *mdp5_kms);

static void mdp5_kms_destroy(struct msm_kms *kms)
{
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));

	if (kms->vm) {
		struct msm_mmu *mmu = to_msm_vm(kms->vm)->mmu;

		mmu->funcs->detach(mmu);
		drm_gpuvm_put(kms->vm);
	}

	mdp_kms_destroy(&mdp5_kms->base);
	mdp5_destroy(mdp5_kms);
}

static const struct mdp_kms_funcs kms_funcs = {
	.base = {
		.hw_init         = mdp5_hw_init,
		.irq_preinstall  = mdp5_irq_preinstall,
		.irq_postinstall = mdp5_irq_postinstall,
		.irq_uninstall   = mdp5_irq_uninstall,
		.irq             = mdp5_irq,
		.enable_vblank   = mdp5_enable_vblank,
		.disable_vblank  = mdp5_disable_vblank,
		.flush_commit    = mdp5_flush_commit,
		.enable_commit   = mdp5_enable_commit,
		.disable_commit  = mdp5_disable_commit,
		.prepare_commit  = mdp5_prepare_commit,
		.wait_flush      = mdp5_wait_flush,
		.complete_commit = mdp5_complete_commit,
		.destroy         = mdp5_kms_destroy,
	},
	.set_irqmask         = mdp5_set_irqmask,
};

static int mdp5_disable(struct mdp5_kms *mdp5_kms)
{
	DBG("");

	mdp5_kms->enable_count--;
	WARN_ON(mdp5_kms->enable_count < 0);

	clk_disable_unprepare(mdp5_kms->tbu_rt_clk);
	clk_disable_unprepare(mdp5_kms->tbu_clk);
	clk_disable_unprepare(mdp5_kms->ahb_clk);
	clk_disable_unprepare(mdp5_kms->axi_clk);
	clk_disable_unprepare(mdp5_kms->core_clk);
	clk_disable_unprepare(mdp5_kms->lut_clk);

	return 0;
}

static int mdp5_enable(struct mdp5_kms *mdp5_kms)
{
	DBG("");

	mdp5_kms->enable_count++;

	clk_prepare_enable(mdp5_kms->ahb_clk);
	clk_prepare_enable(mdp5_kms->axi_clk);
	clk_prepare_enable(mdp5_kms->core_clk);
	clk_prepare_enable(mdp5_kms->lut_clk);
	clk_prepare_enable(mdp5_kms->tbu_clk);
	clk_prepare_enable(mdp5_kms->tbu_rt_clk);

	return 0;
}

static struct drm_encoder *construct_encoder(struct mdp5_kms *mdp5_kms,
					     struct mdp5_interface *intf,
					     struct mdp5_ctl *ctl)
{
	struct drm_device *dev = mdp5_kms->dev;
	struct drm_encoder *encoder;

	encoder = mdp5_encoder_init(dev, intf, ctl);
	if (IS_ERR(encoder)) {
		DRM_DEV_ERROR(dev->dev, "failed to construct encoder\n");
		return encoder;
	}

	return encoder;
}

static int get_dsi_id_from_intf(const struct mdp5_cfg_hw *hw_cfg, int intf_num)
{
	const enum mdp5_intf_type *intfs = hw_cfg->intf.connect;
	const int intf_cnt = ARRAY_SIZE(hw_cfg->intf.connect);
	int id = 0, i;

	for (i = 0; i < intf_cnt; i++) {
		if (intfs[i] == INTF_DSI) {
			if (intf_num == i)
				return id;

			id++;
		}
	}

	return -EINVAL;
}

static int modeset_init_intf(struct mdp5_kms *mdp5_kms,
			     struct mdp5_interface *intf)
{
	struct drm_device *dev = mdp5_kms->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct mdp5_ctl_manager *ctlm = mdp5_kms->ctlm;
	struct mdp5_ctl *ctl;
	struct drm_encoder *encoder;
	int ret = 0;

	switch (intf->type) {
	case INTF_eDP:
		DRM_DEV_INFO(dev->dev, "Skipping eDP interface %d\n", intf->num);
		break;
	case INTF_HDMI:
		if (!priv->kms->hdmi)
			break;

		ctl = mdp5_ctlm_request(ctlm, intf->num);
		if (!ctl) {
			ret = -EINVAL;
			break;
		}

		encoder = construct_encoder(mdp5_kms, intf, ctl);
		if (IS_ERR(encoder)) {
			ret = PTR_ERR(encoder);
			break;
		}

		ret = msm_hdmi_modeset_init(priv->kms->hdmi, dev, encoder);
		break;
	case INTF_DSI:
	{
		const struct mdp5_cfg_hw *hw_cfg =
					mdp5_cfg_get_hw_config(mdp5_kms->cfg);
		int dsi_id = get_dsi_id_from_intf(hw_cfg, intf->num);

		if ((dsi_id >= ARRAY_SIZE(priv->kms->dsi)) || (dsi_id < 0)) {
			DRM_DEV_ERROR(dev->dev, "failed to find dsi from intf %d\n",
				intf->num);
			ret = -EINVAL;
			break;
		}

		if (!priv->kms->dsi[dsi_id])
			break;

		ctl = mdp5_ctlm_request(ctlm, intf->num);
		if (!ctl) {
			ret = -EINVAL;
			break;
		}

		encoder = construct_encoder(mdp5_kms, intf, ctl);
		if (IS_ERR(encoder)) {
			ret = PTR_ERR(encoder);
			break;
		}

		ret = msm_dsi_modeset_init(priv->kms->dsi[dsi_id], dev, encoder);
		if (!ret)
			mdp5_encoder_set_intf_mode(encoder,
						   msm_dsi_is_cmd_mode(priv->kms->dsi[dsi_id]));

		break;
	}
	default:
		DRM_DEV_ERROR(dev->dev, "unknown intf: %d\n", intf->type);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int modeset_init(struct mdp5_kms *mdp5_kms)
{
	struct drm_device *dev = mdp5_kms->dev;
	unsigned int num_crtcs;
	int i, ret, pi = 0, ci = 0;
	struct drm_plane *primary[MAX_BASES] = { NULL };
	struct drm_plane *cursor[MAX_BASES] = { NULL };
	struct drm_encoder *encoder;
	unsigned int num_encoders;

	/*
	 * Construct encoders and modeset initialize connector devices
	 * for each external display interface.
	 */
	for (i = 0; i < mdp5_kms->num_intfs; i++) {
		ret = modeset_init_intf(mdp5_kms, mdp5_kms->intfs[i]);
		if (ret)
			goto fail;
	}

	num_encoders = 0;
	drm_for_each_encoder(encoder, dev)
		num_encoders++;

	/*
	 * We should ideally have less number of encoders (set up by parsing
	 * the MDP5 interfaces) than the number of layer mixers present in HW,
	 * but let's be safe here anyway
	 */
	num_crtcs = min(num_encoders, mdp5_kms->num_hwmixers);

	/*
	 * Construct planes equaling the number of hw pipes, and CRTCs for the
	 * N encoders set up by the driver. The first N planes become primary
	 * planes for the CRTCs, with the remainder as overlay planes:
	 */
	for (i = 0; i < mdp5_kms->num_hwpipes; i++) {
		struct mdp5_hw_pipe *hwpipe = mdp5_kms->hwpipes[i];
		struct drm_plane *plane;
		enum drm_plane_type type;

		if (i < num_crtcs)
			type = DRM_PLANE_TYPE_PRIMARY;
		else if (hwpipe->caps & MDP_PIPE_CAP_CURSOR)
			type = DRM_PLANE_TYPE_CURSOR;
		else
			type = DRM_PLANE_TYPE_OVERLAY;

		plane = mdp5_plane_init(dev, type);
		if (IS_ERR(plane)) {
			ret = PTR_ERR(plane);
			DRM_DEV_ERROR(dev->dev, "failed to construct plane %d (%d)\n", i, ret);
			goto fail;
		}

		if (type == DRM_PLANE_TYPE_PRIMARY)
			primary[pi++] = plane;
		if (type == DRM_PLANE_TYPE_CURSOR)
			cursor[ci++] = plane;
	}

	for (i = 0; i < num_crtcs; i++) {
		struct drm_crtc *crtc;

		crtc  = mdp5_crtc_init(dev, primary[i], cursor[i], i);
		if (IS_ERR(crtc)) {
			ret = PTR_ERR(crtc);
			DRM_DEV_ERROR(dev->dev, "failed to construct crtc %d (%d)\n", i, ret);
			goto fail;
		}
	}

	/*
	 * Now that we know the number of crtcs we've created, set the possible
	 * crtcs for the encoders
	 */
	drm_for_each_encoder(encoder, dev)
		encoder->possible_crtcs = (1 << dev->mode_config.num_crtc) - 1;

	return 0;

fail:
	return ret;
}

static void read_mdp_hw_revision(struct mdp5_kms *mdp5_kms,
				 u32 *major, u32 *minor)
{
	struct device *dev = &mdp5_kms->pdev->dev;
	u32 version;

	{
		int pret = pm_runtime_get_sync(dev);

		version = mdp5_read(mdp5_kms, REG_MDP5_HW_VERSION);
		dev_info(dev, "talkman-mdp: kms pm=%d raw0=%08x raw_caf0xf00=%08x\n",
			 pret, version, mdp5_read(mdp5_kms, 0xf00));
		pm_runtime_put_sync(dev);
	}

	*major = FIELD(version, MDP5_HW_VERSION_MAJOR);
	*minor = FIELD(version, MDP5_HW_VERSION_MINOR);

	DRM_DEV_INFO(dev, "MDP5 version v%d.%d", *major, *minor);
}

static int get_clk(struct platform_device *pdev, struct clk **clkp,
		const char *name, bool mandatory)
{
	struct device *dev = &pdev->dev;
	struct clk *clk = msm_clk_get(pdev, name);
	if (IS_ERR(clk) && mandatory) {
		DRM_DEV_ERROR(dev, "failed to get %s (%ld)\n", name, PTR_ERR(clk));
		return PTR_ERR(clk);
	}
	if (IS_ERR(clk))
		DBG("skipping %s", name);
	else
		*clkp = clk;

	return 0;
}

static int mdp5_init(struct platform_device *pdev, struct drm_device *dev);

static int mdp5_kms_init(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct platform_device *pdev;
	struct mdp5_kms *mdp5_kms;
	struct mdp5_cfg *config;
	struct msm_kms *kms = priv->kms;
	struct drm_gpuvm *vm;
	int i, ret;

	ret = mdp5_init(to_platform_device(dev->dev), dev);
	if (ret)
		return ret;

	mdp5_kms = to_mdp5_kms(to_mdp_kms(kms));

	pdev = mdp5_kms->pdev;

	ret = mdp_kms_init(&mdp5_kms->base, &kms_funcs);
	if (ret) {
		DRM_DEV_ERROR(&pdev->dev, "failed to init kms\n");
		goto fail;
	}

	config = mdp5_cfg_get_config(mdp5_kms->cfg);

	/* make sure things are off before attaching iommu (bootloader could
	 * have left things on, in which case we'll start getting faults if
	 * we don't disable):
	 */
	pm_runtime_get_sync(&pdev->dev);
	for (i = 0; i < MDP5_INTF_NUM_MAX; i++) {
		if (mdp5_cfg_intf_is_virtual(config->hw->intf.connect[i]) ||
		    !config->hw->intf.base[i])
			continue;
		mdp5_write(mdp5_kms, REG_MDP5_INTF_TIMING_ENGINE_EN(i), 0);

		mdp5_write(mdp5_kms, REG_MDP5_INTF_FRAME_LINE_COUNT_EN(i), 0x3);
	}
	mdelay(16);

	vm = msm_kms_init_vm(mdp5_kms->dev, pdev->dev.parent);
	if (IS_ERR(vm)) {
		ret = PTR_ERR(vm);
		goto fail;
	}

	kms->vm = vm;

	pm_runtime_put_sync(&pdev->dev);

	ret = modeset_init(mdp5_kms);
	if (ret) {
		DRM_DEV_ERROR(&pdev->dev, "modeset_init failed: %d\n", ret);
		goto fail;
	}

	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = 0xffff;
	dev->mode_config.max_height = 0xffff;

	dev->max_vblank_count = 0; /* max_vblank_count is set on each CRTC */
	dev->vblank_disable_immediate = true;

	return 0;
fail:
	if (kms)
		mdp5_kms_destroy(kms);

	return ret;
}

static void mdp5_destroy(struct mdp5_kms *mdp5_kms)
{
	if (mdp5_kms->rpm_enabled)
		pm_runtime_disable(&mdp5_kms->pdev->dev);

	drm_atomic_private_obj_fini(&mdp5_kms->glob_state);
}

static int construct_pipes(struct mdp5_kms *mdp5_kms, int cnt,
		const enum mdp5_pipe *pipes, const uint32_t *offsets,
		uint32_t caps)
{
	struct drm_device *dev = mdp5_kms->dev;
	int i, ret;

	for (i = 0; i < cnt; i++) {
		struct mdp5_hw_pipe *hwpipe;

		hwpipe = mdp5_pipe_init(dev, pipes[i], offsets[i], caps);
		if (IS_ERR(hwpipe)) {
			ret = PTR_ERR(hwpipe);
			DRM_DEV_ERROR(dev->dev, "failed to construct pipe for %s (%d)\n",
					pipe2name(pipes[i]), ret);
			return ret;
		}
		hwpipe->idx = mdp5_kms->num_hwpipes;
		mdp5_kms->hwpipes[mdp5_kms->num_hwpipes++] = hwpipe;
	}

	return 0;
}

static int hwpipe_init(struct mdp5_kms *mdp5_kms)
{
	static const enum mdp5_pipe rgb_planes[] = {
			SSPP_RGB0, SSPP_RGB1, SSPP_RGB2, SSPP_RGB3,
	};
	static const enum mdp5_pipe vig_planes[] = {
			SSPP_VIG0, SSPP_VIG1, SSPP_VIG2, SSPP_VIG3,
	};
	static const enum mdp5_pipe dma_planes[] = {
			SSPP_DMA0, SSPP_DMA1,
	};
	static const enum mdp5_pipe cursor_planes[] = {
			SSPP_CURSOR0, SSPP_CURSOR1,
	};
	const struct mdp5_cfg_hw *hw_cfg;
	int ret;

	hw_cfg = mdp5_cfg_get_hw_config(mdp5_kms->cfg);

	/* Construct RGB pipes: */
	ret = construct_pipes(mdp5_kms, hw_cfg->pipe_rgb.count, rgb_planes,
			hw_cfg->pipe_rgb.base, hw_cfg->pipe_rgb.caps);
	if (ret)
		return ret;

	/* Construct video (VIG) pipes: */
	ret = construct_pipes(mdp5_kms, hw_cfg->pipe_vig.count, vig_planes,
			hw_cfg->pipe_vig.base, hw_cfg->pipe_vig.caps);
	if (ret)
		return ret;

	/* Construct DMA pipes: */
	ret = construct_pipes(mdp5_kms, hw_cfg->pipe_dma.count, dma_planes,
			hw_cfg->pipe_dma.base, hw_cfg->pipe_dma.caps);
	if (ret)
		return ret;

	/* Construct cursor pipes: */
	ret = construct_pipes(mdp5_kms, hw_cfg->pipe_cursor.count,
			cursor_planes, hw_cfg->pipe_cursor.base,
			hw_cfg->pipe_cursor.caps);
	if (ret)
		return ret;

	return 0;
}

static int hwmixer_init(struct mdp5_kms *mdp5_kms)
{
	struct drm_device *dev = mdp5_kms->dev;
	const struct mdp5_cfg_hw *hw_cfg;
	int i, ret;

	hw_cfg = mdp5_cfg_get_hw_config(mdp5_kms->cfg);

	for (i = 0; i < hw_cfg->lm.count; i++) {
		struct mdp5_hw_mixer *mixer;

		mixer = mdp5_mixer_init(dev, &hw_cfg->lm.instances[i]);
		if (IS_ERR(mixer)) {
			ret = PTR_ERR(mixer);
			DRM_DEV_ERROR(dev->dev, "failed to construct LM%d (%d)\n",
				i, ret);
			return ret;
		}

		mixer->idx = mdp5_kms->num_hwmixers;
		mdp5_kms->hwmixers[mdp5_kms->num_hwmixers++] = mixer;
	}

	return 0;
}

static int interface_init(struct mdp5_kms *mdp5_kms)
{
	struct drm_device *dev = mdp5_kms->dev;
	const struct mdp5_cfg_hw *hw_cfg;
	const enum mdp5_intf_type *intf_types;
	int i;

	hw_cfg = mdp5_cfg_get_hw_config(mdp5_kms->cfg);
	intf_types = hw_cfg->intf.connect;

	for (i = 0; i < ARRAY_SIZE(hw_cfg->intf.connect); i++) {
		struct mdp5_interface *intf;

		if (intf_types[i] == INTF_DISABLED)
			continue;

		intf = devm_kzalloc(dev->dev, sizeof(*intf), GFP_KERNEL);
		if (!intf) {
			DRM_DEV_ERROR(dev->dev, "failed to construct INTF%d\n", i);
			return -ENOMEM;
		}

		intf->num = i;
		intf->type = intf_types[i];
		intf->mode = MDP5_INTF_MODE_NONE;
		intf->idx = mdp5_kms->num_intfs;
		mdp5_kms->intfs[mdp5_kms->num_intfs++] = intf;
	}

	return 0;
}

static int mdp5_init(struct platform_device *pdev, struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));
	struct mdp5_cfg *config;
	u32 major, minor;
	int ret;

	mdp5_kms->dev = dev;

	drm_atomic_private_obj_init(mdp5_kms->dev, &mdp5_kms->glob_state,
				    &mdp5_global_state_funcs);

	/* we need to set a default rate before enabling.  Set a safe
	 * rate first, then figure out hw revision, and then set a
	 * more optimal rate:
	 */
	clk_set_rate(mdp5_kms->core_clk, 200000000);

	pm_runtime_enable(&pdev->dev);
	mdp5_kms->rpm_enabled = true;

	read_mdp_hw_revision(mdp5_kms, &major, &minor);

	mdp5_kms->cfg = mdp5_cfg_init(mdp5_kms, major, minor);
	if (IS_ERR(mdp5_kms->cfg)) {
		ret = PTR_ERR(mdp5_kms->cfg);
		mdp5_kms->cfg = NULL;
		goto fail;
	}

	config = mdp5_cfg_get_config(mdp5_kms->cfg);
	mdp5_kms->caps = config->hw->mdp.caps;

	/* TODO: compute core clock rate at runtime */
	clk_set_rate(mdp5_kms->core_clk, config->hw->max_clk);

	/*
	 * Some chipsets have a Shared Memory Pool (SMP), while others
	 * have dedicated latency buffering per source pipe instead;
	 * this section initializes the SMP:
	 */
	if (mdp5_kms->caps & MDP_CAP_SMP) {
		mdp5_kms->smp = mdp5_smp_init(mdp5_kms, &config->hw->smp);
		if (IS_ERR(mdp5_kms->smp)) {
			ret = PTR_ERR(mdp5_kms->smp);
			mdp5_kms->smp = NULL;
			goto fail;
		}
	}

	mdp5_kms->ctlm = mdp5_ctlm_init(dev, mdp5_kms->mmio, mdp5_kms->cfg);
	if (IS_ERR(mdp5_kms->ctlm)) {
		ret = PTR_ERR(mdp5_kms->ctlm);
		mdp5_kms->ctlm = NULL;
		goto fail;
	}

	ret = hwpipe_init(mdp5_kms);
	if (ret)
		goto fail;

	ret = hwmixer_init(mdp5_kms);
	if (ret)
		goto fail;

	ret = interface_init(mdp5_kms);
	if (ret)
		goto fail;

	return 0;
fail:
	mdp5_destroy(mdp5_kms);
	return ret;
}

static int mdp5_setup_interconnect(struct platform_device *pdev)
{
	struct icc_path *path0 = msm_icc_get(&pdev->dev, "mdp0-mem");
	struct icc_path *path1 = msm_icc_get(&pdev->dev, "mdp1-mem");
	struct icc_path *path_rot = msm_icc_get(&pdev->dev, "rotator-mem");

	if (IS_ERR(path0))
		return PTR_ERR(path0);

	if (!path0) {
		/* no interconnect support is not necessarily a fatal
		 * condition, the platform may simply not have an
		 * interconnect driver yet.  But warn about it in case
		 * bootloader didn't setup bus clocks high enough for
		 * scanout.
		 */
		dev_warn(&pdev->dev, "No interconnect support may cause display underflows!\n");
		return 0;
	}

	icc_set_bw(path0, 0, MBps_to_icc(6400));

	if (!IS_ERR_OR_NULL(path1))
		icc_set_bw(path1, 0, MBps_to_icc(6400));
	if (!IS_ERR_OR_NULL(path_rot))
		icc_set_bw(path_rot, 0, MBps_to_icc(6400));

	return 0;
}

static int mdp5_dev_probe(struct platform_device *pdev)
{
	struct mdp5_kms *mdp5_kms;
	int ret, irq;

	DBG("");

	if (!msm_disp_drv_should_bind(&pdev->dev, false))
		return -ENODEV;

	mdp5_kms = devm_kzalloc(&pdev->dev, sizeof(*mdp5_kms), GFP_KERNEL);
	if (!mdp5_kms)
		return -ENOMEM;

	ret = mdp5_setup_interconnect(pdev);
	if (ret)
		return ret;

	mdp5_kms->pdev = pdev;

	spin_lock_init(&mdp5_kms->resource_lock);

	mdp5_kms->mmio = msm_ioremap(pdev, "mdp_phys");
	if (IS_ERR(mdp5_kms->mmio))
		return PTR_ERR(mdp5_kms->mmio);

	if (pdev->dev.parent) {
		struct platform_device *mdss_pdev =
			to_platform_device(pdev->dev.parent);
		struct resource *vres;

		vres = platform_get_resource_byname(mdss_pdev, IORESOURCE_MEM,
						    "vbif_phys");
		if (vres)
			mdp5_kms->vbif = devm_ioremap(&pdev->dev, vres->start,
						      resource_size(vres));
	}

	/* mandatory clocks: */
	ret = get_clk(pdev, &mdp5_kms->axi_clk, "bus", true);
	if (ret)
		return ret;
	ret = get_clk(pdev, &mdp5_kms->ahb_clk, "iface", true);
	if (ret)
		return ret;
	ret = get_clk(pdev, &mdp5_kms->core_clk, "core", true);
	if (ret)
		return ret;
	ret = get_clk(pdev, &mdp5_kms->vsync_clk, "vsync", true);
	if (ret)
		return ret;

	/* optional clocks: */
	get_clk(pdev, &mdp5_kms->lut_clk, "lut", false);
	get_clk(pdev, &mdp5_kms->tbu_clk, "tbu", false);
	get_clk(pdev, &mdp5_kms->tbu_rt_clk, "tbu_rt", false);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return dev_err_probe(&pdev->dev, irq, "failed to get irq\n");

	mdp5_kms->base.base.irq = irq;

	{
		struct resource *r;
		u32 raw0, raw_caf;
		int pret;

		r = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						 "mdp_phys");
		/*
		 * CAF 8992/8994 HW_VERSION is mdp_phys+0x1000 = 0xfd901000.
		 * Mainline maps 0xfd900100, so +0xf00 is that register.
		 * Enable parent MDSS GDSC + MDP AHB/AXI/core before the
		 * read. Do not feed either value into mdp5_cfg_init.
		 */
		pret = pm_runtime_get_sync(pdev->dev.parent);
		if (pret < 0) {
			dev_info(&pdev->dev,
				 "talkman-mdp: pa=%pa parent_pm=%d skip read\n",
				 r ? &r->start : NULL, pret);
		} else {
			clk_prepare_enable(mdp5_kms->ahb_clk);
			clk_prepare_enable(mdp5_kms->axi_clk);
			clk_prepare_enable(mdp5_kms->core_clk);
			raw0 = readl_relaxed(mdp5_kms->mmio);
			raw_caf = readl_relaxed(mdp5_kms->mmio + 0xf00);
			dev_info(&pdev->dev,
				 "talkman-mdp: pa=%pa parent_pm=%d raw0=%08x raw_caf0xf00=%08x\n",
				 r ? &r->start : NULL, pret, raw0, raw_caf);
			clk_disable_unprepare(mdp5_kms->core_clk);
			clk_disable_unprepare(mdp5_kms->axi_clk);
			clk_disable_unprepare(mdp5_kms->ahb_clk);
			pm_runtime_put_sync(pdev->dev.parent);
		}
	}

	return msm_drv_probe(&pdev->dev, mdp5_kms_init, &mdp5_kms->base.base);
}

static void mdp5_dev_remove(struct platform_device *pdev)
{
	DBG("");
	component_master_del(&pdev->dev, &msm_drm_ops);
}

static __maybe_unused int mdp5_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_drm_private *priv = platform_get_drvdata(pdev);
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));

	DBG("");

	return mdp5_disable(mdp5_kms);
}

static __maybe_unused int mdp5_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_drm_private *priv = platform_get_drvdata(pdev);
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));

	DBG("");

	return mdp5_enable(mdp5_kms);
}

static void mdp5_pm_complete(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_drm_private *priv = platform_get_drvdata(pdev);

	/*
	 * 3.10 mdss_mdp_idle_pc_restore runs on the first screen
	 * update after GDSC collapse, before ping-pong: clk on,
	 * mdss_hw_init, ctl_restore (INTF_SEL + split_display_enable).
	 * drm_mode_config_helper_resume is that update. testFM:
	 * dpm_resume finished, then 20nm PLL locked with no
	 * encoder_enable / hw_reset — hang is inside helper_resume.
	 */
	if (priv && priv->kms) {
		struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));
		const struct mdp5_cfg_hw *hw =
			mdp5_cfg_get_hw_config(mdp5_kms->cfg);

		/*
		 * 3.10 ctl_restore: mdss_mdp_clk_ctrl(POWER_ON) stays
		 * up for the screen update. put_sync before
		 * helper_resume dropped GDSC under a live PHY.
		 * INTF_SEL + split_display_enable(1) + CTL_OP, never
		 * INTF_SEL=0.
		 */
		if (mdp5_hw_dual_lm_no_ppb(hw)) {
			struct drm_crtc *crtc;

			pm_runtime_get_sync(dev);
			drm_for_each_crtc(crtc, mdp5_kms->dev) {
				struct mdp5_crtc_state *cs;

				if (!crtc->state)
					continue;
				cs = to_mdp5_crtc_state(crtc->state);
				if (!cs->ctl || !cs->pipeline.intf)
					continue;
				mdp5_ctl_set_pipeline(cs->ctl, &cs->pipeline);
			}
			msm_kms_pm_complete(dev);
			pm_runtime_put_sync(dev);
			return;
		}
	}

	msm_kms_pm_complete(dev);
}

static const struct dev_pm_ops mdp5_pm_ops = {
	SET_RUNTIME_PM_OPS(mdp5_runtime_suspend, mdp5_runtime_resume, NULL)
	.prepare = msm_kms_pm_prepare,
	.complete = mdp5_pm_complete,
};

static const struct of_device_id mdp5_dt_match[] = {
	{ .compatible = "qcom,mdp5", },
	/* to support downstream DT files */
	{ .compatible = "qcom,mdss_mdp", },
	{}
};
MODULE_DEVICE_TABLE(of, mdp5_dt_match);

static struct platform_driver mdp5_driver = {
	.probe = mdp5_dev_probe,
	.remove = mdp5_dev_remove,
	.shutdown = msm_kms_shutdown,
	.driver = {
		.name = "msm_mdp",
		.of_match_table = mdp5_dt_match,
		.pm = &mdp5_pm_ops,
	},
};

void __init msm_mdp_register(void)
{
	DBG("");
	platform_driver_register(&mdp5_driver);
}

void __exit msm_mdp_unregister(void)
{
	DBG("");
	platform_driver_unregister(&mdp5_driver);
}
