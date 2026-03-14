#include "canfd1_ext_spi.h"

#include <string.h>

#include "clock_config.h"
#include "drv_canfdspi_api.h"
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "lpspi1_bus.h"

#define MCP2517FD_ADDR_C1CON (0x000U)
#define MCP2517FD_ADDR_C1NBTCFG (0x004U)
#define MCP2517FD_ADDR_C1DBTCFG (0x008U)
#define MCP2517FD_ADDR_C1VEC (0x018U)
#define MCP2517FD_ADDR_C1INT (0x01CU)
#define MCP2517FD_ADDR_C1TXREQ (0x030U)
#define MCP2517FD_ADDR_C1TREC (0x034U)
#define MCP2517FD_ADDR_C1BDIAG1 (0x03CU)
#define MCP2517FD_ADDR_C1TEFCON (0x040U)
#define MCP2517FD_ADDR_C1TEFSTA (0x044U)
#define MCP2517FD_ADDR_C1TEFUA (0x048U)
#define MCP2517FD_ADDR_C1TXQCON (0x050U)
#define MCP2517FD_ADDR_C1TXQSTA (0x054U)
#define MCP2517FD_ADDR_C1TXQUA (0x058U)
#define MCP2517FD_ADDR_C1FIFOCON1 (0x05CU)
#define MCP2517FD_ADDR_C1FIFOSTA1 (0x060U)
#define MCP2517FD_ADDR_C1FIFOUA1 (0x064U)
#define MCP2517FD_ADDR_C1FLTCON0 (0x1D0U)
#define MCP2517FD_ADDR_C1FLTOBJ0 (0x1F0U)
#define MCP2517FD_ADDR_C1MASK0 (0x1F4U)
#define MCP2517FD_RAM_ADDR_BASE (0x400U)

#define MCP2517FD_C1CON_REQOP_MASK (0x07U)
#define MCP2517FD_C1CON_OPMOD_MASK (0xE0U)
#define MCP2517FD_C1CON_TXQEN_MASK (0x10U)
#define MCP2517FD_C1CON_STEF_MASK (0x08U)
#define MCP2517FD_C1CON_BRSDIS_MASK (0x10U)

#define MCP2517FD_MODULE_ID (0U)
#define MCP2517FD_TX_QUEUE_CHANNEL (CAN_TXQUEUE_CH0)
#define MCP2517FD_RX_FIFO_CHANNEL (CAN_FIFO_CH1)
#define MCP2517FD_ACCEPT_ALL_FILTER (CAN_FILTER0)

#define MCP2517FD_CAN_CLK_HZ (40000000U)
#define MCP2517FD_T1_DLC_MASK (0x0FU)
#define MCP2517FD_T1_BRS_MASK (0x40U)
#define MCP2517FD_T1_FDF_MASK (0x80U)
#define MCP2517FD_T1_SEQ_SHIFT (9U)

#define MCP2517FD_TXQCON_TXEN_MASK (0x80U)
#define MCP2517FD_TXQCON_UINC_MASK (0x01U)
#define MCP2517FD_TXQCON_TXREQ_MASK (0x02U)
#define MCP2517FD_TXQCON_FRESET_MASK (0x04U)
#define MCP2517FD_TXQSTA_NOT_FULL_MASK (0x01U)
#define MCP2517FD_TXQCON_PLSIZE_SHIFT (29U)
#define MCP2517FD_TXQSTA_TXABT_MASK (0x80U)
#define MCP2517FD_TXQSTA_TXLARB_MASK (0x40U)
#define MCP2517FD_TXQSTA_TXERR_MASK (0x20U)
#define MCP2517FD_TXQSTA_TXATIF_MASK (0x10U)
#define MCP2517FD_TXQSTA_EMPTY_MASK (0x04U)

#define MCP2517FD_TEFCON_TEFTSEN_MASK (0x20U)
#define MCP2517FD_TEFCON_FRESET_MASK (0x04U)
#define MCP2517FD_TEFCON_UINC_MASK (0x01U)
#define MCP2517FD_TEFSTA_TEFNEIF_MASK (0x01U)
#define MCP2517FD_TEF_OBJECT_SIZE (12U)

#define MCP2517FD_FIFO_RXTSEN_MASK (0x20U)
#define MCP2517FD_FIFO_FRESET_MASK (0x04U)
#define MCP2517FD_FIFO_UINC_MASK (0x01U)
#define MCP2517FD_FIFOSTA_RXOVIF_MASK (0x08U)
#define MCP2517FD_FIFOSTA_TFNRFNIF_MASK (0x01U)
#define MCP2517FD_RX_OBJECT_HEADER_SIZE (12U)

#define MCP2517FD_TREC_TXBO_MASK (1UL << 21U)
#define MCP2517FD_TREC_TXBP_MASK (1UL << 20U)
#define MCP2517FD_TREC_RXBP_MASK (1UL << 19U)

#define MCP2517FD_C1INT_IVMIF_MASK (1UL << 15U)
#define MCP2517FD_C1INT_CERRIF_MASK (1UL << 13U)
#define MCP2517FD_C1INT_SERRIF_MASK (1UL << 12U)
#define MCP2517FD_C1INT_TXATIF_MASK (1UL << 10U)
#define MCP2517FD_C1INT_TEFIF_MASK (1UL << 4U)
#define MCP2517FD_C1INT_MODIF_MASK (1UL << 3U)
#define MCP2517FD_C1INT_RXIF_MASK (1UL << 1U)
#define MCP2517FD_C1INT_TXIF_MASK (1UL << 0U)
#define MCP2517FD_C1INT_STATUS_FLAGS_MASK                                                                                 \
    (MCP2517FD_C1INT_IVMIF_MASK | MCP2517FD_C1INT_CERRIF_MASK | MCP2517FD_C1INT_SERRIF_MASK |                            \
     MCP2517FD_C1INT_TXATIF_MASK | MCP2517FD_C1INT_TEFIF_MASK | MCP2517FD_C1INT_MODIF_MASK |                             \
     MCP2517FD_C1INT_RXIF_MASK | MCP2517FD_C1INT_TXIF_MASK)

#define MCP2517FD_BDIAG1_DLCMM_MASK (1UL << 31U)
#define MCP2517FD_BDIAG1_DCRCERR_MASK (1UL << 29U)
#define MCP2517FD_BDIAG1_DSTUFERR_MASK (1UL << 28U)
#define MCP2517FD_BDIAG1_DFORMERR_MASK (1UL << 27U)
#define MCP2517FD_BDIAG1_DBIT1ERR_MASK (1UL << 25U)
#define MCP2517FD_BDIAG1_DBIT0ERR_MASK (1UL << 24U)
#define MCP2517FD_BDIAG1_TXBOERR_MASK (1UL << 23U)
#define MCP2517FD_BDIAG1_NCRCERR_MASK (1UL << 21U)
#define MCP2517FD_BDIAG1_NSTUFERR_MASK (1UL << 20U)
#define MCP2517FD_BDIAG1_NFORMERR_MASK (1UL << 19U)
#define MCP2517FD_BDIAG1_NACKERR_MASK (1UL << 18U)
#define MCP2517FD_BDIAG1_NBIT1ERR_MASK (1UL << 17U)
#define MCP2517FD_BDIAG1_NBIT0ERR_MASK (1UL << 16U)

