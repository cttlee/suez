/* Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mt_afe_def.h"
#include "mt_afe_reg.h"
#include "mt_afe_clk.h"
#include "mt_afe_control.h"
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <sound/soc.h>

enum mt_afe_ul1_capture_mux {
	UL1_MTK_INTERFACE = 0,
	UL1_I2S2,
	UL1_I2S0
};

enum mt_afe_ul1_i2s_mclk_mode {
	MCLK_INDEPENDENT = 0,
	MCLK_SHARE_FROM_I2S0,
	MCLK_SHARE_FROM_I2S0_MAX,
	MCLK_SHARE_FROM_I2S1,
	MCLK_SHARE_FROM_I2S1_MAX
};

struct mt_pcm_capture_priv {
	bool prepared;
	bool enable_i2s2_low_jitter;
	bool enable_i2s0_low_jitter;
	bool enable_sram;
	unsigned int mono_type;
	unsigned int capture_mux;
	unsigned int i2s_clock_mode;
	unsigned int i2s_mclk_mode;
	unsigned int i2s_mclk_divider_type;
};


/*
 *    function implementation
 */
static int mt_pcm_capture_close(struct snd_pcm_substream *substream);

static void mt_pcm_capture_start_audio_hw(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt_pcm_capture_priv *priv = snd_soc_platform_get_drvdata(rtd->platform);
	struct mt_afe_irq_status irq_status;
	struct timespec curr_tstamp;

	pr_debug("%s\n", __func__);

	mt_afe_set_memif_fetch_format(MT_AFE_DIGITAL_BLOCK_MEM_VUL, MT_AFE_MEMIF_16_BIT);
	mt_afe_set_sample_rate(MT_AFE_DIGITAL_BLOCK_MEM_VUL, runtime->rate);
	mt_afe_set_channels(MT_AFE_DIGITAL_BLOCK_MEM_VUL, runtime->channels);
	mt_afe_set_out_conn_format(MT_AFE_CONN_OUTPUT_16BIT, INTER_CONN_O09);
	mt_afe_set_out_conn_format(MT_AFE_CONN_OUTPUT_16BIT, INTER_CONN_O10);

	if (priv->capture_mux == UL1_I2S2) {
		mt_afe_set_i2s_adc_in(runtime->rate, priv->i2s_clock_mode);
		if (!mt_afe_get_memory_path_state(MT_AFE_DIGITAL_BLOCK_I2S_IN_ADC)) {
			mt_afe_enable_memory_path(MT_AFE_DIGITAL_BLOCK_I2S_IN_ADC);
			mt_afe_enable_i2s_adc();
		} else {
			mt_afe_enable_memory_path(MT_AFE_DIGITAL_BLOCK_I2S_IN_ADC);
		}
	} else if (priv->capture_mux == UL1_I2S0) {
		mt_afe_set_sample_rate(MT_AFE_DIGITAL_BLOCK_MEM_I2S, runtime->rate);
		mt_afe_set_2nd_i2s_in(MT_AFE_I2S_WLEN_16BITS,
				MT_AFE_I2S_SRC_MASTER_MODE,
				MT_AFE_BCK_INV_NO_INVERSE,
				priv->i2s_clock_mode);
		if (!mt_afe_get_memory_path_state(MT_AFE_DIGITAL_BLOCK_I2S_IN_2)) {
			mt_afe_enable_memory_path(MT_AFE_DIGITAL_BLOCK_I2S_IN_2);
			mt_afe_enable_2nd_i2s_in();
		} else {
			mt_afe_enable_memory_path(MT_AFE_DIGITAL_BLOCK_I2S_IN_2);
		}
	} else {
		mt_afe_set_mtkif_adc_in(runtime->rate);
		if (!mt_afe_get_memory_path_state(MT_AFE_DIGITAL_BLOCK_I2S_IN_ADC)) {
			mt_afe_enable_memory_path(MT_AFE_DIGITAL_BLOCK_I2S_IN_ADC);
			mt_afe_enable_mtkif_adc();
		} else {
			mt_afe_enable_memory_path(MT_AFE_DIGITAL_BLOCK_I2S_IN_ADC);
		}
	}

	if (runtime->channels == 1) {
		if (priv->mono_type == R_MONO)
			mt_afe_set_mono_type(MT_AFE_DIGITAL_BLOCK_MEM_VUL, R_MONO);
		else
			mt_afe_set_mono_type(MT_AFE_DIGITAL_BLOCK_MEM_VUL, L_MONO);
	}

	mt_afe_enable_memory_path(MT_AFE_DIGITAL_BLOCK_MEM_VUL);


	if (priv->capture_mux == UL1_I2S0) {
		mt_afe_set_connection(INTER_CONNECT, INTER_CONN_I00, INTER_CONN_O09);
		mt_afe_set_connection(INTER_CONNECT, INTER_CONN_I01, INTER_CONN_O10);
	} else {
		mt_afe_set_connection(INTER_CONNECT, INTER_CONN_I03, INTER_CONN_O09);
		mt_afe_set_connection(INTER_CONNECT, INTER_CONN_I04, INTER_CONN_O10);
	}

	mt_afe_enable_afe(true);

	udelay(UPLINK_IRQ_DELAY_SAMPLES * 1000000 / runtime->rate);

	/* here to set interrupt */
	mt_afe_get_irq_state(MT_AFE_IRQ_MCU_MODE_IRQ2, &irq_status);
	if (likely(!irq_status.status)) {
		mt_afe_set_irq_counter(MT_AFE_IRQ_MCU_MODE_IRQ2, runtime->period_size);
		mt_afe_set_irq_rate(MT_AFE_IRQ_MCU_MODE_IRQ2, runtime->rate);
		mt_afe_set_irq_state(MT_AFE_IRQ_MCU_MODE_IRQ2, true);
		snd_pcm_gettime(runtime, (struct timespec *)&curr_tstamp);
		pr_debug("%s curr_tstamp %ld %ld\n", __func__, curr_tstamp.tv_sec,
			 curr_tstamp.tv_nsec);
	} else {
		pr_debug("%s IRQ2_MCU_MODE is enabled, use original irq2 interrupt mode\n",
			 __func__);
	}
}

