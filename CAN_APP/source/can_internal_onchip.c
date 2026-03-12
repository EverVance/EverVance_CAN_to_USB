#include "can_internal_onchip.h"

#include <stddef.h>
#include <string.h>

#include "board/clock_config.h"
#include "fsl_device_registers.h"
#include "tja1042_drv.h"

#define ONCHIP_TX_QUEUE_DEPTH (32U)
#define ONCHIP_RX_QUEUE_DEPTH (32U)
#define ONCHIP_TX_MB_INDEX (0U)
#define ONCHIP_RX_MB_INDEX (1U)
#define ONCHIP_CONFIG_TIMEOUT (100000U)

#define FLEXCAN_MB_CODE_RX_EMPTY (0x4U)
#define FLEXCAN_MB_CODE_TX_INACTIVE (0x8U)
#define FLEXCAN_MB_CODE_TX_DATA (0xCU)

typedef struct
{
    CAN_Type *base;
    can_channel_t channel;
    can_channel_config_t config;
    can_frame_t txQueue[ONCHIP_TX_QUEUE_DEPTH];
    volatile uint8_t txHead;
    volatile uint8_t txTail;
    can_frame_t rxQueue[ONCHIP_RX_QUEUE_DEPTH];
    volatile uint8_t rxHead;
    volatile uint8_t rxTail;
    uint32_t lastEsr1;
    bool supportsFd;
    bool ready;
    bool fdEnabled;
} onchip_channel_ctx_t;

static onchip_channel_ctx_t s_OnchipCtx[kCanChannel_Count];

