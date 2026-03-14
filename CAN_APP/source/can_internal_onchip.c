#include "can_internal_onchip.h"

#include <string.h>

#include "can_bridge.h"
#include "can_stack.h"
#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_debug_console.h"
#include "fsl_flexcan.h"

#define ONCHIP_RX_QUEUE_DEPTH (16U)
#define ONCHIP_EVENT_QUEUE_DEPTH (32U)
#define ONCHIP_TX_MB_INDEX (8U)
#define ONCHIP_RX_STD_MB_INDEX (9U)
#define ONCHIP_RX_EXT_MB_INDEX (10U)

typedef struct
{
    can_channel_t channel;
    CAN_Type *base;
    bool supportsFd;
    bool ready;
    bool txPending;
    bool rxStdArmed;
    bool rxExtArmed;
    bool lastErrorLatched;
    can_channel_config_t activeConfig;
    can_driver_runtime_state_t runtimeState;
    can_frame_t lastTxFrame;
    can_frame_t rxQueue[ONCHIP_RX_QUEUE_DEPTH];
    uint8_t rxHead;
    uint8_t rxTail;
    can_bus_event_t eventQueue[ONCHIP_EVENT_QUEUE_DEPTH];
    uint8_t eventHead;
    uint8_t eventTail;
    flexcan_handle_t handle;
    flexcan_mb_transfer_t txTransfer;
    flexcan_mb_transfer_t rxStdTransfer;
    flexcan_mb_transfer_t rxExtTransfer;
    flexcan_frame_t txFrame;
    flexcan_frame_t rxStdFrame;
    flexcan_frame_t rxExtFrame;
    flexcan_fd_frame_t txFdFrame;
    flexcan_fd_frame_t rxStdFdFrame;
    flexcan_fd_frame_t rxExtFdFrame;
} onchip_channel_ctx_t;

static onchip_channel_ctx_t s_OnchipCtx[kCanChannel_Count] = {
    {0},
    {.channel = kCanChannel_Can2, .base = CAN3, .supportsFd = true},
    {.channel = kCanChannel_Can3, .base = CAN1, .supportsFd = false},
    {.channel = kCanChannel_Can4, .base = CAN2, .supportsFd = false},
};

static FLEXCAN_CALLBACK(OnchipFlexcanCallback);

static uint32_t OnchipEnterCritical(void)
{
    return DisableGlobalIRQ();
}

static void OnchipExitCritical(uint32_t primask)
{
    EnableGlobalIRQ(primask);
}

static bool OnchipIsManagedChannel(can_channel_t channel)
{
    return (channel == kCanChannel_Can2) || (channel == kCanChannel_Can3) || (channel == kCanChannel_Can4);
}

static bool OnchipUsesFdController(const onchip_channel_ctx_t *ctx)
{
    return (ctx != NULL) && ctx->supportsFd;
}

static onchip_channel_ctx_t *OnchipGetCtx(can_channel_t channel)
{
    if (!OnchipIsManagedChannel(channel))
    {
        return NULL;
    }

    return &s_OnchipCtx[channel];
}

static uint16_t OnchipComputeClassicSamplePointPermille(const flexcan_timing_config_t *timingConfig)
{
    uint32_t sampleTq;
    uint32_t totalTq;

    if (timingConfig == NULL)
    {
        return 0U;
    }

    sampleTq = 1U + ((uint32_t)timingConfig->propSeg + 1U) + ((uint32_t)timingConfig->phaseSeg1 + 1U);
    totalTq = sampleTq + ((uint32_t)timingConfig->phaseSeg2 + 1U);
    return (totalTq == 0U) ? 0U : (uint16_t)(((sampleTq * 1000U) + (totalTq / 2U)) / totalTq);
}

static uint16_t OnchipComputeFdDataSamplePointPermille(const flexcan_timing_config_t *timingConfig)
{
    uint32_t sampleTq;
    uint32_t totalTq;

    if (timingConfig == NULL)
    {
        return 0U;
    }

    sampleTq = 1U + (uint32_t)timingConfig->fpropSeg + ((uint32_t)timingConfig->fphaseSeg1 + 1U);
    totalTq = sampleTq + ((uint32_t)timingConfig->fphaseSeg2 + 1U);
    return (totalTq == 0U) ? 0U : (uint16_t)(((sampleTq * 1000U) + (totalTq / 2U)) / totalTq);
}

