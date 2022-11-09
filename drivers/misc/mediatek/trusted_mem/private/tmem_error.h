/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef TMEM_ERROR_H
#define TMEM_ERROR_H

enum TMEM_ERROR_STATUS {
	TMEM_OK = 0,
	TMEM_GENERAL_ERROR = -1,
	TMEM_PARAMETER_ERROR = -2,
	TMEM_INVALID_REGISTER_DEVICE = -3,
	TMEM_MEM_DEVICE_ALREADY_REGISTERED = -4,
	TMEM_CREATE_DEVICE_FAILED = -5,
	TMEM_SSMR_OFFLINE_FAILED = -6,
	TMEM_SSMR_ONLINE_FAILED = -7,
	TMEM_OPERATION_NOT_IMPLEMENTED = -8,
	TMEM_INVALID_ADDR_OR_SIZE = -9,
	TMEM_CREATE_PEER_DESC_FAILED = -10,
	TMEM_MGR_SESSION_IS_NOT_READY = -11,
	TMEM_MGR_SESSION_IS_ALREADY_OPEN = -12,
	TMEM_MGR_SESSION_IS_ALREADY_CLOSE = -13,
	TMEM_MGR_OPEN_SESSION_FAILED = -14,
	TMEM_MGR_CLOSE_SESSION_FAILED = -15,
	TMEM_MGR_ALLOC_MEM_FAILED = -16,
	TMEM_MGR_FREE_MEM_FAILED = -17,
	TMEM_MGR_MEM_ADD_FAILED = -18,
	TMEM_MGR_MEM_REMOVE_FAILED = -19,
	TMEM_REGION_POWER_ON_FAILED = -21,
	TMEM_REGION_POWER_OFF_FAILED = -22,
	TMEM_OPERATION_NOT_REGISTERED = -23,
	TMEM_INVALID_PHYICAL_MIN_CHUNK_SIZE = -24,
	TMEM_INVALID_DEVICE_MIN_CHUNK_SIZE = -25,
	TMEM_INVALID_ALIGNMENT_REQUEST = -26,
	TMEM_REGION_IS_NOT_READY_BEFORE_MEM_FREE_OPERATION = -27,
	TMEM_INVALID_OPS_HOOKS = -28,
	TMEM_CREATE_SHARED_DEVICE_FAILED = -29,
	TMEM_SHARED_DEVICE_REGION_IS_BUSY = -30,
	TMEM_MGR_INVOKE_COMMAND_FAILED = -31,
	TMEM_COMMAND_NOT_SUPPORTED = -32,
	TMEM_TEE_NOTIFY_MEM_ADD_CFG_TO_MTEE_FAILED = -33,
	TMEM_TEE_NOTIFY_MEM_REMOVE_CFG_TO_MTEE_FAILED = -34,

	TMEM_MTEE_CREATE_SESSION_DATA_FAILED = -1000,
	TMEM_MTEE_CREATE_SESSION_FAILED = -1001,
	TMEM_MTEE_CLOSE_SESSION_FAILED = -1002,
	TMEM_MTEE_APPEND_MEMORY_FAILED = -1003,
	TMEM_MTEE_RELEASE_MEMORY_FAILED = -1004,
	TMEM_MTEE_ALLOC_CHUNK_FAILED = -1005,
	TMEM_MTEE_FREE_CHUNK_FAILED = -1006,
	TMEM_MTEE_NOTIFY_MEM_ADD_CFG_TO_TEE_FAILED = -1007,
	TMEM_MTEE_NOTIFY_MEM_REMOVE_CFG_TO_TEE_FAILED = -1008,
	TMEM_MTEE_INVOKE_COMMAND_FAILED = -1009,

	TMEM_TEE_CREATE_SESSION_DATA_FAILED = -2000,
	TMEM_TEE_CREATE_SESSION_FAILED = -2001,
	TMEM_TEE_CLOSE_SESSION_FAILED = -2002,
	TMEM_TEE_APPEND_MEMORY_FAILED = -2003,
	TMEM_TEE_RELEASE_MEMORY_FAILED = -2004,
	TMEM_TEE_ALLOC_CHUNK_FAILED = -2005,
	TMEM_TEE_FREE_CHUNK_FAILED = -2006,
	TMEM_TEE_INVOKE_COMMAND_FAILED = -2007,
	TMEM_TEE_OPEN_SINGLE_SESSION_FAILED = -2017,
	TMEM_TEE_SESSION_IS_NOT_READY = -2018,
};

#endif /* end of TMEM_ERROR_H */
