#include "canfd1_ext_spi.h"

#include <string.h>

#include "clock_config.h"
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "lpspi1_bus.h"

#define MCP2517FD_SPI_CMD_RESET (0x0U)
#define MCP2517FD_SPI_CMD_WRITE (0x2U)
#define MCP2517FD_SPI_CMD_READ (0x3U)

#define MCP2517FD_ADDR_C1CON (0x000U)
#define MCP2517FD_ADDR_C1NBTCFG (0x004U)
#define MCP2517FD_ADDR_C1DBTCFG (0x008U)
#define MCP2517FD_ADDR_C1TXQCON (0x050U)
#define MCP2517FD_ADDR_C1TXQSTA (0x054U)
#define MCP2517FD_ADDR_C1TXQUA (0x058U)

#define MCP2517FD_C1CON_REQOP_MASK (0x07U)
#define MCP2517FD_C1CON_OPMOD_MASK (0xE0U)
#define MCP2517FD_C1CON_TXQEN_MASK (0x10U)
#define MCP2517FD_C1CON_BRSDIS_MASK (0x10U)

#define MCP2517FD_MODE_NORMAL (0x0U)
#define MCP2517FD_MODE_CONFIG (0x4U)

#define MCP2517FD_TXQCON_TXEN_MASK (0x80U)
#define MCP2517FD_TXQCON_UINC_TXREQ_MASK (0x03U)
#define MCP2517FD_TXQCON_FRESET_MASK (0x04U)
#define MCP2517FD_TXQSTA_NOT_FULL_MASK (0x01U)

#define MCP2517FD_MSG_OBJ_HEADER_SIZE (8U)
#define MCP2517FD_MSG_OBJ_DATA_SIZE (8U)
#define MCP2517FD_MSG_OBJ_SIZE (MCP2517FD_MSG_OBJ_HEADER_SIZE + MCP2517FD_MSG_OBJ_DATA_SIZE)
#define MCP2517FD_TX_WAIT_RETRY (20000U)
#define MCP2517FD_TX_SW_QUEUE_DEPTH (32U)
#define MCP2517FD_RX_SW_QUEUE_DEPTH (32U)
#define MCP2517FD_SPI_BAUD_HZ (10000000U)
#define MCP2517FD_RESET_DELAY_US (1000U)

/* 外置 CANFD 控制器软件状态与缓冲。 */
static bool s_ExtCanReady;
static uint8_t s_TxSequence;
static can_frame_t s_TxQueue[MCP2517FD_TX_SW_QUEUE_DEPTH];
static volatile uint8_t s_TxQHead;
static volatile uint8_t s_TxQTail;
static can_frame_t s_RxQueue[MCP2517FD_RX_SW_QUEUE_DEPTH];
static volatile uint8_t s_RxQHead;
static volatile uint8_t s_RxQTail;
static uint32_t s_TxDropCount;
static uint32_t s_TxHwFailCount;
static uint32_t s_RxDropCount;

static bool MCP2517FD_Read8(uint16_t addr, uint8_t *value);
static bool MCP2517FD_Read32(uint16_t addr, uint32_t *value);

static void MCP2517FD_DumpBringUpState(const char *stage)
{
    uint32_t c1con = 0U;
    uint32_t nbtcfg = 0U;
    uint32_t dbtcfg = 0U;
    uint32_t txqcon = 0U;
    uint8_t txqsta = 0U;

    (void)MCP2517FD_Read32(MCP2517FD_ADDR_C1CON, &c1con);
    (void)MCP2517FD_Read32(MCP2517FD_ADDR_C1NBTCFG, &nbtcfg);
    (void)MCP2517FD_Read32(MCP2517FD_ADDR_C1DBTCFG, &dbtcfg);
    (void)MCP2517FD_Read32(MCP2517FD_ADDR_C1TXQCON, &txqcon);
    (void)MCP2517FD_Read8(MCP2517FD_ADDR_C1TXQSTA, &txqsta);

    PRINTF("CANFD1 %s: LPSPI root=%u CR=%08X SR=%08X TCR=%08X CCR=%08X\r\n",
           stage,
           BOARD_BOOTCLOCKRUN_LPSPI_CLK_ROOT,
           LPSPI1->CR,
           LPSPI1->SR,
           LPSPI1->TCR,
           LPSPI1->CCR);
    PRINTF("CANFD1 %s: C1CON=%08X NBTCFG=%08X DBTCFG=%08X TXQCON=%08X TXQSTA=%02X\r\n",
           stage,
           c1con,
           nbtcfg,
           dbtcfg,
           txqcon,
           txqsta);
}

