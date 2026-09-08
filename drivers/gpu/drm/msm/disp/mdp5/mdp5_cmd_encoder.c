// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_probe_helper.h>

#include "mdp5_kms.h"
#include "dsi/dsi.h"

void qcom_iommu_mdp_kickoff_attach(struct device *master);
void qcom_iommu_mdp_kickoff_done(struct device *master);

#ifdef CONFIG_DRM_MSM_DSI

static struct mdp5_kms *get_kms(struct drm_encoder *encoder)
{
	struct msm_drm_private *priv = encoder->dev->dev_private;
	return to_mdp5_kms(to_mdp_kms(priv->kms));
}

#define VSYNC_CLK_RATE 19200000

/*
 * 3.10 mdss_mdp_ctl_init sets ctl->dst_format = RGB888 (0x213F)
 * for 24bpp DSI, including command mode. Only mdss_mdp_video_start
 * writes INTF_PANEL_FORMAT. Cmd overlay_start never does. After
 * POWER_OFF GDSC that register is POR 0; first boot kept lk.
 * Sergej is 24bpp; same value as mainline vid mode_set 0x2100|0x3F.
 */
#define MDP5_INTF_PANEL_FORMAT_RGB888	0x213F

void mdp5_cmd_restore_intf_format(struct mdp5_kms *mdp5_kms,
				  struct mdp5_pipeline *pipeline)
{
	struct mdp5_interface *intf;

	if (!mdp5_kms || !pipeline)
		return;

	intf = pipeline->intf;
	if (intf && intf->mode == MDP5_INTF_DSI_MODE_COMMAND) {
		mdp5_write(mdp5_kms, REG_MDP5_INTF_PANEL_FORMAT(intf->num),
			   MDP5_INTF_PANEL_FORMAT_RGB888);
		/*
		 * mdp5_init writes FRAME_LINE_COUNT_EN=0x3 once
		 * (lk/splash GDSC). 3.10 video_start rewrites it;
		 * cmd never does. testIA overlay_start after GDSC
		 * read 0 while first boot kept 0x3.
		 */
		mdp5_write(mdp5_kms,
			   REG_MDP5_INTF_FRAME_LINE_COUNT_EN(intf->num), 0x3);
	}
	intf = pipeline->sintf;
	if (intf) {
		mdp5_write(mdp5_kms, REG_MDP5_INTF_PANEL_FORMAT(intf->num),
			   MDP5_INTF_PANEL_FORMAT_RGB888);
		mdp5_write(mdp5_kms,
			   REG_MDP5_INTF_FRAME_LINE_COUNT_EN(intf->num), 0x3);
	}
}

/*
 * 3.10 mdss_mdp_cmd_tearcheck_cfg. SYNC_WRCOUNT is start_pos +
 * threshold_start + 1. Mainline never wrote it; first boot kept
 * the lk1st value, POWER_OFF GDSC left it 0.
 */
static void pingpong_write_te(struct mdp5_kms *mdp5_kms, int pp_id,
			      u32 cfg, const struct drm_display_mode *mode)
{
	u32 start = mode->vdisplay;
	u32 thresh = MDP5_PP_SYNC_THRESH_START(4) |
		     MDP5_PP_SYNC_THRESH_CONTINUE(4);

	mdp5_write(mdp5_kms, REG_MDP5_PP_SYNC_CONFIG_VSYNC(pp_id), cfg);
	/*
	 * 3.10 mdss_panel_parse_te_params default when
	 * qcom,mdss-tear-check-sync-cfg-height is absent: 0xfff0.
	 * Sergej has TE pin (hw_vsync_mode). HEIGHT 2*vtotal is the
	 * vsync_clk fallback; after GDSC that wrap is not 3.10.
	 */
	mdp5_write(mdp5_kms, REG_MDP5_PP_SYNC_CONFIG_HEIGHT(pp_id),
		   0xfff0);
	mdp5_write(mdp5_kms, REG_MDP5_PP_VSYNC_INIT_VAL(pp_id), start);
	mdp5_write(mdp5_kms, REG_MDP5_PP_RD_PTR_IRQ(pp_id), start + 1);
	mdp5_write(mdp5_kms, REG_MDP5_PP_START_POS(pp_id), start);
	mdp5_write(mdp5_kms, REG_MDP5_PP_SYNC_THRESH(pp_id), thresh);
	mdp5_write(mdp5_kms, REG_MDP5_PP_SYNC_WRCOUNT(pp_id), start + 4 + 1);
	mdp5_write(mdp5_kms, REG_MDP5_PP_AUTOREFRESH_CONFIG(pp_id), 0);
}

