// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#include <drm/drm_crtc.h>
#include <drm/drm_probe_helper.h>

#include "mdp5_kms.h"
#include "dsi/dsi.h"

#ifdef CONFIG_DRM_MSM_DSI

static struct mdp5_kms *get_kms(struct drm_encoder *encoder)
{
	struct msm_drm_private *priv = encoder->dev->dev_private;
	return to_mdp5_kms(to_mdp_kms(priv->kms));
}

#define VSYNC_CLK_RATE 19200000

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

	mdp5_ctl_commit(ctl, pipeline, mdp_ctl_flush_mask_encoder(intf), true);

	mdp5_ctl_set_encoder_state(ctl, pipeline, true);

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
