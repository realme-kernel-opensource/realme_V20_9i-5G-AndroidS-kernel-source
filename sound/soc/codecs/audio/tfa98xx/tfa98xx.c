/*
 * tfa98xx.c   tfa98xx codec module
 *
 * Copyright 2014-2017 NXP Semiconductors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define pr_fmt(fmt) "%s(): " fmt, __func__

#include <linux/module.h>
#include <linux/i2c.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/debugfs.h>
#include <linux/version.h>
#include <linux/input.h>
#include "config.h"
#include "tfa98xx.h"
#include "tfa.h"
#include "tfa_internal.h"

#ifdef OPLUS_ARCH_EXTENDS
#undef CONFIG_DEBUG_FS
#endif /* OPLUS_ARCH_EXTENDS */
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#else
#include <linux/proc_fs.h>
#endif


/* MTK platform header file. */
#include <mtk-sp-spk-amp.h>
/* required for enum tfa9912_irq */
#include "tfa98xx_tfafieldnames.h"

#ifdef OPLUS_BUG_COMPATIBILITY
#include <linux/proc_fs.h>

bool tfa98xx_dual_spk = true;
int tfa98xx_i2cbus;

extern int main_hwid6_val;
extern bool g_speaker_resistance_fail_1;
extern bool g_speaker_resistance_fail_2;
/* add for mono spk calibration */
extern bool g_speaker_resistance_fail;
#endif /* OPLUS_BUG_COMPATIBILITY */



#define TFA98XX_VERSION	TFA98XX_API_REV_STR

#define I2C_RETRIES 50
#define I2C_RETRY_DELAY 5 /* ms */

/* Change volume selection behavior:
 * Uncomment following line to generate a profile change when updating
 * a volume control (also changes to the profile of the modified  volume
 * control)
 */
/*#define TFA98XX_ALSA_CTRL_PROF_CHG_ON_VOL	1
*/

/* Supported rates and data formats */
#define TFA98XX_RATES SNDRV_PCM_RATE_8000_48000
#define TFA98XX_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

#define TF98XX_MAX_DSP_START_TRY_COUNT	10
#define TFADSP_FLAG_CALIBRATE_DONE 1

/* data accessible by all instances */
static struct kmem_cache *tfa98xx_cache = NULL;  /* Memory pool used for DSP messages */
/* Mutex protected data */
static DEFINE_MUTEX(tfa98xx_mutex);
static LIST_HEAD(tfa98xx_device_list);
static int tfa98xx_device_count = 0;
static int tfa98xx_sync_count = 0;
static LIST_HEAD(profile_list);        /* list of user selectable profiles */
static int tfa98xx_mixer_profiles = 0; /* number of user selectable profiles */
static int tfa98xx_mixer_profile = 0;  /* current mixer profile */
static struct snd_kcontrol_new *tfa98xx_controls;
static nxpTfaContainer_t *tfa98xx_container = NULL;

static int tfa98xx_kmsg_regs = 0;
static int tfa98xx_ftrace_regs = 0;

#ifdef OPLUS_BUG_COMPATIBILITY
struct pinctrl *pinctrltfa, *pinctrltfa_2;
struct pinctrl_state *tfarstdefaul, *tfarsthigh, *tfarstlow, *tfarsthigh_2, *tfarstlow_2;
#define FW_LOADING_RETRY_TIMES 5
int fwload_cnt = 0;
int fwload_comm_cnt = 0;
static char fw_name[100] = {0};
static char fw_comm_name[100] = {0};
static char *fw_name_orig = "tfa98xx.cnt";
static int tfa_dual_start_cnt = 0;
static int tfa_wait_start_cnt = 0;
static bool tfa_spk_1_start_done = false;
static bool tfa_spk_2_start_done = false;

#define SMART_PA_RANGE_DEFAULT_MIN  5000
#define SMART_PA_RANGE_DEFAULT_MAX  7800

module_param(fw_name_orig, charp, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(fw_name_orig, "TFA98xx DSP firmware (container file) name.");
#else /* OPLUS_BUG_COMPATIBILITY */
static char *fw_name = "tfa98xx.cnt";
module_param(fw_name, charp, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(fw_name, "TFA98xx DSP firmware (container file) name.");
#endif /* OPLUS_BUG_COMPATIBILITY */

static int trace_level = 0;
module_param(trace_level, int, S_IRUGO);
MODULE_PARM_DESC(trace_level, "TFA98xx debug trace level (0=off, bits:1=verbose,2=regdmesg,3=regftrace,4=timing).");

static char *dflt_prof_name = "";
module_param(dflt_prof_name, charp, S_IRUGO);

static int no_start = 0;
module_param(no_start, int, S_IRUGO);
MODULE_PARM_DESC(no_start, "do not start the work queue; for debugging via user\n");

static int no_reset = 0;
module_param(no_reset, int, S_IRUGO);
MODULE_PARM_DESC(no_reset, "do not use the reset line; for debugging via user\n");

static int pcm_sample_format = 0;
module_param(pcm_sample_format, int, S_IRUGO);
MODULE_PARM_DESC(pcm_sample_format, "PCM sample format: 0=S16_LE, 1=S24_LE, 2=S32_LE\n");

static int pcm_no_constraint = 1;
module_param(pcm_no_constraint, int, S_IRUGO);
MODULE_PARM_DESC(pcm_no_constraint, "do not use constraints for PCM parameters\n");

static void tfa98xx_tapdet_check_update(struct tfa98xx *tfa98xx);
static int tfa98xx_get_fssel(unsigned int rate);
static void tfa98xx_interrupt_enable(struct tfa98xx *tfa98xx, bool enable);

static int get_profile_from_list(char *buf, int id);
static int get_profile_id_for_sr(int id, unsigned int rate);
static enum Tfa98xx_Error tfa9874_calibrate(struct tfa98xx *tfa98xx, int *speakerImpedance);
#ifdef OPLUS_BUG_COMPATIBILITY
static int tfa98xx_load_container(struct tfa98xx *tfa98xx);
static uint8_t g_tfa98xx_firmware_status;
#endif
#ifdef OPLUS_BUG_COMPATIBILITY
static int s_mic_mode = 3;  //default Dmic
static int s_headset_mic_mode = 2;
static int s_soundtrigger_enable = 0;

static int oppo_audio_mic_mode_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = s_mic_mode;
	return 0;
}

static int oppo_audio_mic_mode_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int oppo_audio_headset_mic_mode_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = s_headset_mic_mode;
	return 0;
}

static int oppo_audio_headset_mic_mode_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int oppo_audio_soundtrigger_info_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = s_soundtrigger_enable;
	return 0;
}

static int oppo_audio_soundtrigger_info_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static const struct snd_kcontrol_new oppo_audio_hardware_resource_controls[] = {
	SOC_SINGLE_EXT("Audio_Mic_Mode", SND_SOC_NOPM, 0, 6, 0, oppo_audio_mic_mode_get, oppo_audio_mic_mode_set),
	SOC_SINGLE_EXT("Audio_Headset_Mic_Mode", SND_SOC_NOPM, 0, 6, 0, oppo_audio_headset_mic_mode_get, oppo_audio_headset_mic_mode_set),
	SOC_SINGLE_EXT("Audio_Soundtrigger_Info", SND_SOC_NOPM, 0, 1, 0, oppo_audio_soundtrigger_info_get, oppo_audio_soundtrigger_info_set),
};
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
void oppo_create_hardware_resource_controls(struct snd_soc_codec *codec)
{
   struct snd_soc_component *cmpnt = &codec->component;
#else
void oppo_create_hardware_resource_controls(struct snd_soc_component *component)
{
   struct snd_soc_component *cmpnt = component;
#endif
	snd_soc_add_component_controls(cmpnt, oppo_audio_hardware_resource_controls, ARRAY_SIZE(oppo_audio_hardware_resource_controls));
}
#endif

#ifdef OPLUS_BUG_COMPATIBILITY
static bool is_tfa98xx_series(int rev)
{
	bool ret = false;

	if ((((rev & 0xff) == 0x80) || ((rev & 0xff) == 0x81) ||
		((rev & 0xff) == 0x92) || ((rev & 0xff) == 0x91)) ||
		((rev & 0xff) == 0x94)
		|| ((rev & 0xff) == 0x74)
	) {
		ret = true;
	}

	return ret;
}

static bool is_tfa98xx_series_v6(int rev)
{
    bool ret = false;
	if ((((rev & 0xff) == 0x80) || ((rev & 0xff) == 0x81) ||
		((rev & 0xff) == 0x92) || ((rev & 0xff) == 0x91)) ||
		((rev & 0xff) == 0x94)
		|| ((rev & 0xff) == 0x74)
	) {
		ret = true;
	}
    return ret;
}

static char const *ftm_spk_rev_text[] = {"NG", "OK"};
static const struct soc_enum ftm_spk_rev_enum = SOC_ENUM_SINGLE_EXT(2, ftm_spk_rev_text);
static int ftm_spk_rev_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int rev = 0;
	int ret;
	int retries = I2C_RETRIES;
	#if LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,19,0)
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	#else
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	#endif

	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
	#else
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
	#endif

retry:
	ret = regmap_read(tfa98xx->regmap, 0x03, &rev);
	if (ret < 0 || !is_tfa98xx_series(rev)) {
		pr_err("%s i2c error at retries left: %d, rev:0x%x\n", __func__, retries, rev);
		if (retries) {
			retries--;
			msleep(I2C_RETRY_DELAY);
			goto retry;
		}
	}

	rev =  rev & 0xff;
	pr_info("%s: ID revision 0x%04x\n", __func__, rev);
	if (is_tfa98xx_series(rev)) {
		ucontrol->value.integer.value[0] = 1;
	} else {
		ucontrol->value.integer.value[0] = 0;
	}

	return 0;
}

static int ftm_spk_rev_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static const struct snd_kcontrol_new ftm_spk_rev_controls[] = {
	SOC_ENUM_EXT("SPK_Pa Revision", ftm_spk_rev_enum,
			ftm_spk_rev_get, ftm_spk_rev_put),
};
#endif /* OPLUS_BUG_COMPATIBILITY */

struct tfa98xx_rate {
	unsigned int rate;
	unsigned int fssel;
};

int tfa98xx_send_data_to_dsp(int8_t *buffer, int16_t DataLength)
{
	int result = 0;

	if (buffer == NULL)
		return -EFAULT;
#if defined(CONFIG_SND_SOC_MTK_AUDIO_DSP)
	result = mtk_spk_send_ipi_buf_to_dsp(buffer, DataLength);
#endif
	/*msleep(50);*/
	return result;
}

int tfa98xx_receive_data_from_dsp(int8_t *buffer,
	int16_t size,
	uint32_t *DataLength)
{
	int result = 0;
#if defined(CONFIG_SND_SOC_MTK_AUDIO_DSP)
	result = mtk_spk_recv_ipi_buf_from_dsp(buffer, size, DataLength);
#endif
	return result;
}


static const struct tfa98xx_rate rate_to_fssel[] = {
	{ 8000, 0 },
	{ 11025, 1 },
	{ 12000, 2 },
	{ 16000, 3 },
	{ 22050, 4 },
	{ 24000, 5 },
	{ 32000, 6 },
	{ 44100, 7 },
	{ 48000, 8 },
};

#ifdef OPLUS_BUG_COMPATIBILITY

unsigned short tfa98xx_vol_value = 0;
static void tfa98xx_set_volume_work(struct work_struct *work)
{
	struct tfa98xx *tfa98xx = container_of(work, struct tfa98xx, volume_work.work);
	int err = 0;
	int waitcnt = 30;

	pr_info("%s: volume: %d\n", __func__, tfa98xx_vol_value);

	if (tfa98xx_dual_spk) {
		list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
			if (tfa98xx->flags & TFA98XX_FLAG_CHIP_SELECTED) {
				pr_info("%s: set volume, addr = 0x%x\n", __func__, tfa98xx->i2c->addr);

				while ((tfa98xx->dsp_init != TFA98XX_DSP_INIT_DONE) && (--waitcnt)) {
					msleep(10);
				}

				pr_info("%s: wait end\n", __func__);

				if (tfa98xx->dsp_init == TFA98XX_DSP_INIT_DONE) {
					mutex_lock(&tfa98xx->dsp_lock);
					err = tfa98xx_set_volume_level(tfa98xx->tfa, tfa98xx_vol_value);
					if (err) {
						pr_info("%s: set volume error, code:%d\n", __func__, err);
					} else {
						pr_info("%s: set volume ok\n", __func__);
					}
					mutex_unlock(&tfa98xx->dsp_lock);
				} else {
					pr_info("%s: tfa98xx dsp status error\n", __func__);
				}
			}
		}
	} else {
		while ((tfa98xx->dsp_init != TFA98XX_DSP_INIT_DONE) && (--waitcnt)) {
		    msleep(10);
		}

		pr_info("%s: wait end\n", __func__);

		if (tfa98xx->dsp_init == TFA98XX_DSP_INIT_DONE) {
		    mutex_lock(&tfa98xx->dsp_lock);
		    err = tfa98xx_set_volume_level(tfa98xx->tfa, tfa98xx_vol_value);
		    if (err) {
		        pr_info("%s: set volume error, code:%d\n", __func__, err);
		    } else {
		        pr_info("%s: set volume ok\n", __func__);
		    }
		    mutex_unlock(&tfa98xx->dsp_lock);
		} else {
		    pr_info("%s: tfa98xx dsp status error\n", __func__);
		}
	}

}

static int tfa98xx_volume_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int tfa98xx_volume_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
#else
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
#endif

	tfa98xx_vol_value = ucontrol->value.integer.value[0];

	queue_delayed_work(tfa98xx->tfa98xx_wq,
			                   &tfa98xx->volume_work, 0);

	return 0;
}

unsigned int g_tfa98xx_ana_vol = 16;
static int tfa98xx_ana_volume_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
#else
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
#endif
	unsigned int tfa98xx_ana_vol;

	mutex_lock(&tfa98xx->dsp_lock);
	tfa98xx_get_ana_volume(tfa98xx->tfa, &tfa98xx_ana_vol);
	pr_info("%s: get ana volume ok, ana volume: %d\n", __func__, tfa98xx_ana_vol);
	mutex_unlock(&tfa98xx->dsp_lock);

	ucontrol->value.integer.value[0] = tfa98xx_ana_vol;

	return 0;
}

static int tfa98xx_ana_volume_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
#else
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
#endif

	int err = 0;

	g_tfa98xx_ana_vol = ucontrol->value.integer.value[0];

	pr_info("%s: ana volume: %d\n", __func__, g_tfa98xx_ana_vol);

	mutex_lock(&tfa98xx->dsp_lock);
	err = tfa98xx_set_ana_volume(tfa98xx->tfa, g_tfa98xx_ana_vol);
	if (err) {
		pr_err("%s: set ana volume error, code:%d\n", __func__, err);
	} else {
		pr_info("%s: set ana volume ok\n", __func__);
	}
	mutex_unlock(&tfa98xx->dsp_lock);

	return 0;
}

static const struct snd_kcontrol_new tfa98xx_snd_controls[] = {
	SOC_SINGLE_EXT("TFA98XX Volume", SND_SOC_NOPM, 0, 0xff, 0,
		       tfa98xx_volume_get, tfa98xx_volume_set),
	SOC_SINGLE_EXT("TFA98XX ANA Volume", SND_SOC_NOPM, 0, 0xff, 0,
			   tfa98xx_ana_volume_get, tfa98xx_ana_volume_set),
};

#endif /* OPLUS_BUG_COMPATIBILITY */


static inline char *tfa_cont_profile_name(struct tfa98xx *tfa98xx, int prof_idx)
{
	if (tfa98xx->tfa->cnt == NULL)
		return NULL;
	return tfaContProfileName(tfa98xx->tfa->cnt, tfa98xx->tfa->dev_idx, prof_idx);
}


static enum Tfa98xx_Error tfa98xx_write_re25(struct tfa_device *tfa, int value)
{
	enum Tfa98xx_Error err;

	/* clear MTPEX */
	err = tfa_dev_mtp_set(tfa, TFA_MTP_EX, 0);
	if (err == Tfa98xx_Error_Ok) {
		/* set RE25 in shadow regiser */
		err = tfa_dev_mtp_set(tfa, TFA_MTP_RE25_PRIM, value);
	}
	if (err == Tfa98xx_Error_Ok) {
		/* set MTPEX to copy RE25 into MTP  */
		err = tfa_dev_mtp_set(tfa, TFA_MTP_EX, 2);
	}

	return err;
}

#ifdef OPLUS_BUG_COMPATIBILITY
struct proc_dir_entry *prEntry_temp_v6 = NULL;
#define TFA98XX_DEBUG_FS_NAME_V6 "ftm_tfa98xx_v6"
static int kernel_debug_open(struct inode *inode, struct file *file)
{
	pr_err("%s \n", __FUNCTION__);
	return 0;
}

static ssize_t kernel_debug_read(struct file *file, char __user *buf,
                                 size_t count, loff_t *pos)
{
/* /sys/kernel/debug/ftm_tfa98xx */
	const int size = 1024;
	char buffer[size];
	int n = 0;
#if 0
	n += scnprintf(buffer + n, size - n, "%s ", ftm_load_file);
	n += scnprintf(buffer + n, size - n, "%s ", ftm_clk);
	n += scnprintf(buffer + n, size - n, "%s ", ftm_SpeakerCalibration);
	n += scnprintf(buffer + n, size - n, "%s ", ftm_path);
	n += scnprintf(buffer + n, size - n, "%s ", ftm_spk_resistance);
	n += scnprintf(buffer + n, size - n, "%s ", ftm_tfa98xx_flag);
	n += scnprintf(buffer + n, size - n, "%d ", ftm_mode);
#endif
	return simple_read_from_buffer(buf, count, pos, buffer, n);
}

static ssize_t kernel_debug_write(struct file *f, const char __user *buf,
                                  size_t count, loff_t *offset)
{
	pr_err("%s \n", __FUNCTION__);
	return 0;
}

static const struct file_operations tfa98xx_debug_ops =
{
	.open = kernel_debug_open,
	.read = kernel_debug_read,
	.write = kernel_debug_write,
};
#endif /* OPLUS_BUG_COMPATIBILITY */

static enum Tfa98xx_Error tfa9874_calibrate(struct tfa98xx *tfa98xx, int *speakerImpedance)
{

	enum Tfa98xx_Error err;
	int imp, profile, cal_profile, i;
	char buffer[6] = {0};
	unsigned char bytes[6] = {0};
	#ifdef OPLUS_ARCH_EXTENDS
	if(!g_tfa98xx_firmware_status){
		pr_info("firmware not ready \n");
		return Tfa98xx_Error_Fail;
	}
	#endif

	cal_profile = tfaContGetCalProfile(tfa98xx->tfa);
	if (cal_profile >= 0)
		profile = cal_profile;
	else
		profile = 0;

	if (tfa_dev_mtp_get(tfa98xx->tfa, TFA_MTP_EX)){
		pr_info("DSP already calibrated, MTPEX==1\n");
		err = tfa_dsp_get_calibration_impedance(tfa98xx->tfa);
		imp = tfa_get_calibration_info(tfa98xx->tfa, 0);
		pr_info("Calibration value: %d.%3d ohm\n", imp%1000,imp-1000*(imp%1000));

	}

	pr_info("Calibration started profile %d\n", profile);

	err = (enum Tfa98xx_Error)tfa_dev_mtp_set(tfa98xx->tfa, TFA_MTP_OTC, 0);
	if (err) {
		pr_err("MTPOTC write failed\n");
		return Tfa98xx_Error_Fail;
	}

	err = (enum Tfa98xx_Error)tfa_dev_mtp_set(tfa98xx->tfa, TFA_MTP_EX, 0);
	if (err) {
		pr_err("MTPEX write failed\n");
		return Tfa98xx_Error_Fail;
	}

	/* write all the registers from the profile list */
	err = tfaContWriteRegsProf(tfa98xx->tfa, profile);
	if (err){
		pr_err("MTP write failed\n");
		return Tfa98xx_Error_Fail;
	}

	/* set the DSP commands SetAlgoParams and SetMBDrc reset flag */
	tfa98xx->tfa->needs_reset = 1;

	/* write all the files from the profile list (volumestep 0) */
	err = tfaContWriteProfile(tfa98xx->tfa, profile, 0);
	if (err) {
		pr_err("profile write failed\n");
		return Tfa98xx_Error_Bad_Parameter;
	}

	/* clear the DSP commands SetAlgoParams and SetMBDrc reset flag */
	tfa98xx->tfa->needs_reset = 0;

	/* First read the GetStatusChange to clear the status (sticky) */
	err = tfa_dsp_cmd_id_write_read(tfa98xx->tfa, MODULE_FRAMEWORK, FW_PAR_ID_GET_STATUS_CHANGE, 6, (unsigned char *)buffer);

	/* We need to send zero's to trigger calibration */
	memset(bytes, 0, 6);
	err = tfa_dsp_cmd_id_write(tfa98xx->tfa, MODULE_SPEAKERBOOST, SB_PARAM_SET_RE25C, sizeof(bytes), bytes);

	/* Wait a maximum of 5 seconds to get the calibration results */
	for (i=0; i < 50; i++ ) {
		/* Get the GetStatusChange results */
		err = tfa_dsp_cmd_id_write_read(tfa98xx->tfa, MODULE_FRAMEWORK, FW_PAR_ID_GET_STATUS_CHANGE, 6, (unsigned char *)buffer);

		/* Avoid busload */
		msleep_interruptible(100);

		/* If the calibration trigger is set break the loop */
		if(buffer[5] & TFADSP_FLAG_CALIBRATE_DONE) {
			break;
		}
	}