static void mt_pcm_capture_stop_audio_hw(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt_pcm_capture_priv *priv = snd_soc_platform_get_drvdata(rtd->platform);

	pr_debug("%s\n", __func__);

	if (priv->capture_mux == UL1_I2S0) {
		mt_afe_disable_memory_path(MT_AFE_DIGITAL_BLOCK_I2S_IN_2);
		if (!mt_afe_get_memory_path_state(MT_AFE_DIGITAL_BLOCK_I2S_IN_2))
			mt_afe_disable_2nd_i2s_in();
	} else {
		mt_afe_disable_memory_path(MT_AFE_DIGITAL_BLOCK_I2S_IN_ADC);
		if (!mt_afe_get_memory_path_state(MT_AFE_DIGITAL_BLOCK_I2S_IN_ADC)) {
			if (priv->capture_mux == UL1_I2S2)
				mt_afe_disable_i2s_adc();
			else
				mt_afe_disable_mtkif_adc();
		}
	}

	mt_afe_disable_memory_path(MT_AFE_DIGITAL_BLOCK_MEM_VUL);

	/* here to set interrupt */
	mt_afe_set_irq_state(MT_AFE_IRQ_MCU_MODE_IRQ2, false);

	/* here to turn off digital part */
	if (priv->capture_mux == UL1_I2S0) {
		mt_afe_set_connection(INTER_DISCONNECT, INTER_CONN_I00, INTER_CONN_O09);
		mt_afe_set_connection(INTER_DISCONNECT, INTER_CONN_I01, INTER_CONN_O10);
	} else {
		mt_afe_set_connection(INTER_DISCONNECT, INTER_CONN_I03, INTER_CONN_O09);
		mt_afe_set_connection(INTER_DISCONNECT, INTER_CONN_I04, INTER_CONN_O10);
	}

	mt_afe_enable_afe(false);
}