static int pingpong_tearcheck_setup(struct drm_crtc *crtc,
				    struct drm_display_mode *mode)
{
	struct msm_drm_private *priv = crtc->dev->dev_private;
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));
	struct device *dev = crtc->dev->dev;
	u32 total_lines, vclks_line, cfg;
	long vsync_clk_speed;
	struct mdp5_hw_mixer *mixer = mdp5_crtc_get_mixer(crtc);
	int pp_id;

	if (IS_ERR_OR_NULL(mixer))
		return mixer ? PTR_ERR(mixer) : -ENODEV;
	pp_id = mixer->pp;

	if (IS_ERR_OR_NULL(mdp5_kms->vsync_clk)) {
		DRM_DEV_ERROR(dev, "vsync_clk is not initialized\n");
		return -EINVAL;
	}

	total_lines = mode->vtotal * drm_mode_vrefresh(mode);
	if (!total_lines) {
		DRM_DEV_ERROR(dev, "%s: vtotal(%d) or vrefresh(%d) is 0\n",
			      __func__, mode->vtotal, drm_mode_vrefresh(mode));
		return -EINVAL;
	}

	vsync_clk_speed = clk_round_rate(mdp5_kms->vsync_clk, VSYNC_CLK_RATE);
	if (vsync_clk_speed <= 0) {
		DRM_DEV_ERROR(dev, "vsync_clk round rate failed %ld\n",
							vsync_clk_speed);
		return -EINVAL;
	}
	vclks_line = vsync_clk_speed / total_lines;

	cfg = MDP5_PP_SYNC_CONFIG_VSYNC_COUNTER_EN
		| MDP5_PP_SYNC_CONFIG_VSYNC_IN_EN;
	cfg |= MDP5_PP_SYNC_CONFIG_VSYNC_COUNT(vclks_line);

	/*
	 * 3.10 Sergej TE pin: HEIGHT 0xfff0, WRCOUNT start+4+1.
	 * pingpong_write_te matches mdss_mdp_cmd_tearcheck_cfg.
	 */
	pingpong_write_te(mdp5_kms, pp_id, cfg, mode);

	/*
	 * 3.10 mdss_mdp_intf_cmd: pingpong-split also programs
	 * slave_pingpong_base (CAF 0x73000 / mainline 0x72000 =
	 * PP0 + pp_split.slave_pp_off). Do not use a fake PP index;
	 * xml may only enumerate two pingpongs.
	 */
	if (pp_id == 0) {
		const struct mdp5_cfg_hw *hw;
		u32 s;

		hw = mdp5_cfg_get_hw_config(mdp5_kms->cfg);
		s = hw ? hw->pp_split.slave_pp_off : 0;
		if (s) {
			u32 start = mode->vdisplay;
			u32 thresh = MDP5_PP_SYNC_THRESH_START(4) |
				     MDP5_PP_SYNC_THRESH_CONTINUE(4);

			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_SYNC_CONFIG_VSYNC(0) + s, cfg);
			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_SYNC_CONFIG_HEIGHT(0) + s,
				   0xfff0);
			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_VSYNC_INIT_VAL(0) + s, start);
			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_RD_PTR_IRQ(0) + s, start + 1);
			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_START_POS(0) + s, start);
			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_SYNC_THRESH(0) + s, thresh);
			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_SYNC_WRCOUNT(0) + s, start + 5);
			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_AUTOREFRESH_CONFIG(0) + s, 0);
		}
	}

	/*
	 * 8994 dual-LM: real PP1 (r_mixer->pp), not dest-split extra TE
	 * at mdp_phys+0x73000. 3.10 intf_cmd programs both pingpongs.
	 */
	{
		struct mdp5_pipeline *pipeline;
		struct mdp5_hw_mixer *r_mixer;

		pipeline = mdp5_crtc_get_pipeline(crtc);
		r_mixer = pipeline->r_mixer;
		if (r_mixer && r_mixer->pp >= 0 && r_mixer->pp != pp_id)
			pingpong_write_te(mdp5_kms, r_mixer->pp, cfg, mode);
	}

	return 0;
}