	err = tfa_dsp_get_calibration_impedance(tfa98xx->tfa);

	imp = (tfa_get_calibration_info(tfa98xx->tfa, 0));

	*speakerImpedance = imp;
	pr_info("%s: Calibration value: %d.%3d ohm\n", __func__, imp/1000, imp%1000);

	/* Write calibration value to MTP */
	err = (enum Tfa98xx_Error)tfa_dev_mtp_set(tfa98xx->tfa, TFA_MTP_RE25, (uint16_t)((int)(imp)));
	if (err) {
		pr_err("MTP Re25 write failed\n");
		return Tfa98xx_Error_Fail;
	}

	/* Set MTPEX to indicate calibration is done! */
	err = (enum Tfa98xx_Error)tfa_dev_mtp_set(tfa98xx->tfa, TFA_MTP_OTC, 1);
	if (err) {
		pr_err("OTC write failed\n");
		return Tfa98xx_Error_Fail;
	}

	/* Set MTPEX to indicate calibration is done! */
	err = (enum Tfa98xx_Error)tfa_dev_mtp_set(tfa98xx->tfa, TFA_MTP_EX, 2);
	if (err) {
		pr_err("MTPEX write failed\n");
		return Tfa98xx_Error_Fail;
	}

	/* Save the current profile */
	tfa_dev_set_swprof(tfa98xx->tfa, (unsigned short)profile);

	return Tfa98xx_Error_Ok;

}

/* Wrapper for tfa start */
static enum Tfa98xx_Error tfa98xx_tfa_start(struct tfa98xx *tfa98xx, int next_profile, int vstep)
{
	enum Tfa98xx_Error err;
	ktime_t start_time, stop_time;
	u64 delta_time;

	if (trace_level & 8) {
		start_time = ktime_get_boottime();
	}

	dev_info(&tfa98xx->i2c->dev, "tfa98xx_tfa_start enter\n");

	err = tfa_dev_start(tfa98xx->tfa, next_profile, vstep);

	if (trace_level & 8) {
		stop_time = ktime_get_boottime();
		delta_time = ktime_to_ns(ktime_sub(stop_time, start_time));
		do_div(delta_time, 1000);
		dev_dbg(&tfa98xx->i2c->dev, "tfa_dev_start(%d,%d) time = %lld us\n",
		        next_profile, vstep, delta_time);
	}

	if ((err == Tfa98xx_Error_Ok) && (tfa98xx->set_mtp_cal)) {
		enum Tfa98xx_Error err_cal;
		err_cal = tfa98xx_write_re25(tfa98xx->tfa, tfa98xx->cal_data);
		if (err_cal != Tfa98xx_Error_Ok) {
			pr_err("Error, setting calibration value in mtp, err=%d\n", err_cal);
		} else {
			tfa98xx->set_mtp_cal = false;
			pr_info("Calibration value (%d) set in mtp\n",
			        tfa98xx->cal_data);
		}
	}

	/* Check and update tap-detection state (in case of profile change) */
	tfa98xx_tapdet_check_update(tfa98xx);

	/* Remove sticky bit by reading it once */
	tfa_get_noclk(tfa98xx->tfa);

	/* A cold start erases the configuration, including interrupts setting.
	 * Restore it if required
	 */
	tfa98xx_interrupt_enable(tfa98xx, true);

	return err;
}

static int tfa98xx_input_open(struct input_dev *dev)
{
	struct tfa98xx *tfa98xx = input_get_drvdata(dev);
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	dev_info(tfa98xx->codec->dev, "opening device file\n");
    #else
	dev_info(tfa98xx->component->dev, "opening device file\n");
    #endif

	/* note: open function is called only once by the framework.
	 * No need to count number of open file instances.
	 */
	if (tfa98xx->dsp_fw_state != TFA98XX_DSP_FW_OK) {
		dev_info(&tfa98xx->i2c->dev,
			"DSP not loaded, cannot start tap-detection\n");
		return -EIO;
	}

	/* enable tap-detection service */
	tfa98xx->tapdet_open = true;
	tfa98xx_tapdet_check_update(tfa98xx);

        return 0;
}

static void tfa98xx_input_close(struct input_dev *dev)
{
	struct tfa98xx *tfa98xx = input_get_drvdata(dev);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	dev_info(tfa98xx->codec->dev, "closing device file\n");
 #else
	 dev_info(tfa98xx->component->dev, "closing device file\n");
#endif

	/* Note: close function is called if the device is unregistered */

	/* disable tap-detection service */
	tfa98xx->tapdet_open = false;
	tfa98xx_tapdet_check_update(tfa98xx);
}

static int tfa98xx_register_inputdev(struct tfa98xx *tfa98xx)
{
	int err;
	struct input_dev *input;
	input = input_allocate_device();

	if (!input) {

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		dev_err(tfa98xx->codec->dev, "Unable to allocate input device\n");
#else
		dev_err(tfa98xx->component->dev, "Unable to allocate input device\n");
#endif
		return -ENOMEM;
	}

	input->evbit[0] = BIT_MASK(EV_KEY);
	input->keybit[BIT_WORD(BTN_0)] |= BIT_MASK(BTN_0);
	input->keybit[BIT_WORD(BTN_1)] |= BIT_MASK(BTN_1);
	input->keybit[BIT_WORD(BTN_2)] |= BIT_MASK(BTN_2);
	input->keybit[BIT_WORD(BTN_3)] |= BIT_MASK(BTN_3);
	input->keybit[BIT_WORD(BTN_4)] |= BIT_MASK(BTN_4);
	input->keybit[BIT_WORD(BTN_5)] |= BIT_MASK(BTN_5);
	input->keybit[BIT_WORD(BTN_6)] |= BIT_MASK(BTN_6);
	input->keybit[BIT_WORD(BTN_7)] |= BIT_MASK(BTN_7);
	input->keybit[BIT_WORD(BTN_8)] |= BIT_MASK(BTN_8);
	input->keybit[BIT_WORD(BTN_9)] |= BIT_MASK(BTN_9);

	input->open = tfa98xx_input_open;
	input->close = tfa98xx_input_close;

	input->name = "tfa98xx-tapdetect";

	input->id.bustype = BUS_I2C;
	input_set_drvdata(input, tfa98xx);

	err = input_register_device(input);
	if (err) {
	 #if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		dev_err(tfa98xx->codec->dev, "Unable to register input device\n");
	 #else
		dev_err(tfa98xx->component->dev, "Unable to register input device\n");
	 #endif
		goto err_free_dev;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	dev_dbg(tfa98xx->codec->dev, "Input device for tap-detection registered: %s\n",
		input->name);
#else
    dev_dbg(tfa98xx->component->dev, "Input device for tap-detection registered: %s\n",
	        input->name);
#endif
	tfa98xx->input = input;
	return 0;

err_free_dev:
	input_free_device(input);
	return err;
}

/*
 * Check if an input device for tap-detection can and shall be registered.
 * Register it if appropriate.
 * If already registered, check if still relevant and remove it if necessary.
 * unregister: true to request inputdev unregistration.
 */
static void __tfa98xx_inputdev_check_register(struct tfa98xx *tfa98xx, bool unregister)
{
	bool tap_profile = false;
	unsigned int i;
	for (i = 0; i < tfa_cnt_get_dev_nprof(tfa98xx->tfa); i++) {
		if (strstr(tfa_cont_profile_name(tfa98xx, i), ".tap")) {
			tap_profile = true;
			tfa98xx->tapdet_profiles |= 1 << i;
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
			dev_info(tfa98xx->codec->dev,
				"found a tap-detection profile (%d - %s)\n",
				i, tfa_cont_profile_name(tfa98xx, i));
	#else
			dev_info(tfa98xx->component->dev,
				"found a tap-detection profile (%d - %s)\n",
				i, tfa_cont_profile_name(tfa98xx, i));
	#endif
		}
	}

	/* Check for device support:
	 *  - at device level
	 *  - at container (profile) level
	 */
	if (!(tfa98xx->flags & TFA98XX_FLAG_TAPDET_AVAILABLE) ||
		!tap_profile ||
		unregister) {
		/* No input device supported or required */
		if (tfa98xx->input) {
			input_unregister_device(tfa98xx->input);
			tfa98xx->input = NULL;
		}
		return;
	}

	/* input device required */
	if (tfa98xx->input)
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		dev_info(tfa98xx->codec->dev, "Input device already registered, skipping\n");
	#else
	    dev_info(tfa98xx->component->dev, "Input device already registered, skipping\n");
	#endif
	else
		tfa98xx_register_inputdev(tfa98xx);
}

static void tfa98xx_inputdev_check_register(struct tfa98xx *tfa98xx)
{
	__tfa98xx_inputdev_check_register(tfa98xx, false);
}

static void tfa98xx_inputdev_unregister(struct tfa98xx *tfa98xx)
{
	__tfa98xx_inputdev_check_register(tfa98xx, true);
}

#ifdef CONFIG_DEBUG_FS

/* OTC reporting
 * Returns the MTP0 OTC bit value
 */
static int tfa98xx_dbgfs_otc_get(void *data, u64 *val)
{
	struct i2c_client *i2c = (struct i2c_client *)data;
	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	int value;

	mutex_lock(&tfa98xx->dsp_lock);
	value = tfa_dev_mtp_get(tfa98xx->tfa, TFA_MTP_OTC);
	mutex_unlock(&tfa98xx->dsp_lock);

	if (value < 0) {
		pr_err("[0x%x] Unable to check DSP access: %d\n", tfa98xx->i2c->addr, value);
		return -EIO;
	}

	*val = value;
	pr_debug("[0x%x] OTC : %d\n", tfa98xx->i2c->addr, value);

	return 0;
}

static int tfa98xx_dbgfs_otc_set(void *data, u64 val)
{
	struct i2c_client *i2c = (struct i2c_client *)data;
	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	enum Tfa98xx_Error err;

	if (val != 0 && val != 1) {
		pr_err("[0x%x] Unexpected value %llu\n", tfa98xx->i2c->addr, val);
		return -EINVAL;
	}

	mutex_lock(&tfa98xx->dsp_lock);
	err = tfa_dev_mtp_set(tfa98xx->tfa, TFA_MTP_OTC, val);
	mutex_unlock(&tfa98xx->dsp_lock);

	if (err != Tfa98xx_Error_Ok) {
		pr_err("[0x%x] Unable to check DSP access: %d\n", tfa98xx->i2c->addr, err);
		return -EIO;
	}

	pr_debug("[0x%x] OTC < %llu\n", tfa98xx->i2c->addr, val);

	return 0;
}

static int tfa98xx_dbgfs_mtpex_get(void *data, u64 *val)
{
	struct i2c_client *i2c = (struct i2c_client *)data;
	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	int value;

	mutex_lock(&tfa98xx->dsp_lock);
	value = tfa_dev_mtp_get(tfa98xx->tfa, TFA_MTP_EX);
	mutex_unlock(&tfa98xx->dsp_lock);

	if (value < 0) {
		pr_err("[0x%x] Unable to check DSP access: %d\n", tfa98xx->i2c->addr, value);
		return -EIO;
	}


	*val = value;
	pr_debug("[0x%x] MTPEX : %d\n", tfa98xx->i2c->addr, value);

	return 0;
}

static int tfa98xx_dbgfs_mtpex_set(void *data, u64 val)
{
	struct i2c_client *i2c = (struct i2c_client *)data;
	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	enum Tfa98xx_Error err;

	if (val != 0) {
		pr_err("[0x%x] Can only clear MTPEX (0 value expected)\n", tfa98xx->i2c->addr);
		return -EINVAL;
	}

	mutex_lock(&tfa98xx->dsp_lock);
	err = tfa_dev_mtp_set(tfa98xx->tfa, TFA_MTP_EX, val);
	mutex_unlock(&tfa98xx->dsp_lock);

	if (err != Tfa98xx_Error_Ok) {
		pr_err("[0x%x] Unable to check DSP access: %d\n", tfa98xx->i2c->addr, err);
		return -EIO;
	}

	pr_debug("[0x%x] MTPEX < 0\n", tfa98xx->i2c->addr);

	return 0;
}

static int tfa98xx_dbgfs_temp_get(void *data, u64 *val)
{
	struct i2c_client *i2c = (struct i2c_client *)data;
	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);

	mutex_lock(&tfa98xx->dsp_lock);
	*val = tfa98xx_get_exttemp(tfa98xx->tfa);
	mutex_unlock(&tfa98xx->dsp_lock);

	pr_debug("[0x%x] TEMP : %llu\n", tfa98xx->i2c->addr, *val);

	return 0;
}

static int tfa98xx_dbgfs_temp_set(void *data, u64 val)
{
	struct i2c_client *i2c = (struct i2c_client *)data;
	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);

	mutex_lock(&tfa98xx->dsp_lock);
	tfa98xx_set_exttemp(tfa98xx->tfa, (short)val);
	mutex_unlock(&tfa98xx->dsp_lock);

	pr_debug("[0x%x] TEMP < %llu\n", tfa98xx->i2c->addr, val);

	return 0;
}
#endif //CONFIG_DEBUG_FS

static ssize_t tfa98xx_dbgfs_start_set(struct file *file,
			const char __user *user_buf,
			size_t count, loff_t *ppos)
{
#ifdef CONFIG_DEBUG_FS
        struct i2c_client *i2c = file->private_data;
#else /*CONFIG_DEBUG_FS*/
        struct i2c_client *i2c = PDE_DATA(file_inode(file));
#endif/*CONFIG_DEBUG_FS*/

	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	enum Tfa98xx_Error ret;
	char buf[32];
	const char ref[] = "please calibrate now";
	int buf_size;

	/* check string length, and account for eol */
	if (count > sizeof(ref) + 1 || count < (sizeof(ref) - 1))
		return -EINVAL;

	buf_size = min(count, (size_t)(sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	/* Compare string, excluding the trailing \0 and the potentials eol */
	if (strncmp(buf, ref, sizeof(ref) - 1))
		return -EINVAL;

	mutex_lock(&tfa98xx->dsp_lock);
	ret = tfa_calibrate(tfa98xx->tfa);
	if (ret == Tfa98xx_Error_Ok)
		ret = tfa98xx_tfa_start(tfa98xx, tfa98xx->profile, tfa98xx->vstep);
	if (ret == Tfa98xx_Error_Ok)
			tfa_dev_set_state(tfa98xx->tfa, TFA_STATE_UNMUTE);
	mutex_unlock(&tfa98xx->dsp_lock);

	if (ret) {
		pr_info("[0x%x] Calibration start failed (%d)\n", tfa98xx->i2c->addr, ret);
		return -EIO;
	} else {
		pr_info("[0x%x] Calibration started\n", tfa98xx->i2c->addr);
	}

	return count;
}

#ifdef OPLUS_BUG_COMPATIBILITY
static int tfa98xx_speaker_recalibration(struct tfa_device *tfa, int *speakerImpedance)
{
	int err, error = Tfa98xx_Error_Ok;
	int cal_profile = -1;
	int profile = -1;

	/* Do not open/close tfa98xx: not required by tfa_clibrate */
	error = tfa_calibrate(tfa);
	msleep_interruptible(25);

	cal_profile = tfaContGetCalProfile(tfa);
	if (cal_profile >= 0) {
		profile = cal_profile;
	} else {
		profile = 0;
	}
	pr_err("%s profile=%d\n", __func__, profile);

	error = tfaRunSpeakerBoost(tfa, 1, profile); /* No force coldstart (with profile 0) */
	if(error) {
		pr_err("%s Calibration failed (error = %d)\n", __func__, error);
		*speakerImpedance = 0;
	} else {
		pr_err("%s Calibration sucessful! \n", __func__);
		*speakerImpedance = tfa->mohm[0];
		if (TFA_GET_BF(tfa, PWDN) != 0) {
			   err = tfa98xx_powerdown(tfa, 0);  //leave power off state
		   }
		tfaRunUnmute(tfa);	/* unmute */
	}
	return error;
}
#endif /* OPLUS_BUG_COMPATIBILITY */

static ssize_t tfa98xx_dbgfs_r_read(struct file *file,
				     char __user *user_buf, size_t count,
				     loff_t *ppos)
{
#ifdef CONFIG_DEBUG_FS
        struct i2c_client *i2c = file->private_data;
#else /*CONFIG_DEBUG_FS*/
        struct i2c_client *i2c = PDE_DATA(file_inode(file));
#endif/*CONFIG_DEBUG_FS*/

	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	char *str;
	uint16_t status;
	int ret;
	#ifdef OPLUS_BUG_COMPATIBILITY
	int calibrate_done = 0;
	int speakerImpedance = 0;
	#endif /* OPLUS_BUG_COMPATIBILITY */

	mutex_lock(&tfa98xx->dsp_lock);

	/* Need to ensure DSP is access-able, use mtp read access for this
	 * purpose
	 */
	ret = tfa98xx_get_mtp(tfa98xx->tfa, &status);
	if (ret) {
		ret = -EIO;
		pr_err("[0x%x] MTP read failed\n", tfa98xx->i2c->addr);
		goto r_c_err;
	}

	if (tfa98xx->tfa->is_probus_device) {
		tfa9874_calibrate( tfa98xx, &speakerImpedance);
	} else {
		#ifdef OPLUS_BUG_COMPATIBILITY
		ret = tfaRunSpeakerCalibration_result(tfa98xx->tfa, &calibrate_done);
		#else /* OPLUS_BUG_COMPATIBILITY */
		ret = tfaRunSpeakerCalibration(tfa98xx->tfa);
		#endif /* OPLUS_BUG_COMPATIBILITY */
		if (ret) {
			ret = -EIO;
			pr_err("[0x%x] calibration failed\n", tfa98xx->i2c->addr);
			#ifndef OPLUS_BUG_COMPATIBILITY
			goto r_c_err;
			#endif /* OPLUS_BUG_COMPATIBILITY */
		}

		#ifdef OPLUS_BUG_COMPATIBILITY
		if (1 == calibrate_done) {
			tfa98xx_speaker_recalibration(tfa98xx->tfa, &speakerImpedance);
		}
		#endif /* OPLUS_BUG_COMPATIBILITY */
	}

	str = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!str) {
		ret = -ENOMEM;
		pr_err("[0x%x] memory allocation failed\n", tfa98xx->i2c->addr);
		goto r_c_err;
	}

	#ifdef OPLUS_BUG_COMPATIBILITY
	ret = snprintf(str, PAGE_SIZE, " Prim:%d mOhms, Sec:%d mOhms\n",
					speakerImpedance,
					tfa98xx->tfa->mohm[1]);
	#else /* OPLUS_BUG_COMPATIBILITY */
	if (tfa98xx->tfa->spkr_count > 1) {
		ret = snprintf(str, PAGE_SIZE,
		              "Prim:%d mOhms, Sec:%d mOhms\n",
		              tfa98xx->tfa->mohm[0],
		              tfa98xx->tfa->mohm[1]);
	} else {
		ret = snprintf(str, PAGE_SIZE,
		              "Prim:%d mOhms\n",
		              tfa98xx->tfa->mohm[0]);
	}
	#endif /* OPLUS_BUG_COMPATIBILITY */

	pr_info("[0x%x] calib_done: %s", tfa98xx->i2c->addr, str);

	if (ret < 0)
		goto r_err;

	ret = simple_read_from_buffer(user_buf, count, ppos, str, ret);

r_err:
	kfree(str);
r_c_err:
	mutex_unlock(&tfa98xx->dsp_lock);
	return ret;
}

#ifdef OPLUS_BUG_COMPATIBILITY
static ssize_t tfa98xx_dbgfs_range_read(struct file *file,
				     char __user *user_buf, size_t count,
				     loff_t *ppos)
{
#ifdef CONFIG_DEBUG_FS
        struct i2c_client *i2c = file->private_data;
#else /*CONFIG_DEBUG_FS*/
        struct i2c_client *i2c = PDE_DATA(file_inode(file));
#endif/*CONFIG_DEBUG_FS*/

	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	char *str;
	int ret;

	if (!tfa98xx) {
		pr_err("%s tfa98xx is null\n", __func__);
		return -EINVAL;
	}

	if (!tfa98xx->tfa) {
		pr_err("%s tfa is null\n", __func__);
		return -EINVAL;
	}

	str = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!str) {
		ret = -ENOMEM;
		pr_err("[0x%x] memory allocation failed\n", tfa98xx->i2c->addr);
		goto range_err;
	}

	ret = snprintf(str, PAGE_SIZE, " Min:%d mOhms, Max:%d mOhms\n",
					tfa98xx->tfa->min_mohms, tfa98xx->tfa->max_mohms);

	ret = simple_read_from_buffer(user_buf, count, ppos, str, ret);

	kfree(str);

range_err:
	return ret;
}
#endif /* OPLUS_BUG_COMPATIBILITY */

static ssize_t tfa98xx_dbgfs_version_read(struct file *file,
				     char __user *user_buf, size_t count,
				     loff_t *ppos)
{
	char str[] = TFA98XX_VERSION "\n";
	int ret;

	ret = simple_read_from_buffer(user_buf, count, ppos, str, sizeof(str));

	return ret;
}

