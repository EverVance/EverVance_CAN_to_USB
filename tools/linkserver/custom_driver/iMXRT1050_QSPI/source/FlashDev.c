//*****************************************************************************
// FlashDev.c
// LPCXpresso Flash driver - Flash Device settings
//*****************************************************************************
//
// Copyright 2014-2015, 2018, 2020, 2021 NXP
// All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause
//*****************************************************************************

#include <flexspi_QSPI_flash.h>
#include "lpcx_flash_driver.h"
#include "fsl_common.h"

FLASHDEV_SECTION
FlashDeviceV_t FlashDevice  =  {
   FLASH_DRV_VERS,				// Driver Version, do not modify!
   "RT1062_MX25L3233_4K "__DATE__" "__TIME__,
   EXTSPI,     			// Device Type
   FlexSPI_AMBA_BASE, 	// Device Start Address
   DEVICE_SIZE,   			// Device Size
   PSEUDO_PAGE_SIZE,    // Programming Page Size
   0,          			// Reserved, must be 0
   0xFF,       			// Initial Content of Erased Memory
   3000,       			// Program Page Timeout
   6000,       			// Erase Sector Timeout
   // Specify Size and Address of Sectors
   {{SECTOR_SIZE, 0},   // Sector sizes
   {SECTOR_END}}
};