static bool OnchipBuildTimingConfig(onchip_channel_ctx_t *ctx,
                                    uint32_t sourceClockHz,
                                    const can_channel_config_t *requestedConfig,
                                    flexcan_timing_config_t *timingConfig,
                                    can_channel_config_t *appliedConfig)
{
    bool ok;

    if ((ctx == NULL) || (requestedConfig == NULL) || (timingConfig == NULL) || (appliedConfig == NULL))
    {
        return false;
    }
    *appliedConfig = *requestedConfig;
    (void)memset(timingConfig, 0, sizeof(*timingConfig));

    if (OnchipUsesFdController(ctx))
    {
        if (requestedConfig->frameFormat == kCanFrameFormat_Fd)
        {
            ok = FLEXCAN_FDCalculateImprovedTimingValues(
                ctx->base, requestedConfig->nominalBitrate, requestedConfig->dataBitrate, sourceClockHz, timingConfig);
            if (!ok)
            {
                PRINTF("Onchip timing fail CH%u FD n=%u d=%u src=%u\r\n",
                       (uint32_t)ctx->channel,
                       requestedConfig->nominalBitrate,
                       requestedConfig->dataBitrate,
                       sourceClockHz);
                return false;
            }
            appliedConfig->dataSamplePointPermille = OnchipComputeFdDataSamplePointPermille(timingConfig);
        }
        else
        {
            ok = FLEXCAN_CalculateImprovedTimingValues(ctx->base, requestedConfig->nominalBitrate, sourceClockHz, timingConfig);
            if (!ok)
            {
                PRINTF("Onchip timing fail CH%u classic-on-fd n=%u src=%u\r\n",
                       (uint32_t)ctx->channel,
                       requestedConfig->nominalBitrate,
                       sourceClockHz);
                return false;
            }
            timingConfig->fpreDivider = timingConfig->preDivider;
            timingConfig->fphaseSeg1 = timingConfig->phaseSeg1;
            timingConfig->fphaseSeg2 = timingConfig->phaseSeg2;
            timingConfig->fpropSeg = (timingConfig->propSeg + 1U);
            timingConfig->frJumpwidth = timingConfig->rJumpwidth;
            appliedConfig->dataBitrate = requestedConfig->nominalBitrate;
            appliedConfig->dataSamplePointPermille = 0U;
        }
    }
    else
    {
        ok = FLEXCAN_CalculateImprovedTimingValues(ctx->base, requestedConfig->nominalBitrate, sourceClockHz, timingConfig);
        if (!ok)
        {
            PRINTF("Onchip timing fail CH%u classic n=%u src=%u\r\n",
                   (uint32_t)ctx->channel,
                   requestedConfig->nominalBitrate,
                   sourceClockHz);
            return false;
        }
    }

    appliedConfig->nominalSamplePointPermille = OnchipComputeClassicSamplePointPermille(timingConfig);
    return true;
}

static bool OnchipQueueFramePush(onchip_channel_ctx_t *ctx, const can_frame_t *frame)
{
    uint8_t nextHead;

    if ((ctx == NULL) || (frame == NULL))
    {
        return false;
    }

    nextHead = (uint8_t)((ctx->rxHead + 1U) % ONCHIP_RX_QUEUE_DEPTH);
    if (nextHead == ctx->rxTail)
    {
        return false;
    }

    ctx->rxQueue[ctx->rxHead] = *frame;
    ctx->rxHead = nextHead;
    return true;
}

static bool OnchipQueueFramePop(onchip_channel_ctx_t *ctx, can_frame_t *frame)
{
    if ((ctx == NULL) || (frame == NULL) || (ctx->rxHead == ctx->rxTail))
    {
        return false;
    }

    *frame = ctx->rxQueue[ctx->rxTail];
    ctx->rxTail = (uint8_t)((ctx->rxTail + 1U) % ONCHIP_RX_QUEUE_DEPTH);
    return true;
}

static bool OnchipQueueEventPush(onchip_channel_ctx_t *ctx, const can_bus_event_t *event)
{
    uint8_t nextHead;

    if ((ctx == NULL) || (event == NULL))
    {
        return false;
    }

    nextHead = (uint8_t)((ctx->eventHead + 1U) % ONCHIP_EVENT_QUEUE_DEPTH);
    if (nextHead == ctx->eventTail)
    {
        return false;
    }

    ctx->eventQueue[ctx->eventHead] = *event;
    ctx->eventHead = nextHead;
    return true;
}

static bool OnchipQueueEventPop(onchip_channel_ctx_t *ctx, can_bus_event_t *event)
{
    if ((ctx == NULL) || (event == NULL) || (ctx->eventHead == ctx->eventTail))
    {
        return false;
    }

    *event = ctx->eventQueue[ctx->eventTail];
    ctx->eventTail = (uint8_t)((ctx->eventTail + 1U) % ONCHIP_EVENT_QUEUE_DEPTH);
    return true;
}