static ssize_t tfa98xx_dbgfs_dsp_state_get(struct file *file,
				     char __user *user_buf, size_t count,
				     loff_t *ppos)
{
#ifdef CONFIG_DEBUG_FS
        struct i2c_client *i2c = file->private_data;
#else /*CONFIG_DEBUG_FS*/
        struct i2c_client *i2c = PDE_DATA(file_inode(file));
#endif/*CONFIG_DEBUG_FS*/

	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	int ret = 0;
	char *str;

	switch (tfa98xx->dsp_init) {
	case TFA98XX_DSP_INIT_STOPPED:
		str = "Stopped\n";
		break;
	case TFA98XX_DSP_INIT_RECOVER:
		str = "Recover requested\n";
		break;
	case TFA98XX_DSP_INIT_FAIL:
		str = "Failed init\n";
		break;
	case TFA98XX_DSP_INIT_PENDING:
		str =  "Pending init\n";
		break;
	case TFA98XX_DSP_INIT_DONE:
		str = "Init complete\n";
		break;
	default:
		str = "Invalid\n";
	}

	pr_info("[0x%x] dsp_state : %s\n", tfa98xx->i2c->addr, str);

	ret = simple_read_from_buffer(user_buf, count, ppos, str, strlen(str));
	return ret;
}

static ssize_t tfa98xx_dbgfs_dsp_state_set(struct file *file,
				     const char __user *user_buf,
				     size_t count, loff_t *ppos)
{
#ifdef CONFIG_DEBUG_FS
        struct i2c_client *i2c = file->private_data;
#else /*CONFIG_DEBUG_FS*/
        struct i2c_client *i2c = PDE_DATA(file_inode(file));
#endif/*CONFIG_DEBUG_FS*/

	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	enum Tfa98xx_Error ret;
	char buf[32];
	const char start_cmd[] = "start";
	const char stop_cmd[] = "stop";
	const char mon_start_cmd[] = "monitor start";
	const char mon_stop_cmd[] = "monitor stop";
	int buf_size;

	buf_size = min(count, (size_t)(sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	/* Compare strings, excluding the trailing \0 */
	if (!strncmp(buf, start_cmd, sizeof(start_cmd) - 1)) {
		pr_info("[0x%x] Manual triggering of dsp start...\n", tfa98xx->i2c->addr);
		mutex_lock(&tfa98xx->dsp_lock);
		ret = tfa98xx_tfa_start(tfa98xx, tfa98xx->profile, tfa98xx->vstep);
		mutex_unlock(&tfa98xx->dsp_lock);
		pr_debug("[0x%x] tfa_dev_start complete: %d\n",  tfa98xx->i2c->addr, ret);
	} else if (!strncmp(buf, stop_cmd, sizeof(stop_cmd) - 1)) {
		pr_info("[0x%x] Manual triggering of dsp stop...\n",  tfa98xx->i2c->addr);
		mutex_lock(&tfa98xx->dsp_lock);
		ret = tfa_dev_stop(tfa98xx->tfa);
		mutex_unlock(&tfa98xx->dsp_lock);
		pr_debug("[0x%x] tfa_dev_stop complete: %d\n",  tfa98xx->i2c->addr, ret);
	} else if (!strncmp(buf, mon_start_cmd, sizeof(mon_start_cmd) - 1)) {
		pr_info("[0x%x] Manual start of monitor thread...\n",  tfa98xx->i2c->addr);
		queue_delayed_work(tfa98xx->tfa98xx_wq,
					&tfa98xx->monitor_work, HZ);
	} else if (!strncmp(buf, mon_stop_cmd, sizeof(mon_stop_cmd) - 1)) {
		pr_info("[0x%x] Manual stop of monitor thread...\n",  tfa98xx->i2c->addr);
		cancel_delayed_work_sync(&tfa98xx->monitor_work);
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t tfa98xx_dbgfs_fw_state_get(struct file *file,
				     char __user *user_buf, size_t count,
				     loff_t *ppos)
{
#ifdef CONFIG_DEBUG_FS
        struct i2c_client *i2c = file->private_data;
#else /*CONFIG_DEBUG_FS*/
        struct i2c_client *i2c = PDE_DATA(file_inode(file));
#endif/*CONFIG_DEBUG_FS*/

	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	char *str;

	switch (tfa98xx->dsp_fw_state) {
	case TFA98XX_DSP_FW_NONE:
		str = "None\n";
		break;
	case TFA98XX_DSP_FW_PENDING:
		str = "Pending\n";
		break;
	case TFA98XX_DSP_FW_FAIL:
		str = "Fail\n";
		break;
	case TFA98XX_DSP_FW_OK:
		str =  "Ok\n";
		break;
	default:
		str = "Invalid\n";
	}

	pr_info("[0x%x] fw_state : %s", tfa98xx->i2c->addr, str);

	return simple_read_from_buffer(user_buf, count, ppos, str, strlen(str));
}

extern int send_tfa_cal_apr(void *buf, int cmd_size, bool bRead);
static ssize_t tfa98xx_dbgfs_rpc_read(struct file *file,
				     char __user *user_buf, size_t count,
				     loff_t *ppos)
{
#ifdef CONFIG_DEBUG_FS
        struct i2c_client *i2c = file->private_data;
#else /*CONFIG_DEBUG_FS*/
        struct i2c_client *i2c = PDE_DATA(file_inode(file));
#endif/*CONFIG_DEBUG_FS*/

	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	int ret = 0;
	uint32_t DataLength = 0;
	uint8_t *buffer;
	enum Tfa98xx_Error error;

	if (tfa98xx->tfa == NULL)
		return -ENODEV;


	if (count == 0)
		return 0;

	buffer = kmalloc(count, GFP_KERNEL);
	if (buffer == NULL) {
		pr_err("[0x%x] can not allocate memory\n", tfa98xx->i2c->addr);
		return -ENOMEM;
	}
	#ifdef OPLUS_ARCH_EXTENDS
	memset((void *)buffer, 0, count);
	#endif

	mutex_lock(&tfa98xx->dsp_lock);
	if (tfa98xx->tfa->is_probus_device)
		error = tfa98xx_receive_data_from_dsp(buffer, count, &DataLength);
	else
		error = dsp_msg_read(tfa98xx->tfa, count, buffer);

	mutex_unlock(&tfa98xx->dsp_lock);
	if (error) {
		pr_info("[0x%x] dsp_msg_read error: %d\n", tfa98xx->i2c->addr, error);
		kfree(buffer);
		return -EFAULT;
	}

	ret = copy_to_user(user_buf, buffer, count);
	kfree(buffer);
	if (ret)
		return -EFAULT;

	*ppos += count;
	return count;
}

static ssize_t tfa98xx_dbgfs_rpc_send(struct file *file,
				     const char __user *user_buf,
				     size_t count, loff_t *ppos)
{
#ifdef CONFIG_DEBUG_FS
        struct i2c_client *i2c = file->private_data;
#else /*CONFIG_DEBUG_FS*/
        struct i2c_client *i2c = PDE_DATA(file_inode(file));
#endif/*CONFIG_DEBUG_FS*/

	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	nxpTfaFileDsc_t *msg_file;
	enum Tfa98xx_Error error;
	int err = 0;

	if (tfa98xx->tfa == NULL) {
		pr_debug("[0x%x] dsp is not available\n", tfa98xx->i2c->addr);
		return -ENODEV;
	}


		/* msg_file.name is not used */
		msg_file = kmalloc(count + sizeof(nxpTfaFileDsc_t), GFP_KERNEL);
		if ( msg_file == NULL ) {
			pr_debug("[0x%x] can not allocate memory\n", tfa98xx->i2c->addr);
			return  -ENOMEM;
		}
		msg_file->size = count;

		if (copy_from_user(msg_file->data, user_buf, count)) {
			kfree(msg_file);
			return -EFAULT;
		}

	if (tfa98xx->tfa->is_probus_device) {

		mutex_lock(&tfa98xx->dsp_lock);

		error = tfa98xx_send_data_to_dsp(msg_file->data,
			msg_file->size);

		if (err != 0)
			pr_err("[0x%x] dsp_msg error: %d\n", i2c->addr, err);

		mdelay(5);

		mutex_unlock(&tfa98xx->dsp_lock);
	} else {
		mutex_lock(&tfa98xx->dsp_lock);
		if ((msg_file->data[0] == 'M') && (msg_file->data[1] == 'G')) {
			error = tfaContWriteFile(tfa98xx->tfa,
				msg_file,
				0,
				0);
				/* int vstep_idx, int vstep_msg_idx both 0 */
			if (error != Tfa98xx_Error_Ok) {
				pr_debug("[0x%x] tfaContWriteFile error: %d\n",
					tfa98xx->i2c->addr, error);
				err = -EIO;
			}
		} else {
			error = dsp_msg(tfa98xx->tfa,
				msg_file->size,
				msg_file->data);
			if (error != Tfa98xx_Error_Ok) {
				pr_debug("[0x%x] dsp_msg error: %d\n",
					tfa98xx->i2c->addr, error);
				err = -EIO;
			}
		}
		mutex_unlock(&tfa98xx->dsp_lock);
	}

	kfree(msg_file);
	if (err)
		return err;
	return count;
}
/* -- RPC */

#ifdef CONFIG_DEBUG_FS
static int tfa98xx_dbgfs_pga_gain_get(void *data, u64 *val)
{
	struct i2c_client *i2c = (struct i2c_client *)data;
	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	int value;

	value = tfa_get_pga_gain(tfa98xx->tfa);
	if (value < 0)
		return -EINVAL;

	*val = value;
	return 0;
}

static int tfa98xx_dbgfs_pga_gain_set(void *data, u64 val)
{
	struct i2c_client *i2c = (struct i2c_client *)data;
	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);
	uint16_t value;
	int err;

	value = val & 0xffff;
	if (value > 7)
		return -EINVAL;

	err = tfa_set_pga_gain(tfa98xx->tfa, value);
	if (err < 0)
		return -EINVAL;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(tfa98xx_dbgfs_calib_otc_fops, tfa98xx_dbgfs_otc_get,
						tfa98xx_dbgfs_otc_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(tfa98xx_dbgfs_calib_mtpex_fops, tfa98xx_dbgfs_mtpex_get,
						tfa98xx_dbgfs_mtpex_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(tfa98xx_dbgfs_calib_temp_fops, tfa98xx_dbgfs_temp_get,
						tfa98xx_dbgfs_temp_set, "%llu\n");

DEFINE_SIMPLE_ATTRIBUTE(tfa98xx_dbgfs_pga_gain_fops, tfa98xx_dbgfs_pga_gain_get,
						tfa98xx_dbgfs_pga_gain_set, "%llu\n");
#endif //CONFIG_DEBUG_FS

static const struct file_operations tfa98xx_dbgfs_calib_start_fops = {
	.open = simple_open,
	.write = tfa98xx_dbgfs_start_set,
	.llseek = default_llseek,
};

static const struct file_operations tfa98xx_dbgfs_r_fops = {
	.open = simple_open,
	.read = tfa98xx_dbgfs_r_read,
	.llseek = default_llseek,
};

#ifdef OPLUS_BUG_COMPATIBILITY
static const struct file_operations tfa98xx_dbgfs_range_fops = {
	.open = simple_open,
	.read = tfa98xx_dbgfs_range_read,
	.llseek = default_llseek,
};
#endif /* OPLUS_BUG_COMPATIBILITY */

static const struct file_operations tfa98xx_dbgfs_version_fops = {
	.open = simple_open,
	.read = tfa98xx_dbgfs_version_read,
	.llseek = default_llseek,
};

static const struct file_operations tfa98xx_dbgfs_dsp_state_fops = {
	.open = simple_open,
	.read = tfa98xx_dbgfs_dsp_state_get,
	.write = tfa98xx_dbgfs_dsp_state_set,
	.llseek = default_llseek,
};

static const struct file_operations tfa98xx_dbgfs_fw_state_fops = {
	.open = simple_open,
	.read = tfa98xx_dbgfs_fw_state_get,
	.llseek = default_llseek,
};

static const struct file_operations tfa98xx_dbgfs_rpc_fops = {
	.open = simple_open,
	.read = tfa98xx_dbgfs_rpc_read,
	.write = tfa98xx_dbgfs_rpc_send,
	.llseek = default_llseek,
};

static void tfa98xx_debug_init(struct tfa98xx *tfa98xx, struct i2c_client *i2c)
{
	char name[50];

	scnprintf(name, MAX_CONTROL_NAME, "%s-%x", i2c->name, i2c->addr);
#ifdef CONFIG_DEBUG_FS
	tfa98xx->dbg_dir = debugfs_create_dir(name, NULL);
	debugfs_create_file("OTC", 0664, tfa98xx->dbg_dir,
			    i2c,
			    &tfa98xx_dbgfs_calib_otc_fops);
	debugfs_create_file("MTPEX", 0664, tfa98xx->dbg_dir,
			    i2c,
			    &tfa98xx_dbgfs_calib_mtpex_fops);
	debugfs_create_file("TEMP", 0664, tfa98xx->dbg_dir,
			    i2c,
			    &tfa98xx_dbgfs_calib_temp_fops);
	debugfs_create_file("calibrate", 0664, tfa98xx->dbg_dir,
			    i2c,
			    &tfa98xx_dbgfs_calib_start_fops);
	debugfs_create_file("R", 0444, tfa98xx->dbg_dir,
			    i2c,
			    &tfa98xx_dbgfs_r_fops);
	#ifdef OPLUS_BUG_COMPATIBILITY
	debugfs_create_file("range", S_IRUGO, tfa98xx->dbg_dir,
						i2c, &tfa98xx_dbgfs_range_fops);
	#endif /* OPLUS_BUG_COMPATIBILITY */
	debugfs_create_file("version", 0444, tfa98xx->dbg_dir,
			    i2c,
			    &tfa98xx_dbgfs_version_fops);
	debugfs_create_file("dsp-state", 0664, tfa98xx->dbg_dir,
			    i2c,
			    &tfa98xx_dbgfs_dsp_state_fops);
	debugfs_create_file("fw-state", 0664, tfa98xx->dbg_dir,
			    i2c,
			    &tfa98xx_dbgfs_fw_state_fops);
	debugfs_create_file("rpc", 0664, tfa98xx->dbg_dir,
			    i2c, &tfa98xx_dbgfs_rpc_fops);

	if (tfa98xx->flags & TFA98XX_FLAG_SAAM_AVAILABLE) {
		dev_dbg(tfa98xx->dev, "Adding pga_gain debug interface\n");
		debugfs_create_file("pga_gain", 0444, tfa98xx->dbg_dir,
						tfa98xx->i2c,
						&tfa98xx_dbgfs_pga_gain_fops);
	}
#else /*CONFIG_DEBUG_FS*/
	tfa98xx->dbg_dir = proc_mkdir(name, NULL);
	proc_create_data("calibrate", S_IRUGO|S_IWUGO, tfa98xx->dbg_dir,
					&tfa98xx_dbgfs_calib_start_fops, i2c);
	proc_create_data("R", S_IRUGO, tfa98xx->dbg_dir,
					&tfa98xx_dbgfs_r_fops, i2c);
#ifdef OPLUS_ARCH_EXTENDS
	proc_create_data("range", S_IRUGO, tfa98xx->dbg_dir,
					&tfa98xx_dbgfs_range_fops, i2c);
#endif /* OPLUS_ARCH_EXTENDS */
	proc_create_data("version", S_IRUGO, tfa98xx->dbg_dir,
					&tfa98xx_dbgfs_version_fops, i2c);
	proc_create_data("dsp-state", S_IRUGO|S_IWUGO, tfa98xx->dbg_dir,
					&tfa98xx_dbgfs_dsp_state_fops, i2c);
	proc_create_data("fw-state", S_IRUGO|S_IWUGO, tfa98xx->dbg_dir,
					&tfa98xx_dbgfs_fw_state_fops, i2c);
	proc_create_data("rpc", S_IRUGO|S_IWUGO, tfa98xx->dbg_dir,
					&tfa98xx_dbgfs_rpc_fops, i2c);
#endif /*CONFIG_DEBUG_FS*/
}

static void tfa98xx_debug_remove(struct tfa98xx *tfa98xx)
{
#ifdef CONFIG_DEBUG_FS
        if (tfa98xx->dbg_dir)
            debugfs_remove_recursive(tfa98xx->dbg_dir);
#else
        if (tfa98xx->dbg_dir)
            proc_remove(tfa98xx->dbg_dir);
#endif/*CONFIG_DEBUG_FS*/
}


/* copies the profile basename (i.e. part until .) into buf */
static void get_profile_basename(char* buf, char* profile)
{
	int cp_len = 0, idx = 0;
	char *pch;

	pch = strchr(profile, '.');
	idx = pch - profile;
	cp_len = (pch != NULL) ? idx : (int) strlen(profile);
	memcpy(buf, profile, cp_len);
	buf[cp_len] = 0;
}

/* return the profile name accociated with id from the profile list */
static int get_profile_from_list(char *buf, int id)
{
	struct tfa98xx_baseprofile *bprof;

	list_for_each_entry(bprof, &profile_list, list) {
		if (bprof->item_id == id) {
			strcpy(buf, bprof->basename);
			return 0;
		}
	}

	return -1;
}

/* search for the profile in the profile list */
static int is_profile_in_list(char *profile, int len)
{
	struct tfa98xx_baseprofile *bprof;

	list_for_each_entry(bprof, &profile_list, list) {

		if ((len == bprof->len) && (0 == strncmp(bprof->basename, profile, len)))
			return 1;
	}

    return 0;
}

/*
 * for the profile with id, look if the requested samplerate is
 * supported, if found return the (container)profile for this
 * samplerate, on error or if not found return -1
 */
static int get_profile_id_for_sr(int id, unsigned int rate)
{
	int idx = 0;
	struct tfa98xx_baseprofile *bprof;

	list_for_each_entry(bprof, &profile_list, list) {
		if (id == bprof->item_id) {
			idx = tfa98xx_get_fssel(rate);
			if (idx < 0) {
				/* samplerate not supported */
				return -1;
			}

			return bprof->sr_rate_sup[idx];
		}
	}

	/* profile not found */
	return -1;
}

#if 0
/* check if this profile is a calibration profile */
static int is_calibration_profile(char *profile)
{
	if (strstr(profile, ".cal") != NULL)
		return 1;
	return 0;
}
#endif

/*
 * adds the (container)profile index of the samplerate found in
 * the (container)profile to a fixed samplerate table in the (mixer)profile
 */
static int add_sr_to_profile(struct tfa98xx *tfa98xx, char *basename, int len, int profile)
{
	struct tfa98xx_baseprofile *bprof;
	int idx = 0;
	unsigned int sr = 0;

	list_for_each_entry(bprof, &profile_list, list) {
		if ((len == bprof->len) && (0 == strncmp(bprof->basename, basename, len))) {
			/* add supported samplerate for this profile */
			sr = tfa98xx_get_profile_sr(tfa98xx->tfa, profile);
			if (!sr) {
				pr_err("unable to identify supported sample rate for %s\n", bprof->basename);
				return -1;
			}

			/* get the index for this samplerate */
			idx = tfa98xx_get_fssel(sr);
			if (idx < 0 || idx >= TFA98XX_NUM_RATES) {
				pr_err("invalid index for samplerate %d\n", idx);
				return -1;
			}

			/* enter the (container)profile for this samplerate at the corresponding index */
			bprof->sr_rate_sup[idx] = profile;

			pr_info("added profile:samplerate = [%d:%d] for mixer profile: %s\n", profile, sr, bprof->basename);
		}
	}

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
static struct snd_soc_codec *snd_soc_kcontrol_codec(struct snd_kcontrol *kcontrol)
{
	return snd_kcontrol_chip(kcontrol);
}
#endif

static int tfa98xx_get_vstep(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
#else
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
#endif

	int mixer_profile = kcontrol->private_value;
	int ret = 0;
	int profile;

	profile = get_profile_id_for_sr(mixer_profile, tfa98xx->rate);
	if (profile < 0) {
		pr_err("tfa98xx: tfa98xx_get_vstep: invalid profile %d (mixer_profile=%d, rate=%d)\n", profile, mixer_profile, tfa98xx->rate);
		return -EINVAL;
	}

	mutex_lock(&tfa98xx_mutex);
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		int vstep = tfa98xx->prof_vsteps[profile];

		ucontrol->value.integer.value[tfa98xx->tfa->dev_idx] =
				tfacont_get_max_vstep(tfa98xx->tfa, profile)
				- vstep - 1;
	}
	mutex_unlock(&tfa98xx_mutex);

	return ret;
}

static int tfa98xx_set_vstep(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
     #if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
     struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	 struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
     #else
     struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
     struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
     #endif

	int mixer_profile = kcontrol->private_value;
	int profile;
	int err = 0;
	int change = 0;

	if (no_start != 0)
		return 0;

	profile = get_profile_id_for_sr(mixer_profile, tfa98xx->rate);
	if (profile < 0) {
		pr_err("tfa98xx: tfa98xx_set_vstep: invalid profile %d (mixer_profile=%d, rate=%d)\n", profile, mixer_profile, tfa98xx->rate);
		return -EINVAL;
	}

	mutex_lock(&tfa98xx_mutex);
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		int vstep, vsteps;
		int ready = 0;
		int new_vstep;
		int value = ucontrol->value.integer.value[tfa98xx->tfa->dev_idx];

		vstep = tfa98xx->prof_vsteps[profile];
		vsteps = tfacont_get_max_vstep(tfa98xx->tfa, profile);

		if (vstep == vsteps - value - 1)
			continue;

		new_vstep = vsteps - value - 1;

		if (new_vstep < 0)
			new_vstep = 0;

		tfa98xx->prof_vsteps[profile] = new_vstep;

#ifndef TFA98XX_ALSA_CTRL_PROF_CHG_ON_VOL
		if (profile == tfa98xx->profile) {
#endif
			/* this is the active profile, program the new vstep */
			tfa98xx->vstep = new_vstep;
			mutex_lock(&tfa98xx->dsp_lock);
			tfa98xx_dsp_system_stable(tfa98xx->tfa, &ready);

			if (ready) {
				err = tfa98xx_tfa_start(tfa98xx, tfa98xx->profile, tfa98xx->vstep);
				if (err) {
					pr_err("Write vstep error: %d\n", err);
				} else {
					pr_info("Succesfully changed vstep index!\n");
					change = 1;
				}
			}

			mutex_unlock(&tfa98xx->dsp_lock);
#ifndef TFA98XX_ALSA_CTRL_PROF_CHG_ON_VOL
		}
#endif
		pr_info("%d: vstep:%d, (control value: %d) - profile %d\n",
			tfa98xx->tfa->dev_idx, new_vstep, value, profile);
	}

	if (change) {
		list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
			mutex_lock(&tfa98xx->dsp_lock);
			tfa_dev_set_state(tfa98xx->tfa, TFA_STATE_UNMUTE);
			mutex_unlock(&tfa98xx->dsp_lock);
		}
	}

	mutex_unlock(&tfa98xx_mutex);

	return change;
}

static int tfa98xx_info_vstep(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_info *uinfo)
{
      #if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
      struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	  struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
      #else
      struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
      struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
      #endif

	int mixer_profile = tfa98xx_mixer_profile;
	int profile = get_profile_id_for_sr(mixer_profile, tfa98xx->rate);
	if (profile < 0) {
		pr_err("tfa98xx: tfa98xx_info_vstep: invalid profile %d (mixer_profile=%d, rate=%d)\n", profile, mixer_profile, tfa98xx->rate);
		return -EINVAL;
	}

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	mutex_lock(&tfa98xx_mutex);
	uinfo->count = tfa98xx_device_count;
	mutex_unlock(&tfa98xx_mutex);
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = max(0, tfacont_get_max_vstep(tfa98xx->tfa, profile) - 1);
	pr_info("vsteps count: %d [prof=%d]\n", tfacont_get_max_vstep(tfa98xx->tfa, profile),
			profile);
	return 0;
}

static int tfa98xx_get_profile(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	mutex_lock(&tfa98xx_mutex);
	ucontrol->value.integer.value[0] = tfa98xx_mixer_profile;
	mutex_unlock(&tfa98xx_mutex);

	return 0;
}

static int tfa98xx_set_profile(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
    #else
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
    #endif
	int change = 0;
	int new_profile;
	int prof_idx;
	int profile_count = tfa98xx_mixer_profiles;
	int profile = tfa98xx_mixer_profile;

	if (no_start != 0)
		return 0;

	new_profile = ucontrol->value.integer.value[0];
	if (new_profile == profile)
		return 0;

	if ((new_profile < 0) || (new_profile >= profile_count)) {
		pr_err("not existing profile (%d)\n", new_profile);
		return -EINVAL;
	}

	/* get the container profile for the requested sample rate */
	prof_idx = get_profile_id_for_sr(new_profile, tfa98xx->rate);
	if (prof_idx < 0) {
		pr_err("tfa98xx: sample rate [%d] not supported for this mixer profile [%d].\n", tfa98xx->rate, new_profile);
		return 0;
	}
	pr_info("selected container profile [%d]\n", prof_idx);

	/* update mixer profile */
	tfa98xx_mixer_profile = new_profile;

	#ifdef OPLUS_BUG_COMPATIBILITY
	pr_info("selected container profile [%d]\n", new_profile);
	#endif /* OPLUS_BUG_COMPATIBILITY */

	#ifndef OPLUS_BUG_COMPATIBILITY
	mutex_lock(&tfa98xx_mutex);
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		int err;
		int ready = 0;

		/* update 'real' profile (container profile) */
		tfa98xx->profile = prof_idx;
		tfa98xx->vstep = tfa98xx->prof_vsteps[prof_idx];

		/* Don't call tfa_dev_start() if there is no clock. */
		mutex_lock(&tfa98xx->dsp_lock);
		tfa98xx_dsp_system_stable(tfa98xx->tfa, &ready);
		if (ready) {
			/* Also re-enables the interrupts */
			err = tfa98xx_tfa_start(tfa98xx, prof_idx, tfa98xx->vstep);
			if (err) {
				pr_info("Write profile error: %d\n", err);
			} else {
				pr_info("Changed to profile %d (vstep = %d)\n",
				         prof_idx, tfa98xx->vstep);
				change = 1;
			}
		}
		mutex_unlock(&tfa98xx->dsp_lock);

		/* Flag DSP as invalidated as the profile change may invalidate the
		 * current DSP configuration. That way, further stream start can
		 * trigger a tfa_dev_start.
		 */
		tfa98xx->dsp_init = TFA98XX_DSP_INIT_INVALIDATED;
	}

	if (change) {
		list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
			mutex_lock(&tfa98xx->dsp_lock);
			tfa_dev_set_state(tfa98xx->tfa, TFA_STATE_UNMUTE);
			mutex_unlock(&tfa98xx->dsp_lock);
		}
	}

	mutex_unlock(&tfa98xx_mutex);
	#else /* OPLUS_BUG_COMPATIBILITY */
	change = 1;
	#endif /*OPLUS_BUG_COMPATIBILITY*/

	return change;
}

static int tfa98xx_info_profile(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_info *uinfo)
{
	char profile_name[MAX_CONTROL_NAME] = {0};
	int count = tfa98xx_mixer_profiles, err = -1;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = count;

	if (uinfo->value.enumerated.item >= count)
		uinfo->value.enumerated.item = count - 1;

	err = get_profile_from_list(profile_name, uinfo->value.enumerated.item);
	if (err != 0)
		return -EINVAL;

	strcpy(uinfo->value.enumerated.name, profile_name);

	return 0;
}

static int tfa98xx_info_stop_ctl(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	mutex_lock(&tfa98xx_mutex);
	uinfo->count = tfa98xx_device_count;
	mutex_unlock(&tfa98xx_mutex);
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;

	return 0;
}

static int tfa98xx_get_stop_ctl(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct tfa98xx *tfa98xx;

	mutex_lock(&tfa98xx_mutex);
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		ucontrol->value.integer.value[tfa98xx->tfa->dev_idx] = 0;
	}
	mutex_unlock(&tfa98xx_mutex);

	return 0;
}

static int tfa98xx_set_stop_ctl(struct snd_kcontrol *kcontrol,
			        struct snd_ctl_elem_value *ucontrol)
{
	struct tfa98xx *tfa98xx;

	mutex_lock(&tfa98xx_mutex);
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		int ready = 0;
		int i = tfa98xx->tfa->dev_idx;

		pr_info("%d: %ld\n", i, ucontrol->value.integer.value[i]);

		tfa98xx_dsp_system_stable(tfa98xx->tfa, &ready);

		if ((ucontrol->value.integer.value[i] != 0) && ready) {
			cancel_delayed_work_sync(&tfa98xx->monitor_work);

			cancel_delayed_work_sync(&tfa98xx->init_work);
			if (tfa98xx->dsp_fw_state != TFA98XX_DSP_FW_OK)
				continue;

			mutex_lock(&tfa98xx->dsp_lock);
			tfa_dev_stop(tfa98xx->tfa);
			tfa98xx->dsp_init = TFA98XX_DSP_INIT_STOPPED;
			mutex_unlock(&tfa98xx->dsp_lock);
		}

		ucontrol->value.integer.value[i] = 0;
	}
	mutex_unlock(&tfa98xx_mutex);

	return 1;
}

