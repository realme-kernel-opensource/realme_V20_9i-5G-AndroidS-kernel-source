/* SPDX-License-Identifier: GPL-2.0 */
/*
 * audio_mem_control.h --  Mediatek ADSP dmemory control
 *
 * Copyright (c) 2018 MediaTek Inc.
 * Author: Chipeng <Chipeng.chang@mediatek.com>
 */

#ifndef MT6779_DSP_MEM_CONTROL_H
#define MT6779_DSP_MEM_CONTROL_H

#include <linux/genalloc.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#include "../v1/mtk-base-dsp.h"

/* clk relate */
#include "../../mt6779/mt6779-afe-clk.h"
#include "../../mt6779/mt6779-afe-common.h"

#define MEMIF_NUM_MAX MT6779_MEMIF_NUM

/* get struct of sharemem_block */
struct audio_dsp_dram *mtk_get_adsp_sharemem_block(int audio_task_id);
struct mtk_adsp_task_attr *mtk_get_adsp_task_attr(int adsp_id);
bool mtk_adsp_dai_id_support_share_mem(int dai_id);

#endif