static uint32_t EnterCritical(void)
{
    /* 轻量临界区：保护 head/tail 更新。 */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void ExitCritical(uint32_t primask)
{
    __set_PRIMASK(primask);
}

static bool MCP2517FD_RxQueuePush(const can_frame_t *frame)
{
    uint8_t nextHead;
    uint32_t primask;

    if (frame == NULL)
    {
        return false;
    }

    /* 接收队列写入，由任务上下文使用。 */
    primask = EnterCritical();
    nextHead = (uint8_t)((s_RxQHead + 1U) % MCP2517FD_RX_SW_QUEUE_DEPTH);
    if (nextHead == s_RxQTail)
    {
        s_RxDropCount++;
        ExitCritical(primask);
        return false;
    }

    s_RxQueue[s_RxQHead] = *frame;
    s_RxQHead = nextHead;
    ExitCritical(primask);
    return true;
}

static bool MCP2517FD_SpiRead(uint16_t addr, uint8_t *data, size_t len)
{
    uint8_t tx[2U + MCP2517FD_MSG_OBJ_SIZE];
    uint8_t rx[2U + MCP2517FD_MSG_OBJ_SIZE];

    if ((data == NULL) || (len == 0U) || (len > MCP2517FD_MSG_OBJ_SIZE))
    {
        return false;
    }

    /* 指令格式：OP[7:4] + ADDR[11:8]，第二字节为 ADDR[7:0]。 */
    tx[0] = (uint8_t)((MCP2517FD_SPI_CMD_READ << 4U) | ((addr >> 8U) & 0x0FU));
    tx[1] = (uint8_t)(addr & 0xFFU);
    (void)memset(&tx[2], 0xFF, len);

    if (!LPSPI1_Transfer(tx, rx, len + 2U))
    {
        return false;
    }

    (void)memcpy(data, &rx[2], len);
    return true;
}

static bool MCP2517FD_SpiWrite(uint16_t addr, const uint8_t *data, size_t len)
{
    uint8_t tx[2U + MCP2517FD_MSG_OBJ_SIZE];

    if ((data == NULL) || (len == 0U) || (len > MCP2517FD_MSG_OBJ_SIZE))
    {
        return false;
    }

    tx[0] = (uint8_t)((MCP2517FD_SPI_CMD_WRITE << 4U) | ((addr >> 8U) & 0x0FU));
    tx[1] = (uint8_t)(addr & 0xFFU);
    (void)memcpy(&tx[2], data, len);

    return LPSPI1_Transfer(tx, NULL, len + 2U);
}

static bool MCP2517FD_Read8(uint16_t addr, uint8_t *value)
{
    return MCP2517FD_SpiRead(addr, value, 1U);
}

static bool MCP2517FD_Write8(uint16_t addr, uint8_t value)
{
    return MCP2517FD_SpiWrite(addr, &value, 1U);
}

static bool MCP2517FD_Modify8(uint16_t addr, uint8_t clearMask, uint8_t setMask)
{
    uint8_t value;
    if (!MCP2517FD_Read8(addr, &value))
    {
        return false;
    }

    value = (uint8_t)((value & (uint8_t)(~clearMask)) | setMask);
    return MCP2517FD_Write8(addr, value);
}

static bool MCP2517FD_Read32(uint16_t addr, uint32_t *value)
{
    uint8_t raw[4];

    if ((value == NULL) || !MCP2517FD_SpiRead(addr, raw, sizeof(raw)))
    {
        return false;
    }

    *value = ((uint32_t)raw[0]) | ((uint32_t)raw[1] << 8U) | ((uint32_t)raw[2] << 16U) | ((uint32_t)raw[3] << 24U);
    return true;
}

static bool MCP2517FD_Write32(uint16_t addr, uint32_t value)
{
    uint8_t raw[4];
    raw[0] = (uint8_t)(value & 0xFFU);
    raw[1] = (uint8_t)((value >> 8U) & 0xFFU);
    raw[2] = (uint8_t)((value >> 16U) & 0xFFU);
    raw[3] = (uint8_t)((value >> 24U) & 0xFFU);
    return MCP2517FD_SpiWrite(addr, raw, sizeof(raw));
}

static bool CANFD1_ExtControllerReset(void)
{
    uint8_t cmd[2] = {0x00U, 0x00U};
    return LPSPI1_Transfer(cmd, NULL, sizeof(cmd));
}

static bool MCP2517FD_RequestMode(uint8_t reqMode)
{
    uint8_t c1conByte2;
    uint32_t retry;

    if (!MCP2517FD_Modify8(MCP2517FD_ADDR_C1CON + 3U, MCP2517FD_C1CON_REQOP_MASK, (reqMode & MCP2517FD_C1CON_REQOP_MASK)))
    {
        return false;
    }

    /* 请求后轮询 OPMOD，确保控制器已稳定切换。 */
    for (retry = 0; retry < 10000U; retry++)
    {
        if (!MCP2517FD_Read8(MCP2517FD_ADDR_C1CON + 2U, &c1conByte2))
        {
            return false;
        }

        if (((c1conByte2 & MCP2517FD_C1CON_OPMOD_MASK) >> 5U) == (reqMode & MCP2517FD_C1CON_REQOP_MASK))
        {
            return true;
        }
    }

    return false;
}

static bool MCP2517FD_IsMode(uint8_t expectedMode)
{
    uint8_t c1conByte2;
    if (!MCP2517FD_Read8(MCP2517FD_ADDR_C1CON + 2U, &c1conByte2))
    {
        return false;
    }

    return (((c1conByte2 & MCP2517FD_C1CON_OPMOD_MASK) >> 5U) == (expectedMode & MCP2517FD_C1CON_REQOP_MASK));
}

static bool MCP2517FD_ConfigureBitrate500K(void)
{
    uint32_t nbtcfg;
    uint32_t dbtcfg;

    /* 名义位时序（示例参数）：
     * 500 kbps @ 40 MHz: BRP=4, TSEG1=14, TSEG2=5, SJW=4
     * Register encoding is (value - 1).
     */
    nbtcfg = (3U << 24U) | (13U << 16U) | (4U << 8U) | 3U;

    /* Data phase kept conservative for bring-up (1 Mbps, no BRS in this stage). */
    dbtcfg = (3U << 24U) | (8U << 16U) | (1U << 8U) | 1U;

    if (!MCP2517FD_Write32(MCP2517FD_ADDR_C1NBTCFG, nbtcfg))
    {
        return false;
    }

    if (!MCP2517FD_Write32(MCP2517FD_ADDR_C1DBTCFG, dbtcfg))
    {
        return false;
    }

    return true;
}

static bool MCP2517FD_ConfigureTxQueue(void)
{
    uint32_t txqcon;

    /* TXQ 基础配置：1 个元素、8 字节数据、发送优先级 1。 */
    txqcon = (1U << 16U) | MCP2517FD_TXQCON_TXEN_MASK;
    if (!MCP2517FD_Write32(MCP2517FD_ADDR_C1TXQCON, txqcon))
    {
        return false;
    }

    /* Reset TXQ pointers while still in Configuration mode. */
    if (!MCP2517FD_Write8(MCP2517FD_ADDR_C1TXQCON + 1U, MCP2517FD_TXQCON_FRESET_MASK))
    {
        return false;
    }

    if (!MCP2517FD_Modify8(MCP2517FD_ADDR_C1CON + 2U, 0U, MCP2517FD_C1CON_TXQEN_MASK))
    {
        return false;
    }

    return true;
}

static bool MCP2517FD_WaitTxQueueNotFull(void)
{
    uint32_t retry;
    uint8_t txqsta;

    for (retry = 0U; retry < MCP2517FD_TX_WAIT_RETRY; retry++)
    {
        if (!MCP2517FD_Read8(MCP2517FD_ADDR_C1TXQSTA, &txqsta))
        {
            return false;
        }

        if ((txqsta & MCP2517FD_TXQSTA_NOT_FULL_MASK) != 0U)
        {
            return true;
        }
    }

    return false;
}

bool CANFD1_ExtSpiInit(void)
{
    const uint32_t lpspiRootHz = BOARD_BOOTCLOCKRUN_LPSPI_CLK_ROOT;

    /* 先初始化 SPI 总线，再按“复位->配置态->参数配置->正常态”顺序启动控制器。 */
    if (!LPSPI1_BusInit(lpspiRootHz, MCP2517FD_SPI_BAUD_HZ))
    {
        s_ExtCanReady = false;
        return false;
    }

    if (!CANFD1_ExtControllerReset())
    {
        s_ExtCanReady = false;
        MCP2517FD_DumpBringUpState("reset-fail");
        return false;
    }
    SDK_DelayAtLeastUs(MCP2517FD_RESET_DELAY_US, SystemCoreClock);

    if (!MCP2517FD_RequestMode(MCP2517FD_MODE_CONFIG))
    {
        s_ExtCanReady = false;
        MCP2517FD_DumpBringUpState("config-mode-fail");
        return false;
    }

    if (!MCP2517FD_ConfigureBitrate500K())
    {
        s_ExtCanReady = false;
        MCP2517FD_DumpBringUpState("bitrate-fail");
        return false;
    }

    if (!MCP2517FD_ConfigureTxQueue())
    {
        s_ExtCanReady = false;
        MCP2517FD_DumpBringUpState("txq-fail");
        return false;
    }

    /* Keep initial bring-up in Classic CAN mode (no BRS). */
    if (!MCP2517FD_Modify8(MCP2517FD_ADDR_C1CON + 1U, 0U, MCP2517FD_C1CON_BRSDIS_MASK))
    {
        s_ExtCanReady = false;
        MCP2517FD_DumpBringUpState("brsdis-fail");
        return false;
    }

    if (!MCP2517FD_RequestMode(MCP2517FD_MODE_NORMAL))
    {
        s_ExtCanReady = false;
        MCP2517FD_DumpBringUpState("normal-mode-fail");
        return false;
    }

    s_TxSequence = 0U;
    s_TxQHead = 0U;
    s_TxQTail = 0U;
    s_RxQHead = 0U;
    s_RxQTail = 0U;
    s_TxDropCount = 0U;
    s_TxHwFailCount = 0U;
    s_RxDropCount = 0U;
    s_ExtCanReady = true;
    MCP2517FD_DumpBringUpState("ready");
    PRINTF("CANFD1(MCP2517FD) ready\r\n");
    return true;
}

static bool MCP2517FD_TransmitFrame(const can_frame_t *frame)
{
    uint32_t txUa;
    uint8_t msgObj[MCP2517FD_MSG_OBJ_SIZE];
    uint32_t t0;
    uint32_t t1;

    if ((frame == NULL) || (frame->dlc > 8U))
    {
        return false;
    }
    if (!MCP2517FD_IsMode(MCP2517FD_MODE_NORMAL))
    {
        return false;
    }
    if (!MCP2517FD_WaitTxQueueNotFull())
    {
        return false;
    }

    if (!MCP2517FD_Read32(MCP2517FD_ADDR_C1TXQUA, &txUa))
    {
        return false;
    }

    /* 当前先实现标准帧 + 8 字节以内数据，便于先打通链路。 */
    t0 = (frame->id & 0x7FFU);
    t1 = ((uint32_t)(s_TxSequence & 0x7FU) << 9U) | (uint32_t)(frame->dlc & 0x0FU);

    msgObj[0] = (uint8_t)(t0 & 0xFFU);
    msgObj[1] = (uint8_t)((t0 >> 8U) & 0xFFU);
    msgObj[2] = (uint8_t)((t0 >> 16U) & 0xFFU);
    msgObj[3] = (uint8_t)((t0 >> 24U) & 0xFFU);
    msgObj[4] = (uint8_t)(t1 & 0xFFU);
    msgObj[5] = (uint8_t)((t1 >> 8U) & 0xFFU);
    msgObj[6] = (uint8_t)((t1 >> 16U) & 0xFFU);
    msgObj[7] = (uint8_t)((t1 >> 24U) & 0xFFU);
    (void)memset(&msgObj[MCP2517FD_MSG_OBJ_HEADER_SIZE], 0, MCP2517FD_MSG_OBJ_DATA_SIZE);
    if (frame->dlc > 0U)
    {
        (void)memcpy(&msgObj[MCP2517FD_MSG_OBJ_HEADER_SIZE], frame->data, frame->dlc);
    }

    if (!MCP2517FD_SpiWrite((uint16_t)(txUa & 0x0FFFU), msgObj, sizeof(msgObj)))
    {
        return false;
    }

    if (!MCP2517FD_Write8(MCP2517FD_ADDR_C1TXQCON + 1U, MCP2517FD_TXQCON_UINC_TXREQ_MASK))
    {
        return false;
    }
    if (!MCP2517FD_WaitTxQueueNotFull())
    {
        return false;
    }

    s_TxSequence++;
    return true;
}

void CANFD1_ExtSpiTask(void)
{
    can_frame_t frame;
    uint8_t tail;
    uint32_t primask;

    if (!s_ExtCanReady)
    {
        return;
    }

    /* 后台任务从软件发送队列取帧并驱动硬件发送。 */
    primask = EnterCritical();
    if (s_TxQHead == s_TxQTail)
    {
        ExitCritical(primask);
        return;
    }
    tail = s_TxQTail;
    frame = s_TxQueue[tail];
    s_TxQTail = (uint8_t)((s_TxQTail + 1U) % MCP2517FD_TX_SW_QUEUE_DEPTH);
    ExitCritical(primask);

    if (!MCP2517FD_TransmitFrame(&frame))
    {
        s_TxHwFailCount++;
        if ((s_TxHwFailCount % 100U) == 1U)
        {
            PRINTF("CANFD1 tx hw fail=%u\r\n", s_TxHwFailCount);
        }
    }
}

bool CANFD1_ExtSpiSend(const can_frame_t *frame)
{
    uint8_t nextHead;
    uint32_t primask;

    if ((s_ExtCanReady == false) || (frame == NULL) || (frame->dlc > 8U))
    {
        return false;
    }

    /* 前台只负责入队，快速返回，避免调用方被 SPI 事务阻塞。 */
    primask = EnterCritical();
    nextHead = (uint8_t)((s_TxQHead + 1U) % MCP2517FD_TX_SW_QUEUE_DEPTH);
    if (nextHead == s_TxQTail)
    {
        s_TxDropCount++;
        ExitCritical(primask);
        if ((s_TxDropCount % 100U) == 1U)
        {
            PRINTF("CANFD1 tx drop=%u\r\n", s_TxDropCount);
        }
        return false;
    }

    s_TxQueue[s_TxQHead] = *frame;
    s_TxQHead = nextHead;
    ExitCritical(primask);
    return true;
}

bool CANFD1_ExtSpiReceive(can_frame_t *frame)
{
    uint8_t tail;
    uint32_t primask;

    if ((frame == NULL) || (!s_ExtCanReady))
    {
        return false;
    }

    primask = EnterCritical();
    if (s_RxQHead == s_RxQTail)
    {
        ExitCritical(primask);
        return false;
    }

    tail = s_RxQTail;
    *frame = s_RxQueue[tail];
    s_RxQTail = (uint8_t)((s_RxQTail + 1U) % MCP2517FD_RX_SW_QUEUE_DEPTH);
    ExitCritical(primask);
    return true;
}

void CANFD1_ExtSpiGetStats(canfd1_ext_stats_t *stats)
{
    uint32_t primask;

    if (stats == NULL)
    {
        return;
    }

    primask = EnterCritical();
    stats->txDropCount = s_TxDropCount;
    stats->txHwFailCount = s_TxHwFailCount;
    stats->rxDropCount = s_RxDropCount;
    ExitCritical(primask);
}