static int tfa98xx_info_cal_ctl(struct snd_kcontrol *kcontrol,
		                struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	mutex_lock(&tfa98xx_mutex);
	uinfo->count = tfa98xx_device_count;
	mutex_unlock(&tfa98xx_mutex);
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 0xffff; /* 16 bit value */

	return 0;
}

static int tfa98xx_set_cal_ctl(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct tfa98xx *tfa98xx;

	#ifdef OPLUS_ARCH_EXTENDS
	if(!g_tfa98xx_firmware_status){
		pr_info("%s, firmware is not ready\n",__func__);
		return -1;
	}
	#endif

	mutex_lock(&tfa98xx_mutex);
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		enum Tfa98xx_Error err;
		int i = tfa98xx->tfa->dev_idx;

		tfa98xx->cal_data = (uint16_t)ucontrol->value.integer.value[i];

		mutex_lock(&tfa98xx->dsp_lock);
		err = tfa98xx_write_re25(tfa98xx->tfa, tfa98xx->cal_data);
		tfa98xx->set_mtp_cal = (err != tfa_error_ok);
		if (tfa98xx->set_mtp_cal == false) {
			pr_info("Calibration value (%d) set in mtp\n",
			        tfa98xx->cal_data);
		}
		mutex_unlock(&tfa98xx->dsp_lock);
	}
	mutex_unlock(&tfa98xx_mutex);

	return 1;
}

static int tfa98xx_get_cal_ctl(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct tfa98xx *tfa98xx;

    #ifdef OPLUS_ARCH_EXTENDS
	if(!g_tfa98xx_firmware_status){
		pr_info("%s, firmware is not ready\n",__func__);
		return -1;
	}
	#endif

	mutex_lock(&tfa98xx_mutex);
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		mutex_lock(&tfa98xx->dsp_lock);
		ucontrol->value.integer.value[tfa98xx->tfa->dev_idx] = tfa_dev_mtp_get(tfa98xx->tfa, TFA_MTP_RE25_PRIM);
		mutex_unlock(&tfa98xx->dsp_lock);
	}
	mutex_unlock(&tfa98xx_mutex);

	return 0;
}

#define CHIP_SELECTOR_STEREO	(0)
#define CHIP_SELECTOR_LEFT	(1)
#define CHIP_SELECTOR_RIGHT	(2)
#define CHIP_SELECTOR_RCV	(3)
#define CHIP_LEFT_ADDR		(0x34)
#define CHIP_RIGHT_ADDR	(0x35)
#define CHIP_RCV_ADDR		(0x34)

static int tfa98xx_info_stereo_ctl(struct snd_kcontrol *kcontrol,
                                struct snd_ctl_elem_info *uinfo)
{
        uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
        uinfo->count = 1;
        uinfo->value.integer.min = 0;
        uinfo->value.integer.max = 3;
        return 0;
}

static int tfa98xx_set_stereo_ctl(struct snd_kcontrol *kcontrol,
                               struct snd_ctl_elem_value *ucontrol)
{
	struct tfa98xx *tfa98xx;
	int selector;

	selector = ucontrol->value.integer.value[0];

	pr_info("%s: selector = %d\n", __func__, selector);

	mutex_lock(&tfa98xx_mutex);
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		if (selector == CHIP_SELECTOR_LEFT) {
			if (tfa98xx->i2c->addr == CHIP_LEFT_ADDR)
				tfa98xx->flags |= TFA98XX_FLAG_CHIP_SELECTED;
			else
				tfa98xx->flags &= ~TFA98XX_FLAG_CHIP_SELECTED;
		}
		else if (selector == CHIP_SELECTOR_RIGHT) {
			if (tfa98xx->i2c->addr == CHIP_RIGHT_ADDR)
				tfa98xx->flags |= TFA98XX_FLAG_CHIP_SELECTED;
			else
				tfa98xx->flags &= ~TFA98XX_FLAG_CHIP_SELECTED;
		}
		else if (selector == CHIP_SELECTOR_RCV) {
			if (tfa98xx->i2c->addr == CHIP_RCV_ADDR) {
				tfa98xx->flags |= TFA98XX_FLAG_CHIP_SELECTED;
			}
			else
				tfa98xx->flags &= ~TFA98XX_FLAG_CHIP_SELECTED;
		} else {
			tfa98xx->flags |= TFA98XX_FLAG_CHIP_SELECTED;
		}
	}

	if (tfa98xx_dual_spk) {
		if (selector == CHIP_SELECTOR_STEREO) {
			tfa_dual_start_cnt = 0;
		} else {
			tfa_dual_start_cnt = 99;
		}
	}

	mutex_unlock(&tfa98xx_mutex);

	return 1;
}

static int tfa98xx_get_stereo_ctl(struct snd_kcontrol *kcontrol,
                               struct snd_ctl_elem_value *ucontrol)
{
	struct tfa98xx *tfa98xx;

	mutex_lock(&tfa98xx_mutex);
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		ucontrol->value.integer.value[0] = tfa98xx->flags;
	}
	mutex_unlock(&tfa98xx_mutex);

	return 0;
}
#ifdef OPLUS_BUG_COMPATIBILITY
/* check if this profile is a calibration profile */
static int tfa98xx_show_firmware_status(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = g_tfa98xx_firmware_status;
	pr_info("g_tfa98xx_firmware_status=%d\n", g_tfa98xx_firmware_status);
	return 0;
}

static int tfa98xx_load_firmware_kcontrol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;

	/* we don't need load it again once firmware was loaded. */
	if (g_tfa98xx_firmware_status > 0) {
		pr_info("the firmware was loaded, we don't need load it again!!!\n");
		return ret;
	}

	mutex_lock(&tfa98xx_mutex);
	pr_info("g_tfa98xx_firmware_status=%d    new=%d\n",
		g_tfa98xx_firmware_status,
		(uint8_t)ucontrol->value.integer.value[0]);

	if (ucontrol->value.integer.value[0] > 0) {
		struct tfa98xx *tfa98xx = NULL;

                g_tfa98xx_firmware_status =
			(uint8_t)ucontrol->value.integer.value[0];

		list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
			ret = tfa98xx_load_container(tfa98xx);
			pr_info("Container loading requested: %d\n", ret);
		}
	}

	mutex_unlock(&tfa98xx_mutex);
	return 0;
}

static int tfa98xx_info_firmware(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	mutex_lock(&tfa98xx_mutex);
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	mutex_unlock(&tfa98xx_mutex);
	return 0;
}
#endif

