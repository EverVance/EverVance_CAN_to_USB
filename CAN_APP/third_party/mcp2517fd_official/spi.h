#ifndef MCP2517FD_OFFICIAL_SPI_H
#define MCP2517FD_OFFICIAL_SPI_H

#include <stdint.h>

#include "fsl_common.h"
#include "fsl_device_registers.h"
#include "fsl_lpspi.h"

#define LPSPI1_PERIPHERAL LPSPI1

uint32_t LPSPI_MasterTransferWithCS(LPSPI_Type *base, lpspi_transfer_t *transfer);

#endif