#define MCP2517FD_MSG_OBJ_HEADER_SIZE (8U)
#define MCP2517FD_MSG_OBJ_DATA_SIZE (64U)
#define MCP2517FD_MSG_OBJ_SIZE (MCP2517FD_MSG_OBJ_HEADER_SIZE + MCP2517FD_MSG_OBJ_DATA_SIZE)
#define MCP2517FD_RX_OBJECT_SIZE (MCP2517FD_RX_OBJECT_HEADER_SIZE + MCP2517FD_MSG_OBJ_DATA_SIZE)
#define MCP2517FD_SPI_TRANSFER_MAX (MCP2517FD_RX_OBJECT_SIZE)
#define MCP2517FD_TX_WAIT_RETRY (20000U)
#define MCP2517FD_TX_SW_QUEUE_DEPTH (32U)
#define MCP2517FD_RX_SW_QUEUE_DEPTH (32U)
#define MCP2517FD_EVENT_QUEUE_DEPTH (32U)
#define MCP2517FD_SPI_BAUD_HZ (20000000U)
#define MCP2517FD_RESET_DELAY_US (1000U)
#define MCP2517FD_TX_STALL_DEBUG_POLLS (200U)

/* 外置 CANFD 控制器软件状态与缓冲。 */
static bool s_ExtCanReady;
static uint8_t s_TxSequence;
static can_frame_t s_TxQueue[MCP2517FD_TX_SW_QUEUE_DEPTH];
static volatile uint8_t s_TxQHead;
static volatile uint8_t s_TxQTail;
static can_frame_t s_RxQueue[MCP2517FD_RX_SW_QUEUE_DEPTH];
static volatile uint8_t s_RxQHead;
static volatile uint8_t s_RxQTail;
static can_bus_event_t s_EventQueue[MCP2517FD_EVENT_QUEUE_DEPTH];
static volatile uint8_t s_EventQHead;
static volatile uint8_t s_EventQTail;
static uint32_t s_TxDropCount;
static uint32_t s_TxHwFailCount;
static uint32_t s_RxDropCount;
static can_channel_config_t s_ActiveConfig;
static uint8_t s_ActivePayloadSize = 8U;
static bool s_BitRateSwitchEnabled;
static bool s_TxInFlight;
static can_frame_t s_TxInFlightFrame;
static uint32_t s_LastTxDiagBits;
static uint16_t s_TxPendingDebugPolls;
static uint32_t s_LastTxPendingDebugSignature;
static uint16_t s_LastTxObjAddr;
static uint8_t s_LastTxObjSize;
static uint8_t s_LastTxObjBytes[16];

static bool MCP2517FD_Read8(uint16_t addr, uint8_t *value);
static bool MCP2517FD_Read32(uint16_t addr, uint32_t *value);
static bool MCP2517FD_SpiRead(uint16_t addr, uint8_t *data, size_t len);
static bool MCP2517FD_Write32(uint16_t addr, uint32_t value);
static bool MCP2517FD_EventPush(const can_bus_event_t *event);
static uint8_t MCP2517FD_DlcToLength(uint8_t dlc, bool fdFrame);
static uint8_t MCP2517FD_MapDiagToError(uint32_t txqsta, uint32_t bdiag1, uint32_t trec);
static bool MCP2517FD_FrameUsesFd(const can_frame_t *frame);
static void MCP2517FD_ClearTxDiagnostics(void);
static void MCP2517FD_DumpTxObjectSnapshot(void);
static void MCP2517FD_DumpTxPendingState(uint8_t txqsta,
                                         uint8_t tefsta,
                                         uint32_t c1int,
                                         uint32_t c1vec,
                                         uint32_t txreq,
                                         uint32_t txqcon,
                                         uint32_t tefcon,
                                         uint32_t txqua,
                                         uint32_t tefua,
                                         uint32_t bdiag1,
                                         uint32_t trec);
static bool MCP2517FD_PayloadSizeToEnum(uint8_t payloadSizeBytes, CAN_FIFO_PLSIZE *payloadSize);
static void MCP2517FD_SetMsgId(CAN_MSGOBJ_ID *msgId, uint32_t id, bool extended);
static uint32_t MCP2517FD_GetMsgId(const CAN_MSGOBJ_ID *msgId, bool extended);
static void MCP2517FD_FrameToTxObject(const can_frame_t *frame, bool fdFrame, CAN_TX_MSGOBJ *txObj);
static void MCP2517FD_RxObjectToFrame(const CAN_RX_MSGOBJ *rxObj, const uint8_t *payload, can_frame_t *frame);
static void MCP2517FD_TefObjectToFrame(const CAN_TEF_MSGOBJ *tefObj, can_frame_t *frame);
static bool MCP2517FD_ConfigureControllerFeatures(bool enableBrs);
static uint32_t MCP2517FD_BusDiagToBits(const CAN_BUS_DIAGNOSTIC *diag);
static uint32_t MCP2517FD_ErrorStateToTrecBits(CAN_ERROR_STATE errorState);

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

static void MCP2517FD_DumpTxPendingState(uint8_t txqsta,
                                         uint8_t tefsta,
                                         uint32_t c1int,
                                         uint32_t c1vec,
                                         uint32_t txreq,
                                         uint32_t txqcon,
                                         uint32_t tefcon,
                                         uint32_t txqua,
                                         uint32_t tefua,
                                         uint32_t bdiag1,
                                         uint32_t trec)
{
    PRINTF("CANFD1 tx stall: txqsta=%02X tefsta=%02X c1int=%08X c1vec=%08X txreq=%08X txqcon=%08X tefcon=%08X txqua=%08X tefua=%08X bdiag1=%08X trec=%08X\r\n",
           (unsigned int)txqsta,
           (unsigned int)tefsta,
           (unsigned int)c1int,
           (unsigned int)c1vec,
           (unsigned int)txreq,
           (unsigned int)txqcon,
           (unsigned int)tefcon,
           (unsigned int)txqua,
           (unsigned int)tefua,
           (unsigned int)bdiag1,
           (unsigned int)trec);
}