static int tfa98xx_create_controls(struct tfa98xx *tfa98xx)
{
	int prof, nprof, mix_index = 0;
	int  nr_controls = 0, id = 0;
	char *name;
	#ifdef OPLUS_ARCH_EXTENDS
	char *prof_name = NULL;
	#endif
	struct tfa98xx_baseprofile *bprofile;

	/* Create the following controls:
	 *  - enum control to select the active profile
	 *  - one volume control for each profile hosting a vstep
	 *  - Stop control on TFA1 devices
	 */

	nr_controls = 2; /* Profile and stop control */

	if (tfa98xx_dual_spk)
		nr_controls += 1;

	if (tfa98xx->flags & TFA98XX_FLAG_CALIBRATION_CTL)
		nr_controls += 1; /* calibration */

	/* allocate the tfa98xx_controls base on the nr of profiles */
	nprof = tfa_cnt_get_dev_nprof(tfa98xx->tfa);
	for (prof = 0; prof < nprof; prof++) {
		if (tfacont_get_max_vstep(tfa98xx->tfa, prof))
			nr_controls++; /* Playback Volume control */
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	tfa98xx_controls = devm_kzalloc(tfa98xx->codec->dev,
			nr_controls * sizeof(tfa98xx_controls[0]), GFP_KERNEL);
#else
 	tfa98xx_controls = devm_kzalloc(tfa98xx->component->dev,
		nr_controls * sizeof(tfa98xx_controls[0]), GFP_KERNEL);

#endif
	if (!tfa98xx_controls)
		return -ENOMEM;

	/* Create a mixer item for selecting the active profile */
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	name = devm_kzalloc(tfa98xx->codec->dev, MAX_CONTROL_NAME, GFP_KERNEL);
    #else
	name = devm_kzalloc(tfa98xx->component->dev, MAX_CONTROL_NAME, GFP_KERNEL);
    #endif
	if (!name)
		return -ENOMEM;
	scnprintf(name, MAX_CONTROL_NAME, "%s Profile", tfa98xx->fw.name);
	tfa98xx_controls[mix_index].name = name;
	tfa98xx_controls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	tfa98xx_controls[mix_index].info = tfa98xx_info_profile;
	tfa98xx_controls[mix_index].get = tfa98xx_get_profile;
	tfa98xx_controls[mix_index].put = tfa98xx_set_profile;
	// tfa98xx_controls[mix_index].private_value = profs; /* save number of profiles */
	mix_index++;
#ifdef OPLUS_BUG_COMPATIBILITY
	tfa98xx_controls[mix_index].name = "TFA98XX Firmware";
	tfa98xx_controls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	tfa98xx_controls[mix_index].info = tfa98xx_info_firmware;
	tfa98xx_controls[mix_index].get = tfa98xx_show_firmware_status;
	tfa98xx_controls[mix_index].put = tfa98xx_load_firmware_kcontrol;
	mix_index++;
#endif
	/* create mixer items for each profile that has volume */
	for (prof = 0; prof < nprof; prof++) {
		/* create an new empty profile */
     	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		bprofile = devm_kzalloc(tfa98xx->codec->dev, sizeof(*bprofile), GFP_KERNEL);
        #else
		bprofile = devm_kzalloc(tfa98xx->component->dev, sizeof(*bprofile), GFP_KERNEL);
    	#endif
		if (!bprofile)
			return -ENOMEM;

		bprofile->len = 0;
		bprofile->item_id = -1;
		INIT_LIST_HEAD(&bprofile->list);

		/* copy profile name into basename until the . */
		#ifdef OPLUS_ARCH_EXTENDS
		if ((prof_name = tfa_cont_profile_name(tfa98xx, prof)) != NULL) {
			get_profile_basename(bprofile->basename, prof_name);
		}
		#else
		get_profile_basename(bprofile->basename, tfa_cont_profile_name(tfa98xx, prof));
		#endif

		bprofile->len = strlen(bprofile->basename);

		/*
		 * search the profile list for a profile with basename, if it is not found then
		 * add it to the list and add a new mixer control (if it has vsteps)
		 * also, if it is a calibration profile, do not add it to the list
		 */
		#ifdef OPLUS_BUG_COMPATIBILITY
		if (is_profile_in_list(bprofile->basename, bprofile->len) == 0) {
		#else /* OPLUS_BUG_COMPATIBILITY */
		if ((is_profile_in_list(bprofile->basename, bprofile->len) == 0) &&
			 is_calibration_profile(tfa_cont_profile_name(tfa98xx, prof)) == 0) {
		#endif /* OPLUS_BUG_COMPATIBILITY */
			/* the profile is not present, add it to the list */
			list_add(&bprofile->list, &profile_list);
			bprofile->item_id = id++;

			pr_info("profile added [%d]: %s\n", bprofile->item_id, bprofile->basename);

			if (tfacont_get_max_vstep(tfa98xx->tfa, prof)) {
			#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
				name = devm_kzalloc(tfa98xx->codec->dev, MAX_CONTROL_NAME, GFP_KERNEL);
            #else
				name = devm_kzalloc(tfa98xx->component->dev, MAX_CONTROL_NAME, GFP_KERNEL);
			#endif
				if (!name)
					return -ENOMEM;

				scnprintf(name, MAX_CONTROL_NAME, "%s %s Playback Volume",
				tfa98xx->fw.name, bprofile->basename);

				tfa98xx_controls[mix_index].name = name;
				tfa98xx_controls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
				tfa98xx_controls[mix_index].info = tfa98xx_info_vstep;
				tfa98xx_controls[mix_index].get = tfa98xx_get_vstep;
				tfa98xx_controls[mix_index].put = tfa98xx_set_vstep;
				tfa98xx_controls[mix_index].private_value = bprofile->item_id; /* save profile index */
				mix_index++;
			}
		}

		/* look for the basename profile in the list of mixer profiles and add the
		   container profile index to the supported samplerates of this mixer profile */
		add_sr_to_profile(tfa98xx, bprofile->basename, bprofile->len, prof);
	}

	/* set the number of user selectable profiles in the mixer */
	tfa98xx_mixer_profiles = id;

	/* Create a mixer item for stop control on TFA1 */
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	name = devm_kzalloc(tfa98xx->codec->dev, MAX_CONTROL_NAME, GFP_KERNEL);
	#else
	name = devm_kzalloc(tfa98xx->component->dev, MAX_CONTROL_NAME, GFP_KERNEL);
	#endif
	if (!name)
		return -ENOMEM;

	scnprintf(name, MAX_CONTROL_NAME, "%s Stop", tfa98xx->fw.name);
	tfa98xx_controls[mix_index].name = name;
	tfa98xx_controls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	tfa98xx_controls[mix_index].info = tfa98xx_info_stop_ctl;
	tfa98xx_controls[mix_index].get = tfa98xx_get_stop_ctl;
	tfa98xx_controls[mix_index].put = tfa98xx_set_stop_ctl;
	mix_index++;

	if (tfa98xx->flags & TFA98XX_FLAG_CALIBRATION_CTL) {
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		name = devm_kzalloc(tfa98xx->codec->dev, MAX_CONTROL_NAME, GFP_KERNEL);
	#else
	    name = devm_kzalloc(tfa98xx->component->dev, MAX_CONTROL_NAME, GFP_KERNEL);
	#endif
		if (!name)
			return -ENOMEM;

		scnprintf(name, MAX_CONTROL_NAME, "%s Calibration", tfa98xx->fw.name);
		tfa98xx_controls[mix_index].name = name;
		tfa98xx_controls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		tfa98xx_controls[mix_index].info = tfa98xx_info_cal_ctl;
		tfa98xx_controls[mix_index].get = tfa98xx_get_cal_ctl;
		tfa98xx_controls[mix_index].put = tfa98xx_set_cal_ctl;
		mix_index++;
	}

	if (tfa98xx_dual_spk) {
		tfa98xx_controls[mix_index].name = "TFA_CHIP_SELECTOR";
		tfa98xx_controls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		tfa98xx_controls[mix_index].info = tfa98xx_info_stereo_ctl;
		tfa98xx_controls[mix_index].get = tfa98xx_get_stereo_ctl;
		tfa98xx_controls[mix_index].put = tfa98xx_set_stereo_ctl;
		mix_index++;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	return snd_soc_add_codec_controls(tfa98xx->codec,
#else
	return snd_soc_add_component_controls(tfa98xx->component,
#endif
		tfa98xx_controls, mix_index);
}

static void *tfa98xx_devm_kstrdup(struct device *dev, char *buf)
{
	char *str = devm_kzalloc(dev, strlen(buf) + 1, GFP_KERNEL);
	if (!str)
		return str;
	memcpy(str, buf, strlen(buf));
	return str;
}

static int tfa98xx_append_i2c_address(struct device *dev,
				struct i2c_client *i2c,
				struct snd_soc_dapm_widget *widgets,
				int num_widgets,
				struct snd_soc_dai_driver *dai_drv,
				int num_dai)
{
	char buf[50];
	int i;
	int i2cbus = i2c->adapter->nr;
	int addr = i2c->addr;
	tfa98xx_i2cbus = i2cbus;
	if (dai_drv && num_dai > 0)
		for(i = 0; i < num_dai; i++) {
			snprintf(buf, 50, "%s-%x-%x",dai_drv[i].name, i2cbus,
				addr);
			dai_drv[i].name = tfa98xx_devm_kstrdup(dev, buf);

			snprintf(buf, 50, "%s-%x-%x",
						dai_drv[i].playback.stream_name,
						i2cbus, addr);
			dai_drv[i].playback.stream_name = tfa98xx_devm_kstrdup(dev, buf);

			snprintf(buf, 50, "%s-%x-%x",
						dai_drv[i].capture.stream_name,
						i2cbus, addr);
			dai_drv[i].capture.stream_name = tfa98xx_devm_kstrdup(dev, buf);
		}

	/* the idea behind this is convert:
	 * SND_SOC_DAPM_AIF_IN("AIF IN", "AIF Playback", 0, SND_SOC_NOPM, 0, 0),
	 * into:
	 * SND_SOC_DAPM_AIF_IN("AIF IN", "AIF Playback-2-36", 0, SND_SOC_NOPM, 0, 0),
	 */
	if (widgets && num_widgets > 0)
		for(i = 0; i < num_widgets; i++) {
			if(!widgets[i].sname)
				continue;
			if((widgets[i].id == snd_soc_dapm_aif_in)
				|| (widgets[i].id == snd_soc_dapm_aif_out)) {
				snprintf(buf, 50, "%s-%x-%x", widgets[i].sname,
					i2cbus, addr);
				widgets[i].sname = tfa98xx_devm_kstrdup(dev, buf);
			}
		}

	return 0;
}

static struct snd_soc_dapm_widget tfa98xx_dapm_widgets_common[] = {
	/* Stream widgets */
	SND_SOC_DAPM_AIF_IN("AIF IN", "AIF Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF OUT", "AIF Capture", 0, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_OUTPUT("OUTL"),
	SND_SOC_DAPM_INPUT("AEC Loopback"),
};

static struct snd_soc_dapm_widget tfa98xx_dapm_widgets_stereo[] = {
	SND_SOC_DAPM_OUTPUT("OUTR"),
};

static struct snd_soc_dapm_widget tfa98xx_dapm_widgets_saam[] = {
	SND_SOC_DAPM_INPUT("SAAM MIC"),
};

static struct snd_soc_dapm_widget tfa9888_dapm_inputs[] = {
	SND_SOC_DAPM_INPUT("DMIC1"),
	SND_SOC_DAPM_INPUT("DMIC2"),
	SND_SOC_DAPM_INPUT("DMIC3"),
	SND_SOC_DAPM_INPUT("DMIC4"),
};

static const struct snd_soc_dapm_route tfa98xx_dapm_routes_common[] = {
	{ "OUTL", NULL, "AIF IN" },
	{ "AIF OUT", NULL, "AEC Loopback" },
};

static const struct snd_soc_dapm_route tfa98xx_dapm_routes_saam[] = {
	{ "AIF OUT", NULL, "SAAM MIC" },
};

static const struct snd_soc_dapm_route tfa98xx_dapm_routes_stereo[] = {
	{ "OUTR", NULL, "AIF IN" },
};

static const struct snd_soc_dapm_route tfa9888_input_dapm_routes[] = {
	{ "AIF OUT", NULL, "DMIC1" },
	{ "AIF OUT", NULL, "DMIC2" },
	{ "AIF OUT", NULL, "DMIC3" },
	{ "AIF OUT", NULL, "DMIC4" },
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,2,0)
static struct snd_soc_dapm_context *snd_soc_codec_get_dapm(struct snd_soc_codec *codec)
{
	return &codec->dapm;
}
#endif

static void tfa98xx_add_widgets(struct tfa98xx *tfa98xx)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct snd_soc_dapm_context *dapm = snd_soc_codec_get_dapm(tfa98xx->codec);
#else
	struct snd_soc_dapm_context *dapm = snd_soc_component_get_dapm(tfa98xx->component);
#endif
	struct snd_soc_dapm_widget *widgets;
	unsigned int num_dapm_widgets = ARRAY_SIZE(tfa98xx_dapm_widgets_common);

	widgets = devm_kzalloc(&tfa98xx->i2c->dev,
			sizeof(struct snd_soc_dapm_widget) *
				ARRAY_SIZE(tfa98xx_dapm_widgets_common),
			GFP_KERNEL);
	if (!widgets)
		return;
	memcpy(widgets, tfa98xx_dapm_widgets_common,
			sizeof(struct snd_soc_dapm_widget) *
				ARRAY_SIZE(tfa98xx_dapm_widgets_common));

	tfa98xx_append_i2c_address(&tfa98xx->i2c->dev,
				tfa98xx->i2c,
				widgets,
				num_dapm_widgets,
				NULL,
				0);

	snd_soc_dapm_new_controls(dapm, widgets,
				  ARRAY_SIZE(tfa98xx_dapm_widgets_common));
	snd_soc_dapm_add_routes(dapm, tfa98xx_dapm_routes_common,
				ARRAY_SIZE(tfa98xx_dapm_routes_common));

	if (tfa98xx->flags & TFA98XX_FLAG_STEREO_DEVICE) {
		snd_soc_dapm_new_controls(dapm, tfa98xx_dapm_widgets_stereo,
					  ARRAY_SIZE(tfa98xx_dapm_widgets_stereo));
		snd_soc_dapm_add_routes(dapm, tfa98xx_dapm_routes_stereo,
					ARRAY_SIZE(tfa98xx_dapm_routes_stereo));
	}

	if (tfa98xx->flags & TFA98XX_FLAG_MULTI_MIC_INPUTS) {
		snd_soc_dapm_new_controls(dapm, tfa9888_dapm_inputs,
					  ARRAY_SIZE(tfa9888_dapm_inputs));
		snd_soc_dapm_add_routes(dapm, tfa9888_input_dapm_routes,
					ARRAY_SIZE(tfa9888_input_dapm_routes));
	}

	if (tfa98xx->flags & TFA98XX_FLAG_SAAM_AVAILABLE) {
		snd_soc_dapm_new_controls(dapm, tfa98xx_dapm_widgets_saam,
					  ARRAY_SIZE(tfa98xx_dapm_widgets_saam));
		snd_soc_dapm_add_routes(dapm, tfa98xx_dapm_routes_saam,
					ARRAY_SIZE(tfa98xx_dapm_routes_saam));
	}
}

/* I2C wrapper functions */
enum Tfa98xx_Error tfa98xx_write_register16(struct tfa_device *tfa,
					unsigned char subaddress,
					unsigned short value)
{
	enum Tfa98xx_Error error = Tfa98xx_Error_Ok;
	struct tfa98xx *tfa98xx;
	int ret;
	int retries = I2C_RETRIES;

	if (tfa == NULL) {
		pr_err("No device available\n");
		return Tfa98xx_Error_Fail;
	}

	tfa98xx = (struct tfa98xx *)tfa->data;
	if (!tfa98xx || !tfa98xx->regmap) {
		pr_err("No tfa98xx regmap available\n");
		return Tfa98xx_Error_Bad_Parameter;
	}
retry:
	ret = regmap_write(tfa98xx->regmap, subaddress, value);
	if (ret < 0) {
		pr_warn("i2c error, retries left: %d\n", retries);
		if (retries) {
			retries--;
			msleep(I2C_RETRY_DELAY);
			goto retry;
		}
		return Tfa98xx_Error_Fail;
	}
	if (tfa98xx_kmsg_regs)
		dev_dbg(&tfa98xx->i2c->dev, "  WR reg=0x%02x, val=0x%04x %s\n",
		        subaddress, value,
		        ret<0? "Error!!" : "");

	if (tfa98xx_ftrace_regs)
		tfa98xx_trace_printk("\tWR     reg=0x%02x, val=0x%04x %s\n",
		                     subaddress, value,
		                     ret<0? "Error!!" : "");
	return error;
}

enum Tfa98xx_Error tfa98xx_read_register16(struct tfa_device *tfa,
					unsigned char subaddress,
					unsigned short *val)
{
	enum Tfa98xx_Error error = Tfa98xx_Error_Ok;
	struct tfa98xx *tfa98xx;
	unsigned int value;
	int retries = I2C_RETRIES;
	int ret;

	if (tfa == NULL) {
		pr_err("No device available\n");
		return Tfa98xx_Error_Fail;
	}

	tfa98xx = (struct tfa98xx *)tfa->data;
	if (!tfa98xx || !tfa98xx->regmap) {
		pr_err("No tfa98xx regmap available\n");
		return Tfa98xx_Error_Bad_Parameter;
	}
retry:
	ret = regmap_read(tfa98xx->regmap, subaddress, &value);
	if (ret < 0) {
		pr_warn("i2c error at subaddress 0x%x, retries left: %d\n", subaddress, retries);
		if (retries) {
			retries--;
			msleep(I2C_RETRY_DELAY);
			goto retry;
		}
		return Tfa98xx_Error_Fail;
	}
	*val = value & 0xffff;

	if (tfa98xx_kmsg_regs)
		dev_dbg(&tfa98xx->i2c->dev, "RD   reg=0x%02x, val=0x%04x %s\n",
		        subaddress, *val,
		        ret<0? "Error!!" : "");
	if (tfa98xx_ftrace_regs)
		tfa98xx_trace_printk("\tRD     reg=0x%02x, val=0x%04x %s\n",
		                    subaddress, *val,
		                    ret<0? "Error!!" : "");

	return error;
}


/*
 * init external dsp
 */
enum Tfa98xx_Error
tfa98xx_init_dsp(struct tfa_device *tfa)
{
	return Tfa98xx_Error_Not_Supported;
}

int tfa98xx_get_dsp_status(struct tfa_device *tfa)
{
	return 0;
}

/*
 * write external dsp message
 */
enum Tfa98xx_Error
tfa98xx_write_dsp(struct tfa_device *tfa,  int num_bytes, const char *command_buffer)
{
	int err = 0;
	//struct tfa98xx *tfa98xx = (struct tfa98xx *)tfa->data;
	uint8_t *buffer = NULL;

	buffer = kmalloc(num_bytes, GFP_KERNEL);
	if ( buffer == NULL ) {
		pr_err("[0x%x] can not allocate memory\n", tfa->slave_address);
		return	Tfa98xx_Error_Fail;
	}

	memcpy(buffer ,command_buffer, num_bytes);

	//mutex_lock(&tfa98xx->dsp_lock);

	err = tfa98xx_send_data_to_dsp(buffer, num_bytes);
	if (err) {
		pr_err("[0x%x] dsp_msg error: %d\n", tfa->slave_address, err);
	}

	//mutex_unlock(&tfa98xx->dsp_lock);
	kfree(buffer);

	mdelay(5);

	return Tfa98xx_Error_Ok;
}

/*
 * read external dsp message
 */
enum Tfa98xx_Error
tfa98xx_read_dsp(struct tfa_device *tfa,  int num_bytes, unsigned char *result_buffer)
{
	uint32_t DataLength = 0;
	enum Tfa98xx_Error error;
	//struct tfa98xx *tfa98xx = (struct tfa98xx *)tfa->data;

	//mutex_lock(&tfa98xx->dsp_lock);

	error = tfa98xx_receive_data_from_dsp(result_buffer, num_bytes, &DataLength);

	//mutex_unlock(&tfa98xx->dsp_lock);
	if (error) {
		pr_err("[0x%x] dsp_msg_read error: %d\n", tfa->slave_address, error);
		return Tfa98xx_Error_Fail;
	}

	return Tfa98xx_Error_Ok;
}
/*
 * write/read external dsp message
 */
enum Tfa98xx_Error
tfa98xx_writeread_dsp(struct tfa_device *tfa, int command_length, void *command_buffer,
											  int result_length, void *result_buffer)
{
	return Tfa98xx_Error_Not_Supported;
}

enum Tfa98xx_Error tfa98xx_read_data(struct tfa_device *tfa,
				unsigned char reg,
				int len, unsigned char value[])
{
	enum Tfa98xx_Error error = Tfa98xx_Error_Ok;
	struct tfa98xx *tfa98xx;
	struct i2c_client *tfa98xx_client;
	int err;
	int tries = 0;
	struct i2c_msg msgs[] = {
		{
			.flags = 0,
			.len = 1,
			.buf = &reg,
		}, {
			.flags = I2C_M_RD,
			.len = len,
			.buf = value,
		},
	};

	if (tfa == NULL) {
		pr_err("No device available\n");
		return Tfa98xx_Error_Fail;
	}

	tfa98xx = (struct tfa98xx *)tfa->data;
	if (tfa98xx->i2c) {
		tfa98xx_client = tfa98xx->i2c;
		msgs[0].addr = tfa98xx_client->addr;
		msgs[1].addr = tfa98xx_client->addr;

		do {
			err = i2c_transfer(tfa98xx_client->adapter, msgs,
							ARRAY_SIZE(msgs));
			if (err != ARRAY_SIZE(msgs))
				msleep_interruptible(I2C_RETRY_DELAY);
		} while ((err != ARRAY_SIZE(msgs)) && (++tries < I2C_RETRIES));

		if (err != ARRAY_SIZE(msgs)) {
			dev_err(&tfa98xx_client->dev, "read transfer error %d\n",
									err);
			error = Tfa98xx_Error_Fail;
		}

		if (tfa98xx_kmsg_regs)
			dev_dbg(&tfa98xx_client->dev, "RD-DAT reg=0x%02x, len=%d\n",
								reg, len);
		if (tfa98xx_ftrace_regs)
			tfa98xx_trace_printk("\t\tRD-DAT reg=0x%02x, len=%d\n",
					reg, len);
	} else {
		pr_err("No device available\n");
		error = Tfa98xx_Error_Fail;
	}
	return error;
}

enum Tfa98xx_Error tfa98xx_write_raw(struct tfa_device *tfa,
				int len,
				const unsigned char data[])
{
	enum Tfa98xx_Error error = Tfa98xx_Error_Ok;
	struct tfa98xx *tfa98xx;
	int ret;
	int retries = I2C_RETRIES;


	if (tfa == NULL) {
		pr_err("No device available\n");
		return Tfa98xx_Error_Fail;
	}

	tfa98xx = (struct tfa98xx *)tfa->data;

retry:
	ret = i2c_master_send(tfa98xx->i2c, data, len);
	if (ret < 0) {
		pr_warn("i2c error, retries left: %d\n", retries);
		if (retries) {
			retries--;
			msleep(I2C_RETRY_DELAY);
			goto retry;
		}
	}

	if (ret == len) {
		if (tfa98xx_kmsg_regs)
			dev_dbg(&tfa98xx->i2c->dev, "  WR-RAW len=%d\n", len);
		if (tfa98xx_ftrace_regs)
			tfa98xx_trace_printk("\t\tWR-RAW len=%d\n", len);
		return Tfa98xx_Error_Ok;
	}
	pr_err("  WR-RAW (len=%d) Error I2C send size mismatch %d\n", len, ret);
	error = Tfa98xx_Error_Fail;

	return error;
}

/* Interrupts management */

static void tfa98xx_interrupt_enable_tfa2(struct tfa98xx *tfa98xx, bool enable)
{
	/* Only for 0x72 we need to enable NOCLK interrupts */
	if (tfa98xx->flags & TFA98XX_FLAG_REMOVE_PLOP_NOISE)
		tfa_irq_ena(tfa98xx->tfa, tfa9912_irq_stnoclk, enable);

	if (tfa98xx->flags & TFA98XX_FLAG_LP_MODES) {
		tfa_irq_ena(tfa98xx->tfa, 36, enable); /* FIXME: IELP0 does not excist for 9912 */
		tfa_irq_ena(tfa98xx->tfa, tfa9912_irq_stclpr, enable);
	}
}

/* Check if tap-detection can and shall be enabled.
 * Configure SPK interrupt accordingly or setup polling mode
 * Tap-detection shall be active if:
 *  - the service is enabled (tapdet_open), AND
 *  - the current profile is a tap-detection profile
 * On TFA1 familiy of devices, activating tap-detection means enabling the SPK
 * interrupt if available.
 * We also update the tapdet_enabled and tapdet_poll variables.
 */
static void tfa98xx_tapdet_check_update(struct tfa98xx *tfa98xx)
{
	unsigned int enable = false;

	/* Support tap-detection on TFA1 family of devices */
	if ((tfa98xx->flags & TFA98XX_FLAG_TAPDET_AVAILABLE) == 0)
		return;

	if (tfa98xx->tapdet_open &&
		(tfa98xx->tapdet_profiles & (1 << tfa98xx->profile)))
		enable = true;

	if (!gpio_is_valid(tfa98xx->irq_gpio)) {
		/* interrupt not available, setup polling mode */
		tfa98xx->tapdet_poll = true;
		if (enable)
			queue_delayed_work(tfa98xx->tfa98xx_wq,
						&tfa98xx->tapdet_work, HZ/10);
		else
			cancel_delayed_work_sync(&tfa98xx->tapdet_work);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		dev_dbg(tfa98xx->codec->dev,
#else
		dev_dbg(tfa98xx->component->dev,
#endif
			"Polling for tap-detection: %s (%d; 0x%x, %d)\n",
			enable? "enabled":"disabled",
			tfa98xx->tapdet_open, tfa98xx->tapdet_profiles,
			tfa98xx->profile);

	} else {
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		dev_dbg(tfa98xx->codec->dev,
	#else
		dev_dbg(tfa98xx->component->dev,
	#endif
			"Interrupt for tap-detection: %s (%d; 0x%x, %d)\n",
				enable? "enabled":"disabled",
				tfa98xx->tapdet_open, tfa98xx->tapdet_profiles,
				tfa98xx->profile);
		/*  enabled interrupt */
		tfa_irq_ena(tfa98xx->tfa, tfa9912_irq_sttapdet , enable);
	}

	/* check disabled => enabled transition to clear pending events */
	if (!tfa98xx->tapdet_enabled && enable) {
		/* clear pending event if any */
		tfa_irq_clear(tfa98xx->tfa, tfa9912_irq_sttapdet);
	}

	if (!tfa98xx->tapdet_poll)
		tfa_irq_ena(tfa98xx->tfa, tfa9912_irq_sttapdet, 1); /* enable again */
}

/* global enable / disable interrupts */
static void tfa98xx_interrupt_enable(struct tfa98xx *tfa98xx, bool enable)
{
	if (tfa98xx->flags & TFA98XX_FLAG_SKIP_INTERRUPTS)
		return;

	if (tfa98xx->tfa->tfa_family == 2)
		tfa98xx_interrupt_enable_tfa2(tfa98xx, enable);
}

/* Firmware management */
static void tfa98xx_container_loaded(const struct firmware *cont, void *context)
{
	nxpTfaContainer_t *container;
	struct tfa98xx *tfa98xx = context;
	enum tfa_error tfa_err;
	int container_size;
	int ret;

	tfa98xx->dsp_fw_state = TFA98XX_DSP_FW_FAIL;

	if (!cont) {
		#ifdef OPLUS_BUG_COMPATIBILITY
		if (fwload_cnt < FW_LOADING_RETRY_TIMES) {
			pr_err("%s: Failed to read %s, try again: %d\n", __func__, fw_name, fwload_cnt);
			request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
									fw_name, tfa98xx->dev, GFP_KERNEL,
									tfa98xx, tfa98xx_container_loaded);
			fwload_cnt++;
			return;
		} else {
			if(fwload_comm_cnt < FW_LOADING_RETRY_TIMES) {
				pr_err("%s: read comm %s, try: %d\n", __func__, fw_comm_name, fwload_comm_cnt);
				request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
										fw_comm_name, tfa98xx->dev, GFP_KERNEL,
										tfa98xx, tfa98xx_container_loaded);
				fwload_comm_cnt++;
			} else {
				pr_err("%s: Failed to read comm firmware after 5 times, exit\n", __func__);
			}
		}
		#else /* OPLUS_BUG_COMPATIBILITY */
		pr_err("Failed to read %s\n", fw_name);
		#endif /* OPLUS_BUG_COMPATIBILITY */

		return;
	}

	pr_info("loaded %s - size: %zu\n", fw_name, cont->size);

	mutex_lock(&tfa98xx_mutex);
	if (tfa98xx_container == NULL) {
		container = kzalloc(cont->size, GFP_KERNEL);
		if (container == NULL) {
			mutex_unlock(&tfa98xx_mutex);
			release_firmware(cont);
			pr_err("Error allocating memory\n");
			return;
		}

		container_size = cont->size;
		memcpy(container, cont->data, container_size);
		release_firmware(cont);

		pr_info("%.2s%.2s\n", container->version, container->subversion);
		pr_info("%.8s\n", container->customer);
		pr_info("%.8s\n", container->application);
		pr_info("%.8s\n", container->type);
		pr_info("%d ndev\n", container->ndev);
		pr_info("%d nprof\n", container->nprof);

		tfa_err = tfa_load_cnt(container, container_size);
		if (tfa_err != tfa_error_ok) {
			mutex_unlock(&tfa98xx_mutex);
			kfree(container);
			dev_err(tfa98xx->dev, "Cannot load container file, aborting\n");
			return;
		}

		tfa98xx_container = container;
	} else {
		pr_info("container file already loaded...\n");
		container = tfa98xx_container;
		release_firmware(cont);
	}
	mutex_unlock(&tfa98xx_mutex);

	tfa98xx->tfa->cnt = container;

	/*
		i2c transaction limited to 64k
		(Documentation/i2c/writing-clients)
	*/
	tfa98xx->tfa->buffer_size = 65536;

	/* DSP messages via i2c */
	tfa98xx->tfa->has_msg = 0;

	if (tfa_dev_probe(tfa98xx->i2c->addr, tfa98xx->tfa) != 0) {
		dev_err(tfa98xx->dev, "Failed to probe TFA98xx @ 0x%.2x\n", tfa98xx->i2c->addr);
		return;
	}

	tfa98xx->tfa->dev_idx = tfa_cont_get_idx(tfa98xx->tfa);
	if (tfa98xx->tfa->dev_idx < 0) {
		dev_err(tfa98xx->dev, "Failed to find TFA98xx @ 0x%.2x in container file\n", tfa98xx->i2c->addr);
		return;
	}

	/* Enable debug traces */
	tfa98xx->tfa->verbose = trace_level & 1;

	/* prefix is the application name from the cnt */
	tfa_cnt_get_app_name(tfa98xx->tfa, tfa98xx->fw.name);

	/* set default profile/vstep */
	tfa98xx->profile = 0;
	tfa98xx->vstep = 0;

	/* Override default profile if requested */
	if (strcmp(dflt_prof_name, "")) {
		unsigned int i;
		int nprof = tfa_cnt_get_dev_nprof(tfa98xx->tfa);
		for (i = 0; i < nprof; i++) {
			if (strcmp(tfa_cont_profile_name(tfa98xx, i),
							dflt_prof_name) == 0) {
				tfa98xx->profile = i;
				dev_info(tfa98xx->dev,
					"changing default profile to %s (%d)\n",
					dflt_prof_name, tfa98xx->profile);
				break;
			}
		}
		if (i >= nprof)
			dev_info(tfa98xx->dev,
				"Default profile override failed (%s profile not found)\n",
				dflt_prof_name);
	}

	tfa98xx->dsp_fw_state = TFA98XX_DSP_FW_OK;
	pr_info("Firmware init complete\n");

	if (no_start != 0)
		return;

 	/* Only controls for master device */
	if (tfa98xx->tfa->dev_idx == 0)
		tfa98xx_create_controls(tfa98xx);

#ifdef OPLUS_BUG_COMPATIBILITY
    if (tfa98xx->tfa->dev_idx == 0) {
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
        oppo_create_hardware_resource_controls(tfa98xx->codec);
	#else
        oppo_create_hardware_resource_controls(tfa98xx->component);
	#endif
	}
#endif

	tfa98xx_inputdev_check_register(tfa98xx);

	if (tfa_is_cold(tfa98xx->tfa) == 0) {
		pr_info("Warning: device 0x%.2x is still warm\n", tfa98xx->i2c->addr);
		tfa_reset(tfa98xx->tfa);
	}

	#ifdef OPLUS_BUG_COMPATIBILITY
	if (tfa98xx->flags & TFA98XX_FLAG_TDM_DEVICE) {
		return;
	}
	#endif /* OPLUS_BUG_COMPATIBILITY */

	/* Preload settings using internal clock on TFA2 */
	if (tfa98xx->tfa->tfa_family == 2) {
		mutex_lock(&tfa98xx->dsp_lock);
		ret = tfa98xx_tfa_start(tfa98xx, tfa98xx->profile, tfa98xx->vstep);
		if (ret == Tfa98xx_Error_Not_Supported)
			tfa98xx->dsp_fw_state = TFA98XX_DSP_FW_FAIL;
		mutex_unlock(&tfa98xx->dsp_lock);
	}

	tfa98xx_interrupt_enable(tfa98xx, true);
}

static int tfa98xx_load_container(struct tfa98xx *tfa98xx)
{
#ifdef OPLUS_BUG_COMPATIBILITY
	unsigned int reg;
	int ret;
	int retries = I2C_RETRIES;

retry:
	ret = regmap_read(tfa98xx->regmap, 0x03, &reg);
	reg = reg & 0xff;
	if ((ret < 0) || !is_tfa98xx_series_v6(reg)) {
		pr_err("%s i2c error at retries left: %d, rev:%x\n", __func__, retries, reg);
		if (retries) {
			retries--;
			msleep(I2C_RETRY_DELAY);
			goto retry;
		}
	}

	if ((reg == 0x94) || (reg == 0x74)) {/* tfa9894 */

		sprintf(fw_name,"tfa/tfa/tfa9894/tfa98xx.cnt");
		sprintf(fw_comm_name,"tfa/common/tfa98xx.cnt");
	}

	pr_info("%s: ID revision 0x%04x, fw_name is %s, fw_comm_name is %s\n", __func__, reg, fw_name, fw_comm_name);
#endif /* OPLUS_BUG_COMPATIBILITY */

	tfa98xx->dsp_fw_state = TFA98XX_DSP_FW_PENDING;

	return request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
	                               fw_name, tfa98xx->dev, GFP_KERNEL,
	                               tfa98xx, tfa98xx_container_loaded);
}


static void tfa98xx_tapdet(struct tfa98xx *tfa98xx)
{
	unsigned int tap_pattern;
	int btn;

	/* check tap pattern (BTN_0 is "error" wrong tap indication */
	tap_pattern = tfa_get_tap_pattern(tfa98xx->tfa);
	switch (tap_pattern) {
	case 0xffffffff:
		pr_info("More than 4 taps detected! (flagTapPattern = -1)\n");
		btn = BTN_0;
		break;
	case 0xfffffffe:
	case 0xfe:
		pr_info("Illegal tap detected!\n");
		btn = BTN_0;
		break;
	case 0:
		pr_info("Unrecognized pattern! (flagTapPattern = 0)\n");
		btn = BTN_0;
		break;
	default:
		pr_info("Detected pattern: %d\n", tap_pattern);
		btn = BTN_0 + tap_pattern;
		break;
	}

	input_report_key(tfa98xx->input, btn, 1);
	input_report_key(tfa98xx->input, btn, 0);
	input_sync(tfa98xx->input);

	/* acknowledge event done by clearing interrupt */

}

static void tfa98xx_tapdet_work(struct work_struct *work)
{
	struct tfa98xx *tfa98xx;

	//TODO check is this is still needed for tap polling
	tfa98xx = container_of(work, struct tfa98xx, tapdet_work.work);

	if ( tfa_irq_get(tfa98xx->tfa, tfa9912_irq_sttapdet))
		tfa98xx_tapdet(tfa98xx);

	queue_delayed_work(tfa98xx->tfa98xx_wq, &tfa98xx->tapdet_work, HZ/10);
}

static void tfa98xx_monitor(struct work_struct *work)
{
	struct tfa98xx *tfa98xx;
	enum Tfa98xx_Error error = Tfa98xx_Error_Ok;

	tfa98xx = container_of(work, struct tfa98xx, monitor_work.work);

	/* Check for tap-detection - bypass monitor if it is active */
	if (!tfa98xx->input) {
		mutex_lock(&tfa98xx->dsp_lock);
		error = tfa_status(tfa98xx->tfa);
		mutex_unlock(&tfa98xx->dsp_lock);
		if (error == Tfa98xx_Error_DSP_not_running) {
			if (tfa98xx->dsp_init == TFA98XX_DSP_INIT_DONE) {
				tfa98xx->dsp_init = TFA98XX_DSP_INIT_RECOVER;
				queue_delayed_work(tfa98xx->tfa98xx_wq, &tfa98xx->init_work, 0);
			}
		}
	}

	/* reschedule */
	queue_delayed_work(tfa98xx->tfa98xx_wq, &tfa98xx->monitor_work, 5*HZ);
}

static void tfa98xx_dsp_init(struct tfa98xx *tfa98xx)
{
	int ret;
	bool failed = false;
	bool reschedule = false;
	bool sync= false;
	#ifdef OPLUS_BUG_COMPATIBILITY
	int value = 0;
	#endif /* OPLUS_BUG_COMPATIBILITY */

	if (tfa98xx->dsp_fw_state != TFA98XX_DSP_FW_OK) {
		pr_debug("Skipping tfa_dev_start (no FW: %d)\n", tfa98xx->dsp_fw_state);
		return;
	}

	if(tfa98xx->dsp_init == TFA98XX_DSP_INIT_DONE) {
		pr_debug("Stream already started, skipping DSP power-on\n");
		return;
	}

	dev_info(&tfa98xx->i2c->dev, "tfa98xx_dsp_init enter\n");

	mutex_lock(&tfa98xx->dsp_lock);

	tfa98xx->dsp_init = TFA98XX_DSP_INIT_PENDING;

	#ifdef OPLUS_BUG_COMPATIBILITY
	if (!tfa98xx->tfa->is_probus_device) {
		/*This is only for DSP TFA like TFA9894*/
		value = tfa_dev_mtp_get(tfa98xx->tfa, TFA_MTP_EX);
		if (!value) {
			tfa98xx->profile = tfaContGetCalProfile(tfa98xx->tfa);
			pr_info("%s force the change to calibration profile for calibrate\n", __func__);
		}
	} else {
		/*For non-DSP TFA, like TFA9874, we don't load the cal profile to do calibration */
		value = 1;
	}
	#endif /* OPLUS_BUG_COMPATIBILITY */

	if (tfa98xx->init_count < TF98XX_MAX_DSP_START_TRY_COUNT) {
		/* directly try to start DSP */
		ret = tfa98xx_tfa_start(tfa98xx, tfa98xx->profile, tfa98xx->vstep);
		if (ret == Tfa98xx_Error_Not_Supported) {
			tfa98xx->dsp_fw_state = TFA98XX_DSP_FW_FAIL;
			dev_err(&tfa98xx->i2c->dev, "Failed starting device\n");
			failed = true;
		} else if (ret != Tfa98xx_Error_Ok) {
			/* It may fail as we may not have a valid clock at that
			 * time, so re-schedule and re-try later.
			 */
			dev_err(&tfa98xx->i2c->dev,
					"tfa_dev_start failed! (err %d) - %d\n",
					ret, tfa98xx->init_count);
			reschedule = true;
		} else {
			sync = true;

			/* Subsystem ready, tfa init complete */
			tfa98xx->dsp_init = TFA98XX_DSP_INIT_DONE;
			dev_info(&tfa98xx->i2c->dev,
						"tfa_dev_start success (%d)\n",
						tfa98xx->init_count);
			/* cancel other pending init works */
			cancel_delayed_work(&tfa98xx->init_work);
			tfa98xx->init_count = 0;
		}
		if (tfa98xx_dual_spk) {
			if (tfa98xx->i2c->addr == CHIP_LEFT_ADDR) {
				tfa_spk_1_start_done = true;
			} else if (tfa98xx->i2c->addr == CHIP_RIGHT_ADDR) {
				tfa_spk_2_start_done = true;
			}
		}
	} else {
		/* exceeded max number ot start tentatives, cancel start */
		dev_err(&tfa98xx->i2c->dev,
			"Failed starting device (%d)\n",
			tfa98xx->init_count);
			failed = true;
	}
	if (reschedule) {
		/* reschedule this init work for later */
		queue_delayed_work(tfa98xx->tfa98xx_wq,
						&tfa98xx->init_work,
						msecs_to_jiffies(5));
		tfa98xx->init_count++;
	}
	if (failed) {
		tfa98xx->dsp_init = TFA98XX_DSP_INIT_FAIL;
		/* cancel other pending init works */
		cancel_delayed_work(&tfa98xx->init_work);
		tfa98xx->init_count = 0;
	}
	mutex_unlock(&tfa98xx->dsp_lock);

	if (sync) {
		/* check if all devices have started */
		bool do_sync;
		mutex_lock(&tfa98xx_mutex);

		if (tfa98xx_sync_count < tfa98xx_device_count)
			tfa98xx_sync_count++;

		do_sync = (tfa98xx_sync_count >= tfa98xx_device_count);
		mutex_unlock(&tfa98xx_mutex);

		/* when all devices have started then unmute */
		if (do_sync) {
			tfa98xx_sync_count = 0;
			list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
				mutex_lock(&tfa98xx->dsp_lock);
				#ifndef OPLUS_BUG_COMPATIBILITY
				tfa_dev_set_state(tfa98xx->tfa, TFA_STATE_UNMUTE);
				#else
				if (tfa98xx_dual_spk) {
					if (((tfa98xx->i2c->addr == CHIP_LEFT_ADDR) && (!g_speaker_resistance_fail_1))
						||  ((tfa98xx->i2c->addr == CHIP_RIGHT_ADDR) && (!g_speaker_resistance_fail_2))) {
						tfa_dev_set_state(tfa98xx->tfa, TFA_STATE_UNMUTE);
					} else {
						pr_err("tfa addr = 0x%x, set mute state for resistance out of range!\n",
							tfa98xx->i2c->addr);
						tfa_dev_set_state(tfa98xx->tfa, TFA_STATE_MUTE);
					}
				} else {
					if (!g_speaker_resistance_fail) {
						tfa_dev_set_state(tfa98xx->tfa, TFA_STATE_UNMUTE);
						if (g_tfa98xx_ana_vol < 16) {
							ret = tfa98xx_set_ana_volume(tfa98xx->tfa, g_tfa98xx_ana_vol);
							if (ret) {
								pr_err("%s: set ana volume error, code:%d\n", __func__, ret);
							} else {
								pr_info("%s: set ana volume ok\n", __func__);
							}
							g_tfa98xx_ana_vol = 16;
						}
					} else {
						pr_err("set mute state for resistance out of range!\n");
						tfa_dev_set_state(tfa98xx->tfa, TFA_STATE_MUTE);
					}
				}
				#endif /* OPLUS_BUG_COMPATIBILITY */

				/*
				 * start monitor thread to check IC status bit
				 * periodically, and re-init IC to recover if
				 * needed.
				 */
				if (tfa98xx->tfa->tfa_family == 1)
					queue_delayed_work(tfa98xx->tfa98xx_wq,
					                &tfa98xx->monitor_work,
					                1*HZ);
				mutex_unlock(&tfa98xx->dsp_lock);
			}

		}
	}


	return;
}


static void tfa98xx_dsp_init_work(struct work_struct *work)
{
	struct tfa98xx *tfa98xx = container_of(work, struct tfa98xx, init_work.work);

	tfa98xx_dsp_init(tfa98xx);
}

static void tfa98xx_interrupt(struct work_struct *work)
{
	struct tfa98xx *tfa98xx = container_of(work, struct tfa98xx, interrupt_work.work);

	pr_info("\n");

	if (tfa98xx->flags & TFA98XX_FLAG_TAPDET_AVAILABLE) {
		/* check for tap interrupt */
		if (tfa_irq_get(tfa98xx->tfa, tfa9912_irq_sttapdet)) {
			tfa98xx_tapdet(tfa98xx);

			/* clear interrupt */
			tfa_irq_clear(tfa98xx->tfa, tfa9912_irq_sttapdet);
		}
	} /* TFA98XX_FLAG_TAPDET_AVAILABLE */

	if (tfa98xx->flags & TFA98XX_FLAG_REMOVE_PLOP_NOISE) {
		int start_triggered;

		mutex_lock(&tfa98xx->dsp_lock);
		start_triggered = tfa_plop_noise_interrupt(tfa98xx->tfa, tfa98xx->profile, tfa98xx->vstep);
		/* Only enable when the return value is 1, otherwise the interrupt is triggered twice */
		if(start_triggered)
			tfa98xx_interrupt_enable(tfa98xx, true);
		mutex_unlock(&tfa98xx->dsp_lock);
	} /* TFA98XX_FLAG_REMOVE_PLOP_NOISE */

	if (tfa98xx->flags & TFA98XX_FLAG_LP_MODES) {
		tfa_lp_mode_interrupt(tfa98xx->tfa);
	} /* TFA98XX_FLAG_LP_MODES */

	/* unmask interrupts masked in IRQ handler */
	 tfa_irq_unmask(tfa98xx->tfa);
}

static int tfa98xx_startup(struct snd_pcm_substream *substream,
						struct snd_soc_dai *dai)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct snd_soc_codec *codec = dai->codec;
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
#else
	struct snd_soc_component *component = dai->component;
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
#endif

	unsigned int sr;
	int len, prof, nprof, idx = 0;
	char *basename;
	#ifdef OPLUS_ARCH_EXTENDS
	char *prof_name = NULL;
	#endif
	u64 formats;
	int err;

	/*
	 * Support CODEC to CODEC links,
	 * these are called with a NULL runtime pointer.
	 */
	if (!substream->runtime)
		return 0;

	if (pcm_no_constraint != 0)
		return 0;

	switch (pcm_sample_format) {
	case 1:
		formats = SNDRV_PCM_FMTBIT_S24_LE;
		break;
	case 2:
		formats = SNDRV_PCM_FMTBIT_S32_LE;
		break;
	default:
		formats = SNDRV_PCM_FMTBIT_S16_LE;
		break;
	}

	err = snd_pcm_hw_constraint_mask64(substream->runtime,
					SNDRV_PCM_HW_PARAM_FORMAT, formats);
	if (err < 0)
		return err;

	if (no_start != 0)
		return 0;

	if (tfa98xx->dsp_fw_state != TFA98XX_DSP_FW_OK) {
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		dev_info(codec->dev, "Container file not loaded\n");
	#else
	    dev_info(component->dev, "Container file not loaded\n");
	#endif
		return -EINVAL;
	}

	basename = kzalloc(MAX_CONTROL_NAME, GFP_KERNEL);
	if (!basename)
		return -ENOMEM;

	/* copy profile name into basename until the . */
	#ifdef OPLUS_ARCH_EXTENDS
	if ((prof_name = tfa_cont_profile_name(tfa98xx, tfa98xx->profile)) != NULL) {
		get_profile_basename(basename, prof_name);
	}
	#else
	get_profile_basename(basename, tfa_cont_profile_name(tfa98xx, tfa98xx->profile));
	#endif

	len = strlen(basename);

	/* loop over all profiles and get the supported samples rate(s) from
	 * the profiles with the same basename
	 */
	nprof = tfa_cnt_get_dev_nprof(tfa98xx->tfa);
	tfa98xx->rate_constraint.list = &tfa98xx->rate_constraint_list[0];
	tfa98xx->rate_constraint.count = 0;
	for (prof = 0; prof < nprof; prof++) {
		if (0 == strncmp(basename, tfa_cont_profile_name(tfa98xx, prof), len)) {
			/* Check which sample rate is supported with current profile,
			 * and enforce this.
			 */
			sr = tfa98xx_get_profile_sr(tfa98xx->tfa, prof);
			if (!sr)
			#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
				dev_info(codec->dev, "Unable to identify supported sample rate\n");
			#else
			    dev_info(component->dev, "Unable to identify supported sample rate\n");
			#endif

			if (tfa98xx->rate_constraint.count >= TFA98XX_NUM_RATES) {
			#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
				dev_err(codec->dev, "too many sample rates\n");
			#else
				dev_err(component->dev, "too many sample rates\n");
			#endif
			} else {
				tfa98xx->rate_constraint_list[idx++] = sr;
				tfa98xx->rate_constraint.count += 1;
			}
		}
	}

	kfree(basename);

	return snd_pcm_hw_constraint_list(substream->runtime, 0,
				   SNDRV_PCM_HW_PARAM_RATE,
				   &tfa98xx->rate_constraint);
}

static int tfa98xx_set_dai_sysclk(struct snd_soc_dai *codec_dai,
				  int clk_id, unsigned int freq, int dir)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec_dai->codec);
