// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 The Linux Foundation. All rights reserved.
 */

#include "mdp5_kms.h"

/*
 * As of now, there are only 2 combinations possible for source split:
 *
 * Left | Right
 * -----|------
 *  LM0 | LM1
 *  LM2 | LM5
 *
 */
static int lm_right_pair[] = { 1, -1, 5, -1, -1, -1 };

static int get_right_pair_idx(struct mdp5_kms *mdp5_kms, int lm)
{
	int i;
	int pair_lm;

	pair_lm = lm_right_pair[lm];
	if (pair_lm < 0)
		return -EINVAL;

	for (i = 0; i < mdp5_kms->num_hwmixers; i++) {
		struct mdp5_hw_mixer *mixer = mdp5_kms->hwmixers[i];

		if (mixer->lm == pair_lm)
			return mixer->idx;
	}

	return -1;
}

int mdp5_mixer_assign(struct drm_atomic_commit *s, struct drm_crtc *crtc,
		      uint32_t caps, struct mdp5_hw_mixer **mixer,
		      struct mdp5_hw_mixer **r_mixer)
{
	struct msm_drm_private *priv = s->dev->dev_private;
	struct mdp5_kms *mdp5_kms = to_mdp5_kms(to_mdp_kms(priv->kms));
	struct mdp5_global_state *global_state = mdp5_get_global_state(s);
	struct mdp5_hw_mixer_state *new_state;
	int i;

	if (IS_ERR(global_state))
		return PTR_ERR(global_state);

	new_state = &global_state->hwmixer;

	for (i = 0; i < mdp5_kms->num_hwmixers; i++) {
		struct mdp5_hw_mixer *cur = mdp5_kms->hwmixers[i];

		/*
		 * skip if already in-use by a different CRTC. If there is a
		 * mixer already assigned to this CRTC, it means this call is
		 * a request to get an additional right mixer. Assume that the
		 * existing mixer is the 'left' one, and try to see if we can
		 * get its corresponding 'right' pair.
		 */
		if (new_state->hwmixer_to_crtc[cur->idx] &&
		    new_state->hwmixer_to_crtc[cur->idx] != crtc)
			continue;

		/* skip if doesn't support some required caps: */
		if (caps & ~cur->caps)
			continue;

		if (r_mixer) {
			int pair_idx;

			pair_idx = get_right_pair_idx(mdp5_kms, cur->lm);
			if (pair_idx < 0)
				return -EINVAL;

			if (new_state->hwmixer_to_crtc[pair_idx])
				continue;
		}

		/*
		 * Prefer a pair-able LM so a CRTC can enter source-split
		 * without a full modeset. Keep the first pair: the old
		 * "any PAIR overwrites" pick took LM2/LM5 on 8994 (last
		 * PAIR in the catalog).
		 *
		 * 3.10 mdss_mdp_mixer_alloc takes the first free INTF
		 * mixer. ctl_setup errors on MIPI_CMD if mixer_left->num
		 * >= 1 ("use only DSPP0 and DSPP1 with cmd split").
		 * Cityman dual-DSI is that path: mixer 0 + mixer 1.
		 */
		if (!(*mixer) ||
		    ((cur->caps & MDP_LM_CAP_PAIR) &&
		     !((*mixer)->caps & MDP_LM_CAP_PAIR))) {
			*mixer = cur;
			if (r_mixer) {
				int pair_idx = get_right_pair_idx(mdp5_kms,
								  cur->lm);

				*r_mixer = mdp5_kms->hwmixers[pair_idx];
			}
		}
	}

	if (!(*mixer))
		return -ENOMEM;

	if (r_mixer && !(*r_mixer))
		return -ENOMEM;

	pr_info("talkman-mdss: assigning LM%d/%d to %s\n",
		(*mixer)->lm, r_mixer ? (*r_mixer)->lm : -1, crtc->name);

	new_state->hwmixer_to_crtc[(*mixer)->idx] = crtc;
	if (r_mixer) {
		DBG("assigning Right Layer Mixer %d to crtc %s", (*r_mixer)->lm,
		    crtc->name);
		new_state->hwmixer_to_crtc[(*r_mixer)->idx] = crtc;
	}

	return 0;
}

int mdp5_mixer_release(struct drm_atomic_commit *s, struct mdp5_hw_mixer *mixer)
{
	struct mdp5_global_state *global_state = mdp5_get_global_state(s);
	struct mdp5_hw_mixer_state *new_state;

	if (!mixer)
		return 0;

	if (IS_ERR(global_state))
		return PTR_ERR(global_state);

	new_state = &global_state->hwmixer;

	if (WARN_ON(!new_state->hwmixer_to_crtc[mixer->idx]))
		return -EINVAL;

	DBG("%s: release from crtc %s", mixer->name,
	    new_state->hwmixer_to_crtc[mixer->idx]->name);

	new_state->hwmixer_to_crtc[mixer->idx] = NULL;

	return 0;
}

static const char * const mixer_names[] = {
	"LM0", "LM1", "LM2", "LM3", "LM4", "LM5",
};

struct mdp5_hw_mixer *mdp5_mixer_init(struct drm_device *dev,
				      const struct mdp5_lm_instance *lm)
{
	struct mdp5_hw_mixer *mixer;

	mixer = devm_kzalloc(dev->dev, sizeof(*mixer), GFP_KERNEL);
	if (!mixer)
		return ERR_PTR(-ENOMEM);

	mixer->name = mixer_names[lm->id];
	mixer->lm = lm->id;
	mixer->caps = lm->caps;
	mixer->pp = lm->pp;
	mixer->dspp = lm->dspp;
	mixer->flush_mask = mdp_ctl_flush_mask_lm(lm->id);

	return mixer;
}