static void OnchipResetQueues(onchip_channel_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    ctx->rxHead = 0U;
    ctx->rxTail = 0U;
    ctx->eventHead = 0U;
    ctx->eventTail = 0U;
}

static uint8_t OnchipLengthToDlc(uint8_t length)
{
    if (length <= 8U)
    {
        return length;
    }
    if (length <= 12U)
    {
        return 9U;
    }
    if (length <= 16U)
    {
        return 10U;
    }
    if (length <= 20U)
    {
        return 11U;
    }
    if (length <= 24U)
    {
        return 12U;
    }
    if (length <= 32U)
    {
        return 13U;
    }
    if (length <= 48U)
    {
        return 14U;
    }

    return 15U;
}

static uint8_t OnchipDlcToLength(uint8_t dlc, bool fdFrame)
{
    if ((!fdFrame) || (dlc <= 8U))
    {
        return (dlc <= 8U) ? dlc : 8U;
    }

    return (uint8_t)DLC_LENGTH_DECODE(dlc);
}

static bool OnchipIsExtendedId(uint32_t id)
{
    return (id > 0x7FFU);
}

static uint32_t OnchipEncodeId(uint32_t id, bool extended)
{
    return extended ? FLEXCAN_ID_EXT(id) : FLEXCAN_ID_STD(id);
}

static uint32_t OnchipDecodeId(uint32_t rawId, bool extended)
{
    return extended ? (rawId & 0x1FFFFFFFU) : ((rawId >> CAN_ID_STD_SHIFT) & 0x7FFU);
}

static uint32_t OnchipPackWord(const uint8_t *src, uint8_t length, uint8_t offset)
{
    uint32_t value = 0U;
    uint8_t i;

    for (i = 0U; i < 4U; i++)
    {
        uint8_t index = (uint8_t)(offset + i);

        if (index < length)
        {
            value |= ((uint32_t)src[index]) << (8U * (3U - i));
        }
    }

    return value;
}

static void OnchipUnpackWords(uint8_t *dst, uint8_t length, const uint32_t *srcWords, uint8_t wordCount)
{
    uint8_t i;

    if ((dst == NULL) || (srcWords == NULL))
    {
        return;
    }

    for (i = 0U; i < length; i++)
    {
        uint8_t wordIndex = (uint8_t)(i / 4U);

        if (wordIndex >= wordCount)
        {
            break;
        }

        dst[i] = (uint8_t)((srcWords[wordIndex] >> (8U * (3U - (i % 4U)))) & 0xFFU);
    }
}

static bool OnchipFrameIsFdPayload(const can_frame_t *frame)
{
    return (frame != NULL) && ((((frame->flags & CAN_BRIDGE_FLAG_CANFD) != 0U) || (frame->dlc > 8U)));
}

static void OnchipFrameToClassic(const can_frame_t *src, flexcan_frame_t *dst)
{
    bool extended;

    (void)memset(dst, 0, sizeof(*dst));
    extended = OnchipIsExtendedId(src->id);

    dst->format = extended ? (uint32_t)kFLEXCAN_FrameFormatExtend : (uint32_t)kFLEXCAN_FrameFormatStandard;
    dst->type = (uint32_t)kFLEXCAN_FrameTypeData;
    dst->length = (src->dlc > 8U) ? 8U : src->dlc;
    dst->id = OnchipEncodeId(src->id, extended);
    dst->dataWord0 = OnchipPackWord(src->data, dst->length, 0U);
    dst->dataWord1 = OnchipPackWord(src->data, dst->length, 4U);
}

static void OnchipFrameToFd(const can_frame_t *src, bool fdFrame, bool enableBrs, flexcan_fd_frame_t *dst)
{
    bool extended;
    uint8_t word;
    uint8_t payloadLength;

    (void)memset(dst, 0, sizeof(*dst));
    extended = OnchipIsExtendedId(src->id);
    payloadLength = fdFrame ? src->dlc : ((src->dlc > 8U) ? 8U : src->dlc);

    dst->format = extended ? (uint32_t)kFLEXCAN_FrameFormatExtend : (uint32_t)kFLEXCAN_FrameFormatStandard;
    dst->type = (uint32_t)kFLEXCAN_FrameTypeData;
    dst->length = fdFrame ? OnchipLengthToDlc(payloadLength) : payloadLength;
    dst->id = OnchipEncodeId(src->id, extended);
    dst->edl = fdFrame ? 1U : 0U;
    dst->brs = (fdFrame && enableBrs) ? 1U : 0U;

    for (word = 0U; word < 16U; word++)
    {
        dst->dataWord[word] = OnchipPackWord(src->data, payloadLength, (uint8_t)(word * 4U));
    }
}