#else
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(codec_dai->component);
#endif

	tfa98xx->sysclk = freq;
	return 0;
}

static int tfa98xx_set_tdm_slot(struct snd_soc_dai *dai, unsigned int tx_mask,
			unsigned int rx_mask, int slots, int slot_width)
{
	pr_debug("\n");
	return 0;
}

static int tfa98xx_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(dai->codec);
	struct snd_soc_codec *codec = dai->codec;;
    #else
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(dai->component);
	struct snd_soc_component *component = dai->component;
    #endif

	pr_info("fmt=0x%x\n", fmt);

	/* Supported mode: regular I2S, slave, or PDM */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBS_CFS) {
		#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
			dev_err(codec->dev, "Invalid Codec master mode\n");
		#else
			dev_err(component->dev, "Invalid Codec master mode\n");
		#endif
			return -EINVAL;
		}
		break;
	case SND_SOC_DAIFMT_PDM:
		break;
	default:
		#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		dev_err(codec->dev, "Unsupported DAI format %d\n",
		#else
		dev_err(component->dev, "Unsupported DAI format %d\n",
		#endif
					fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	tfa98xx->audio_mode = fmt & SND_SOC_DAIFMT_FORMAT_MASK;

	return 0;
}

static int tfa98xx_get_fssel(unsigned int rate)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(rate_to_fssel); i++) {
		if (rate_to_fssel[i].rate == rate) {
			return rate_to_fssel[i].fssel;
		}
	}
	return -EINVAL;
}

