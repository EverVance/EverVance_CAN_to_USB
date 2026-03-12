/*
 * 
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2016-2019 NXP
 *
 * All rights reserved.
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
 */

#include <flexspi_QSPI_flash.h>
#include <stdio.h>
#include "fsl_flexspi.h"

#include "pin_mux.h"
#include "board.h"
#include "clock_config.h"
#include "fsl_common.h"

#include "lpcx_flash_driver.h"


/*******************************************************************************
* Definitions
******************************************************************************/

/*******************************************************************************
* Prototypes
******************************************************************************/

/*******************************************************************************
 * Variables
 ******************************************************************************/

extern uint32_t checkblank(uint32_t adr, uint32_t words, uint32_t blankval);

//FlashDriver Global data
unsigned char buf[16 * 1024]  __attribute__ ((aligned (4)));
extern Mailbox_Init_DynInfo_t Mailbox_Init_DynInfo;
char itoa_buf[32];

extern flexspi_device_config_t deviceconfig;

static uint8_t s_spififlash_program_buffer[FLASH_PAGE_SIZE];


int main(void) {
	uint32_t i = 0;
	uint32_t n = 0;
	status_t status;
	uint32_t sector = 0;
	uint32_t page = 0;

	printf("Starting Flash Programming Test\n");

	status = QSPI_init();	// perform call to initialise the device and flash
	if (status != kStatus_Success) {
		printf("Initialisation failed with status %x\n", status);
		while (1)
			;
	} else {
		printf("Initialisation Successful!\n");
	}

	/* Loop through flash sectors */

	for (sector = 0; sector < SECTOR_NUMBER; sector++) {

		printf("Sector Erase %d at address offset %x ", sector,
				sector * SECTOR_SIZE);

		FLEXSPI_SoftwareReset(EXAMPLE_FLEXSPI);	// restore flash into memory map, here for debugging purposes

		status = flexspi_nor_flash_erase_sector(EXAMPLE_FLEXSPI,
				sector * SECTOR_SIZE);

		if (status != kStatus_Success) {
			printf("failed \n");
		} else {
			printf("passed \n");
		}

		// check sector erase has occurred

		FLEXSPI_SoftwareReset(EXAMPLE_FLEXSPI);
		status = checkblank(FlexSPI_AMBA_BASE + sector * SECTOR_SIZE,
				SECTOR_SIZE >> 2, 0xFFFFFFFF);

		if (status != kStatus_Success) {
			printf(" checkblank failed \n");
		} else {
			//printf(" checkblank passed \n");
		}

		/* Loop through flash pages */

		for (page = 0; page < (SECTOR_SIZE / FLASH_PAGE_SIZE); page++) {

			for (i = 0; i < sizeof(s_spififlash_program_buffer); i += 4) {
				n = (sector * SECTOR_SIZE) + (page * FLASH_PAGE_SIZE) + i;

				/* poke values into the flash programming buffer */
				s_spififlash_program_buffer[i + 3] = n >> 24 & 0xFF;
				s_spififlash_program_buffer[i + 2] = n >> 16 & 0xFF;
				s_spififlash_program_buffer[i + 1] = n >> 8 & 0xFF;
				s_spififlash_program_buffer[i + 0] = n & 0xFF;
			}
			status = flexspi_nor_flash_page_program(EXAMPLE_FLEXSPI,
					sector * SECTOR_SIZE + (page * FLASH_PAGE_SIZE),
					(void *) s_spififlash_program_buffer);

			/* if line below is enabled, this will increase the program execution time */

			// printf("Page Program %d, Address %x ", page,sector * SECTOR_SIZE + (page* FLASH_PAGE_SIZE) );
			if (status) {
				printf("failed \n");
			} else {
				//printf("passed \n");			// enabling this line will slow program execution
			}

			/* check that contents of flash match the programming buffer */

			status = Verify(
					FlexSPI_AMBA_BASE + (page * FLASH_PAGE_SIZE)
							+ (sector * SECTOR_SIZE), FLASH_PAGE_SIZE,
					s_spififlash_program_buffer);

			if (status != kStatus_Success) {
				printf("failed page %x verify\n",
						FlexSPI_AMBA_BASE + (page * FLASH_PAGE_SIZE)
								+ (sector * SECTOR_SIZE));
			}
		}
	}

	/* Restore the memory map. */
	FLEXSPI_SoftwareReset(EXAMPLE_FLEXSPI);

	printf("Completed! \n");

	while (1) {
	}
}