static void OnchipClassicToFrame(const flexcan_frame_t *src, can_frame_t *dst)
{
    bool extended;
    uint32_t words[2];

    (void)memset(dst, 0, sizeof(*dst));
    extended = (src->format == (uint32_t)kFLEXCAN_FrameFormatExtend);
    dst->id = OnchipDecodeId(src->id, extended);
    dst->dlc = (uint8_t)src->length;
    dst->flags = 0U;

    words[0] = src->dataWord0;
    words[1] = src->dataWord1;
    OnchipUnpackWords(dst->data, dst->dlc, words, 2U);
}

static void OnchipFdToFrame(const flexcan_fd_frame_t *src, can_frame_t *dst)
{
    bool extended;
    uint8_t length;
    bool fdFrame;

    (void)memset(dst, 0, sizeof(*dst));
    extended = (src->format == (uint32_t)kFLEXCAN_FrameFormatExtend);
    fdFrame = (src->edl != 0U);
    length = OnchipDlcToLength((uint8_t)src->length, fdFrame);

    dst->id = OnchipDecodeId(src->id, extended);
    dst->dlc = length;
    dst->flags = fdFrame ? CAN_BRIDGE_FLAG_CANFD : 0U;
    OnchipUnpackWords(dst->data, dst->dlc, src->dataWord, 16U);
}

static void OnchipResetRuntimeContext(onchip_channel_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    ctx->ready = false;
    ctx->txPending = false;
    ctx->rxStdArmed = false;
    ctx->rxExtArmed = false;
    ctx->lastErrorLatched = false;
    ctx->activeConfig = (can_channel_config_t){0};
    ctx->runtimeState = (can_driver_runtime_state_t){0};
    ctx->lastTxFrame = (can_frame_t){0};
    ctx->txTransfer = (flexcan_mb_transfer_t){0};
    ctx->rxStdTransfer = (flexcan_mb_transfer_t){0};
    ctx->rxExtTransfer = (flexcan_mb_transfer_t){0};
    ctx->txFrame = (flexcan_frame_t){0};
    ctx->rxStdFrame = (flexcan_frame_t){0};
    ctx->rxExtFrame = (flexcan_frame_t){0};
    ctx->txFdFrame = (flexcan_fd_frame_t){0};
    ctx->rxStdFdFrame = (flexcan_fd_frame_t){0};
    ctx->rxExtFdFrame = (flexcan_fd_frame_t){0};
    OnchipResetQueues(ctx);
}

static uint8_t OnchipMapEsr1ToErrorCode(uint32_t esr1)
{
    if ((esr1 & CAN_ESR1_ACKERR_MASK) != 0U)
    {
        return 0x05U;
    }
    if (((esr1 & CAN_ESR1_BIT0ERR_MASK) != 0U) || ((esr1 & CAN_ESR1_BIT1ERR_MASK) != 0U) ||
        ((esr1 & CAN_ESR1_BIT0ERR_FAST_MASK) != 0U) || ((esr1 & CAN_ESR1_BIT1ERR_FAST_MASK) != 0U))
    {
        return 0x01U;
    }
    if (((esr1 & CAN_ESR1_STFERR_MASK) != 0U) || ((esr1 & CAN_ESR1_STFERR_FAST_MASK) != 0U))
    {
        return 0x02U;
    }
    if (((esr1 & CAN_ESR1_CRCERR_MASK) != 0U) || ((esr1 & CAN_ESR1_CRCERR_FAST_MASK) != 0U))
    {
        return 0x03U;
    }
    if (((esr1 & CAN_ESR1_FRMERR_MASK) != 0U) || ((esr1 & CAN_ESR1_FRMERR_FAST_MASK) != 0U))
    {
        return 0x04U;
    }
    if (((esr1 & CAN_ESR1_FLTCONF_MASK) >> CAN_ESR1_FLTCONF_SHIFT) == 0x01U)
    {
        return 0x07U;
    }
    if (((esr1 & CAN_ESR1_BOFFINT_MASK) != 0U) ||
        ((((esr1 & CAN_ESR1_FLTCONF_MASK) >> CAN_ESR1_FLTCONF_SHIFT) & 0x02U) != 0U))
    {
        return 0x06U;
    }

    return 0U;
}