static int tfa98xx_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct snd_soc_codec *codec = dai->codec;
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
    #else
	struct snd_soc_component *component = dai->component;
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
    #endif
	unsigned int rate;
	int prof_idx;

	/* Supported */
	rate = params_rate(params);
	pr_info("Requested rate: %d, sample size: %d, physical size: %d\n",
			rate, snd_pcm_format_width(params_format(params)),
			snd_pcm_format_physical_width(params_format(params)));

	if (no_start != 0)
		return 0;

	/* check if samplerate is supported for this mixer profile */
	prof_idx = get_profile_id_for_sr(tfa98xx_mixer_profile, rate);
	if (prof_idx < 0) {
		pr_err("tfa98xx: invalid sample rate %d.\n", rate);
		return -EINVAL;
	}
	pr_info("mixer profile:container profile = [%d:%d]\n", tfa98xx_mixer_profile, prof_idx);


	/* update 'real' profile (container profile) */
	tfa98xx->profile = prof_idx;

	/* update to new rate */
	tfa98xx->rate = rate;

	return 0;
}

static uint8_t bytes[3*3+1] = {0};

enum Tfa98xx_Error tfa98xx_adsp_send_calib_values(void)
{
	struct tfa98xx *tfa98xx;
	int ret = 0;
	int value = 0, nr, dsp_cal_value = 0;

	/* if the calibration value was sent to host DSP, we clear flag only (stereo case). */
	if ((tfa98xx_device_count > 1) && (tfa98xx_device_count == bytes[0])) {
		pr_info("The calibration value was sent to host DSP.\n");
		bytes[0] = 0;
		return Tfa98xx_Error_Ok;
	}

	nr = 4;
	/* read calibrated impendance from all devices. */
	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		struct tfa_device *tfa = tfa98xx->tfa;
		if (TFA_GET_BF(tfa, MTPEX) == 1) {
			value = tfa_dev_mtp_get(tfa, TFA_MTP_RE25);
			dsp_cal_value = (value * 65536) / 1000;
			pr_info("Device 0x%x cal value is 0x%d\n", tfa98xx->i2c->addr, dsp_cal_value);

			bytes[nr++] = (uint8_t)((dsp_cal_value >> 16) & 0xff);
			bytes[nr++] = (uint8_t)((dsp_cal_value >> 8) & 0xff);
			bytes[nr++] = (uint8_t)(dsp_cal_value & 0xff);
			bytes[0] += 1;
		}
	}

	/*for mono case, we will copy primary channel data to secondary channel. */
	if (1 == tfa98xx_device_count) {
		memcpy(&bytes[7], &bytes[4], sizeof(char)*3);
	}
	pr_info("tfa98xx_device_count=%d  bytes[0]=%d\n", tfa98xx_device_count, bytes[0]);

	/* we will send it to host DSP algorithm once calibraion value loaded from all device. */
	if (tfa98xx_device_count == bytes[0]) {
		bytes[1] = 0x00;
		bytes[2] = 0x81;
		bytes[3] = 0x05;

		pr_info("calibration value send to host DSP.\n");
		ret = tfa98xx_send_data_to_dsp(&bytes[1], sizeof(bytes) - 1);
		msleep(10);

		/* for mono case, we should clear flag here. */
		if (1 == tfa98xx_device_count)
			bytes[0] = 0;

	} else {
		pr_err("load calibration data from device failed.\n");
		ret = Tfa98xx_Error_Bad_Parameter;
	}

	return ret;
}

static int tfa98xx_mute(struct snd_soc_dai *dai, int mute, int stream)
{
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	struct snd_soc_codec *codec = dai->codec;
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
    #else
	struct snd_soc_component *component = dai->component;
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
    #endif

	dev_info(&tfa98xx->i2c->dev, "%s: state: %d\n", __func__, mute);

	if (no_start) {
		pr_info("no_start parameter set no tfa_dev_start or tfa_dev_stop, returning\n");
		return 0;
	}

	if (mute) {
		/* stop DSP only when both playback and capture streams
		 * are deactivated
		 */
		if (stream == SNDRV_PCM_STREAM_PLAYBACK)
			tfa98xx->pstream = 0;
		else
			tfa98xx->cstream = 0;
		if (tfa98xx->pstream != 0 || tfa98xx->cstream != 0)
			return 0;

		mutex_lock(&tfa98xx_mutex);
		tfa98xx_sync_count = 0;
		mutex_unlock(&tfa98xx_mutex);

		cancel_delayed_work_sync(&tfa98xx->monitor_work);

		cancel_delayed_work_sync(&tfa98xx->init_work);
		if (tfa98xx->dsp_fw_state != TFA98XX_DSP_FW_OK)
			return 0;
		mutex_lock(&tfa98xx->dsp_lock);
		tfa_dev_stop(tfa98xx->tfa);
		tfa98xx->dsp_init = TFA98XX_DSP_INIT_STOPPED;
		mutex_unlock(&tfa98xx->dsp_lock);
	} else {
		if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
			tfa98xx->pstream = 1;
			if (tfa98xx->tfa->is_probus_device) {
				tfa98xx_adsp_send_calib_values();
			}
		}
		else
			tfa98xx->cstream = 1;

		if (tfa98xx_dual_spk) {
			pr_info("%s: addr = 0x%x, tfa_dual_start_cnt = %d\n", __func__,
				tfa98xx->i2c->addr, tfa_dual_start_cnt);
			if (tfa_dual_start_cnt < 2) {
				tfa_dual_start_cnt++;
			}
		}

		/* Start DSP */
		#ifndef OPLUS_BUG_COMPATIBILITY
		if (tfa98xx->dsp_init != TFA98XX_DSP_INIT_PENDING)
			queue_delayed_work(tfa98xx->tfa98xx_wq,
			                   &tfa98xx->init_work, 0);
		#else /* OPLUS_BUG_COMPATIBILITY */
		if (tfa98xx_dual_spk) {
			if (tfa98xx->dsp_init != TFA98XX_DSP_INIT_PENDING) {
				pr_info("%s: addr = 0x%x, tfa98xx->flags = 0x%x\n", __func__, tfa98xx->i2c->addr, tfa98xx->flags);
				if (tfa98xx->flags & TFA98XX_FLAG_CHIP_SELECTED) {
					queue_delayed_work(tfa98xx->tfa98xx_wq,
									   &tfa98xx->init_work, 0);
				}
			}
			if (tfa_dual_start_cnt == 2) {
				tfa_wait_start_cnt = 10;
				while (tfa_wait_start_cnt > 0) {
					tfa_wait_start_cnt--;
					mutex_lock(&tfa98xx->dsp_lock);
					if ((tfa_spk_1_start_done == true) && (tfa_spk_2_start_done == true)) {
						mutex_unlock(&tfa98xx->dsp_lock);
						break;
					}
					mutex_unlock(&tfa98xx->dsp_lock);
					msleep(2);
				}
				tfa_spk_1_start_done = false;
				tfa_spk_2_start_done = false;
				pr_info("%s: tfa wait 2 spk start done\n", __func__);
			}
		} else {
			if (tfa98xx->dsp_init != TFA98XX_DSP_INIT_PENDING)
				tfa98xx_dsp_init(tfa98xx);
		}
		#endif /* OPLUS_BUG_COMPATIBILITY */
	}

	return 0;
}

static const struct snd_soc_dai_ops tfa98xx_dai_ops = {
	.startup = tfa98xx_startup,
	.set_fmt = tfa98xx_set_fmt,
	.set_sysclk = tfa98xx_set_dai_sysclk,
	.set_tdm_slot = tfa98xx_set_tdm_slot,
	.hw_params = tfa98xx_hw_params,
	.mute_stream = tfa98xx_mute,
};

