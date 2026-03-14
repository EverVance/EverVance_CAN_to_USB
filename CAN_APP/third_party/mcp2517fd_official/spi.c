#include "spi.h"

#include "lpspi1_bus.h"

uint32_t LPSPI_MasterTransferWithCS(LPSPI_Type *base, lpspi_transfer_t *transfer)
{
    (void)base;

    if ((transfer == NULL) || (transfer->dataSize == 0U))
    {
        return 1U;
    }

    return LPSPI1_Transfer(transfer->txData, transfer->rxData, transfer->dataSize) ? 0U : 1U;
}