static void OnchipRefreshRuntimeState(onchip_channel_ctx_t *ctx, uint32_t esr1)
{
    uint32_t fltconf;

    if (ctx == NULL)
    {
        return;
    }

    fltconf = (esr1 & CAN_ESR1_FLTCONF_MASK) >> CAN_ESR1_FLTCONF_SHIFT;
    ctx->runtimeState.busOff = (fltconf == 0x02U) ? 1U : 0U;
    ctx->runtimeState.errorPassive = (fltconf == 0x01U) ? 1U : 0U;
    ctx->runtimeState.txPending = ctx->txPending ? 1U : 0U;
    ctx->runtimeState.rxPending = (ctx->rxHead != ctx->rxTail) ? 1U : 0U;
}

static bool OnchipStartReceive(onchip_channel_ctx_t *ctx, uint8_t mbIdx)
{
    status_t status;

    if ((ctx == NULL) || (!ctx->ready))
    {
        return false;
    }

    if (OnchipUsesFdController(ctx))
    {
        flexcan_mb_transfer_t *transfer = (mbIdx == ONCHIP_RX_STD_MB_INDEX) ? &ctx->rxStdTransfer : &ctx->rxExtTransfer;

        transfer->mbIdx = mbIdx;
        transfer->framefd = (mbIdx == ONCHIP_RX_STD_MB_INDEX) ? &ctx->rxStdFdFrame : &ctx->rxExtFdFrame;
        transfer->frame = NULL;
        status = FLEXCAN_TransferFDReceiveNonBlocking(ctx->base, &ctx->handle, transfer);
    }
    else
    {
        flexcan_mb_transfer_t *transfer = (mbIdx == ONCHIP_RX_STD_MB_INDEX) ? &ctx->rxStdTransfer : &ctx->rxExtTransfer;

        transfer->mbIdx = mbIdx;
        transfer->frame = (mbIdx == ONCHIP_RX_STD_MB_INDEX) ? &ctx->rxStdFrame : &ctx->rxExtFrame;
        status = FLEXCAN_TransferReceiveNonBlocking(ctx->base, &ctx->handle, transfer);
    }

    if (status == kStatus_Success)
    {
        if (mbIdx == ONCHIP_RX_STD_MB_INDEX)
        {
            ctx->rxStdArmed = true;
        }
        else
        {
            ctx->rxExtArmed = true;
        }
        return true;
    }

    return false;
}

static void OnchipPushRxEvent(onchip_channel_ctx_t *ctx, const can_frame_t *frame)
{
    can_bus_event_t event;

    if ((ctx == NULL) || (frame == NULL))
    {
        return;
    }

    (void)memset(&event, 0, sizeof(event));
    event.type = kCanBusEvent_RxFrame;
    event.frame = *frame;
    event.rawStatus = FLEXCAN_GetStatusFlags(ctx->base);

    (void)OnchipQueueFramePush(ctx, frame);
    (void)OnchipQueueEventPush(ctx, &event);
}

static void OnchipPushTxCompleteEvent(onchip_channel_ctx_t *ctx)
{
    can_bus_event_t event;

    if (ctx == NULL)
    {
        return;
    }

    (void)memset(&event, 0, sizeof(event));
    event.type = kCanBusEvent_TxComplete;
    event.frame = ctx->lastTxFrame;
    event.isTx = 1U;
    event.rawStatus = FLEXCAN_GetStatusFlags(ctx->base);

    (void)OnchipQueueEventPush(ctx, &event);
}

static void OnchipPushErrorEvent(onchip_channel_ctx_t *ctx, uint8_t errorCode, uint32_t rawStatus)
{
    can_bus_event_t event;

    if ((ctx == NULL) || (errorCode == 0U))
    {
        return;
    }

    (void)memset(&event, 0, sizeof(event));
    event.type = kCanBusEvent_Error;
    event.frame = ctx->lastTxFrame;
    event.errorCode = errorCode;
    event.isTx = ctx->txPending ? 1U : 0U;
    event.rawStatus = rawStatus;

    (void)OnchipQueueEventPush(ctx, &event);
}

static void OnchipAbortSend(onchip_channel_ctx_t *ctx)
{
    if ((ctx == NULL) || (!ctx->txPending))
    {
        return;
    }

    if (OnchipUsesFdController(ctx))
    {
        FLEXCAN_TransferFDAbortSend(ctx->base, &ctx->handle, ONCHIP_TX_MB_INDEX);
    }
    else
    {
        FLEXCAN_TransferAbortSend(ctx->base, &ctx->handle, ONCHIP_TX_MB_INDEX);
    }

    ctx->txPending = false;
    ctx->runtimeState.txPending = 0U;
}

