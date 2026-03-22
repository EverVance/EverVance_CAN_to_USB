#include "can_internal_onchip.h"

#include <string.h>

#include "can_bridge.h"
#include "can_stack.h"
#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_debug_console.h"
#include "fsl_flexcan.h"

/* 文件说明：
 * 本文件封装片上 FlexCAN 控制器（CH1~CH3）的自研适配层。
 * 底层基于官方 SDK 驱动，本文件额外负责：
 * - 通道上下文与队列缓存
 * - 位时序搜索与配置应用
 * - 统一事件模型与运行态输出
 *
 * 如果问题只出现在 CH1~CH3，不出现在 CH0，优先从这里排查。 */

#define ONCHIP_RX_QUEUE_DEPTH (16U)
#define ONCHIP_EVENT_QUEUE_DEPTH (32U)
#define ONCHIP_TX_MB_INDEX (8U)
#define ONCHIP_RX_STD_MB_INDEX (9U)
#define ONCHIP_RX_EXT_MB_INDEX (10U)
#define ONCHIP_MIN_PHASE_SEG2_ACTUAL (2U)

#define ONCHIP_CTRL1_MAX_PROPSEG ((uint32_t)(CAN_CTRL1_PROPSEG_MASK >> CAN_CTRL1_PROPSEG_SHIFT))
#define ONCHIP_CTRL1_MAX_PHASESEG1 ((uint32_t)(CAN_CTRL1_PSEG1_MASK >> CAN_CTRL1_PSEG1_SHIFT))
#define ONCHIP_CTRL1_MAX_PHASESEG2 ((uint32_t)(CAN_CTRL1_PSEG2_MASK >> CAN_CTRL1_PSEG2_SHIFT))
#define ONCHIP_CTRL1_MAX_RJW ((uint32_t)(CAN_CTRL1_RJW_MASK >> CAN_CTRL1_RJW_SHIFT))
#define ONCHIP_CTRL1_MAX_PRESDIV ((uint32_t)(CAN_CTRL1_PRESDIV_MASK >> CAN_CTRL1_PRESDIV_SHIFT))

#define ONCHIP_CBT_MAX_PROPSEG ((uint32_t)(CAN_CBT_EPROPSEG_MASK >> CAN_CBT_EPROPSEG_SHIFT))
#define ONCHIP_CBT_MAX_PHASESEG1 ((uint32_t)(CAN_CBT_EPSEG1_MASK >> CAN_CBT_EPSEG1_SHIFT))
#define ONCHIP_CBT_MAX_PHASESEG2 ((uint32_t)(CAN_CBT_EPSEG2_MASK >> CAN_CBT_EPSEG2_SHIFT))
#define ONCHIP_CBT_MAX_RJW ((uint32_t)(CAN_CBT_ERJW_MASK >> CAN_CBT_ERJW_SHIFT))
#define ONCHIP_CBT_MAX_PRESDIV ((uint32_t)(CAN_CBT_EPRESDIV_MASK >> CAN_CBT_EPRESDIV_SHIFT))

#define ONCHIP_FDCBT_MAX_PROPSEG ((uint32_t)(CAN_FDCBT_FPROPSEG_MASK >> CAN_FDCBT_FPROPSEG_SHIFT))
#define ONCHIP_FDCBT_MAX_PHASESEG1 ((uint32_t)(CAN_FDCBT_FPSEG1_MASK >> CAN_FDCBT_FPSEG1_SHIFT))
#define ONCHIP_FDCBT_MAX_PHASESEG2 ((uint32_t)(CAN_FDCBT_FPSEG2_MASK >> CAN_FDCBT_FPSEG2_SHIFT))
#define ONCHIP_FDCBT_MAX_RJW ((uint32_t)(CAN_FDCBT_FRJW_MASK >> CAN_FDCBT_FRJW_SHIFT))
#define ONCHIP_FDCBT_MAX_PRESDIV ((uint32_t)(CAN_FDCBT_FPRESDIV_MASK >> CAN_FDCBT_FPRESDIV_SHIFT))

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

