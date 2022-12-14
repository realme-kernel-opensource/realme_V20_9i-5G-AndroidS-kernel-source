/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2016 MediaTek Inc.
 */

/* An example test TA implementation.
 */

#ifndef __TRUSTZONE_TA_DRMKEY__
#define __TRUSTZONE_TA_DRMKEY__

#define TZ_TA_DRMKEY_UUID   "989850BF-4663-9DCD-394C-07A45F4633D1"

/* Data Structure for DRMKEY TA */
/* You should define data structure used both in REE/TEE here
 * N/A for Test TA
 */

/* Command for Test TA */
#define TZCMD_DRMKEY_INSTALL		0
#define TZCMD_DRMKEY_QUERY		1
#define TZCMD_DRMKEY_GEN_EKKB_PUB	2
#define TZCMD_DRMKEY_GEN_KB_EKKB_EKC	3
#define TZCMD_DRMKEY_GEN_REENC_EKKB	4
#define TZCMD_DRMKEY_INIT_ENV		5
#define TZCMD_DRMKEY_VERIFY_AEK		6
#define TZCMD_DRMKEY_SIGNATURE_OP	7

#endif				/* __TRUSTZONE_TA_DRMKEY__ */