static void MCP2517FD_ClearTxDiagnostics(void)
{
    (void)DRV_CANFDSPI_ModuleEventClear(MCP2517FD_MODULE_ID,
                                        CAN_TX_EVENT | CAN_TEF_EVENT | CAN_TX_ATTEMPTS_EVENT | CAN_RX_OVERFLOW_EVENT |
                                            CAN_SYSTEM_ERROR_EVENT | CAN_BUS_ERROR_EVENT | CAN_RX_INVALID_MESSAGE_EVENT);
    (void)DRV_CANFDSPI_TransmitChannelEventAttemptClear(MCP2517FD_MODULE_ID, MCP2517FD_TX_QUEUE_CHANNEL);
    (void)DRV_CANFDSPI_ReceiveChannelEventOverflowClear(MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL);
    (void)DRV_CANFDSPI_TefEventOverflowClear(MCP2517FD_MODULE_ID);
    (void)DRV_CANFDSPI_BusDiagnosticsClear(MCP2517FD_MODULE_ID);
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

static void MCP2517FD_DumpTxObjectSnapshot(void)
{
    uint8_t readback[16];

    if ((s_LastTxObjSize == 0U) || (s_LastTxObjSize > sizeof(readback)))
    {
        return;
    }
    if (!MCP2517FD_SpiRead(s_LastTxObjAddr, readback, s_LastTxObjSize))
    {
        return;
    }

    PRINTF("CANFD1 tx obj addr=%03X size=%u wr=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X rd=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
           (unsigned int)s_LastTxObjAddr,
           (unsigned int)s_LastTxObjSize,
           (unsigned int)s_LastTxObjBytes[0],
           (unsigned int)s_LastTxObjBytes[1],
           (unsigned int)s_LastTxObjBytes[2],
           (unsigned int)s_LastTxObjBytes[3],
           (unsigned int)s_LastTxObjBytes[4],
           (unsigned int)s_LastTxObjBytes[5],
           (unsigned int)s_LastTxObjBytes[6],
           (unsigned int)s_LastTxObjBytes[7],
           (unsigned int)s_LastTxObjBytes[8],
           (unsigned int)s_LastTxObjBytes[9],
           (unsigned int)s_LastTxObjBytes[10],
           (unsigned int)s_LastTxObjBytes[11],
           (unsigned int)s_LastTxObjBytes[12],
           (unsigned int)s_LastTxObjBytes[13],
           (unsigned int)s_LastTxObjBytes[14],
           (unsigned int)s_LastTxObjBytes[15],
           (unsigned int)readback[0],
           (unsigned int)readback[1],
           (unsigned int)readback[2],
           (unsigned int)readback[3],
           (unsigned int)readback[4],
           (unsigned int)readback[5],
           (unsigned int)readback[6],
           (unsigned int)readback[7],
           (unsigned int)readback[8],
           (unsigned int)readback[9],
           (unsigned int)readback[10],
           (unsigned int)readback[11],
           (unsigned int)readback[12],
           (unsigned int)readback[13],
           (unsigned int)readback[14],
           (unsigned int)readback[15]);
}

static uint8_t MCP2517FD_LengthToDlc(uint8_t length, bool fdFrame)
{
    if (!fdFrame)
    {
        return (length <= 8U) ? length : 0xFFU;
    }

    switch (length)
    {
        case 0U:
        case 1U:
        case 2U:
        case 3U:
        case 4U:
        case 5U:
        case 6U:
        case 7U:
        case 8U:
            return length;
        case 12U:
            return 9U;
        case 16U:
            return 10U;
        case 20U:
            return 11U;
        case 24U:
            return 12U;
        case 32U:
            return 13U;
        case 48U:
            return 14U;
        case 64U:
            return 15U;
        default:
            return 0xFFU;
    }
}

static uint8_t MCP2517FD_DlcToLength(uint8_t dlc, bool fdFrame)
{
    if (!fdFrame)
    {
        return (dlc <= 8U) ? dlc : 8U;
    }

    switch (dlc)
    {
        case 0U:
        case 1U:
        case 2U:
        case 3U:
        case 4U:
        case 5U:
        case 6U:
        case 7U:
        case 8U:
            return dlc;
        case 9U:
            return 12U;
        case 10U:
            return 16U;
        case 11U:
            return 20U;
        case 12U:
            return 24U;
        case 13U:
            return 32U;
        case 14U:
            return 48U;
        case 15U:
            return 64U;
        default:
            return 0U;
    }
}

static bool MCP2517FD_PayloadSizeToEnum(uint8_t payloadSizeBytes, CAN_FIFO_PLSIZE *payloadSize)
{
    if (payloadSize == NULL)
    {
        return false;
    }

    switch (payloadSizeBytes)
    {
        case 8U:
            *payloadSize = CAN_PLSIZE_8;
            return true;
        case 12U:
            *payloadSize = CAN_PLSIZE_12;
            return true;
        case 16U:
            *payloadSize = CAN_PLSIZE_16;
            return true;
        case 20U:
            *payloadSize = CAN_PLSIZE_20;
            return true;
        case 24U:
            *payloadSize = CAN_PLSIZE_24;
            return true;
        case 32U:
            *payloadSize = CAN_PLSIZE_32;
            return true;
        case 48U:
            *payloadSize = CAN_PLSIZE_48;
            return true;
        case 64U:
            *payloadSize = CAN_PLSIZE_64;
            return true;
        default:
            return false;
    }
}

static void MCP2517FD_SetMsgId(CAN_MSGOBJ_ID *msgId, uint32_t id, bool extended)
{
    if (msgId == NULL)
    {
        return;
    }

    (void)memset(msgId, 0, sizeof(*msgId));
    if (extended)
    {
        msgId->SID = (id >> 18U) & 0x7FFU;
        msgId->EID = id & 0x3FFFFU;
    }
    else
    {
        msgId->SID = id & 0x7FFU;
    }
}

static uint32_t MCP2517FD_GetMsgId(const CAN_MSGOBJ_ID *msgId, bool extended)
{
    if (msgId == NULL)
    {
        return 0U;
    }

    if (!extended)
    {
        return msgId->SID & 0x7FFU;
    }

    return ((msgId->SID & 0x7FFU) << 18U) | (msgId->EID & 0x3FFFFU);
}

static void MCP2517FD_FrameToTxObject(const can_frame_t *frame, bool fdFrame, CAN_TX_MSGOBJ *txObj)
{
    bool extended;

    if ((frame == NULL) || (txObj == NULL))
    {
        return;
    }

    (void)memset(txObj, 0, sizeof(*txObj));
    extended = (frame->id > 0x7FFU);
    MCP2517FD_SetMsgId(&txObj->bF.id, frame->id, extended);
    txObj->bF.ctrl.DLC = (uint32_t)DRV_CANFDSPI_DataBytesToDlc(frame->dlc);
    txObj->bF.ctrl.IDE = extended ? 1U : 0U;
    txObj->bF.ctrl.RTR = 0U;
    txObj->bF.ctrl.BRS = (fdFrame && s_BitRateSwitchEnabled) ? 1U : 0U;
    txObj->bF.ctrl.FDF = fdFrame ? 1U : 0U;
    txObj->bF.ctrl.ESI = 0U;
    txObj->bF.ctrl.SEQ = s_TxSequence & 0x7FU;
    txObj->bF.timeStamp = 0U;
}

static void MCP2517FD_RxObjectToFrame(const CAN_RX_MSGOBJ *rxObj, const uint8_t *payload, can_frame_t *frame)
{
    bool fdFrame;
    bool extended;
    uint8_t length;

    if ((rxObj == NULL) || (frame == NULL))
    {
        return;
    }

    fdFrame = (rxObj->bF.ctrl.FDF != 0U);
    extended = (rxObj->bF.ctrl.IDE != 0U);
    length = MCP2517FD_DlcToLength((uint8_t)rxObj->bF.ctrl.DLC, fdFrame);

    (void)memset(frame, 0, sizeof(*frame));
    frame->id = MCP2517FD_GetMsgId(&rxObj->bF.id, extended);
    frame->dlc = length;
    frame->flags = fdFrame ? 0x01U : 0U;
    if ((payload != NULL) && (length > 0U))
    {
        (void)memcpy(frame->data, payload, length);
    }
}

static void MCP2517FD_TefObjectToFrame(const CAN_TEF_MSGOBJ *tefObj, can_frame_t *frame)
{
    bool fdFrame;
    bool extended;

    if ((tefObj == NULL) || (frame == NULL))
    {
        return;
    }

    fdFrame = (tefObj->bF.ctrl.FDF != 0U);
    extended = (tefObj->bF.ctrl.IDE != 0U);

    (void)memset(frame, 0, sizeof(*frame));
    frame->id = MCP2517FD_GetMsgId(&tefObj->bF.id, extended);
    frame->dlc = MCP2517FD_DlcToLength((uint8_t)tefObj->bF.ctrl.DLC, fdFrame);
    frame->flags = fdFrame ? 0x01U : 0U;
}

static bool MCP2517FD_EventPush(const can_bus_event_t *event)
{
    uint8_t nextHead;
    uint32_t primask;

    if (event == NULL)
    {
        return false;
    }

    primask = EnterCritical();
    nextHead = (uint8_t)((s_EventQHead + 1U) % MCP2517FD_EVENT_QUEUE_DEPTH);
    if (nextHead == s_EventQTail)
    {
        ExitCritical(primask);
        return false;
    }

    s_EventQueue[s_EventQHead] = *event;
    s_EventQHead = nextHead;
    ExitCritical(primask);
    return true;
}

static uint32_t MCP2517FD_BusDiagToBits(const CAN_BUS_DIAGNOSTIC *diag)
{
    if (diag == NULL)
    {
        return 0U;
    }

    return diag->word[1];
}

static uint32_t MCP2517FD_ErrorStateToTrecBits(CAN_ERROR_STATE errorState)
{
    uint32_t trec = 0U;

    if ((errorState & CAN_TX_BUS_OFF_STATE) != 0U)
    {
        trec |= MCP2517FD_TREC_TXBO_MASK;
    }
    if ((errorState & CAN_TX_BUS_PASSIVE_STATE) != 0U)
    {
        trec |= MCP2517FD_TREC_TXBP_MASK;
    }
    if ((errorState & CAN_RX_BUS_PASSIVE_STATE) != 0U)
    {
        trec |= MCP2517FD_TREC_RXBP_MASK;
    }

    return trec;
}

static uint8_t MCP2517FD_MapDiagToError(uint32_t txqsta, uint32_t bdiag1, uint32_t trec)
{
    if ((bdiag1 & MCP2517FD_BDIAG1_NACKERR_MASK) != 0U)
    {
        return 0x05U;
    }
    if (((bdiag1 & MCP2517FD_BDIAG1_NBIT0ERR_MASK) != 0U) || ((bdiag1 & MCP2517FD_BDIAG1_NBIT1ERR_MASK) != 0U) ||
        ((bdiag1 & MCP2517FD_BDIAG1_DBIT0ERR_MASK) != 0U) || ((bdiag1 & MCP2517FD_BDIAG1_DBIT1ERR_MASK) != 0U))
    {
        return 0x01U;
    }
    if (((bdiag1 & MCP2517FD_BDIAG1_NSTUFERR_MASK) != 0U) || ((bdiag1 & MCP2517FD_BDIAG1_DSTUFERR_MASK) != 0U))
    {
        return 0x02U;
    }
    if (((bdiag1 & MCP2517FD_BDIAG1_NCRCERR_MASK) != 0U) || ((bdiag1 & MCP2517FD_BDIAG1_DCRCERR_MASK) != 0U))
    {
        return 0x03U;
    }
    if (((bdiag1 & MCP2517FD_BDIAG1_NFORMERR_MASK) != 0U) || ((bdiag1 & MCP2517FD_BDIAG1_DFORMERR_MASK) != 0U))
    {
        return 0x04U;
    }
    if ((txqsta & MCP2517FD_TXQSTA_TXLARB_MASK) != 0U)
    {
        return 0x08U;
    }
    if ((txqsta & MCP2517FD_TXQSTA_TXABT_MASK) != 0U)
    {
        return 0x08U;
    }
    if (((trec & MCP2517FD_TREC_TXBP_MASK) != 0U) || ((trec & MCP2517FD_TREC_RXBP_MASK) != 0U))
    {
        return 0x07U;
    }
    if (((trec & MCP2517FD_TREC_TXBO_MASK) != 0U) || ((bdiag1 & MCP2517FD_BDIAG1_TXBOERR_MASK) != 0U))
    {
        return 0x06U;
    }

    return 0U;
}

static bool MCP2517FD_CalcBitTiming(uint32_t bitrate,
                                    uint16_t samplePointPermille,
                                    uint32_t maxTqs,
                                    uint32_t maxBrp,
                                    uint32_t *timingReg)
{
    uint32_t brp;
    uint32_t bestError = 0xFFFFFFFFU;
    uint32_t bestReg = 0U;

    if ((bitrate == 0U) || (timingReg == NULL) || (maxTqs < 4U) || (maxBrp == 0U))
    {
        return false;
    }

    for (brp = 1U; brp <= maxBrp; brp++)
    {
        uint32_t tqs = MCP2517FD_CAN_CLK_HZ / (bitrate * brp);
        uint32_t actualBitrate;
        uint32_t sampleTq;
        uint32_t tseg1;
        uint32_t tseg2;
        uint32_t sampleError;

        if ((tqs < 4U) || (tqs > maxTqs))
        {
            continue;
        }

        actualBitrate = MCP2517FD_CAN_CLK_HZ / (brp * tqs);
        if (actualBitrate != bitrate)
        {
            continue;
        }

        sampleTq = (tqs * samplePointPermille + 500U) / 1000U;
        if ((sampleTq < 2U) || (sampleTq >= tqs))
        {
            continue;
        }

        tseg1 = sampleTq - 1U;
        tseg2 = tqs - sampleTq;
        if ((tseg1 == 0U) || (tseg1 > 256U) || (tseg2 == 0U) || (tseg2 > 128U))
        {
            continue;
        }

        sampleError = (uint32_t)((int32_t)(samplePointPermille - (uint16_t)((sampleTq * 1000U) / tqs)));
        if (sampleError > 1000U)
        {
            sampleError = 1000U - sampleError;
        }

        if (sampleError <= bestError)
        {
            bestError = sampleError;
            bestReg = ((brp - 1U) << 24U) | ((tseg1 - 1U) << 16U) | ((tseg2 - 1U) << 8U) |
                      (((tseg2 > 16U) ? 16U : tseg2) - 1U);
            if (sampleError == 0U)
            {
                break;
            }
        }
    }

    if (bestError == 0xFFFFFFFFU)
    {
        return false;
    }

    *timingReg = bestReg;
    return true;
}

static __attribute__((unused)) bool MCP2517FD_RxQueuePush(const can_frame_t *frame)
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
    if ((data == NULL) || (len == 0U) || (len > MCP2517FD_SPI_TRANSFER_MAX))
    {
        return false;
    }

    return (DRV_CANFDSPI_ReadByteArray(MCP2517FD_MODULE_ID, addr, data, (uint16_t)len) == 0);
}