static struct snd_pcm_hardware mt_pcm_capture_hardware = {
	.info = (SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID),
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SOC_NORMAL_USE_RATE,
	.rate_min = SOC_NORMAL_USE_RATE_MIN,
	.rate_max = SOC_NORMAL_USE_RATE_MAX,
	.channels_min = SOC_NORMAL_USE_CHANNELS_MIN,
	.channels_max = SOC_NORMAL_USE_CHANNELS_MAX,
	.buffer_bytes_max = UL1_MAX_BUFFER_SIZE,
	.period_bytes_min = SOC_NORMAL_USE_PERIOD_SIZE_MIN,
	.period_bytes_max = (UL1_MAX_BUFFER_SIZE / SOC_NORMAL_USE_PERIODS_MIN),
	.periods_min = SOC_NORMAL_USE_PERIODS_MIN,
	.periods_max = SOC_NORMAL_USE_PERIODS_MAX,
	.fifo_size = 0,
};

static struct snd_pcm_hw_constraint_list mt_pcm_capture_constraints_rates = {
	.count = ARRAY_SIZE(soc_normal_supported_sample_rates),
	.list = soc_normal_supported_sample_rates,
};

static int mt_pcm_capture_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret = 0;

	pr_debug("%s\n", __func__);

	snd_soc_set_runtime_hwparams(substream, &mt_pcm_capture_hardware);

	pr_debug("%s runtime->hw->rates = 0x%x mt_pcm_capture_hardware = %p\n ",
		 __func__, runtime->hw.rates, &mt_pcm_capture_hardware);

	ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &mt_pcm_capture_constraints_rates);
	if (unlikely(ret < 0))
		pr_err("%s snd_pcm_hw_constraint_list failed %d\n", __func__, ret);

	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);

	if (unlikely(ret < 0))
		pr_err("%s snd_pcm_hw_constraint_integer failed %d\n", __func__, ret);

	/* here open audio clocks */
	mt_afe_main_clk_on();
	mt_afe_adc_clk_on();

	if (unlikely(ret < 0)) {
		pr_err("%s mt_pcm_capture_close\n", __func__);
		mt_pcm_capture_close(substream);
		return ret;
	}

	pr_debug("%s return\n", __func__);
	return 0;
}

static int mt_pcm_capture_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt_pcm_capture_priv *priv = snd_soc_platform_get_drvdata(rtd->platform);

	pr_debug("%s\n", __func__);

	if (priv->prepared) {
		if (priv->enable_i2s2_low_jitter) {
			mt_afe_disable_apll_div_power(priv->i2s_mclk_divider_type, runtime->rate);
			mt_afe_disable_apll_div_power(MT_AFE_ENGEN, runtime->rate);
			mt_afe_disable_apll_tuner(runtime->rate);
			mt_afe_disable_apll(runtime->rate);
			priv->enable_i2s2_low_jitter = false;
		}
		if (priv->enable_i2s0_low_jitter) {
			mt_afe_disable_apll_div_power(priv->i2s_mclk_divider_type, runtime->rate);
			mt_afe_disable_apll_div_power(MT_AFE_ENGEN, runtime->rate);
			mt_afe_disable_apll_tuner(runtime->rate);
			mt_afe_disable_apll(runtime->rate);
			priv->enable_i2s0_low_jitter = false;
		}
		priv->prepared = false;
	}

	mt_afe_adc_clk_off();
	mt_afe_main_clk_off();

	return 0;
}

static int mt_pcm_capture_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *hw_params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt_pcm_capture_priv *priv = snd_soc_platform_get_drvdata(rtd->platform);
	int ret = 0;
	size_t buffer_size = params_buffer_bytes(hw_params);

	pr_debug("%s\n", __func__);
#ifdef AUDIO_USE_INFRA_SRAM
	if (buffer_size > mt_afe_get_infra_capture_sram_size()) {
		pr_debug("%s force to use dram for size %zu\n", __func__, buffer_size);
		priv->enable_sram = false;
	} else {
		priv->enable_sram = true;
	}
#else
	priv->enable_sram = false;
#endif

	if (priv->enable_sram) {
		substream->runtime->dma_bytes = buffer_size;
		substream->runtime->dma_area = (unsigned char *)mt_afe_get_infra_sram_base_ptr();
		substream->runtime->dma_addr = mt_afe_get_infra_sram_phy_addr();
	} else {
		ret = snd_pcm_lib_malloc_pages(substream, buffer_size);
		mt_afe_emi_clk_on();
	}

	if (likely(ret >= 0))
		mt_afe_init_dma_buffer(MT_AFE_MEM_CTX_VUL, runtime);
	else
		pr_err("%s snd_pcm_lib_malloc_pages fail %d\n", __func__, ret);

	pr_debug("%s dma_bytes = %zu dma_area = %p dma_addr = 0x%llx\n",
		 __func__, runtime->dma_bytes, runtime->dma_area,
		 (unsigned long long)runtime->dma_addr);
	return ret;
}

