// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <dt-bindings/sound/tas2552.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include "common.h"
#include "qdsp6/q6afe.h"

#define SLIM_MAX_TX_PORTS 16
#define SLIM_MAX_RX_PORTS 16
#define WCD9335_DEFAULT_MCLK_RATE	9600000
#define MI2S_BCLK_RATE			1536000
#define I2S_PCM_SEL_I2S		0
#define I2S_PCM_SEL_OFFSET	1
/* TAS2552 PGA: −7 dB + 1 dB/step. 22 = +15 dB. */
#define TAS2552_PGA_P15DB	22

struct apq8096_data {
	void __iomem *quat_mux;
};

static int apq8096_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int msm_snd_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	u32 rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	u32 rx_ch_cnt = 0, tx_ch_cnt = 0;
	int ret = 0;

	ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);
	if (ret != 0 && ret != -ENOTSUPP) {
		pr_err("failed to get codec chan map, err:%d\n", ret);
		goto end;
	} else if (ret == -ENOTSUPP) {
		return 0;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
						  rx_ch_cnt, rx_ch);
	else
		ret = snd_soc_dai_set_channel_map(cpu_dai, tx_ch_cnt, tx_ch,
						  0, NULL);
	if (ret != 0 && ret != -ENOTSUPP)
		pr_err("Failed to set cpu chan map, err:%d\n", ret);
	else if (ret == -ENOTSUPP)
		ret = 0;
end:
	return ret;
}

static void apq8096_kctl_set(struct snd_soc_card *card, const char *name,
			     long val)
{
	struct snd_kcontrol *kctl;
	struct snd_ctl_elem_value ucontrol = {};

	kctl = snd_soc_card_get_kcontrol(card, name);
	if (!kctl)
		return;

	ucontrol.value.integer.value[0] = val;
	kctl->put(kctl, &ucontrol);
}

static int apq8096_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct apq8096_data *priv = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	if (cpu_dai->id != QUATERNARY_MI2S_RX)
		return 0;

	if (!priv || !priv->quat_mux) {
		dev_err(rtd->card->dev, "missing lpaif_quat_mode_muxsel\n");
		return -EINVAL;
	}

	/* Select I2S on the QUAT LPAIF mux (PCM is the other mode). */
	iowrite32(I2S_PCM_SEL_I2S << I2S_PCM_SEL_OFFSET, priv->quat_mux);

	ret = snd_soc_dai_set_sysclk(cpu_dai, LPAIF_BIT_CLK,
				     MI2S_BCLK_RATE, 0);
	if (ret)
		return ret;

	snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
	if (!snd_soc_dai_is_dummy(codec_dai)) {
		snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_BC_FC |
					       SND_SOC_DAIFMT_NB_NF |
					       SND_SOC_DAIFMT_I2S);
		snd_soc_dai_set_sysclk(codec_dai, TAS2552_PLL_CLKIN_BCLK,
				       MI2S_BCLK_RATE, SND_SOC_CLOCK_IN);
	}

	apq8096_kctl_set(rtd->card,
			 "QUAT_MI2S_RX Audio Mixer MultiMedia1", 1);
	apq8096_kctl_set(rtd->card, "Speaker Driver Playback Volume",
			 TAS2552_PGA_P15DB);

	return 0;
}

static void apq8096_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	if (cpu_dai->id == QUATERNARY_MI2S_RX)
		snd_soc_dai_set_sysclk(cpu_dai, LPAIF_BIT_CLK, 0, 0);
}

static const struct snd_soc_ops apq8096_ops = {
	.startup = apq8096_startup,
	.shutdown = apq8096_shutdown,
	.hw_params = msm_snd_hw_params,
};

static int apq8096_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);

	/*
	 * Codec SLIMBUS configuration
	 * RX1, RX2, RX3, RX4, RX5, RX6, RX7, RX8, RX9, RX10, RX11, RX12, RX13
	 * TX1, TX2, TX3, TX4, TX5, TX6, TX7, TX8, TX9, TX10, TX11, TX12, TX13
	 * TX14, TX15, TX16
	 */
	unsigned int rx_ch[SLIM_MAX_RX_PORTS] = {144, 145, 146, 147, 148, 149,
					150, 151, 152, 153, 154, 155, 156};
	unsigned int tx_ch[SLIM_MAX_TX_PORTS] = {128, 129, 130, 131, 132, 133,
					    134, 135, 136, 137, 138, 139,
					    140, 141, 142, 143};

	snd_soc_dai_set_channel_map(codec_dai, ARRAY_SIZE(tx_ch),
					tx_ch, ARRAY_SIZE(rx_ch), rx_ch);

	snd_soc_dai_set_sysclk(codec_dai, 0, WCD9335_DEFAULT_MCLK_RATE,
				SNDRV_PCM_STREAM_PLAYBACK);

	return 0;
}

static void apq8096_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->be_hw_params_fixup = apq8096_be_hw_params_fixup;
			link->ops = &apq8096_ops;
			if (link->id != QUATERNARY_MI2S_RX)
				link->init = apq8096_init;
		}
	}
}

static int apq8096_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct apq8096_data *priv;
	struct resource *muxsel;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	muxsel = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					      "lpaif_quat_mode_muxsel");
	if (muxsel) {
		priv->quat_mux = devm_ioremap_resource(dev, muxsel);
		if (IS_ERR(priv->quat_mux))
			return PTR_ERR(priv->quat_mux);
	}

	card->driver_name = "apq8096";
	card->dev = dev;
	card->owner = THIS_MODULE;
	snd_soc_card_set_drvdata(card, priv);
	dev_set_drvdata(dev, card);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	apq8096_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id msm_snd_apq8096_dt_match[] = {
	{.compatible = "qcom,apq8096-sndcard"},
	{}
};

MODULE_DEVICE_TABLE(of, msm_snd_apq8096_dt_match);

static struct platform_driver msm_snd_apq8096_driver = {
	.probe  = apq8096_platform_probe,
	.driver = {
		.name = "msm-snd-apq8096",
		.of_match_table = msm_snd_apq8096_dt_match,
	},
};
module_platform_driver(msm_snd_apq8096_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("APQ8096 ASoC Machine Driver");
MODULE_LICENSE("GPL");