typedef struct
{
    uint32_t maxPreDivider;
    uint32_t maxPropSeg;
    uint32_t maxPhaseSeg1;
    uint32_t maxPhaseSeg2;
    uint32_t maxRjw;
} onchip_timing_limits_t;

/* 片上 CAN 通道上下文表。
 * 约定：
 * - 下标仍按逻辑通道号访问，便于上层直接索引。
 * - CH0 外置 MCP2517FD，不在这里管理，因此索引 0 保持空白。
 * - supportsFd 描述“控制器硬件能力”，不是当前配置是否在 FD 模式。 */
static onchip_channel_ctx_t s_OnchipCtx[kCanChannel_Count] = {
    {0},
    {.channel = kCanChannel_Can2, .base = CAN3, .supportsFd = true},
    {.channel = kCanChannel_Can3, .base = CAN1, .supportsFd = false},
    {.channel = kCanChannel_Can4, .base = CAN2, .supportsFd = false},
};

static FLEXCAN_CALLBACK(OnchipFlexcanCallback);

/* 进入临界区，保护本文件内的软件队列 head/tail 与运行态缓存。
 * 时间片影响：
 * - 仅包裹极短的数据结构更新，不允许在临界区里做 SDK 调用或等待。 */
static uint32_t OnchipEnterCritical(void)
{
    return DisableGlobalIRQ();
}

/* 退出临界区，与 OnchipEnterCritical 成对使用。 */
static void OnchipExitCritical(uint32_t primask)
{
    EnableGlobalIRQ(primask);
}

/* 判断逻辑通道是否由“片上 FlexCAN 适配层”管理。
 * 维护意义：
 * - CH0 由外置 MCP2517FD 管理，后续若新增片上通道，应先更新这里。 */
static bool OnchipIsManagedChannel(can_channel_t channel)
{
    return (channel == kCanChannel_Can2) || (channel == kCanChannel_Can3) || (channel == kCanChannel_Can4);
}

/* 判断当前上下文对应的控制器是否具备 CAN FD 能力。 */
static bool OnchipUsesFdController(const onchip_channel_ctx_t *ctx)
{
    return (ctx != NULL) && ctx->supportsFd;
}

/* 根据逻辑通道取上下文指针。
 * 若不是本层负责的通道，返回 NULL，调用方必须兜底。 */
static onchip_channel_ctx_t *OnchipGetCtx(can_channel_t channel)
{
    if (!OnchipIsManagedChannel(channel))
    {
        return NULL;
    }

    return &s_OnchipCtx[channel];
}

/* 根据经典 CAN 位时序寄存器值反推采样点。
 * 用途：
 * - ApplyConfig 后把“实际生效”的采样点回报给 Host。
 * - 便于界面看到离散化后的真实值，而不是仅显示请求值。 */
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

/* 根据 FD 数据段位时序寄存器值反推采样点。 */
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