static void OnchipHandleErrorStatus(onchip_channel_ctx_t *ctx, uint32_t rawStatus)
{
    uint8_t errorCode;
    uint32_t previousErrorCode;

    if (ctx == NULL)
    {
        return;
    }

    errorCode = OnchipMapEsr1ToErrorCode(rawStatus);
    previousErrorCode = ctx->runtimeState.lastErrorCode;
    ctx->runtimeState.lastErrorCode = errorCode;
    OnchipRefreshRuntimeState(ctx, rawStatus);

    if (errorCode == 0U)
    {
        ctx->lastErrorLatched = false;
        return;
    }

    if ((!ctx->lastErrorLatched) || (previousErrorCode != errorCode))
    {
        OnchipPushErrorEvent(ctx, errorCode, rawStatus);
    }

    ctx->lastErrorLatched = true;

    if (ctx->txPending)
    {
        OnchipAbortSend(ctx);
    }
}

static void OnchipConfigureMessageBuffers(onchip_channel_ctx_t *ctx)
{
    flexcan_rx_mb_config_t rxConfig;

    if (ctx == NULL)
    {
        return;
    }

    if (OnchipUsesFdController(ctx))
    {
        FLEXCAN_SetFDTxMbConfig(ctx->base, ONCHIP_TX_MB_INDEX, true);
    }
    else
    {
        FLEXCAN_SetTxMbConfig(ctx->base, ONCHIP_TX_MB_INDEX, true);
    }

    (void)memset(&rxConfig, 0, sizeof(rxConfig));
    rxConfig.id = FLEXCAN_ID_STD(0U);
    rxConfig.format = kFLEXCAN_FrameFormatStandard;
    rxConfig.type = kFLEXCAN_FrameTypeData;
    if (OnchipUsesFdController(ctx))
    {
        FLEXCAN_SetFDRxMbConfig(ctx->base, ONCHIP_RX_STD_MB_INDEX, &rxConfig, true);
    }
    else
    {
        FLEXCAN_SetRxMbConfig(ctx->base, ONCHIP_RX_STD_MB_INDEX, &rxConfig, true);
    }

    rxConfig.id = FLEXCAN_ID_EXT(0U);
    rxConfig.format = kFLEXCAN_FrameFormatExtend;
    if (OnchipUsesFdController(ctx))
    {
        FLEXCAN_SetFDRxMbConfig(ctx->base, ONCHIP_RX_EXT_MB_INDEX, &rxConfig, true);
    }
    else
    {
        FLEXCAN_SetRxMbConfig(ctx->base, ONCHIP_RX_EXT_MB_INDEX, &rxConfig, true);
    }

    FLEXCAN_SetRxMbGlobalMask(ctx->base, 0U);
}

static bool OnchipApplyControllerConfig(onchip_channel_ctx_t *ctx, const can_channel_config_t *config)
{
    flexcan_config_t controllerConfig;
    flexcan_timing_config_t timingConfig;
    can_channel_config_t appliedConfig;
    uint32_t sourceClockHz;
    bool enableBrs;
    flexcan_mb_size_t mbSize;

    if ((ctx == NULL) || (config == NULL))
    {
        return false;
    }
    if ((config->frameFormat == kCanFrameFormat_Fd) && (!ctx->supportsFd))
    {
        return false;
    }

    sourceClockHz = CLOCK_GetClockRootFreq(kCLOCK_CanClkRoot);
    if (sourceClockHz == 0U)
    {
        return false;
    }
    if (!OnchipBuildTimingConfig(ctx, sourceClockHz, config, &timingConfig, &appliedConfig))
    {
        return false;
    }

    FLEXCAN_Deinit(ctx->base);
    FLEXCAN_GetDefaultConfig(&controllerConfig);
    controllerConfig.maxMbNum = 16U;
    controllerConfig.enableLoopBack = false;
    controllerConfig.enableListenOnlyMode = false;
    controllerConfig.enableIndividMask = false;
    controllerConfig.disableSelfReception = true;
    controllerConfig.bitRate = appliedConfig.nominalBitrate;
    controllerConfig.timingConfig = timingConfig;

    if (OnchipUsesFdController(ctx))
    {
        if (config->frameFormat == kCanFrameFormat_Fd)
        {
            controllerConfig.bitRateFD = appliedConfig.dataBitrate;
            enableBrs = (appliedConfig.dataBitrate != appliedConfig.nominalBitrate);
            mbSize = kFLEXCAN_64BperMB;
        }
        else
        {
            controllerConfig.bitRateFD = appliedConfig.nominalBitrate;
            enableBrs = false;
            mbSize = kFLEXCAN_8BperMB;
        }

        FLEXCAN_FDInit(ctx->base, &controllerConfig, sourceClockHz, mbSize, enableBrs);
    }
    else
    {
        FLEXCAN_Init(ctx->base, &controllerConfig, sourceClockHz);
    }

    FLEXCAN_TransferCreateHandle(ctx->base, &ctx->handle, OnchipFlexcanCallback, ctx);
    OnchipConfigureMessageBuffers(ctx);

    ctx->activeConfig = appliedConfig;
    ctx->runtimeState = (can_driver_runtime_state_t){0};
    ctx->txPending = false;
    ctx->rxStdArmed = false;
    ctx->rxExtArmed = false;
    ctx->lastErrorLatched = false;
    OnchipResetQueues(ctx);
    ctx->ready = true;

    (void)OnchipStartReceive(ctx, ONCHIP_RX_STD_MB_INDEX);
    (void)OnchipStartReceive(ctx, ONCHIP_RX_EXT_MB_INDEX);
    OnchipRefreshRuntimeState(ctx, FLEXCAN_GetStatusFlags(ctx->base));
    return true;
}

