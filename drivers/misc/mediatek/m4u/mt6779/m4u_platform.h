/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __M4U_PORT_PRIV_H__
#define __M4U_PORT_PRIV_H__

static const char *const gM4U_SMILARB[] = {
	"mediatek,smi_larb0", "mediatek,smi_larb1", "mediatek,smi_larb2",
	"mediatek,smi_larb3", "mediatek,smi_larb4", "mediatek,smi_larb5",
	"mediatek,smi_larb6", "mediatek,smi_larb7", "mediatek,smi_larb8",
	"mediatek,smi_larb9", "mediatek,smi_larb10"
};

#define M4U0_PORT_INIT(name, slave, larb_id, smi_select_larb_id, port)  {\
		name, 0, slave, larb_id, port, \
		(((smi_select_larb_id)<<7)|((port)<<2)), 1\
}

#define M4U1_PORT_INIT(name, slave, larb_id, smi_select_larb_id, port)  {\
		name, 1, slave, larb_id, port, \
		(((smi_select_larb_id)<<7)|((port)<<2)), 1\
}

struct m4u_port_t gM4uPort[] = {
	/* larb0 -MMSYS-9 */
	M4U0_PORT_INIT("DISP_POSTMASK0", 0, 0, 0, 0),
	M4U0_PORT_INIT("DISP_OVL0_HDR", 0, 0, 0, 1),
	M4U0_PORT_INIT("DISP_OVL1_HDR", 0, 0, 0, 2),
	M4U0_PORT_INIT("DISP_OVL0", 0, 0, 0, 3),
	M4U0_PORT_INIT("DISP_OVL1", 0, 0, 0, 4),
	M4U0_PORT_INIT("DISP_PVRIC0", 0, 0, 0, 5),
	M4U0_PORT_INIT("DISP_RDMA0", 0, 0, 0, 6),
	M4U0_PORT_INIT("DISP_WDMA0", 0, 0, 0, 7),
	M4U0_PORT_INIT("DISP_FAKE0", 0, 0, 0, 8),
	/*larb1-MMSYS-14*/
	M4U0_PORT_INIT("DISP_OVL0_2L_HDR", 0, 1, 4, 0),
	M4U0_PORT_INIT("DISP_OVL1_2L_HDR", 0, 1, 4, 1),
	M4U0_PORT_INIT("DISP_OVL0_2L", 0, 1, 4, 2),
	M4U0_PORT_INIT("DISP_OVL1_2L", 0, 1, 4, 3),
	M4U0_PORT_INIT("DISP_RDMA1", 0, 1, 4, 4),
	M4U0_PORT_INIT("MDP_PVRIC0", 0, 1, 4, 5),
	M4U0_PORT_INIT("MDP_PVRIC1", 0, 1, 4, 6),
	M4U0_PORT_INIT("MDP_RDMA0", 0, 1, 4, 7),
	M4U0_PORT_INIT("MDP_RDMA1", 0, 1, 4, 8),
	M4U0_PORT_INIT("MDP_WROT0_R", 0, 1, 4, 9),
	M4U0_PORT_INIT("MDP_WROT0_W", 0, 1, 4, 10),
	M4U0_PORT_INIT("MDP_WROT1_R", 0, 1, 4, 11),
	M4U0_PORT_INIT("MDP_WROT1_W", 0, 1, 4, 12),
	M4U0_PORT_INIT("DISP_FAKE1", 0, 1, 4, 13),
	/*larb2-VDEC-12*/
	M4U0_PORT_INIT("VDEC_MC_EXT", 0, 2, 8, 0),
	M4U0_PORT_INIT("VDEC_UFO_EXT", 0, 2, 8, 1),
	M4U0_PORT_INIT("VDEC_PP_EXT", 0, 2, 8, 2),
	M4U0_PORT_INIT("VDEC_PRED_RD_EXT", 0, 2, 8, 3),
	M4U0_PORT_INIT("VDEC_PRED_WR_EXT", 0, 2, 8, 4),
	M4U0_PORT_INIT("VDEC_PPWRAP_EXT", 0, 2, 8, 5),
	M4U0_PORT_INIT("VDEC_TILE_EXT", 0, 2, 8, 6),
	M4U0_PORT_INIT("VDEC_VLD_EXT", 0, 2, 8, 7),
	M4U0_PORT_INIT("VDEC_VLD2_EXT", 0, 2, 8, 8),
	M4U0_PORT_INIT("VDEC_AVC_MV_EXT", 0, 2, 8, 9),
	M4U0_PORT_INIT("VDEC_UFO_ENC_EXT", 0, 2, 8, 10),
	M4U0_PORT_INIT("VDEC_RG_CTRL_DMA_EXT", 0, 2, 8, 11),
	/*larb3-VENC-19*/
	M4U0_PORT_INIT("VENC_RCPU", 0, 3, 12, 0),
	M4U0_PORT_INIT("VENC_REC", 0, 3, 12, 1),
	M4U0_PORT_INIT("VENC_BSDMA", 0, 3, 12, 2),
	M4U0_PORT_INIT("VENC_SV_COMV", 0, 3, 12, 3),
	M4U0_PORT_INIT("VENC_RD_COMV", 0, 3, 12, 4),