/* 无符号差值辅助函数，避免在时序搜索里反复写条件判断。 */
static uint32_t OnchipAbsDiffU32(uint32_t a, uint32_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

/* 数值钳位辅助函数，确保搜索结果不会超出硬件字段范围。 */
static uint32_t OnchipClampU32(uint32_t value, uint32_t minValue, uint32_t maxValue)
{
    if (value < minValue)
    {
        return minValue;
    }
    if (value > maxValue)
    {
        return maxValue;
    }

    return value;
}

/* 将经典 CAN 的 seg1 实际时间量拆分为 propSeg/phaseSeg1。
 * 背景：
 * - Host 传入的是更符合用户认知的波特率/采样点。
 * - FlexCAN 需要拆成寄存器字段。
 * 优化：
 * - 优先让 phaseSeg1 接近 phaseSeg2，减少波形畸变风险。 */
static bool OnchipSplitClassicSeg1(uint32_t seg1Actual,
                                   uint32_t phase2Actual,
                                   const onchip_timing_limits_t *limits,
                                   uint8_t *propSeg,
                                   uint8_t *phaseSeg1)
{
    uint32_t phase1ActualMin = 1U;
    uint32_t phase1ActualMax;
    uint32_t propActualMin = 1U;
    uint32_t propActualMax;
    uint32_t lowerBound;
    uint32_t upperBound;
    uint32_t phase1Actual;
    uint32_t propActual;

    if ((limits == NULL) || (propSeg == NULL) || (phaseSeg1 == NULL))
    {
        return false;
    }

    phase1ActualMax = limits->maxPhaseSeg1 + 1U;
    propActualMax = limits->maxPropSeg + 1U;
    lowerBound = (seg1Actual > propActualMax) ? (seg1Actual - propActualMax) : phase1ActualMin;
    upperBound = (seg1Actual > propActualMin) ? (seg1Actual - propActualMin) : 0U;
    if (lowerBound < phase1ActualMin)
    {
        lowerBound = phase1ActualMin;
    }
    if (upperBound > phase1ActualMax)
    {
        upperBound = phase1ActualMax;
    }
    if ((upperBound < lowerBound) || (seg1Actual < (phase1ActualMin + propActualMin)))
    {
        return false;
    }

    phase1Actual = OnchipClampU32(phase2Actual, lowerBound, upperBound);
    propActual = seg1Actual - phase1Actual;
    if ((propActual < propActualMin) || (propActual > propActualMax))
    {
        return false;
    }

    *propSeg = (uint8_t)(propActual - 1U);
    *phaseSeg1 = (uint8_t)(phase1Actual - 1U);
    return true;
}

/* FD 数据段的 seg1 拆分版本。
 * 与经典段不同，FDCBT 的字段定义和可用范围不同，因此单独实现。 */
static bool OnchipSplitFdSeg1(uint32_t seg1Actual,
                              uint32_t phase2Actual,
                              const onchip_timing_limits_t *limits,
                              uint8_t *propSeg,
                              uint8_t *phaseSeg1)
{
    uint32_t phase1ActualMin = 1U;
    uint32_t phase1ActualMax;
    uint32_t propActualMax;
    uint32_t lowerBound;
    uint32_t upperBound;
    uint32_t phase1Actual;
    uint32_t propActual;

    if ((limits == NULL) || (propSeg == NULL) || (phaseSeg1 == NULL))
    {
        return false;
    }

    phase1ActualMax = limits->maxPhaseSeg1 + 1U;
    propActualMax = limits->maxPropSeg;
    lowerBound = (seg1Actual > propActualMax) ? (seg1Actual - propActualMax) : phase1ActualMin;
    upperBound = seg1Actual;
    if (lowerBound < phase1ActualMin)
    {
        lowerBound = phase1ActualMin;
    }
    if (upperBound > phase1ActualMax)
    {
        upperBound = phase1ActualMax;
    }
    if ((upperBound < lowerBound) || (seg1Actual < phase1ActualMin))
    {
        return false;
    }

    phase1Actual = OnchipClampU32(phase2Actual, lowerBound, upperBound);
    propActual = seg1Actual - phase1Actual;
    if (propActual > propActualMax)
    {
        return false;
    }

    *propSeg = (uint8_t)propActual;
    *phaseSeg1 = (uint8_t)(phase1Actual - 1U);
    return true;
}

static bool OnchipFindClassicTiming(uint32_t sourceClockHz,
                                    uint32_t bitrate,
                                    uint16_t requestedSamplePermille,
                                    const onchip_timing_limits_t *limits,
                                    flexcan_timing_config_t *timingConfig)
{
    bool found = false;
    uint32_t bestSampleDiff = 0xFFFFFFFFUL;
    uint32_t bestPreDivider = 0xFFFFFFFFUL;
    uint32_t bestTotalTq = 0U;
    uint32_t preDivider;

    if ((limits == NULL) || (timingConfig == NULL) || (bitrate == 0U))
    {
        return false;
    }

    for (preDivider = 0U; preDivider <= limits->maxPreDivider; preDivider++)
    {
        uint64_t divider = (uint64_t)bitrate * (uint64_t)(preDivider + 1U);
        uint32_t totalTq;
        uint32_t phase2Actual;

        if ((divider == 0U) || (((uint64_t)sourceClockHz % divider) != 0U))
        {
            continue;
        }

        totalTq = (uint32_t)((uint64_t)sourceClockHz / divider);
        for (phase2Actual = ONCHIP_MIN_PHASE_SEG2_ACTUAL; phase2Actual <= (limits->maxPhaseSeg2 + 1U); phase2Actual++)
        {
            uint32_t seg1Actual;
            uint32_t samplePermille;
            uint32_t sampleDiff;
            uint8_t propSeg;
            uint8_t phaseSeg1;
            uint32_t rjwActual;

            if (totalTq <= (1U + phase2Actual))
            {
                continue;
            }

            seg1Actual = totalTq - 1U - phase2Actual;
            if (!OnchipSplitClassicSeg1(seg1Actual, phase2Actual, limits, &propSeg, &phaseSeg1))
            {
                continue;
            }

            samplePermille = (uint32_t)((((uint64_t)(totalTq - phase2Actual) * 1000ULL) + ((uint64_t)totalTq / 2ULL)) /
                                        (uint64_t)totalTq);
            sampleDiff = OnchipAbsDiffU32(samplePermille, requestedSamplePermille);
            if ((!found) || (sampleDiff < bestSampleDiff) ||
                ((sampleDiff == bestSampleDiff) && (totalTq > bestTotalTq)) ||
                ((sampleDiff == bestSampleDiff) && (totalTq == bestTotalTq) && (preDivider < bestPreDivider)))
            {
                found = true;
                bestSampleDiff = sampleDiff;
                bestTotalTq = totalTq;
                bestPreDivider = preDivider;
                rjwActual = phase2Actual;
                if (rjwActual > ((uint32_t)phaseSeg1 + 1U))
                {
                    rjwActual = (uint32_t)phaseSeg1 + 1U;
                }
                if (rjwActual > (limits->maxRjw + 1U))
                {
                    rjwActual = limits->maxRjw + 1U;
                }

                timingConfig->preDivider = (uint16_t)preDivider;
                timingConfig->propSeg = propSeg;
                timingConfig->phaseSeg1 = phaseSeg1;
                timingConfig->phaseSeg2 = (uint8_t)(phase2Actual - 1U);
                timingConfig->rJumpwidth = (uint8_t)(rjwActual - 1U);
            }
        }
    }

    return found;
}

static bool OnchipFindFdDataTiming(uint32_t sourceClockHz,
                                   uint32_t bitrate,
                                   uint16_t requestedSamplePermille,
                                   const onchip_timing_limits_t *limits,
                                   flexcan_timing_config_t *timingConfig)
{
    bool found = false;
    uint32_t bestSampleDiff = 0xFFFFFFFFUL;
    uint32_t bestPreDivider = 0xFFFFFFFFUL;
    uint32_t bestTotalTq = 0U;
    uint32_t preDivider;

    if ((limits == NULL) || (timingConfig == NULL) || (bitrate == 0U))
    {
        return false;
    }

    for (preDivider = 0U; preDivider <= limits->maxPreDivider; preDivider++)
    {
        uint64_t divider = (uint64_t)bitrate * (uint64_t)(preDivider + 1U);
        uint32_t totalTq;
        uint32_t phase2Actual;

        if ((divider == 0U) || (((uint64_t)sourceClockHz % divider) != 0U))
        {
            continue;
        }

        totalTq = (uint32_t)((uint64_t)sourceClockHz / divider);
        for (phase2Actual = ONCHIP_MIN_PHASE_SEG2_ACTUAL; phase2Actual <= (limits->maxPhaseSeg2 + 1U); phase2Actual++)
        {
            uint32_t seg1Actual;
            uint32_t samplePermille;
            uint32_t sampleDiff;
            uint8_t propSeg;
            uint8_t phaseSeg1;
            uint32_t rjwActual;

            if (totalTq <= (1U + phase2Actual))
            {
                continue;
            }

            seg1Actual = totalTq - 1U - phase2Actual;
            if (!OnchipSplitFdSeg1(seg1Actual, phase2Actual, limits, &propSeg, &phaseSeg1))
            {
                continue;
            }

            samplePermille = (uint32_t)((((uint64_t)(totalTq - phase2Actual) * 1000ULL) + ((uint64_t)totalTq / 2ULL)) /
                                        (uint64_t)totalTq);
            sampleDiff = OnchipAbsDiffU32(samplePermille, requestedSamplePermille);
            if ((!found) || (sampleDiff < bestSampleDiff) ||
                ((sampleDiff == bestSampleDiff) && (totalTq > bestTotalTq)) ||
                ((sampleDiff == bestSampleDiff) && (totalTq == bestTotalTq) && (preDivider < bestPreDivider)))
            {
                found = true;
                bestSampleDiff = sampleDiff;
                bestTotalTq = totalTq;
                bestPreDivider = preDivider;
                rjwActual = phase2Actual;
                if (rjwActual > ((uint32_t)phaseSeg1 + 1U))
                {
                    rjwActual = (uint32_t)phaseSeg1 + 1U;
                }
                if (rjwActual > (limits->maxRjw + 1U))
                {
                    rjwActual = limits->maxRjw + 1U;
                }

                timingConfig->fpreDivider = (uint16_t)preDivider;
                timingConfig->fpropSeg = propSeg;
                timingConfig->fphaseSeg1 = phaseSeg1;
                timingConfig->fphaseSeg2 = (uint8_t)(phase2Actual - 1U);
                timingConfig->frJumpwidth = (uint8_t)(rjwActual - 1U);
            }
        }
    }

    return found;
}

static bool OnchipBuildTimingConfig(onchip_channel_ctx_t *ctx,
                                    uint32_t sourceClockHz,
                                    const can_channel_config_t *requestedConfig,
                                    flexcan_timing_config_t *timingConfig,
                                    can_channel_config_t *appliedConfig)
{
    bool ok;
    onchip_timing_limits_t nominalLimits;
    onchip_timing_limits_t dataLimits;

    if ((ctx == NULL) || (requestedConfig == NULL) || (timingConfig == NULL) || (appliedConfig == NULL))
    {
        return false;
    }
    *appliedConfig = *requestedConfig;
    (void)memset(timingConfig, 0, sizeof(*timingConfig));

    if (OnchipUsesFdController(ctx))
    {
        nominalLimits.maxPreDivider = ONCHIP_CBT_MAX_PRESDIV;
        nominalLimits.maxPropSeg = ONCHIP_CBT_MAX_PROPSEG;
        nominalLimits.maxPhaseSeg1 = ONCHIP_CBT_MAX_PHASESEG1;
        nominalLimits.maxPhaseSeg2 = ONCHIP_CBT_MAX_PHASESEG2;
        nominalLimits.maxRjw = ONCHIP_CBT_MAX_RJW;

        if (requestedConfig->frameFormat == kCanFrameFormat_Fd)
        {
            dataLimits.maxPreDivider = ONCHIP_FDCBT_MAX_PRESDIV;
            dataLimits.maxPropSeg = ONCHIP_FDCBT_MAX_PROPSEG;
            dataLimits.maxPhaseSeg1 = ONCHIP_FDCBT_MAX_PHASESEG1;
            dataLimits.maxPhaseSeg2 = ONCHIP_FDCBT_MAX_PHASESEG2;
            dataLimits.maxRjw = ONCHIP_FDCBT_MAX_RJW;

            ok = OnchipFindClassicTiming(sourceClockHz,
                                         requestedConfig->nominalBitrate,
                                         requestedConfig->nominalSamplePointPermille,
                                         &nominalLimits,
                                         timingConfig) &&
                 OnchipFindFdDataTiming(sourceClockHz,
                                        requestedConfig->dataBitrate,
                                        requestedConfig->dataSamplePointPermille,
                                        &dataLimits,
                                        timingConfig);
            if (!ok)
            {
                ok = FLEXCAN_FDCalculateImprovedTimingValues(
                    ctx->base, requestedConfig->nominalBitrate, requestedConfig->dataBitrate, sourceClockHz, timingConfig);
            }
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
            ok = OnchipFindClassicTiming(sourceClockHz,
                                         requestedConfig->nominalBitrate,
                                         requestedConfig->nominalSamplePointPermille,
                                         &nominalLimits,
                                         timingConfig);
            if (!ok)
            {
                ok = FLEXCAN_CalculateImprovedTimingValues(ctx->base, requestedConfig->nominalBitrate, sourceClockHz, timingConfig);
            }
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
        nominalLimits.maxPreDivider = ONCHIP_CTRL1_MAX_PRESDIV;
        nominalLimits.maxPropSeg = ONCHIP_CTRL1_MAX_PROPSEG;
        nominalLimits.maxPhaseSeg1 = ONCHIP_CTRL1_MAX_PHASESEG1;
        nominalLimits.maxPhaseSeg2 = ONCHIP_CTRL1_MAX_PHASESEG2;
        nominalLimits.maxRjw = ONCHIP_CTRL1_MAX_RJW;

        ok = OnchipFindClassicTiming(sourceClockHz,
                                     requestedConfig->nominalBitrate,
                                     requestedConfig->nominalSamplePointPermille,
                                     &nominalLimits,
                                     timingConfig);
        if (!ok)
        {
            ok = FLEXCAN_CalculateImprovedTimingValues(ctx->base, requestedConfig->nominalBitrate, sourceClockHz, timingConfig);
        }
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

/* 从软件接收队列弹出一帧。
 * 调用场景：
 * - 仅由上层 CAN Stack 的 Receive/Poll 路径读取。
 * - 不直接访问硬件，确保上层在任务上下文中读取更稳定。 */
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

/* 向软件事件队列压入一个总线事件。
 * 事件队列统一承载：
 * - TX 完成
 * - RX 帧
 * - 错误事件
 * 这样上层可以用一致的事件模型处理 CH1~CH3。 */
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

/* 从软件事件队列弹出事件。 */
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

/* 清空本通道的软件队列。
 * 使用时机：
 * - 重新初始化控制器前
 * - 通道关闭/恢复时
 * - 避免旧帧、旧错误事件污染新一轮会话 */
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

/* 将数据长度映射为 CAN FD DLC。
 * 对经典 CAN 长度 0~8 保持不变；超过 8 时按 FD 标准离散映射。 */
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

/* 将 DLC 反解为真实 payload 长度。
 * 维护注意：
 * - 经典 CAN 的 DLC>8 在协议上无意义，这里统一钳到 8。 */
static uint8_t OnchipDlcToLength(uint8_t dlc, bool fdFrame)
{
    if ((!fdFrame) || (dlc <= 8U))
    {
        return (dlc <= 8U) ? dlc : 8U;
    }

    return (uint8_t)DLC_LENGTH_DECODE(dlc);
}

/* 通过 ID 范围判断是否需要扩展帧格式。 */
static bool OnchipIsExtendedId(uint32_t id)
{
    return (id > 0x7FFU);
}

/* 将逻辑 ID 编码为 SDK 所需的寄存器格式。 */
static uint32_t OnchipEncodeId(uint32_t id, bool extended)
{
    return extended ? FLEXCAN_ID_EXT(id) : FLEXCAN_ID_STD(id);
}

/* 将 SDK 提供的原始 ID 字段解码回逻辑 ID。 */
static uint32_t OnchipDecodeId(uint32_t rawId, bool extended)
{
    return extended ? (rawId & 0x1FFFFFFFU) : ((rawId >> CAN_ID_STD_SHIFT) & 0x7FFU);
}

/* 按 FlexCAN SDK 的字节序约定打包 32bit dataWord。
 * 这里是此前“MSB/LSB 颠倒”问题的关键修复点之一，
 * 后续如再出现 payload 字节顺序异常，应优先检查这里。 */
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

/* 从 SDK 的 dataWord[] 还原线性字节数组，与 OnchipPackWord 成对。 */
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

/* 判断一帧在发送时是否应走 FD 对象格式。 */
static bool OnchipFrameIsFdPayload(const can_frame_t *frame)
{
    return (frame != NULL) && ((((frame->flags & CAN_BRIDGE_FLAG_CANFD) != 0U) || (frame->dlc > 8U)));
}

/* 将统一帧结构转换为经典 CAN SDK 帧。
 * 说明：
 * - 这里只负责格式转换，不负责控制器是否真的处于经典模式。 */
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

/* 将统一帧结构转换为 FD SDK 帧。
 * enableBrs 控制是否打开数据段加速，来源于当前通道配置。 */
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

/* 将经典 SDK 帧还原为统一 can_frame_t。 */
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

/* 将 FD SDK 帧还原为统一 can_frame_t。 */
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

/* 复位运行时上下文，不改动静态硬件拓扑信息。
 * 设计目的：
 * - 区分“硬件能力/通道归属”与“本次会话动态状态”。 */
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

/* 关闭通道时应用的“温和禁用”路径。
 * 为什么不直接 Deinit：
 * - 过去在批量启停通道时，激进 Deinit/Reinit 容易引入不稳定行为。
 * - 当前策略改为停发送、停接收、关中断、清软件状态，再配合 PHY standby。
 * - 这样既能保证关闭通道不干扰其他总线，也能减少频繁重配带来的副作用。 */
static __attribute__((unused)) bool OnchipApplyDisabledConfig(onchip_channel_ctx_t *ctx, const can_channel_config_t *config)
{
    uint32_t mbMask;
    can_channel_config_t disabledConfig;

    if ((ctx == NULL) || (config == NULL))
    {
        return false;
    }

    disabledConfig = *config;
    disabledConfig.terminationEnabled = false;

    if (!ctx->ready)
    {
        ctx->activeConfig = disabledConfig;
        ctx->runtimeState = (can_driver_runtime_state_t){0};
        ctx->txPending = false;
        ctx->rxStdArmed = false;
        ctx->rxExtArmed = false;
        ctx->lastErrorLatched = false;
        OnchipResetQueues(ctx);
        return true;
    }

    if (ctx->ready)
    {
        mbMask = (1UL << ONCHIP_TX_MB_INDEX) | (1UL << ONCHIP_RX_STD_MB_INDEX) | (1UL << ONCHIP_RX_EXT_MB_INDEX);
        if (OnchipUsesFdController(ctx))
        {
            FLEXCAN_TransferFDAbortSend(ctx->base, &ctx->handle, ONCHIP_TX_MB_INDEX);
            FLEXCAN_TransferFDAbortReceive(ctx->base, &ctx->handle, ONCHIP_RX_STD_MB_INDEX);
            FLEXCAN_TransferFDAbortReceive(ctx->base, &ctx->handle, ONCHIP_RX_EXT_MB_INDEX);
        }
        else
        {
            FLEXCAN_TransferAbortSend(ctx->base, &ctx->handle, ONCHIP_TX_MB_INDEX);
            FLEXCAN_TransferAbortReceive(ctx->base, &ctx->handle, ONCHIP_RX_STD_MB_INDEX);
            FLEXCAN_TransferAbortReceive(ctx->base, &ctx->handle, ONCHIP_RX_EXT_MB_INDEX);
        }
        FLEXCAN_DisableMbInterrupts(ctx->base, mbMask);
        FLEXCAN_DisableInterrupts(ctx->base, FLEXCAN_ERROR_AND_STATUS_INIT_FLAG);
        FLEXCAN_ClearMbStatusFlags(ctx->base, mbMask);
        FLEXCAN_ClearStatusFlags(ctx->base, FLEXCAN_ERROR_AND_STATUS_INIT_FLAG);
    }

    /* Avoid fully deinitializing the controller on a normal channel-close path.
     * The next enable operation already performs a clean init sequence, while
     * repeated deinit/reinit here has shown unstable behaviour in mixed
     * enable/disable batches. Keeping the IP quiesced and the PHY in standby is
     * sufficient to prevent a disabled channel from interfering with active
     * buses. */
    ctx->ready = false;
    ctx->txPending = false;
    ctx->rxStdArmed = false;
    ctx->rxExtArmed = false;
    ctx->lastErrorLatched = false;
    OnchipResetQueues(ctx);
    ctx->activeConfig = disabledConfig;
    ctx->runtimeState = (can_driver_runtime_state_t){0};
    return true;
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

/* 根据 ESR1 刷新软件缓存的运行态。
 * 这里是“设备运行时状态页”和“LED/监控状态”共同依赖的数据源。 */
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

/* 重新挂起一个接收 MB。
 * 维护注意：
 * - 每次 RX 回调处理完后都要及时重新 Arm，否则后续帧会静默丢失。 */
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

/* 构造 RX 事件并推送到软件事件队列。 */
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

/* 构造 TX 完成事件，供上层做发送回显与统计。 */
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

/* 构造错误事件并推送到软件事件队列。 */
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

/* 中止正在进行的发送。
 * 典型场景：
 * - 发送卡死
 * - 通道关闭
 * - 发现需要恢复控制器的错误状态 */
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

/* 统一处理 ESR1 导出的错误状态。
 * 设计原则：
 * - 先刷新 runtimeState
 * - 再根据优先级映射错误码
 * - 最后只在状态变化时推送事件，避免错误风暴刷爆 Host */
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

/* 配置本通道使用的发送/接收 MB。
 * 这里固定了 MB 索引分工，后续如要扩展更多过滤器或 RX buffer，
 * 需要同步修改本函数与回调里的 MB 约定。 */
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

/* 将上层 can_channel_config_t 应用到官方 SDK 控制器。
 * 职责边界：
 * - 负责控制器初始化、位时序求解、MB 配置、回调注册
 * - 不负责 PHY standby/termination，这些在 CAN Stack 层处理 */
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

/* FlexCAN SDK 回调。
 * 中断上下文要求：
 * - 不能做阻塞操作
 * - 不能访问可能睡眠的 RTOS API
 * - 只做“把硬件结果转成软件缓存/事件”这类轻操作 */
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

/* 初始化片上 CAN 适配层的软件上下文。
 * 说明：
 * - 这里只做软件层初始化，不主动启用任一路控制器。
 * - 真正的硬件配置由 ApplyConfig 按需触发。 */
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

/* 对指定通道应用配置，是 CH1~CH3 的核心入口。 */
bool CAN_InternalOnChipApplyConfig(can_channel_t channel, const can_channel_config_t *config)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);

    if ((ctx == NULL) || (config == NULL))
    {
        return false;
    }

    /* Keep on-chip controllers configured even when the channel is logically
     * disabled. The stack/PHY layer already blocks traffic and puts the
     * transceiver into standby, while keeping the controller init path uniform
     * avoids the split behaviour that appears when only part of the channel set
     * is enabled. */
    return OnchipApplyControllerConfig(ctx, config);
}

/* 读取当前生效配置，回给 Host 或状态页使用。 */
bool CAN_InternalOnChipGetAppliedConfig(can_channel_t channel, can_channel_config_t *config)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);
    uint32_t primask;

    if ((ctx == NULL) || (config == NULL))
    {
        return false;
    }

    primask = OnchipEnterCritical();
    *config = ctx->activeConfig;
    OnchipExitCritical(primask);
    return true;
}

/* 片上 CAN 的总任务入口。
 * 当前主要保留作统一入口，实际工作更多在各通道任务和 SDK 回调中完成。 */
void CAN_InternalOnChipTask(void)
{
    CAN_InternalOnChipTaskChannel(kCanChannel_Can2);
    CAN_InternalOnChipTaskChannel(kCanChannel_Can3);
    CAN_InternalOnChipTaskChannel(kCanChannel_Can4);
}

/* 指定通道的后台维护入口。
 * 当前主要职责是轮询/刷新错误状态，弥补仅靠回调不够完整的场景。 */
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

/* 发送一帧到指定片上 CAN 通道。 */
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

/* 从软件接收队列读取一帧。 */
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

/* 轮询软件事件队列。 */
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

/* 获取当前缓存的运行态。 */
bool CAN_InternalOnChipGetRuntimeState(can_channel_t channel, can_driver_runtime_state_t *state)
{
    onchip_channel_ctx_t *ctx = OnchipGetCtx(channel);
    uint32_t primask;

    if ((ctx == NULL) || (state == NULL))
    {
        return false;
    }

    primask = OnchipEnterCritical();
    if (ctx->ready)
    {
        OnchipRefreshRuntimeState(ctx, FLEXCAN_GetStatusFlags(ctx->base));
    }
    *state = ctx->runtimeState;
    OnchipExitCritical(primask);
    return true;
}
