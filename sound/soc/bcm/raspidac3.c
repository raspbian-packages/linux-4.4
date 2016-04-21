/*
 * ASoC Driver for RaspiDAC v3
 *
 * Author:	Jan Grulich <jan@grulich.eu>
 *		Copyright 2015
 *              based on code by Daniel Matuschek <daniel@hifiberry.com>
 *		based on code by Florian Meier <florian.meier@koalo.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/soc-dapm.h>

#include "../codecs/pcm512x.h"
#include "../codecs/tpa6130a2.h"

/* sound card init */
static int snd_rpi_raspidac3_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_codec *codec = rtd->codec;
	snd_soc_update_bits(codec, PCM512x_GPIO_EN, 0x08, 0x08);
	snd_soc_update_bits(codec, PCM512x_GPIO_OUTPUT_4, 0xf, 0x02);
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08,0x00);

	ret = snd_soc_limit_volume(card, "Digital Playback Volume", 207);
	if (ret < 0)
		dev_warn(card->dev, "Failed to set volume limit: %d\n", ret);
	else {
		struct snd_kcontrol *kctl;

		ret = tpa6130a2_add_controls(codec);
		if (ret < 0)
			dev_warn(card->dev, "Failed to add TPA6130A2 controls: %d\n",
				 ret);
		ret = snd_soc_limit_volume(card,
					   "TPA6130A2 Headphone Playback Volume",
					   54);
		if (ret < 0)
			dev_warn(card->dev, "Failed to set TPA6130A2 volume limit: %d\n",
				 ret);
		kctl = snd_soc_card_get_kcontrol(card,
						 "TPA6130A2 Headphone Playback Volume");
		if (kctl) {
			strcpy(kctl->id.name, "Headphones Playback Volume");
			/* disable the volume dB scale so alsamixer works */
			kctl->vd[0].access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		}

		kctl = snd_soc_card_get_kcontrol(card,
						 "TPA6130A2 Headphone Playback Switch");
		if (kctl)
			strcpy(kctl->id.name, "Headphones Playback Switch");
	}

	return 0;
}

/* set hw parameters */
static int snd_rpi_raspidac3_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	unsigned int sample_bits =
		snd_pcm_format_physical_width(params_format(params));

	return snd_soc_dai_set_bclk_ratio(cpu_dai, sample_bits * 2);
}

/* startup */
static int snd_rpi_raspidac3_startup(struct snd_pcm_substream *substream) {
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08,0x08);
	tpa6130a2_stereo_enable(codec, 1);
	return 0;
}

/* shutdown */
static void snd_rpi_raspidac3_shutdown(struct snd_pcm_substream *substream) {
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08,0x00);
	tpa6130a2_stereo_enable(codec, 0);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_raspidac3_ops = {
	.hw_params = snd_rpi_raspidac3_hw_params,
	.startup = snd_rpi_raspidac3_startup,
	.shutdown = snd_rpi_raspidac3_shutdown,
};

/* interface setup */
static struct snd_soc_dai_link snd_rpi_raspidac3_dai[] = {
{
	.name		= "RaspiDAC Rev.3x",
	.stream_name	= "RaspiDAC HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "pcm512x-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "pcm512x.1-004c",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_raspidac3_ops,
	.init		= snd_rpi_raspidac3_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_raspidac3 = {
	.name         = "RaspiDAC Rev.3x HiFi Audio Card",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_raspidac3_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_raspidac3_dai),
};

/* sound card test */
static int snd_rpi_raspidac3_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_raspidac3.dev = &pdev->dev;

	if (pdev->dev.of_node) {
	    struct device_node *i2s_node;
	    struct snd_soc_dai_link *dai = &snd_rpi_raspidac3_dai[0];
	    i2s_node = of_parse_phandle(pdev->dev.of_node,
					"i2s-controller", 0);

	    if (i2s_node) {
		dai->cpu_dai_name = NULL;
		dai->cpu_of_node = i2s_node;
		dai->platform_name = NULL;
		dai->platform_of_node = i2s_node;
	    }
	}

	ret = snd_soc_register_card(&snd_rpi_raspidac3);
	if (ret)
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

/* sound card disconnect */
static int snd_rpi_raspidac3_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_raspidac3);
}

static const struct of_device_id raspidac3_of_match[] = {
	{ .compatible = "jg,raspidacv3", },
	{},
};
MODULE_DEVICE_TABLE(of, raspidac3_of_match);

/* sound card platform driver */
static struct platform_driver snd_rpi_raspidac3_driver = {
	.driver = {
		.name   = "snd-rpi-raspidac3",
		.owner  = THIS_MODULE,
		.of_match_table = raspidac3_of_match,
	},
	.probe          = snd_rpi_raspidac3_probe,
	.remove         = snd_rpi_raspidac3_remove,
};

module_platform_driver(snd_rpi_raspidac3_driver);

MODULE_AUTHOR("Jan Grulich <jan@grulich.eu>");
MODULE_DESCRIPTION("ASoC Driver for RaspiDAC Rev.3x");
MODULE_LICENSE("GPL v2");