static bool MCP2517FD_SpiWrite(uint16_t addr, const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U) || (len > MCP2517FD_SPI_TRANSFER_MAX))
    {
        return false;
    }

    return (DRV_CANFDSPI_WriteByteArray(MCP2517FD_MODULE_ID, addr, (uint8_t *)(uintptr_t)data, (uint16_t)len) == 0);
}

static bool MCP2517FD_Read8(uint16_t addr, uint8_t *value)
{
    if (value == NULL)
    {
        return false;
    }

    return (DRV_CANFDSPI_ReadByte(MCP2517FD_MODULE_ID, addr, value) == 0);
}

static bool MCP2517FD_Write8(uint16_t addr, uint8_t value)
{
    return (DRV_CANFDSPI_WriteByte(MCP2517FD_MODULE_ID, addr, value) == 0);
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
    if (value == NULL)
    {
        return false;
    }

    return (DRV_CANFDSPI_ReadWord(MCP2517FD_MODULE_ID, addr, value) == 0);
}

static bool MCP2517FD_Write32(uint16_t addr, uint32_t value)
{
    return (DRV_CANFDSPI_WriteWord(MCP2517FD_MODULE_ID, addr, value) == 0);
}

static bool MCP2517FD_FrameUsesFd(const can_frame_t *frame)
{
    return ((frame != NULL) && ((frame->flags & 0x01U) != 0U));
}

static uint16_t MCP2517FD_RamAddrFromUa(uint32_t ua)
{
    return (uint16_t)(MCP2517FD_RAM_ADDR_BASE + (ua & 0x07FFU));
}

static uint8_t MCP2517FD_GetActiveTxObjectSize(void)
{
    return (uint8_t)(MCP2517FD_MSG_OBJ_HEADER_SIZE + s_ActivePayloadSize);
}