	M4U0_PORT_INIT("VENC_NBM_RDMA", 0, 3, 12, 5),
	M4U0_PORT_INIT("VENC_NBM_RDMA_LITE", 0, 3, 12, 6),
	M4U0_PORT_INIT("JPGENC_Y_RDMA", 0, 3, 12, 7),
	M4U0_PORT_INIT("JPGENC_C_RDMA", 0, 3, 12, 8),
	M4U0_PORT_INIT("JPGENC_Q_TABLE", 0, 3, 12, 9),

	M4U0_PORT_INIT("JPGENC_BSDMA", 0, 3, 12, 10),
	M4U0_PORT_INIT("JPGEDC_WDMA", 0, 3, 12, 11),
	M4U0_PORT_INIT("JPGEDC_BSDMA", 0, 3, 12, 12),
	M4U0_PORT_INIT("VENC_NBM_WDMA", 0, 3, 12, 13),
	M4U0_PORT_INIT("VENC_NBM_WDMA_LITE", 0, 3, 12, 14),

	M4U0_PORT_INIT("VENC_CUR_LUMA", 0, 3, 12, 15),
	M4U0_PORT_INIT("VENC_CUR_CHROMA", 0, 3, 12, 16),
	M4U0_PORT_INIT("VENC_REF_LUMA", 0, 3, 12, 17),
	M4U0_PORT_INIT("VENC_REF_CHROMA", 0, 3, 12, 18),
	/*larb4-dummy*/

	/*larb5-IMG-26*/
	M4U0_PORT_INIT("IMGI_D1", 0, 5, 16, 0),
	M4U0_PORT_INIT("IMGBI_D1", 0, 5, 16, 1),
	M4U0_PORT_INIT("DMGI_D1", 0, 5, 16, 2),
	M4U0_PORT_INIT("DEPI_D1", 0, 5, 16, 3),
	M4U0_PORT_INIT("LCEI_D1", 0, 5, 16, 4),
	M4U0_PORT_INIT("SMTI_D1", 0, 5, 16, 5),
	M4U0_PORT_INIT("SMTO_D2", 0, 5, 16, 6),
	M4U0_PORT_INIT("SMTO_D1", 0, 5, 16, 7),
	M4U0_PORT_INIT("CRZO_D1", 0, 5, 16, 8),
	M4U0_PORT_INIT("IMG3O_D1", 0, 5, 16, 9),

	M4U0_PORT_INIT("VIPI_D1", 0, 5, 16, 10),
	M4U0_PORT_INIT("WPE_A_RDMA1", 0, 5, 16, 11),
	M4U0_PORT_INIT("WPE_A_RDMA0", 0, 5, 16, 12),
	M4U0_PORT_INIT("WPE_A_WDMA", 0, 5, 16, 13),
	M4U0_PORT_INIT("TIMGO_D1", 0, 5, 16, 14),
	M4U0_PORT_INIT("MFB_RDMA0", 0, 5, 16, 15),
	M4U0_PORT_INIT("MFB_RDMA1", 0, 5, 16, 16),
	M4U0_PORT_INIT("MFB_RDMA2", 0, 5, 16, 17),
	M4U0_PORT_INIT("MFB_RDMA3", 0, 5, 16, 18),
	M4U0_PORT_INIT("MFB_WDMA", 0, 5, 16, 19),