static uint32_t EnterCritical(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void ExitCritical(uint32_t primask)
{
    __set_PRIMASK(primask);
}

static bool OnchipIsManagedChannel(can_channel_t channel)
{
    return (channel == kCanChannel_Can2) || (channel == kCanChannel_Can3) || (channel == kCanChannel_Can4);
}

static onchip_channel_ctx_t *OnchipGetContext(can_channel_t channel)
{
    if (!OnchipIsManagedChannel(channel))
    {
        return NULL;
    }

    return &s_OnchipCtx[channel];
}

static bool OnchipQueuePush(can_frame_t *queue, volatile uint8_t *head, volatile uint8_t *tail, uint8_t depth, const can_frame_t *frame)
{
    uint8_t nextHead;

    if (frame == NULL)
    {
        return false;
    }

    nextHead = (uint8_t)((*head + 1U) % depth);
    if (nextHead == *tail)
    {
        return false;
    }

    queue[*head] = *frame;
    *head = nextHead;
    return true;
}

static bool OnchipQueuePop(can_frame_t *queue, volatile uint8_t *head, volatile uint8_t *tail, uint8_t depth, can_frame_t *frame)
{
    (void)depth;

    if ((frame == NULL) || (*head == *tail))
    {
        return false;
    }

    *frame = queue[*tail];
    *tail = (uint8_t)((*tail + 1U) % ONCHIP_RX_QUEUE_DEPTH);
    return true;
}

static bool OnchipWaitMaskSet(volatile uint32_t *reg, uint32_t mask)
{
    uint32_t retry;

    for (retry = 0U; retry < ONCHIP_CONFIG_TIMEOUT; retry++)
    {
        if ((*reg & mask) == mask)
        {
            return true;
        }
    }

    return false;
}

static bool OnchipWaitMaskClear(volatile uint32_t *reg, uint32_t mask)
{
    uint32_t retry;

    for (retry = 0U; retry < ONCHIP_CONFIG_TIMEOUT; retry++)
    {
        if ((*reg & mask) == 0U)
        {
            return true;
        }
    }

    return false;
}

static bool OnchipEnterFreeze(onchip_channel_ctx_t *ctx)
{
    uint32_t mcr;

    if ((ctx == NULL) || (ctx->base == NULL))
    {
        return false;
    }

    mcr = ctx->base->MCR;
    mcr |= CAN_MCR_FRZ_MASK | CAN_MCR_HALT_MASK;
    mcr &= ~CAN_MCR_MDIS_MASK;
    ctx->base->MCR = mcr;
    return OnchipWaitMaskSet(&ctx->base->MCR, CAN_MCR_FRZACK_MASK);
}

static bool OnchipExitFreeze(onchip_channel_ctx_t *ctx)
{
    uint32_t mcr;

    if ((ctx == NULL) || (ctx->base == NULL))
    {
        return false;
    }

    mcr = ctx->base->MCR;
    mcr &= ~(CAN_MCR_HALT_MASK | CAN_MCR_FRZ_MASK);
    ctx->base->MCR = mcr;
    return OnchipWaitMaskClear(&ctx->base->MCR, CAN_MCR_FRZACK_MASK);
}

static bool OnchipCalcClassicTiming(uint32_t bitrate, uint16_t samplePointPermille, uint32_t *ctrl1)
{
    uint32_t presdiv;
    uint32_t bestError = 0xFFFFFFFFU;
    uint32_t bestCtrl1 = 0U;
    uint32_t tqClock = BOARD_BOOTCLOCKRUN_CAN_CLK_ROOT;

    if ((bitrate == 0U) || (ctrl1 == NULL))
    {
        return false;
    }

    for (presdiv = 0U; presdiv <= 255U; presdiv++)
    {
        uint32_t tqs = tqClock / ((presdiv + 1U) * bitrate);
        uint32_t tseg1;
        uint32_t tseg2;
        uint32_t propseg;
        uint32_t pseg1;
        uint32_t sampleTq;
        uint32_t sampleError;
        uint32_t actualBitrate;

        if ((tqs < 8U) || (tqs > 25U))
        {
            continue;
        }
        actualBitrate = tqClock / ((presdiv + 1U) * tqs);
        if (actualBitrate != bitrate)
        {
            continue;
        }

        sampleTq = (tqs * samplePointPermille + 500U) / 1000U;
        if ((sampleTq < 3U) || (sampleTq >= tqs))
        {
            continue;
        }

        tseg1 = sampleTq - 1U;
        tseg2 = tqs - sampleTq;
        if ((tseg2 < 2U) || (tseg2 > 8U))
        {
            continue;
        }

        propseg = tseg1 / 2U;
        if (propseg < 1U)
        {
            propseg = 1U;
        }
        if (propseg > 8U)
        {
            propseg = 8U;
        }

        pseg1 = tseg1 - propseg;
        if (pseg1 < 1U)
        {
            pseg1 = 1U;
            propseg = tseg1 - pseg1;
        }
        if ((propseg < 1U) || (propseg > 8U) || (pseg1 < 1U) || (pseg1 > 8U))
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
            bestCtrl1 = CAN_CTRL1_PRESDIV(presdiv) | CAN_CTRL1_PROPSEG(propseg - 1U) | CAN_CTRL1_PSEG1(pseg1 - 1U) |
                        CAN_CTRL1_PSEG2(tseg2 - 1U) | CAN_CTRL1_RJW(((tseg2 > 4U) ? 4U : tseg2) - 1U) |
                        CAN_CTRL1_BOFFREC(1U);
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

    *ctrl1 = bestCtrl1;
    return true;
}

static bool OnchipCalcCbtTiming(uint32_t bitrate, uint16_t samplePointPermille, uint32_t *cbt)
{
    uint32_t presdiv;
    uint32_t bestError = 0xFFFFFFFFU;
    uint32_t bestCbt = 0U;
    uint32_t tqClock = BOARD_BOOTCLOCKRUN_CAN_CLK_ROOT;

    if ((bitrate == 0U) || (cbt == NULL))
    {
        return false;
    }

    for (presdiv = 0U; presdiv <= 1023U; presdiv++)
    {
        uint32_t tqs = tqClock / ((presdiv + 1U) * bitrate);
        uint32_t sampleTq;
        uint32_t tseg1;
        uint32_t tseg2;
        uint32_t propseg;
        uint32_t pseg1;
        uint32_t sampleError;
        uint32_t actualBitrate;

        if ((tqs < 8U) || (tqs > 80U))
        {
            continue;
        }
        actualBitrate = tqClock / ((presdiv + 1U) * tqs);
        if (actualBitrate != bitrate)
        {
            continue;
        }

        sampleTq = (tqs * samplePointPermille + 500U) / 1000U;
        if ((sampleTq < 3U) || (sampleTq >= tqs))
        {
            continue;
        }

        tseg1 = sampleTq - 1U;
        tseg2 = tqs - sampleTq;
        if ((tseg2 < 2U) || (tseg2 > 32U))
        {
            continue;
        }

        propseg = tseg1 / 2U;
        if (propseg < 1U)
        {
            propseg = 1U;
        }
        if (propseg > 64U)
        {
            propseg = 64U;
        }

        pseg1 = tseg1 - propseg;
        if (pseg1 < 1U)
        {
            pseg1 = 1U;
            propseg = tseg1 - pseg1;
        }
        if ((propseg < 1U) || (propseg > 64U) || (pseg1 < 1U) || (pseg1 > 32U))
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
            bestCbt = CAN_CBT_EPRESDIV(presdiv) | CAN_CBT_EPROPSEG(propseg - 1U) | CAN_CBT_EPSEG1(pseg1 - 1U) |
                      CAN_CBT_EPSEG2(tseg2 - 1U) | CAN_CBT_ERJW(((tseg2 > 16U) ? 16U : tseg2) - 1U) | CAN_CBT_BTF(1U);
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

    *cbt = bestCbt;
    return true;
}

static bool OnchipCalcFdTiming(uint32_t bitrate, uint16_t samplePointPermille, uint32_t *fdcbt)
{
    uint32_t presdiv;
    uint32_t bestError = 0xFFFFFFFFU;
    uint32_t bestFdcbt = 0U;
    uint32_t tqClock = BOARD_BOOTCLOCKRUN_CAN_CLK_ROOT;

    if ((bitrate == 0U) || (fdcbt == NULL))
    {
        return false;
    }

    for (presdiv = 0U; presdiv <= 1023U; presdiv++)
    {
        uint32_t tqs = tqClock / ((presdiv + 1U) * bitrate);
        uint32_t sampleTq;
        uint32_t tseg1;
        uint32_t tseg2;
        uint32_t propseg;
        uint32_t pseg1;
        uint32_t sampleError;
        uint32_t actualBitrate;

        if ((tqs < 5U) || (tqs > 25U))
        {
            continue;
        }
        actualBitrate = tqClock / ((presdiv + 1U) * tqs);
        if (actualBitrate != bitrate)
        {
            continue;
        }

        sampleTq = (tqs * samplePointPermille + 500U) / 1000U;
        if ((sampleTq < 3U) || (sampleTq >= tqs))
        {
            continue;
        }

        tseg1 = sampleTq - 1U;
        tseg2 = tqs - sampleTq;
        if ((tseg2 < 2U) || (tseg2 > 8U))
        {
            continue;
        }

        propseg = tseg1 / 2U;
        if (propseg < 1U)
        {
            propseg = 1U;
        }
        if (propseg > 32U)
        {
            propseg = 32U;
        }

        pseg1 = tseg1 - propseg;
        if (pseg1 < 1U)
        {
            pseg1 = 1U;
            propseg = tseg1 - pseg1;
        }
        if ((propseg < 1U) || (propseg > 32U) || (pseg1 < 1U) || (pseg1 > 8U))
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
            bestFdcbt = CAN_FDCBT_FPRESDIV(presdiv) | CAN_FDCBT_FPROPSEG(propseg - 1U) | CAN_FDCBT_FPSEG1(pseg1 - 1U) |
                        CAN_FDCBT_FPSEG2(tseg2 - 1U) | CAN_FDCBT_FRJW(((tseg2 > 8U) ? 8U : tseg2) - 1U);
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

    *fdcbt = bestFdcbt;
    return true;
}

static uint8_t OnchipLengthToDlc(uint8_t length, bool fdFrame)
{
    if (!fdFrame)
    {
        return length;
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

static uint8_t OnchipDlcToLength(uint8_t dlc, bool fdFrame)
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

static uint32_t OnchipEncodeId(uint32_t id, bool extended)
{
    return extended ? (id & 0x1FFFFFFFU) : ((id & 0x7FFU) << 18U);
}

static uint32_t OnchipDecodeId(uint32_t rawId, bool extended)
{
    return extended ? (rawId & 0x1FFFFFFFU) : ((rawId >> 18U) & 0x7FFU);
}

static void OnchipWriteWords(onchip_channel_ctx_t *ctx, uint8_t mbIndex, const uint32_t *words, uint8_t wordCount)
{
    uint8_t i;

    if (ctx->fdEnabled)
    {
        for (i = 0U; i < wordCount; i++)
        {
            ctx->base->MB_64B.MB_64B_L[mbIndex].WORD[i] = words[i];
        }
        for (; i < 16U; i++)
        {
            ctx->base->MB_64B.MB_64B_L[mbIndex].WORD[i] = 0U;
        }
    }
    else
    {
        ctx->base->MB[mbIndex].WORD0 = (wordCount > 0U) ? words[0] : 0U;
        ctx->base->MB[mbIndex].WORD1 = (wordCount > 1U) ? words[1] : 0U;
    }
}

static void OnchipReadWords(onchip_channel_ctx_t *ctx, uint8_t mbIndex, uint32_t *words, uint8_t wordCount)
{
    uint8_t i;

    if (ctx->fdEnabled)
    {
        for (i = 0U; i < wordCount; i++)
        {
            words[i] = ctx->base->MB_64B.MB_64B_L[mbIndex].WORD[i];
        }
    }
    else
    {
        if (wordCount > 0U)
        {
            words[0] = ctx->base->MB[mbIndex].WORD0;
        }
        if (wordCount > 1U)
        {
            words[1] = ctx->base->MB[mbIndex].WORD1;
        }
    }
}

static bool OnchipApplyHardwareConfig(onchip_channel_ctx_t *ctx, const can_channel_config_t *config)
{
    uint32_t ctrl1 = 0U;
    uint32_t cbt = 0U;
    uint32_t fdctrl = 0U;
    uint32_t fdcbt = 0U;
    uint32_t mcr;

    if ((ctx == NULL) || (config == NULL) || (ctx->base == NULL))
    {
        return false;
    }

    if (!OnchipCalcClassicTiming(config->nominalBitrate, config->nominalSamplePointPermille, &ctrl1))
    {
        return false;
    }
    if (ctx->supportsFd && (config->frameFormat == kCanFrameFormat_Fd))
    {
        if (!OnchipCalcCbtTiming(config->nominalBitrate, config->nominalSamplePointPermille, &cbt) ||
            !OnchipCalcFdTiming(config->dataBitrate, config->dataSamplePointPermille, &fdcbt))
        {
            return false;
        }
        fdctrl = CAN_FDCTRL_MBDSR0(3U) | CAN_FDCTRL_FDRATE(1U) | CAN_FDCTRL_TDCEN(1U) | CAN_FDCTRL_TDCOFF(4U);
        ctx->fdEnabled = true;
    }
    else
    {
        ctx->fdEnabled = false;
    }

    ctx->base->MCR |= CAN_MCR_MDIS_MASK;
    mcr = ctx->base->MCR;
    mcr &= ~(CAN_MCR_MAXMB_MASK | CAN_MCR_IDAM_MASK);
    mcr |= CAN_MCR_MAXMB(ONCHIP_RX_MB_INDEX) | CAN_MCR_FRZ_MASK | CAN_MCR_HALT_MASK | CAN_MCR_SRXDIS_MASK |
           CAN_MCR_IRMQ_MASK;
    if (ctx->fdEnabled)
    {
        mcr |= CAN_MCR_FDEN_MASK;
    }
    else
    {
        mcr &= ~CAN_MCR_FDEN_MASK;
    }
    mcr &= ~CAN_MCR_MDIS_MASK;
    ctx->base->MCR = mcr;
    if (!OnchipEnterFreeze(ctx))
    {
        return false;
    }

    ctx->base->CTRL1 = ctrl1 & ~(CAN_CTRL1_LPB_MASK | CAN_CTRL1_LOM_MASK);
    ctx->base->CTRL2 |= CAN_CTRL2_ISOCANFDEN(ctx->fdEnabled ? 1U : 0U);
    if (ctx->supportsFd)
    {
        ctx->base->CBT = ctx->fdEnabled ? cbt : 0U;
        ctx->base->FDCTRL = fdctrl;
        ctx->base->FDCBT = ctx->fdEnabled ? fdcbt : 0U;
    }

    ctx->base->RXMGMASK = 0U;
    ctx->base->RX14MASK = 0U;
    ctx->base->RX15MASK = 0U;
    {
        uint32_t i;
        for (i = 0U; i < 64U; i++)
        {
            ctx->base->RXIMR[i] = 0U;
        }
    }

    ctx->base->IFLAG1 = 0xFFFFFFFFU;
    ctx->base->IFLAG2 = 0xFFFFFFFFU;
    ctx->base->IMASK1 = 0U;
    ctx->base->IMASK2 = 0U;

    if (ctx->fdEnabled)
    {
        ctx->base->MB_64B.MB_64B_L[ONCHIP_TX_MB_INDEX].CS = CAN_CS_CODE(FLEXCAN_MB_CODE_TX_INACTIVE);
        ctx->base->MB_64B.MB_64B_L[ONCHIP_TX_MB_INDEX].ID = 0U;
        ctx->base->MB_64B.MB_64B_L[ONCHIP_RX_MB_INDEX].ID = 0U;
        ctx->base->MB_64B.MB_64B_L[ONCHIP_RX_MB_INDEX].CS = CAN_CS_CODE(FLEXCAN_MB_CODE_RX_EMPTY);
    }
    else
    {
        ctx->base->MB[ONCHIP_TX_MB_INDEX].CS = CAN_CS_CODE(FLEXCAN_MB_CODE_TX_INACTIVE);
        ctx->base->MB[ONCHIP_TX_MB_INDEX].ID = 0U;
        ctx->base->MB[ONCHIP_TX_MB_INDEX].WORD0 = 0U;
        ctx->base->MB[ONCHIP_TX_MB_INDEX].WORD1 = 0U;
        ctx->base->MB[ONCHIP_RX_MB_INDEX].ID = 0U;
        ctx->base->MB[ONCHIP_RX_MB_INDEX].CS = CAN_CS_CODE(FLEXCAN_MB_CODE_RX_EMPTY);
    }

    ctx->config = *config;
    ctx->ready = OnchipExitFreeze(ctx);
    return ctx->ready;
}

static bool OnchipTxMailboxIdle(onchip_channel_ctx_t *ctx)
{
    uint32_t cs;
    uint32_t code;

    if (ctx->fdEnabled)
    {
        cs = ctx->base->MB_64B.MB_64B_L[ONCHIP_TX_MB_INDEX].CS;
    }
    else
    {
        cs = ctx->base->MB[ONCHIP_TX_MB_INDEX].CS;
    }

    if ((ctx->base->IFLAG1 & (1UL << ONCHIP_TX_MB_INDEX)) != 0U)
    {
        ctx->base->IFLAG1 = (1UL << ONCHIP_TX_MB_INDEX);
        if (ctx->fdEnabled)
        {
            ctx->base->MB_64B.MB_64B_L[ONCHIP_TX_MB_INDEX].CS = CAN_CS_CODE(FLEXCAN_MB_CODE_TX_INACTIVE);
        }
        else
        {
            ctx->base->MB[ONCHIP_TX_MB_INDEX].CS = CAN_CS_CODE(FLEXCAN_MB_CODE_TX_INACTIVE);
        }
        return true;
    }

    code = (cs & CAN_CS_CODE_MASK) >> CAN_CS_CODE_SHIFT;
    return (code == FLEXCAN_MB_CODE_TX_INACTIVE);
}

static bool OnchipTransmitFrame(onchip_channel_ctx_t *ctx, const can_frame_t *frame)
{
    uint8_t dlc;
    uint8_t wordCount;
    uint8_t i;
    uint32_t words[16];
    uint32_t cs;
    bool extended;
    bool fdFrame;

    if ((ctx == NULL) || (frame == NULL) || (!ctx->ready))
    {
        return false;
    }

    fdFrame = ((frame->flags & 0x01U) != 0U) && ctx->supportsFd && ctx->fdEnabled;
    if ((!fdFrame) && (frame->dlc > 8U))
    {
        return false;
    }

    dlc = OnchipLengthToDlc(frame->dlc, fdFrame);
    if (dlc == 0xFFU)
    {
        return false;
    }
    if (!OnchipTxMailboxIdle(ctx))
    {
        return false;
    }

    extended = (frame->id > 0x7FFU);
    wordCount = (uint8_t)((frame->dlc + 3U) / 4U);
    if (!fdFrame)
    {
        wordCount = 2U;
    }
    (void)memset(words, 0, sizeof(words));
    for (i = 0U; i < frame->dlc; i++)
    {
        words[i / 4U] |= ((uint32_t)frame->data[i] << ((i % 4U) * 8U));
    }

    if (ctx->fdEnabled)
    {
        ctx->base->MB_64B.MB_64B_L[ONCHIP_TX_MB_INDEX].ID = OnchipEncodeId(frame->id, extended);
    }
    else
    {
        ctx->base->MB[ONCHIP_TX_MB_INDEX].ID = OnchipEncodeId(frame->id, extended);
    }
    OnchipWriteWords(ctx, ONCHIP_TX_MB_INDEX, words, wordCount);

    cs = CAN_CS_CODE(FLEXCAN_MB_CODE_TX_DATA) | CAN_CS_DLC(dlc);
    if (extended)
    {
        cs |= CAN_CS_IDE_MASK | CAN_CS_SRR_MASK;
    }
    if (fdFrame)
    {
        cs |= CAN_CS_EDL_MASK | CAN_CS_BRS_MASK;
    }

    if (ctx->fdEnabled)
    {
        ctx->base->MB_64B.MB_64B_L[ONCHIP_TX_MB_INDEX].CS = cs;
    }
    else
    {
        ctx->base->MB[ONCHIP_TX_MB_INDEX].CS = cs;
    }

    return true;
}

static void OnchipRearmRx(onchip_channel_ctx_t *ctx)
{
    if (ctx->fdEnabled)
    {
        ctx->base->MB_64B.MB_64B_L[ONCHIP_RX_MB_INDEX].ID = 0U;
        ctx->base->MB_64B.MB_64B_L[ONCHIP_RX_MB_INDEX].CS = CAN_CS_CODE(FLEXCAN_MB_CODE_RX_EMPTY);
    }
    else
    {
        ctx->base->MB[ONCHIP_RX_MB_INDEX].ID = 0U;
        ctx->base->MB[ONCHIP_RX_MB_INDEX].CS = CAN_CS_CODE(FLEXCAN_MB_CODE_RX_EMPTY);
    }
}

static void OnchipPollReceive(onchip_channel_ctx_t *ctx)
{
    can_frame_t frame;
    uint32_t cs;
    uint32_t id;
    uint32_t words[16];
    uint8_t dlc;
    uint8_t length;
    uint8_t i;
    bool extended;
    bool fdFrame;

    if ((ctx == NULL) || (!ctx->ready))
    {
        return;
    }
    if ((ctx->base->IFLAG1 & (1UL << ONCHIP_RX_MB_INDEX)) == 0U)
    {
        return;
    }

    if (ctx->fdEnabled)
    {
        cs = ctx->base->MB_64B.MB_64B_L[ONCHIP_RX_MB_INDEX].CS;
        id = ctx->base->MB_64B.MB_64B_L[ONCHIP_RX_MB_INDEX].ID;
    }
    else
    {
        cs = ctx->base->MB[ONCHIP_RX_MB_INDEX].CS;
        id = ctx->base->MB[ONCHIP_RX_MB_INDEX].ID;
    }

    extended = ((cs & CAN_CS_IDE_MASK) != 0U);
    fdFrame = ((cs & CAN_CS_EDL_MASK) != 0U);
    dlc = (uint8_t)((cs & CAN_CS_DLC_MASK) >> CAN_CS_DLC_SHIFT);
    length = OnchipDlcToLength(dlc, fdFrame);

    (void)memset(&frame, 0, sizeof(frame));
    frame.id = OnchipDecodeId(id, extended);
    frame.dlc = length;
    frame.flags = fdFrame ? 0x01U : 0U;

    (void)memset(words, 0, sizeof(words));
    OnchipReadWords(ctx, ONCHIP_RX_MB_INDEX, words, ctx->fdEnabled ? 16U : 2U);
    for (i = 0U; i < length; i++)
    {
        frame.data[i] = (uint8_t)((words[i / 4U] >> ((i % 4U) * 8U)) & 0xFFU);
    }

    (void)ctx->base->TIMER;
    ctx->base->IFLAG1 = (1UL << ONCHIP_RX_MB_INDEX);
    OnchipRearmRx(ctx);

    {
        uint32_t primask = EnterCritical();
        (void)OnchipQueuePush(ctx->rxQueue, &ctx->rxHead, &ctx->rxTail, ONCHIP_RX_QUEUE_DEPTH, &frame);
        ExitCritical(primask);
    }
}

static void OnchipPollErrors(onchip_channel_ctx_t *ctx)
{
    uint32_t esr1;

    if ((ctx == NULL) || (!ctx->ready))
    {
        return;
    }

    esr1 = ctx->base->ESR1;
    ctx->lastEsr1 = esr1;
    if ((esr1 & CAN_ESR1_BOFFINT_MASK) != 0U)
    {
        (void)TJA1042_NotifyBusState(
            (ctx->channel == kCanChannel_Can2) ? kTja1042_CanFd2 :
            (ctx->channel == kCanChannel_Can3) ? kTja1042_Can1 :
                                                 kTja1042_Can2,
            true);
    }
}

bool CAN_InternalOnChipInit(void)
{
    (void)memset(s_OnchipCtx, 0, sizeof(s_OnchipCtx));

    s_OnchipCtx[kCanChannel_Can2].base = CAN3;
    s_OnchipCtx[kCanChannel_Can2].channel = kCanChannel_Can2;
    s_OnchipCtx[kCanChannel_Can2].supportsFd = true;

    s_OnchipCtx[kCanChannel_Can3].base = CAN1;
    s_OnchipCtx[kCanChannel_Can3].channel = kCanChannel_Can3;
    s_OnchipCtx[kCanChannel_Can3].supportsFd = false;

    s_OnchipCtx[kCanChannel_Can4].base = CAN2;
    s_OnchipCtx[kCanChannel_Can4].channel = kCanChannel_Can4;
    s_OnchipCtx[kCanChannel_Can4].supportsFd = false;

    return true;
}

bool CAN_InternalOnChipApplyConfig(can_channel_t channel, const can_channel_config_t *config)
{
    onchip_channel_ctx_t *ctx = OnchipGetContext(channel);

    if ((ctx == NULL) || (config == NULL))
    {
        return false;
    }
    if ((config->frameFormat == kCanFrameFormat_Fd) && (!ctx->supportsFd))
    {
        return false;
    }

    ctx->txHead = 0U;
    ctx->txTail = 0U;
    ctx->rxHead = 0U;
    ctx->rxTail = 0U;
    return OnchipApplyHardwareConfig(ctx, config);
}

void CAN_InternalOnChipTask(void)
{
    CAN_InternalOnChipTaskChannel(kCanChannel_Can2);
    CAN_InternalOnChipTaskChannel(kCanChannel_Can3);
    CAN_InternalOnChipTaskChannel(kCanChannel_Can4);
}

void CAN_InternalOnChipTaskChannel(can_channel_t channel)
{
    onchip_channel_ctx_t *ctx = OnchipGetContext(channel);
    can_frame_t frame;
    uint32_t primask;

    if ((ctx == NULL) || (!ctx->ready))
    {
        return;
    }

    OnchipPollReceive(ctx);
    OnchipPollErrors(ctx);

    primask = EnterCritical();
    if (ctx->txHead == ctx->txTail)
    {
        ExitCritical(primask);
        return;
    }
    frame = ctx->txQueue[ctx->txTail];
    if (OnchipTransmitFrame(ctx, &frame))
    {
        ctx->txTail = (uint8_t)((ctx->txTail + 1U) % ONCHIP_TX_QUEUE_DEPTH);
    }
    ExitCritical(primask);
}

bool CAN_InternalOnChipSend(can_channel_t channel, const can_frame_t *frame)
{
    onchip_channel_ctx_t *ctx = OnchipGetContext(channel);
    uint32_t primask;
    bool ok;

    if ((ctx == NULL) || (frame == NULL) || (!ctx->ready))
    {
        return false;
    }

    if ((!ctx->fdEnabled) && (frame->dlc > 8U))
    {
        return false;
    }

    primask = EnterCritical();
    ok = OnchipQueuePush(ctx->txQueue, &ctx->txHead, &ctx->txTail, ONCHIP_TX_QUEUE_DEPTH, frame);
    ExitCritical(primask);
    return ok;
}

bool CAN_InternalOnChipReceive(can_channel_t channel, can_frame_t *frame)
{
    onchip_channel_ctx_t *ctx = OnchipGetContext(channel);
    uint32_t primask;
    bool ok;

    if ((ctx == NULL) || (frame == NULL) || (!ctx->ready))
    {
        return false;
    }

    primask = EnterCritical();
    ok = OnchipQueuePop(ctx->rxQueue, &ctx->rxHead, &ctx->rxTail, ONCHIP_RX_QUEUE_DEPTH, frame);
    ExitCritical(primask);
    return ok;
}