static FLEXCAN_CALLBACK(OnchipFlexcanCallback)
{
    onchip_channel_ctx_t *ctx = (onchip_channel_ctx_t *)userData;
    uint32_t primask;

    (void)base;
    (void)handle;

    if ((ctx == NULL) || (!ctx->ready))
    {
        return;
    }

    primask = OnchipEnterCritical();

    switch (status)
    {
        case kStatus_FLEXCAN_RxIdle:
            if (result == ONCHIP_RX_STD_MB_INDEX)
            {
                can_frame_t frame;

                ctx->rxStdArmed = false;
                if (OnchipUsesFdController(ctx))
                {
                    OnchipFdToFrame(&ctx->rxStdFdFrame, &frame);
                }
                else
                {
                    OnchipClassicToFrame(&ctx->rxStdFrame, &frame);
                }
                OnchipPushRxEvent(ctx, &frame);
                (void)OnchipStartReceive(ctx, ONCHIP_RX_STD_MB_INDEX);
            }
            else if (result == ONCHIP_RX_EXT_MB_INDEX)
            {
                can_frame_t frame;

                ctx->rxExtArmed = false;
                if (OnchipUsesFdController(ctx))
                {
                    OnchipFdToFrame(&ctx->rxExtFdFrame, &frame);
                }
                else
                {
                    OnchipClassicToFrame(&ctx->rxExtFrame, &frame);
                }
                OnchipPushRxEvent(ctx, &frame);
                (void)OnchipStartReceive(ctx, ONCHIP_RX_EXT_MB_INDEX);
            }
            break;

        case kStatus_FLEXCAN_TxIdle:
            if (result == ONCHIP_TX_MB_INDEX)
            {
                ctx->txPending = false;
                ctx->runtimeState.lastErrorCode = 0U;
                ctx->lastErrorLatched = false;
                OnchipRefreshRuntimeState(ctx, FLEXCAN_GetStatusFlags(ctx->base));
                OnchipPushTxCompleteEvent(ctx);
            }
            break;

        case kStatus_FLEXCAN_ErrorStatus:
            OnchipHandleErrorStatus(ctx, result);
            break;

        default:
            break;
    }

    OnchipExitCritical(primask);
}

bool CAN_InternalOnChipInit(void)
{
    uint8_t index;

    for (index = 0U; index < (uint8_t)kCanChannel_Count; index++)
    {
        onchip_channel_ctx_t *ctx = OnchipGetCtx((can_channel_t)index);

        if (ctx != NULL)
        {
            OnchipResetRuntimeContext(ctx);
        }
    }

    return true;
}

bool CAN_InternalOnChipApplyConfig(can_channel_t channel, const can_channel_config_t *config)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);

    if ((ctx == NULL) || (config == NULL))
    {
        return false;
    }

    return OnchipApplyControllerConfig(ctx, config);
}

bool CAN_InternalOnChipGetAppliedConfig(can_channel_t channel, can_channel_config_t *config)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);
    uint32_t primask;

    if ((ctx == NULL) || (config == NULL) || (!ctx->ready))
    {
        return false;
    }

    primask = OnchipEnterCritical();
    *config = ctx->activeConfig;
    OnchipExitCritical(primask);
    return true;
}

