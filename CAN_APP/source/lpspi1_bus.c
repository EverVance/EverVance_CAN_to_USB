#include "lpspi1_bus.h"

#include "fsl_dmamux.h"
#include "fsl_edma.h"
#include "fsl_gpio.h"
#include "fsl_lpspi.h"
#include "fsl_lpspi_edma.h"

#define LPSPI1_CS_PORT GPIO3
#define LPSPI1_CS_PIN (13U)

#define LPSPI1_DMA_RX_CHANNEL (0U)
#define LPSPI1_DMA_TX_CHANNEL (1U)
#define LPSPI1_TRANSFER_TIMEOUT (600000U)

static bool s_Lpspi1Ready;
static volatile bool s_Lpspi1TransferDone;
static volatile status_t s_Lpspi1TransferStatus;

static edma_handle_t s_Lpspi1EdmaRxHandle;
static edma_handle_t s_Lpspi1EdmaTxHandle;
static lpspi_master_edma_handle_t s_Lpspi1MasterEdmaHandle;
static edma_config_t s_EdmaConfig;

static void LPSPI1_MasterEdmaCallback(LPSPI_Type *base, lpspi_master_edma_handle_t *handle, status_t status, void *userData)
{
    (void)base;
    (void)handle;
    (void)userData;
    s_Lpspi1TransferStatus = status;
    s_Lpspi1TransferDone = true;
}

static void LPSPI1_Select(void)
{
    GPIO_PinWrite(LPSPI1_CS_PORT, LPSPI1_CS_PIN, 0U);
}

static void LPSPI1_Unselect(void)
{
    GPIO_PinWrite(LPSPI1_CS_PORT, LPSPI1_CS_PIN, 1U);
}

bool LPSPI1_BusInit(uint32_t srcClockHz, uint32_t busHz)
{
    lpspi_master_config_t masterConfig;

    if ((srcClockHz == 0U) || (busHz == 0U))
    {
        return false;
    }

    /* 初始化 LPSPI1 主机配置。 */
    LPSPI_MasterGetDefaultConfig(&masterConfig);
    masterConfig.baudRate = busHz;
    masterConfig.bitsPerFrame = 8U;
    masterConfig.whichPcs = kLPSPI_Pcs0;
    masterConfig.cpol = kLPSPI_ClockPolarityActiveHigh;
    masterConfig.cpha = kLPSPI_ClockPhaseFirstEdge;
    masterConfig.direction = kLPSPI_MsbFirst;
    LPSPI_MasterInit(LPSPI1, &masterConfig, srcClockHz);
    LPSPI_SetDummyData(LPSPI1, 0xFFU);

    /* 初始化 DMA/DMAMUX，并将 LPSPI1 RX/TX 请求映射到通道 0/1。 */
    EDMA_GetDefaultConfig(&s_EdmaConfig);
    EDMA_Init(DMA0, &s_EdmaConfig);

    DMAMUX_Init(DMAMUX);
    DMAMUX_DisableChannel(DMAMUX, LPSPI1_DMA_RX_CHANNEL);
    DMAMUX_DisableChannel(DMAMUX, LPSPI1_DMA_TX_CHANNEL);
    DMAMUX_SetSource(DMAMUX, LPSPI1_DMA_RX_CHANNEL, (int32_t)kDmaRequestMuxLPSPI1Rx);
    DMAMUX_SetSource(DMAMUX, LPSPI1_DMA_TX_CHANNEL, (int32_t)kDmaRequestMuxLPSPI1Tx);
    DMAMUX_EnableChannel(DMAMUX, LPSPI1_DMA_RX_CHANNEL);
    DMAMUX_EnableChannel(DMAMUX, LPSPI1_DMA_TX_CHANNEL);

    EDMA_CreateHandle(&s_Lpspi1EdmaRxHandle, DMA0, LPSPI1_DMA_RX_CHANNEL);
    EDMA_CreateHandle(&s_Lpspi1EdmaTxHandle, DMA0, LPSPI1_DMA_TX_CHANNEL);
    LPSPI_MasterTransferCreateHandleEDMA(
        LPSPI1, &s_Lpspi1MasterEdmaHandle, LPSPI1_MasterEdmaCallback, NULL, &s_Lpspi1EdmaRxHandle, &s_Lpspi1EdmaTxHandle);

    NVIC_SetPriority(DMA0_DMA16_IRQn, 6U);
    NVIC_SetPriority(DMA1_DMA17_IRQn, 6U);
    EnableIRQ(DMA0_DMA16_IRQn);
    EnableIRQ(DMA1_DMA17_IRQn);

    LPSPI1_Unselect();
    s_Lpspi1Ready = true;
    return true;
}

bool LPSPI1_Transfer(const uint8_t *txData, uint8_t *rxData, size_t length)
{
    lpspi_transfer_t transfer;
    uint32_t timeout;

    if ((s_Lpspi1Ready == false) || (length == 0U))
    {
        return false;
    }

    /* 启动一次 eDMA 事务，外层仍提供同步等待语义。 */
    transfer.txData = txData;
    transfer.rxData = rxData;
    transfer.dataSize = length;
    transfer.configFlags = (uint32_t)kLPSPI_MasterPcs0;

    s_Lpspi1TransferDone = false;
    s_Lpspi1TransferStatus = kStatus_LPSPI_Busy;

    LPSPI1_Select();
    if (LPSPI_MasterTransferEDMA(LPSPI1, &s_Lpspi1MasterEdmaHandle, &transfer) != kStatus_Success)
    {
        LPSPI1_Unselect();
        return false;
    }

    /* 轮询等待 DMA 回调置位完成标志。 */
    timeout = LPSPI1_TRANSFER_TIMEOUT;
    while ((!s_Lpspi1TransferDone) && (timeout > 0U))
    {
        timeout--;
    }

    LPSPI1_Unselect();
    return (s_Lpspi1TransferDone && (s_Lpspi1TransferStatus == kStatus_Success));
}

void DMA0_DMA16_IRQHandler(void)
{
    EDMA_HandleIRQ(&s_Lpspi1EdmaRxHandle);
    __DSB();
}

void DMA1_DMA17_IRQHandler(void)
{
    EDMA_HandleIRQ(&s_Lpspi1EdmaTxHandle);
    __DSB();
}