static struct snd_soc_dai_driver tfa98xx_dai[] = {
	{
		.name = "tfa98xx-aif",
		.id = 1,
		.playback = {
			.stream_name = "AIF Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates = TFA98XX_RATES,
			.formats = TFA98XX_FORMATS,
		},
		.capture = {
			 .stream_name = "AIF Capture",
			 .channels_min = 1,
			 .channels_max = 4,
			 .rates = TFA98XX_RATES,
			 .formats = TFA98XX_FORMATS,
		 },
		.ops = &tfa98xx_dai_ops,
#ifndef OPLUS_BUG_COMPATIBILITY
		.symmetric_rates = 1,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
		.symmetric_channels = 1,
		.symmetric_samplebits = 1,
#endif
#endif /* OPLUS_BUG_COMPATIBILITY */
	},
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
static int tfa98xx_probe(struct snd_soc_codec *codec)
{
   struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
#else
static int tfa98xx_probe(struct snd_soc_component *component)
{
   struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
#endif
	int ret = 0;

	pr_info("%s\n", __func__);

	snd_soc_component_init_regmap(component,tfa98xx->regmap);
#ifdef OPLUS_BUG_COMPATIBILITY
	g_tfa98xx_firmware_status = 0;
#endif
	/* setup work queue, will be used to initial DSP on first boot up */
	tfa98xx->tfa98xx_wq = create_singlethread_workqueue("tfa98xx");
	if (!tfa98xx->tfa98xx_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&tfa98xx->init_work, tfa98xx_dsp_init_work);
	INIT_DELAYED_WORK(&tfa98xx->monitor_work, tfa98xx_monitor);
	INIT_DELAYED_WORK(&tfa98xx->interrupt_work, tfa98xx_interrupt);
	INIT_DELAYED_WORK(&tfa98xx->tapdet_work, tfa98xx_tapdet_work);

	#ifdef OPLUS_BUG_COMPATIBILITY
	INIT_DELAYED_WORK(&tfa98xx->volume_work, tfa98xx_set_volume_work);
	#endif /* OPLUS_BUG_COMPATIBILITY */

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	tfa98xx->codec = codec;
#else
 	tfa98xx->component = component;
#endif

	#ifdef OPLUS_BUG_COMPATIBILITY
	//ret = tfa98xx_load_container(tfa98xx);
	ret = tfa98xx_create_controls(tfa98xx);
	pr_info("Container loading requested: %d\n", ret);
	#endif /* OPLUS_BUG_COMPATIBILITY */

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
	codec->control_data = tfa98xx->regmap;
	ret = snd_soc_codec_set_cache_io(codec, 8, 16, SND_SOC_REGMAP);
	if (ret != 0) {
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
		dev_err(codec->dev, "Failed to set cache I/O: %d\n", ret);
	#else
	    dev_err(component->dev, "Failed to set cache I/O: %d\n", ret);
	#endif
		return ret;
	}
#endif
	tfa98xx_add_widgets(tfa98xx);

	#ifdef OPLUS_BUG_COMPATIBILITY
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	snd_soc_add_codec_controls(tfa98xx->codec,
	#else
	snd_soc_add_component_controls(tfa98xx->component,
	#endif
		tfa98xx_snd_controls, ARRAY_SIZE(tfa98xx_snd_controls));
	#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	snd_soc_add_codec_controls(tfa98xx->codec,
	#else
	snd_soc_add_component_controls(tfa98xx->component,
	#endif
		ftm_spk_rev_controls, ARRAY_SIZE(ftm_spk_rev_controls));
	#endif /* OPLUS_BUG_COMPATIBILITY */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	dev_info(codec->dev, "tfa98xx codec registered (%s) ret=%d",
#else
	dev_info(component->dev, "tfa98xx codec registered (%s) ret=%d",
#endif
							tfa98xx->fw.name, ret);

	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
static int tfa98xx_remove(struct snd_soc_codec *codec)
{
	struct tfa98xx *tfa98xx = snd_soc_codec_get_drvdata(codec);
#else
static void tfa98xx_remove(struct snd_soc_component *component)
{
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
#endif

	pr_debug("\n");

	tfa98xx_interrupt_enable(tfa98xx, false);

	tfa98xx_inputdev_unregister(tfa98xx);

	cancel_delayed_work_sync(&tfa98xx->interrupt_work);
	cancel_delayed_work_sync(&tfa98xx->monitor_work);
	cancel_delayed_work_sync(&tfa98xx->init_work);
	cancel_delayed_work_sync(&tfa98xx->tapdet_work);

	#ifdef OPLUS_BUG_COMPATIBILITY
	cancel_delayed_work_sync(&tfa98xx->volume_work);
	#endif /* OPLUS_BUG_COMPATIBILITY */

	if (tfa98xx->tfa98xx_wq)
		destroy_workqueue(tfa98xx->tfa98xx_wq);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	return 0;
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,16,0)&&LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
static struct regmap *tfa98xx_get_regmap(struct device *dev)
{
	struct tfa98xx *tfa98xx = dev_get_drvdata(dev);

	return tfa98xx->regmap;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
static struct snd_soc_codec_driver soc_codec_dev_tfa98xx = {
	.probe =	tfa98xx_probe,
	.remove =	tfa98xx_remove,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,16,0)
	.get_regmap = tfa98xx_get_regmap,
#endif
};
#else
static struct snd_soc_component_driver soc_component_dev_tfa98xx = {
	.probe =	tfa98xx_probe,
	.remove =	tfa98xx_remove,
};
#endif


static bool tfa98xx_writeable_register(struct device *dev, unsigned int reg)
{
	/* enable read access for all registers */
	return 1;
}

static bool tfa98xx_readable_register(struct device *dev, unsigned int reg)
{
	/* enable read access for all registers */
	return 1;
}

static bool tfa98xx_volatile_register(struct device *dev, unsigned int reg)
{
	/* enable read access for all registers */
	return 1;
}

static const struct regmap_config tfa98xx_regmap = {
	.reg_bits = 8,
	.val_bits = 16,

	.max_register = TFA98XX_MAX_REGISTER,
	.writeable_reg = tfa98xx_writeable_register,
	.readable_reg = tfa98xx_readable_register,
	.volatile_reg = tfa98xx_volatile_register,
	.cache_type = REGCACHE_NONE,
};


#ifndef OPLUS_BUG_COMPATIBILITY
static void tfa98xx_late_fw_load(struct work_struct *work)
{
   struct tfa98xx *tfa98xx = container_of(work, struct tfa98xx, interrupt_work.work);

    int ret = 0;

    pr_info("%s: \n", __func__);

	list_for_each_entry(tfa98xx, &tfa98xx_device_list, list) {
		//pr_info("%s: addr = 0x%x\n", __func__, tfa98xx->i2c->addr);
		ret = tfa98xx_load_container(tfa98xx);
		pr_info("Container loading requested: load_firmware %d retry_time:%d\n", load_firmware,fwload_cnt);
		if (!load_firmware && fwload_cnt< FW_LOAD_RETRIES){

		    queue_delayed_work(tfa98xx->tfa98xx_wq, &tfa98xx->load_fw_work,  msecs_to_jiffies(1*1000));

		}else
		cancel_delayed_work_sync(&tfa98xx->load_fw_work);
	}
}

static ssize_t tfa98xx_fw_store(struct file *file, const char __user *buf, size_t size, loff_t *lo)
{
	int val = 0;
	int rc;
	char k_buf[10];

    printk("%s:\n", __func__);

	if (size > 2)
		return -EINVAL;

	memset(k_buf, 0, 10);
	if( copy_from_user(k_buf, buf, size) ){
		printk("%s: read fw_update input error.\n", __func__);
		printk("buf is %s, k_buf is %s\n",buf, k_buf);
		return size;
	}
	printk("k_buf = %s\n", k_buf);
	rc = sscanf(k_buf, "%d", &val);
	printk("start update fw , k_buf = %s , rc = %d val = %d\n",k_buf, rc, val);

    //if(val);
        tfa98xx_late_fw_load();

	return size;
}

static const struct file_operations proc_firmware_update =
{
	.write = tfa98xx_fw_store,
	.open = simple_open,
	.owner = THIS_MODULE,
};
#endif /* OPLUS_BUG_COMPATIBILITY */
#if 0
static void tfa98xx_irq_tfa2(struct tfa98xx *tfa98xx)
{
	pr_info("\n");

	/*
	 * mask interrupts
	 * will be unmasked after handling interrupts in workqueue
	 */
	tfa_irq_mask(tfa98xx->tfa);
	queue_delayed_work(tfa98xx->tfa98xx_wq, &tfa98xx->interrupt_work, 0);
}


static irqreturn_t tfa98xx_irq(int irq, void *data)
{
	struct tfa98xx *tfa98xx = data;

	if (tfa98xx->tfa->tfa_family == 2)
		tfa98xx_irq_tfa2(tfa98xx);

	return IRQ_HANDLED;
}
#endif
static int tfa98xx_ext_reset(struct tfa98xx *tfa98xx)
{
#ifdef OPLUS_BUG_COMPATIBILITY
	int ret = 0;
	pr_info("tfa9890 rst addr = 0x%x\n", tfa98xx->i2c->addr);

	if (tfa98xx_dual_spk) {
	if (tfa98xx->i2c->addr == CHIP_RIGHT_ADDR) {
		if (IS_ERR(pinctrltfa)) {
			ret = PTR_ERR(pinctrltfa);
			pr_err("Cannot find pinctrlaud 2spk!\n");
			return ret;
		}
		if (IS_ERR(tfarsthigh)) {
			ret = PTR_ERR(tfarsthigh);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_high fail %d\n", __func__, ret);
			return ret;
		}
		if (IS_ERR(tfarstlow)) {
			ret = PTR_ERR(tfarstlow);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_low fail %d\n", __func__, ret);
			return ret;
		}

		ret = pinctrl_select_state(pinctrltfa, tfarsthigh);
		if (ret) {
			pr_err("%s: could not set tfa98xx rst pin high\n", __func__);
		}
		mdelay(5);
		ret = pinctrl_select_state(pinctrltfa, tfarstlow);
		if (ret) {
			pr_err("%s: could not set tfa98xx rst pin low\n", __func__);
		}
	} else if (tfa98xx->i2c->addr == CHIP_LEFT_ADDR) {
		if (IS_ERR(pinctrltfa_2)) {
			ret = PTR_ERR(pinctrltfa_2);
			pr_err("Cannot find pinctrlaud 2spk 2!\n");
			return ret;
		}

		if (IS_ERR(tfarsthigh_2)) {
			ret = PTR_ERR(tfarsthigh_2);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_high_2 fail %d\n", __func__, ret);
			return ret;
		}

		if (IS_ERR(tfarstlow_2)) {
			ret = PTR_ERR(tfarstlow_2);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_low_2 fail %d\n", __func__, ret);
			return ret;
		}

		ret = pinctrl_select_state(pinctrltfa_2, tfarsthigh_2);
		if (ret) {
			pr_err("%s: could not set tfa98xx_2 rst pin high\n", __func__);
		}
		mdelay(5);
		ret = pinctrl_select_state(pinctrltfa_2, tfarstlow_2);
		if (ret) {
			pr_err("%s: could not set tfa98xx_2 rst pin low\n", __func__);
		}
	}
	} else {
	if (IS_ERR(pinctrltfa)) {
		ret = PTR_ERR(pinctrltfa);
		pr_err("Cannot find pinctrlaud!\n");
		return ret;
	}
	if (IS_ERR(tfarsthigh)) {
		ret = PTR_ERR(tfarsthigh);
		pr_err("%s pinctrl_lookup_state tfa98xx_reset_high fail %d\n", __func__, ret);
		return ret;
	}
	if (IS_ERR(tfarstlow)) {
		ret = PTR_ERR(tfarstlow);
		pr_err("%s pinctrl_lookup_state tfa98xx_reset_low fail %d\n", __func__, ret);
		return ret;
	}

	ret = pinctrl_select_state(pinctrltfa, tfarsthigh);
	if (ret) {
		pr_err("%s: could not set tfa98xx rst pin high\n", __func__);
	}
	mdelay(5);
	ret = pinctrl_select_state(pinctrltfa, tfarstlow);
	if (ret) {
		pr_err("%s: could not set tfa98xx rst pin low\n", __func__);
	}
	}
	return ret;
#else
	if (tfa98xx && gpio_is_valid(tfa98xx->reset_gpio)) {
		gpio_set_value_cansleep(tfa98xx->reset_gpio, 1);
		mdelay(1);
		gpio_set_value_cansleep(tfa98xx->reset_gpio, 0);
		mdelay(1);
	}
	return 0;
#endif
}
#if 0
static int tfa98xx_parse_dt(struct device *dev, struct tfa98xx *tfa98xx,
		struct device_node *np) {
	tfa98xx->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);
	if (tfa98xx->reset_gpio < 0)
		dev_dbg(dev, "No reset GPIO provided, will not HW reset device\n");

	tfa98xx->irq_gpio =  of_get_named_gpio(np, "irq-gpio", 0);
	if (tfa98xx->irq_gpio < 0)
		dev_dbg(dev, "No IRQ GPIO provided.\n");

	return 0;
}
#endif

#ifdef OPLUS_BUG_COMPATIBILITY
static int tfa98xx_parse_dual_spk(struct device *dev, struct tfa98xx *tfa98xx)
{
	int ret = 0;
	u32 dual_spk = 0;
	char *tfa_dual_spk = "oppo,tfa98xx-dual-spk";

	ret = of_property_read_u32(dev->of_node, tfa_dual_spk,
				&dual_spk);
	if (ret) {
		dev_info(dev,
			"%s: missing %s in dt node\n", __func__, tfa_dual_spk);
		//tfa98xx_dual_spk = false;
	} else {
		dev_info(dev, "%s: dual_spk %d\n", __func__, dual_spk);
		if (dual_spk) {
			tfa98xx_dual_spk = true;
		} else {
			tfa98xx_dual_spk = false;
		}
	}

	return 0;
}
#endif /* OPLUS_BUG_COMPATIBILITY */

static ssize_t tfa98xx_reg_write(struct file *filp, struct kobject *kobj,
				struct bin_attribute *bin_attr,
				char *buf, loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct tfa98xx *tfa98xx = dev_get_drvdata(dev);

	if (count != 1) {
		pr_debug("invalid register address");
		return -EINVAL;
	}

	tfa98xx->reg = buf[0];

	return 1;
}

static ssize_t tfa98xx_rw_write(struct file *filp, struct kobject *kobj,
				struct bin_attribute *bin_attr,
				char *buf, loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct tfa98xx *tfa98xx = dev_get_drvdata(dev);
	u8 *data;
	int ret;
	int retries = I2C_RETRIES;

	data = kmalloc(count+1, GFP_KERNEL);
	if (data == NULL) {
		pr_info("can not allocate memory\n");
		return  -ENOMEM;
	}

	data[0] = tfa98xx->reg;
	memcpy(&data[1], buf, count);

retry:
	ret = i2c_master_send(tfa98xx->i2c, data, count+1);
	if (ret < 0) {
		pr_warn("i2c error, retries left: %d\n", retries);
		if (retries) {
			retries--;
			msleep(I2C_RETRY_DELAY);
			goto retry;
		}
	}

	kfree(data);

	/* the number of data bytes written without the register address */
	return ((ret > 1) ? count : -EIO);
}

static ssize_t tfa98xx_rw_read(struct file *filp, struct kobject *kobj,
				struct bin_attribute *bin_attr,
				char *buf, loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct tfa98xx *tfa98xx = dev_get_drvdata(dev);
	struct i2c_msg msgs[] = {
		{
			.addr = tfa98xx->i2c->addr,
			.flags = 0,
			.len = 1,
			.buf = &tfa98xx->reg,
		},
		{
			.addr = tfa98xx->i2c->addr,
			.flags = I2C_M_RD,
			.len = count,
			.buf = buf,
		},
	};
	int ret;
	int retries = I2C_RETRIES;
retry:
	ret = i2c_transfer(tfa98xx->i2c->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0) {
		pr_warn("i2c error, retries left: %d\n", retries);
		if (retries) {
			retries--;
			msleep(I2C_RETRY_DELAY);
			goto retry;
		}
		return ret;
	}
	/* ret contains the number of i2c transaction */
	/* return the number of bytes read */
	return ((ret > 1) ? count : -EIO);
}

static struct bin_attribute dev_attr_rw = {
	.attr = {
		.name = "rw",
		.mode = S_IRUSR | S_IWUSR,
	},
	.size = 0,
	.read = tfa98xx_rw_read,
	.write = tfa98xx_rw_write,
};

static struct bin_attribute dev_attr_reg = {
	.attr = {
		.name = "reg",
		.mode = S_IWUSR,
	},
	.size = 0,
	.read = NULL,
	.write = tfa98xx_reg_write,
};

static int tfa98xx_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	struct snd_soc_dai_driver *dai;
	struct tfa98xx *tfa98xx;
	//struct device_node *np = i2c->dev.of_node;
	//int irq_flags;
	unsigned int reg;
	int ret;
	#ifdef OPLUS_BUG_COMPATIBILITY
	int i = 0;
	#endif /* OPLUS_BUG_COMPATIBILITY */

	#ifdef OPLUS_BUG_COMPATIBILITY
	//struct proc_dir_entry *prEntry_temp = NULL;
	struct proc_dir_entry *prEntry_tfa9890 = NULL;
	prEntry_tfa9890 = proc_mkdir("tfa98xx", NULL);
	#endif /* OPLUS_BUG_COMPATIBILITY */

	pr_info("addr=0x%x\n", i2c->addr);

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C)) {
		dev_err(&i2c->dev, "check_functionality failed\n");
		return -EIO;
	}

	tfa98xx = devm_kzalloc(&i2c->dev, sizeof(struct tfa98xx), GFP_KERNEL);
	if (tfa98xx == NULL)
		return -ENOMEM;

	tfa98xx->dev = &i2c->dev;
	tfa98xx->i2c = i2c;
	tfa98xx->dsp_init = TFA98XX_DSP_INIT_STOPPED;
	tfa98xx->rate = 48000; /* init to the default sample rate (48kHz) */
	tfa98xx->tfa = NULL;

	tfa98xx->regmap = devm_regmap_init_i2c(i2c, &tfa98xx_regmap);
	if (IS_ERR(tfa98xx->regmap)) {
		ret = PTR_ERR(tfa98xx->regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	i2c_set_clientdata(i2c, tfa98xx);
	mutex_init(&tfa98xx->dsp_lock);
	init_waitqueue_head(&tfa98xx->wq);
#ifdef OPLUS_BUG_COMPATIBILITY
	tfa98xx->reset_gpio = -1;
	tfa98xx->irq_gpio = -1;
#else /* OPLUS_BUG_COMPATIBILITY */
	if (np) {
		ret = tfa98xx_parse_dt(&i2c->dev, tfa98xx, np);
		if (ret) {
			dev_err(&i2c->dev, "Failed to parse DT node\n");
			return ret;
		}
		if (no_start)
			tfa98xx->irq_gpio = -1;
		if (no_reset)
			tfa98xx->reset_gpio = -1;
	} else {
		tfa98xx->reset_gpio = -1;
		tfa98xx->irq_gpio = -1;
	}
#endif /* OPLUS_BUG_COMPATIBILITY */

#ifdef OPLUS_BUG_COMPATIBILITY
	tfa98xx_parse_dual_spk(&i2c->dev, tfa98xx);
#endif /* OPLUS_BUG_COMPATIBILITY */

	if (gpio_is_valid(tfa98xx->reset_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, tfa98xx->reset_gpio,
			GPIOF_OUT_INIT_LOW, "TFA98XX_RST");
		if (ret)
			return ret;
	}

	if (gpio_is_valid(tfa98xx->irq_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, tfa98xx->irq_gpio,
			GPIOF_DIR_IN, "TFA98XX_INT");
		if (ret)
			return ret;
	}

#ifdef OPLUS_BUG_COMPATIBILITY
	if (tfa98xx_dual_spk) {
	if (tfa98xx->i2c->addr == CHIP_RIGHT_ADDR) {
		pinctrltfa = devm_pinctrl_get(&i2c->dev);
		if (IS_ERR(pinctrltfa)) {
			ret = PTR_ERR(pinctrltfa);
			pr_err("Cannot find pinctrlaud! 2spk\n");
			return ret;
		}
		tfarsthigh = pinctrl_lookup_state(pinctrltfa, "tfa98xx_reset_high");
		if (IS_ERR(tfarsthigh)) {
			ret = PTR_ERR(tfarsthigh);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_high fail %d\n", __func__, ret);
		}
		tfarstlow = pinctrl_lookup_state(pinctrltfa, "tfa98xx_reset_low");
		if (IS_ERR(tfarstlow)) {
			ret = PTR_ERR(tfarstlow);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_low fail %d\n", __func__, ret);
		}
	} else if (tfa98xx->i2c->addr == CHIP_LEFT_ADDR) {
		pinctrltfa_2 = devm_pinctrl_get(&i2c->dev);
		if (IS_ERR(pinctrltfa_2)) {
			ret = PTR_ERR(pinctrltfa_2);
			pr_err("Cannot find pinctrlaud 2spk 2!\n");
			return ret;
		}
		tfarsthigh_2 = pinctrl_lookup_state(pinctrltfa_2, "tfa98xx_reset_high_2");
		if (IS_ERR(tfarsthigh_2)) {
			ret = PTR_ERR(tfarsthigh_2);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_high_2 fail %d\n", __func__, ret);
		}
		tfarstlow_2 = pinctrl_lookup_state(pinctrltfa_2, "tfa98xx_reset_low_2");
		if (IS_ERR(tfarstlow_2)) {
			ret = PTR_ERR(tfarstlow_2);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_low_2 fail %d\n", __func__, ret);
		}
	}
	} else {
	pinctrltfa = devm_pinctrl_get(&i2c->dev);
	if (IS_ERR(pinctrltfa)) {
		ret = PTR_ERR(pinctrltfa);
		pr_err("Cannot find pinctrlaud orig!\n");
		//return ret;
	} else {
		tfarsthigh = pinctrl_lookup_state(pinctrltfa, "tfa98xx_reset_high");
		if (IS_ERR(tfarsthigh)) {
			ret = PTR_ERR(tfarsthigh);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_high fail %d\n", __func__, ret);
		}
		tfarstlow = pinctrl_lookup_state(pinctrltfa, "tfa98xx_reset_low");
		if (IS_ERR(tfarstlow)) {
			ret = PTR_ERR(tfarstlow);
			pr_err("%s pinctrl_lookup_state tfa98xx_reset_low fail %d\n", __func__, ret);
		}
	}
    }
#endif /* OPLUS_BUG_COMPATIBILITY */

#ifdef OPLUS_BUG_COMPATIBILITY
    ret = of_property_read_s32(i2c->dev.of_node, "audio_mic_mode", &s_mic_mode);
	if (ret) {
        pr_err("Cannot find audio_mic_mode config\n");
    }

	ret = of_property_read_s32(i2c->dev.of_node, "headset_mic_mode", &s_headset_mic_mode);
	if (ret) {
        pr_err("Cannot find audio_mic_mode config\n");
    }

	ret = of_property_read_s32(i2c->dev.of_node, "soundtrigger_enable", &s_soundtrigger_enable);
	if (ret) {
        pr_err("Cannot find soundtrigger_enable config\n");
    }

	pr_err("mic_mode = %d, headset_mic_mode = %d, soundtrigger_enable = %d\n", s_mic_mode, s_headset_mic_mode, s_soundtrigger_enable);
#endif /* OPLUS_BUG_COMPATIBILITY */

	/* Power up! */
	tfa98xx_ext_reset(tfa98xx);

	if ((no_start == 0) && (no_reset == 0)) {
		#ifdef OPLUS_BUG_COMPATIBILITY
		for (i = 0; i < 100; i++) {
			ret = regmap_read(tfa98xx->regmap, 0x03, &reg);
			if ((ret < 0) || !is_tfa98xx_series_v6(reg & 0xff)) {
				dev_err(&i2c->dev, "Failed to read Revision register: %d\n", ret);
				msleep(2);//2ms
			} else {
				break;
			}
		}
		#else /* OPLUS_BUG_COMPATIBILITY */
		ret = regmap_read(tfa98xx->regmap, 0x03, &reg);
		if (ret < 0) {
			dev_err(&i2c->dev, "Failed to read Revision register: %d\n",
				ret);
			return -EIO;
		}
		#endif /* OPLUS_BUG_COMPATIBILITY */
		switch (reg & 0xff) {
		case 0x72: /* tfa9872 */
			pr_info("TFA9872 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_MULTI_MIC_INPUTS;
			tfa98xx->flags |= TFA98XX_FLAG_CALIBRATION_CTL;
			tfa98xx->flags |= TFA98XX_FLAG_REMOVE_PLOP_NOISE;
			/* tfa98xx->flags |= TFA98XX_FLAG_LP_MODES; */
			tfa98xx->flags |= TFA98XX_FLAG_TDM_DEVICE;
			break;
		case 0x74: /* tfa9874 */
			pr_info("TFA9874 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_MULTI_MIC_INPUTS;
			tfa98xx->flags |= TFA98XX_FLAG_CALIBRATION_CTL;
			tfa98xx->flags |= TFA98XX_FLAG_TDM_DEVICE;
			break;
		case 0x88: /* tfa9888 */
			pr_info("TFA9888 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_STEREO_DEVICE;
			tfa98xx->flags |= TFA98XX_FLAG_MULTI_MIC_INPUTS;
			tfa98xx->flags |= TFA98XX_FLAG_TDM_DEVICE;
			break;
		case 0x13: /* tfa9912 */
			pr_info("TFA9912 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_MULTI_MIC_INPUTS;
			tfa98xx->flags |= TFA98XX_FLAG_TDM_DEVICE;
			/* tfa98xx->flags |= TFA98XX_FLAG_TAPDET_AVAILABLE; */
			break;
		case 0x94: /* tfa9894 */
			pr_info("TFA9894 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_MULTI_MIC_INPUTS;
			tfa98xx->flags |= TFA98XX_FLAG_TDM_DEVICE;
			break;
		case 0x80: /* tfa9890 */
		case 0x81: /* tfa9890 */
			pr_info("TFA9890 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_SKIP_INTERRUPTS;
			break;
		case 0x92: /* tfa9891 */
			pr_info("TFA9891 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_SAAM_AVAILABLE;
			tfa98xx->flags |= TFA98XX_FLAG_SKIP_INTERRUPTS;
			break;
		case 0x12: /* tfa9895 */
			pr_info("TFA9895 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_SKIP_INTERRUPTS;
			break;
		case 0x97:
			pr_info("TFA9897 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_SKIP_INTERRUPTS;
			tfa98xx->flags |= TFA98XX_FLAG_TDM_DEVICE;
			break;
		case 0x96:
			pr_info("TFA9896 detected\n");
			tfa98xx->flags |= TFA98XX_FLAG_SKIP_INTERRUPTS;
			tfa98xx->flags |= TFA98XX_FLAG_TDM_DEVICE;
			break;
		default:
			pr_info("Unsupported device revision (0x%x)\n", reg & 0xff);
			return -EINVAL;
		}
	}

	if (tfa98xx_dual_spk)
		tfa98xx->flags |= TFA98XX_FLAG_CHIP_SELECTED;

	tfa98xx->tfa = devm_kzalloc(&i2c->dev, sizeof(struct tfa_device), GFP_KERNEL);
	if (tfa98xx->tfa == NULL)
		return -ENOMEM;

	tfa98xx->tfa->data = (void *)tfa98xx;
	tfa98xx->tfa->cachep = tfa98xx_cache;

	#ifdef OPLUS_ARCH_EXTENDS
	ret = of_property_read_u32(i2c->dev.of_node, "tfa_min_range", &tfa98xx->tfa->min_mohms);
	if (ret) {
		pr_err("Failed to parse spk_min_range node\n");
		tfa98xx->tfa->min_mohms = SMART_PA_RANGE_DEFAULT_MIN;
	}

	ret = of_property_read_u32(i2c->dev.of_node, "tfa_max_range", &tfa98xx->tfa->max_mohms);
	if (ret) {
		pr_err("Failed to parse spk_max_range node\n");
		tfa98xx->tfa->max_mohms = SMART_PA_RANGE_DEFAULT_MAX;
	}

	pr_info("min_mohms=%d, max_mohms=%d\n",
			tfa98xx->tfa->min_mohms, tfa98xx->tfa->max_mohms);
	#endif/*OPLUS_ARCH_EXTENDS*/


	/* Modify the stream names, by appending the i2c device address.
	 * This is used with multicodec, in order to discriminate the devices.
	 * Stream names appear in the dai definition and in the stream  	 .
	 * We create copies of original structures because each device will
	 * have its own instance of this structure, with its own address.
	 */
	dai = devm_kzalloc(&i2c->dev, sizeof(tfa98xx_dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;
	memcpy(dai, tfa98xx_dai, sizeof(tfa98xx_dai));

	tfa98xx_append_i2c_address(&i2c->dev,
				i2c,
				NULL,
				0,
				dai,
				ARRAY_SIZE(tfa98xx_dai));

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	ret = snd_soc_register_codec(&i2c->dev,
				&soc_codec_dev_tfa98xx, dai,
				ARRAY_SIZE(tfa98xx_dai));
#else
	ret = snd_soc_register_component(&i2c->dev,
			&soc_component_dev_tfa98xx, dai,
			ARRAY_SIZE(tfa98xx_dai));

#endif
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to register TFA98xx: %d\n", ret);
		return ret;
	}
#if 0
	if (gpio_is_valid(tfa98xx->irq_gpio) &&
		!(tfa98xx->flags & TFA98XX_FLAG_SKIP_INTERRUPTS)) {
		/* register irq handler */
		irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
		ret = devm_request_threaded_irq(&i2c->dev,
					gpio_to_irq(tfa98xx->irq_gpio),
					NULL, tfa98xx_irq, irq_flags,
					"tfa98xx", tfa98xx);
		if (ret != 0) {
			dev_err(&i2c->dev, "Failed to request IRQ %d: %d\n",
					gpio_to_irq(tfa98xx->irq_gpio), ret);
			return ret;
		}
	} else {
		dev_info(&i2c->dev, "Skipping IRQ registration\n");
		/* disable feature support if gpio was invalid */
		tfa98xx->flags |= TFA98XX_FLAG_SKIP_INTERRUPTS;
	}
#endif

	#ifndef OPLUS_BUG_COMPATIBILITY
	prEntry_temp = proc_create("oppo_tfa98xx_fw_update", 0644, prEntry_tfa9890,&proc_firmware_update);
	#endif /* OPLUS_BUG_COMPATIBILITY */

	if (no_start == 0)
		tfa98xx_debug_init(tfa98xx, i2c);

	#ifdef OPLUS_BUG_COMPATIBILITY
	// create node
	prEntry_temp_v6 = proc_create(TFA98XX_DEBUG_FS_NAME_V6, 0644, prEntry_tfa9890, &tfa98xx_debug_ops);
	#endif /* OPLUS_BUG_COMPATIBILITY */

	/* Register the sysfs files for climax backdoor access */
	ret = device_create_bin_file(&i2c->dev, &dev_attr_rw);
	if (ret)
		dev_info(&i2c->dev, "error creating sysfs files\n");
	ret = device_create_bin_file(&i2c->dev, &dev_attr_reg);
	if (ret)
		dev_info(&i2c->dev, "error creating sysfs files\n");

	pr_info("%s Probe completed successfully!\n", __func__);

	INIT_LIST_HEAD(&tfa98xx->list);

	mutex_lock(&tfa98xx_mutex);
	tfa98xx_device_count++;
	list_add(&tfa98xx->list, &tfa98xx_device_list);
	mutex_unlock(&tfa98xx_mutex);

	return 0;
}

static int tfa98xx_i2c_remove(struct i2c_client *i2c)
{
	struct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);

	pr_info("addr=0x%x\n", i2c->addr);

	tfa98xx_interrupt_enable(tfa98xx, false);

	cancel_delayed_work_sync(&tfa98xx->interrupt_work);
	cancel_delayed_work_sync(&tfa98xx->monitor_work);
	cancel_delayed_work_sync(&tfa98xx->init_work);
	cancel_delayed_work_sync(&tfa98xx->tapdet_work);

	#ifdef OPLUS_BUG_COMPATIBILITY
	cancel_delayed_work_sync(&tfa98xx->volume_work);
	#endif /* OPLUS_BUG_COMPATIBILITY */

	device_remove_bin_file(&i2c->dev, &dev_attr_reg);
	device_remove_bin_file(&i2c->dev, &dev_attr_rw);

	tfa98xx_debug_remove(tfa98xx);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,19,0)
	snd_soc_unregister_codec(&i2c->dev);
#else
    snd_soc_unregister_component(&i2c->dev);
#endif

	if (gpio_is_valid(tfa98xx->irq_gpio))
		devm_gpio_free(&i2c->dev, tfa98xx->irq_gpio);
	if (gpio_is_valid(tfa98xx->reset_gpio))
		devm_gpio_free(&i2c->dev, tfa98xx->reset_gpio);

	mutex_lock(&tfa98xx_mutex);
	list_del(&tfa98xx->list);
	tfa98xx_device_count--;
	if (tfa98xx_device_count == 0) {
		kfree(tfa98xx_container);
		tfa98xx_container = NULL;
	}
	mutex_unlock(&tfa98xx_mutex);

	return 0;
}

static const struct i2c_device_id tfa98xx_i2c_id[] = {
	{ "tfa98xx", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tfa98xx_i2c_id);

#ifdef CONFIG_OF
static struct of_device_id tfa98xx_dt_match[] = {
	{ .compatible = "nxp,tfa98xx" },
	{ .compatible = "nxp,tfa9872" },
	{ .compatible = "nxp,tfa9874" },
	{ .compatible = "nxp,tfa9888" },
	{ .compatible = "nxp,tfa9890" },
	{ .compatible = "nxp,tfa9891" },
	{ .compatible = "nxp,tfa9894" },
	{ .compatible = "nxp,tfa9895" },
	{ .compatible = "nxp,tfa9896" },
	{ .compatible = "nxp,tfa9897" },
	{ .compatible = "nxp,tfa9912" },
	{ },
};
#endif

static struct i2c_driver tfa98xx_i2c_driver = {
	.driver = {
		.name = "tfa98xx",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(tfa98xx_dt_match),
	},
	.probe =    tfa98xx_i2c_probe,
	.remove =   tfa98xx_i2c_remove,
	.id_table = tfa98xx_i2c_id,
};

static int __init tfa98xx_i2c_init(void)
{
	int ret = 0;

	pr_info("TFA98XX driver version %s\n", TFA98XX_VERSION);

	/* Enable debug traces */
	tfa98xx_kmsg_regs = trace_level & 2;
	tfa98xx_ftrace_regs = trace_level & 4;

	/* Initialize kmem_cache */
	tfa98xx_cache = kmem_cache_create("tfa98xx_cache", /* Cache name /proc/slabinfo */
				PAGE_SIZE, /* Structure size, we should fit in single page */
				0, /* Structure alignment */
				(SLAB_HWCACHE_ALIGN | SLAB_RECLAIM_ACCOUNT |
				SLAB_MEM_SPREAD), /* Cache property */
				NULL); /* Object constructor */
	if (!tfa98xx_cache) {
		pr_err("tfa98xx can't create memory pool\n");
		ret = -ENOMEM;
	}

	ret = i2c_add_driver(&tfa98xx_i2c_driver);

	return ret;
}
module_init(tfa98xx_i2c_init);

static void __exit tfa98xx_i2c_exit(void)
{
	i2c_del_driver(&tfa98xx_i2c_driver);
	kmem_cache_destroy(tfa98xx_cache);
}
module_exit(tfa98xx_i2c_exit);

MODULE_DESCRIPTION("ASoC TFA98XX driver");
MODULE_LICENSE("GPL");