void CAN_InternalOnChipTask(void)
{
    CAN_InternalOnChipTaskChannel(kCanChannel_Can2);
    CAN_InternalOnChipTaskChannel(kCanChannel_Can3);
    CAN_InternalOnChipTaskChannel(kCanChannel_Can4);
}

void CAN_InternalOnChipTaskChannel(can_channel_t channel)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);
    uint32_t primask;

    if ((ctx == NULL) || (!ctx->ready))
    {
        return;
    }

    primask = OnchipEnterCritical();
    OnchipRefreshRuntimeState(ctx, FLEXCAN_GetStatusFlags(ctx->base));
    if (!ctx->rxStdArmed)
    {
        (void)OnchipStartReceive(ctx, ONCHIP_RX_STD_MB_INDEX);
    }
    if (!ctx->rxExtArmed)
    {
        (void)OnchipStartReceive(ctx, ONCHIP_RX_EXT_MB_INDEX);
    }
    OnchipExitCritical(primask);
}

bool CAN_InternalOnChipSend(can_channel_t channel, const can_frame_t *frame)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);
    status_t status;
    uint32_t primask;
    bool enableBrs;

    if ((ctx == NULL) || (frame == NULL) || (!ctx->ready))
    {
        return false;
    }
    if ((ctx->activeConfig.frameFormat == kCanFrameFormat_Classic) && (((frame->flags & CAN_BRIDGE_FLAG_CANFD) != 0U) || (frame->dlc > 8U)))
    {
        return false;
    }
    if ((ctx->activeConfig.frameFormat == kCanFrameFormat_Fd) && (!ctx->supportsFd))
    {
        return false;
    }

    primask = OnchipEnterCritical();
    if (ctx->txPending)
    {
        OnchipExitCritical(primask);
        return false;
    }

    ctx->lastTxFrame = *frame;
    ctx->lastErrorLatched = false;

    if (OnchipUsesFdController(ctx))
    {
        bool fdFrame = (ctx->activeConfig.frameFormat == kCanFrameFormat_Fd) && OnchipFrameIsFdPayload(frame);

        enableBrs = (ctx->activeConfig.dataBitrate != ctx->activeConfig.nominalBitrate);
        OnchipFrameToFd(frame, fdFrame, enableBrs, &ctx->txFdFrame);
        ctx->txTransfer.mbIdx = ONCHIP_TX_MB_INDEX;
        ctx->txTransfer.framefd = &ctx->txFdFrame;
        ctx->txTransfer.frame = NULL;
        status = FLEXCAN_TransferFDSendNonBlocking(ctx->base, &ctx->handle, &ctx->txTransfer);
    }
    else
    {
        OnchipFrameToClassic(frame, &ctx->txFrame);
        ctx->txTransfer.mbIdx = ONCHIP_TX_MB_INDEX;
        ctx->txTransfer.frame = &ctx->txFrame;
        status = FLEXCAN_TransferSendNonBlocking(ctx->base, &ctx->handle, &ctx->txTransfer);
    }

    if (status == kStatus_Success)
    {
        ctx->txPending = true;
        ctx->runtimeState.txPending = 1U;
        OnchipExitCritical(primask);
        return true;
    }

    OnchipExitCritical(primask);
    return false;
}

bool CAN_InternalOnChipReceive(can_channel_t channel, can_frame_t *frame)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);
    bool ok;
    uint32_t primask;

    if ((ctx == NULL) || (frame == NULL))
    {
        return false;
    }

    primask = OnchipEnterCritical();
    ok = OnchipQueueFramePop(ctx, frame);
    OnchipRefreshRuntimeState(ctx, FLEXCAN_GetStatusFlags(ctx->base));
    OnchipExitCritical(primask);
    return ok;
}

bool CAN_InternalOnChipPollEvent(can_channel_t channel, can_bus_event_t *event)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);
    bool ok;
    uint32_t primask;

    if ((ctx == NULL) || (event == NULL))
    {
        return false;
    }

    primask = OnchipEnterCritical();
    ok = OnchipQueueEventPop(ctx, event);
    OnchipRefreshRuntimeState(ctx, FLEXCAN_GetStatusFlags(ctx->base));
    OnchipExitCritical(primask);
    return ok;
}

bool CAN_InternalOnChipGetRuntimeState(can_channel_t channel, can_driver_runtime_state_t *state)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);
    uint32_t primask;

    if ((ctx == NULL) || (state == NULL))
    {
        return false;
    }

    primask = OnchipEnterCritical();
    OnchipRefreshRuntimeState(ctx, FLEXCAN_GetStatusFlags(ctx->base));
    *state = ctx->runtimeState;
    OnchipExitCritical(primask);
    return true;
}
