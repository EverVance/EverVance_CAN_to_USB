//*****************************************************************************
// iap.h
// LPCXpresso Flash driver - IAP header
//*****************************************************************************
//
// Copyright 2014, 2018, 2020, 2021 NXP
// All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause
//*****************************************************************************

#ifndef LPCXFLASHDRIVERLIB_INC_IAP_H_
#define LPCXFLASHDRIVERLIB_INC_IAP_H_

#include <stdint.h>

/* IAP command definitions */
#define IAP_INIT_IAP_CMD				49
#define IAP_PREPARE_FOR_WRITE_CMD		50
#define IAP_COPY_RAM_TO_FLASH_CMD		51
#define IAP_ERASE_SECTOR_CMD			52
#define IAP_BLANKCHECK_SECTOR_CMD      53
#define IAP_COMPARE_CMD                 56
#define IAP_SET_ACTIVE_BOOT_CMD 		60

/* IAP responses */
#define IAP_CMD_SUCCESS             0
#define IAP_SECTOR_NOT_BLANK        8
#define IAP_COMPARE_ERROR           10

struct sIAP {                  // IAP Structure
  uint32_t cmd;           // Command
  uint32_t par[5];        // Parameters
  uint32_t stat;          // Status
  uint32_t res[2];        // Result
};

extern struct sIAP IAP;

typedef void (*IAP_ENTRY_T)(uint32_t *cmd, uint32_t *stat);
#define IAP_Call ((IAP_ENTRY_T) IAP_ADDRESS)
#define IAP_Call_Indirect ((IAP_ENTRY_T) *((uint32_t *) IAP_ADDRESS))

#endif /* LPCXFLASHDRIVERLIB_INC_IAP_H_ */
