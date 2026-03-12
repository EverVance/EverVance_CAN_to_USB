 /*
 * 
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2018-2019 NXP
 * All rights reserved.
 *
 *
 * 
 *  Redistribution and use in source and binary forms, with or without modification,
 *  are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright notice, this list
 *    of conditions and the following disclaimer.
 * 
 *  2. Redistributions in binary form must reproduce the above copyright notice, this
 *    list of conditions and the following disclaimer in the documentation and/or
 *    other materials provided with the distribution.
 * 
 *  3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 * 
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * flexspi_spifi_flash.h
 *
 * This file defines the page sizes and other configuration options used
 * in the flash driver system.
 *
 *
 */
#include "fsl_flexspi.h"
#include "fsl_debug_console.h"

#include "pin_mux.h"
#include "board.h"
#include "clock_config.h"
#include "fsl_common.h"

/*******************************************************************************
* Definitions
******************************************************************************/
#define EXAMPLE_FLEXSPI FLEXSPI
#define FLASH_SIZE 0x1000 /* 32Mb / 4MByte */
#define EXAMPLE_FLEXSPI_AMBA_BASE FlexSPI_AMBA_BASE
#define FLASH_PAGE_SIZE 256
#define EXAMPLE_SECTOR 0
#define SECTOR_SIZE (4 * 1024) /* MX25L3233 supports 4KB sector erase (0x20) */
#define EXAMPLE_FLEXSPI_CLOCK kCLOCK_FlexSpi

// Additional defines for flash driver
#define PSEUDO_PAGE_SIZE (4 * 1024)
#define DEVICE_SIZE 0x400000
#define SECTOR_NUMBER (DEVICE_SIZE / SECTOR_SIZE)

#define NOR_CMD_LUT_SEQ_IDX_READ_NORMAL 0
#define NOR_CMD_LUT_SEQ_IDX_READ_FAST 1
#define NOR_CMD_LUT_SEQ_IDX_READ_FAST_QUAD 2
#define NOR_CMD_LUT_SEQ_IDX_READSTATUS 3
#define NOR_CMD_LUT_SEQ_IDX_WRITEENABLE 4
#define NOR_CMD_LUT_SEQ_IDX_ERASESECTOR 5
#define NOR_CMD_LUT_SEQ_IDX_PAGEPROGRAM_SINGLE 6
#define NOR_CMD_LUT_SEQ_IDX_PAGEPROGRAM_QUAD 7
#define NOR_CMD_LUT_SEQ_IDX_READID 8
#define NOR_CMD_LUT_SEQ_IDX_WRITESTATUSREG 9
#define NOR_CMD_LUT_SEQ_IDX_ENTERQPI 10
#define NOR_CMD_LUT_SEQ_IDX_EXITQPI 11
#define NOR_CMD_LUT_SEQ_IDX_READSTATUSREG 12

#define CUSTOM_LUT_LENGTH 60
#define FLASH_BUSY_STATUS_POL 1
#define FLASH_BUSY_STATUS_OFFSET 0
#define FLASH_ERROR_STATUS_MASK 0x00

/*******************************************************************************
* Prototypes
*****************************************************************************/
status_t flexspi_nor_write_enable(FLEXSPI_Type *base, uint32_t baseAddr);
status_t flexspi_nor_wait_bus_busy(FLEXSPI_Type *base);
status_t flexspi_nor_enable_quad_mode(FLEXSPI_Type *base);
status_t flexspi_nor_flash_erase_sector(FLEXSPI_Type *base, uint32_t address);
status_t flexspi_nor_flash_page_program(FLEXSPI_Type *base, uint32_t dstAddr, uint32_t length, const uint32_t *src);
status_t flexspi_nor_get_vendor_id(FLEXSPI_Type *base, uint8_t *vendorId);
status_t QSPI_init(void);
