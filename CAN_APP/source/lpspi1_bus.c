#include "lpspi1_bus.h"

#include <string.h>

#include "fsl_common.h"
#include "fsl_dmamux.h"
#include "fsl_edma.h"
#include "fsl_gpio.h"
#include "fsl_lpspi.h"
#include "fsl_lpspi_edma.h"

/* 文件说明：
 * 本文件提供 LPSPI1 的最小可靠传输封装，专门服务于 CH0 外置 MCP2517FD。
 * 上层驱动不直接操作 LPSPI 寄存器，而统一通过这里完成 SPI 收发。 */

#define LPSPI1_CS_PORT GPIO3
#define LPSPI1_CS_PIN (13U)

#define LPSPI1_DMA_RX_CHANNEL (0U)
#define LPSPI1_DMA_TX_CHANNEL (1U)
#define LPSPI1_TRANSFER_TIMEOUT (600000U)
#define LPSPI1_RX_STAGING_SIZE (96U)

static bool s_Lpspi1Ready;
static volatile bool s_Lpspi1TransferDone;
static volatile status_t s_Lpspi1TransferStatus;
static uint32_t s_Lpspi1SourceClockHz;
static uint32_t s_Lpspi1ConfiguredBaudHz;

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

/* 片选拉低，开始一次外设访问。 */
static void LPSPI1_Select(void)
{
    GPIO_PinWrite(LPSPI1_CS_PORT, LPSPI1_CS_PIN, 0U);
}

/* 片选释放，结束一次外设访问。 */
static void LPSPI1_Unselect(void)
{
    GPIO_PinWrite(LPSPI1_CS_PORT, LPSPI1_CS_PIN, 1U);
}

/* 初始化 LPSPI1 + eDMA 总线层。
 * 这是 CH0 外置 MCP2517FD 的唯一 SPI 通路。 */
bool LPSPI1_BusInit(uint32_t srcClockHz, uint32_t busHz)
{
    lpspi_master_config_t masterConfig;

    if ((srcClockHz == 0U) || (busHz == 0U))
    {
        return false;
    }

    s_Lpspi1SourceClockHz = srcClockHz;
    s_Lpspi1ConfiguredBaudHz = busHz;

    LPSPI_MasterGetDefaultConfig(&masterConfig);
    masterConfig.baudRate = busHz;
    masterConfig.bitsPerFrame = 8U;
    masterConfig.whichPcs = kLPSPI_Pcs0;
    masterConfig.cpol = kLPSPI_ClockPolarityActiveHigh;
    masterConfig.cpha = kLPSPI_ClockPhaseFirstEdge;
    masterConfig.direction = kLPSPI_MsbFirst;
    LPSPI_MasterInit(LPSPI1, &masterConfig, srcClockHz);
    LPSPI_SetDummyData(LPSPI1, 0xFFU);

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

/* 执行一次 SPI 全双工传输。 */
bool LPSPI1_Transfer(const uint8_t *txData, uint8_t *rxData, size_t length)
{
    lpspi_transfer_t transfer;
    uint32_t timeout;
    SDK_ALIGN(uint8_t txStaging[LPSPI1_RX_STAGING_SIZE], FSL_FEATURE_L1DCACHE_LINESIZE_BYTE);
    SDK_ALIGN(uint8_t rxStaging[LPSPI1_RX_STAGING_SIZE], FSL_FEATURE_L1DCACHE_LINESIZE_BYTE);

    if ((s_Lpspi1Ready == false) || (length == 0U))
    {
        return false;
    }
    if (length > LPSPI1_RX_STAGING_SIZE)
    {
        return false;
    }

    if (txData != NULL)
    {
        (void)memcpy(txStaging, txData, length);
    }
    else
    {
        (void)memset(txStaging, 0xFF, length);
    }
    (void)memset(rxStaging, 0, length);
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((uint32_t *)txStaging, (int32_t)length);
#endif

    transfer.txData = txStaging;
    transfer.rxData = (rxData != NULL) ? rxStaging : NULL;
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

    timeout = LPSPI1_TRANSFER_TIMEOUT;
    while ((!s_Lpspi1TransferDone) && (timeout > 0U))
    {
        timeout--;
    }

    LPSPI1_Unselect();
    if (!(s_Lpspi1TransferDone && (s_Lpspi1TransferStatus == kStatus_Success)))
    {
        return false;
    }

    if (rxData != NULL)
    {
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
        SCB_InvalidateDCache_by_Addr((uint32_t *)rxStaging, (int32_t)length);
#endif
        (void)memcpy(rxData, rxStaging, length);
    }

    return true;
}

/* 查询 LPSPI1 源时钟。 */
uint32_t LPSPI1_GetSourceClockHz(void)
{
    return s_Lpspi1SourceClockHz;
}

/* 查询当前配置的波特率。 */
uint32_t LPSPI1_GetConfiguredBaudHz(void)
{
    return s_Lpspi1ConfiguredBaudHz;
}

/* DMA 中断入口。 */
void DMA0_DMA16_IRQHandler(void)
{
    EDMA_HandleIRQ(&s_Lpspi1EdmaRxHandle);
    __DSB();
}

/* DMA 中断入口。 */
void DMA1_DMA17_IRQHandler(void)
{
    EDMA_HandleIRQ(&s_Lpspi1EdmaTxHandle);
    __DSB();
}