	M4U0_PORT_INIT("RESERVED1", 0, 5, 16, 20),
	M4U0_PORT_INIT("RESERVED2", 0, 5, 16, 21),
	M4U0_PORT_INIT("RESERVED3", 0, 5, 16, 22),
	M4U0_PORT_INIT("RESERVED4", 0, 5, 16, 23),
	M4U0_PORT_INIT("RESERVED5", 0, 5, 16, 24),
	M4U0_PORT_INIT("RESERVED6", 0, 5, 16, 25),

	/*larb6-IMG-3
	 *M4U0_PORT_INIT("IMG_IPUO", 0, 6, 3, 0),
	 *M4U0_PORT_INIT("IMG_IPU3O", 0, 6, 3, 1),
	 *M4U0_PORT_INIT("IMG_IPUI,", 0, 6, 3, 2),
	 */

	/*larb7-IPESYS-4*/
	M4U0_PORT_INIT("DVS_RDMA", 0, 7, 20, 0),
	M4U0_PORT_INIT("DVS_WDMA", 0, 7, 20, 1),
	M4U0_PORT_INIT("DVP_RDMA,", 0, 7, 20, 2),
	M4U0_PORT_INIT("DVP_WDMA,", 0, 7, 20, 3),

	/*larb8-IPESYS-10*/
	M4U0_PORT_INIT("FDVT_RDA", 0, 8, 21, 0),
	M4U0_PORT_INIT("FDVT_RDB", 0, 8, 21, 1),
	M4U0_PORT_INIT("FDVT_WRA", 0, 8, 21, 2),
	M4U0_PORT_INIT("FDVT_WRB", 0, 8, 21, 3),
	M4U0_PORT_INIT("FE_RD0", 0, 8, 21, 4),
	M4U0_PORT_INIT("FE_RD1", 0, 8, 21, 5),
	M4U0_PORT_INIT("FE_WR0", 0, 8, 21, 6),
	M4U0_PORT_INIT("FE_WR1", 0, 8, 21, 7),
	M4U0_PORT_INIT("RSC_RDMA0", 0, 8, 21, 8),
	M4U0_PORT_INIT("RSC_WDMA", 0, 8, 21, 9),

	/*larb9-CAM-24*/
	M4U0_PORT_INIT("CAM_IMGO_R1_C", 0, 9, 28, 0),
	M4U0_PORT_INIT("CAM_RRZO_R1_C", 0, 9, 28, 1),
	M4U0_PORT_INIT("CAM_LSCI_R1_C", 0, 9, 28, 2),
	M4U0_PORT_INIT("CAM_BPCI_R1_C", 0, 9, 28, 3),
	M4U0_PORT_INIT("CAM_YUVO_R1_C", 0, 9, 28, 4),
	M4U0_PORT_INIT("CAM_UFDI_R2_C", 0, 9, 28, 5),
	M4U0_PORT_INIT("CAM_RAWI_R2_C", 0, 9, 28, 6),
	M4U0_PORT_INIT("CAM_RAWI_R5_C", 0, 9, 28, 7),
	M4U0_PORT_INIT("CAM_CAMSV_1", 0, 9, 28, 8),
	M4U0_PORT_INIT("CAM_CAMSV_2", 0, 9, 28, 9),

	M4U0_PORT_INIT("CAM_CAMSV_3", 0, 9, 28, 10),
	M4U0_PORT_INIT("CAM_CAMSV_4", 0, 9, 28, 11),
	M4U0_PORT_INIT("CAM_CAMSV_5", 0, 9, 28, 12),
	M4U0_PORT_INIT("CAM_CAMSV_6", 0, 9, 28, 13),
	M4U0_PORT_INIT("CAM_AAO_R1_C", 0, 9, 28, 14),
	M4U0_PORT_INIT("CAM_AFO_R1_C", 0, 9, 28, 15),
	M4U0_PORT_INIT("CAM_FLKO_R1_C", 0, 9, 28, 16),
	M4U0_PORT_INIT("CAM_LCESO_R1_C", 0, 9, 28, 17),
	M4U0_PORT_INIT("CAM_CRZO_R1_C", 0, 9, 28, 18),
	M4U0_PORT_INIT("CAM_LTMSO_R1_C", 0, 9, 28, 19),