static int mt_pcm_capture_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt_pcm_capture_priv *priv = snd_soc_platform_get_drvdata(rtd->platform);
	int ret = 0;

	pr_debug("%s\n", __func__);

	if (!priv->enable_sram && runtime->dma_area) {
		ret = snd_pcm_lib_free_pages(substream);
		mt_afe_emi_clk_off();
	}
	return ret;
}

static int mt_pcm_capture_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt_pcm_capture_priv *priv = snd_soc_platform_get_drvdata(rtd->platform);

	if (priv->prepared)
		return 0;

	if (priv->i2s_clock_mode == MT_AFE_LOW_JITTER_CLOCK && priv->capture_mux == UL1_I2S2) {
		if (priv->i2s_mclk_mode == MCLK_SHARE_FROM_I2S0 ||
		    priv->i2s_mclk_mode == MCLK_SHARE_FROM_I2S0_MAX)
			priv->i2s_mclk_divider_type = MT_AFE_I2S0;
		else if (priv->i2s_mclk_mode == MCLK_SHARE_FROM_I2S1 ||
			 priv->i2s_mclk_mode == MCLK_SHARE_FROM_I2S1_MAX)
			priv->i2s_mclk_divider_type = MT_AFE_I2S1;
		else
			priv->i2s_mclk_divider_type = MT_AFE_I2S2;

		mt_afe_enable_apll(runtime->rate);
		mt_afe_enable_apll_tuner(runtime->rate);

		if (priv->i2s_mclk_mode == MCLK_SHARE_FROM_I2S0_MAX ||
		    priv->i2s_mclk_mode == MCLK_SHARE_FROM_I2S1_MAX)
			mt_afe_set_mclk(priv->i2s_mclk_divider_type,
				(runtime->rate % 8000) ? 176400 : 192000);
		else
			mt_afe_set_mclk(priv->i2s_mclk_divider_type, runtime->rate);

		mt_afe_set_mclk(MT_AFE_ENGEN, runtime->rate);
		mt_afe_enable_apll_div_power(priv->i2s_mclk_divider_type, runtime->rate);
		mt_afe_enable_apll_div_power(MT_AFE_ENGEN, runtime->rate);

		priv->enable_i2s2_low_jitter = true;
	} else if (priv->i2s_clock_mode == MT_AFE_LOW_JITTER_CLOCK &&
		priv->capture_mux == UL1_I2S0) {
		if (priv->i2s_mclk_mode == MCLK_SHARE_FROM_I2S1 ||
		    priv->i2s_mclk_mode == MCLK_SHARE_FROM_I2S1_MAX)
			priv->i2s_mclk_divider_type = MT_AFE_I2S1;
		else
			priv->i2s_mclk_divider_type = MT_AFE_I2S0;

		mt_afe_enable_apll(runtime->rate);
		mt_afe_enable_apll_tuner(runtime->rate);

		if (priv->i2s_mclk_mode == MCLK_SHARE_FROM_I2S1_MAX)
			mt_afe_set_mclk(priv->i2s_mclk_divider_type,
				(runtime->rate % 8000) ? 176400 : 192000);
		else
			mt_afe_set_mclk(priv->i2s_mclk_divider_type, runtime->rate);

		mt_afe_set_mclk(MT_AFE_ENGEN, runtime->rate);
		mt_afe_enable_apll_div_power(priv->i2s_mclk_divider_type, runtime->rate);
		mt_afe_enable_apll_div_power(MT_AFE_ENGEN, runtime->rate);

		priv->enable_i2s0_low_jitter = true;
	}

	priv->prepared = true;

	return 0;
}

static int mt_pcm_capture_start(struct snd_pcm_substream *substream)
{
	pr_debug("%s\n", __func__);
	mt_afe_add_ctx_substream(MT_AFE_MEM_CTX_VUL, substream);
	mt_pcm_capture_start_audio_hw(substream);
	return 0;
}