static int pingpong_tearcheck_enable(struct drm_encoder *encoder)
{
	struct mdp5_kms *mdp5_kms = get_kms(encoder);
	struct mdp5_hw_mixer *mixer = mdp5_crtc_get_mixer(encoder->crtc);
	int pp_id = mixer->pp;
	int ret;

	ret = clk_set_rate(mdp5_kms->vsync_clk,
		clk_round_rate(mdp5_kms->vsync_clk, VSYNC_CLK_RATE));
	if (ret) {
		DRM_DEV_ERROR(encoder->dev->dev,
			"vsync_clk clk_set_rate failed, %d\n", ret);
		return ret;
	}
	ret = clk_prepare_enable(mdp5_kms->vsync_clk);
	if (ret) {
		DRM_DEV_ERROR(encoder->dev->dev,
			"vsync_clk clk_prepare_enable failed, %d\n", ret);
		return ret;
	}

	mdp5_write(mdp5_kms, REG_MDP5_PP_TEAR_CHECK_EN(pp_id), 1);
	if (pp_id == 0) {
		const struct mdp5_cfg_hw *hw;
		u32 s;

		hw = mdp5_cfg_get_hw_config(mdp5_kms->cfg);
		s = hw ? hw->pp_split.slave_pp_off : 0;
		if (s)
			mdp5_write(mdp5_kms, REG_MDP5_PP_TEAR_CHECK_EN(0) + s, 1);
	}
	{
		struct mdp5_pipeline *pipeline;
		struct mdp5_hw_mixer *r_mixer;

		pipeline = mdp5_crtc_get_pipeline(encoder->crtc);
		r_mixer = pipeline->r_mixer;
		if (r_mixer && r_mixer->pp >= 0 && r_mixer->pp != pp_id)
			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_TEAR_CHECK_EN(r_mixer->pp), 1);
	}

	return 0;
}

static void pingpong_tearcheck_disable(struct drm_encoder *encoder)
{
	struct mdp5_kms *mdp5_kms = get_kms(encoder);
	struct mdp5_hw_mixer *mixer = mdp5_crtc_get_mixer(encoder->crtc);
	int pp_id = mixer->pp;

	mdp5_write(mdp5_kms, REG_MDP5_PP_TEAR_CHECK_EN(pp_id), 0);
	if (pp_id == 0) {
		const struct mdp5_cfg_hw *hw;
		u32 s;

		hw = mdp5_cfg_get_hw_config(mdp5_kms->cfg);
		s = hw ? hw->pp_split.slave_pp_off : 0;
		if (s)
			mdp5_write(mdp5_kms, REG_MDP5_PP_TEAR_CHECK_EN(0) + s, 0);
	}
	{
		struct mdp5_pipeline *pipeline;
		struct mdp5_hw_mixer *r_mixer;

		pipeline = mdp5_crtc_get_pipeline(encoder->crtc);
		r_mixer = pipeline->r_mixer;
		if (r_mixer && r_mixer->pp >= 0 && r_mixer->pp != pp_id)
			mdp5_write(mdp5_kms,
				   REG_MDP5_PP_TEAR_CHECK_EN(r_mixer->pp), 0);
	}
	clk_disable_unprepare(mdp5_kms->vsync_clk);
}

void mdp5_cmd_encoder_mode_set(struct drm_encoder *encoder,
			       struct drm_display_mode *mode,
			       struct drm_display_mode *adjusted_mode)
{
	mode = adjusted_mode;

	DBG("set mode: " DRM_MODE_FMT, DRM_MODE_ARG(mode));
	pingpong_tearcheck_setup(encoder->crtc, mode);
	mdp5_crtc_set_pipeline(encoder->crtc);
}

void mdp5_cmd_tearcheck_setup_crtc(struct drm_crtc *crtc)
{
	if (!crtc || !crtc->state)
		return;
	pingpong_tearcheck_setup(crtc, &crtc->state->adjusted_mode);
}