	M4U0_PORT_INIT("CAM_RSSO_R1_C", 0, 9, 28, 20),
	M4U0_PORT_INIT("CAM_CCUI", 0, 9, 28, 21),
	M4U0_PORT_INIT("CAM_CCUO", 0, 9, 28, 22),
	M4U0_PORT_INIT("CAM_FAKE", 0, 9, 28, 23),

	/*larb10-CAM-31*/
	M4U0_PORT_INIT("CAM_IMGO_R1_A", 0, 10, 25, 0),
	M4U0_PORT_INIT("CAM_RRZO_R1_A", 0, 10, 25, 1),
	M4U0_PORT_INIT("CAM_LSCI_R1_A", 0, 10, 25, 2),
	M4U0_PORT_INIT("CAM_BPCI_R1_A", 0, 10, 25, 3),
	M4U0_PORT_INIT("CAM_YUVO_R1_A", 0, 10, 25, 4),
	M4U0_PORT_INIT("CAM_UFDI_R2_A", 0, 10, 25, 5),
	M4U0_PORT_INIT("CAM_RAWI_R2_A", 0, 10, 25, 6),
	M4U0_PORT_INIT("CAM_RAWI_R5_A", 0, 10, 25, 7),
	M4U0_PORT_INIT("CAM_IMGO_R1_B", 0, 10, 25, 8),
	M4U0_PORT_INIT("CAM_RRZO_R1_B", 0, 10, 25, 9),

	M4U0_PORT_INIT("CAM_LSCI_R1_B", 0, 10, 25, 10),
	M4U0_PORT_INIT("CAM_BPCI_R1_B", 0, 10, 25, 11),
	M4U0_PORT_INIT("CAM_YUVO_R1_B,", 0, 10, 25, 12),
	M4U0_PORT_INIT("CAM_UFDI_R2_B", 0, 10, 25, 13),
	M4U0_PORT_INIT("CAM_RAWI_R2_B", 0, 10, 25, 14),
	M4U0_PORT_INIT("CAM_RAWI_R5_B", 0, 10, 25, 15),
	M4U0_PORT_INIT("CAM_CAMSV_0", 0, 10, 25, 16),
	M4U0_PORT_INIT("CAM_AAO_R1_A", 0, 10, 25, 17),
	M4U0_PORT_INIT("CAM_AFO_R1_A", 0, 10, 25, 18),
	M4U0_PORT_INIT("CAM_FLKO_R1_A", 0, 10, 25, 19),

	M4U0_PORT_INIT("CAM_LCESO_R1_A", 0, 10, 25, 20),
	M4U0_PORT_INIT("CAM_CRZO_R1_A", 0, 10, 25, 21),
	M4U0_PORT_INIT("CAM_AAO_R1_B", 0, 10, 25, 22),
	M4U0_PORT_INIT("CAM_AFO_R1_B", 0, 10, 25, 23),
	M4U0_PORT_INIT("CAM_FLKO_R1_B", 0, 10, 25, 24),
	M4U0_PORT_INIT("CAM_LCESO_R1_B", 0, 10, 25, 25),
	M4U0_PORT_INIT("CAM_CRZO_R1_B", 0, 10, 25, 26),
	M4U0_PORT_INIT("CAM_LTMSO_R1_A", 0, 10, 25, 27),
	M4U0_PORT_INIT("CAM_RSSO_R1_A", 0, 10, 25, 28),
	M4U0_PORT_INIT("CAM_LTMSO_R1_B", 0, 10, 25, 29),
	M4U0_PORT_INIT("CAM_RSSO_R1_B", 0, 10, 25, 30),
	/*larb11-CAM-5
	 *M4U0_PORT_INIT("CAM_IPUO", 0, 11, 3, 0),
	 *M4U0_PORT_INIT("CAM_IPU2O", 0, 11, 3, 1),
	 *M4U0_PORT_INIT("CAM_IPU3O", 0, 11, 3, 2),
	 *M4U0_PORT_INIT("CAM_IPUI", 0, 11, 3, 3),
	 *M4U0_PORT_INIT("CAM_IPU2I", 0, 11, 3, 4),
	 */

	M4U0_PORT_INIT("CCU0", 0, 9, 24, 0),
	M4U0_PORT_INIT("CCU1", 0, 9, 24, 1),

	M4U1_PORT_INIT("VPU", 0, 0, 0, 0),

	M4U0_PORT_INIT("UNKNOWN", 0, 0, 0, 0)
};


#endif