static int mt_pcm_capture_stop(struct snd_pcm_substream *substream)
{
	pr_debug("%s\n", __func__);
	mt_pcm_capture_stop_audio_hw(substream);
	mt_afe_reset_dma_buffer(MT_AFE_MEM_CTX_VUL);
	mt_afe_remove_ctx_substream(MT_AFE_MEM_CTX_VUL);
	return 0;
}

static int mt_pcm_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
	pr_debug("%s cmd=%d\n", __func__, cmd);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		return mt_pcm_capture_start(substream);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return mt_pcm_capture_stop(substream);
	}
	return -EINVAL;
}

static int mtk_pcm_capture_copy(struct snd_pcm_substream *substream,
				int channel, snd_pcm_uframes_t pos,
				void __user *dst, snd_pcm_uframes_t count)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mt_pcm_capture_priv *priv = snd_soc_platform_get_drvdata(rtd->platform);
	char *hwbuf = runtime->dma_area + frames_to_bytes(runtime, pos);

	if (priv->enable_sram) {
		if (copy_to_user_fromio(dst, hwbuf, frames_to_bytes(runtime, count))) {
			pr_err("%s copy_to_user_fromio failed!\n", __func__);
			return -EFAULT;
		}
	} else {
		if (copy_to_user(dst, hwbuf, frames_to_bytes(runtime, count))) {
			pr_err("%s copy_to_user failed!\n", __func__);
			return -EFAULT;
		}
	}
	return 0;
}

static snd_pcm_uframes_t mt_pcm_capture_pointer(struct snd_pcm_substream *substream)
{
	return mt_afe_update_hw_ptr(MT_AFE_MEM_CTX_VUL);
}

static struct snd_pcm_ops mt_pcm_capture_ops = {
	.open = mt_pcm_capture_open,
	.close = mt_pcm_capture_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = mt_pcm_capture_hw_params,
	.hw_free = mt_pcm_capture_hw_free,
	.prepare = mt_pcm_capture_prepare,
	.trigger = mt_pcm_capture_trigger,
	.copy = mtk_pcm_capture_copy,
	.pointer = mt_pcm_capture_pointer,
};

static const char *const capture_mono_type_function[] = {
	ENUM_TO_STR(L_MONO),
	ENUM_TO_STR(R_MONO),
};

static int capture_mono_type_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct mt_pcm_capture_priv *priv = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = priv->mono_type;
	return 0;
}

static int capture_mono_type_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct mt_pcm_capture_priv *priv = snd_soc_component_get_drvdata(component);

	priv->mono_type = ucontrol->value.integer.value[0];
	return 0;
}

static const char *const mt_pcm_ul1_capture_mux_function[] = {
	ENUM_TO_STR(UL1_MTK_INTERFACE),
	ENUM_TO_STR(UL1_I2S2),
	ENUM_TO_STR(UL1_I2S0)
};

static int ul1_capture_mux_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct mt_pcm_capture_priv *priv = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = priv->capture_mux;
	return 0;
}

static int ul1_capture_mux_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct mt_pcm_capture_priv *priv = snd_soc_component_get_drvdata(component);

	priv->capture_mux = ucontrol->value.integer.value[0];
	return 0;
}

static const char *const mt_pcm_ul1_i2s_clock_function[] = { "Normal", "Low Jitter" };

static int ul1_i2s_clock_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct mt_pcm_capture_priv *priv = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = priv->i2s_clock_mode;
	return 0;
}

static int ul1_i2s_clock_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct mt_pcm_capture_priv *priv = snd_soc_component_get_drvdata(component);

	priv->i2s_clock_mode = ucontrol->value.integer.value[0];
	return 0;
}

static const char *const mt_pcm_ul1_i2s_mclk_function[] = {
	ENUM_TO_STR(MCLK_INDEPENDENT),
	ENUM_TO_STR(MCLK_SHARE_FROM_I2S0),
	ENUM_TO_STR(MCLK_SHARE_FROM_I2S0_MAX),
	ENUM_TO_STR(MCLK_SHARE_FROM_I2S1),
	ENUM_TO_STR(MCLK_SHARE_FROM_I2S1_MAX),
};

static int ul1_i2s_mclk_mode_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct mt_pcm_capture_priv *priv = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = priv->i2s_mclk_mode;
	return 0;
}