static void MCP2517FD_DebugVerifyTxObjectWrite(uint16_t addr, const uint8_t *expected, uint8_t len)
{
    (void)addr;
    (void)expected;
    (void)len;
}

static bool CANFD1_ExtControllerReset(void)
{
    return (DRV_CANFDSPI_Reset(MCP2517FD_MODULE_ID) == 0);
}

static bool MCP2517FD_RequestMode(CAN_OPERATION_MODE reqMode)
{
    uint32_t retry;

    if (DRV_CANFDSPI_OperationModeSelect(MCP2517FD_MODULE_ID, reqMode) != 0)
    {
        return false;
    }

    /* 请求后轮询 OPMOD，确保控制器已稳定切换。 */
    for (retry = 0; retry < 10000U; retry++)
    {
        if (DRV_CANFDSPI_OperationModeGet(MCP2517FD_MODULE_ID) == reqMode)
        {
            return true;
        }
    }

    return false;
}

static bool MCP2517FD_IsMode(CAN_OPERATION_MODE expectedMode)
{
    return (DRV_CANFDSPI_OperationModeGet(MCP2517FD_MODULE_ID) == expectedMode);
}

static __attribute__((unused)) bool MCP2517FD_ConfigureBitrate500K(void)
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

static __attribute__((unused)) bool MCP2517FD_ConfigureTxQueue(void)
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

static bool MCP2517FD_ConfigureControllerFeatures(bool enableBrs)
{
    CAN_CONFIG config;

    if (DRV_CANFDSPI_ConfigureObjectReset(&config) != 0)
    {
        return false;
    }

    config.StoreInTEF = 1U;
    config.TXQEnable = 1U;
    config.BitRateSwitchDisable = enableBrs ? 0U : 1U;
    config.RestrictReTxAttempts = 0U;
    config.TxBandWidthSharing = CAN_TXBWS_NO_DELAY;

    return (DRV_CANFDSPI_Configure(MCP2517FD_MODULE_ID, &config) == 0);
}