void mdp5_cmd_encoder_disable(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_cmd_enc = to_mdp5_encoder(encoder);
	struct mdp5_ctl *ctl = mdp5_cmd_enc->ctl;
	struct mdp5_interface *intf = mdp5_cmd_enc->intf;
	struct mdp5_pipeline *pipeline = mdp5_crtc_get_pipeline(encoder->crtc);

	if (WARN_ON(!mdp5_cmd_enc->enabled))
		return;

	pingpong_tearcheck_disable(encoder);

	mdp5_ctl_set_encoder_state(ctl, pipeline, false);
	mdp5_ctl_commit(ctl, pipeline, mdp_ctl_flush_mask_encoder(intf), true);

	/*
	 * 3.10 ctl_stop writes SPLIT_DISPLAY_EN=0 + CTL_OP=0 here.
	 * Doing that in testDW hung s2idle after console suspend
	 * (watchdog back to lk1st, no pstore stack). Restore is
	 * encoder enable: hw_reset_after_pc then mode_set.
	 */

	mdp5_cmd_enc->enabled = false;
}

void mdp5_cmd_encoder_enable(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_cmd_enc = to_mdp5_encoder(encoder);
	struct mdp5_ctl *ctl = mdp5_cmd_enc->ctl;
	struct mdp5_interface *intf = mdp5_cmd_enc->intf;
	struct mdp5_pipeline *pipeline = mdp5_crtc_get_pipeline(encoder->crtc);

	if (WARN_ON(mdp5_cmd_enc->enabled))
		return;

	mdp5_cmd_restore_intf_format(get_kms(encoder), pipeline);

	if (pingpong_tearcheck_enable(encoder))
		return;

	/*
	 * 3.10 LINK_READY (first kickoff, after pipes): mdss_dsi_on
	 * then op_mode_config. DRM pre_enable can run that before
	 * panel DCS DMA. Re-assert CMD_MODE_EN + CMD_MDP_DONE irq
	 * immediately before the first CTL_START.
	 */
	{
		struct msm_kms *kms = &get_kms(encoder)->base.base;
		int i;

		for (i = 0; i < MSM_DSI_CONTROLLER_COUNT; i++)
			if (kms->dsi[i] && kms->dsi[i]->host)
				msm_dsi_host_enable(kms->dsi[i]->host);
	}

	/*
	 * 3.10 overlay_kickoff: iommu_ctrl(1), then pipe_queue
	 * (image_setup, format, set_ot_limit halt, smp_alloc,
	 * src_addr), then flush+START. DRM had SMP+OT halt
	 * before the post-SMMU pipe rewrite.
	 */
	{
		struct mdp5_kms *mdp5_kms = get_kms(encoder);
		struct mdp5_global_state *gs;
		struct drm_plane *plane;

		qcom_iommu_mdp_kickoff_attach(&mdp5_kms->pdev->dev);
		drm_atomic_crtc_for_each_plane(plane, encoder->crtc)
			mdp5_plane_kickoff_queue(plane);
		if (mdp5_kms->smp) {
			gs = mdp5_get_existing_global_state(mdp5_kms);
			if (gs)
				mdp5_smp_prepare_commit(mdp5_kms->smp,
							&gs->smp);
			pr_info("talkman-mdss: smp at kickoff smp0=%08x smp8=%08x smp9=%08x\n",
				mdp5_read(mdp5_kms, REG_MDP5_SMP_ALLOC_W_REG(0)),
				mdp5_read(mdp5_kms, REG_MDP5_SMP_ALLOC_W_REG(8)),
				mdp5_read(mdp5_kms, REG_MDP5_SMP_ALLOC_W_REG(9)));
		}
		mdp5_vbif_ot_kickoff(mdp5_kms);
		pr_info("talkman-mdss: kickoff dma0 xy=%08x outxy=%08x stile=%08x qos=%08x vc1=%08x swst=%08x clkstat=%08x wm1=%08x wm2=%08x\n",
			mdp5_read(mdp5_kms, REG_MDP5_PIPE_SRC_XY(SSPP_DMA0)),
			mdp5_read(mdp5_kms, REG_MDP5_PIPE_OUT_XY(SSPP_DMA0)),
			mdp5_read(mdp5_kms, REG_MDP5_PIPE_STILE_FRAME_SIZE(SSPP_DMA0)),
			mdp5_read(mdp5_kms, REG_MDP5_PIPE_SRC_SIZE(SSPP_DMA0) + 0x06c),
			mdp5_read(mdp5_kms, REG_MDP5_PIPE_VC1_RANGE(SSPP_DMA0)),
			mdp5_read(mdp5_kms, REG_MDP5_PIPE_SRC_ADDR_SW_STATUS(SSPP_DMA0)),
			mdp5_read(mdp5_kms, 0x2b0),
			mdp5_read(mdp5_kms, REG_MDP5_PIPE_REQPRIO_FIFO_WM_1(SSPP_DMA0)),
			mdp5_read(mdp5_kms, REG_MDP5_PIPE_REQPRIO_FIFO_WM_2(SSPP_DMA0)));
		if (mdp5_kms->vbif)
			pr_info("talkman-mdss: kickoff vbif amem0=%08x amem1=%08x rr=%08x d8=%08x rd0=%08x\n",
				readl_relaxed(mdp5_kms->vbif + 0x160),
				readl_relaxed(mdp5_kms->vbif + 0x164),
				readl_relaxed(mdp5_kms->vbif + 0x124),
				readl_relaxed(mdp5_kms->vbif + 0x0d8),
				readl_relaxed(mdp5_kms->vbif + 0xb0));
	}

	/*
	 * 3.10 mdss_mdp_display_commit flush_kickoff (~3881):
	 * CTL_FLUSH (pipes + LM + CTL + INTF, master and slave)
	 * then wmb() then display_fnc (CTL_START). Mainline
	 * committed the encoder bit with encoder_enabled still
	 * false (START skipped), then set_encoder_state STARTed
	 * with no flush. After GDSC that leaves SSPP CURRENT=0
	 * while DSI sits CMD_MDP_BUSY. Arm START first so this
	 * commit is flush+start together.
	 */
	{
		struct drm_plane *plane;
		u32 mask = mdp_ctl_flush_mask_encoder(intf);

		if (pipeline->sintf)
			mask |= mdp_ctl_flush_mask_encoder(pipeline->sintf);
		if (pipeline->mixer)
			mask |= mdp_ctl_flush_mask_lm(pipeline->mixer->lm);
		if (pipeline->r_mixer)
			mask |= mdp_ctl_flush_mask_lm(pipeline->r_mixer->lm);
		mask |= MDP5_CTL_FLUSH_CTL;
		drm_atomic_crtc_for_each_plane(plane, encoder->crtc) {
			if (!plane->state || !plane->state->visible)
				continue;
			mask |= mdp5_plane_get_flush(plane);
		}
		mdp5_ctl_encoder_arm(ctl, pipeline, true);
		mdp5_ctl_commit(ctl, pipeline, mask, true);
		qcom_iommu_mdp_kickoff_done(&get_kms(encoder)->pdev->dev);
	}

	mdp5_cmd_enc->enabled = true;
}

void mdp5_cmd_encoder_kickoff(struct drm_encoder *encoder)
{
	struct mdp5_encoder *mdp5_cmd_enc = to_mdp5_encoder(encoder);
	struct mdp5_pipeline *pipeline;
	struct mdp5_ctl *ctl;

	if (!encoder || !encoder->crtc)
		return;

	ctl = mdp5_cmd_enc->ctl;
	pipeline = mdp5_crtc_get_pipeline(encoder->crtc);
	if (!ctl || !pipeline || !pipeline->intf)
		return;
	if (pipeline->intf->mode != MDP5_INTF_DSI_MODE_COMMAND)
		return;

	/*
	 * encoder enable STARTs while the PHY is still clamped
	 * (unclamp is DSI atomic_enable). Kick again after unclamp
	 * or the AMOLED keeps the last GRAM frame.
	 */
	pr_info("talkman-mdss: LP2 cmd kickoff\n");
	mdp5_ctl_set_encoder_state(ctl, pipeline, true);
}
#endif /* CONFIG_DRM_MSM_DSI */