static int ul1_i2s_mclk_mode_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct mt_pcm_capture_priv *priv = snd_soc_component_get_drvdata(component);

	priv->i2s_mclk_mode = ucontrol->value.integer.value[0];
	return 0;
}

static const struct soc_enum mt_pcm_caprure_control_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(capture_mono_type_function),
			capture_mono_type_function),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mt_pcm_ul1_capture_mux_function),
			mt_pcm_ul1_capture_mux_function),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mt_pcm_ul1_i2s_clock_function),
			mt_pcm_ul1_i2s_clock_function),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mt_pcm_ul1_i2s_mclk_function),
			mt_pcm_ul1_i2s_mclk_function),
};

static const struct snd_kcontrol_new mt_pcm_capture_controls[] = {
	SOC_ENUM_EXT("Capture_Mono_Type", mt_pcm_caprure_control_enum[0],
		capture_mono_type_get, capture_mono_type_set),
	SOC_ENUM_EXT("UL1_Capture_Mux", mt_pcm_caprure_control_enum[1],
		ul1_capture_mux_get, ul1_capture_mux_set),
	SOC_ENUM_EXT("UL1_I2S_Clock", mt_pcm_caprure_control_enum[2],
		ul1_i2s_clock_get, ul1_i2s_clock_set),
	SOC_ENUM_EXT("UL1_I2S_Mclk", mt_pcm_caprure_control_enum[3],
		ul1_i2s_mclk_mode_get, ul1_i2s_mclk_mode_set),
};

static int mt_pcm_capture_probe(struct snd_soc_platform *platform)
{
	snd_soc_add_platform_controls(platform, mt_pcm_capture_controls,
				ARRAY_SIZE(mt_pcm_capture_controls));
	return 0;
}

static int mt_pcm_capture_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	struct snd_pcm *pcm = rtd->pcm;
	int ret = 0;

	ret = snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, card->dev,
						    UL1_MAX_BUFFER_SIZE, UL1_MAX_BUFFER_SIZE);
	return ret;
}

static void mt_pcm_capture_pcm_free(struct snd_pcm *pcm)
{
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

static struct snd_soc_platform_driver mt_pcm_capture_platform = {
	.ops = &mt_pcm_capture_ops,
	.probe = mt_pcm_capture_probe,
	.pcm_new = mt_pcm_capture_pcm_new,
	.pcm_free = mt_pcm_capture_pcm_free,
};

static int mt_pcm_capture_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt_pcm_capture_priv *priv;
	int rc;

	pr_debug("%s dev name %s\n", __func__, dev_name(dev));

	rc = dma_set_mask(dev, DMA_BIT_MASK(33));
	if (rc)
		return rc;

	if (dev->of_node) {
		dev_set_name(dev, "%s", MT_SOC_UL1_PCM);
		pr_debug("%s set dev name %s\n", __func__, dev_name(dev));
	}

	priv = devm_kzalloc(dev, sizeof(struct mt_pcm_capture_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->i2s_clock_mode = MT_AFE_LOW_JITTER_CLOCK;
	priv->i2s_mclk_divider_type = MT_AFE_I2S2;

	dev_set_drvdata(dev, priv);

	return snd_soc_register_platform(dev, &mt_pcm_capture_platform);
}

static int mt_pcm_capture_dev_remove(struct platform_device *pdev)
{
	pr_debug("%s\n", __func__);
	snd_soc_unregister_platform(&pdev->dev);
	return 0;
}

static const struct of_device_id mt_pcm_capture_dt_match[] = {
	{.compatible = "mediatek," MT_SOC_UL1_PCM,},
	{}
};

MODULE_DEVICE_TABLE(of, mt_pcm_capture_dt_match);

static struct platform_driver mt_pcm_capture_driver = {
	.driver = {
		   .name = MT_SOC_UL1_PCM,
		   .owner = THIS_MODULE,
		   .of_match_table = mt_pcm_capture_dt_match,
		   },
	.probe = mt_pcm_capture_dev_probe,
	.remove = mt_pcm_capture_dev_remove,
};

module_platform_driver(mt_pcm_capture_driver);

MODULE_DESCRIPTION("AFE PCM Capture platform driver");
MODULE_LICENSE("GPL");