static bool MCP2517FD_ConfigureBitTimingEx(const can_channel_config_t *config)
{
    uint32_t nbtcfg;
    uint32_t dbtcfg;
    uint32_t dataBitrate;
    uint16_t dataSamplePoint;

    if (config == NULL)
    {
        return false;
    }

    if (!MCP2517FD_CalcBitTiming(config->nominalBitrate, config->nominalSamplePointPermille, 256U, 256U, &nbtcfg))
    {
        return false;
    }

    dataBitrate = (config->frameFormat == kCanFrameFormat_Fd) ? config->dataBitrate : config->nominalBitrate;
    dataSamplePoint =
        (config->frameFormat == kCanFrameFormat_Fd) ? config->dataSamplePointPermille : config->nominalSamplePointPermille;
    if (!MCP2517FD_CalcBitTiming(dataBitrate, dataSamplePoint, 32U, 256U, &dbtcfg))
    {
        return false;
    }

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

static bool MCP2517FD_ConfigureTxQueueEx(uint8_t payloadSizeBytes)
{
    CAN_TX_QUEUE_CONFIG config;
    CAN_FIFO_PLSIZE payloadSize;

    if (!MCP2517FD_PayloadSizeToEnum(payloadSizeBytes, &payloadSize))
    {
        return false;
    }
    if (DRV_CANFDSPI_TransmitQueueConfigureObjectReset(&config) != 0)
    {
        return false;
    }

    config.TxPriority = 1U;
    config.FifoSize = 0U;
    config.PayLoadSize = payloadSize;

    return (DRV_CANFDSPI_TransmitQueueConfigure(MCP2517FD_MODULE_ID, &config) == 0) &&
           (DRV_CANFDSPI_TransmitChannelReset(MCP2517FD_MODULE_ID, MCP2517FD_TX_QUEUE_CHANNEL) == 0);
}

static bool MCP2517FD_ConfigureTxEventFifo(void)
{
    CAN_TEF_CONFIG config;

    if (DRV_CANFDSPI_TefConfigureObjectReset(&config) != 0)
    {
        return false;
    }

    config.TimeStampEnable = 1U;
    config.FifoSize = 0U;

    return (DRV_CANFDSPI_TefConfigure(MCP2517FD_MODULE_ID, &config) == 0) &&
           (DRV_CANFDSPI_TefReset(MCP2517FD_MODULE_ID) == 0);
}

static bool MCP2517FD_ConfigureRxFifo(uint8_t payloadSizeBytes)
{
    CAN_RX_FIFO_CONFIG config;
    CAN_FIFO_PLSIZE payloadSize;

    if (!MCP2517FD_PayloadSizeToEnum(payloadSizeBytes, &payloadSize))
    {
        return false;
    }
    if (DRV_CANFDSPI_ReceiveChannelConfigureObjectReset(&config) != 0)
    {
        return false;
    }

    config.RxTimeStampEnable = 1U;
    config.FifoSize = 0U;
    config.PayLoadSize = payloadSize;

    return (DRV_CANFDSPI_ReceiveChannelConfigure(MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL, &config) == 0) &&
           (DRV_CANFDSPI_ReceiveChannelReset(MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL) == 0);
}

static bool MCP2517FD_ConfigureAcceptAllFilter(void)
{
    CAN_FILTEROBJ_ID filterObject = {0};
    CAN_MASKOBJ_ID maskObject = {0};

    return (DRV_CANFDSPI_FilterDisable(MCP2517FD_MODULE_ID, MCP2517FD_ACCEPT_ALL_FILTER) == 0) &&
           (DRV_CANFDSPI_FilterObjectConfigure(MCP2517FD_MODULE_ID, MCP2517FD_ACCEPT_ALL_FILTER, &filterObject) == 0) &&
           (DRV_CANFDSPI_FilterMaskConfigure(MCP2517FD_MODULE_ID, MCP2517FD_ACCEPT_ALL_FILTER, &maskObject) == 0) &&
           (DRV_CANFDSPI_FilterToFifoLink(
                MCP2517FD_MODULE_ID, MCP2517FD_ACCEPT_ALL_FILTER, MCP2517FD_RX_FIFO_CHANNEL, true) == 0);
}

static bool MCP2517FD_ApplyConfigInternal(const can_channel_config_t *config)
{
    if (config == NULL)
    {
        return false;
    }

    if (!MCP2517FD_RequestMode(CAN_CONFIGURATION_MODE))
    {
        return false;
    }
    if (!MCP2517FD_ConfigureBitTimingEx(config))
    {
        return false;
    }

    if (!MCP2517FD_ConfigureTxEventFifo())
    {
        return false;
    }

    s_ActivePayloadSize = (config->frameFormat == kCanFrameFormat_Fd) ? 64U : 8U;
    if (!MCP2517FD_ConfigureTxQueueEx(s_ActivePayloadSize))
    {
        return false;
    }
    if (!MCP2517FD_ConfigureRxFifo(s_ActivePayloadSize))
    {
        return false;
    }
    if (!MCP2517FD_ConfigureAcceptAllFilter())
    {
        return false;
    }

    s_BitRateSwitchEnabled =
        (config->frameFormat == kCanFrameFormat_Fd) && (config->dataBitrate > 0U) && (config->dataBitrate != config->nominalBitrate);
    if (!MCP2517FD_ConfigureControllerFeatures(s_BitRateSwitchEnabled))
    {
        return false;
    }
    if (!MCP2517FD_RequestMode(CAN_NORMAL_MODE))
    {
        return false;
    }

    s_TxQHead = 0U;
    s_TxQTail = 0U;
    s_RxQHead = 0U;
    s_RxQTail = 0U;
    s_EventQHead = 0U;
    s_EventQTail = 0U;
    s_TxInFlight = false;
    s_LastTxDiagBits = 0U;
    s_TxPendingDebugPolls = 0U;
    s_LastTxPendingDebugSignature = 0U;
    s_LastTxObjAddr = 0U;
    s_LastTxObjSize = 0U;
    (void)memset(s_LastTxObjBytes, 0, sizeof(s_LastTxObjBytes));
    (void)memset(&s_TxInFlightFrame, 0, sizeof(s_TxInFlightFrame));
    s_ActiveConfig = *config;
    MCP2517FD_ClearTxDiagnostics();
    return true;
}

static bool MCP2517FD_WaitTxQueueNotFull(void)
{
    uint32_t retry;
    CAN_TX_FIFO_STATUS status;

    for (retry = 0U; retry < MCP2517FD_TX_WAIT_RETRY; retry++)
    {
        if (DRV_CANFDSPI_TransmitChannelStatusGet(MCP2517FD_MODULE_ID, MCP2517FD_TX_QUEUE_CHANNEL, &status) != 0)
        {
            return false;
        }

        if ((status & CAN_TX_FIFO_NOT_FULL) != 0U)
        {
            return true;
        }
    }

    return false;
}

static void MCP2517FD_AbortTxQueue(void)
{
    (void)DRV_CANFDSPI_TransmitChannelReset(MCP2517FD_MODULE_ID, MCP2517FD_TX_QUEUE_CHANNEL);
    MCP2517FD_ClearTxDiagnostics();
}

bool CANFD1_ExtSpiInit(void)
{
    const uint32_t lpspiRootHz = BOARD_BOOTCLOCKRUN_LPSPI_CLK_ROOT;
    can_channel_config_t defaultConfig;

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

    s_TxSequence = 0U;
    s_TxQHead = 0U;
    s_TxQTail = 0U;
    s_RxQHead = 0U;
    s_RxQTail = 0U;
    s_TxDropCount = 0U;
    s_TxHwFailCount = 0U;
    s_RxDropCount = 0U;
    s_ActivePayloadSize = 8U;
    s_BitRateSwitchEnabled = false;
    (void)memset(&s_ActiveConfig, 0, sizeof(s_ActiveConfig));

    defaultConfig.enabled = true;
    defaultConfig.terminationEnabled = false;
    defaultConfig.frameFormat = kCanFrameFormat_Fd;
    defaultConfig.nominalBitrate = 500000U;
    defaultConfig.nominalSamplePointPermille = 800U;
    defaultConfig.dataBitrate = 2000000U;
    defaultConfig.dataSamplePointPermille = 750U;
    if (!MCP2517FD_ApplyConfigInternal(&defaultConfig))
    {
        s_ExtCanReady = false;
        MCP2517FD_DumpBringUpState("apply-default-fail");
        return false;
    }

    s_ExtCanReady = true;
    MCP2517FD_DumpBringUpState("ready");
    PRINTF("CANFD1(MCP2517FD) ready; lpspi_src=%u spi=%u payload=%u brs=%u\r\n",
           LPSPI1_GetSourceClockHz(),
           LPSPI1_GetConfiguredBaudHz(),
           s_ActivePayloadSize,
           s_BitRateSwitchEnabled ? 1U : 0U);
    return true;
}

bool CANFD1_ExtSpiApplyConfig(const can_channel_config_t *config)
{
    if (!s_ExtCanReady || (config == NULL))
    {
        return false;
    }

    return MCP2517FD_ApplyConfigInternal(config);
}

static bool MCP2517FD_TransmitFrame(const can_frame_t *frame)
{
    CAN_TX_MSGOBJ txObj;
    bool fdFrame;

    if (frame == NULL)
    {
        return false;
    }
    if (!MCP2517FD_IsMode(CAN_NORMAL_MODE))
    {
        return false;
    }
    if (!MCP2517FD_WaitTxQueueNotFull())
    {
        return false;
    }

    fdFrame = MCP2517FD_FrameUsesFd(frame);
    if (fdFrame && (s_ActiveConfig.frameFormat != kCanFrameFormat_Fd))
    {
        return false;
    }
    if ((!fdFrame && (frame->dlc > 8U)) || (frame->dlc > s_ActivePayloadSize))
    {
        return false;
    }
    MCP2517FD_ClearTxDiagnostics();

    MCP2517FD_FrameToTxObject(frame, fdFrame, &txObj);
    if (DRV_CANFDSPI_TransmitChannelLoad(MCP2517FD_MODULE_ID,
                                         MCP2517FD_TX_QUEUE_CHANNEL,
                                         &txObj,
                                         (uint8_t *)(uintptr_t)frame->data,
                                         frame->dlc,
                                         true) != 0)
    {
        return false;
    }

    s_LastTxObjAddr = 0U;
    s_LastTxObjSize = 0U;
    (void)memset(s_LastTxObjBytes, 0, sizeof(s_LastTxObjBytes));
    s_TxInFlight = true;
    s_TxInFlightFrame = *frame;
    s_LastTxDiagBits = 0U;
    s_TxPendingDebugPolls = 0U;
    s_LastTxPendingDebugSignature = 0U;
    s_TxSequence++;
    return true;
}

static __attribute__((unused)) bool MCP2517FD_TransmitFrameLegacy(const can_frame_t *frame)
{
    uint32_t txUa;
    uint8_t msgObj[MCP2517FD_MSG_OBJ_SIZE];
    uint32_t t0;
    uint32_t t1;
    uint8_t txObjSize;

    if ((frame == NULL) || (frame->dlc > 8U))
    {
        return false;
    }
    if (!MCP2517FD_IsMode(CAN_NORMAL_MODE))
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
    txObjSize = MCP2517FD_GetActiveTxObjectSize();

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
    (void)memset(&msgObj[MCP2517FD_MSG_OBJ_HEADER_SIZE], 0, s_ActivePayloadSize);
    if (frame->dlc > 0U)
    {
        (void)memcpy(&msgObj[MCP2517FD_MSG_OBJ_HEADER_SIZE], frame->data, frame->dlc);
    }

    if (!MCP2517FD_SpiWrite(MCP2517FD_RamAddrFromUa(txUa), msgObj, txObjSize))
    {
        return false;
    }

    MCP2517FD_DebugVerifyTxObjectWrite(MCP2517FD_RamAddrFromUa(txUa), msgObj, s_LastTxObjSize);
    if (!MCP2517FD_Write8(MCP2517FD_ADDR_C1TXQCON + 1U, MCP2517FD_TXQCON_UINC_MASK))
    {
        return false;
    }
    if (!MCP2517FD_Write8(MCP2517FD_ADDR_C1TXQCON + 1U, MCP2517FD_TXQCON_TXREQ_MASK))
    {
        return false;
    }

    s_TxSequence++;
    return true;
}

static void MCP2517FD_PollTxEvents(void)
{
    CAN_TEF_FIFO_EVENT tefEvents = CAN_TEF_FIFO_NO_EVENT;
    CAN_TX_FIFO_STATUS txStatus = CAN_TX_FIFO_FULL;
    CAN_MODULE_EVENT moduleEvents = CAN_NO_EVENT;
    CAN_BUS_DIAGNOSTIC busDiag;
    CAN_ERROR_STATE errorState = CAN_ERROR_FREE_STATE;
    uint8_t tec = 0U;
    uint8_t rec = 0U;
    uint32_t txreq = 0U;
    uint32_t bdiag1 = 0U;
    uint32_t trec = 0U;
    bool txComplete = false;

    if (!s_ExtCanReady)
    {
        return;
    }

    (void)memset(&busDiag, 0, sizeof(busDiag));
    if ((DRV_CANFDSPI_TefEventGet(MCP2517FD_MODULE_ID, &tefEvents) == 0) && ((tefEvents & CAN_TEF_FIFO_NOT_EMPTY_EVENT) != 0U))
    {
        CAN_TEF_FIFO_STATUS tefStatus;
        CAN_TEF_MSGOBJ tefObj;
        can_bus_event_t event;

        if ((DRV_CANFDSPI_TefStatusGet(MCP2517FD_MODULE_ID, &tefStatus) == 0) && ((tefStatus & CAN_TEF_FIFO_NOT_EMPTY) != 0U) &&
            (DRV_CANFDSPI_TefMessageGet(MCP2517FD_MODULE_ID, &tefObj) == 0))
        {
            (void)memset(&event, 0, sizeof(event));
            event.type = kCanBusEvent_TxComplete;
            event.isTx = 1U;
            MCP2517FD_TefObjectToFrame(&tefObj, &event.frame);
            if (s_TxInFlight && (s_TxInFlightFrame.id == event.frame.id))
            {
                event.frame = s_TxInFlightFrame;
            }
            (void)MCP2517FD_EventPush(&event);
            s_TxInFlight = false;
            s_TxPendingDebugPolls = 0U;
            s_LastTxPendingDebugSignature = 0U;
            txComplete = true;
        }
    }

    if ((DRV_CANFDSPI_TransmitChannelStatusGet(MCP2517FD_MODULE_ID, MCP2517FD_TX_QUEUE_CHANNEL, &txStatus) != 0) ||
        (DRV_CANFDSPI_TransmitRequestGet(MCP2517FD_MODULE_ID, &txreq) != 0) ||
        (DRV_CANFDSPI_BusDiagnosticsGet(MCP2517FD_MODULE_ID, &busDiag) != 0) ||
        (DRV_CANFDSPI_ErrorCountStateGet(MCP2517FD_MODULE_ID, &tec, &rec, &errorState) != 0) ||
        (DRV_CANFDSPI_ModuleEventGet(MCP2517FD_MODULE_ID, &moduleEvents) != 0))
    {
        return;
    }

    bdiag1 = MCP2517FD_BusDiagToBits(&busDiag);
    trec = MCP2517FD_ErrorStateToTrecBits(errorState);

    {
        uint32_t diagBits = ((uint32_t)txStatus << 24U) | (bdiag1 & 0xBFBF0000UL) |
                            (trec & (MCP2517FD_TREC_TXBO_MASK | MCP2517FD_TREC_TXBP_MASK | MCP2517FD_TREC_RXBP_MASK));
        uint8_t errorCode = MCP2517FD_MapDiagToError((uint32_t)txStatus, bdiag1, trec);

        if ((errorCode != 0U) && (diagBits != s_LastTxDiagBits))
        {
            can_bus_event_t event;

            (void)memset(&event, 0, sizeof(event));
            event.type = kCanBusEvent_Error;
            event.errorCode = errorCode;
            event.isTx = s_TxInFlight ? 1U : 0U;
            event.rawStatus = diagBits;
            if (s_TxInFlight)
            {
                event.frame = s_TxInFlightFrame;
            }
            (void)MCP2517FD_EventPush(&event);
            PRINTF("CANFD1 err=0x%02X txqsta=%02X bdiag1=%08X trec=%08X txInFlight=%u raw=%08X\r\n",
                   errorCode,
                   (unsigned int)txStatus,
                   (unsigned int)bdiag1,
                   (unsigned int)trec,
                   s_TxInFlight ? 1U : 0U,
                   (unsigned int)diagBits);

            if (s_TxInFlight)
            {
                MCP2517FD_AbortTxQueue();
                s_TxInFlight = false;
                s_TxPendingDebugPolls = 0U;
                s_LastTxPendingDebugSignature = 0U;
            }
        }
        s_LastTxDiagBits = diagBits;

        if (s_TxInFlight && !txComplete && (errorCode == 0U) && ((bdiag1 & MCP2517FD_BDIAG1_DLCMM_MASK) == 0U) &&
            ((txreq & (uint32_t)MCP2517FD_TX_QUEUE_CHANNEL) == 0U) && ((txStatus & CAN_TX_FIFO_EMPTY) != 0U))
        {
            if ((moduleEvents & (CAN_SYSTEM_ERROR_EVENT | CAN_BUS_ERROR_EVENT | CAN_TX_ATTEMPTS_EVENT | CAN_TEF_EVENT |
                                 CAN_TX_EVENT | CAN_RX_OVERFLOW_EVENT | CAN_RX_INVALID_MESSAGE_EVENT)) == 0U)
            {
                can_bus_event_t event;

                (void)memset(&event, 0, sizeof(event));
                event.type = kCanBusEvent_TxComplete;
                event.isTx = 1U;
                event.frame = s_TxInFlightFrame;
                (void)MCP2517FD_EventPush(&event);
                s_TxInFlight = false;
                s_TxPendingDebugPolls = 0U;
                s_LastTxPendingDebugSignature = 0U;
                txComplete = true;
                PRINTF("CANFD1 tx complete via txreq fallback: txqsta=%02X txreq=%08X bdiag1=%08X trec=%08X\r\n",
                       (unsigned int)txStatus,
                       (unsigned int)txreq,
                       (unsigned int)bdiag1,
                       (unsigned int)trec);
            }
        }

        if (s_TxInFlight && (errorCode == 0U) && !txComplete)
        {
            uint32_t c1int = (uint32_t)moduleEvents;
            uint32_t c1vec = 0U;
            uint32_t txqcon = 0U;
            uint32_t tefcon = 0U;
            uint32_t txqua = 0U;
            uint32_t tefua = 0U;
            uint32_t signature;

            if (s_TxPendingDebugPolls < 0xFFFFU)
            {
                s_TxPendingDebugPolls++;
            }
            if ((s_TxPendingDebugPolls >= MCP2517FD_TX_STALL_DEBUG_POLLS) &&
                MCP2517FD_Read32(MCP2517FD_ADDR_C1VEC, &c1vec) &&
                MCP2517FD_Read32(MCP2517FD_ADDR_C1TXQCON, &txqcon) &&
                MCP2517FD_Read32(MCP2517FD_ADDR_C1TEFCON, &tefcon) &&
                MCP2517FD_Read32(MCP2517FD_ADDR_C1TXQUA, &txqua) &&
                MCP2517FD_Read32(MCP2517FD_ADDR_C1TEFUA, &tefua))
            {
                signature = (c1int ^ c1vec ^ txreq ^ txqcon ^ tefcon ^ txqua ^ tefua ^ bdiag1 ^ trec ^
                             ((uint32_t)txStatus << 8U) ^ (uint32_t)tefEvents);
                if (signature != s_LastTxPendingDebugSignature)
                {
                    MCP2517FD_DumpTxPendingState((uint8_t)txStatus, (uint8_t)tefEvents, c1int, c1vec, txreq, txqcon, tefcon,
                                                 txqua, tefua, bdiag1,
                                                 trec);
                    if (((moduleEvents & CAN_RX_INVALID_MESSAGE_EVENT) != 0U) || ((bdiag1 & MCP2517FD_BDIAG1_DLCMM_MASK) != 0U))
                    {
                        MCP2517FD_DumpTxObjectSnapshot();
                    }
                    s_LastTxPendingDebugSignature = signature;
                }
            }
        }
        else
        {
            s_TxPendingDebugPolls = 0U;
            s_LastTxPendingDebugSignature = 0U;
        }
    }
}

static void MCP2517FD_PollRxFifo(void)
{
    CAN_RX_FIFO_EVENT rxEvents = CAN_RX_FIFO_NO_EVENT;
    CAN_RX_FIFO_STATUS rxStatus = CAN_RX_FIFO_EMPTY;

    if (!s_ExtCanReady)
    {
        return;
    }
    if ((DRV_CANFDSPI_ReceiveChannelEventGet(MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL, &rxEvents) != 0) ||
        (DRV_CANFDSPI_ReceiveChannelStatusGet(MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL, &rxStatus) != 0))
    {
        return;
    }

    if ((rxEvents & CAN_RX_FIFO_OVERFLOW_EVENT) != 0U)
    {
        can_bus_event_t event;

        s_RxDropCount++;
        (void)memset(&event, 0, sizeof(event));
        event.type = kCanBusEvent_Error;
        event.errorCode = 0x04U;
        event.isTx = 0U;
        event.rawStatus = (uint32_t)rxEvents;
        (void)MCP2517FD_EventPush(&event);
        (void)DRV_CANFDSPI_ReceiveChannelEventOverflowClear(MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL);
    }

    while ((rxStatus & CAN_RX_FIFO_NOT_EMPTY) != 0U)
    {
        CAN_RX_MSGOBJ rxObj;
        uint8_t payload[MCP2517FD_MSG_OBJ_DATA_SIZE];
        can_frame_t frame;
        can_bus_event_t event;
        uint32_t primask;
        uint8_t nextHead;

        if ((DRV_CANFDSPI_ReceiveChannelStatusGet(MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL, &rxStatus) != 0) ||
            ((rxStatus & CAN_RX_FIFO_NOT_EMPTY) == 0U) ||
            (DRV_CANFDSPI_ReceiveMessageGet(
                 MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL, &rxObj, payload, s_ActivePayloadSize) != 0))
        {
            break;
        }

        MCP2517FD_RxObjectToFrame(&rxObj, payload, &frame);

        primask = EnterCritical();
        nextHead = (uint8_t)((s_RxQHead + 1U) % MCP2517FD_RX_SW_QUEUE_DEPTH);
        if (nextHead == s_RxQTail)
        {
            s_RxDropCount++;
            ExitCritical(primask);
        }
        else
        {
            s_RxQueue[s_RxQHead] = frame;
            s_RxQHead = nextHead;
            ExitCritical(primask);
        }

        (void)memset(&event, 0, sizeof(event));
        event.type = kCanBusEvent_RxFrame;
        event.frame = frame;
        event.isTx = 0U;
        (void)MCP2517FD_EventPush(&event);

        if (DRV_CANFDSPI_ReceiveChannelStatusGet(MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL, &rxStatus) != 0)
        {
            break;
        }
    }
}

void CANFD1_ExtSpiTask(void)
{
    can_frame_t frame;
    uint8_t tail;
    uint32_t primask;
    bool hasFrame = false;

    if (!s_ExtCanReady)
    {
        return;
    }

    MCP2517FD_PollTxEvents();
    MCP2517FD_PollRxFifo();

    if (s_TxInFlight)
    {
        return;
    }

    /* 后台任务从软件发送队列取帧并驱动硬件发送。 */
    primask = EnterCritical();
    if (s_TxQHead != s_TxQTail)
    {
        tail = s_TxQTail;
        frame = s_TxQueue[tail];
        hasFrame = true;
    }
    ExitCritical(primask);

    if (!hasFrame)
    {
        return;
    }

    if (!MCP2517FD_TransmitFrame(&frame))
    {
        s_TxHwFailCount++;
        if ((s_TxHwFailCount % 100U) == 1U)
        {
            PRINTF("CANFD1 tx hw fail=%u\r\n", s_TxHwFailCount);
        }
        return;
    }

    primask = EnterCritical();
    if (s_TxQHead != s_TxQTail)
    {
        s_TxQTail = (uint8_t)((s_TxQTail + 1U) % MCP2517FD_TX_SW_QUEUE_DEPTH);
    }
    ExitCritical(primask);
}

bool CANFD1_ExtSpiSend(const can_frame_t *frame)
{
    uint8_t nextHead;
    uint32_t primask;
    bool fdFrame;

    if ((s_ExtCanReady == false) || (frame == NULL))
    {
        return false;
    }

    fdFrame = MCP2517FD_FrameUsesFd(frame);
    if (fdFrame && (s_ActiveConfig.frameFormat != kCanFrameFormat_Fd))
    {
        return false;
    }
    if ((!fdFrame && (frame->dlc > 8U)) || (frame->dlc > s_ActivePayloadSize) || (MCP2517FD_LengthToDlc(frame->dlc, fdFrame) == 0xFFU))
    {
        return false;
    }

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

static __attribute__((unused)) bool CANFD1_ExtSpiSendLegacyUnused(const can_frame_t *frame)
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

bool CANFD1_ExtSpiPollEvent(can_bus_event_t *event)
{
    uint32_t primask;

    if ((event == NULL) || (!s_ExtCanReady))
    {
        return false;
    }

    primask = EnterCritical();
    if (s_EventQHead == s_EventQTail)
    {
        ExitCritical(primask);
        return false;
    }

    *event = s_EventQueue[s_EventQTail];
    s_EventQTail = (uint8_t)((s_EventQTail + 1U) % MCP2517FD_EVENT_QUEUE_DEPTH);
    ExitCritical(primask);
    return true;
}

bool CANFD1_ExtSpiGetRuntimeState(can_driver_runtime_state_t *state)
{
    CAN_TX_FIFO_STATUS txStatus = CAN_TX_FIFO_FULL;
    CAN_RX_FIFO_STATUS rxStatus = CAN_RX_FIFO_EMPTY;
    CAN_BUS_DIAGNOSTIC busDiag;
    CAN_ERROR_STATE errorState = CAN_ERROR_FREE_STATE;
    uint8_t tec = 0U;
    uint8_t rec = 0U;
    uint32_t bdiag1 = 0U;
    uint32_t trec = 0U;

    if ((state == NULL) || (!s_ExtCanReady))
    {
        return false;
    }

    (void)memset(&busDiag, 0, sizeof(busDiag));
    (void)DRV_CANFDSPI_TransmitChannelStatusGet(MCP2517FD_MODULE_ID, MCP2517FD_TX_QUEUE_CHANNEL, &txStatus);
    (void)DRV_CANFDSPI_ReceiveChannelStatusGet(MCP2517FD_MODULE_ID, MCP2517FD_RX_FIFO_CHANNEL, &rxStatus);
    (void)DRV_CANFDSPI_BusDiagnosticsGet(MCP2517FD_MODULE_ID, &busDiag);
    (void)DRV_CANFDSPI_ErrorCountStateGet(MCP2517FD_MODULE_ID, &tec, &rec, &errorState);
    bdiag1 = MCP2517FD_BusDiagToBits(&busDiag);
    trec = MCP2517FD_ErrorStateToTrecBits(errorState);

    (void)memset(state, 0, sizeof(*state));
    state->busOff = ((trec & MCP2517FD_TREC_TXBO_MASK) != 0U) ? 1U : 0U;
    state->errorPassive = (((trec & MCP2517FD_TREC_TXBP_MASK) != 0U) || ((trec & MCP2517FD_TREC_RXBP_MASK) != 0U)) ? 1U : 0U;
    state->rxPending = ((s_RxQHead != s_RxQTail) || ((rxStatus & CAN_RX_FIFO_NOT_EMPTY) != 0U)) ? 1U : 0U;
    state->txPending = ((s_TxQHead != s_TxQTail) || s_TxInFlight) ? 1U : 0U;
    state->lastErrorCode = MCP2517FD_MapDiagToError((uint32_t)txStatus, bdiag1, trec);
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
